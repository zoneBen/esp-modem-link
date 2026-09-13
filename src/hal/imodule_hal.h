#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "at_channel/iat_channel.h"
#include "esp_modem_link/common_types.h"
#include "esp_modem_link/config_types.h"
#include "esp_modem_link/http_client.h"
#include "esp_modem_link/mqtt_client.h"
#include "esp_modem_link/module_capabilities.h"
#include "esp_modem_link/network_error.h"

namespace esp_modem_link::hal {

// Callback types for async events from HAL
using TcpDataCallback =
    std::function<void(int connect_id, std::string_view data)>;
using TcpCloseCallback = std::function<void(int connect_id)>;
using UdpDataCallback =
    std::function<void(int connect_id, std::string_view host,
                       uint16_t port, std::string_view data)>;
using NetworkStateCallback =
    std::function<void(RegistrationState state)>;
using SignalInfoCallback = std::function<void(const SignalInfo& info)>;

// Names one registration, so that it can be withdrawn without naming the cid it
// is attached to. See SubscribeTcp for why that distinction is load-bearing.
using HalSubscription = uint64_t;

class IModuleHal {
 public:
  virtual ~IModuleHal() = default;

  // Module identification
  virtual ModuleType GetModuleType() const = 0;
  virtual std::string_view GetModuleName() const = 0;
  virtual const ModuleCapabilities& GetCapabilities() const = 0;

  // Lifecycle
  virtual Result<> Initialize(at_channel::IAtChannel& channel) = 0;
  virtual Result<> Reset() = 0;

  // SIM & Network
  virtual SimState GetSimState() = 0;
  virtual Result<> ConfigureApn(const ApnConfig& config) = 0;
  virtual RegistrationState GetRegistrationState() = 0;
  virtual SignalInfo GetSignalInfo() = 0;

  // Device info
  virtual Result<std::string> GetImei() = 0;
  virtual Result<std::string> GetIccid() = 0;
  virtual Result<std::string> GetRevision() = 0;
  virtual Result<std::string> GetCarrier() = 0;

  // Low power (optional - default: not supported)
  virtual bool SupportsLowPower() const { return false; }
  virtual Result<> EnterSleep(const SleepConfig& config) {
    (void)config;
    return std::unexpected(
        NetworkError::NotSupported("low power mode not supported"));
  }
  virtual Result<> ExitSleep() {
    return std::unexpected(
        NetworkError::NotSupported("low power mode not supported"));
  }
  virtual void SetDtrWakeup(bool enable) { (void)enable; }

  // Flight mode
  virtual Result<> SetFlightMode(bool enable) = 0;

  // TCP client (required for all modules with data capability)
  //
  // `config` describes the handshake when `ssl` is set and is ignored otherwise,
  // except that a module must refuse certificate material handed to a plaintext
  // socket rather than let the connection come up silently unauthenticated. Not
  // every field maps onto every module: an honest HAL reports the ones its AT
  // commands cannot express instead of dropping them, so a caller that asked for
  // verification finds out before the socket is open.
  virtual Result<int> TcpConnect(std::string_view host,
                                 uint16_t port,
                                 bool ssl = false,
                                 const TlsConfig& config = {}) = 0;
  virtual Result<> TcpClose(int connect_id) = 0;
  virtual Result<int> TcpSend(int connect_id,
                              const void* data,
                              size_t len) = 0;

  // UDP client (required)
  virtual Result<int> UdpOpen(std::string_view host, uint16_t port) = 0;
  virtual Result<> UdpClose(int connect_id) = 0;
  virtual Result<int> UdpSend(int connect_id,
                              const void* data,
                              size_t len) = 0;

  // Routes one connection's events to the client that opened it, and returns the
  // handle that names this registration. Zero means nothing was registered.
  //
  // Socket events are keyed by cid, because a cid is the only thing the module
  // names in them - there is no per-connection handle of its own. The
  // subscription is *named* by a handle rather than by that cid, because a
  // released cid goes back into the pool and can be reallocated while the client
  // that held it is still tearing down: withdrawing by cid would then clear the
  // route belonging to the connection that replaced it, which would leave a live
  // socket with nothing listening to it.
  //
  // A cid has one owner. Registering a second time for the same cid leaves the
  // first registration in place but no longer delivered to, so a HAL that
  // reopens on a cid must see its client withdraw first.
  HalSubscription SubscribeTcp(int connect_id,
                               TcpDataCallback on_data,
                               TcpCloseCallback on_close) {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    const HalSubscription handle = next_subscription_++;
    tcp_routes_[handle] =
        TcpRoute{connect_id, std::move(on_data), std::move(on_close)};
    return handle;
  }

