#include "platform/itask.h"

#include <atomic>
#include <string>
#include <thread>

namespace esp_modem_link::platform {

namespace {

class PosixTask : public ITask {
 public:
  PosixTask(std::string_view name, TaskFunction func, size_t stack_size,
            int priority)
      : func_(std::move(func)),
        name_(name),
        stack_size_(stack_size),
        priority_(priority) {}

  ~PosixTask() override { Stop(); }

  void Start() override {
    if (running_) return;
    running_ = true;
    thread_ = std::jthread([this]() {
      if (func_) func_();
      running_ = false;
    });
  }

  void Stop() override {
    running_ = false;
    if (thread_.joinable()) {
      thread_.request_stop();
      if (thread_.joinable()) thread_.join();
    }
  }

  bool IsRunning() const override { return running_; }

 private:
  TaskFunction func_;
  std::string name_;
  size_t stack_size_;
  int priority_;
  std::jthread thread_;
  std::atomic<bool> running_{false};
};

}  // namespace

std::unique_ptr<ITask> CreateTask(std::string_view name,
                                  TaskFunction func,
                                  size_t stack_size,
                                  int priority) {
  return std::make_unique<PosixTask>(name, std::move(func), stack_size,
                                     priority);
}

}  // namespace esp_modem_link::platform
