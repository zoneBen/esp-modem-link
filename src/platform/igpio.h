#pragma once

#include <functional>
#include <memory>

namespace esp_modem_link::platform {

enum class GpioLevel { kLow, kHigh };

enum class GpioEdge { kRising, kFalling, kBoth };

using GpioInterruptHandler = std::function<void(GpioLevel)>;

class IGpio {
 public:
  virtual ~IGpio() = default;
  virtual void SetLevel(GpioLevel level) = 0;
  virtual GpioLevel GetLevel() const = 0;
  virtual void SetDirection(bool output) = 0;
  virtual void EnableInterrupt(GpioEdge edge,
                               GpioInterruptHandler handler) = 0;
  virtual void DisableInterrupt() = 0;
};

std::unique_ptr<IGpio> CreateGpio(int pin);

}  // namespace esp_modem_link::platform
