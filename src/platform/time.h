#pragma once

#include <chrono>

namespace esp_modem_link::platform {

std::chrono::milliseconds Now();

void Sleep(std::chrono::milliseconds duration);

}  // namespace esp_modem_link::platform