  HalSubscription SubscribeUdp(int connect_id, UdpDataCallback on_data) {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    const HalSubscription handle = next_subscription_++;
    udp_routes_[handle] = UdpRoute{connect_id, std::move(on_data)};
    return handle;
  }

  void Unsubscribe(HalSubscription handle) {
    if (handle == 0) return;
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    tcp_routes_.erase(handle);
    udp_routes_.erase(handle);
  }

  // Network state callbacks. One subscriber is enough for these: they describe
  // the module rather than any one connection, and the device layer is the only
  // thing that wants them.
  virtual void SetNetworkStateCallback(NetworkStateCallback cb) {
    on_network_state_ = std::move(cb);
  }
  virtual void SetSignalInfoCallback(SignalInfoCallback cb) {
    on_signal_info_ = std::move(cb);
  }

  // Built-in HTTP (optional)
  virtual bool HasBuiltinHttp() const { return false; }
  virtual Result<std::unique_ptr<HttpClient>> CreateBuiltinHttp() {
    return std::unexpected(
        NetworkError::NotSupported("builtin HTTP not supported"));
  }

  // Built-in MQTT (optional)
  virtual bool HasBuiltinMqtt() const { return false; }
  virtual Result<std::unique_ptr<MqttClient>> CreateBuiltinMqtt() {
    return std::unexpected(
        NetworkError::NotSupported("builtin MQTT not supported"));
  }

 protected:
  // What a HAL calls from its URC handlers. Kept here rather than in each
  // module so routing is one implementation, not one per module: a new module
  // gets multiplexing by calling these.
  //
  // Handlers are copied out under the lock and invoked after it is released, so
  // a handler that subscribes, withdraws or opens a socket cannot deadlock
  // against a dispatch in progress.
  void DispatchTcpData(int connect_id, std::string_view data) {
    TcpDataCallback handler;
    {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      if (const TcpRoute* route = FindTcpRouteLocked(connect_id)) {
        handler = route->on_data;
      }
    }
    if (handler) handler(connect_id, data);
  }

  // Ending a connection ends its route: the event is delivered to whoever owned
  // the cid and then removed, so a route cannot outlive the socket it describes.
  void DispatchTcpClose(int connect_id) {
    std::vector<TcpCloseCallback> handlers;
    {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      for (auto it = tcp_routes_.begin(); it != tcp_routes_.end();) {
        if (it->second.connect_id == connect_id) {
          if (it->second.on_close) handlers.push_back(it->second.on_close);
          it = tcp_routes_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& handler : handlers) handler(connect_id);
  }

  void DispatchUdpData(int connect_id,
                       std::string_view host,
                       uint16_t port,
                       std::string_view data) {
    UdpDataCallback handler;
    {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      for (const auto& [handle, route] : udp_routes_) {
        if (route.connect_id == connect_id) {
          handler = route.on_data;
          break;
        }
      }
    }
    if (handler) handler(connect_id, host, port, data);
  }

  NetworkStateCallback on_network_state_;
  SignalInfoCallback on_signal_info_;

 private:
  struct TcpRoute {
    int connect_id = -1;
    TcpDataCallback on_data;
    TcpCloseCallback on_close;
  };
  struct UdpRoute {
    int connect_id = -1;
    UdpDataCallback on_data;
  };

  // Caller holds subscription_mutex_.
  const TcpRoute* FindTcpRouteLocked(int connect_id) const {
    for (const auto& [handle, route] : tcp_routes_) {
      if (route.connect_id == connect_id) return &route;
    }
    return nullptr;
  }

  mutable std::mutex subscription_mutex_;
  // Starts at 1 so that zero can mean "nothing registered" everywhere a handle
  // is stored.
  HalSubscription next_subscription_ = 1;
  std::unordered_map<HalSubscription, TcpRoute> tcp_routes_;
  std::unordered_map<HalSubscription, UdpRoute> udp_routes_;
};

}  // namespace esp_modem_link::hal
