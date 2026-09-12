// Raw serial probe for hardware bring-up.
//
//   serial_probe COM8             -> try the usual baud rates
//   serial_probe COM8 9600        -> only try 9600
//
// Bypasses the whole AT stack: opens the port, writes "AT\r\n", and prints
// whatever bytes come back. If this shows nothing, the problem is wiring,
// power, or baud — not the library.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "esp_modem_link/uart_config.h"
#include "platform/serial_uart.h"

using esp_modem_link::platform::CreateSerialUart;
using esp_modem_link::platform::UartConfig;

namespace {

// Renders non-printable bytes so CR/LF and noise are visible in the output.
std::string ToVisible(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; ++i) {
    uint8_t c = data[i];
    if (c == '\r') {
      out += "\\r";
    } else if (c == '\n') {
      out += "\\n\n      ";
    } else if (c >= 0x20 && c < 0x7f) {
      out += static_cast<char>(c);
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02X", c);
      out += buf;
    }
  }
  return out;
}

bool ProbeBaud(const std::string& port, int baud) {
  UartConfig config;
  config.device = port;
  config.baud_rate = baud;

  auto uart = CreateSerialUart(config);
  if (!uart) {
    std::printf("  %6d  could not open port\n", baud);
    return false;
  }

  // Give a cold module a moment, then flush whatever boot noise arrived.
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  uint8_t buffer[512];
  size_t got = 0;
  while (true) {
    int n = uart->Receive(buffer + got, sizeof(buffer) - got - 1,
                           std::chrono::milliseconds(200));
    if (n <= 0) break;
    got += static_cast<size_t>(n);
    if (got >= sizeof(buffer) - 1) break;
  }
  if (got > 0) {
    std::printf("  %6d  boot noise: %s\n", baud,
                ToVisible(buffer, got).c_str());
  }

  // Send AT a few times; a module in a weird state often answers the 2nd or 3rd.
  for (int attempt = 1; attempt <= 3; ++attempt) {
    const char* cmd = "AT\r\n";
    if (uart->Send(cmd, 4) < 0) {
      std::printf("  %6d  write failed\n", baud);
      return false;
    }

    got = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      int n = uart->Receive(buffer + got, sizeof(buffer) - got - 1,
                             std::chrono::milliseconds(200));
      if (n > 0) got += static_cast<size_t>(n);
      // "OK" anywhere in the reply means this baud rate is correct.
      if (std::string(reinterpret_cast<char*>(buffer), got).find("OK") !=
          std::string::npos) {
        break;
      }
    }

    if (got > 0) {
      std::printf("  %6d  attempt %d -> %s\n", baud, attempt,
                  ToVisible(buffer, got).c_str());
      if (std::string(reinterpret_cast<char*>(buffer), got).find("OK") !=
          std::string::npos) {
        std::printf("  %6d  << module is alive at this baud rate\n", baud);
        return true;
      }
    } else {
      std::printf("  %6d  attempt %d -> (no response)\n", baud, attempt);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string port = argc > 1 ? argv[1] : "COM8";

  const std::vector<int> rates =
      argc > 2 ? std::vector<int>{std::atoi(argv[2])}
               : std::vector<int>{115200, 9600, 57600, 38400, 19200, 230400,
                                  460800, 921600};

  std::printf("Probing %s\n", port.c_str());
  for (int baud : rates) {
    if (ProbeBaud(port, baud)) {
      std::printf("\nWorking baud rate: %d\n", baud);
      return 0;
    }
  }

  std::printf(
      "\nNo baud rate produced a response.\n"
      "Things to check:\n"
      "  - Is another program holding the port open?\n"
      "  - Is the module powered, and is the antenna attached?\n"
      "  - Some boards need EN/RESET driven, or a USB-serial adapter with\n"
      "    the right voltage (3.3 V, not 5 V).\n");
  return 1;
}
