#pragma once

#include <atomic>
#include <cstdint>
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

class MqttClient {
 public:
  virtual ~MqttClient() = default;

  virtual void SetKeepAlive(int seconds) = 0;
  virtual void SetClientId(std::string_view client_id) = 0;
  virtual void SetCredentials(std::string_view user,
                              std::string_view pass) = 0;
  virtual void SetWill(const MqttWill& will) = 0;
  virtual void SetTlsConfig(const TlsConfig& config) = 0;

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

  void OnConnected(EventCallback callback) {
    on_connected_ = std::move(callback);
  }
  void OnDisconnected(EventCallback callback) {
    on_disconnected_ = std::move(callback);
  }
  void OnMessage(MessageCallback callback) {
    on_message_ = std::move(callback);
  }
  void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }
  void OnPublishComplete(PublishAckCallback callback) {
    on_publish_ack_ = std::move(callback);
  }

  bool IsConnected() const { return connected_.load(); }

 protected:
  EventCallback on_connected_;
  EventCallback on_disconnected_;
  MessageCallback on_message_;
  ErrorCallback on_error_;
  PublishAckCallback on_publish_ack_;
  // Atomic because the engine sets it from the transport's receive thread and
  // the application reads it from its own.
  std::atomic<bool> connected_{false};
};

}  // namespace esp_modem_link
