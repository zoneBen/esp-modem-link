#pragma once

#include <memory>

#include "platform/iuart.h"

namespace esp_modem_link::platform {

// Opens the real serial port named by config.device (e.g. "COM8" or
// "/dev/ttyUSB0"). Returns nullptr when the port cannot be opened, so a
// mistyped device name surfaces as an error instead of silently falling back
// to the in-memory mock.
std::unique_ptr<IUart> CreateSerialUart(const UartConfig& config);

}  // namespace esp_modem_link::platform
