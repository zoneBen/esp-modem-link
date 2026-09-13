#include "platform/time.h"

#include <algorithm>
#include <chrono>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esp_modem_link::platform {

std::chrono::milliseconds Now() {
  // Microseconds from the hardware timer, which is monotonic in the way POSIX's
  // steady_clock is - and that is what the callers assume. Every one of them
  // uses the value as a delta (a keep-alive's last send, a ping's send time, a
  // deadline), and none compares it against a calendar clock.
  return std::chrono::milliseconds(esp_timer_get_time() / 1000);
}

void Sleep(std::chrono::milliseconds duration) {
  if (duration <= std::chrono::milliseconds::zero()) return;

  // vTaskDelay counts ticks, and a request shorter than one tick rounds down to
  // zero - which yields the CPU and returns immediately rather than sleeping, so
  // a caller polling on a short interval would spin instead of waiting. Round up
  // to one tick, and cap at the longest delay the scheduler can express.
  const int64_t ms = duration.count();
  const int64_t ticks = ms * configTICK_RATE_HZ / 1000;
  vTaskDelay(static_cast<TickType_t>(
      std::max<int64_t>(1, std::min<int64_t>(ticks, portMAX_DELAY - 1))));
}

}  // namespace esp_modem_link::platform
