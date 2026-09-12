#pragma once

#include <chrono>
#include <cstdint>
#include <string_view>

#include "esp_modem_link/callbacks.h"
#include "esp_modem_link/network_error.h"

namespace esp_modem_link {

enum class WebSocketCloseCode : uint16_t {
  kNormal = 1000,
  kGoingAway = 1001,
  kProtocolError = 1002,
  kUnsupportedData = 1003,
  kNoStatus = 1005,
  kAbnormalClosure = 1006,
  kInvalidPayloadData = 1007,
  kPolicyViolation = 1008,
  kMessageTooBig = 1009,
  kMandatoryExtension = 1010,
  kInternalError = 1011,
};

class WebSocketClient {
 public:
  virtual ~WebSocketClient() = default;

  virtual void SetHeader(std::string_view key, std::string_view value) = 0;
  virtual void SetHeartbeat(std::chrono::seconds interval,
                            std::chrono::seconds timeout) = 0;
  virtual void SetAutoReconnect(bool enable, int max_retries = -1) = 0;

  virtual Result<> Connect(std::string_view url) = 0;
  virtual void Close(WebSocketCloseCode code = WebSocketCloseCode::kNormal,
                     std::string_view reason = "") = 0;

  virtual Result<> Send(std::string_view data, bool binary = false) = 0;
  virtual Result<> SendFragment(const void* data,
                                size_t len,
                                bool binary,
                                bool fin) = 0;
  virtual void Ping(std::string_view payload = "") = 0;

  void OnConnected(EventCallback callback) {
    on_connected_ = std::move(callback);
  }
  void OnDisconnected(CloseCallback callback) {
    on_disconnected_ = std::move(callback);
  }
  void OnMessage(DataCallback callback) {
    on_message_ = std::move(callback);
  }
  void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }
  void OnPong(DataCallback callback) { on_pong_ = std::move(callback); }

  bool IsConnected() const { return connected_; }

 protected:
  EventCallback on_connected_;
  CloseCallback on_disconnected_;
  DataCallback on_message_;
  ErrorCallback on_error_;
  DataCallback on_pong_;
  bool connected_ = false;
};

}  // namespace esp_modem_link
