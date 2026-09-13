#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mock_at_channel.h"
#include "modules/air780e/air780e_hal.h"

using namespace esp_modem_link;
using namespace esp_modem_link::modules::air780e;
using esp_modem_link::testing::MockAtChannel;

namespace {

constexpr const char* kCgmrAir780e =
    "+CGMR: \"AirM2M_780EPV_V1004_LTE_AT\"\r\nOK\r\n";

// Programs the sequence Initialize runs through. Every one of these is a fact
// about the module rather than a convenience: AT+CIPSHUT is answered with an
// event ("SHUT OK") instead of a response line, AT+CIPMUX has to be read back
// because a leftover cid makes the module refuse the change silently, and the
// SSL switch is read back for the same reason - it fails later, at the
// handshake, where it would read as a network problem.
void ExpectInit(MockAtChannel& channel) {
  channel.ExpectCommand("ATE0").Respond("ATE0\r\nOK\r\n");
  channel.ExpectCommand("AT+CFUN=1").Respond("AT+CFUN=1\r\nOK\r\n");
  channel.ExpectCommand("AT+CIPSHUT").ThenUrc("SHUT OK\r\n");
  channel.ExpectCommand("AT+CIPMUX=1").Respond("OK\r\n");
  channel.ExpectCommand("AT+CIPMUX?").Respond("+CIPMUX: 1\r\n\r\nOK\r\n");
  channel.ExpectCommand("AT+CIPRXGET=5").Respond("OK\r\n");
  channel.ExpectCommand("AT+CIPSRIP=1").Respond("OK\r\n");
  channel.ExpectCommand("AT+CIPSSL=0").Respond("OK\r\n");
  channel.ExpectCommand("AT+CIPSSL?").Respond("+CIPSSL: 0\r\n\r\nOK\r\n");
}

// One read reply: the header naming the length, then the payload as hex. Shape
// measured on the module, including the blank line before OK.
std::string ReadReply(int id, int len, std::string_view source = {}) {
  std::string header = "+CIPRXGET: 3," + std::to_string(id) + "," +
                       std::to_string(len) + ",0";
  if (!source.empty()) {
    header += ",\"" + std::string(source) + "\"";
  }
  if (len == 0) {
    // An empty socket answers the header alone - there is no payload line.
    return header + "\r\n\r\nOK\r\n";
  }
  std::string hex;
  hex.reserve(static_cast<size_t>(len) * 2);
  for (int i = 0; i < len; ++i) hex += "AB";
  return header + "\r\n" + hex + "\r\n\r\nOK\r\n";
}

// A read that returns `len` bytes, repeated for every read after it.
std::string ReadsOf(int id, int len) { return ReadReply(id, len); }

// Position of the first command starting with `prefix`, or SentCommands().size()
// when it was never sent. The order these go out in is part of the module's
// contract: AT+CIPSHUT resets every cid to INITIAL, and a cid still in CLOSED
// makes AT+CIPMUX refuse the change, so the reset has to come first.
size_t CommandIndex(const MockAtChannel& channel, std::string_view prefix) {
  const auto& commands = channel.SentCommands();
  for (size_t i = 0; i < commands.size(); ++i) {
    if (commands[i].rfind(prefix, 0) == 0) return i;
  }
  return commands.size();
}

class Air780eHalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // The receive task stays unstarted: it wakes on a timer to pull the same
    // sockets, so a test that wants to assert on what has been delivered calls
    // DrainPending() and owns the timing itself.
    hal_ = std::make_unique<Air780eHal>(Air780eHal::Options{.start_reader = false});
    ExpectInit(channel_);
    ASSERT_TRUE(hal_->Initialize(channel_).has_value());
    channel_.Reset();
  }

  // Opens a TCP socket and returns the cid the allocator chose. The outcome is
  // scripted as a URC because that is how the module reports it - it is not a
  // line in AT+CIPSTART's own response.
  int OpenTcp(std::string_view host = "example.com", uint16_t port = 80) {
    channel_.ExpectCommand("AT+CIPSTART")
        .ThenUrcFrom([](const std::string& cmd) {
          // AT+CIPSTART=<id>,"TCP","host",<port>
          size_t begin = cmd.find('=') + 1;
          size_t end = cmd.find(',', begin);
          return cmd.substr(begin, end - begin) + ", CONNECT OK\r\n";
        });
    auto id = hal_->TcpConnect(host, port);
    EXPECT_TRUE(id.has_value());
    return id ? *id : -1;
  }

  MockAtChannel channel_;
  std::unique_ptr<Air780eHal> hal_;
};

