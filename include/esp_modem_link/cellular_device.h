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

  // Which module is on the other end of a channel that is already open.
  using IdentifyFn = std::function<Result<std::unique_ptr<hal::IModuleHal>>(
      at_channel::IAtChannel& channel)>;

  // Body shared by all four entry points: settle the wire rate, identify the
  // module, settle the rate again, assemble. Written once rather than four
  // times because "every entry point aligns" has to be a structural fact - the
  // rate negotiation used to live inside the HAL, where three of the four
  // entry points never reached it.
  static Result<std::unique_ptr<CellularDevice>> BringUp(
      std::unique_ptr<Impl> impl,
      const IdentifyFn& identify);

  // Stage one of the rate negotiation: find a rate the module answers at,
  // before anything has tried to identify it. Returns the rate the caller
  // opened the port at, which is what stage two asks the module for.
  //
  // Finding nothing is not a failure to start - the firmware may not implement
  // AT+IPR? at all, and the caller may well have asked for the right rate - so
  // this does not fail, and leaves the channel where the caller put it. The
  // identification that follows is what decides whether the device is usable.
  static int AlignBeforeIdentify(at_channel::IAtChannel& channel);

  // Stage two: ask the module to come to `requested`, now that identification
  // has proved what is on the other end.
  //
  // AT+IPR=<rate> is persistent - a module keeps it across a power cycle - so on
  // the Detect entry points it is not written until AT+CGMR has identified the
  // module as one this library knows. A failed identification therefore never
  // reconfigures anything.
  //
  // The Create entry points do not identify, and cannot: the caller naming a
  // ModuleType IS the identification, and CreateModule builds a HAL from it
  // without sending a byte. So a Create against a device that is not the type
  // named will have its rate written, on the caller's assertion. That is the
  // meaning of the API - it is why the type is a required argument rather than
  // guessed - but it means the split above is a property of Detect, not of this
  // function. A caller who is not sure what is on the port wants Detect.
  //
  // A failure here is propagated: the one alignment error worth reporting is a
  // module that moved to a new rate and a channel that could not follow - or a
  // module that neither took the new rate nor went back to the old one - which
  // leaves nothing able to talk and would otherwise surface as an unexplained
  // timeout in the first command of initialization.
  static Result<> AlignAfterIdentify(at_channel::IAtChannel& channel,
                                     int requested);
};

}  // namespace esp_modem_link
