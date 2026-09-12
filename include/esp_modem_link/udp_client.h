#pragma once

#include <cstdint>
#include <string_view>

#include "esp_modem_link/callbacks.h"
#include "esp_modem_link/network_error.h"

namespace esp_modem_link {

using UdpMessageCallback = std::function<void(std::string_view host,
                                               uint16_t port,
                                               std::string_view data)>;

class UdpClient {
 public:
  virtual ~UdpClient() = default;

  virtual Result<> Connect(std::string_view host, uint16_t port) = 0;
  virtual Result<> Bind(uint16_t port) = 0;
  virtual void Disconnect() = 0;
  virtual Result<int> Send(const void* data, size_t len) = 0;
  virtual Result<int> SendTo(const void* data,
                             size_t len,
                             std::string_view host,
                             uint16_t port) = 0;

  void OnMessage(UdpMessageCallback callback) {
    on_message_ = std::move(callback);
  }
  void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }

  bool IsConnected() const { return connected_; }

 protected:
  UdpMessageCallback on_message_;
  ErrorCallback on_error_;
  bool connected_ = false;
};

}  // namespace esp_modem_link