// --- Identification ---

TEST_F(Air780eHalTest, Identity) {
  EXPECT_EQ(hal_->GetModuleType(), ModuleType::kAir780E);
  EXPECT_EQ(hal_->GetModuleName(), "AIR780E");
}

TEST_F(Air780eHalTest, Capabilities) {
  const auto& caps = hal_->GetCapabilities();
  EXPECT_TRUE(caps.tcp);
  EXPECT_TRUE(caps.udp);
  EXPECT_TRUE(caps.ssl_tcp);
  EXPECT_TRUE(caps.http);
  EXPECT_TRUE(caps.mqtt);
  EXPECT_TRUE(caps.low_power);
  // Six, per AT+CIPMUX=? reporting (0-5).
  EXPECT_EQ(caps.max_connections, 6);
  EXPECT_EQ(caps.max_baud_rate, 921600);
  // The module has stacks of its own, but this HAL cannot build a client on
  // them, and Auto mode reads these flags as "the builtin path will work".
  EXPECT_FALSE(hal_->HasBuiltinHttp());
  EXPECT_FALSE(hal_->HasBuiltinMqtt());
}

// --- Initialize ---

TEST(Air780eHalInitTest, SendsInitSequenceInOrder) {
  MockAtChannel channel;
  ExpectInit(channel);

  Air780eHal hal(Air780eHal::Options{.start_reader = false});
  ASSERT_TRUE(hal.Initialize(channel).has_value());

  // The reset comes before the mode change, and the reset is the only thing that
  // can be relied on to clear a cid left behind by an earlier process.
  EXPECT_LT(CommandIndex(channel, "AT+CIPSHUT"),
            CommandIndex(channel, "AT+CIPMUX=1"));
  // Manual receive has to be on before the reader can be told about an arrival.
  EXPECT_LT(CommandIndex(channel, "AT+CIPRXGET=5"),
            CommandIndex(channel, "AT+CIPSRIP=1"));
  EXPECT_TRUE(channel.WasCommandSent("AT+CFUN=1"));
  EXPECT_TRUE(channel.WasCommandSent("AT+CIPSSL=0"));
}

TEST(Air780eHalInitTest, FailsWhenCipmuxDoesNotTake) {
  MockAtChannel channel;
  ExpectInit(channel);
  // What a module with a cid still in CLOSED answers: it takes the command, or
  // appears to, and the mode stays where it was.
  channel.ExpectCommand("AT+CIPMUX?").Respond("+CIPMUX: 0\r\n\r\nOK\r\n");

  Air780eHal hal(Air780eHal::Options{.start_reader = false});
  auto r = hal.Initialize(channel);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, NetworkErrc::kProtocolError);
}

TEST(Air780eHalInitTest, FailsWhenCipshutIsNotConfirmed) {
  MockAtChannel channel;
  ExpectInit(channel);
  // The transmit fails outright, which is the case a caller can distinguish from
  // the module simply never answering SHUT OK.
  channel.FailCommand("AT+CIPSHUT", AtErrc::kTimeout);

  Air780eHal hal(Air780eHal::Options{.start_reader = false});
  EXPECT_FALSE(hal.Initialize(channel).has_value());
}

