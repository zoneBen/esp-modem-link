#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

#include "platform/iqueue.h"

namespace esp_modem_link::platform {

template <typename T>
class PosixQueue : public IQueue<T> {
 public:
  explicit PosixQueue(size_t max_items) : max_items_(max_items) {}

  bool Send(const T& item, std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (timeout == std::chrono::milliseconds::zero()) {
      if (queue_.size() >= max_items_) return false;
    } else {
      if (!cv_not_full_.wait_for(lock, timeout,
                                  [&] { return queue_.size() < max_items_; })) {
        return false;
      }
    }
    queue_.push_back(item);
    cv_not_empty_.notify_one();
    return true;
  }

  bool Receive(T& item, std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (timeout == std::chrono::milliseconds::zero()) {
      if (queue_.empty()) return false;
    } else {
      if (!cv_not_empty_.wait_for(lock, timeout,
                                   [&] { return !queue_.empty(); })) {
        return false;
      }
    }
    item = std::move(queue_.front());
    queue_.pop_front();
    cv_not_full_.notify_one();
    return true;
  }

  size_t Count() const override {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

 private:
  size_t max_items_;
  mutable std::mutex mutex_;
  std::condition_variable cv_not_empty_;
  std::condition_variable cv_not_full_;
  std::deque<T> queue_;
};

template <typename T>
std::unique_ptr<IQueue<T>> CreateQueue(size_t max_items) {
  return std::make_unique<PosixQueue<T>>(max_items);
}

}  // namespace esp_modem_link::platform
