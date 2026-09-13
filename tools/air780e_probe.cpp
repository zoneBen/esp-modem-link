// Raw bring-up probe for the Air780E AT firmware (AirM2M_780EPV_*_LTE_AT).
//
//   air780e_probe <port> [scenario] [baud]
//
// Scenarios:
//   tcp     a single TCP transaction end to end (the default)
//   state   what the module says about cids that are free, taken, or dead
//   reset   whether a socket stuck in CLOSED can be cleared, and how
//   recycle what happens to a cid after a close, a failure, or a peer's close
//   readlen how much of a queued body one AT+CIPRXGET=3 will hand back
//   push    what AT+CIPRXGET=0 pushes, for the argument against using it
//   tls     the TLS configuration surface, and whether a switch is accepted
//   tlsread a whole page over TLS, read to empty with the count checked each round
//   udp     a UDP round trip, and whether the source address is recoverable
//   misc    identity, network, APN and rate queries
//
// Every scenario starts by putting the module back on a clean slate and ends by
// restoring the defaults, so a run does not change what the next one measures.
//
// It deliberately sits below AtUart: the line-oriented layer would hide exactly
// the things this module has to be measured for - the send prompt's framing,
// whether inbound data is pushed or pulled, and what a read hands back.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "esp_modem_link/uart_config.h"
#include "platform/serial_uart.h"

using esp_modem_link::platform::CreateSerialUart;
using esp_modem_link::platform::UartConfig;

