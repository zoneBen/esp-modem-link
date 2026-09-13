#pragma once

#include <cstdint>
#include <string_view>

#include "esp_modem_link/config_types.h"
#include "esp_modem_link/tcp_client.h"
#include "hal/imodule_hal.h"

namespace esp_modem_link::hal {

class HalTcpClient : public TcpClient {
 public:
  // The config is a constructor argument rather than a Connect parameter
  // because the engines above hold a client across connects and set TLS once,
  // but it only takes effect on the next Connect: the handshake is performed by
  // TcpConnect, and nothing here can re-key an open socket.
  explicit HalTcpClient(IModuleHal& hal,
                        bool ssl = false,
                        const TlsConfig& config = {});
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
  TlsConfig tls_config_;
  HalSubscription subscription_ = 0;
  int connect_id_ = -1;
  // Bumped by every Connect. Naming a connect rather than reading the fields it
  // set, because Connect hands control to the user's callbacks while it is still
  // on the stack: a callback that reconnects is a nested Connect, and this is what
  // tells the outer one that it no longer speaks for the object - the cid alone
  // cannot, since a pool is free to hand the same one back.
  uint32_t generation_ = 0;
};

}  // namespace esp_modem_link::hal
