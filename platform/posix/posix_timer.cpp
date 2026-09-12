#include "platform/itimer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace esp_modem_link::platform {

namespace {

class PosixTimer : public ITimer {
 public:
  explicit PosixTimer(TimerCallback callback)
      : callback_(std::move(callback)) {}

  ~PosixTimer() override { Stop(); }

  void Start(std::chrono::milliseconds period, bool periodic) override {
    Stop();
    period_ = period;
    periodic_ = periodic;
    running_ = true;
    thread_ = std::jthread([this](std::stop_token st) {
      std::unique_lock<std::mutex> lock(mutex_);
      while (running_ && !st.stop_requested()) {
        if (cv_.wait_for(lock, period_,
                         [&] { return !running_ || st.stop_requested(); })) {
          break;
        }
        if (callback_) callback_();
        if (!periodic_) break;
      }
      running_ = false;
    });
  }

  void Stop() override {
    running_ = false;
    cv_.notify_all();
    if (thread_.joinable()) {
      thread_.request_stop();
      thread_.join();
    }
  }

  bool IsRunning() const override { return running_; }

 private:
  TimerCallback callback_;
  std::chrono::milliseconds period_{0};
  bool periodic_ = false;
  std::atomic<bool> running_{false};
  std::jthread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
};

}  // namespace

std::unique_ptr<ITimer> CreateTimer(TimerCallback callback) {
  return std::make_unique<PosixTimer>(std::move(callback));
}

}  // namespace esp_modem_link::platform
