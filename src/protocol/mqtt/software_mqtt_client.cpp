#include "protocol/mqtt/software_mqtt_client.h"

#include <utility>

#include "platform/time.h"

namespace esp_modem_link::protocol {
namespace {

// How often the keep-alive task wakes. Short enough that a PINGREQ goes out
// near its deadline, long enough not to matter, and it bounds how long
// StopKeepAlive() waits for the task to notice it should stop.
constexpr std::chrono::milliseconds kKeepAliveTick{200};

// The most inbound bytes that may pile up unconsumed. A remainder of a stream
// that never completes its length field would otherwise be buffered forever,
// and a broker promising more than this is beyond what a device should hold.
constexpr size_t kMaxBufferedBytes = 64 * 1024;

}  // namespace

SoftwareMqttClient::SoftwareMqttClient(MqttTransportFactory factory,
                                      std::chrono::milliseconds connack_timeout)
    : factory_(std::move(factory)), connack_timeout_(connack_timeout) {}

SoftwareMqttClient::~SoftwareMqttClient() { Disconnect(); }

void SoftwareMqttClient::SetKeepAlive(int seconds) {
  std::lock_guard<std::mutex> lock(mutex_);
  keep_alive_seconds_ = seconds;
}

void SoftwareMqttClient::SetClientId(std::string_view client_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  client_id_ = std::string(client_id);
}

void SoftwareMqttClient::SetCredentials(std::string_view user,
                                        std::string_view pass) {
  std::lock_guard<std::mutex> lock(mutex_);
  username_ = std::string(user);
  password_ = std::string(pass);
  has_credentials_ = true;
}

void SoftwareMqttClient::SetWill(const MqttWill& will) {
  std::lock_guard<std::mutex> lock(mutex_);
  will_ = will;
  has_will_ = true;
}

void SoftwareMqttClient::SetTlsConfig(const TlsConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  tls_ = config;
  // Asking for TLS settings is how a caller says it wants TLS; see the note in
  // the header for why this is not inferred from the port.
  tls_enabled_ = true;
}

Result<> SoftwareMqttClient::Connect(std::string_view host,
                                     uint16_t port,
                                     bool clean_session) {
  MqttConnectOptions options;
  bool tls = false;
  TlsConfig tls_config;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connected_ || transport_) {
      return std::unexpected(NetworkError(NetworkErrc::kAlreadyConnected, 0,
                                          "already connected"));
    }
    options.client_id = client_id_;
    options.username = username_;
    options.password = password_;
    options.has_username = has_credentials_;
    // The protocol requires a password to be accompanied by a user name, so a
    // credential pair is offered as both or as neither.
    options.has_password = has_credentials_;
    options.clean_session = clean_session;
    options.keep_alive_seconds =
        static_cast<uint16_t>(keep_alive_seconds_ < 0 ? 0 : keep_alive_seconds_);
    options.has_will = has_will_;
    options.will = will_;
    tls = tls_enabled_;
    tls_config = tls_;
    connecting_ = true;
    connack_ready_ = false;
    connack_error_.reset();
    rx_buffer_.clear();
  }

  auto created = factory_(tls, tls_config);
  if (!created.has_value()) {
    return std::unexpected(created.error());
  }
  // Registered before the first byte is written: a transport that answers
  // inside Send() must not be able to reach a client that is not listening yet.
  auto transport = std::shared_ptr<TcpClient>(std::move(created.value()));
  transport->OnData([this](std::string_view data) { OnTransportData(data); });
  transport->OnDisconnected([this]() { OnTransportClosed(); });

  auto opened = transport->Connect(host, port);
  if (!opened.has_value()) {
    return std::unexpected(opened.error());
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    transport_ = transport;
  }

  auto sent = SendPacket(EncodeConnect(options));
  if (!sent.has_value()) {
    Teardown(false);
    return std::unexpected(sent.error());
  }

  std::unique_lock<std::mutex> lock(mutex_);
  const bool answered =
      cv_.wait_for(lock, connack_timeout_, [this] { return connack_ready_; });
  if (!answered) {
    lock.unlock();
    // Sent even though no CONNACK came: the broker may have accepted the
    // CONNECT and lost the answer, and a session nobody will use is better
    // closed than left to time out.
    Teardown(true);
    return std::unexpected(NetworkError::Timeout("no CONNACK from broker"));
  }
  if (connack_error_.has_value()) {
    const NetworkError error = *connack_error_;
    lock.unlock();
    // The broker refused, so there is no session to say goodbye to.
    Teardown(false);
    return std::unexpected(error);
  }
  lock.unlock();

  StartKeepAlive();
  // Outside the lock, and after the connection is genuinely usable: a callback
  // that publishes has to find a session that is already established.
  if (on_connected_) on_connected_();
  return {};
}

