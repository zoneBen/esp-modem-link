#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "esp_modem_link/callbacks.h"
#include "esp_modem_link/config_types.h"
#include "esp_modem_link/network_error.h"

namespace esp_modem_link {

enum class MqttQoS {
  kQoS0 = 0,
  kQoS1 = 1,
  kQoS2 = 2,
};

// One message as it arrived, with the delivery properties the broker gave it.
// They belong to the message rather than to the notification, because a caller
// that subscribed at one QoS and is being handed a message at another has no
// other way to find out which guarantee it came under - and a message published
// with retain set is a different thing from one that merely matched a filter.
struct MqttMessage {
  std::string topic;
  std::string payload;
  MqttQoS qos = MqttQoS::kQoS0;
  // Set only on a message the broker sent because a subscription was just made.
  // The protocol requires it to be clear on everything forwarded from an
  // existing subscription, so this is usually - but not always - false.
  bool retain = false;
  // The packet id of the PUBLISH it arrived under, or zero at QoS 0, which has
  // no id on the wire. Meaningless as a handle by the time the callback runs:
  // the acknowledgements for it are already sent.
  int message_id = 0;
};

// Taken by value rather than by reference, for two reasons that point the same
// way. The message owns its strings, so a callback that wants to keep it should
// be able to take it rather than copy it - and a `const MqttMessage&` to the
// engine's local would look just as safe to a caller who stored it. The engine
// has to build this object either way, so the move costs nothing.
//
// It is declared here and not in callbacks.h because it names MqttQoS, and that
// header is a list of aliases over types it does not own.
using MqttMessageCallback = std::function<void(MqttMessage message)>;

class MqttClient {
 public:
  virtual ~MqttClient() = default;

  virtual void SetKeepAlive(int seconds) = 0;
  virtual void SetClientId(std::string_view client_id) = 0;
  virtual void SetCredentials(std::string_view user,
                              std::string_view pass) = 0;
  virtual void SetWill(const MqttWill& will) = 0;
  virtual void SetTlsConfig(const TlsConfig& config) = 0;

  // `clean_session` is a parameter rather than a setter because it is a field of
  // the CONNECT packet, and the CONNECT packet is sent once: the only thing a
  // setter could do is change what the next Connect() means, which this already
  // says in the same breath as making the call.
  virtual Result<> Connect(std::string_view host,
                           uint16_t port,
                           bool clean_session = true) = 0;
  virtual void Disconnect() = 0;

  virtual Result<int> Publish(std::string_view topic,
                              std::string_view payload,
                              MqttQoS qos = MqttQoS::kQoS0,
                              bool retain = false) = 0;
  virtual Result<int> Subscribe(std::string_view topic,
                                MqttQoS qos = MqttQoS::kQoS0) = 0;
  virtual Result<> Unsubscribe(std::string_view topic) = 0;

  // Register these before Connect(). Each is read on the thread that delivers
  // it - the transport's receive thread, or the keep-alive task - without a lock
  // held, so replacing one mid-session is a race against a delivery in flight
  // rather than something this class can make safe for you.
  void OnConnected(EventCallback callback) {
    on_connected_ = std::move(callback);
  }
  void OnDisconnected(EventCallback callback) {
    on_disconnected_ = std::move(callback);
  }
  void OnMessage(MqttMessageCallback callback) {
    on_message_ = std::move(callback);
  }
  void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }
  // Fires when the broker has taken a message: on the PUBACK of a QoS 1 publish
  // or the PUBCOMP of a QoS 2 one. Named for the handshake ending rather than
  // for either half of it, and never fired for QoS 0, which nothing answers.
  void OnPublishComplete(PublishAckCallback callback) {
    on_publish_ack_ = std::move(callback);
  }

  bool IsConnected() const { return connected_.load(); }

 protected:
  EventCallback on_connected_;
  EventCallback on_disconnected_;
  MqttMessageCallback on_message_;
  ErrorCallback on_error_;
  PublishAckCallback on_publish_ack_;
  // Atomic because the engine sets it from the transport's receive thread and
  // the application reads it from its own.
  std::atomic<bool> connected_{false};
};

}  // namespace esp_modem_link
