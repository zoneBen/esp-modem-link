#include "network/cellular_network.h"

#include "protocol/http/software_http_client.h"

namespace esp_modem_link::network {

CellularNetwork::CellularNetwork(hal::IModuleHal& hal) : hal_(hal) {}

Result<std::unique_ptr<TcpClient>> CellularNetwork::CreateTcp() {
  const auto& caps = hal_.GetCapabilities();
  if (!caps.tcp) {
    return std::unexpected(
        NetworkError::NotSupported("TCP not supported by module"));
  }
  return std::make_unique<hal::HalTcpClient>(hal_, false);
}

Result<std::unique_ptr<TcpClient>> CellularNetwork::CreateSsl() {
  const auto& caps = hal_.GetCapabilities();
  if (!caps.ssl_tcp) {
    return std::unexpected(
        NetworkError::NotSupported("SSL/TLS not supported by module"));
  }
  return std::make_unique<hal::HalTcpClient>(hal_, true);
}

Result<std::unique_ptr<UdpClient>> CellularNetwork::CreateUdp() {
  const auto& caps = hal_.GetCapabilities();
  if (!caps.udp) {
    return std::unexpected(
        NetworkError::NotSupported("UDP not supported by module"));
  }
  return std::make_unique<hal::HalUdpClient>(hal_);
}

Result<std::unique_ptr<HttpClient>> CellularNetwork::CreateHttp() {
  const auto& caps = hal_.GetCapabilities();

  // Builtin where the firmware has an HTTP stack, unless the caller has asked
  // for the software engine specifically. A builtin stack that will not come up
  // is not the end of the request in Auto mode: the module can still carry
  // HTTP/1.1 over a raw socket, and asking for the builtin one was this layer's
  // preference rather than the caller's requirement. An explicit kBuiltin is a
  // requirement, so its failure is reported instead of being papered over.
  if (protocol_mode_ != ProtocolMode::kSoftware && caps.http &&
      hal_.HasBuiltinHttp()) {
    auto builtin = hal_.CreateBuiltinHttp();
    if (builtin || protocol_mode_ == ProtocolMode::kBuiltin) return builtin;
  }

  // Otherwise HTTP/1.1 runs over a raw socket from the same HAL, so a module
  // gets the same protocol behaviour whether or not its firmware has an HTTP
  // stack of its own. The transport is built lazily because only the parsed URL
  // says whether the request needs TLS.
  if (!caps.tcp) {
    return std::unexpected(NetworkError::NotSupported(
        "HTTP not available: the module has neither a builtin HTTP stack nor "
        "TCP"));
  }
  return std::unique_ptr<HttpClient>(std::make_unique<protocol::SoftwareHttpClient>(
      [this](bool tls,
             const TlsConfig&) -> Result<std::unique_ptr<TcpClient>> {
        if (tls && !hal_.GetCapabilities().ssl_tcp) {
          return std::unexpected(NetworkError::NotSupported(
              "HTTPS requires TLS, which this module does not support"));
        }
        return std::unique_ptr<TcpClient>(
            std::make_unique<hal::HalTcpClient>(hal_, tls));
      }));
}

Result<std::unique_ptr<MqttClient>> CellularNetwork::CreateMqtt() {
  // No fallback here yet, unlike HTTP: the software engine this would fall back
  // to does not exist, so a failed builtin attempt has nothing to land on. It
  // will take the same shape as CreateHttp once it does.
  if (protocol_mode_ != ProtocolMode::kSoftware && hal_.HasBuiltinMqtt()) {
    return hal_.CreateBuiltinMqtt();
  }
  return std::unexpected(
      NetworkError::NotSupported(
          "MQTT not available (software mode not implemented yet)"));
}

Result<std::unique_ptr<WebSocketClient>> CellularNetwork::CreateWebSocket() {
  return std::unexpected(
      NetworkError::NotSupported(
          "WebSocket not available (software mode not implemented yet)"));
}

bool CellularNetwork::HasCapability(NetworkProtocol proto) const {
  const auto& caps = hal_.GetCapabilities();
  switch (proto) {
    case NetworkProtocol::kTcp:
      return caps.tcp;
    case NetworkProtocol::kSsl:
      return caps.ssl_tcp;
    case NetworkProtocol::kUdp:
      return caps.udp;
    case NetworkProtocol::kHttp:
    case NetworkProtocol::kHttps:
      return caps.http;
    case NetworkProtocol::kMqtt:
    case NetworkProtocol::kMqtts:
      return caps.mqtt;
    case NetworkProtocol::kWebSocket:
    case NetworkProtocol::kWss:
      return false;
  }
  return false;
}

int CellularNetwork::GetMaxConnections(NetworkProtocol proto) const {
  const auto& caps = hal_.GetCapabilities();
  switch (proto) {
    case NetworkProtocol::kTcp:
    case NetworkProtocol::kSsl:
    case NetworkProtocol::kUdp:
      return caps.max_connections;
    default:
      return 0;
  }
}

}  // namespace esp_modem_link::network
