#include "hal/hal_tcp_client.h"

#include <cstring>

namespace esp_modem_link::hal {

HalTcpClient::HalTcpClient(IModuleHal& hal, bool ssl, const TlsConfig& config)
    : hal_(hal), ssl_(ssl), tls_config_(config) {}

HalTcpClient::~HalTcpClient() {
  if (connected_) {
    Disconnect();
  }
  // Normally finds nothing: Disconnect() withdraws for a connected client, and a
  // peer close has already had its route erased by the HAL. It is here so that no
  // path out of this object can leave the HAL - which outlives every client -
  // holding a closure into freed memory.
  hal_.Unsubscribe(subscription_);
  subscription_ = 0;
}

Result<> HalTcpClient::Connect(std::string_view host, uint16_t port) {
  if (connected_) {
    return std::unexpected(
        NetworkError(NetworkErrc::kAlreadyConnected, 0, "already connected"));
  }

  auto result = hal_.TcpConnect(host, port, ssl_, tls_config_);
  if (!result.has_value()) {
    return std::unexpected(result.error());
  }

  const int connect_id = result.value();
  connect_id_ = connect_id;
  connected_ = true;
  const uint32_t my_generation = ++generation_;

  // Inbound payload and peer closes arrive unsolicited, so the HAL needs a route
  // back to this client's user callbacks. The subscription is per connection, so
  // several clients share the HAL without seeing each other's traffic.
  //
  // A peer that sent or closed in the instant between the socket opening and this
  // call has its events held by the HAL and delivered from inside here, so the two
  // fields above are set before the route exists - which is what lets a close that
  // arrives that early leave this object disconnected rather than wrongly
  // connected. The close is also the reason this must stay the last thing Connect
  // does: nothing after it may assume the connection is still open.
  const HalSubscription handle = hal_.SubscribeTcp(
      connect_id_,
      [this](int id, std::string_view data) { OnTcpData(id, data); },
      [this](int id) { OnTcpClose(id); });

  // Registering the route is the call that delivers those events, and the
  // delivery is the user's own callback - which may have reconnected from there.
  // A nested Connect has taken the object over by now, so its handle is the one
  // that must survive: assigning this call's handle would overwrite the route the
  // live connection is using with the dead one it replaced, and neither
  // Disconnect() nor the destructor could then withdraw the route the callbacks
  // still point through.
  if (generation_ == my_generation) {
    subscription_ = handle;
  }

  return {};
}

void HalTcpClient::Disconnect() {
  if (!connected_) return;

  // Clear the id before closing: the module answers with a close URC, and the
  // handlers below must not treat our own teardown as a peer-initiated event.
  int id = connect_id_;
  connect_id_ = -1;
  connected_ = false;

  // Withdrawn before the close so the URC the close provokes arrives with nobody
  // listening, rather than being delivered as a peer close. By handle rather
  // than by cid, because the pool may already have handed this cid to another
  // client - and a withdrawal by cid would take that client's route with it.
  hal_.Unsubscribe(subscription_);
  subscription_ = 0;

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

// The HAL routes by cid, so these only ever see this client's own connection.
void HalTcpClient::OnTcpData(int /*connect_id*/, std::string_view data) {
  if (!on_data_) return;
  on_data_(data);
}

void HalTcpClient::OnTcpClose(int /*connect_id*/) {
  connect_id_ = -1;
  connected_ = false;
  if (on_disconnected_) {
    on_disconnected_();
  }
}

}  // namespace esp_modem_link::hal
