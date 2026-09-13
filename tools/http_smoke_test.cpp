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
// The host defaults to example.com. The plain-HTTP checks are the ones that host
// can always serve; the HTTPS check at the end depends on the module rather than
// on the host, because the ML307R this was written against holds no certificate
// authority - AT+MSSLCFG="cert" reads back "NULL","NULL","NULL" - so a verified
// handshake has nothing to check the server against and fails wherever it is
// pointed. That is why TlsConfig's default asks for no verification, and why the
// HTTPS section makes its second request with verification asked for
// explicitly: to show the difference rather than to assume either answer.
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
  // Redirect following is off for this check, because it is not what is under
  // test and on some hosts it decides the outcome: www.baidu.com answers a POST
  // to / with "302 Found" and a Location into HTTPS, so a client that follows
  // ends up testing TLS - and on a module with no certificate authority that hop
  // fails, which reads as a POST failure that it is not. What is asserted is the
  // status, so the proof being looked for is that the server parsed the request
  // and answered at all.
  std::printf("\nExecuting POST %s\n", root.c_str());
  {
    http.SetKeepAlive(false);
    http.SetFollowRedirects(false);
    http.SetHeader("Content-Type", "text/plain");
    http.SetBody("esp-modem-link");
    auto response = http.Execute("POST", root);
    // A host that refuses the method answers 405, which proves the same thing.
    if (!response) {
      Check("POST completes", false, Describe(response.error()).c_str());
    } else {
      Check("POST is answered", response->status_code > 0,
            "status " + std::to_string(response->status_code));
      const auto location = response->headers.find("location");
      if (location != response->headers.end()) {
        std::printf("  answered with a redirect to %s\n",
                    Elide(location->second, 100).c_str());
      }
    }
  }

  // --- 5b. The other half: that redirect, followed ---
  // A redirect the engine does follow has to end up on the transport the
  // Location names, and on this host that transport is HTTPS - so the hop runs
  // into the same missing certificate authority as the HTTPS check below, and
  // reaching it from this direction is that same finding rather than a second
  // one. Every other way of failing here is a real failure, so only that
  // outcome is reported rather than counted.
  std::printf("\nExecuting POST %s following redirects\n", root.c_str());
  {
    http.SetFollowRedirects(true);
    auto response = http.Execute("POST", root);
    if (response) {
      Check("the redirect is followed", response->status_code > 0,
            "status " + std::to_string(response->status_code));
    } else if (response.error().code == NetworkErrc::kTlsHandshakeFailed) {
      std::printf(
          "  note: the redirect leads into HTTPS, and the module will not "
          "perform that handshake: %s\n      see the HTTPS check below\n",
          Describe(response.error()).c_str());
    } else {
      Check("the redirect is followed", false, Describe(response.error()).c_str());
    }
  }

  // --- 5c. A body streamed as chunks ---
  // The proof being looked for is that the server accepted the framing and read
  // the body. A server that could not parse it answers 400 or 411; one waiting
  // for a terminator that never came says nothing at all, which surfaces here as
  // a timeout. So a real status, and specifically not one of those two, is what
  // says the request was well formed - and the exact status is the host's
  // business, so it is reported rather than asserted.
  //
  // A server is also entitled to answer before the body is finished: this host
  // answers 302 to a POST to / and closes the socket straight away, which is
  // measured rather than assumed - see the note below the pieces. Either outcome
  // exercises the framing, so both are accepted and which one happened is said.
  //
  // The body goes out in pieces rather than at once, because one chunk would not
  // tell a correct size line from a correct-by-luck single frame.
  std::printf("\nStreaming a chunked POST %s\n", root.c_str());
  {
    http.SetKeepAlive(false);
    http.SetFollowRedirects(false);
    http.SetHeader("Content-Type", "text/plain");
    // The body section above left one set, and a measured body and a streamed
    // one cannot both be the request's - the head would carry a length that the
    // chunks then contradict. Clearing it is the documented way to say which of
    // the two this request is.
    http.SetBody("");
    http.SetChunkedUpload(true);
    auto opened = http.Open("POST", root);
    if (!opened) {
      Check("a chunked request opens", false, Describe(opened.error()).c_str());
    } else {
      bool answered_early = false;
      bool wrote_all = true;
      for (const std::string& piece : {std::string("esp-modem"), std::string("-"),
                                       std::string("link ") +
                                           std::string(300, 'x')}) {
        auto wrote = http.Write(piece.data(), piece.size());
        if (!wrote) {
          wrote_all = false;
          // The server answering mid-body is not the framing's fault, and the
          // client is expected to stop and say so rather than keep writing into
          // a socket the server has let go of.
          answered_early = wrote.error().code == NetworkErrc::kProtocolError;
          Check("every chunk is accepted", answered_early,
                Describe(wrote.error()).c_str());
          if (!answered_early && !http.GetStatusCode().has_value()) {
            // Measured against www.baidu.com, which answers a POST to / with a
            // 302 and closes: that answer arrives while AT+MIPSEND is still in
            // flight, and AtUart folds a line arriving mid-command into that
            // command's response instead of handing it to the URC handlers. The
            // 302 is swallowed there, so the module's CME 550 on the next send
            // is all that is left to report. The framing is not what failed -
            // the chunk sends either side of the bad one were accepted - but
            // the upload cannot be finished until a response that arrives in
            // that window survives it.
            std::printf(
                "      note: the host answered before the body was finished and "
                "the answer never reached the client. A line that arrives while "
                "an AT command is in flight is absorbed into that command's "
                "response rather than dispatched as a URC, so the early answer "
                "is lost and the next send fails on a socket the host has "
                "already closed. Running this against a host that reads the "
                "whole body first - httpbin.org, which answers 405 only after "
                "the terminator - passes.\n");
          }
          break;
        }
        if (static_cast<size_t>(*wrote) != piece.size()) {
          wrote_all = false;
          Check("every chunk is accepted", false, "short write");
          break;
        }
      }
      if (wrote_all) {
        Check("every chunk is accepted", true);
        // Nothing has been answered yet, and cannot be: the server is still
        // waiting for the body to end. Reading its status here is what shows the
        // client is not pretending the response has started.
        Check("no response before the body ends",
              !http.GetStatusCode().has_value());
        if (auto ended = http.EndBody(); !ended) {
          Check("the body ends and the server answers", false,
                Describe(ended.error()).c_str());
        }
      } else if (answered_early) {
        std::printf("  the server answered before the body was finished\n");
      }

      auto status = http.GetStatusCode();
      const int code = status.value_or(0);
      Check("the server answers a chunked request", status.has_value(),
            status ? "status " + std::to_string(code)
                   : Describe(status.error()));
      Check("the chunked framing is accepted", code != 400 && code != 411,
            "status " + std::to_string(code));
      if (code > 0) {
        std::printf("  answered %d after %s the body\n", code,
                    answered_early ? "part of" : "all of");
      }
      Drain(http);
    }
    http.SetChunkedUpload(false);
  }

  // --- 6. TLS through the same engine ---
  // The transport is asked for TLS by the URL, which is the seam the factory
  // exists for; on this module TLS is configured in firmware by the HAL.
  //
  // Two requests, because the two halves answer different questions. The first
  // is the client's default, which asks the module to verify nothing, and it
  // says whether the transport can carry TLS at all. The second asks for a
  // checked server, which needs a certificate authority on the module: this
  // firmware holds none, so AT+MSSLCFG="cert" reads back "NULL","NULL","NULL"
  // and the handshake fails on every host. A handshake that fails there runs
  // inside AT+MIPOPEN, so the HAL reports it as a handshake failure with the
  // module's own code in `native` - 753 against example.com and 762 against
  // www.baidu.com on ML307R-DL-MBRH0S01. Which codes those are is the HAL's list
  // to keep; asking it here rather than matching codes again is what stops this
  // tool from disagreeing with the library about them.
  const std::string secure = "https://" + host + "/";
  std::printf("\nExecuting GET %s\n", secure.c_str());
  {
    auto response = http.Execute("GET", secure);
    if (!response) {
      Check("HTTPS through the software engine", false,
            Describe(response.error()).c_str());
    } else {
      Check("HTTPS returns 200", response->status_code == 200,
            "status " + std::to_string(response->status_code));
    }
  }

  // The same request asking for a checked server, which is the half this
  // hardware cannot do and the reason the default is what it is. Reported rather
  // than counted when the module refuses: a firmware holding no certificate
  // authority refusing to verify is the module behaving correctly, and the run
  // above already proved the transport works. Anything that is not that refusal
  // is a real failure, so those are still counted.
  {
    TlsConfig config;
    config.verify_certificate = true;
    config.verify_hostname = true;
    http.SetTlsConfig(config);
    std::printf("\nExecuting GET %s asking for verification\n", secure.c_str());
    auto response = http.Execute("GET", secure);
    if (response) {
      Check("HTTPS with verification returns 200", response->status_code == 200,
            "status " + std::to_string(response->status_code));
    } else if (response.error().code == NetworkErrc::kTlsHandshakeFailed) {
      std::printf(
          "  note: the module will not perform a verified handshake: %s\n"
          "      AT+MSSLCFG=\"cert\" reads back \"NULL\",\"NULL\",\"NULL\" on "
          "this firmware, so its SSL context holds no certificate authority to "
          "check a server against. A TlsConfig that asks for verification "
          "therefore cannot be honoured here; the connection above succeeded "
          "because the default asks for none.\n",
          Describe(response.error()).c_str());
    } else {
      Check("HTTPS with verification", false, Describe(response.error()).c_str());
    }
  }

  std::printf("\n%s\n", g_failures == 0 ? "PASS - all checks passed"
                                        : "FAIL - see the checks above");
  return g_failures == 0 ? 0 : 1;
}