void SoftwareMqttClient::Disconnect() {
  StopKeepAlive();
  Teardown(true);
}

void SoftwareMqttClient::Teardown(bool send_disconnect) {
  std::shared_ptr<TcpClient> transport;
  bool had_transport = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    transport = std::move(transport_);
    had_transport = transport != nullptr;
    connected_ = false;
    connecting_ = false;
    connack_ready_ = false;
    rx_buffer_.clear();
    pending_qos2_.clear();
    pending_topics_.clear();
  }
  // Ends the loop if it is still running, whichever thread got here. The join
  // itself happens in StopKeepAlive(), which a callback is not allowed to
  // reach - it may be this very task that noticed.
  keepalive_running_ = false;
  ping_sent_ms_ = 0;

  if (!had_transport) return;

  if (send_disconnect) {
    // Sent through the local handle rather than through SendPacket(), which
    // looks the transport up in transport_ - already given up above.
    const std::string packet = EncodeDisconnect();
    transport->Send(packet.data(), packet.size());
  }
  transport->Disconnect();
  if (on_disconnected_) on_disconnected_();
}

Result<int> SoftwareMqttClient::Publish(std::string_view topic,
                                        std::string_view payload,
                                        MqttQoS qos,
                                        bool retain) {
  const uint8_t qos_value = static_cast<uint8_t>(qos);
  uint16_t packet_id = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) {
      return std::unexpected(
          NetworkError(NetworkErrc::kNotConnected, 0, "not connected"));
    }
    // QoS 0 is fire and forget: a packet id would have no acknowledgement to
    // match, so the message goes out without one and the caller gets zero back.
    if (qos_value > 0) packet_id = NextPacketIdLocked();
  }

  auto sent = SendPacket(EncodePublish(packet_id, topic, payload, qos_value,
                                       retain));
  if (!sent.has_value()) {
    return std::unexpected(sent.error());
  }
  return static_cast<int>(packet_id);
}

Result<int> SoftwareMqttClient::Subscribe(std::string_view topic, MqttQoS qos) {
  uint16_t packet_id = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) {
      return std::unexpected(
          NetworkError(NetworkErrc::kNotConnected, 0, "not connected"));
    }
    packet_id = NextPacketIdLocked();
    // Remembered so the SUBACK can name the filter if the broker refuses it.
    pending_topics_[packet_id] = std::string(topic);
  }

  auto sent = SendPacket(EncodeSubscribe(packet_id, topic,
                                         static_cast<uint8_t>(qos)));
  if (!sent.has_value()) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_topics_.erase(packet_id);
    return std::unexpected(sent.error());
  }
  return static_cast<int>(packet_id);
}

Result<> SoftwareMqttClient::Unsubscribe(std::string_view topic) {
  uint16_t packet_id = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) {
      return std::unexpected(
          NetworkError(NetworkErrc::kNotConnected, 0, "not connected"));
    }
    packet_id = NextPacketIdLocked();
    pending_topics_[packet_id] = std::string(topic);
  }

  auto sent = SendPacket(EncodeUnsubscribe(packet_id, topic));
  if (!sent.has_value()) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_topics_.erase(packet_id);
    return std::unexpected(sent.error());
  }
  return {};
}