TEST(Air780eHalInitTest, FailsWhenTheSslSwitchDoesNotTake) {
  MockAtChannel channel;
  ExpectInit(channel);
  // A switch that did not take would not fail here - it would surface as CONNECT
  // FAIL on every TLS socket from then on, which reads as a network problem.
  channel.ExpectCommand("AT+CIPSSL?").Respond("+CIPSSL: 1\r\n\r\nOK\r\n");

  Air780eHal hal(Air780eHal::Options{.start_reader = false});
  auto r = hal.Initialize(channel);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, NetworkErrc::kProtocolError);
}

// --- Detection ---

TEST(Air780eDetectTest, AcceptsThisVendorsFirmware) {
  MockAtChannel channel;
  channel.ExpectCommand("AT").Respond("OK\r\n");
  channel.ExpectCommand("AT+CGMR").Respond(kCgmrAir780e);
  EXPECT_TRUE(DetectAir780e(channel));
}

TEST(Air780eDetectTest, RejectsAnotherModule) {
  MockAtChannel channel;
  channel.ExpectCommand("AT").Respond("OK\r\n");
  channel.ExpectCommand("AT+CGMR").Respond("+CGMR: ML307R-DNLM_V1.0.0\r\nOK\r\n");
  EXPECT_FALSE(DetectAir780e(channel));
}

TEST(Air780eDetectTest, RejectsAModuleThatDoesNotAnswer) {
  MockAtChannel channel;
  channel.FailCommand("AT", AtErrc::kTimeout);
  EXPECT_FALSE(DetectAir780e(channel));
}

// --- Connecting ---

TEST_F(Air780eHalTest, ConnectSendsTheDocumentedCommandAndReturnsTheCid) {
  channel_.ExpectCommand("AT+CIPSTART").ThenUrc("0, CONNECT OK\r\n");

  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, 0);
  EXPECT_TRUE(channel_.WasCommandSent("AT+CIPSTART=0,\"TCP\",\"example.com\",80"));
}

TEST_F(Air780eHalTest, ConnectMovesToAnotherCidWhenOneIsHeldByAnotherProcess) {
  // What a cid left open by an earlier run looks like: the code and then the
  // event, both naming that cid.
  channel_.ExpectCommand("AT+CIPSTART")
      .ThenUrc("0, ALREADY CONNECT\r\n");
  channel_.ExpectCommand("AT+CIPSTART").ThenUrc("1, CONNECT OK\r\n");

  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, 1) << "the cid the module still holds is not ours to take";
}

TEST_F(Air780eHalTest, ConnectReportsAFailureTheModuleNames) {
  channel_.ExpectCommand("AT+CIPSTART").ThenUrc("0, CONNECT FAIL\r\n");

  auto id = hal_->TcpConnect("no-such-host.invalid", 80);
  ASSERT_FALSE(id.has_value());
  EXPECT_EQ(id.error().code, NetworkErrc::kConnectFailed);
}

TEST_F(Air780eHalTest, ConnectRefusesMoreThanTheModuleHasCids) {
  const auto& caps = hal_->GetCapabilities();
  std::vector<int> ids;
  for (int i = 0; i < caps.max_connections; ++i) {
    // One queued outcome per connect: each matching command consumes one, so a
    // test that opens several sockets has to script several.
    channel_.ExpectCommand("AT+CIPSTART")
        .ThenUrcFrom([](const std::string& cmd) {
          const size_t begin = cmd.find('=') + 1;
          return cmd.substr(begin, cmd.find(',', begin) - begin) + ", CONNECT OK\r\n";
        });
    auto id = hal_->TcpConnect("example.com", 80);
    ASSERT_TRUE(id.has_value()) << "socket " << i;
    ids.push_back(*id);
  }

  auto extra = hal_->TcpConnect("example.com", 80);
  ASSERT_FALSE(extra.has_value());
  EXPECT_EQ(extra.error().code, NetworkErrc::kNoResources);
}

// --- Receiving ---

