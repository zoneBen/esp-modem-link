#include "esp_modem_link/cellular_device.h"

#include <chrono>
#include <string>
#include <utility>

#include "at_channel/at_uart.h"
#include "at_channel/baud_alignment.h"
#include "at_channel/iat_channel.h"
#include "device/cellular_device_impl.h"
#include "esp_modem_link/config_types.h"
#include "esp_modem_link/network_interface.h"
#include "hal/module_registry.h"
#include "modules/module_registrations.h"
#include "platform/iuart.h"
#include "platform/time.h"

namespace esp_modem_link {

CellularDevice::CellularDevice(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

CellularDevice::~CellularDevice() = default;

Result<std::unique_ptr<at_channel::IAtChannel>> CellularDevice::OpenChannel(
    Impl& impl,
    const platform::UartConfig& uart_config) {
  impl.uart = platform::CreateUart(uart_config);
  if (!impl.uart) {
    return std::unexpected(
        NetworkError(NetworkErrc::kInvalidArgument, 0,
                     "failed to open UART " + uart_config.device));
  }
  // AtUart borrows the UART, so the UART has to outlive it; Impl declares them
  // in that order for exactly this reason.
  return std::unique_ptr<at_channel::IAtChannel>(
      std::make_unique<at_channel::AtUart>(*impl.uart, uart_config));
}

Result<std::unique_ptr<CellularDevice>> CellularDevice::Assemble(
    std::unique_ptr<Impl> impl,
    std::unique_ptr<hal::IModuleHal> hal) {
  impl->hal = std::move(hal);

  auto init_result = impl->hal->Initialize(*impl->at_uart);
  if (!init_result) {
    return std::unexpected(init_result.error());
  }

  impl->network = std::make_unique<network::CellularNetwork>(*impl->hal);

  return std::unique_ptr<CellularDevice>(
      new CellularDevice(std::move(impl)));
}

Result<std::unique_ptr<CellularDevice>> CellularDevice::Detect(
    const platform::UartConfig& uart_config) {
  EnsureModulesRegistered();

  auto impl = std::make_unique<Impl>();
  auto channel = OpenChannel(*impl, uart_config);
  if (!channel) {
    return std::unexpected(channel.error());
  }
  impl->at_uart = std::move(*channel);

  return BringUp(std::move(impl), [](at_channel::IAtChannel& channel) {
    return hal::ModuleRegistry::Instance().DetectModule(channel);
  });
}

Result<std::unique_ptr<CellularDevice>> CellularDevice::Create(
    ModuleType type,
    const platform::UartConfig& uart_config) {
  EnsureModulesRegistered();

  auto impl = std::make_unique<Impl>();
  auto channel = OpenChannel(*impl, uart_config);
  if (!channel) {
    return std::unexpected(channel.error());
  }
  impl->at_uart = std::move(*channel);

  return BringUp(std::move(impl), [type](at_channel::IAtChannel&) {
    return hal::ModuleRegistry::Instance().CreateModule(type);
  });
}

Result<std::unique_ptr<CellularDevice>> CellularDevice::Detect(
    std::unique_ptr<at_channel::IAtChannel> channel) {
  EnsureModulesRegistered();

  if (!channel) {
    return std::unexpected(
        NetworkError(NetworkErrc::kInvalidArgument, 0,
                     "AT channel must not be null"));
  }

  auto impl = std::make_unique<Impl>();
  impl->at_uart = std::move(channel);

  return BringUp(std::move(impl), [](at_channel::IAtChannel& channel) {
    return hal::ModuleRegistry::Instance().DetectModule(channel);
  });
}

Result<std::unique_ptr<CellularDevice>> CellularDevice::Create(
    ModuleType type,
    std::unique_ptr<at_channel::IAtChannel> channel) {
  EnsureModulesRegistered();

  if (!channel) {
    return std::unexpected(
        NetworkError(NetworkErrc::kInvalidArgument, 0,
                     "AT channel must not be null"));
  }

  auto impl = std::make_unique<Impl>();
  impl->at_uart = std::move(channel);

  return BringUp(std::move(impl), [type](at_channel::IAtChannel&) {
    return hal::ModuleRegistry::Instance().CreateModule(type);
  });
}

int CellularDevice::AlignBeforeIdentify(at_channel::IAtChannel& channel) {
  // Read before the scan, which moves the channel: this is the rate the caller
  // asked for, and it is what stage two restores the link to.
  const int requested = channel.GetBaudRate();

  // The scan's error is deliberately dropped. It means "no candidate answered",
  // which is exactly the state a module with no AT+IPR? support is left in, and
  // it has already put the channel back where the caller set it. What the
  // identification does next is the real answer to whether this is a device.
  if (requested > 0) {
    (void)at_channel::FindModuleBaudRate(
        channel, at_channel::BaudRateCandidates(requested));
  }
  return requested;
}

Result<> CellularDevice::AlignAfterIdentify(at_channel::IAtChannel& channel,
                                            int requested) {
  // A channel that does not name a rate cannot be moved to one and cannot be
  // told apart from a stub, so it is left alone rather than written to.
  if (requested <= 0) return {};

  auto aligned = at_channel::SetModuleBaudRate(channel, requested);
  if (!aligned) return std::unexpected(aligned.error().ToNetworkError());
  return {};
}

Result<std::unique_ptr<CellularDevice>> CellularDevice::BringUp(
    std::unique_ptr<Impl> impl,
    const IdentifyFn& identify) {
  const int requested = AlignBeforeIdentify(*impl->at_uart);

  auto hal_result = identify(*impl->at_uart);
  if (!hal_result) {
    return std::unexpected(hal_result.error());
  }

  if (auto aligned = AlignAfterIdentify(*impl->at_uart, requested); !aligned) {
    return std::unexpected(aligned.error());
  }

  return Assemble(std::move(impl), std::move(*hal_result));
}

Result<RegistrationState> CellularDevice::WaitForNetwork(
    std::chrono::milliseconds timeout) {
  auto start = std::chrono::steady_clock::now();
  auto poll_interval = std::chrono::milliseconds(1000);

  while (true) {
    auto state = impl_->hal->GetRegistrationState();
    if (state == RegistrationState::kRegisteredHome ||
        state == RegistrationState::kRegisteredRoaming) {
      return state;
    }

    auto elapsed = std::chrono::steady_clock::now() - start;
    if (elapsed >= timeout) {
      return std::unexpected(
          NetworkError(NetworkErrc::kTimeout, 0,
                       "timeout waiting for network registration"));
    }

    platform::Sleep(poll_interval);
  }
}

Result<> CellularDevice::ConfigureApn(const ApnConfig& config) {
  return impl_->hal->ConfigureApn(config);
}

void CellularDevice::OnNetworkState(
    std::function<void(RegistrationState)> callback) {
  impl_->hal->SetNetworkStateCallback(std::move(callback));
}

Result<> CellularDevice::Reboot() {
  return impl_->hal->Reset();
}

Result<> CellularDevice::SetFlightMode(bool enable) {
  return impl_->hal->SetFlightMode(enable);
}

Result<> CellularDevice::SetSleepMode(bool enable,
                                       const SleepConfig& config) {
  if (enable) {
    return impl_->hal->EnterSleep(config);
  } else {
    return impl_->hal->ExitSleep();
  }
}

Result<std::string> CellularDevice::GetImei() {
  return impl_->hal->GetImei();
}

Result<std::string> CellularDevice::GetIccid() {
  return impl_->hal->GetIccid();
}

Result<std::string> CellularDevice::GetModuleRevision() {
  return impl_->hal->GetRevision();
}

Result<std::string> CellularDevice::GetCarrierName() {
  return impl_->hal->GetCarrier();
}

Result<int> CellularDevice::GetSignalStrength() {
  return impl_->hal->GetSignalInfo().rssi;
}

Result<RegistrationState> CellularDevice::GetRegistrationState() {
  return impl_->hal->GetRegistrationState();
}

Result<SimState> CellularDevice::GetSimState() {
  return impl_->hal->GetSimState();
}

const ModuleCapabilities& CellularDevice::GetCapabilities() const {
  return impl_->hal->GetCapabilities();
}

ModuleType CellularDevice::GetModuleType() const {
  return impl_->hal->GetModuleType();
}

NetworkInterface& CellularDevice::GetNetwork() {
  return *impl_->network;
}

at_channel::IAtChannel& CellularDevice::GetAtChannel() {
  return *impl_->at_uart;
}

}  // namespace esp_modem_link
