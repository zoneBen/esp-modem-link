#pragma once

#include <cstdint>

namespace esp_modem_link::platform {

// An unpredictable 32-bit value.
//
// A platform with a hardware generator should back this with it: the one caller
// today is the WebSocket masking key, whose whole purpose is to keep the bytes
// on the wire from being predictable to anything sitting between the device and
// the server. A seed derived from the clock would compile everywhere and defeat
// that, so the implementation is left to each platform rather than shared.
uint32_t RandomUint32();

}  // namespace esp_modem_link::platform
