#pragma once

#include "esp_modem_link/cellular_device.h"

namespace esp_modem_link {

// Ensure all built-in module HALs are linked in and their static registrars
// have run. Called internally by CellularDevice::Detect/Create.
//
// Why this exists: a static library only pulls in object files whose symbols
// are referenced. Each module HAL registers itself via a static initializer
// in its own .cpp, so without an explicit reference the linker would skip
// those objects entirely and the module registry would come up empty.
void EnsureModulesRegistered();

}  // namespace esp_modem_link
