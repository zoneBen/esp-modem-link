#pragma once

#include <memory>

#include "esp_modem_link/common_types.h"
#include "esp_modem_link/http_client.h"
#include "esp_modem_link/mqtt_client.h"
#include "esp_modem_link/network_error.h"
#include "esp_modem_link/tcp_client.h"
#include "esp_modem_link/udp_client.h"
#include "esp_modem_link/websocket_client.h"

namespace esp_modem_link {

class NetworkInterface {
 public:
  virtual ~NetworkInterface() = default;

  virtual Result<std::unique_ptr<TcpClient>> CreateTcp() = 0;
  virtual Result<std::unique_ptr<TcpClient>> CreateSsl() = 0;
  virtual Result<std::unique_ptr<UdpClient>> CreateUdp() = 0;

  virtual Result<std::unique_ptr<HttpClient>> CreateHttp() = 0;
  virtual Result<std::unique_ptr<MqttClient>> CreateMqtt() = 0;
  virtual Result<std::unique_ptr<WebSocketClient>> CreateWebSocket() = 0;

  virtual bool HasCapability(NetworkProtocol proto) const = 0;
  virtual int GetMaxConnections(NetworkProtocol proto) const = 0;

  virtual void SetProtocolMode(ProtocolMode mode) { protocol_mode_ = mode; }
  virtual ProtocolMode GetProtocolMode() const { return protocol_mode_; }

 protected:
  ProtocolMode protocol_mode_ = ProtocolMode::kAuto;
};

}  // namespace esp_modem_link
