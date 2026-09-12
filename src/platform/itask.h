#pragma once

#include <functional>
#include <memory>
#include <string_view>

namespace esp_modem_link::platform {

class ITask {
 public:
  virtual ~ITask() = default;
  virtual void Start() = 0;
  virtual void Stop() = 0;
  virtual bool IsRunning() const = 0;
};

using TaskFunction = std::function<void()>;

std::unique_ptr<ITask> CreateTask(std::string_view name,
                                  TaskFunction func,
                                  size_t stack_size = 4096,
                                  int priority = 5);

}  // namespace esp_modem_link::platform