TEST_F(Air780eHalTest, DrainsUntilTheSocketIsEmpty) {
  const int id = OpenTcp();

  std::string received;
  bool closed = false;
  hal_->SubscribeTcp(
      id, [&](int, std::string_view data) { received.append(data); },
      [&](int) { closed = true; });

  channel_.ExpectCommand("AT+CIPRXGET=3")
      .RespondInTurn({ReadReply(id, 4), ReadReply(id, 0)});

  channel_.InjectUrc("+CIPRXGET: 1," + std::to_string(id) + "\r\n");
  hal_->DrainPending();

  EXPECT_EQ(received.size(), 4u);
  EXPECT_FALSE(closed) << "an empty read is not a close";
}

TEST_F(Air780eHalTest, CapDoesNotStrandTheRemainderOfADeepBuffer) {
  const int id = OpenTcp();

  size_t received = 0;
  hal_->SubscribeTcp(id, [&](int, std::string_view data) { received += data.size(); },
                     [](int) {});

  // AT+CIPRXGET=5 announces an arrival once and never repeats it, so a pass that
  // stops at kMaxDrainPerWake has to put the flag back or the rest of the buffer
  // sits in the module with nothing left to wake the reader. 14 chunks of 720 is
  // 10080 bytes against a cap of 8192, so the cap is crossed twice over and the
  // last two chunks arrive only if the flag is re-armed.
  std::vector<std::string> replies;
  for (int i = 0; i < 14; ++i) replies.push_back(ReadsOf(id, 720));
  replies.push_back(ReadReply(id, 0));
  channel_.ExpectCommand("AT+CIPRXGET=3").RespondInTurn(std::move(replies));

  channel_.InjectUrc("+CIPRXGET: 1," + std::to_string(id) + "\r\n");
  hal_->DrainPending();

  EXPECT_EQ(received, 14u * 720u);
}

// The shared layer holds payload that arrived for a cid with no route yet, and
// only the HAL can say when that cid has been given back. This is the module-side
// half of that contract: a released cid must not carry its predecessor's payload
// into whatever takes the cid next, because the module names a cid and nothing
// else - the two generations are indistinguishable from the outside.
TEST_F(Air780eHalTest, AReleasedCidCarriesNoHeldPayloadIntoTheNextSocket) {
  const int id = OpenTcp();

  // Payload arrives for a cid nobody has subscribed to, so it is held.
  channel_.ExpectCommand("AT+CIPRXGET=3")
      .RespondInTurn({ReadReply(id, 4), ReadReply(id, 0)});
  channel_.InjectUrc("+CIPRXGET: 1," + std::to_string(id) + "\r\n");
  hal_->DrainPending();

  channel_.ExpectCommand("AT+CIPCLOSE").ThenUrcFrom([](const std::string& cmd) {
    return cmd.substr(cmd.find('=') + 1) + ", CLOSE OK\r\n";
  });
  ASSERT_TRUE(hal_->TcpClose(id).has_value());

  // The pool hands the freed cid straight back out, so the replacement lands on
  // the same one.
  const int reopened = OpenTcp();
  EXPECT_EQ(reopened, id);

  std::string received;
  hal_->SubscribeTcp(
      reopened, [&](int, std::string_view data) { received.append(data); },
      [](int) {});

  EXPECT_TRUE(received.empty())
      << "the retired socket's payload was handed to the socket that replaced it";
}

// --- A peer's close ---

TEST_F(Air780eHalTest, PeerCloseWaitsForTheDataInFrontOfIt) {
  const int id = OpenTcp();

  std::string received;
  bool closed = false;
  hal_->SubscribeTcp(
      id, [&](int, std::string_view data) { received.append(data); },
      [&](int) { closed = true; });

  // Measured on the module: a read came back with 720 bytes for this cid *after*
  // the close line. Ending the socket where the close arrives would deliver that
  // chunk to nobody and truncate the body one chunk early.
  channel_.ExpectCommand("AT+CIPRXGET=3")
      .RespondInTurn({ReadReply(id, 720), ReadReply(id, 0)});
  channel_.InjectUrc(std::to_string(id) + ", CLOSED\r\n");
  channel_.InjectUrc("+CIPRXGET: 1," + std::to_string(id) + "\r\n");

  hal_->DrainPending();

  EXPECT_EQ(received.size(), 720u) << "the bytes behind the close were dropped";
  EXPECT_TRUE(closed) << "the socket was never ended";
}

