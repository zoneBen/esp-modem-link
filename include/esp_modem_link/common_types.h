#pragma once

namespace esp_modem_link {

enum class ModuleType {
  kUnknown,
  kMl307,
  kEc801E,
  kEc600N,
  kBg95,
  kBg96,
  kL610,
  kN58,
  kA7670C,
  kEspAt,
  // Appended rather than placed beside the other 合宙 entries: inserting in the
  // middle would renumber every enumerator after it, and these values are
  // crossed by the module registry and a caller's persisted config.
  kAir780E,
};

enum class NetworkProtocol {
  kTcp,
  kSsl,
  kUdp,
  kHttp,
  kHttps,
  kMqtt,
  kMqtts,
  kWebSocket,
  kWss,
};

enum class ProtocolMode {
  kAuto,
  kBuiltin,
  kSoftware,
};

enum class RegistrationState {
  kNotRegistered,
  kRegisteredHome,
  kSearching,
  kRegistrationDenied,
  kUnknown,
  kRegisteredRoaming,
};

enum class SimState {
  kUnknown,
  kNotInserted,
  kReady,
  kPinRequired,
  kPukRequired,
  kError,
};

struct SignalInfo {
  int rssi = 0;    // 0-31, 99 = unknown
  int ber = 0;     // bit error rate, 99 = unknown
};

}  // namespace esp_modem_link
