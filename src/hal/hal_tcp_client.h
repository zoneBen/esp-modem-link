#pragma once

#include <cstdint>
#include <string_view>

#include "esp_modem_link/tcp_client.h"
#include "hal/imodule_hal.h"

namespace esp_modem_link::hal {

class HalTcpClient : public TcpClient {
 public:
  explicit HalTcpClient(IModuleHal& hal, bool ssl = false);
  ~HalTcpClient() override;

  Result<> Connect(std::string_view host, uint16_t port) override;
  void Disconnect() override;
  Result<int> Send(const void* data, size_t len) override;

  size_t GetSendBufferFree() const override;

 private:
  void OnTcpData(int connect_id, std::string_view data);
  void OnTcpClose(int connect_id);

  IModuleHal& hal_;
  bool ssl_ = false;
  int connect_id_ = -1;
};

}  // namespace esp_modem_link::hal
