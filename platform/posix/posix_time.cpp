#include "platform/time.h"

#include <chrono>
#include <thread>

namespace esp_modem_link::platform {

std::chrono::milliseconds Now() {
  auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::milliseconds>(now);
}

void Sleep(std::chrono::milliseconds duration) {
  std::this_thread::sleep_for(duration);
}

}  // namespace esp_modem_link::platform
