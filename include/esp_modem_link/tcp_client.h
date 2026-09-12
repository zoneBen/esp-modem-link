#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "esp_modem_link/callbacks.h"
#include "esp_modem_link/network_error.h"

namespace esp_modem_link {

class TcpClient {
 public:
  virtual ~TcpClient() = default;

  virtual Result<> Connect(std::string_view host, uint16_t port) = 0;
  virtual void Disconnect() = 0;
  virtual Result<int> Send(const void* data, size_t len) = 0;

  void OnData(DataCallback callback) { on_data_ = std::move(callback); }
  void OnDisconnected(EventCallback callback) {
    on_disconnected_ = std::move(callback);
  }
  void OnError(ErrorCallback callback) { on_error_ = std::move(callback); }

  bool IsConnected() const { return connected_; }

  virtual size_t GetSendBufferFree() const { return SIZE_MAX; }

 protected:
  DataCallback on_data_;
  EventCallback on_disconnected_;
  ErrorCallback on_error_;
  bool connected_ = false;
};

}  // namespace esp_modem_link
