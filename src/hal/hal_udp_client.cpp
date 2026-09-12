#include "hal/hal_udp_client.h"

namespace esp_modem_link::hal {

HalUdpClient::HalUdpClient(IModuleHal& hal) : hal_(hal) {}

HalUdpClient::~HalUdpClient() {
  if (connected_) {
    Disconnect();
  }
  // Normally finds nothing: Disconnect() already withdrew. It is here so that no
  // path out of this object can leave the HAL - which outlives every client -
  // holding a closure into freed memory.
  hal_.Unsubscribe(subscription_);
  subscription_ = 0;
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

  subscription_ = hal_.SubscribeUdp(
      connect_id_,
      [this](int id, std::string_view src_host, uint16_t src_port,
             std::string_view data) {
        OnUdpData(id, src_host, src_port, data);
      });
  return {};
}

// The HAL routes by cid, so this only ever sees this client's own socket.
void HalUdpClient::OnUdpData(int /*connect_id*/,
                             std::string_view host,
                             uint16_t port,
                             std::string_view data) {
  if (!on_message_) return;
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

  // Withdrawn before the close, by handle rather than by cid: the pool may
  // already have handed this cid to another client, and a withdrawal by cid
  // would take that client's route with it.
  hal_.Unsubscribe(subscription_);
  subscription_ = 0;

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
