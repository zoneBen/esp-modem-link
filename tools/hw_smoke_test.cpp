// Hardware smoke test: drives a real module over a host serial port.
//
//   hw_smoke_test COM8
//   hw_smoke_test COM8 115200
//
// Prints each stage so a failure says where it stopped, not just that it did.
// Read-only against the module apart from ATE0/CFUN that Detect already sends.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "esp_modem_link/cellular_device.h"
#include "esp_modem_link/uart_config.h"

using namespace esp_modem_link;

namespace {

int g_failures = 0;

void Section(const char* name) {
  std::printf("\n--- %s ---\n", name);
}

template <typename T>
void Show(const char* label, const Result<T>& result) {
  if (result) {
    if constexpr (std::is_same_v<T, std::string>) {
      std::printf("  %-14s %s\n", label, result->c_str());
    } else if constexpr (std::is_same_v<T, int>) {
      std::printf("  %-14s %d\n", label, *result);
    } else {
      std::printf("  %-14s ok\n", label);
    }
  } else {
    std::printf("  %-14s FAILED: %s (code=%d)\n", label,
                result.error().Message(),
                static_cast<int>(result.error().code));
    ++g_failures;
  }
}

void Show(const char* label, const Result<>& result) {
  if (result) {
    std::printf("  %-14s ok\n", label);
  } else {
    std::printf("  %-14s FAILED: %s (code=%d)\n", label,
                result.error().Message(),
                static_cast<int>(result.error().code));
    ++g_failures;
  }
}

const char* SimStateName(SimState state) {
  switch (state) {
    case SimState::kReady: return "READY";
    case SimState::kPinRequired: return "PIN required";
    case SimState::kPukRequired: return "PUK required";
    case SimState::kNotInserted: return "not inserted";
    case SimState::kError: return "error";
    case SimState::kUnknown: return "unknown";
  }
  return "?";
}

const char* RegStateName(RegistrationState state) {
  switch (state) {
    case RegistrationState::kNotRegistered: return "not registered";
    case RegistrationState::kRegisteredHome: return "registered (home)";
    case RegistrationState::kSearching: return "searching";
    case RegistrationState::kRegistrationDenied: return "denied";
    case RegistrationState::kRegisteredRoaming: return "registered (roaming)";
    case RegistrationState::kUnknown: return "unknown";
  }
  return "?";
}

const char* ModuleName(ModuleType type) {
  switch (type) {
    case ModuleType::kMl307: return "ML307";
    case ModuleType::kEc801E: return "EC801E";
    case ModuleType::kAir780E: return "AIR780E";
    default: return "unknown";
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string port = argc > 1 ? argv[1] : "COM8";
  const int baud = argc > 2 ? std::atoi(argv[2]) : 115200;

  std::printf("Opening %s at %d baud\n", port.c_str(), baud);

  platform::UartConfig config;
  config.device = port;
  config.baud_rate = baud;

  Section("detect + initialize");
  auto device_result = CellularDevice::Detect(config);
  if (!device_result) {
    std::printf("  detect FAILED: %s (code=%d)\n",
                device_result.error().Message(),
                static_cast<int>(device_result.error().code));
    std::printf(
        "\nNo module answered. Check the port name, that no other program "
        "holds it open, and that the module is powered.\n");
    return 1;
  }
  auto& device = **device_result;

  const auto& caps = device.GetCapabilities();
  std::printf("  module         %s\n", ModuleName(device.GetModuleType()));
  std::printf("  max conns      %d, max baud %d\n", caps.max_connections,
              caps.max_baud_rate);
  std::printf("  caps           tcp=%d udp=%d ssl=%d http=%d mqtt=%d lp=%d\n",
              caps.tcp, caps.udp, caps.ssl_tcp, caps.http, caps.mqtt,
              caps.low_power);

  Section("identity");
  Show("revision", device.GetModuleRevision());
  Show("imei", device.GetImei());
  Show("iccid", device.GetIccid());
  Show("carrier", device.GetCarrierName());

  Section("sim + network");
  auto sim = device.GetSimState();
  if (sim) {
    std::printf("  %-14s %s\n", "sim", SimStateName(*sim));
  } else {
    std::printf("  %-14s FAILED\n", "sim");
    ++g_failures;
  }

  auto reg = device.GetRegistrationState();
  if (reg) {
    std::printf("  %-14s %s\n", "registration", RegStateName(*reg));
  } else {
    std::printf("  %-14s FAILED\n", "registration");
    ++g_failures;
  }

  auto signal = device.GetSignalStrength();
  if (signal) {
    // RSSI is an index, not dBm: dBm = -113 + 2 * rssi.
    std::printf("  %-14s %d (%d dBm)%s\n", "signal", *signal,
                -113 + 2 * (*signal), *signal == 99 ? " [no signal]" : "");
  } else {
    std::printf("  %-14s FAILED\n", "signal");
    ++g_failures;
  }

  Section("wait for registration (up to 30s)");
  auto wait = device.WaitForNetwork(std::chrono::seconds(30));
  if (wait) {
    std::printf("  %-14s %s\n", "network", RegStateName(*wait));
  } else {
    std::printf("  %-14s %s\n", "network", wait.error().Message());
    ++g_failures;
  }

  std::printf("\n%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
