#include "hal/hal_udp_client.h"

namespace esp_modem_link::hal {

HalUdpClient::HalUdpClient(IModuleHal& hal) : hal_(hal) {}

HalUdpClient::~HalUdpClient() {
  if (connected_) {
    Disconnect();
  }
  // The HAL outlives this client and holds this closure, so drop it rather than
  // leave it pointing at a destroyed 'this'.
  hal_.SetUdpDataCallback(nullptr);
}

Result<> HalUdpClient::Connect(std::string_view host, uint16_t port) {
  if (connected_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kAlreadyConnected, 0, "already connected"));
  }

  auto result = hal_.UdpOpen(host, port);
  if (!result.has_value()) {
    return std::unexpected(result.error());
  }

  connect_id_ = result.value();
  remote_host_ = std::string(host);
  remote_port_ = port;
  connected_ = true;

  hal_.SetUdpDataCallback(
      [this](int id, std::string_view src_host, uint16_t src_port,
             std::string_view data) {
        OnUdpData(id, src_host, src_port, data);
      });
  return {};
}

void HalUdpClient::OnUdpData(int connect_id,
                             std::string_view host,
                             uint16_t port,
                             std::string_view data) {
  // The HAL reports every socket; only this client's connection is ours.
  if (connect_id != connect_id_ || !on_message_) return;
  // Not every module reports the datagram's source, so fall back to the peer
  // this socket was opened against. On a connected UDP socket that is the only
  // address a datagram can arrive from.
  if (host.empty()) {
    host = remote_host_;
    port = remote_port_;
  }
  on_message_(host, port, data);
}

Result<> HalUdpClient::Bind(uint16_t port) {
  (void)port;
  return std::unexpected(
      NetworkError::NotSupported("UDP Bind not supported by module HAL"));
}

void HalUdpClient::Disconnect() {
  if (!connected_) return;

  int id = connect_id_;
  connect_id_ = -1;
  connected_ = false;

  hal_.UdpClose(id);
}

Result<int> HalUdpClient::Send(const void* data, size_t len) {
  if (!connected_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotConnected, 0, "not connected"));
  }
  return hal_.UdpSend(connect_id_, data, len);
}

Result<int> HalUdpClient::SendTo(const void* data,
                                 size_t len,
                                 std::string_view host,
                                 uint16_t port) {
  (void)host;
  (void)port;
  if (!connected_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotConnected, 0, "not connected"));
  }
  return hal_.UdpSend(connect_id_, data, len);
}

}  // namespace esp_modem_link::hal
