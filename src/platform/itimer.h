#pragma once

#include <chrono>
#include <functional>
#include <memory>

namespace esp_modem_link::platform {

class ITimer {
 public:
  virtual ~ITimer() = default;
  virtual void Start(std::chrono::milliseconds period,
                     bool periodic = false) = 0;
  virtual void Stop() = 0;
  virtual bool IsRunning() const = 0;
};

using TimerCallback = std::function<void()>;

std::unique_ptr<ITimer> CreateTimer(TimerCallback callback);

}  // namespace esp_modem_link::platform
