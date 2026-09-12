#pragma once

#include <cstdint>
#include <string_view>

#include "esp_modem_link/udp_client.h"
#include "hal/imodule_hal.h"

namespace esp_modem_link::hal {

class HalUdpClient : public UdpClient {
 public:
  explicit HalUdpClient(IModuleHal& hal);
  ~HalUdpClient() override;

  Result<> Connect(std::string_view host, uint16_t port) override;
  Result<> Bind(uint16_t port) override;
  void Disconnect() override;
  Result<int> Send(const void* data, size_t len) override;
  Result<int> SendTo(const void* data,
                     size_t len,
                     std::string_view host,
                     uint16_t port) override;

 private:
  // Bound to the HAL in Connect(). The HAL reports datagrams for every socket it
  // owns, so this filters down to the connection this client opened.
  void OnUdpData(int connect_id,
                 std::string_view host,
                 uint16_t port,
                 std::string_view data);

  IModuleHal& hal_;
  int connect_id_ = -1;
  std::string remote_host_;
  uint16_t remote_port_ = 0;
};

}  // namespace esp_modem_link::hal
