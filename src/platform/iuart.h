#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>

#include "esp_modem_link/uart_config.h"

namespace esp_modem_link::platform {

using DmaRxCallback = std::function<void(const uint8_t* data, size_t len)>;

class IUart {
 public:
  virtual ~IUart() = default;

  virtual void SetBaudRate(int baud) = 0;
  virtual int GetBaudRate() const = 0;

  virtual int Send(const void* data, size_t len) = 0;
  virtual int Receive(void* buffer,
                      size_t len,
                      std::chrono::milliseconds timeout) = 0;

  virtual bool SupportsDma() const { return false; }
  virtual void StartDmaRx(size_t buffer_size, DmaRxCallback callback) {
    (void)buffer_size;
    (void)callback;
  }
  virtual void StopDmaRx() {}

  virtual void Flush() {}
};

std::unique_ptr<IUart> CreateUart(const UartConfig& config);

}  // namespace esp_modem_link::platform
