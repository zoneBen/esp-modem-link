#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

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
  virtual Result<int> TcpConnect(std::string_view host,
                                 uint16_t port,
                                 bool ssl = false) = 0;
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

  // TCP/UDP callbacks
  virtual void SetTcpDataCallback(TcpDataCallback cb) {
    on_tcp_data_ = std::move(cb);
  }
  virtual void SetTcpCloseCallback(TcpCloseCallback cb) {
    on_tcp_close_ = std::move(cb);
  }
  virtual void SetUdpDataCallback(UdpDataCallback cb) {
    on_udp_data_ = std::move(cb);
  }

  // Network state callbacks
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
  TcpDataCallback on_tcp_data_;
  TcpCloseCallback on_tcp_close_;
  UdpDataCallback on_udp_data_;
  NetworkStateCallback on_network_state_;
  SignalInfoCallback on_signal_info_;
};

}  // namespace esp_modem_link::hal
