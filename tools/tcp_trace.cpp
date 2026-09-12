// Raw-byte TCP trace over a real serial port.
//
//   tcp_trace COM8 [apn] [host] [port]
//
// Unlike tcp_smoke_test this bypasses AtUart entirely: it writes AT lines and
// dumps every byte that comes back. It exists to answer one question the
// layered stack hides -- what exactly does the module emit for inbound socket
// data? The HAL subscribes to a specific URC name and field layout, and getting
// either wrong shows up only as silence.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "at_parser/at_parser.h"
#include "esp_modem_link/uart_config.h"
#include "platform/serial_uart.h"

using esp_modem_link::at_parser::EncodeHex;
using esp_modem_link::platform::CreateSerialUart;
using esp_modem_link::platform::IUart;
using esp_modem_link::platform::UartConfig;

namespace {

std::string ToVisible(std::string_view raw) {
  std::string out;
  for (char c : raw) {
    if (c == '\r') {
      out += "\\r";
    } else if (c == '\n') {
      out += "\\n\n";
    } else if (c >= 0x20 && c < 0x7f) {
      out += c;
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02X", static_cast<unsigned char>(c));
      out += buf;
    }
  }
  return out;
}

void SendLine(IUart& uart, const std::string& line) {
  std::printf(">> %s\n", line.c_str());
  std::string wire = line + "\r\n";
  uart.Send(wire.data(), wire.size());
}

// Drain whatever the module has to say for the given window, printing it as it
// arrives rather than after the fact, so a line that never comes is obvious.
void Listen(IUart& uart, std::chrono::milliseconds window) {
  char buf[1024];
  auto deadline = std::chrono::steady_clock::now() + window;
  while (std::chrono::steady_clock::now() < deadline) {
    int n = uart.Receive(buf, sizeof(buf), std::chrono::milliseconds(200));
    if (n > 0) {
      std::printf("<< %s\n", ToVisible({buf, static_cast<size_t>(n)}).c_str());
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string port = argc > 1 ? argv[1] : "COM8";
  const std::string apn = argc > 2 ? argv[2] : "cmnet";
  const std::string host = argc > 3 ? argv[3] : "example.com";
  const int tcp_port = argc > 4 ? std::atoi(argv[4]) : 80;

  UartConfig config;
  config.device = port;
  config.baud_rate = 115200;

  auto uart = CreateSerialUart(config);
  if (!uart) {
    std::printf("Could not open %s\n", port.c_str());
    return 1;
  }

  std::printf("Raw tracing %s at %d baud\n\n", port.c_str(),
              config.baud_rate);

  SendLine(*uart, "ATE0");
  Listen(*uart, std::chrono::milliseconds(500));

  std::printf("\n-- PDP --\n");
  SendLine(*uart, "AT+CGDCONT=1,\"IP\",\"" + apn + "\"");
  Listen(*uart, std::chrono::milliseconds(1000));
  SendLine(*uart, "AT+MIPCALL=1,1");
  Listen(*uart, std::chrono::milliseconds(3000));

  std::printf("\n-- socket setup --\n");
  SendLine(*uart, "AT+MIPCFG=\"ssl\",0,0,0");
  Listen(*uart, std::chrono::milliseconds(1000));
  SendLine(*uart, "AT+MIPCFG=\"encoding\",0,1,1");
  Listen(*uart, std::chrono::milliseconds(1000));

  std::printf("\n-- open --\n");
  char open_cmd[256];
  std::snprintf(open_cmd, sizeof(open_cmd),
                "AT+MIPOPEN=0,\"TCP\",\"%s\",%d,,0", host.c_str(), tcp_port);
  SendLine(*uart, open_cmd);
  Listen(*uart, std::chrono::milliseconds(5000));

  std::printf("\n-- send --\n");
  const std::string request =
      "GET / HTTP/1.0\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
  std::string send_cmd = "AT+MIPSEND=0," + std::to_string(request.size()) + ",";
  send_cmd += EncodeHex(request);
  SendLine(*uart, send_cmd);

  std::printf("\n-- listening 20s for the inbound-data URC --\n");
  Listen(*uart, std::chrono::seconds(20));

  std::printf("\n-- close --\n");
  SendLine(*uart, "AT+MIPCLOSE=0");
  Listen(*uart, std::chrono::milliseconds(2000));

  return 0;
}