namespace {

constexpr int kMaxCids = 6;
// Measured, not assumed: AT+CIPRXGET=3,<id>,1024 is refused with +CME ERROR: 3
// while the same command with 64 is served, so the ceiling is below 1024. 512
// was served in the first session's probe, and AT+CIPRXGET=? is printed by
// EnterDataModes so the advertised range is on the record too.
constexpr int kReadChunk = 512;

std::string ToVisible(const uint8_t* data, size_t len) {
  std::string out;
  for (size_t i = 0; i < len; ++i) {
    uint8_t c = data[i];
    if (c == '\r') {
      out += "\\r";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\t') {
      out += "\\t";
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

class Probe {
 public:
  explicit Probe(esp_modem_link::platform::IUart& uart) : uart_(uart) {}

  void Send(const std::string& text) {
    uart_.Send(text.data(), text.size());
  }

  // Drains for `window_ms`, printing each burst as it arrives so the framing of
  // a reply and the gaps inside it are both readable. Returns true if anything
  // arrived, which is how a scenario asks "did that produce a URC at all?".
  bool Drain(int window_ms, const char* why = nullptr) {
    if (why != nullptr) {
      std::printf("  ... watching %d ms for %s\n", window_ms, why);
      std::fflush(stdout);
    }
    bool saw_anything = false;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(window_ms);
    uint8_t buffer[1024];
    while (std::chrono::steady_clock::now() < deadline) {
      int n = uart_.Receive(buffer, sizeof(buffer),
                            std::chrono::milliseconds(50));
      if (n <= 0) continue;
      saw_anything = true;
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start_)
                               .count();
      std::printf("  [%6lld ms] %s\n", elapsed,
                  ToVisible(buffer, static_cast<size_t>(n)).c_str());
      std::fflush(stdout);
    }
    return saw_anything;
  }

  void Step(std::string_view label, std::string_view line, int window_ms) {
    std::printf("\n>>> %s: %.*s\n", std::string(label).c_str(),
                static_cast<int>(line.size()), line.data());
    std::fflush(stdout);
    Send(std::string(line) + "\r\n");
    Drain(window_ms);
  }

  void Step(std::string_view line, int window_ms) { Step("", line, window_ms); }

  void Payload(const char* label, const void* data, size_t len, int window_ms) {
    std::printf("\n>>> %s (%zu bytes): %s\n", label, len,
                ToVisible(static_cast<const uint8_t*>(data), len).c_str());
    std::fflush(stdout);
    uart_.Send(data, len);
    Drain(window_ms);
  }

  void Section(const char* title) {
    std::printf("\n=== %s ===\n", title);
    std::fflush(stdout);
  }

  // Returns every cid to INITIAL, which is the precondition for changing
  // AT+CIPMUX and the only way to know what the next scenario starts from.
  // Closing the cids one at a time is not enough: a cid whose connect failed or
  // whose peer went away sits in CLOSED, where AT+CIPCLOSE answers +CME ERROR: 3
  // and AT+CIPMUX refuses to move. AT+CIPSHUT clears all of them.
  void CleanSlate() {
    Section("clean slate");
    Step("echo off", "ATE0", 400);
    Step("reset the ip stack", "AT+CIPSHUT", 4000);
  }

  // The two modes every scenario that moves data needs. AT+CIPRXGET comes after
  // AT+CIPMUX because changing the mux is documented to reset it.
  void EnterDataModes(bool report_every_arrival = true) {
    Section("data modes");
    Step("multi-socket", "AT+CIPMUX=1", 400);
    Step("read back the mux", "AT+CIPMUX?", 400);
    Step(report_every_arrival ? "manual receive, report on every arrival"
                              : "manual receive, quiet",
         report_every_arrival ? "AT+CIPRXGET=5" : "AT+CIPRXGET=1", 400);
    Step("read back the receive mode", "AT+CIPRXGET?", 400);
    Step("the receive form's range", "AT+CIPRXGET=?", 800);
  }

  void ReadRounds(int id, int rounds) {
    for (int i = 0; i < rounds; ++i) {
      std::printf("\n--- read round %d of %d ---\n", i + 1, rounds);
      std::fflush(stdout);
      Step("unread count", "AT+CIPRXGET=4," + std::to_string(id), 800);
      Step("read " + std::to_string(kReadChunk) + " as hex",
           "AT+CIPRXGET=3," + std::to_string(id) + "," +
               std::to_string(kReadChunk),
           2500);
    }
  }

  void Close(int id) {
    Step("close", "AT+CIPCLOSE=" + std::to_string(id), 1500);
  }

  // Returns the module to the settings another tool would expect to find.
  void Restore() {
    Section("restore defaults");
    Step("reset the ip stack", "AT+CIPSHUT", 3000);
    Step("read back the mux", "AT+CIPMUX?", 600);
    Step("single-socket mode", "AT+CIPMUX=0", 800);
    Step("read back the mux", "AT+CIPMUX?", 600);
    Step("hand data over on arrival", "AT+CIPRXGET=0", 400);
    Step("no per-socket TLS", "AT+CIPSSL=0", 400);
    Step("no source address prefix", "AT+CIPSRIP=0", 400);
  }

 private:
  esp_modem_link::platform::IUart& uart_;
  const std::chrono::steady_clock::time_point start_ =
      std::chrono::steady_clock::now();
};

// A DNS A query for www.baidu.com, used as the UDP payload: a public resolver
// answers it deterministically, so "did UDP work" has an unambiguous answer.
const uint8_t kDnsQuery[] = {
    0x12, 0x34,              // transaction id
    0x01, 0x00,              // standard query, recursion desired
    0x00, 0x01,              // one question
    0x00, 0x00, 0x00, 0x00,  // no answer, authority or additional records
    0x00, 0x00,
    0x03, 'w', 'w', 'w',     // qname
    0x05, 'b', 'a', 'i', 'd', 'u',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01,              // type A
    0x00, 0x01,              // class IN
};

std::string Http10Request(const std::string& host) {
  return "GET / HTTP/1.0\r\nHost: " + host + "\r\n\r\n";
}

void ScenarioReset(Probe& probe) {
  probe.Section("what is the module holding before anything is asked of it");
  probe.Step("echo off", "ATE0", 400);
  probe.Step("status", "AT+CIPSTATUS", 1500);
  probe.Step("read back the mux", "AT+CIPMUX?", 600);

  probe.Section("can a whole-stack reset clear it");
  probe.Step("reset the ip stack", "AT+CIPSHUT", 5000);
  probe.Step("status", "AT+CIPSTATUS", 1500);
  probe.Step("read back the mux", "AT+CIPMUX?", 600);

  probe.Section("now that the stack is clean, does the mux move");
  probe.Step("multi-socket", "AT+CIPMUX=1", 1200);
  probe.Step("read back the mux", "AT+CIPMUX?", 600);
  probe.Step("single-socket", "AT+CIPMUX=0", 1200);
  probe.Step("read back the mux", "AT+CIPMUX?", 600);
  probe.Step("multi-socket again", "AT+CIPMUX=1", 1200);
  probe.Step("read back the mux", "AT+CIPMUX?", 600);

  probe.Section("does the reset survive an open socket");
  probe.Step("connect", "AT+CIPSTART=0,\"TCP\",\"www.baidu.com\",80", 5000);
  probe.Step("reset while a socket is up", "AT+CIPSHUT", 5000);
  probe.Step("status", "AT+CIPSTATUS", 1500);
}

void ScenarioTcp(Probe& probe, const std::string& host, int port) {
  probe.CleanSlate();
  probe.EnterDataModes();

  probe.Section("open");
  probe.Step("connect",
             "AT+CIPSTART=0,\"TCP\",\"" + host + "\"," + std::to_string(port),
             4000);

  probe.Section("send: the prompt's framing is the point");
  const std::string request = Http10Request(host);
  probe.Step("announce length",
             "AT+CIPSEND=0," + std::to_string(request.size()), 1200);
  probe.Payload("payload", request.data(), request.size(), 4000);

  probe.Section("read: is data pushed, and what does a pull return?");
  probe.ReadRounds(0, 4);

  probe.Section("close");
  probe.Close(0);
}

void ScenarioState(Probe& probe) {
  probe.CleanSlate();
  probe.EnterDataModes();

  probe.Section("ask about a cid that was never opened");
  // The HAL needs one question whose answer distinguishes "free" from "taken",
  // and AT+CIPSTATUS cannot be it: its OK arrives before the table, so a
  // synchronous reader returns with the table still in flight.
  probe.Step("unread count on a free cid", "AT+CIPRXGET=4,3", 800);
  probe.Step("read on a free cid", "AT+CIPRXGET=3,3,64", 1500);
  probe.Step("unread count on another free cid", "AT+CIPRXGET=4,5", 800);
  probe.Step("close a cid that was never opened", "AT+CIPCLOSE=3", 1500);

  probe.Section("open, then ask again about the same cid");
  probe.Step("connect", "AT+CIPSTART=3,\"TCP\",\"www.baidu.com\",80", 4000);
  probe.Step("unread count on a taken cid", "AT+CIPRXGET=4,3", 800);
  probe.Step("status while one link is up", "AT+CIPSTATUS", 1500);

  probe.Section("open on a cid the module has already occupied");
  probe.Step("connect again on the same cid",
             "AT+CIPSTART=3,\"TCP\",\"www.baidu.com\",80", 4000);

  probe.Section("open where the peer cannot be reached");
  probe.Step("connect to an unroutable address",
             "AT+CIPSTART=4,\"TCP\",\"192.0.2.1\",80", 12000);

  probe.Section("open where the name does not resolve");
  probe.Step("connect to a name that does not exist",
             "AT+CIPSTART=5,\"TCP\",\"no-such-host.invalid\",80", 12000);

  probe.Section("clean up");
  probe.Close(3);
  probe.Close(4);
  probe.Close(5);
}

void ScenarioReadLen(Probe& probe) {
  probe.CleanSlate();
  probe.EnterDataModes();

  probe.Step("connect", "AT+CIPSTART=0,\"TCP\",\"www.baidu.com\",80", 6000);
  const std::string request = Http10Request("www.baidu.com");
  probe.Step("announce length",
             "AT+CIPSEND=0," + std::to_string(request.size()), 1200);
  probe.Payload("payload", request.data(), request.size(), 8000);

  probe.Section("how much will one read hand back");
  // AT+CIPRXGET=? advertises (1-1460), but a read of 1024 was refused on a
  // socket holding 2820 bytes while 512 was served. Walk down from the
  // advertised ceiling to find what this firmware actually honours. A refused
  // read consumes nothing, so one socket's buffer serves the whole walk - and
  // a served read only shortens it, leaving the rest for the lengths below.
  //
  // First pass: 1460, 1024 and 800 refused; 600, 513 and 512 served.
  // Second pass: 768 refused; 700, 640, 620, 610 and 601 served.
  // This pass closes the last 67 bytes of the gap.
  probe.Step("unread count", "AT+CIPRXGET=4,0", 800);
  for (int len : {767, 750, 720, 701}) {
    probe.Step("read " + std::to_string(len),
               "AT+CIPRXGET=3,0," + std::to_string(len), 3000);
    probe.Step("unread count", "AT+CIPRXGET=4,0", 600);
  }

  probe.Section("can a read be issued without asking how much is waiting?");
  // If a read on an open socket with nothing queued answers with a count of
  // zero rather than an error, the reader loop can drop the count query
  // entirely and halve its round trips per transfer.
  probe.Step("close", "AT+CIPCLOSE=0", 2000);
  probe.Step("connect a fresh socket",
             "AT+CIPSTART=1,\"TCP\",\"www.baidu.com\",80", 6000);
  probe.Step("read with nothing queued", "AT+CIPRXGET=3,1,512", 2500);
  probe.Step("count with nothing queued", "AT+CIPRXGET=4,1", 800);
  probe.Step("close", "AT+CIPCLOSE=1", 2000);
}

void ScenarioRecycle(Probe& probe) {
  probe.CleanSlate();
  probe.EnterDataModes();

  probe.Section("a cid that closed cleanly, used again at once");
  probe.Step("connect", "AT+CIPSTART=0,\"TCP\",\"www.baidu.com\",80", 6000);
  probe.Step("connect again while it is held",
             "AT+CIPSTART=0,\"TCP\",\"www.baidu.com\",80", 3000);
  probe.Step("close", "AT+CIPCLOSE=0", 3000);
  // The pool hands a cid straight back out, so the interesting question is
  // whether the module agrees that a just-closed cid is free.
  probe.Step("reopen immediately after the close",
             "AT+CIPSTART=0,\"TCP\",\"www.baidu.com\",80", 6000);
  probe.Step("status", "AT+CIPSTATUS", 1500);
  probe.Step("close", "AT+CIPCLOSE=0", 3000);

  probe.Section("a cid whose connect failed, used again");
  probe.Step("connect to a name that does not exist",
             "AT+CIPSTART=1,\"TCP\",\"no-such-host.invalid\",80", 14000);
  probe.Step("status after the failure", "AT+CIPSTATUS", 1500);
  // A CONNECT FAIL leaves the cid in some state; whether the module will take a
  // fresh AT+CIPSTART on it decides if the pool may hand it out again.
  probe.Step("reuse the failed cid",
             "AT+CIPSTART=1,\"TCP\",\"www.baidu.com\",80", 6000);
  probe.Step("close", "AT+CIPCLOSE=1", 3000);

  probe.Section("a cid the peer closed, watched for a notification");
  probe.Step("connect", "AT+CIPSTART=2,\"TCP\",\"www.baidu.com\",80", 6000);

  // HEAD, not GET: a GET of this host's front page answers with 29506 bytes, so
  // the socket would still be receiving when the watch below starts and the peer
  // would not have closed yet. A HEAD response is a few hundred bytes, which
  // drains in one read and leaves a socket the server then closes.
  const std::string request = "HEAD / HTTP/1.0\r\nHost: www.baidu.com\r\n\r\n";
  probe.Step("announce length",
             "AT+CIPSEND=2," + std::to_string(request.size()), 1200);
  probe.Payload("payload", request.data(), request.size(), 4000);

  probe.Section("read the headers, then wait to be told the peer has gone");
  for (int round = 0; round < 4; ++round) {
    probe.Step("unread count", "AT+CIPRXGET=4,2", 600);
    probe.Step("read", "AT+CIPRXGET=3,2," + std::to_string(kReadChunk), 2000);
  }
  const bool saw_close =
      probe.Drain(20000, "a close notification after the body was drained");
  if (!saw_close) {
    std::printf(
        "  ... nothing arrived: a peer close may be reported only on the next\n"
        "      attempted operation, which the queries below test\n");
  }

  probe.Section("what the module says now that the peer has gone");
  probe.Step("unread count after the peer closed", "AT+CIPRXGET=4,2", 1500);
  probe.Step("read after the peer closed", "AT+CIPRXGET=3,2,64", 2000);
  probe.Step("status after the peer closed", "AT+CIPSTATUS", 1500);
  probe.Step("send after the peer closed", "AT+CIPSEND=2,4", 1500);
  probe.Payload("payload", "ping", 4, 2000);

  probe.Section("a cid the peer closed, used again");
  probe.Step("close", "AT+CIPCLOSE=2", 3000);
  probe.Step("reopen", "AT+CIPSTART=2,\"TCP\",\"www.baidu.com\",80", 6000);
  probe.Step("close", "AT+CIPCLOSE=2", 3000);
}

void ScenarioPush(Probe& probe) {
  probe.CleanSlate();
  probe.EnterDataModes(/*report_every_arrival=*/false);

  probe.Section("let the module hand data over on its own");
  probe.Step("auto receive", "AT+CIPRXGET=0", 400);
  probe.Step("read back the receive mode", "AT+CIPRXGET?", 400);
  probe.Step("connect", "AT+CIPSTART=0,\"TCP\",\"www.baidu.com\",80", 4000);

  const std::string request = Http10Request("www.baidu.com");
  probe.Step("announce length",
             "AT+CIPSEND=0," + std::to_string(request.size()), 1200);
  probe.Payload("payload", request.data(), request.size(), 3000);

  // This is the measurement the whole read design turns on: if the body arrives
  // as bare bytes with no length boundary, a line-oriented AT layer cannot frame
  // it, and pulling with AT+CIPRXGET=3 is the only option.
  probe.Section("what the module pushed");
  probe.Drain(8000, "pushed data");
  probe.Step("status", "AT+CIPSTATUS", 1500);
  probe.Close(0);
}

void ScenarioTls(Probe& probe) {
  probe.CleanSlate();
  probe.EnterDataModes();

  probe.Section("the configuration surface");
  probe.Step("per-socket TLS switch", "AT+CIPSSL?", 800);
  probe.Step("per-socket TLS switch params", "AT+CIPSSL=?", 800);
  probe.Step("ssl configuration table", "AT+SSLCFG?", 1500);
  probe.Step("ssl configuration params", "AT+SSLCFG=?", 800);
  probe.Step("installed certificates", "AT+CCERTLIST", 1500);

  probe.Section("can a switch be written, and does it stick?");
  probe.Step("switch socket 0 to TLS", "AT+CIPSSL=0,1", 1000);
  probe.Step("per-socket TLS switch", "AT+CIPSSL?", 800);
  probe.Step("ssl configuration table", "AT+SSLCFG?", 1500);

  probe.Section("open a TLS port with the switch set");
  probe.Step("connect", "AT+CIPSTART=0,\"TCP\",\"www.baidu.com\",443", 6000);
  probe.Step("status", "AT+CIPSTATUS", 1500);

  probe.Section("is the session actually encrypted?");
  // A plaintext request over a socket the module claims is TLS can only get a
  // sensible answer if the module did the handshake and encrypted this. A
  // refusal, a reset, or silence means the switch did not take.
  const std::string request = Http10Request("www.baidu.com");
  probe.Step("announce length",
             "AT+CIPSEND=0," + std::to_string(request.size()), 1500);
  probe.Payload("payload", request.data(), request.size(), 4000);
  probe.ReadRounds(0, 3);

  probe.Close(0);
  probe.Step("switch socket 0 back to plain", "AT+CIPSSL=0,0", 800);
}

void ScenarioTlsApi(Probe& probe) {
  probe.CleanSlate();

  // The obvious family came back empty: AT+SSLCFG? answers with a bare OK and
  // no table, AT+CCERTLIST is not a command at all, and AT+CIPSSL=0,1 is
  // refused. AirM2M firmware of this generation is documented to put TLS behind
  // a different family, so this walks every candidate before concluding the
  // module cannot do it - a bare OK means "not understood" as often as it means
  // "nothing to report", and the two are worth separating.
  struct Query {
    const char* label;
    const char* command;
  };
  static const Query kQueries[] = {
      {"the plain switch, global form", "AT+CIPSSL=1"},
      {"the plain switch", "AT+CIPSSL?"},
      {"the reported switch range", "AT+CIPSSL=?"},
      {"the ssl socket family, settings", "AT+CSSLCFG?"},
      {"the ssl socket family, settings range", "AT+CSSLCFG=?"},
      {"the ssl socket family, open", "AT+CCHOPEN=?"},
      {"the ssl socket family, send", "AT+CCHSEND=?"},
      {"the ssl socket family, close", "AT+CCHCLOSE=?"},
      {"the ssl socket family, status", "AT+CCHSTATUS?"},
      {"the certificate list", "AT+CCERTLIST?"},
      {"the certificate count", "AT+CCERTCNT?"},
      {"whether a certificate can be written", "AT+CCERTDOWN=?"},
      {"the older ssl table", "AT+SSLCFG?"},
      {"the older ssl table range", "AT+SSLCFG=?"},
  };
  for (const auto& query : kQueries) {
    probe.Step(query.label, query.command, 900);
  }

  probe.Section("write forms, so a table that is empty is not read as absent");
  static const Query kWrites[] = {
      {"set the tls version", "AT+CSSLCFG=\"sslversion\",0,4"},
      {"set the auth level", "AT+CSSLCFG=\"authmode\",0,0"},
      {"set the tls version, older spelling", "AT+SSLCFG=\"sslversion\",0,4"},
  };
  for (const auto& write : kWrites) {
    probe.Step(write.label, write.command, 900);
  }
  probe.Step("the ssl socket family, settings", "AT+CSSLCFG?", 1200);
  probe.Step("the older ssl table", "AT+SSLCFG?", 1200);
}

void ScenarioTlsUse(Probe& probe, const std::string& host, int port) {
  probe.CleanSlate();
  probe.EnterDataModes();

  probe.Section("set the switch the way this firmware accepts it");
  // AT+CIPSSL takes one argument here, and AT+CIPSSL? reads the value back, so
  // the switch is real. Whether it applies to one socket or to all of them only
  // shows up in what a second socket does, which is the last section below.
  probe.Step("the tls version", "AT+SSLCFG=\"sslversion\",0,4", 900);
  probe.Step("turn tls on", "AT+CIPSSL=1", 900);
  probe.Step("read it back", "AT+CIPSSL?", 900);

  probe.Section("open an https port with the switch on");
  probe.Step("connect",
             "AT+CIPSTART=0,\"TCP\",\"" + host + "\"," +
                 std::to_string(port),
             15000);

  probe.Section("send a plaintext request and see if it comes back readable");
  // The decisive test. If the module really is doing TLS, it encrypts these
  // bytes, the server answers, and the module hands the reply back in the
  // clear. If the switch did nothing, this is a plaintext request into a TLS
  // port: the server drops it and nothing readable ever arrives.
  const std::string request = Http10Request(host);
  probe.Step("announce length",
             "AT+CIPSEND=0," + std::to_string(request.size()), 1500);
  probe.Payload("payload", request.data(), request.size(), 6000);
  probe.ReadRounds(0, 3);

  probe.Step("close", "AT+CIPCLOSE=0", 3000);

  probe.Section("is the switch per socket or for all of them");
  // A plain socket opened while the switch is still on tells us which. This
  // decides whether TcpConnect can honour a per-connection TlsConfig at all.
  probe.Step("connect a plain port with tls still on",
             "AT+CIPSTART=1,\"TCP\",\"" + host + "\",80", 6000);
  const std::string plain = Http10Request(host);
  probe.Step("announce length",
             "AT+CIPSEND=1," + std::to_string(plain.size()), 1500);
  probe.Payload("payload", plain.data(), plain.size(), 6000);
  probe.Step("unread count", "AT+CIPRXGET=4,1", 900);
  probe.Step("read", "AT+CIPRXGET=3,1," + std::to_string(kReadChunk), 2500);
  probe.Step("close", "AT+CIPCLOSE=1", 3000);

  probe.Step("turn tls off", "AT+CIPSSL=0", 900);
  probe.Step("read it back", "AT+CIPSSL?", 900);
}

// Does a peer close throw away what is still in the module's buffer, or does the
// buffer stay readable afterwards? This decides whether "<id>, CLOSED" may be
// treated as the end of the stream immediately, or whether the reader has to
// drain to empty first and only then report the close.
void ScenarioCloseDrain(Probe& probe, const std::string& host, int port) {
  probe.CleanSlate();
  probe.EnterDataModes();

  probe.Section("open a plain socket");
  probe.Step("connect",
             "AT+CIPSTART=0,\"TCP\",\"" + host + "\"," +
                 std::to_string(port),
             10000);

  probe.Section("ask for something bigger than one read");
  const std::string request = Http10Request(host);
  probe.Step("announce length",
             "AT+CIPSEND=0," + std::to_string(request.size()), 1500);
  probe.Payload("payload", request.data(), request.size(), 4000);

  probe.Section("read once, leaving data behind");
  probe.Step("wait for the arrival", "AT+CIPRXGET=4,0", 4000);
  probe.Step("read 512", "AT+CIPRXGET=3,0,512", 3000);

  probe.Section("let the peer close while data is still buffered");
  probe.Step("idle", "AT", 4000);
  probe.Step("idle", "AT", 4000);
  probe.Step("idle", "AT", 4000);

  probe.Section("is anything still readable?");
  probe.Step("unread count", "AT+CIPRXGET=4,0", 1500);
  probe.Step("read 512", "AT+CIPRXGET=3,0,512", 3000);
  probe.Step("unread count", "AT+CIPRXGET=4,0", 1500);
  probe.Step("read 512", "AT+CIPRXGET=3,0,512", 3000);

  probe.Section("is a fresh connect on the same cid allowed?");
  probe.Step("reconnect",
             "AT+CIPSTART=0,\"TCP\",\"" + host + "\"," +
                 std::to_string(port),
             10000);

  probe.Restore();
}

// Over TLS, does the module hand back the whole body, and does it say the peer
// closed while bytes are still queued? The plain-text answer was measured by
// `closedrain`: CLOSED is withheld until the buffer is drained. This asks the
// same question of a socket that is decrypting, because the software HTTP client
// fails on exactly this path with "connection closed before the response was
// complete" - which is a close arriving, not a read stalling.
void ScenarioTlsRead(Probe& probe, const std::string& host, int port,
                     const std::string& path, bool count_each_round = true) {
  probe.CleanSlate();
  probe.EnterDataModes();

  probe.Section("the switch this firmware accepts");
  probe.Step("the tls version", "AT+SSLCFG=\"sslversion\",0,4", 900);
  probe.Step("turn tls on", "AT+CIPSSL=1", 900);
  probe.Step("read it back", "AT+CIPSSL?", 900);

  probe.Section("open the tls port");
  probe.Step("connect",
             "AT+CIPSTART=0,\"TCP\",\"" + host + "\"," + std::to_string(port),
             15000);

  probe.Section("ask for the page");
  const std::string request = "GET " + path + " HTTP/1.0\r\nHost: " + host +
                              "\r\n\r\n";
  probe.Step("announce length", "AT+CIPSEND=0," + std::to_string(request.size()),
             1500);
  probe.Payload("payload", request.data(), request.size(), 6000);

  probe.Section("read until the socket runs dry, watching the count as well");
  // The count matters as much as the read: a read of length 0 while
  // AT+CIPRXGET=4 still reports bytes queued is the module stranding data, and a
  // read that stops on an error is the cid having gone. Both look the same from
  // above - a body that ends early - and only the pair tells them apart.
  const std::string chunk = std::to_string(kReadChunk);
  size_t total = 0;
  for (int round = 0; round < 90; ++round) {
    std::printf("\n--- round %d, %zu byte(s) so far ---\n", round + 1, total);
    std::fflush(stdout);
    // The count query is a whole round trip, and a round trip is time the module
    // can spend tearing the socket down. Dropping it is how this asks whether the
    // tail of a page can be read at all when the peer closes in the middle of it.
    if (count_each_round) probe.Step("unread count", "AT+CIPRXGET=4,0", 400);
    probe.Step("read", "AT+CIPRXGET=3,0," + chunk, 700);
    total += kReadChunk;
  }

  probe.Section("what is left afterwards");
  probe.Step("unread count", "AT+CIPRXGET=4,0", 900);
  probe.Step("one more read", "AT+CIPRXGET=3,0," + chunk, 1500);
  probe.Step("unread count", "AT+CIPRXGET=4,0", 900);
  probe.Step("is the cid writable still", "AT+CIPSEND=0,4", 1500);

  probe.Restore();
}

void ScenarioUdp(Probe& probe) {
  probe.CleanSlate();
  probe.EnterDataModes();

  probe.Section("ask for the source address of each datagram");
  probe.Step("report the source", "AT+CIPSRIP=1", 600);
  probe.Step("read it back", "AT+CIPSRIP?", 600);

  probe.Section("open a UDP socket against a public resolver");
  probe.Step("connect", "AT+CIPSTART=0,\"UDP\",\"114.114.114.114\",53", 4000);

  probe.Section("send a DNS query");
  probe.Step("announce length",
             "AT+CIPSEND=0," + std::to_string(sizeof(kDnsQuery)), 1200);
  probe.Payload("dns query", kDnsQuery, sizeof(kDnsQuery), 4000);

  probe.Section("read the answer");
  probe.ReadRounds(0, 2);

  probe.Section("can the module name the peer without AT+CIPSRIP?");
  probe.Step("report the source", "AT+CIPSRIP=0", 600);
  probe.Close(0);
  probe.Step("connect again", "AT+CIPSTART=0,\"UDP\",\"114.114.114.114\",53", 4000);
  probe.Step("announce length",
             "AT+CIPSEND=0," + std::to_string(sizeof(kDnsQuery)), 1200);
  probe.Payload("dns query", kDnsQuery, sizeof(kDnsQuery), 4000);
  probe.ReadRounds(0, 2);
  probe.Close(0);
}

void ScenarioMisc(Probe& probe) {
  probe.CleanSlate();

  probe.Section("identity");
  probe.Step("model", "AT+CGMR", 800);
  probe.Step("imei, quoted", "AT+CGSN=1", 800);
  probe.Step("imei, bare", "AT+CGSN", 800);
  probe.Step("iccid", "AT+ICCID", 800);
  probe.Step("sim", "AT+CPIN?", 800);

  probe.Section("network");
  probe.Step("signal", "AT+CSQ", 800);
  probe.Step("registration", "AT+CEREG?", 800);
  probe.Step("operator", "AT+COPS?", 3000);
  probe.Step("attached", "AT+CGATT?", 1500);

  probe.Section("apn");
  probe.Step("pdp contexts", "AT+CGDCONT?", 1500);
  probe.Step("activation state", "AT+CGACT?", 1500);
  probe.Step("the cstt form", "AT+CSTT?", 800);
  probe.Step("the cstt params", "AT+CSTT=?", 800);

  probe.Section("power and rate");
  probe.Step("functionality", "AT+CFUN?", 1200);
  probe.Step("sleep modes", "AT+CSCLK=?", 800);
  probe.Step("sleep mode now", "AT+CSCLK?", 800);
  probe.Step("rate now", "AT+IPR?", 800);
  probe.Step("rates supported", "AT+IPR=?", 1500);
  probe.Step("the cipsend ack mode", "AT+CIPQSEND?", 800);
  probe.Step("the receive mode", "AT+CIPRXGET?", 800);
  probe.Step("whether a header is added", "AT+CIPHEAD?", 800);
  probe.Step("the mux", "AT+CIPMUX?", 800);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string port = argc > 1 ? argv[1] : "COM4";
  const std::string scenario = argc > 2 ? argv[2] : "tcp";
  const int baud = argc > 3 ? std::atoi(argv[3]) : 115200;

  UartConfig config;
  config.device = port;
  config.baud_rate = baud;

  auto uart = CreateSerialUart(config);
  if (!uart) {
    std::printf("Could not open %s\n", port.c_str());
    return 1;
  }

  Probe probe(*uart);
  std::printf("Air780E probe on %s at %d baud, scenario '%s'\n", port.c_str(),
              baud, scenario.c_str());

  if (scenario == "tcp") {
    ScenarioTcp(probe, "www.baidu.com", 80);
  } else if (scenario == "state") {
    ScenarioState(probe);
  } else if (scenario == "recycle") {
    ScenarioRecycle(probe);
  } else if (scenario == "readlen") {
    ScenarioReadLen(probe);
  } else if (scenario == "push") {
    ScenarioPush(probe);
  } else if (scenario == "tls") {
    ScenarioTls(probe);
  } else if (scenario == "tlsapi") {
    ScenarioTlsApi(probe);
  } else if (scenario == "tlsuse") {
    ScenarioTlsUse(probe, argc > 4 ? argv[4] : "www.baidu.com",
                   argc > 5 ? std::atoi(argv[5]) : 443);
  } else if (scenario == "closedrain") {
    ScenarioCloseDrain(probe, argc > 4 ? argv[4] : "www.baidu.com",
                       argc > 5 ? std::atoi(argv[5]) : 80);
  } else if (scenario == "tlsread") {
    ScenarioTlsRead(probe, argc > 4 ? argv[4] : "www.baidu.com",
                    argc > 5 ? std::atoi(argv[5]) : 443,
                    argc > 6 ? argv[6] : "/");
  } else if (scenario == "tlsfast") {
    // Same read, with the count query left out so the loop issues nothing but
    // reads. See the note in ScenarioTlsRead.
    ScenarioTlsRead(probe, argc > 4 ? argv[4] : "www.baidu.com",
                    argc > 5 ? std::atoi(argv[5]) : 443,
                    argc > 6 ? argv[6] : "/", false);
  } else if (scenario == "udp") {
    ScenarioUdp(probe);
  } else if (scenario == "reset") {
    ScenarioReset(probe);
  } else if (scenario == "misc") {
    ScenarioMisc(probe);
  } else {
    std::printf("Unknown scenario '%s'\n", scenario.c_str());
    return 2;
  }

  probe.Restore();
  return 0;
}
