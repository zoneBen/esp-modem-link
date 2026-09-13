#include "platform/ievent_group.h"

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace esp_modem_link::platform {

namespace {

// The interface describes FreeRTOS's event group, so this is a mapping rather
// than an implementation - with two details the interface fixes and the
// FreeRTOS call does not.
//
// The return value is masked to the bits that were asked for: FreeRTOS returns
// the whole group as it stood before any bit was cleared, which can carry bits
// the caller never mentioned. And the two sentinels the POSIX implementation
// accepts for "no timeout" are honoured here too, since a caller that passes one
// means it.
//
// A group that could not be allocated is inert rather than fatal: every method
// becomes a no-op and a wait reports nothing set. CreateEventGroup has no way to
// report failure through the interface, and a caller that cannot create one has
// nowhere useful to go.
class IdfEventGroup : public IEventGroup {
 public:
  IdfEventGroup() : handle_(xEventGroupCreate()) {}
  ~IdfEventGroup() override {
    if (handle_ != nullptr) vEventGroupDelete(handle_);
  }

  void SetBits(uint32_t bits) override {
    if (handle_ != nullptr) xEventGroupSetBits(handle_, bits);
  }

  void ClearBits(uint32_t bits) override {
    if (handle_ != nullptr) xEventGroupClearBits(handle_, bits);
  }

  uint32_t WaitBits(uint32_t bits,
                    bool clear_on_exit,
                    bool wait_all,
                    std::chrono::milliseconds timeout) override {
    if (handle_ == nullptr) return 0;

    TickType_t ticks = 0;  // a zero-tick wait is the non-blocking check
    if (timeout == std::chrono::milliseconds::max() || timeout.count() == -1) {
      ticks = portMAX_DELAY;
    } else if (timeout > std::chrono::milliseconds::zero()) {
      const int64_t ms = timeout.count();
      ticks = static_cast<TickType_t>(std::max<int64_t>(
          1, std::min<int64_t>(ms * configTICK_RATE_HZ / 1000,
                               portMAX_DELAY - 1)));
    }

    const EventBits_t result = xEventGroupWaitBits(
        handle_, bits, clear_on_exit ? pdTRUE : pdFALSE,
        wait_all ? pdTRUE : pdFALSE, ticks);
    // Zero on timeout - which is also how the interface spells "nothing was
    // set" - and otherwise the requested bits.
    return static_cast<uint32_t>(result) & bits;
  }

  uint32_t GetBits() override {
    return handle_ == nullptr ? 0 : xEventGroupGetBits(handle_);
  }

 private:
  EventGroupHandle_t handle_;
};

}  // namespace

std::unique_ptr<IEventGroup> CreateEventGroup() {
  return std::make_unique<IdfEventGroup>();
}

}  // namespace esp_modem_link::platform