Result<> SoftwareMqttClient::SendPacket(std::string_view packet) {
  std::shared_ptr<TcpClient> transport;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    transport = transport_;
  }
  if (!transport) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotConnected, 0, "not connected"));
  }

  // The lock is not held here, so this may run concurrently with the keep-alive
  // task. One packet per call is one AT command at the layer below, which is
  // what keeps two sends from interleaving.
  auto result = transport->Send(packet.data(), packet.size());
  if (!result.has_value()) {
    return std::unexpected(result.error());
  }
  last_sent_ms_ = platform::Now().count();
  return {};
}

void SoftwareMqttClient::StartKeepAlive() {
  int seconds = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    seconds = keep_alive_seconds_;
  }
  // Zero means the client has been told not to keep the session alive.
  if (seconds <= 0) return;

  StopKeepAlive();
  last_sent_ms_ = platform::Now().count();
  ping_sent_ms_ = 0;
  keepalive_running_ = true;
  keepalive_task_ =
      platform::CreateTask("mqtt_ka", [this]() { KeepAliveLoop(); }, 3072, 4);
  keepalive_task_->Start();
}

void SoftwareMqttClient::StopKeepAlive() {
  keepalive_running_ = false;
  if (keepalive_task_) {
    keepalive_task_->Stop();
    keepalive_task_.reset();
  }
}

void SoftwareMqttClient::KeepAliveLoop() {
  int seconds = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    seconds = keep_alive_seconds_;
  }
  if (seconds <= 0) {
    keepalive_running_ = false;
    return;
  }
  const long long interval_ms = static_cast<long long>(seconds) * 1000;

  while (keepalive_running_) {
    platform::Sleep(kKeepAliveTick);
    if (!keepalive_running_) break;

    const long long now = platform::Now().count();
    const long long ping_sent = ping_sent_ms_.load();

    if (ping_sent != 0) {
      // A broker that has had a whole keep-alive period to answer a PINGREQ is
      // not going to. Waiting longer would only delay finding out, so the
      // connection is ended here rather than left to look alive.
      if (now - ping_sent >= interval_ms) {
        ReportError(NetworkError(NetworkErrc::kTimeout, 0,
                                 "no PINGRESP from broker"));
        Teardown(false);
        return;
      }
      continue;
    }

    if (now - last_sent_ms_.load() >= interval_ms) {
      // Nothing has gone out for a whole period, which is the condition the
      // specification gives for sending a PINGREQ.
      if (SendPacket(EncodePingreq()).has_value()) {
        ping_sent_ms_ = now;
      }
    }
  }
}

void SoftwareMqttClient::OnTransportData(std::string_view data) {
  bool overflowed = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    rx_buffer_.append(data);
    if (rx_buffer_.size() > kMaxBufferedBytes) {
      rx_buffer_.clear();
      overflowed = true;
    }
  }
  if (overflowed) {
    // Reported outside the lock: a callback that runs here may legitimately
    // call back in, and holding the lock across it would deadlock.
    ReportError(NetworkError(NetworkErrc::kBufferOverflow, 0,
                             "inbound MQTT stream exceeds the buffer limit"));
    Teardown(false);
    return;
  }

  // Packets are taken under the lock and handled without it, because a handler
  // may publish - which is a reasonable thing to do from OnMessage - and
  // SendPacket takes the lock for itself.
  for (;;) {
    MqttPacket packet;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto taken = TakePacket(rx_buffer_);
      if (!taken.has_value()) break;
      packet = std::move(*taken);
    }
    HandlePacket(packet);
  }
}

void SoftwareMqttClient::OnTransportClosed() {
  // The link is already gone, so there is nothing to say goodbye over.
  Teardown(false);
}

