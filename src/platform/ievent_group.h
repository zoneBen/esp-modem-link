#pragma once

#include <chrono>
#include <cstdint>
#include <memory>

namespace esp_modem_link::platform {

class IEventGroup {
 public:
  virtual ~IEventGroup() = default;
  virtual void SetBits(uint32_t bits) = 0;
  virtual void ClearBits(uint32_t bits) = 0;
  virtual uint32_t WaitBits(uint32_t bits,
                            bool clear_on_exit,
                            bool wait_all,
                            std::chrono::milliseconds timeout) = 0;
  virtual uint32_t GetBits() = 0;
};

std::unique_ptr<IEventGroup> CreateEventGroup();

}  // namespace esp_modem_link::platform
