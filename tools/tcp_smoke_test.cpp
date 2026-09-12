// TCP data-path test over real hardware.
//
//   tcp_smoke_test COM8 [apn] [host] [port]
//
// Opens the module, brings up the PDP context, then does a raw TCP request and
// prints what comes back. This is the path that exercises AT+MIPOPEN /
// AT+MIPSEND and the +MIPURC: "rtcp" receive URC, including the hex encoding
// the HAL assumes, none of which unit tests can confirm against real firmware.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include "esp_modem_link/cellular_device.h"
#include "esp_modem_link/network_interface.h"
#include "esp_modem_link/tcp_client.h"
#include "esp_modem_link/uart_config.h"

using namespace esp_modem_link;

namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
std::string g_received;
bool g_closed = false;

std::string ToVisible(std::string_view raw, size_t limit = 400) {
  std::string out;
  size_t shown = std::min(limit, raw.size());
  for (size_t i = 0; i < shown; ++i) {
    char c = raw[i];
    if (c == '\r') {
      out += "\\r";
    } else if (c == '\n') {
      out += "\\n\n      ";
    } else if (c >= 0x20 && c < 0x7f) {
      out += c;
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02X", static_cast<unsigned char>(c));
      out += buf;
    }
  }
  if (raw.size() > shown) out += "...";
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string port = argc > 1 ? argv[1] : "COM8";
  const std::string apn = argc > 2 ? argv[2] : "cmnet";
  const std::string host = argc > 3 ? argv[3] : "example.com";
  const uint16_t tcp_port =
      static_cast<uint16_t>(argc > 4 ? std::atoi(argv[4]) : 80);

  platform::UartConfig config;
  config.device = port;
  config.baud_rate = 115200;

  std::printf("Opening %s\n", port.c_str());
  auto device_result = CellularDevice::Detect(config);
  if (!device_result) {
    std::printf("detect FAILED: %s\n", device_result.error().Message());
    return 1;
  }
  auto& device = **device_result;
  std::printf("  module: %s\n", std::string(device.GetModuleRevision().value_or("?")).c_str());

  // The module needs a PDP context up before any socket will open.
  std::printf("\nActivating PDP with APN \"%s\"\n", apn.c_str());
  ApnConfig apn_config;
  apn_config.apn = apn;
  auto apn_result = device.ConfigureApn(apn_config);
  if (!apn_result) {
    std::printf("  APN setup FAILED: %s (code=%d native=%d ctx='%s')\n",
                apn_result.error().Message(),
                static_cast<int>(apn_result.error().code),
                apn_result.error().native,
                apn_result.error().context.c_str());
    return 1;
  }
  std::printf("  PDP activated\n");

  std::printf("\nWaiting for registration\n");
  auto wait = device.WaitForNetwork(std::chrono::seconds(30));
  if (!wait) {
    std::printf("  not registered: %s\n", wait.error().Message());
  } else {
    std::printf("  registered\n");
  }

  std::printf("\nOpening TCP to %s:%u\n", host.c_str(), tcp_port);
  auto tcp_result = device.GetNetwork().CreateTcp();
  if (!tcp_result) {
    std::printf("  CreateTcp FAILED: %s\n", tcp_result.error().Message());
    return 1;
  }
  auto& tcp = **tcp_result;

  tcp.OnData([](std::string_view data) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_received.append(data);
    g_cv.notify_all();
  });
  tcp.OnDisconnected([] {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_closed = true;
    g_cv.notify_all();
  });
  tcp.OnError([](const NetworkError& error) {
    std::printf("  tcp error: %s\n", error.Message());
  });

  auto connect = tcp.Connect(host, tcp_port);
  if (!connect) {
    std::printf("  Connect FAILED: %s\n", connect.error().Message());
    return 1;
  }
  std::printf("  connected\n");

  const std::string request = "GET / HTTP/1.0\r\nHost: " + host +
                              "\r\nConnection: close\r\n\r\n";
  auto sent = tcp.Send(request.data(), request.size());
  if (!sent) {
    std::printf("  Send FAILED: %s\n", sent.error().Message());
    tcp.Disconnect();
    return 1;
  }
  std::printf("  sent %d bytes\n", *sent);

  std::printf("\nWaiting for response (up to 20s)\n");
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait_for(lock, std::chrono::seconds(20),
                  [] { return g_closed || g_received.size() > 0; });
  }

  std::printf("\nReceived %zu bytes:\n      %s\n", g_received.size(),
              ToVisible(g_received).c_str());

  const bool got_http = g_received.find("HTTP/") != std::string::npos;
  std::printf("\n%s\n", got_http ? "PASS - HTTP response decoded correctly"
                                 : "FAIL - no HTTP response");

  tcp.Disconnect();
  return got_http ? 0 : 1;
}