void SoftwareMqttClient::HandlePacket(const MqttPacket& packet) {
  switch (packet.header.type) {
    case MqttPacketType::kConnack:
      HandleConnack(packet);
      break;
    case MqttPacketType::kPublish:
      HandlePublish(packet);
      break;
    case MqttPacketType::kPuback:
    case MqttPacketType::kPubcomp:
      // The last acknowledgement of either QoS 1 or QoS 2, whichever the
      // message was sent at: that is the point the broker has taken it.
      HandlePublishComplete(packet);
      break;
    case MqttPacketType::kPubrec:
      HandlePubrec(packet);
      break;
    case MqttPacketType::kPubrel:
      HandlePubrel(packet);
      break;
    case MqttPacketType::kSuback:
      HandleSuback(packet);
      break;
    case MqttPacketType::kUnsuback:
      HandleUnsuback(packet);
      break;
    case MqttPacketType::kPingresp:
      ping_sent_ms_ = 0;
      break;
    default:
      // PINGREQ, CONNECT, SUBSCRIBE and the rest are client-to-server only. A
      // broker sending one is breaking the protocol, and acting on it would be
      // worse than ignoring it.
      break;
  }
}

void SoftwareMqttClient::HandleConnack(const MqttPacket& packet) {
  auto connack = ParseConnack(packet.body);
  std::lock_guard<std::mutex> lock(mutex_);
  // Only a CONNECT this client sent is waiting for a CONNACK. One arriving at
  // any other time answers nothing, and taking it as an acceptance would mark a
  // torn-down session connected.
  if (!connecting_) return;
  connecting_ = false;

  if (!connack.has_value()) {
    connack_error_ = NetworkError(NetworkErrc::kProtocolError, 0,
                                  "malformed CONNACK");
  } else if (connack->code == MqttConnectReturnCode::kAccepted) {
    connected_ = true;
  } else {
    const bool credentials =
        connack->code == MqttConnectReturnCode::kNotAuthorized ||
        connack->code == MqttConnectReturnCode::kBadCredentials;
    // The broker's own code is carried through as the native error: which of
    // the six refusals it was is the difference between a fixable
    // configuration problem and one the device cannot do anything about.
    connack_error_ = NetworkError(
        credentials ? NetworkErrc::kMqttNotAuthorized
                    : NetworkErrc::kConnectFailed,
        static_cast<int>(connack->code),
        DescribeConnectReturnCode(connack->code));
  }
  connack_ready_ = true;
  cv_.notify_all();
}

void SoftwareMqttClient::HandlePublish(const MqttPacket& packet) {
  // Both QoS bits set is not a QoS this protocol has; it is a violation of it,
  // and the specification is explicit that the connection is then closed
  // ([MQTT-3.3.1-4]). Checked here rather than left to ParsePublish, which
  // reports the same packet as merely unreadable - and an unreadable PUBLISH is
  // not something the protocol asks a client to hang up over.
  if (packet.header.qos > 2) {
    ReportError(NetworkError(NetworkErrc::kProtocolError, packet.header.qos,
                             "PUBLISH with both QoS bits set"));
    Teardown(false);
    return;
  }

  auto message = ParsePublish(packet.header, packet.body);
  if (!message.has_value()) {
    ReportError(NetworkError(NetworkErrc::kProtocolError, 0,
                             "malformed PUBLISH from broker"));
    return;
  }

  if (packet.header.qos == 2) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // emplace keeps the first copy: a broker that resends a PUBLISH under an
      // id already held is repeating itself, and the original payload is the
      // one being delivered.
      pending_qos2_.emplace(message->packet_id, *message);
    }
    // Sent even for a repeat, because a repeat means the PUBREC was lost.
    SendPacket(EncodePacketIdAck(MqttPacketType::kPubrec, message->packet_id));
    return;
  }

  if (packet.header.qos == 1) {
    // Acknowledged before delivery: the broker is entitled to resend until it
    // hears the PUBACK, and a duplicate arriving while a callback runs would
    // deliver the same message twice.
    SendPacket(EncodePacketIdAck(MqttPacketType::kPuback, message->packet_id));
  }
  Deliver(std::move(*message));
}

