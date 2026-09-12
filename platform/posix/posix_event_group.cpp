#include "platform/ievent_group.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace esp_modem_link::platform {

namespace {

class PosixEventGroup : public IEventGroup {
 public:
  void SetBits(uint32_t bits) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      bits_ |= bits;
    }
    cv_.notify_all();
  }

  void ClearBits(uint32_t bits) override {
    std::lock_guard<std::mutex> lock(mutex_);
    bits_ &= ~bits;
  }

  uint32_t WaitBits(uint32_t bits,
                    bool clear_on_exit,
                    bool wait_all,
                    std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    auto predicate = [&]() {
      if (wait_all) {
        return (bits_ & bits) == bits;
      } else {
        return (bits_ & bits) != 0;
      }
    };

    bool result = true;
    if (timeout == std::chrono::milliseconds::zero()) {
      // Non-blocking check
      result = predicate();
    } else if (timeout == std::chrono::milliseconds::max() ||
               timeout.count() == -1) {
      cv_.wait(lock, predicate);
      result = true;
    } else {
      result = cv_.wait_for(lock, timeout, predicate);
    }

    uint32_t result_bits = bits_;
    if (result && clear_on_exit) {
      bits_ &= ~bits;
    }
    return result ? (result_bits & bits) : 0;
  }

  uint32_t GetBits() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return bits_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  uint32_t bits_ = 0;
};

}  // namespace

std::unique_ptr<IEventGroup> CreateEventGroup() {
  return std::make_unique<PosixEventGroup>();
}

}  // namespace esp_modem_link::platform
