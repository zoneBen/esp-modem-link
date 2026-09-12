#include "hal/hal_tcp_client.h"

#include <cstring>

namespace esp_modem_link::hal {

HalTcpClient::HalTcpClient(IModuleHal& hal, bool ssl)
    : hal_(hal), ssl_(ssl) {}

HalTcpClient::~HalTcpClient() {
  if (connected_) {
    Disconnect();
  }
  // The HAL outlives this client and holds these closures, so drop them rather
  // than leave them pointing at a destroyed 'this'.
  hal_.SetTcpDataCallback(nullptr);
  hal_.SetTcpCloseCallback(nullptr);
}

Result<> HalTcpClient::Connect(std::string_view host, uint16_t port) {
  if (connected_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kAlreadyConnected, 0, "already connected"));
  }

  auto result = hal_.TcpConnect(host, port, ssl_);
  if (!result.has_value()) {
    return std::unexpected(result.error());
  }

  connect_id_ = result.value();
  connected_ = true;

  // Inbound payload and peer closes arrive unsolicited, so the HAL needs a
  // route back to this client's user callbacks. The HAL accepts a single
  // subscriber per direction, which is why one client owns it for its lifetime.
  hal_.SetTcpDataCallback(
      [this](int id, std::string_view data) { OnTcpData(id, data); });
  hal_.SetTcpCloseCallback([this](int id) { OnTcpClose(id); });

  return {};
}

void HalTcpClient::Disconnect() {
  if (!connected_) return;

  // Clear the id before closing: the module answers with a close URC, and the
  // handlers below must not treat our own teardown as a peer-initiated event.
  int id = connect_id_;
  connect_id_ = -1;
  connected_ = false;

  hal_.TcpClose(id);

  if (on_disconnected_) {
    on_disconnected_();
  }
}

Result<int> HalTcpClient::Send(const void* data, size_t len) {
  if (!connected_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotConnected, 0, "not connected"));
  }
  return hal_.TcpSend(connect_id_, data, len);
}

size_t HalTcpClient::GetSendBufferFree() const {
  return SIZE_MAX;
}

void HalTcpClient::OnTcpData(int connect_id, std::string_view data) {
  // The HAL reports every socket; only this client's connection is ours.
  if (connect_id != connect_id_ || !on_data_) return;
  on_data_(data);
}

void HalTcpClient::OnTcpClose(int connect_id) {
  if (connect_id != connect_id_) return;

  connect_id_ = -1;
  connected_ = false;
  if (on_disconnected_) {
    on_disconnected_();
  }
}

}  // namespace esp_modem_link::hal