void SoftwareMqttClient::HandlePubrec(const MqttPacket& packet) {
  auto packet_id = ParsePacketId(packet.body);
  if (!packet_id.has_value()) {
    ReportError(NetworkError(NetworkErrc::kProtocolError, 0,
                             "malformed PUBREC from broker"));
    return;
  }
  // The second half of the QoS 2 handshake: the broker now knows we hold it.
  SendPacket(EncodePacketIdAck(MqttPacketType::kPubrel, *packet_id));
}

void SoftwareMqttClient::HandlePubrel(const MqttPacket& packet) {
  auto packet_id = ParsePacketId(packet.body);
  if (!packet_id.has_value()) {
    ReportError(NetworkError(NetworkErrc::kProtocolError, 0,
                             "malformed PUBREL from broker"));
    return;
  }

  std::optional<MqttPublishMessage> message;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_qos2_.find(*packet_id);
    if (it != pending_qos2_.end()) {
      message = std::move(it->second);
      pending_qos2_.erase(it);
    }
  }

  // Answered whether or not the message was still held: a PUBREL for an id we
  // have no record of still needs its PUBCOMP, or the broker will keep sending
  // it. What it must not do is deliver a message that was not there.
  SendPacket(EncodePacketIdAck(MqttPacketType::kPubcomp, *packet_id));
  if (message.has_value()) Deliver(std::move(*message));
}

void SoftwareMqttClient::HandlePublishComplete(const MqttPacket& packet) {
  auto packet_id = ParsePacketId(packet.body);
  if (!packet_id.has_value()) {
    ReportError(NetworkError(NetworkErrc::kProtocolError, 0,
                             "malformed publish acknowledgement from broker"));
    return;
  }
  if (on_publish_ack_) on_publish_ack_(static_cast<int>(*packet_id));
}

void SoftwareMqttClient::HandleSuback(const MqttPacket& packet) {
  auto suback = ParseSuback(packet.body);
  if (!suback.has_value()) {
    ReportError(NetworkError(NetworkErrc::kProtocolError, 0,
                             "malformed SUBACK from broker"));
    return;
  }

  std::string topic;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_topics_.find(suback->packet_id);
    if (it != pending_topics_.end()) {
      topic = it->second;
      pending_topics_.erase(it);
    }
  }

  for (uint8_t code : suback->return_codes) {
    // 0x80 is the broker refusing the filter. There is no return value from
    // Subscribe() to report it through - the packet id is already gone by the
    // time this arrives - so it is reported where every other asynchronous
    // failure is.
    if (code == 0x80) {
      ReportError(NetworkError(
          NetworkErrc::kProtocolError, 0x80,
          "broker refused the subscription to '" + topic + "'"));
      return;
    }
  }
}

void SoftwareMqttClient::HandleUnsuback(const MqttPacket& packet) {
  auto packet_id = ParsePacketId(packet.body);
  if (!packet_id.has_value()) {
    ReportError(NetworkError(NetworkErrc::kProtocolError, 0,
                             "malformed UNSUBACK from broker"));
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  pending_topics_.erase(*packet_id);
}

void SoftwareMqttClient::Deliver(MqttPublishMessage&& message) {
  if (!on_message_) return;
  MqttMessage delivered;
  delivered.topic = std::move(message.topic);
  delivered.payload = std::move(message.payload);
  // ParsePublish refuses a QoS above 2, and HandlePublish ends the connection
  // before that on one, so every value that reaches here is one the enum names.
  delivered.qos = static_cast<MqttQoS>(message.qos);
  delivered.retain = message.retain;
  delivered.message_id = static_cast<int>(message.packet_id);
  on_message_(std::move(delivered));
}

void SoftwareMqttClient::ReportError(const NetworkError& error) {
  if (on_error_) on_error_(error);
}

uint16_t SoftwareMqttClient::NextPacketIdLocked() {
  last_packet_id_ = last_packet_id_ == 0xFFFF ? 1 : last_packet_id_ + 1;
  return last_packet_id_;
}

}  // namespace esp_modem_link::protocol
