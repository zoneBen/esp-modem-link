#include "platform/serial_uart.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace esp_modem_link::platform {

namespace {

constexpr auto kReadSliceTimeout = std::chrono::milliseconds(50);

#ifdef _WIN32
using Handle = HANDLE;
// Not constexpr: INVALID_HANDLE_VALUE is a cast of -1, not a constant
// expression MSVC will fold.
const Handle kInvalidHandle = INVALID_HANDLE_VALUE;
#else
using Handle = int;
const Handle kInvalidHandle = -1;
#endif

// A real serial port, used for hardware bring-up on a desktop host.
//
// Windows: Win32 comm API. POSIX: termios.
//
// Reads run on a dedicated thread that blocks on the port and appends into an
// internal buffer; Receive() waits on a condition variable over that buffer.
// The thread matters: a blocking ReadFile holding the send lock would deadlock
// the caller that is trying to write the command the module is waiting for.
class SerialUart : public IUart {
 public:
  explicit SerialUart(const UartConfig& config) : config_(config) {}
  ~SerialUart() override { Close(); }

  SerialUart(const SerialUart&) = delete;
  SerialUart& operator=(const SerialUart&) = delete;

  bool Open() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (IsOpenLocked()) return true;
    if (!OpenLocked()) return false;

    running_ = true;
    reader_ = std::thread([this] { ReaderLoop(); });
    return true;
  }

  bool IsOpen() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return IsOpenLocked();
  }

  std::string LastError() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
  }

  void SetBaudRate(int baud) override {
    std::lock_guard<std::mutex> lock(state_mutex_);
    config_.baud_rate = baud;
    ApplyBaudLocked(baud);
  }

  int GetBaudRate() const override { return config_.baud_rate; }

  int Send(const void* data, size_t len) override {
    // Deliberately not state_mutex_: writing must not block on the reader.
    std::lock_guard<std::mutex> lock(write_mutex_);
    Handle handle;
    {
      std::lock_guard<std::mutex> state_lock(state_mutex_);
      if (!IsOpenLocked()) return -1;
      handle = handle_;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    size_t written = 0;
    while (written < len) {
      int n = WriteChunk(handle, bytes + written, len - written);
      if (n <= 0) return -1;
      written += static_cast<size_t>(n);
    }
    return static_cast<int>(written);
  }

  int Receive(void* buffer,
              size_t len,
              std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(rx_mutex_);
    if (rx_buffer_.empty() && timeout > std::chrono::milliseconds::zero()) {
      rx_cv_.wait_for(lock, timeout, [&] {
        return !rx_buffer_.empty() || !running_;
      });
    }
    size_t to_read = std::min(len, rx_buffer_.size());
    if (to_read == 0) return 0;
    std::memcpy(buffer, rx_buffer_.data(), to_read);
    rx_buffer_.erase(rx_buffer_.begin(),
                     rx_buffer_.begin() + static_cast<ptrdiff_t>(to_read));
    return static_cast<int>(to_read);
  }

  bool SupportsDma() const override { return false; }

  void Flush() override {
    std::lock_guard<std::mutex> rx_lock(rx_mutex_);
    rx_buffer_.clear();

    std::lock_guard<std::mutex> state_lock(state_mutex_);
    if (!IsOpenLocked()) return;
#ifdef _WIN32
    ::PurgeComm(handle_, PURGE_RXCLEAR | PURGE_TXCLEAR);
#else
    ::tcflush(handle_, TCIOFLUSH);
#endif
  }

  void Close() {
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if (!IsOpenLocked() && !reader_.joinable()) return;
      running_ = false;
      CancelIoLocked();
    }
    if (reader_.joinable()) reader_.join();

    std::lock_guard<std::mutex> lock(state_mutex_);
    CloseLocked();
    rx_cv_.notify_all();
  }

 private:
  bool IsOpenLocked() const { return handle_ != kInvalidHandle; }

  bool OpenLocked() {
#ifdef _WIN32
    // COM10+ requires the \\.\ prefix; using it unconditionally is harmless.
    std::string path = "\\\\.\\" + config_.device;
    handle_ = ::CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      last_error_ = "CreateFile(" + path +
                    ") failed, GetLastError=" + std::to_string(::GetLastError());
      return false;
    }

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(handle_, &dcb)) {
      last_error_ = "GetCommState failed";
      CloseLocked();
      return false;
    }
    dcb.BaudRate = static_cast<DWORD>(config_.baud_rate);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    // Drive DTR/RTS: modules commonly sit powered down or held in reset until
    // these lines are asserted.
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fErrorChar = FALSE;
    dcb.fAbortOnError = FALSE;

    if (!::SetCommState(handle_, &dcb)) {
      last_error_ = "SetCommState failed";
      CloseLocked();
      return false;
    }

    // The reader thread wants a bounded block so it can observe running_;
    // ReadIntervalTimeout=MAXDWORD makes ReadFile return as soon as any byte
    // arrives instead of filling the whole buffer.
    COMMTIMEOUTS timeouts{};
    timeouts.ReadIntervalTimeout = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    timeouts.ReadTotalTimeoutConstant =
        static_cast<DWORD>(kReadSliceTimeout.count());
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant = 2000;
    ::SetCommTimeouts(handle_, &timeouts);

    ::SetupComm(handle_, static_cast<DWORD>(config_.rx_buffer_size),
                static_cast<DWORD>(config_.tx_buffer_size));
    return true;
