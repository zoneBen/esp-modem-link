#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "esp_modem_link/mqtt_client.h"
#include "esp_modem_link/tcp_client.h"
#include "platform/itask.h"
#include "protocol/mqtt/mqtt_packet.h"

namespace esp_modem_link::protocol {

// Same shape as the HTTP engine's factory: the client names the security it
// wants and the caller decides what that means on this module.
using MqttTransportFactory = std::function<Result<std::unique_ptr<TcpClient>>(
    bool tls,
    const TlsConfig& config)>;

// MQTT 3.1.1 over any TcpClient, so a module whose firmware has no MQTT stack
// still gets one.
//
// Two things about this class are worth knowing before reading it:
//
// Callbacks run on the transport's receive thread - the AT layer's for a
// cellular module - and also from the keep-alive task, which is where a broker
// that has stopped answering is noticed. They are never called with an internal
// lock held, so a callback may publish, subscribe or disconnect, but it must
// not assume it is on the thread that called Connect().
//
// TLS is switched on by SetTlsConfig(), not by the port number. MqttClient has
// no scheme in its Connect() to read the caller's intent from, and a client that
// guessed 8883 would silently pick a transport nobody asked for.
class SoftwareMqttClient : public MqttClient {
 public:
  // The CONNACK wait is a constructor argument rather than a setter because
  // MqttClient has no SetTimeout, and a caller only ever wants to shorten it.
  explicit SoftwareMqttClient(
      MqttTransportFactory factory,
      std::chrono::milliseconds connack_timeout = std::chrono::seconds(10));
  ~SoftwareMqttClient() override;

  // Seconds between PINGREQs when the connection is otherwise idle. Zero
  // disables keep-alive, as the specification allows (3.1.2.10). Read when the
  // connection is established, so a change applies from the next Connect().
  void SetKeepAlive(int seconds) override;
  void SetClientId(std::string_view client_id) override;
  void SetCredentials(std::string_view user, std::string_view pass) override;
  void SetWill(const MqttWill& will) override;
  void SetTlsConfig(const TlsConfig& config) override;

  // Returns once the broker has accepted the connection, so a successful
  // return means the session is usable and a refusal is reported as the error
  // it is rather than being discovered by the first publish.
  Result<> Connect(std::string_view host,
                   uint16_t port,
                   bool clean_session = true) override;
  void Disconnect() override;

  // Returns the packet id the message was sent under, or zero for QoS 0 - which
  // carries no packet id on the wire and will never be acknowledged, so
  // OnPublishComplete does not fire for it.
  Result<int> Publish(std::string_view topic,
                      std::string_view payload,
                      MqttQoS qos = MqttQoS::kQoS0,
                      bool retain = false) override;
  Result<int> Subscribe(std::string_view topic,
                        MqttQoS qos = MqttQoS::kQoS0) override;
  Result<> Unsubscribe(std::string_view topic) override;

 private:
  // Transport callbacks, invoked from the receive thread.
  void OnTransportData(std::string_view data);
  void OnTransportClosed();

  void HandlePacket(const MqttPacket& packet);
  void HandleConnack(const MqttPacket& packet);
  void HandlePublish(const MqttPacket& packet);
  void HandlePublishComplete(const MqttPacket& packet);
  void HandlePubrec(const MqttPacket& packet);
  void HandlePubrel(const MqttPacket& packet);
  void HandleSuback(const MqttPacket& packet);
  void HandleUnsuback(const MqttPacket& packet);

  void Deliver(const MqttPublishMessage& message);

  // Sends one packet, or reports why it could not be sent. Safe to call from
  // the keep-alive task: the transport is held by shared_ptr for the duration
  // of the call, so a concurrent Disconnect() cannot free it mid-send.
  Result<> SendPacket(std::string_view packet);

  // Closes the connection from whichever thread noticed it ended, notifying
  // OnDisconnected. Never joins the keep-alive task - it may be the keep-alive
  // task that noticed, and a task joining itself is a deadlock - so it stops
  // the loop and lets the join happen in StopKeepAlive().
  void Teardown(bool send_disconnect);

  void StartKeepAlive();
  void StopKeepAlive();
  void KeepAliveLoop();

  void ReportError(const NetworkError& error);

  // Caller holds mutex_. Ids run 1..65535: zero is reserved, so the counter
  // skips it rather than letting it onto the wire.
  uint16_t NextPacketIdLocked();

  MqttTransportFactory factory_;
  const std::chrono::milliseconds connack_timeout_;
  std::shared_ptr<TcpClient> transport_;

  // Guards every member the receive thread and the keep-alive task touch. Never
  // held across a call into the transport, for the same reason as the HTTP
  // engine: a transport that answers inside Send() would otherwise re-enter.
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  std::string rx_buffer_;

  std::string client_id_;
  std::string username_;
  std::string password_;
  bool has_credentials_ = false;
  int keep_alive_seconds_ = 60;
  bool has_will_ = false;
  MqttWill will_;
  bool tls_enabled_ = false;
  TlsConfig tls_;

  // True only between sending a CONNECT and answering it. A CONNACK outside
  // that window answers nothing - it is either a duplicate or the last gasp of
  // a connection this client has already given up - and must not be allowed to
  // mark the session connected again.
  bool connecting_ = false;
  bool connack_ready_ = false;
  std::optional<NetworkError> connack_error_;

  uint16_t last_packet_id_ = 0;
  // One entry per outstanding SUBSCRIBE or UNSUBSCRIBE, so a SUBACK can say
  // which filter the broker refused.
  std::unordered_map<uint16_t, std::string> pending_topics_;
  // Inbound QoS 2 messages, held from PUBREC until PUBREL. Delivering on PUBREL
  // rather than on PUBLISH is what makes QoS 2 exactly-once: a broker that
  // resends the PUBLISH never reaches this map twice.
  std::unordered_map<uint16_t, MqttPublishMessage> pending_qos2_;

  std::unique_ptr<platform::ITask> keepalive_task_;
  std::atomic<bool> keepalive_running_{false};
  // Milliseconds on the platform clock. Idle time is measured from the last
  // packet of any kind, which is what the specification asks for.
  std::atomic<long long> last_sent_ms_{0};
  // Zero when no PINGREQ is outstanding.
  std::atomic<long long> ping_sent_ms_{0};
};

}  // namespace esp_modem_link::protocol
