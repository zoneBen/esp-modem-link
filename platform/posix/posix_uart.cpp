#include "platform/iuart.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

#include "platform/serial_uart.h"

namespace esp_modem_link::platform {

namespace {

// Mock/Pipe UART for testing - loopback or programmable
class PosixUart : public IUart {
 public:
  explicit PosixUart(const UartConfig& config) : config_(config) {}

  void SetBaudRate(int baud) override { config_.baud_rate = baud; }
  int GetBaudRate() const override { return config_.baud_rate; }

  int Send(const void* data, size_t len) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    tx_buffer_.insert(tx_buffer_.end(), bytes, bytes + len);
    if (loopback_) {
      rx_buffer_.insert(rx_buffer_.end(), bytes, bytes + len);
      cv_.notify_all();
    }
    return static_cast<int>(len);
  }

  int Receive(void* buffer,
              size_t len,
              std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (rx_buffer_.empty()) {
      if (timeout == std::chrono::milliseconds::zero()) return 0;
      cv_.wait_for(lock, timeout, [&] { return !rx_buffer_.empty(); });
    }
    size_t to_read = std::min(len, rx_buffer_.size());
    if (to_read == 0) return 0;
    std::memcpy(buffer, rx_buffer_.data(), to_read);
    rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.begin() + to_read);
    return static_cast<int>(to_read);
  }

  // Test helpers
  void SetLoopback(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    loopback_ = enable;
  }

  void InjectRx(const void* data, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    rx_buffer_.insert(rx_buffer_.end(), bytes, bytes + len);
    cv_.notify_all();
  }

  size_t GetTxSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tx_buffer_.size();
  }

  std::vector<uint8_t> GetTxData() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto result = tx_buffer_;
    tx_buffer_.clear();
    return result;
  }

  void Flush() override {
    std::lock_guard<std::mutex> lock(mutex_);
    rx_buffer_.clear();
    tx_buffer_.clear();
  }

 private:
  UartConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<uint8_t> rx_buffer_;
  std::vector<uint8_t> tx_buffer_;
  bool loopback_ = false;
};

}  // namespace

std::unique_ptr<IUart> CreateUart(const UartConfig& config) {
  // A named device means the caller wants real hardware; fall back to the
  // mock only when they did not ask for a specific port.
  if (!config.device.empty()) {
    return CreateSerialUart(config);
  }
  return std::make_unique<PosixUart>(config);
}

}  // namespace esp_modem_link::platform
