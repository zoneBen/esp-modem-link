// HTTP engine test over real hardware.
//
//   http_smoke_test COM8 [apn] [host] [port]
//
// Opens the module, brings up the PDP context, then drives the software HTTP
// engine - the same SoftwareHttpClient the factory hands out - against a real
// server. Unit tests cover the parser and the client against a mock socket;
// what they cannot cover is whether the request this engine builds is one a
// real server accepts, and whether the bytes a real module delivers reassemble
// into the response the parser expects.
//
// The host defaults to example.com, which serves the plain-HTTP checks fine but
// which this module's TLS stack cannot negotiate with; see the HTTPS check at
// the end. Pass a host the module can reach over TLS to exercise that path.
//
// This tool is also sensitive to a socket left open on the module by an earlier
// run, which is how the cid-reuse defect was found: the module names only the
// cid in its socket events, so a close event for a previous occupant of a cid
// arrived after a new connection was up on that cid and tore it down, failing
// the first request with "connection closed before the response was complete"
// while every later request passed. Reproduce it by leaving a socket open -
//
//   at_trace COM8 115200 'AT+MIPCFG="ssl",0,0' 'AT+MIPCFG="encoding",0,1,1' \
//     'AT+MIPOPEN=0,"TCP","www.baidu.com",80,,0'
//
// - and then running this. It cannot be provoked from inside this tool, because
// the HAL closes what it opens even when a client is dropped.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "esp_modem_link/cellular_device.h"
#include "esp_modem_link/http_client.h"
#include "esp_modem_link/network_interface.h"
#include "esp_modem_link/uart_config.h"
#include "at_channel/iat_channel.h"

using namespace esp_modem_link;

