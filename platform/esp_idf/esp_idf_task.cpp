#include "platform/itask.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace esp_modem_link::platform {

namespace {

// What a running task needs, held apart from the object that started it.
//
// The task function is free to destroy that object - a user callback that tears
// the device down runs on this very task - and an ESP-IDF task that returns
// aborts instead of exiting, so Entry ends in vTaskDelete and no destructor runs
// after that point. Everything Entry touches once the function has returned has
// to belong to this struct, which the task keeps a reference to for exactly as
// long as it needs one.
struct Run {
  TaskFunction func;
  SemaphoreHandle_t done = nullptr;
  std::atomic<bool> running{true};
  TaskHandle_t handle = nullptr;

  ~Run() {
    if (done != nullptr) vSemaphoreDelete(done);
  }
};

class IdfTask : public ITask {
 public:
  IdfTask(std::string_view name, TaskFunction func, size_t stack_size,
          int priority)
      : func_(std::move(func)),
        // FreeRTOS keeps at most configMAX_TASK_NAME_LEN bytes including the
        // terminator and silently truncates the rest, so the name is cut here
        // rather than left to depend on that.
        name_(name.substr(0, kMaxNameLength)),
        // The size is bytes on this platform, as it is in the interface: ESP-IDF
        // defines StackType_t as uint8_t, so xTaskCreate's depth counts bytes
        // rather than the words vanilla FreeRTOS counts. The floor is the
        // kernel's own minimum, which carries the exception-frame overhead the
        // library's host-sized stacks do not allow for.
        stack_size_(std::max<size_t>(stack_size, configMINIMAL_STACK_SIZE)),
        // Priority 0 is the idle task's, and a task that shares it would get
        // only the idle task's leftovers - which for something that has to wake
        // a receive loop is not a priority at all.
        priority_(std::clamp(priority, 1, configMAX_PRIORITIES - 1)) {}

  ~IdfTask() override { Stop(); }

  void Start() override {
    if (run_) Stop();

    auto run = std::make_shared<Run>();
    run->func = func_;
    run->done = xSemaphoreCreateBinary();
    if (run->done == nullptr) return;

    // Handed to the task on the heap because a FreeRTOS entry point takes one
    // void* and Entry has to be able to drop the reference explicitly - see Run.
    auto* arg = new std::shared_ptr<Run>(run);
    const BaseType_t created = xTaskCreatePinnedToCore(
        &IdfTask::Entry, name_.c_str(), static_cast<uint32_t>(stack_size_), arg,
        static_cast<UBaseType_t>(priority_), &run->handle, tskNO_AFFINITY);
    if (created != pdPASS) {
      delete arg;
      return;
    }
    run_ = std::move(run);
  }

  void Stop() override {
    auto run = run_;
    run_ = nullptr;
    if (!run) return;

    if (run->handle != nullptr && xTaskGetCurrentTaskHandle() == run->handle) {
      // Called from inside the task's own function, which is reachable: the
      // device destructor can run in a callback on the receive task. Waiting
      // would be waiting for the one task that could ever give the semaphore.
      // The task is already leaving, and the object this call is destroying
      // cannot be used after it returns anyway.
      run->running.store(false);
      return;
    }

    // The semaphore is the task's last act, so this waits for the function to
    // have returned rather than for the task to be gone - which is what a join
    // gives, and what a destructor about to free what that function was using
    // needs. A task that already finished gave it before this call.
    if (run->handle != nullptr) xSemaphoreTake(run->done, portMAX_DELAY);
  }

  bool IsRunning() const override {
    return run_ != nullptr && run_->running.load();
  }

 private:
  static void Entry(void* arg) {
    std::shared_ptr<Run>* holder = static_cast<std::shared_ptr<Run>*>(arg);
    std::shared_ptr<Run> run = std::move(*holder);
    delete holder;

    run->func();

    run->running.store(false);
    xSemaphoreGive(run->done);
    // Dropped here rather than left to the end of the function, because the end
    // of the function never runs: this hands the struct to whoever else holds a
    // reference - a Stop() that is the one taking the semaphore now - or frees
    // it here if nobody does.
    run.reset();
    vTaskDelete(nullptr);
  }

  static constexpr size_t kMaxNameLength = configMAX_TASK_NAME_LEN - 1;

  TaskFunction func_;
  std::string name_;
  size_t stack_size_;
  int priority_;
  std::shared_ptr<Run> run_;
};

}  // namespace

std::unique_ptr<ITask> CreateTask(std::string_view name,
                                  TaskFunction func,
                                  size_t stack_size,
                                  int priority) {
  return std::make_unique<IdfTask>(name, std::move(func), stack_size, priority);
}

}  // namespace esp_modem_link::platform
