#pragma once

#include <memory>

#include "esp_modem_link/network_interface.h"
#include "hal/hal_tcp_client.h"
#include "hal/hal_udp_client.h"
#include "hal/imodule_hal.h"

namespace esp_modem_link::network {

class CellularNetwork : public NetworkInterface {
 public:
  explicit CellularNetwork(hal::IModuleHal& hal);

  Result<std::unique_ptr<TcpClient>> CreateTcp() override;
  Result<std::unique_ptr<TcpClient>> CreateSsl() override;
  Result<std::unique_ptr<UdpClient>> CreateUdp() override;

  Result<std::unique_ptr<HttpClient>> CreateHttp() override;
  Result<std::unique_ptr<MqttClient>> CreateMqtt() override;
  Result<std::unique_ptr<WebSocketClient>> CreateWebSocket() override;

  bool HasCapability(NetworkProtocol proto) const override;
  int GetMaxConnections(NetworkProtocol proto) const override;

 private:
  hal::IModuleHal& hal_;
};

}  // namespace esp_modem_link::network
