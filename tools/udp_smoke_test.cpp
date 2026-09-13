// UDP data-path test over real hardware.
//
//   udp_smoke_test COM8 [apn] [host] [port]
//
// Sends an NTP client query and waits for the reply. UDP is a separate code path
// from TCP in two places that unit tests cannot settle: the module reports
// inbound datagrams under a different URC event name than TCP, and a datagram
// has to survive the same inline-hex encoding. A 48-byte request answered by a
// 48-byte reply proves both directions of that path.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "esp_modem_link/cellular_device.h"
#include "esp_modem_link/network_interface.h"
#include "esp_modem_link/uart_config.h"
#include "esp_modem_link/udp_client.h"

using namespace esp_modem_link;

namespace {

std::mutex g_mutex;
std::condition_variable g_cv;
std::string g_received;

std::string ToVisible(std::string_view raw, size_t limit = 200) {
  std::string out;
  size_t shown = std::min(limit, raw.size());
  for (size_t i = 0; i < shown; ++i) {
    char c = raw[i];
    if (c >= 0x20 && c < 0x7f) {
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
  // An address, not a name, and this one in particular because the name is not
  // enough: "pool.ntp.org" resolves and the socket opens, but no member of the
  // pool ever answers it from this carrier, so the test waits out its full 20 s
  // on a datagram that was never sent. Measured - pool.ntp.org: no reply, twice;
  // 203.107.6.88 and 120.25.115.20: 48 bytes each, first try. A name that does
  // not answer is indistinguishable here from a receive path that does not work,
  // so the default is the address that settles the question the test is asking.
  const std::string host = argc > 3 ? argv[3] : "203.107.6.88";
  const uint16_t udp_port =
      static_cast<uint16_t>(argc > 4 ? std::atoi(argv[4]) : 123);

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
  std::printf("  module: %s\n",
              std::string(device.GetModuleRevision().value_or("?")).c_str());

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

  std::printf("\nOpening UDP to %s:%u\n", host.c_str(), udp_port);
  auto udp_result = device.GetNetwork().CreateUdp();
  if (!udp_result) {
    std::printf("  CreateUdp FAILED: %s\n", udp_result.error().Message());
    return 1;
  }
  auto& udp = **udp_result;

  udp.OnMessage([&](std::string_view src_host, uint16_t src_port,
                    std::string_view data) {
    std::printf("  datagram from %s:%u\n", std::string(src_host).c_str(),
                src_port);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_received.append(data);
    g_cv.notify_all();
  });
  udp.OnError([](const NetworkError& error) {
    std::printf("  udp error: %s\n", error.Message());
  });

  auto connect = udp.Connect(host, udp_port);
  if (!connect) {
    std::printf("  Connect FAILED: %s\n", connect.error().Message());
    return 1;
  }
  std::printf("  opened\n");

  // Minimal NTPv3 client request: LI=0, VN=3, Mode=3 (client) in byte 0, the
  // rest zero. A server answers with the same 48-byte layout.
  char query[48] = {};
  query[0] = 0x1b;

  auto sent = udp.Send(query, sizeof(query));
  if (!sent) {
    std::printf("  Send FAILED: %s\n", sent.error().Message());
    udp.Disconnect();
    return 1;
  }
  std::printf("  sent %d bytes\n", *sent);

  std::printf("\nWaiting for datagram (up to 20s)\n");
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_cv.wait_for(lock, std::chrono::seconds(20),
                  [] { return !g_received.empty(); });
  }

  std::printf("\nReceived %zu bytes:\n      %s\n", g_received.size(),
              ToVisible(g_received).c_str());

  // Any reply at all proves the receive path; NTP servers answer at exactly the
  // 48 bytes they were asked, so size is a cheap sanity check on framing.
  const bool got_reply = g_received.size() == sizeof(query);
  std::printf("\n%s\n", got_reply
                            ? "PASS - UDP datagram decoded correctly"
                            : "FAIL - no datagram, or unexpected length");
  if (!g_received.empty() && !got_reply) {
    std::printf("  (got %zu bytes, expected %zu)\n", g_received.size(),
                sizeof(query));
  }

  udp.Disconnect();
  return got_reply ? 0 : 1;
}