TEST_F(Air780eHalTest, AnUnrequestedCloseOkIsAlsoAPeerClose) {
  const int id = OpenTcp();

  bool closed = false;
  hal_->SubscribeTcp(id, [](int, std::string_view) {}, [&](int) { closed = true; });

  channel_.ExpectCommand("AT+CIPRXGET=3")
      .RespondInTurn({ReadsOf(id, 16), ReadReply(id, 0)});

  // This firmware reports a peer's close under either line, and on a TLS socket
  // this is the one it sends. Nothing asked for a close here, so it is not an
  // answer to AT+CIPCLOSE and ignoring it leaves the client waiting on a socket
  // that is already finished.
  channel_.InjectUrc("+CIPRXGET: 1," + std::to_string(id) + "\r\n");
  channel_.InjectUrc(std::to_string(id) + ", CLOSE OK\r\n");

  hal_->DrainPending();

  EXPECT_TRUE(closed);
}

TEST_F(Air780eHalTest, ACloseHeldForAReleasedSocketDoesNotEndItsReplacement) {
  const int first = OpenTcp();

  // The close arrives while the socket is still open, so it is recorded against
  // it - but nothing drains, so it is still held when the application retires the
  // socket. A cid is reusable the instant it is released, so the next socket
  // lands on the same one.
  channel_.InjectUrc(std::to_string(first) + ", CLOSED\r\n");

  channel_.ExpectCommand("AT+CIPCLOSE")
      .ThenUrc(std::to_string(first) + ", CLOSE OK\r\n");
  ASSERT_TRUE(hal_->TcpClose(first).has_value());

  bool closed = false;
  std::string received;
  channel_.ExpectCommand("AT+CIPSTART").ThenUrc(std::to_string(first) +
                                                ", CONNECT OK\r\n");
  auto second = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(second.has_value());
  ASSERT_EQ(*second, first) << "the cid is expected to be handed straight back";

  hal_->SubscribeTcp(
      *second, [&](int, std::string_view data) { received.append(data); },
      [&](int) { closed = true; });

  channel_.ExpectCommand("AT+CIPRXGET=3")
      .RespondInTurn({ReadsOf(*second, 8), ReadReply(*second, 0)});
  channel_.InjectUrc("+CIPRXGET: 1," + std::to_string(*second) + "\r\n");
  hal_->DrainPending();

  // The held close belonged to the socket that has gone. Acting on it here would
  // end a connection that is perfectly alive - which is what the serial exists
  // to prevent: "this cid" is not the same statement as "the socket that happens
  // to hold this cid now".
  EXPECT_FALSE(closed) << "a close for the previous socket ended the new one";
  EXPECT_EQ(received.size(), 8u) << "the new socket's data was lost with it";
}

// --- Sending ---

TEST_F(Air780eHalTest, SendCarriesThePayloadAndWaitsForTheAcknowledgement) {
  const int id = OpenTcp();

  const std::string payload = "hello";
  channel_.ExpectCommand("AT+CIPSEND")
      .ThenUrc(std::to_string(id) + ", SEND OK\r\n");

  auto sent = hal_->TcpSend(id, payload.data(), payload.size());
  ASSERT_TRUE(sent.has_value()) << sent.error().Message();
  EXPECT_EQ(*sent, 5);

  const auto& data_commands = channel_.SentDataCommands();
  ASSERT_EQ(data_commands.size(), 1u);
  EXPECT_EQ(data_commands[0].first, "AT+CIPSEND=" + std::to_string(id) + ",5");
  EXPECT_EQ(data_commands[0].second, payload);
}

