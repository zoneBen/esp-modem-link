#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string_view>

#include "esp_modem_link/common_types.h"
#include "esp_modem_link/config_types.h"
#include "esp_modem_link/module_capabilities.h"
#include "esp_modem_link/network_error.h"

namespace esp_modem_link {

class NetworkInterface;
struct ApnConfig;
struct SleepConfig;

namespace at_channel {
class IAtChannel;
}  // namespace at_channel

namespace hal {
class IModuleHal;
}  // namespace hal

namespace platform {
struct UartConfig;
}  // namespace platform

class CellularDevice {
 public:
  ~CellularDevice();

  CellularDevice(const CellularDevice&) = delete;
  CellularDevice& operator=(const CellularDevice&) = delete;

  // Auto-detect module and create device
  static Result<std::unique_ptr<CellularDevice>> Detect(
      const platform::UartConfig& uart_config);

  // Create device with specific module type
  static Result<std::unique_ptr<CellularDevice>> Create(
      ModuleType type,
      const platform::UartConfig& uart_config);

  // Same two operations, but over a caller-supplied AT channel instead of one
  // built from a UART. Every other layer in this library takes its transport by
  // reference; this is that same seam for the top-level device, for callers who
  // already own an AT channel (sharing a port, or fronting a non-serial
  // transport) and for tests that need a scripted one.
  static Result<std::unique_ptr<CellularDevice>> Detect(
      std::unique_ptr<at_channel::IAtChannel> channel);
  static Result<std::unique_ptr<CellularDevice>> Create(
      ModuleType type,
      std::unique_ptr<at_channel::IAtChannel> channel);

  // Network management
  Result<RegistrationState> WaitForNetwork(
      std::chrono::milliseconds timeout);
  void OnNetworkState(std::function<void(RegistrationState)> callback);

  // Brings up the PDP context. Call before opening any socket; without it the
  // module has no data path even when registration succeeds.
  Result<> ConfigureApn(const ApnConfig& config);

  // Device control
  Result<> Reboot();
  Result<> SetFlightMode(bool enable);
  Result<> SetSleepMode(bool enable, const SleepConfig& config);

  // Device info
  Result<std::string> GetImei();
  Result<std::string> GetIccid();
  Result<std::string> GetModuleRevision();
  Result<std::string> GetCarrierName();
  Result<int> GetSignalStrength();
  Result<RegistrationState> GetRegistrationState();
  Result<SimState> GetSimState();

  // Capabilities
  const ModuleCapabilities& GetCapabilities() const;
  ModuleType GetModuleType() const;

  // Network interface
  NetworkInterface& GetNetwork();

  // Direct AT channel access (advanced)
  at_channel::IAtChannel& GetAtChannel();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit CellularDevice(std::unique_ptr<Impl> impl);

  // Opens the platform transport and wraps it in an AtUart, the way every
  // UART-based factory starts.
  static Result<std::unique_ptr<at_channel::IAtChannel>> OpenChannel(
      Impl& impl,
      const platform::UartConfig& uart_config);

  // Shared tail of every factory: bring the module up, attach the network
  // layer, and hand back a constructed device. Keeping it in one place is what
  // makes the four entry points above agree on what "created" means.
  static Result<std::unique_ptr<CellularDevice>> Assemble(
      std::unique_ptr<Impl> impl,
      std::unique_ptr<hal::IModuleHal> hal);
};

}  // namespace esp_modem_link