namespace {

int g_failures = 0;

void Check(const char* name, bool ok, const std::string& detail = "") {
  std::printf("  [%s] %s", ok ? "PASS" : "FAIL", name);
  if (!detail.empty()) std::printf(" - %s", detail.c_str());
  std::printf("\n");
  if (!ok) ++g_failures;
}

std::string Elide(std::string_view text, size_t limit = 160) {
  std::string out(text.substr(0, std::min(limit, text.size())));
  for (char& c : out) {
    if (c == '\r' || c == '\n') c = ' ';
  }
  if (text.size() > limit) out += "...";
  return out;
}

// The generic message loses the module's own code, which for a connection
// refusal is the only thing that says why.
std::string Describe(const NetworkError& error) {
  std::string out = error.Message();
  out += " (code=" + std::to_string(static_cast<int>(error.code));
  out += " native=" + std::to_string(error.native);
  if (!error.context.empty()) out += " ctx='" + error.context + "'";
  out += ")";
  return out;
}

// Reads a streaming response to its end, reporting the error rather than
// silently returning a short body.
struct DrainResult {
  bool ok = false;
  std::string body;
  std::string error;
};

DrainResult Drain(HttpClient& client) {
  DrainResult result;
  char buffer[256];
  while (true) {
    auto read = client.Read(buffer, sizeof(buffer));
    if (!read) {
      result.error = Describe(read.error());
      return result;
    }
    if (*read == 0) break;
    result.body.append(buffer, static_cast<size_t>(*read));
  }
  result.ok = true;
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  // The flag is accepted anywhere and then taken out of the line, so asking for
  // a trace does not mean supplying a placeholder for the optional port.
  std::vector<std::string> args;
  bool trace = false;
  int baud = 115200;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "trace") {
      trace = true;
    } else if (arg.rfind("baud=", 0) == 0) {
      baud = std::atoi(arg.c_str() + 5);
    } else {
      args.emplace_back(arg);
    }
  }

  const std::string port = args.size() > 0 ? args[0] : "COM8";
  const std::string apn = args.size() > 1 ? args[1] : "cmnet";
  const std::string host = args.size() > 2 ? args[2] : "example.com";
  const std::string base =
      "http://" + host + (args.size() > 3 ? ":" + args[3] : "");

  platform::UartConfig config;
  config.device = port;
  config.baud_rate = baud;

  std::printf("Opening %s at %d baud\n", port.c_str(), baud);
  auto device_result = CellularDevice::Detect(config);
  if (!device_result) {
    std::printf("detect FAILED: %s\n", Describe(device_result.error()).c_str());
    return 1;
  }
  auto& device = **device_result;
  if (trace) device.GetAtChannel().SetDebugLog(true);
  std::printf("  module: %s\n",
              std::string(device.GetModuleRevision().value_or("?")).c_str());

  std::printf("\nActivating PDP with APN \"%s\"\n", apn.c_str());
  ApnConfig apn_config;
  apn_config.apn = apn;
  if (auto apn_result = device.ConfigureApn(apn_config); !apn_result) {
    std::printf("  APN setup FAILED: %s\n", Describe(apn_result.error()).c_str());
    return 1;
  }
  std::printf("  PDP activated\n");

  std::printf("\nWaiting for registration\n");
  if (auto wait = device.WaitForNetwork(std::chrono::seconds(30)); !wait) {
    std::printf("  not registered: %s\n", Describe(wait.error()).c_str());
  } else {
    std::printf("  registered\n");
  }

  auto& network = device.GetNetwork();

  // The engine under test is the software one, so ask for it explicitly even
  // though the ML307 advertises an HTTP stack of its own.
  network.SetProtocolMode(ProtocolMode::kSoftware);

  auto client_result = network.CreateHttp();
  if (!client_result) {
    std::printf("\nCreateHttp FAILED: %s\n",
                Describe(client_result.error()).c_str());
    return 1;
  }
  auto& http = **client_result;
  http.SetTimeout(std::chrono::seconds(20));

  // --- 1. A whole response, buffered ---
  const std::string root = base + "/";
  std::printf("\nExecuting GET %s\n", root.c_str());
  std::string buffered_body;
  {
    auto response = http.Execute("GET", root);
    if (!response) {
      Check("Execute completes", false, Describe(response.error()).c_str());
    } else {
      std::printf("  status %d, %zu header(s), %zu body byte(s)\n",
                  response->status_code, response->headers.size(),
                  response->body.size());
      for (const auto& [key, value] : response->headers) {
        std::printf("    %s: %s\n", key.c_str(), Elide(value, 80).c_str());
      }
      Check("status is 200", response->status_code == 200,
            "got " + std::to_string(response->status_code));
      Check("body is not empty", !response->body.empty());
      Check("Content-Type was parsed",
            response->headers.find("content-type") != response->headers.end());
      buffered_body = response->body;
    }
  }

  // --- 2. The same request, streamed ---
  // Comparing the two is the check that needs no assumption about the server:
  // the streaming path and the buffered path must reassemble identical bytes.
  std::printf("\nStreaming GET %s\n", root.c_str());
  {
    auto opened = http.Open("GET", root);
    if (!opened) {
      Check("Open returns after headers", false, Describe(opened.error()).c_str());
    } else {
      auto status = http.GetStatusCode();
      Check("status available after Open", status.has_value() && *status == 200,
            status ? "got " + std::to_string(*status)
                   : Describe(status.error()));
      std::printf("  Content-Length: %zu\n", http.GetContentLength());

      auto drained = Drain(http);
      Check("Read streams the whole body", drained.ok, drained.error);
      if (drained.ok && !buffered_body.empty()) {
        Check("streamed body matches buffered body",
              drained.body == buffered_body,
              "streamed " + std::to_string(drained.body.size()) + " vs " +
                  std::to_string(buffered_body.size()) + " bytes");
      }
      if (drained.ok && !drained.body.empty()) {
        std::printf("  body: %s\n", Elide(drained.body).c_str());
      }
    }
  }

  // --- 3. Status codes are reported, not turned into errors ---
  const std::string missing = base + "/esp-modem-link-no-such-page";
  std::printf("\nExecuting GET %s\n", missing.c_str());
  {
    auto response = http.Execute("GET", missing);
    if (!response) {
      Check("a 404 is a response, not an error", false,
            Describe(response.error()).c_str());
    } else {
      Check("absent page reports 404", response->status_code == 404,
            "got " + std::to_string(response->status_code));
    }
  }

  // --- 4. Keep-alive: a second request on the same socket ---
  std::printf("\nKeep-alive: two requests on one connection\n");
  {
    http.SetKeepAlive(true);
    auto first = http.Execute("GET", root);
    auto second = http.Execute("GET", root);
    Check("first request on a kept-alive connection",
          first.has_value() && first->status_code == 200,
          first ? "status " + std::to_string(first->status_code)
                : Describe(first.error()).c_str());
    Check("second request reuses the connection",
          second.has_value() && second->status_code == 200,
          second ? "status " + std::to_string(second->status_code)
                 : Describe(second.error()).c_str());
    http.Close();
  }

  // --- 5. A body the client has to send ---
  std::printf("\nExecuting POST %s\n", root.c_str());
  {
    http.SetKeepAlive(false);
    http.SetHeader("Content-Type", "text/plain");
    http.SetBody("esp-modem-link");
    auto response = http.Execute("POST", root);
    // example.com rejects a POST with 405, which still proves the request was
    // well formed enough for the server to parse and answer.
    if (!response) {
      Check("POST completes", false, Describe(response.error()).c_str());
    } else {
      Check("POST is answered", response->status_code > 0,
            "status " + std::to_string(response->status_code));
    }
  }

  // --- 6. TLS through the same engine ---
  // The transport is asked for TLS by the URL, which is the seam the factory
  // exists for; on this module TLS is configured in firmware by the HAL.
  //
  // The handshake happens inside AT+MIPOPEN, so a server this firmware cannot
  // negotiate with fails here and not later. That shows up as the module's own
  // code in `native` - 753 on ML307R-DL-MBRH0S01 - and it is a property of the
  // host, not of this engine: example.com returns 753 for every configuration
  // tried, while hosts with more conservative TLS settings answer 0. Pass a
  // different host before reading a failure here as an engine defect.
  const std::string secure = "https://" + host + "/";
  std::printf("\nExecuting GET %s\n", secure.c_str());
  {
    auto response = http.Execute("GET", secure);
    if (!response) {
      std::string detail = Describe(response.error());
      if (response.error().native == 753) {
        detail += " - the module's TLS stack could not negotiate with this "
                  "host; try another one";
      }
      Check("HTTPS through the software engine", false, detail);
    } else {
      Check("HTTPS returns 200", response->status_code == 200,
            "status " + std::to_string(response->status_code));
    }
  }

  std::printf("\n%s\n", g_failures == 0 ? "PASS - all checks passed"
                                        : "FAIL - see the checks above");
  return g_failures == 0 ? 0 : 1;
}
