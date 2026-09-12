// AT-layer trace over a real serial port.
//
//   at_trace COM8
//
// Opens the port, runs AtUart on top of it, and prints the raw response buffer
// for each command. Use this when hw_smoke_test fails at Detect: the raw probe
// proves the wiring, this proves what the parser was handed.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "at_channel/at_uart.h"
#include "esp_modem_link/uart_config.h"
#include "platform/serial_uart.h"

using esp_modem_link::at_channel::AtUart;
using esp_modem_link::platform::CreateSerialUart;
using esp_modem_link::platform::UartConfig;

namespace {

std::string ToVisible(std::string_view raw) {
  std::string out;
  for (char c : raw) {
    if (c == '\r') {
      out += "\\r";
    } else if (c == '\n') {
      out += "\\n\n      ";
    } else if (c >= 0x20 && c < 0x7f) {
      out += c;
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02X",
                    static_cast<unsigned char>(c));
      out += buf;
    }
  }
  return out;
}

void Run(AtUart& at, const char* cmd, int timeout_ms) {
  auto result = at.SendCommand(cmd, std::chrono::milliseconds(timeout_ms));
  std::printf("  %-16s -> %s\n", cmd, result ? "OK" : "FAILED");

  auto raw = at.GetResponse();
  if (raw.empty()) {
    std::printf("      (empty response buffer)\n");
  } else {
    std::printf("      raw: %s\n", ToVisible(raw).c_str());
  }

  auto lines = at.GetResponseLines();
  for (size_t i = 0; i < lines.size(); ++i) {
    std::printf("      line[%zu]: '%s'\n", i, std::string(lines[i]).c_str());
  }

  if (!result) {
    std::printf("      error: %s (AtErrc=%d)\n",
                result.error().ToString().c_str(),
                static_cast<int>(result.error().code));
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string port = argc > 1 ? argv[1] : "COM8";
  const int baud = argc > 2 ? std::atoi(argv[2]) : 115200;

  UartConfig config;
  config.device = port;
  config.baud_rate = baud;

  auto uart = CreateSerialUart(config);
  if (!uart) {
    std::printf("Could not open %s\n", port.c_str());
    return 1;
  }

  AtUart at(*uart, config);
  std::printf("Tracing %s at %d baud\n\n", port.c_str(), baud);

  // Extra arguments are run verbatim, which is how new commands get probed
  // against real hardware before they go into a HAL.
  if (argc > 3) {
    for (int i = 3; i < argc; ++i) {
      Run(at, argv[i], 3000);
    }
    return 0;
  }

  // Exactly what Ml307Hal::Initialize sends, with the same timeouts, so a
  // failure here names the command the smoke test only reports as "AT
  // command error".
  std::printf("-- initialize sequence --\n");
  Run(at, "ATE0", 5000);
  Run(at, "AT+CFUN=1", 5000);
  Run(at, "AT+MIPCFG=\"recv_format\",1", 5000);
  std::printf("\n-- detect sequence --\n");
  Run(at, "AT", 1000);
  Run(at, "AT+CGMR", 1000);

  std::printf("\n-- query sweep --\n");
  Run(at, "AT+CGSN", 2000);
  Run(at, "AT+CPIN?", 2000);
  Run(at, "AT+CSQ", 2000);
  Run(at, "AT+CEREG?", 2000);
  Run(at, "AT+COPS?", 2000);

  return 0;
}
