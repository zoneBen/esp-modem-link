#pragma once

#include <cstdint>
#include <string>

namespace esp_modem_link::platform {

enum class UartParity { kNone, kEven, kOdd };

enum class UartStopBits { kOne, kTwo };

enum class UartFlowControl { kNone, kRts, kCts, kRtsCts };

struct UartConfig {
  // Host-only: a real serial device to open instead of the in-memory mock,
  // e.g. "COM8" on Windows or "/dev/ttyUSB0" on Linux. Empty means the
  // platform's default (mock on POSIX, UART<port> on ESP-IDF).
  std::string device;
  int port = 1;
  int tx_pin = -1;
  int rx_pin = -1;
  int rts_pin = -1;
  int cts_pin = -1;
  int baud_rate = 115200;
  int data_bits = 8;
  UartParity parity = UartParity::kNone;
  UartStopBits stop_bits = UartStopBits::kOne;
  UartFlowControl flow_control = UartFlowControl::kNone;
  size_t rx_buffer_size = 4096;
  size_t tx_buffer_size = 4096;
};

}  // namespace esp_modem_link::platform