TEST_F(Air780eHalTest, SendReportsATimeoutWhenTheAcknowledgementNeverComes) {
  const int id = OpenTcp();
  channel_.ExpectCommand("AT+CIPSEND").Respond("OK\r\n");

  auto sent = hal_->TcpSend(id, "x", 1);
  ASSERT_FALSE(sent.has_value());
  EXPECT_EQ(sent.error().code, NetworkErrc::kTimeout);
}

TEST_F(Air780eHalTest, ARefusedSendOnAClosedPeerIsReportedAsALostConnection) {
  const int id = OpenTcp();

  // The peer's close has been reported but not yet acted on - the reader has not
  // run. A send now is refused by the module with "+CME ERROR: 3", which is
  // literally true and useless to the caller: it arrives looking exactly like a
  // malformed command. The close is what actually happened, so that is what is
  // reported.
  channel_.InjectUrc(std::to_string(id) + ", CLOSED\r\n");
  channel_.FailCommand("AT+CIPSEND", AtErrc::kCmeError);

  auto sent = hal_->TcpSend(id, "x", 1);
  ASSERT_FALSE(sent.has_value());
  EXPECT_EQ(sent.error().code, NetworkErrc::kConnectionLost);
}

TEST_F(Air780eHalTest, SendRejectsACidOutOfRange) {
  auto sent = hal_->TcpSend(99, "x", 1);
  ASSERT_FALSE(sent.has_value());
  EXPECT_EQ(sent.error().code, NetworkErrc::kInvalidArgument);
}

// --- Closing ---

TEST_F(Air780eHalTest, CloseSendsCipcloseAndReleasesTheCid) {
  const int id = OpenTcp();
  channel_.ExpectCommand("AT+CIPCLOSE")
      .ThenUrc(std::to_string(id) + ", CLOSE OK\r\n");

  ASSERT_TRUE(hal_->TcpClose(id).has_value());
  EXPECT_TRUE(channel_.WasCommandSent("AT+CIPCLOSE=" + std::to_string(id)));

  // The cid goes back into the rotation and is handed out again at once - there
  // is no quarantine on this module, whose cids were measured reusable on all
  // three paths.
  channel_.ExpectCommand("AT+CIPSTART")
      .ThenUrc(std::to_string(id) + ", CONNECT OK\r\n");
  auto again = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(again.has_value());
  EXPECT_EQ(*again, id);
}

TEST_F(Air780eHalTest, CloseIsANoOpForACidThisProcessDoesNotHold) {
  // A close of a cid that was already let go is how a caller tidies up after the
  // module announced the peer's close by itself. It is not a failure.
  EXPECT_TRUE(hal_->TcpClose(3).has_value());
  EXPECT_FALSE(channel_.WasCommandSent("AT+CIPCLOSE=3"));
}

// --- UDP ---

TEST_F(Air780eHalTest, UdpDrainCarriesTheSenderAddress) {
  channel_.ExpectCommand("AT+CIPSTART").ThenUrc("0, CONNECT OK\r\n");
  auto opened = hal_->UdpOpen("114.114.114.114", 53);
  ASSERT_TRUE(opened.has_value());
  const int id = *opened;

  std::string payload;
  std::string host;
  uint16_t port = 0;
  hal_->SubscribeUdp(id, [&](int, std::string_view sender_host, uint16_t sender_port,
                             std::string_view data) {
    host = std::string(sender_host);
    port = sender_port;
    payload.assign(data);
  });

  // AT+CIPSRIP=1 puts the sender on both the arrival and the read reply, which is
  // the only thing that makes an unconnected datagram attributable to anyone.
  channel_.ExpectCommand("AT+CIPRXGET=3")
      .RespondInTurn({ReadReply(id, 4, "114.114.114.114:53"), ReadReply(id, 0)});

  channel_.InjectUrc("+CIPRXGET: 1," + std::to_string(id) +
                     ",\"114.114.114.114:53\"\r\n");
  hal_->DrainPending();

  EXPECT_EQ(host, "114.114.114.114");
  EXPECT_EQ(port, 53);
  EXPECT_EQ(payload.size(), 4u);
}

