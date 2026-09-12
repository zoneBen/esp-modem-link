#pragma once

#include <chrono>
#include <memory>

namespace esp_modem_link::platform {

template <typename T>
class IQueue {
 public:
  virtual ~IQueue() = default;
  virtual bool Send(const T& item, std::chrono::milliseconds timeout) = 0;
  virtual bool Receive(T& item, std::chrono::milliseconds timeout) = 0;
  virtual size_t Count() const = 0;
};

template <typename T>
std::unique_ptr<IQueue<T>> CreateQueue(size_t max_items);

}  // namespace esp_modem_link::platform
