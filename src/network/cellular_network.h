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
  // The transport the software protocol engines run on: a raw socket from the
  // HAL, opened for TLS when the engine says it needs it and configured from the
  // caller's TlsConfig when it is. Shared by every engine so that "this module
  // has no TLS" is answered in one place.
  //
  // The engine passes the config it was given rather than this class holding
  // one: an engine outlives the connections it makes, and SetTlsConfig between
  // two of them is meant to apply to the second.
  Result<std::unique_ptr<TcpClient>> OpenTransport(bool tls,
                                                   const TlsConfig& config);

  hal::IModuleHal& hal_;
};

}  // namespace esp_modem_link::network
