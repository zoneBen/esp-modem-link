#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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
  //
  // Anything the module sent for this cid before the call is delivered to the new
  // route before it returns - see the undelivered-events note below. That delivery
  // is the client's own callback, and it may close this socket, so a HAL must call
  // this from a thread that is allowed to send a command.
  HalSubscription SubscribeTcp(int connect_id,
                               TcpDataCallback on_data,
                               TcpCloseCallback on_close) {
    HalSubscription handle = 0;
    Undelivered held;
    {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      handle = next_subscription_++;
      tcp_routes_[handle] =
          TcpRoute{connect_id, std::move(on_data), std::move(on_close)};

      if (auto it = undelivered_.find(connect_id); it != undelivered_.end()) {
        held = std::move(it->second);
        undelivered_.erase(it);
      }
    }

    // Delivered outside the lock, because the callback is the client's code and
    // it may well call back in. It is handed over as data-then-close, the order
    // the module produced it in, so that a client counts a response the same way
    // whether it was listening when it arrived or not.
    if (!held.data.empty()) {
      DispatchTcpData(connect_id, held.data);
    }
    if (held.closed) {
      // The module ended this socket and the HAL has already given the cid back,
      // so the route ends here and nothing is owed.
      DispatchTcpClose(connect_id);
    } else if (held.truncated) {
      // Nothing ended this socket: the hold hit its cap and gave up on the rest,
      // and the module still has the socket open. Reporting the connection over
      // without ending it strands the socket - a caller told it is disconnected
      // has no reason to close it, so the cid never goes back to the pool - while
      // handing over a prefix that looks like a whole response is the lie the
      // close exists to prevent. So the socket is ended here, and the caller is
      // told once it really is over.
      //
      // This is the only thing here that sends a command, which is why a HAL must
      // not call SubscribeTcp from a URC handler: that command's reply could only
      // be delivered by the very task now blocked waiting for it. The result is
      // not actionable - if the close did not take, the socket's fate is unknown,
      // and the caller still has to be told that the body it was promised cannot
      // be delivered.
      (void)TcpClose(connect_id);
      DispatchTcpClose(connect_id);
    }
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
  //
  // An event with no route is held rather than dropped, because the route appears
  // only after the client's TcpConnect has returned: a peer that sends or closes
  // the moment it accepts would otherwise have its first events delivered to
  // nobody, and the client would sit on a socket that is already finished. The
  // hold is handed over by SubscribeTcp and voided by DiscardUndelivered.
  void DispatchTcpData(int connect_id, std::string_view data) {
    TcpDataCallback handler;
    {
      std::lock_guard<std::mutex> lock(subscription_mutex_);
      if (const TcpRoute* route = FindTcpRouteLocked(connect_id)) {
        handler = route->on_data;
      } else {
        HoldLocked(connect_id, data);
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
      bool routed = false;
      for (auto it = tcp_routes_.begin(); it != tcp_routes_.end();) {
        if (it->second.connect_id == connect_id) {
          if (it->second.on_close) handlers.push_back(it->second.on_close);
          it = tcp_routes_.erase(it);
          routed = true;
        } else {
          ++it;
        }
      }
      if (routed) {
        // Delivered to a live route, so nothing is waiting for it and any hold
        // for this cid is now older than the socket that is ending.
        undelivered_.erase(connect_id);
      } else {
        undelivered_[connect_id].closed = true;
      }
    }
    for (auto& handler : handlers) handler(connect_id);
  }

  // Voids whatever is held for a cid. A HAL calls this every time a cid changes
  // hands - both when it gives one back and when it takes one - because a cid
  // cannot tell its generations apart: a hold that outlived its socket would be
  // handed to the next client on that cid as if the previous socket's bytes were
  // its own. The two call sites are not redundant. Releasing covers the socket
  // that ended; taking covers the event the module volunteers for a cid nothing
  // owns, which no release precedes.
  //
  // A socket that dies inside the window loses the payload held for it here, and
  // its client gets the close instead - which is the lesser of the two answers,
  // since a hold cannot be kept without also being kept out of the next socket's
  // hands, and a client that is told its connection ended can retry where one
  // shown someone else's bytes cannot.
  void DiscardUndelivered(int connect_id) {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    undelivered_.erase(connect_id);
  }

  // UDP has the same window between opening a socket and subscribing to it, and
  // this drops anything that lands in it. Deliberate: a datagram is a message on
  // its own, so one that arrives before its reader exists is a lost datagram -
  // which is what UDP already means - where a TCP payload arriving in that window
  // is the head of a stream the caller is about to read as a whole response. The
  // hold above exists for that difference, not for the window as such.
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

  // Payload and outcome that arrived for a cid nobody had a route for yet.
  struct Undelivered {
    std::string data;
    bool truncated = false;
    bool closed = false;
  };

  // What one cid's hold may carry before the tail is dropped. Small, because it
  // exists to cover the instant between a socket opening and its client
  // subscribing, and a client that is that far behind is better served by an
  // ended stream than by a truncated body passing for a whole one.
  static constexpr size_t kMaxUndeliveredBytes = 4 * 1024;

  // Caller holds subscription_mutex_.
  void HoldLocked(int connect_id, std::string_view data) {
    Undelivered& held = undelivered_[connect_id];
    const size_t room = held.data.size() < kMaxUndeliveredBytes
                            ? kMaxUndeliveredBytes - held.data.size()
                            : 0;
    const size_t take = std::min(room, data.size());
    held.data.append(data.data(), take);
    // The tail is dropped rather than the head, because TCP is a stream: losing
    // the oldest bytes would leave a hole that is delivered as if the peer had
    // sent it, where a short tail is visible as a stream that ended early.
    if (take < data.size()) held.truncated = true;
  }

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
  std::unordered_map<int, Undelivered> undelivered_;
};

}  // namespace esp_modem_link::hal