#else
    handle_ = ::open(config_.device.c_str(), O_RDWR | O_NOCTTY);
    if (handle_ < 0) {
      last_error_ = "open(" + config_.device +
                    ") failed, errno=" + std::to_string(errno);
      return false;
    }

    termios tty{};
    if (::tcgetattr(handle_, &tty) != 0) {
      last_error_ = "tcgetattr failed";
      CloseLocked();
      return false;
    }
    ::cfmakeraw(&tty);
    speed_t speed = BaudToSpeed(config_.baud_rate);
    ::cfsetispeed(&tty, speed);
    ::cfsetospeed(&tty, speed);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~CRTSCTS;
    // Blocking read that returns after kReadSliceTimeout ms with no data.
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = static_cast<cc_t>(
        std::chrono::duration_cast<std::chrono::deciseconds>(kReadSliceTimeout)
            .count());

    if (::tcsetattr(handle_, TCSANOW, &tty) != 0) {
      last_error_ = "tcsetattr failed";
      CloseLocked();
      return false;
    }
    return true;
#endif
  }

  void CloseLocked() {
#ifdef _WIN32
    if (handle_ != kInvalidHandle) {
      ::CloseHandle(handle_);
      handle_ = kInvalidHandle;
    }
#else
    if (handle_ != kInvalidHandle) {
      ::close(handle_);
      handle_ = kInvalidHandle;
    }
#endif
  }

  void CancelIoLocked() {
#ifdef _WIN32
    if (handle_ != kInvalidHandle) {
      ::CancelIoEx(handle_, nullptr);
    }
#endif
  }

  int WriteChunk(Handle handle, const uint8_t* data, size_t len) {
#ifdef _WIN32
    DWORD written = 0;
    if (!::WriteFile(handle, data, static_cast<DWORD>(len), &written,
                     nullptr)) {
      return -1;
    }
    return static_cast<int>(written);
#else
    ssize_t n = ::write(handle_, data, len);
    if (n < 0) {
      if (errno == EINTR) return 0;
      return -1;
    }
    return static_cast<int>(n);
#endif
  }

  // Blocks for up to kReadSliceTimeout, then returns what it got (possibly 0).
  int ReadChunk(Handle handle, uint8_t* buffer, size_t len) {
#ifdef _WIN32
    DWORD got = 0;
    if (!::ReadFile(handle, buffer, static_cast<DWORD>(len), &got, nullptr)) {
      return -1;
    }
    return static_cast<int>(got);
#else
    ssize_t n = ::read(handle_, buffer, len);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN) return 0;
      return -1;
    }
    return static_cast<int>(n);
#endif
  }

  void ReaderLoop() {
    Handle handle;
    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      handle = handle_;
    }

    std::vector<uint8_t> chunk(1024);
    std::string error;

    while (running_) {
      int n = ReadChunk(handle, chunk.data(), chunk.size());
      if (n < 0) {
        error = "read failed";
        break;
      }
      if (n == 0) continue;

      {
        std::lock_guard<std::mutex> lock(rx_mutex_);
        rx_buffer_.insert(rx_buffer_.end(), chunk.begin(),
                          chunk.begin() + n);
      }
      rx_cv_.notify_all();
    }

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      running_ = false;
      if (!error.empty()) last_error_ = error;
    }
    rx_cv_.notify_all();
  }

  void ApplyBaudLocked(int baud) {
    if (!IsOpenLocked()) return;
#ifdef _WIN32
    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    if (!::GetCommState(handle_, &dcb)) return;
    dcb.BaudRate = static_cast<DWORD>(baud);
    ::SetCommState(handle_, &dcb);
#else
    termios tty{};
    if (::tcgetattr(handle_, &tty) != 0) return;
    speed_t speed = BaudToSpeed(baud);
    ::cfsetispeed(&tty, speed);
    ::cfsetospeed(&tty, speed);
    ::tcsetattr(handle_, TCSANOW, &tty);
#endif
  }

#ifndef _WIN32
  static speed_t BaudToSpeed(int baud) {
    switch (baud) {
      case 9600: return B9600;
      case 19200: return B19200;
      case 38400: return B38400;
      case 57600: return B57600;
      case 115200: return B115200;
      case 230400: return B230400;
      case 460800: return B460800;
      case 921600: return B921600;
      default: return B115200;
    }
  }
#endif

  UartConfig config_;

  mutable std::mutex state_mutex_;
  std::mutex write_mutex_;
  std::mutex rx_mutex_;
  std::condition_variable rx_cv_;

  std::vector<uint8_t> rx_buffer_;
  std::thread reader_;
  std::atomic<bool> running_{false};
  std::string last_error_;

  Handle handle_ = kInvalidHandle;
};

}  // namespace

// Opens a real serial port named by config.device. Returns nullptr if the port
// cannot be opened; the caller checks for that rather than silently falling
// back to the mock, so a mistyped COM port surfaces as an error.
std::unique_ptr<IUart> CreateSerialUart(const UartConfig& config) {
  auto uart = std::make_unique<SerialUart>(config);
  if (!uart->Open()) {
    return nullptr;
  }
  return uart;
}

}  // namespace esp_modem_link::platform