TEST_F(Air780eHalTest, UdpOpensWithTheUdpType) {
  channel_.ExpectCommand("AT+CIPSTART").ThenUrc("0, CONNECT OK\r\n");
  auto opened = hal_->UdpOpen("114.114.114.114", 53);
  ASSERT_TRUE(opened.has_value());
  EXPECT_TRUE(
      channel_.WasCommandSent("AT+CIPSTART=0,\"UDP\",\"114.114.114.114\",53"));
}

// --- TLS ---

TEST_F(Air780eHalTest, TlsOpensThroughTheModuleSwitch) {
  channel_.ExpectCommand("AT+SSLCFG").Respond("OK\r\n");
  channel_.ExpectCommand("AT+CIPSSL=1").Respond("OK\r\n");
  channel_.ExpectCommand("AT+CIPSSL?").Respond("+CIPSSL: 1\r\n\r\nOK\r\n");
  channel_.ExpectCommand("AT+CIPSTART").ThenUrc("0, CONNECT OK\r\n");

  auto id = hal_->TcpConnect("www.baidu.com", 443, /*ssl=*/true);
  ASSERT_TRUE(id.has_value()) << id.error().Message();
  EXPECT_TRUE(channel_.WasCommandSent("AT+CIPSSL=1"));
}

TEST_F(Air780eHalTest, TlsRefusesToShareTheModuleWithAPlaintextSocket) {
  const int plain = OpenTcp();

  // AT+CIPSSL is one switch for the whole module. Opening a TLS socket now would
  // either have to turn it off under the plaintext socket or connect the TLS one
  // in the clear, and both of those fail at the far end for a reason that looks
  // like the network. Refused instead, and said so.
  auto tls = hal_->TcpConnect("www.baidu.com", 443, /*ssl=*/true);
  ASSERT_FALSE(tls.has_value());
  EXPECT_EQ(tls.error().code, NetworkErrc::kResourceBusy);
  EXPECT_FALSE(channel_.WasCommandSent("AT+CIPSSL=1"))
      << "the switch was moved out from under the open socket";

  // And the other way round.
  channel_.ExpectCommand("AT+CIPCLOSE").ThenUrc("0, CLOSE OK\r\n");
  ASSERT_TRUE(hal_->TcpClose(plain).has_value());
  channel_.ExpectCommand("AT+SSLCFG").Respond("OK\r\n");
  channel_.ExpectCommand("AT+CIPSSL=1").Respond("OK\r\n");
  channel_.ExpectCommand("AT+CIPSSL?").Respond("+CIPSSL: 1\r\n\r\nOK\r\n");
  channel_.ExpectCommand("AT+CIPSTART").ThenUrcFrom([](const std::string& cmd) {
    const size_t begin = cmd.find('=') + 1;
    return cmd.substr(begin, cmd.find(',', begin) - begin) + ", CONNECT OK\r\n";
  });
  auto tls_id = hal_->TcpConnect("www.baidu.com", 443, /*ssl=*/true);
  ASSERT_TRUE(tls_id.has_value()) << tls_id.error().Message();

  auto plain_b = hal_->TcpConnect("example.com", 80);
  ASSERT_FALSE(plain_b.has_value());
  EXPECT_EQ(plain_b.error().code, NetworkErrc::kResourceBusy);
}

TEST_F(Air780eHalTest, TlsVerificationIsRefusedRatherThanQuietlySkipped) {
  // No command this firmware accepts installs a certificate authority or turns
  // verification on, so a TlsConfig asking for either cannot be honoured. A
  // socket that is encrypted but unauthenticated is not what the caller asked
  // for, and saying so is the only honest answer.
  TlsConfig config;
  config.verify_certificate = true;

  auto id = hal_->TcpConnect("www.baidu.com", 443, /*ssl=*/true, config);
  ASSERT_FALSE(id.has_value());
  EXPECT_EQ(id.error().code, NetworkErrc::kNotSupported);
  EXPECT_FALSE(channel_.WasCommandSent("AT+CIPSTART"));
}

}  // namespace
