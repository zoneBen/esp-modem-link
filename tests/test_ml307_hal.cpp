#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "mock_at_channel.h"
#include "modules/ml307/ml307_hal.h"

using namespace esp_modem_link;
using namespace esp_modem_link::modules::ml307;
using esp_modem_link::testing::MockAtChannel;

namespace {

constexpr const char* kCgmrMl307 = "+CGMR: ML307R-DNLM_V1.0.0\r\nOK\r\n";

// Programs the command sequence a successful TCP socket open runs through: the
// per-cid TLS switch, the payload-encoding setting, then AT+MIPOPEN whose
// result the module reports asynchronously as "+MIPOPEN: <id>,<code>".
void ExpectTcpOpen(MockAtChannel& channel, int id, bool ssl = false) {
  if (ssl) {
    channel.ExpectCommand("AT+MSSLCFG").Respond("OK\r\n");
  }
  channel.ExpectCommand("AT+MIPCFG=\"ssl\"").Respond("OK\r\n");
  channel.ExpectCommand("AT+MIPCFG=\"encoding\"").Respond("OK\r\n");

  char urc[32];
  std::snprintf(urc, sizeof(urc), "+MIPOPEN: %d,0\r\n", id);
  channel.ExpectCommand("AT+MIPOPEN").ThenUrc(urc).Respond("OK\r\n");
}

void ExpectUdpOpen(MockAtChannel& channel, int id) {
  // UDP is never TLS, but the flag is still cleared per socket so each one
  // starts from a known state - as the reference implementation does.
  channel.ExpectCommand("AT+MIPCFG=\"ssl\"").Respond("OK\r\n");
  channel.ExpectCommand("AT+MIPCFG=\"encoding\"").Respond("OK\r\n");

  char urc[32];
  std::snprintf(urc, sizeof(urc), "+MIPOPEN: %d,0\r\n", id);
  channel.ExpectCommand("AT+MIPOPEN").ThenUrc(urc).Respond("OK\r\n");
}

// Programs a successful open for whichever cid the HAL asks for, by reading the
// id out of the command. A test about the connection pool should not also have to
// predict which cid the allocator picks - it prefers one whose previous occupant
// has finished departing, so the answer is not simply the lowest free id.
void ExpectOpenOnAnyId(MockAtChannel& channel) {
  channel.ExpectCommand("AT+MIPCFG").Respond("OK\r\n");
  channel.ExpectCommand("AT+MIPOPEN")
      .ThenUrcFrom([](const std::string& cmd) {
        // AT+MIPOPEN=<id>,"TCP","host",<port>,,0
        size_t begin = cmd.find('=') + 1;
        size_t end = cmd.find(',', begin);
        return "+MIPOPEN: " + cmd.substr(begin, end - begin) + ",0\r\n";
      })
      .Respond("OK\r\n");
}

class Ml307HalTest : public ::testing::Test {
 protected:
  void SetUp() override {
    hal_ = std::make_unique<Ml307Hal>();
    // Initialization is only the two settings that are global to the module;
    // everything socket-scoped happens at open time.
    channel_.ExpectCommand("ATE0").Respond("ATE0\r\nOK\r\n");
    channel_.ExpectCommand("AT+CFUN=1").Respond("AT+CFUN=1\r\nOK\r\n");
    ASSERT_TRUE(hal_->Initialize(channel_).has_value());
    channel_.Reset();
  }

  MockAtChannel channel_;
  std::unique_ptr<Ml307Hal> hal_;
};

// --- Identification ---

TEST_F(Ml307HalTest, Identity) {
  EXPECT_EQ(hal_->GetModuleType(), ModuleType::kMl307);
  EXPECT_EQ(hal_->GetModuleName(), "ML307");
}

TEST_F(Ml307HalTest, Capabilities) {
  const auto& caps = hal_->GetCapabilities();
  EXPECT_TRUE(caps.tcp);
  EXPECT_TRUE(caps.udp);
  EXPECT_TRUE(caps.ssl_tcp);
  EXPECT_TRUE(caps.http);
  EXPECT_TRUE(caps.mqtt);
  EXPECT_TRUE(caps.low_power);
  // Six, per AT+MIPCFG=? reporting "cid",(0-5).
  EXPECT_EQ(caps.max_connections, 6);
  EXPECT_EQ(caps.max_baud_rate, 921600);
  // The module has MIPHTTP and MIPMQTT, but this HAL cannot construct clients
  // for them yet, and Auto mode reads these flags as "the builtin path will
  // work". Returning true here fails every Auto-mode HTTP request.
  EXPECT_FALSE(hal_->HasBuiltinHttp());
  EXPECT_FALSE(hal_->HasBuiltinMqtt());
}

// --- Initialize ---

TEST(Ml307HalInitTest, SendsInitSequence) {
  MockAtChannel channel;
  channel.ExpectCommand("ATE0").Respond("OK\r\n");
  channel.ExpectCommand("AT+CFUN=1").Respond("OK\r\n");

  Ml307Hal hal;
  auto result = hal.Initialize(channel);
  ASSERT_TRUE(result.has_value());

  EXPECT_TRUE(channel.WasCommandSent("ATE0"));
  EXPECT_TRUE(channel.WasCommandSent("AT+CFUN=1"));
}

// Socket-scoped settings are applied per connection instead of once at init:
// they are per-cid, and doing it at open time also repairs a cid that a module
// reset left in a different state.
TEST(Ml307HalInitTest, LeavesSocketSettingsToConnect) {
  MockAtChannel channel;
  channel.ExpectCommand("ATE0").Respond("OK\r\n");
  channel.ExpectCommand("AT+CFUN=1").Respond("OK\r\n");

  Ml307Hal hal;
  ASSERT_TRUE(hal.Initialize(channel).has_value());

  EXPECT_FALSE(channel.WasCommandSent("AT+MIPCFG"));
}

TEST(Ml307HalInitTest, FailsWhenAte0Fails) {
  MockAtChannel channel;
  channel.FailCommand("ATE0");

  Ml307Hal hal;
  auto result = hal.Initialize(channel);
  EXPECT_FALSE(result.has_value());
}

TEST(Ml307HalInitTest, RegistersUrcHandlers) {
  MockAtChannel channel;
  channel.ExpectCommand("ATE0").Respond("OK\r\n");
  channel.ExpectCommand("AT+CFUN=1").Respond("OK\r\n");

  Ml307Hal hal;
  ASSERT_TRUE(hal.Initialize(channel).has_value());

  // A CEREG URC after init should reach the HAL's network-state callback.
  RegistrationState observed = RegistrationState::kUnknown;
  hal.SetNetworkStateCallback(
      [&](RegistrationState s) { observed = s; });

  channel.InjectUrc("+CEREG: 1\r\n");
  EXPECT_EQ(observed, RegistrationState::kRegisteredHome);

  channel.InjectUrc("+CEREG: 5\r\n");
  EXPECT_EQ(observed, RegistrationState::kRegisteredRoaming);
}

// --- UART rate alignment ---
//
// The negotiation itself lives in at_channel::baud_alignment, and its tests in
// test_baud_alignment.cpp. It cannot be tested from here: it now runs in
// CellularDevice, around module identification, rather than inside
// Ml307Hal::Initialize - which is where it used to sit, out of reach of the
// only code path that could have exercised it. See the seam tests in
// test_cellular_device.cpp.

// --- Device info ---

TEST_F(Ml307HalTest, GetImei) {
  // Captured from a real ML307R-DL: AT+CGSN returns the product serial, so the
  // HAL must ask for AT+CGSN=1 and strip the key prefix.
  channel_.ExpectCommand("AT+CGSN=1")
      .Respond("AT+CGSN=1\r\n+CGSN: 866475084636312\r\nOK\r\n");
  auto result = hal_->GetImei();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "866475084636312");
}

TEST_F(Ml307HalTest, GetImeiFailsOnError) {
  channel_.ExpectCommand("AT+CGSN=1").Respond("AT+CGSN=1\r\nERROR\r\n");
  auto result = hal_->GetImei();
  EXPECT_FALSE(result.has_value());
}

TEST_F(Ml307HalTest, GetRevision) {
  channel_.ExpectCommand("AT+CGMR").Respond(kCgmrMl307);
  auto result = hal_->GetRevision();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "+CGMR: ML307R-DNLM_V1.0.0");
}

TEST_F(Ml307HalTest, GetIccid) {
  // Real shape: AT+ICCID replies with a "+ICCID: " prefixed value. AT+CCID is
  // not implemented on this firmware at all.
  channel_.ExpectCommand("AT+ICCID")
      .Respond("AT+ICCID\r\n+ICCID: 89860836192440160499\r\nOK\r\n");
  auto result = hal_->GetIccid();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "89860836192440160499");
}

TEST_F(Ml307HalTest, GetCarrierStripsQuotes) {
  channel_.ExpectCommand("AT+COPS")
      .Respond("AT+COPS?\r\n+COPS: 0,0,\"CHINA MOBILE\",7\r\nOK\r\n");
  auto result = hal_->GetCarrier();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), "CHINA MOBILE");
}

// --- Signal & registration ---

TEST_F(Ml307HalTest, GetSignalInfo) {
  channel_.ExpectCommand("AT+CSQ").Respond("AT+CSQ\r\n+CSQ: 23,99\r\nOK\r\n");
  auto info = hal_->GetSignalInfo();
  EXPECT_EQ(info.rssi, 23);
  EXPECT_EQ(info.ber, 99);
}

TEST_F(Ml307HalTest, GetSignalInfoUnknown) {
  channel_.ExpectCommand("AT+CSQ").Respond("AT+CSQ\r\n+CSQ: 99,99\r\nOK\r\n");
  auto info = hal_->GetSignalInfo();
  EXPECT_EQ(info.rssi, 99);
}

TEST_F(Ml307HalTest, GetRegistrationStateRegistered) {
  channel_.ExpectCommand("AT+CEREG?")
      .Respond("AT+CEREG?\r\n+CEREG: 0,1\r\nOK\r\n");
  EXPECT_EQ(hal_->GetRegistrationState(),
            RegistrationState::kRegisteredHome);
}

TEST_F(Ml307HalTest, GetRegistrationStateSearching) {
  channel_.ExpectCommand("AT+CEREG?")
      .Respond("AT+CEREG?\r\n+CEREG: 0,2\r\nOK\r\n");
  EXPECT_EQ(hal_->GetRegistrationState(), RegistrationState::kSearching);
}

TEST_F(Ml307HalTest, GetRegistrationStateDenied) {
  channel_.ExpectCommand("AT+CEREG?")
      .Respond("AT+CEREG?\r\n+CEREG: 0,3\r\nOK\r\n");
  EXPECT_EQ(hal_->GetRegistrationState(),
            RegistrationState::kRegistrationDenied);
}

TEST_F(Ml307HalTest, GetRegistrationStateRoaming) {
  channel_.ExpectCommand("AT+CEREG?")
      .Respond("AT+CEREG?\r\n+CEREG: 0,5\r\nOK\r\n");
  EXPECT_EQ(hal_->GetRegistrationState(),
            RegistrationState::kRegisteredRoaming);
}

// --- SIM ---

TEST_F(Ml307HalTest, GetSimStateReady) {
  channel_.ExpectCommand("AT+CPIN?")
      .Respond("AT+CPIN?\r\n+CPIN: READY\r\nOK\r\n");
  EXPECT_EQ(hal_->GetSimState(), SimState::kReady);
}

TEST_F(Ml307HalTest, GetSimStatePinRequired) {
  channel_.ExpectCommand("AT+CPIN?")
      .Respond("AT+CPIN?\r\n+CPIN: SIM PIN\r\nOK\r\n");
  EXPECT_EQ(hal_->GetSimState(), SimState::kPinRequired);
}

TEST_F(Ml307HalTest, GetSimStateUnknownOnError) {
  channel_.ExpectCommand("AT+CPIN?").Respond("ERROR\r\n");
  EXPECT_EQ(hal_->GetSimState(), SimState::kUnknown);
}

// --- APN ---

TEST_F(Ml307HalTest, ConfigureApnWithoutCredentials) {
  channel_.ExpectCommand("AT+CGDCONT")
      .Respond("AT+CGDCONT=1,\"IP\",\"cmnet\"\r\nOK\r\n");
  channel_.ExpectCommand("AT+MIPCALL?").Respond("+MIPCALL: 1,0\r\nOK\r\n");
  channel_.ExpectCommand("AT+MIPCALL=1,1").Respond("OK\r\n");

  ApnConfig apn;
  apn.apn = "cmnet";
  auto result = hal_->ConfigureApn(apn);
  ASSERT_TRUE(result.has_value());

  EXPECT_TRUE(channel_.WasCommandSent("AT+CGDCONT=1,\"IP\",\"cmnet\""));
  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPCALL=1,1"));
}

// The module brings a context up on its own at boot. Activating again in that
// state answers "CME ERROR: 50" on real hardware, which would make ConfigureApn
// fail on a perfectly healthy data path.
TEST_F(Ml307HalTest, ConfigureApnSkipsActivationWhenAlreadyActive) {
  channel_.ExpectCommand("AT+CGDCONT").Respond("OK\r\n");
  channel_.ExpectCommand("AT+MIPCALL?")
      .Respond("+MIPCALL: 1,1,\"10.245.95.205\"\r\nOK\r\n");

  ApnConfig apn;
  apn.apn = "cmnet";
  auto result = hal_->ConfigureApn(apn);
  ASSERT_TRUE(result.has_value());

  EXPECT_FALSE(channel_.WasCommandSent("AT+MIPCALL=1,1"));
}

// Right after activation AT+MIPCALL? answers with no info line at all, which
// must not be mistaken for "active".
TEST_F(Ml307HalTest, ConfigureApnActivatesWhenQueryHasNoInfoLine) {
  channel_.ExpectCommand("AT+CGDCONT").Respond("OK\r\n");
  channel_.ExpectCommand("AT+MIPCALL?").Respond("OK\r\n");
  channel_.ExpectCommand("AT+MIPCALL=1,1").Respond("OK\r\n");

  ApnConfig apn;
  apn.apn = "cmnet";
  ASSERT_TRUE(hal_->ConfigureApn(apn).has_value());
  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPCALL=1,1"));
}

TEST_F(Ml307HalTest, ConfigureApnWithCredentials) {
  channel_.ExpectCommand("AT+CGDCONT").Respond("OK\r\n");
  channel_.ExpectCommand("AT+CGAUTH").Respond("OK\r\n");
  channel_.ExpectCommand("AT+MIPCALL?").Respond("+MIPCALL: 1,0\r\nOK\r\n");
  channel_.ExpectCommand("AT+MIPCALL=1,1").Respond("OK\r\n");

  ApnConfig apn;
  apn.apn = "cmnet";
  apn.username = "user";
  apn.password = "pass";
  auto result = hal_->ConfigureApn(apn);
  ASSERT_TRUE(result.has_value());

  EXPECT_TRUE(channel_.WasCommandSent("AT+CGAUTH=1,1,\"user\",\"pass\""));
}

TEST_F(Ml307HalTest, ConfigureApnErrorNamesTheFailingStep) {
  channel_.ExpectCommand("AT+CGDCONT").Respond("OK\r\n");
  channel_.ExpectCommand("AT+MIPCALL?").Respond("+MIPCALL: 1,0\r\nOK\r\n");
  channel_.ExpectCommand("AT+MIPCALL=1,1").Respond("+CME ERROR: 50\r\n");

  ApnConfig apn;
  apn.apn = "cmnet";
  auto result = hal_->ConfigureApn(apn);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().context, "AT+MIPCALL: 50");
}

// --- TCP ---

TEST_F(Ml307HalTest, TcpConnect) {
  ExpectTcpOpen(channel_, 0);

  auto result = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(result.has_value()) << "error: " << result.error().context;
  EXPECT_EQ(result.value(), 0);
  // The socket type is a quoted string and the timeout slot is left empty; the
  // positional numeric form answers "+CME ERROR: 50" on real ML307R-DL.
  EXPECT_TRUE(
      channel_.WasCommandSent("AT+MIPOPEN=0,\"TCP\",\"example.com\",80,,0"));
}

TEST_F(Ml307HalTest, TcpConnectConfiguresEncodingForTheSocket) {
  ExpectTcpOpen(channel_, 0);

  ASSERT_TRUE(hal_->TcpConnect("example.com", 80).has_value());

  // Hex in both directions: raw receive would corrupt payloads containing \r\n
  // and raw send cannot carry a 0x1A byte without truncating the frame.
  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPCFG=\"encoding\",0,1,1"));
}

// TLS is a per-cid setting applied before MIPOPEN, not an argument to it.
TEST_F(Ml307HalTest, TcpConnectSsl) {
  ExpectTcpOpen(channel_, 0, /*ssl=*/true);

  auto result = hal_->TcpConnect("example.com", 443, true);
  ASSERT_TRUE(result.has_value()) << "error: " << result.error().context;

  EXPECT_TRUE(channel_.WasCommandSent("AT+MSSLCFG=\"auth\",0,0"));
  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPCFG=\"ssl\",0,1,0"));
  EXPECT_TRUE(
      channel_.WasCommandSent("AT+MIPOPEN=0,\"TCP\",\"example.com\",443,,0"));
}

TEST_F(Ml307HalTest, TcpConnectDisablesSslWhenNotRequested) {
  ExpectTcpOpen(channel_, 0);

  ASSERT_TRUE(hal_->TcpConnect("example.com", 80).has_value());

  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPCFG=\"ssl\",0,0,0"));
  EXPECT_FALSE(channel_.WasCommandSent("AT+MSSLCFG"));
}

// AT+MIPOPEN returns OK as soon as the request is accepted; the socket is not
// usable until the module reports the outcome as "+MIPOPEN: <id>,<code>".
TEST_F(Ml307HalTest, TcpConnectFailsWhenModuleReportsError) {
  channel_.ExpectCommand("AT+MIPCFG").Respond("OK\r\n");
  channel_.ExpectCommand("AT+MIPOPEN")
      .ThenUrc("+MIPOPEN: 0,3\r\n")
      .Respond("OK\r\n");

  auto result = hal_->TcpConnect("example.com", 80);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kConnectFailed);
  EXPECT_EQ(result.error().native, 3);

  // The slot must be reclaimed so a retry is not permanently out of resources.
  ExpectOpenOnAnyId(channel_);
  auto retry = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(retry.has_value()) << "error: " << retry.error().context;
  EXPECT_GE(retry.value(), 0);
}

TEST_F(Ml307HalTest, TcpConnectReleasesIdOnFailure) {
  channel_.FailCommand("AT+MIPOPEN", AtErrc::kTimeout);

  auto failed = hal_->TcpConnect("example.com", 80);
  EXPECT_FALSE(failed.has_value());

  // The slot must be reclaimed, so a later connection still gets one. Which cid
  // it is depends on the allocator: the failed one is quarantined for having been
  // released, so the retry takes a pristine cid instead.
  channel_.ClearFailures();
  ExpectOpenOnAnyId(channel_);
  auto ok = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(ok.has_value()) << "error: " << ok.error().context;
  EXPECT_GE(ok.value(), 0);
}

// The module keeps its own view of which cids are in use, and it outlives this
// process: a socket left open by an earlier run answers "+CME ERROR: 552" for
// that id even though the pool here has never touched it. Skipping to the next
// id is what keeps one stale socket from failing every connection until the
// module is reset. Observed on ML307R-DL-MBRH0S01, where the same command
// succeeds immediately after an AT+MIPCLOSE for that id.
TEST_F(Ml307HalTest, TcpConnectSkipsAConnectionIdTheModuleAlreadyHolds) {
  // Keyed by id so only the first attempt is refused - the mock matches on the
  // longest prefix, which is what makes a per-id script possible.
  channel_.ExpectCommand("AT+MIPCFG").Respond("OK\r\n");
  channel_.ExpectCommand("AT+MIPOPEN=0,").Respond("+CME ERROR: 552\r\n");
  channel_.ExpectCommand("AT+MIPOPEN=1,")
      .ThenUrc("+MIPOPEN: 1,0\r\n")
      .Respond("OK\r\n");

  auto result = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result.value(), 1);
}

TEST_F(Ml307HalTest, TcpConnectReportsWhenEveryConnectionIdIsHeld) {
  channel_.ExpectCommand("AT+MIPCFG").Respond("OK\r\n");
  channel_.ExpectCommand("AT+MIPOPEN").Respond("+CME ERROR: 552\r\n");

  auto result = hal_->TcpConnect("example.com", 80);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNoResources);

  // Every id was only ever tried and handed back, so none of them may be left
  // marked used - otherwise one run against a full module would leave the pool
  // permanently empty.
  ExpectTcpOpen(channel_, 0);
  auto after = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(after.has_value()) << after.error().Message();
  EXPECT_EQ(after.value(), 0);
}

// A refused TLS handshake is not a busy slot, and retrying it would burn every
// remaining socket on a host that will never succeed.
TEST_F(Ml307HalTest, TcpConnectDoesNotRetryAGenuineConnectFailure) {
  channel_.ExpectCommand("AT+MIPCFG").Respond("OK\r\n");
  channel_.ExpectCommand("AT+MIPOPEN")
      .ThenUrc("+MIPOPEN: 0,753\r\n")
      .Respond("OK\r\n");

  auto result = hal_->TcpConnect("example.com", 443, true);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kConnectFailed);
  EXPECT_EQ(result.error().native, 753);

  int opens = 0;
  for (const auto& cmd : channel_.SentCommands()) {
    if (cmd.rfind("AT+MIPOPEN", 0) == 0) ++opens;
  }
  EXPECT_EQ(opens, 1);
}

// --- Reusing a connection id the module has not released ---
//
// The pool above tracks what this process opened, but the module's sockets
// outlive the process: a cid freed here can still be held there. AT+MIPSTATE is
// the module's own answer to "is this cid in use", and it comes back as a normal
// response line rather than a URC, so it can be read without racing an event.

// A "+MIPSTATE" reply. A free cid leaves the socket description empty, which is
// what ML307R-DL-MBRH0S01 was observed to send: "+MIPSTATE: 0,,,,"INITIAL"".
std::string StateResponse(int id, std::string_view state) {
  std::string line = "+MIPSTATE: " + std::to_string(id);
  if (state == "INITIAL") {
    line += ",,,,";
  } else {
    line += ",\"TCP\",\"www.baidu.com\",80,";
  }
  line += "\"" + std::string(state) + "\"\r\nOK\r\n";
  return line;
}

TEST_F(Ml307HalTest, TcpConnectReleasesACidTheModuleStillHolds) {
  // Held on the first query, released by the second - the transition a real
  // close goes through.
  channel_.ExpectCommand("AT+MIPSTATE=0").RespondInTurn({
      StateResponse(0, "CONNECTED"),
      StateResponse(0, "INITIAL"),
  });
  channel_.ExpectCommand("AT+MIPCLOSE=0").Respond("OK\r\n");
  ExpectTcpOpen(channel_, 1);

  auto result = hal_->TcpConnect("www.baidu.com", 80);
  ASSERT_TRUE(result.has_value()) << result.error().Message();

  // The connect lands on the next cid rather than the one it just freed. The
  // events announcing that close name only the cid, and a late "disconn" for the
  // old socket arriving after a new one was up on that cid tore the new socket
  // down mid-response, so a cid that had to be closed is left to settle.
  EXPECT_EQ(result.value(), 1);

  // And the close must precede the open it is making room for.
  const auto& sent = channel_.SentCommands();
  auto closed = std::find(sent.begin(), sent.end(), "AT+MIPCLOSE=0");
  auto opened = std::find_if(sent.begin(), sent.end(), [](const std::string& s) {
    return s.rfind("AT+MIPOPEN", 0) == 0;
  });
  ASSERT_NE(closed, sent.end());
  ASSERT_NE(opened, sent.end());
  EXPECT_LT(closed, opened);
}

TEST_F(Ml307HalTest, TcpConnectLeavesAFreeConnectionIdAlone) {
  channel_.ExpectCommand("AT+MIPSTATE=0").Respond(StateResponse(0, "INITIAL"));
  ExpectTcpOpen(channel_, 0);

  auto result = hal_->TcpConnect("www.baidu.com", 80);
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_FALSE(channel_.WasCommandSent("AT+MIPCLOSE"));
}

// A reply carrying no "+MIPSTATE" line says nothing about the cid, so there is
// nothing to clean up. Reading it as "held" would make every connect close a
// socket it never opened, and fail outright on a firmware without the query.
TEST_F(Ml307HalTest, TcpConnectAssumesFreeWhenTheModuleReportsNoState) {
  channel_.ExpectCommand("AT+MIPSTATE=0").Respond("OK\r\n");
  ExpectTcpOpen(channel_, 0);

  auto result = hal_->TcpConnect("www.baidu.com", 80);
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_FALSE(channel_.WasCommandSent("AT+MIPCLOSE"));
}

// Observed on ML307R-DL-MBRH0S01: a close event still in flight is delivered
// ahead of the query's own answer, so the state line is not the first line in
// the buffer and cannot be read positionally. The state behind the stray line is
// deliberately "held" and not "free" - with "free" a positional reader would
// reach the same conclusion by accident and the test would pin nothing.
TEST_F(Ml307HalTest, TcpConnectReadsTheStateLinePastAnEarlierUrc) {
  channel_.ExpectCommand("AT+MIPSTATE=0").RespondInTurn({
      "+MIPCLOSE: 0\r\n" + StateResponse(0, "CONNECTED"),
      StateResponse(0, "INITIAL"),
  });
  channel_.ExpectCommand("AT+MIPCLOSE=0").Respond("OK\r\n");
  ExpectTcpOpen(channel_, 1);

  auto result = hal_->TcpConnect("www.baidu.com", 80);
  ASSERT_TRUE(result.has_value()) << result.error().Message();

  // The state was found by name, past the stray line, and acted on.
  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPCLOSE=0"));
  EXPECT_EQ(result.value(), 1);
}

// A cid that stays held no matter how often it is queried is not safe to open
// on top of, so the connect moves to the next one. The mock repeats the last
// response of a sequence once it runs out, which is what makes "never releases"
// expressible.
TEST_F(Ml307HalTest, TcpConnectMovesOnWhenACidWillNotFree) {
  channel_.ExpectCommand("AT+MIPSTATE=0").Respond(StateResponse(0, "CONNECTED"));
  channel_.ExpectCommand("AT+MIPSTATE=1").Respond(StateResponse(1, "INITIAL"));
  channel_.ExpectCommand("AT+MIPCLOSE=0").Respond("OK\r\n");
  ExpectTcpOpen(channel_, 1);

  auto result = hal_->TcpConnect("www.baidu.com", 80);
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result.value(), 1);
}

// The module does not always put its OK before the result. A line arriving while
// a command is in flight is absorbed into that command's response buffer and
// never reaches the URC dispatcher, so a "+MIPOPEN" that beats its own OK has to
// be read back out of the response - otherwise the connect waits out the full
// timeout for a socket that is already open. No URC is queued here, so the
// rendezvous can only be satisfied from the response buffer.
TEST_F(Ml307HalTest, TcpConnectFindsTheOpenResultInTheResponseBuffer) {
  channel_.ExpectCommand("AT+MIPCFG").Respond("OK\r\n");
  channel_.ExpectCommand("AT+MIPOPEN").Respond("+MIPOPEN: 0,0\r\nOK\r\n");

  auto result = hal_->TcpConnect("www.baidu.com", 80);
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result.value(), 0);
}

TEST_F(Ml307HalTest, TcpConnectAssignsDistinctIds) {
  ExpectTcpOpen(channel_, 0);
  ExpectTcpOpen(channel_, 1);
  ExpectTcpOpen(channel_, 2);

  auto id0 = hal_->TcpConnect("a.com", 80);
  auto id1 = hal_->TcpConnect("b.com", 80);
  auto id2 = hal_->TcpConnect("c.com", 80);

  ASSERT_TRUE(id0.has_value());
  ASSERT_TRUE(id1.has_value());
  ASSERT_TRUE(id2.has_value());
  EXPECT_EQ(id0.value(), 0);
  EXPECT_EQ(id1.value(), 1);
  EXPECT_EQ(id2.value(), 2);
}

TEST_F(Ml307HalTest, TcpConnectExhaustsConnectionPool) {
  for (int i = 0; i < 6; ++i) {
    ExpectTcpOpen(channel_, i);
  }

  std::vector<int> ids;
  for (int i = 0; i < 6; i++) {
    auto id = hal_->TcpConnect("host.com", 80);
    ASSERT_TRUE(id.has_value()) << "connection " << i << " should succeed";
    ids.push_back(id.value());
  }

  auto overflow = hal_->TcpConnect("host.com", 80);
  EXPECT_FALSE(overflow.has_value());
  EXPECT_EQ(overflow.error().code, NetworkErrc::kNoResources);
}

TEST_F(Ml307HalTest, TcpClose) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  channel_.ExpectCommand("AT+MIPCLOSE").Respond("OK\r\n");
  auto result = hal_->TcpClose(id.value());
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPCLOSE=0"));
}

// Releasing must be real: the local pool is the only thing stopping a seventh
// socket, since the module cannot open one.
TEST_F(Ml307HalTest, TcpCloseFreesConnectionSlot) {
  for (int i = 0; i < 6; ++i) {
    ExpectTcpOpen(channel_, i);
  }
  channel_.ExpectCommand("AT+MIPCLOSE").Respond("OK\r\n");

  std::vector<int> ids;
  for (int i = 0; i < 6; i++) {
    auto id = hal_->TcpConnect("host.com", 80);
    ASSERT_TRUE(id.has_value()) << "connection " << i;
    ids.push_back(id.value());
  }
  EXPECT_FALSE(hal_->TcpConnect("host.com", 80).has_value());

  for (int id : ids) {
    ASSERT_TRUE(hal_->TcpClose(id).has_value());
  }

  // Every cid is free again. All six were released just now and so are all
  // quarantined, which leaves the allocator no pristine cid to prefer and it
  // takes the one released longest ago - slot 0.
  ExpectTcpOpen(channel_, 0);
  auto reused = hal_->TcpConnect("host.com", 80);
  ASSERT_TRUE(reused.has_value()) << "error: " << reused.error().context;
  EXPECT_EQ(reused.value(), 0);
}

// The payload travels inline as hex (AT+MIPSEND=<id>,<len>,<hex>) rather than
// as raw bytes after a ">" prompt, so no byte in it can be mistaken for AT
// framing and nothing has to be terminated with 0x1A.
TEST_F(Ml307HalTest, TcpSend) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  channel_.ExpectCommand("AT+MIPSEND").Respond("OK\r\n");

  auto result = hal_->TcpSend(id.value(), "hello", 5);
  ASSERT_TRUE(result.has_value()) << "error: " << result.error().context;
  EXPECT_EQ(result.value(), 5);

  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPSEND=0,5,68656c6c6f"));
  EXPECT_TRUE(channel_.SentDataCommands().empty());
}

// <len> counts payload bytes, not the doubled hex characters, and a long
// payload is split so the command line stays inside the module's input buffer.
TEST_F(Ml307HalTest, TcpSendChunksLongPayload) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  channel_.ExpectCommand("AT+MIPSEND").Respond("OK\r\n");

  std::string payload(2000, 'A');
  auto result = hal_->TcpSend(id.value(), payload.data(), payload.size());
  ASSERT_TRUE(result.has_value()) << "error: " << result.error().context;
  EXPECT_EQ(result.value(), 2000);

  // 4 setup (state query, ssl, encoding, open) + 3 send.
  ASSERT_EQ(channel_.SentCommands().size(), 7u);
  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPSEND=0,730,"));
  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPSEND=0,540,"));
}

TEST_F(Ml307HalTest, TcpSendIsBinarySafe) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  channel_.ExpectCommand("AT+MIPSEND").Respond("OK\r\n");

  // Bytes that would collide with AT framing if sent raw: CR, LF, ESC, 0x1A.
  const char payload[] = {'\r', '\n', 0x1b, 0x1a};
  auto result = hal_->TcpSend(id.value(), payload, sizeof(payload));
  ASSERT_TRUE(result.has_value()) << "error: " << result.error().context;

  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPSEND=0,4,0d0a1b1a"));
}

TEST_F(Ml307HalTest, TcpSendFailsWhenCommandFails) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  channel_.FailCommand("AT+MIPSEND", AtErrc::kTimeout);
  auto result = hal_->TcpSend(id.value(), "hello", 5);
  EXPECT_FALSE(result.has_value());
}

// Closing a socket we no longer hold is a no-op. The module announces the peer's
// close by itself and answers "+CME ERROR: 551" if asked to close it again, so a
// caller tidying up after a disconnect must not see a spurious error.
TEST_F(Ml307HalTest, TcpCloseIsNoopOnceSlotReleased) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  channel_.InjectUrc("+MIPCLOSE: 0\r\n");  // peer closed; slot released
  channel_.Reset();

  EXPECT_TRUE(hal_->TcpClose(id.value()).has_value());
  EXPECT_FALSE(channel_.WasCommandSent("AT+MIPCLOSE"));
}

TEST_F(Ml307HalTest, TcpCloseTreatsModuleRejectionAsAlreadyClosed) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  channel_.ExpectCommand("AT+MIPCLOSE").Respond("+CME ERROR: 551\r\n");
  EXPECT_TRUE(hal_->TcpClose(id.value()).has_value());

  // Released all the same, so a socket is still available afterwards.
  ExpectOpenOnAnyId(channel_);
  auto next = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(next.has_value()) << "error: " << next.error().context;
  EXPECT_GE(next.value(), 0);
}

// A timeout is not the module saying "already closed" - the socket's fate is
// unknown, so it is reported rather than smoothed over.
TEST_F(Ml307HalTest, TcpCloseReportsTransportFailure) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  channel_.FailCommand("AT+MIPCLOSE", AtErrc::kTimeout);
  auto result = hal_->TcpClose(id.value());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kAtTimeout);
}

// --- UDP ---

TEST_F(Ml307HalTest, UdpOpen) {
  ExpectUdpOpen(channel_, 0);

  auto result = hal_->UdpOpen("1.1.1.1", 53);
  ASSERT_TRUE(result.has_value()) << "error: " << result.error().context;
  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPOPEN=0,\"UDP\",\"1.1.1.1\",53,,0"));
}

TEST_F(Ml307HalTest, UdpSend) {
  ExpectUdpOpen(channel_, 0);
  auto id = hal_->UdpOpen("1.1.1.1", 53);
  ASSERT_TRUE(id.has_value());

  channel_.ExpectCommand("AT+MIPSEND").Respond("OK\r\n");
  auto result = hal_->UdpSend(id.value(), "query", 5);
  ASSERT_TRUE(result.has_value()) << "error: " << result.error().context;
  EXPECT_EQ(result.value(), 5);
  EXPECT_TRUE(channel_.WasCommandSent("AT+MIPSEND=0,5,7175657279"));
}

TEST_F(Ml307HalTest, UdpCloseFreesSlot) {
  ExpectUdpOpen(channel_, 0);
  auto id = hal_->UdpOpen("1.1.1.1", 53);
  ASSERT_TRUE(id.has_value());

  channel_.ExpectCommand("AT+MIPCLOSE").Respond("OK\r\n");
  EXPECT_TRUE(hal_->UdpClose(id.value()).has_value());

  channel_.ExpectCommand("AT+MIPCFG").Respond("OK\r\n");
  channel_.ExpectCommand("AT+MIPOPEN")
      .ThenUrcFrom([](const std::string& cmd) {
        size_t begin = cmd.find('=') + 1;
        size_t end = cmd.find(',', begin);
        return "+MIPOPEN: " + cmd.substr(begin, end - begin) + ",0\r\n";
      })
      .Respond("OK\r\n");

  auto next = hal_->UdpOpen("1.1.1.1", 53);
  ASSERT_TRUE(next.has_value()) << "error: " << next.error().context;
  EXPECT_GE(next.value(), 0);
}

// --- URC-driven data delivery ---

TEST_F(Ml307HalTest, TcpDataUrcReachesCallback) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  std::string received;
  int received_id = -1;
  hal_->SubscribeTcp(
      0,
      [&](int cid, std::string_view data) {
        received_id = cid;
        received = std::string(data);
      },
      nullptr);  // this test is about payload, not the close

  // Inbound payload arrives as +MIPURC: "rtcp",<id>,<len>,<hex>.
  channel_.InjectUrc("+MIPURC: \"rtcp\",0,5,\"68656c6c6f\"\r\n");

  EXPECT_EQ(received_id, 0);
  EXPECT_EQ(received, "hello");
}

// The alternative +MIPRTCP shape is subscribed too; hardware will confirm which
// of the two this firmware actually emits.
TEST_F(Ml307HalTest, TcpRtcpUrcReachesCallback) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  std::string received;
  hal_->SubscribeTcp(
      0, [&](int, std::string_view data) { received = std::string(data); },
      nullptr);

  channel_.InjectUrc("+MIPRTCP: 0,5,\"68656c6c6f\"\r\n");

  EXPECT_EQ(received, "hello");
}

TEST_F(Ml307HalTest, UdpDataUrcReachesCallback) {
  ExpectUdpOpen(channel_, 0);
  auto id = hal_->UdpOpen("example.com", 53);
  ASSERT_TRUE(id.has_value());

  std::string received;
  int received_id = -1;
  hal_->SubscribeUdp(
      0,
      [&](int cid, std::string_view, uint16_t, std::string_view data) {
        received_id = cid;
        received = std::string(data);
      });

  // Inbound UDP datagrams use their own event name, "rudp". Matching only
  // "rtcp" silently drops every datagram, which is precisely what happened
  // before this test existed.
  channel_.InjectUrc("+MIPURC: \"rudp\",0,5,\"68656c6c6f\"\r\n");

  EXPECT_EQ(received_id, 0);
  EXPECT_EQ(received, "hello");
}

// The reference implementation reads only four fields, so source host/port are
// treated as optional rather than required.
TEST_F(Ml307HalTest, UdpDataUrcReportsSourceWhenPresent) {
  ExpectUdpOpen(channel_, 0);
  auto id = hal_->UdpOpen("example.com", 53);
  ASSERT_TRUE(id.has_value());

  std::string host;
  uint16_t port = 0;
  hal_->SubscribeUdp(
      0, [&](int, std::string_view h, uint16_t p, std::string_view) {
        host = std::string(h);
        port = p;
      });

  channel_.InjectUrc("+MIPURC: \"rudp\",0,2,\"0102\",\"1.1.1.1\",443\r\n");

  EXPECT_EQ(host, "1.1.1.1");
  EXPECT_EQ(port, 443);
}

// The two data events share one URC name, so a "rudp" datagram must not be
// handed to the TCP callback.
TEST_F(Ml307HalTest, UdpDataUrcDoesNotReachTcpCallback) {
  ExpectTcpOpen(channel_, 0);
  ASSERT_TRUE(hal_->TcpConnect("example.com", 80).has_value());

  bool tcp_called = false;
  hal_->SubscribeTcp(
      0, [&](int, std::string_view) { tcp_called = true; }, nullptr);

  channel_.InjectUrc("+MIPURC: \"rudp\",0,5,\"68656c6c6f\"\r\n");

  EXPECT_FALSE(tcp_called);
}

TEST_F(Ml307HalTest, TcpCloseUrcReleasesSlotAndNotifies) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  bool closed = false;
  int closed_id = -1;
  hal_->SubscribeTcp(0, nullptr, [&](int cid) {
    closed = true;
    closed_id = cid;
  });

  channel_.InjectUrc("+MIPCLOSE: 0\r\n");

  EXPECT_TRUE(closed);
  EXPECT_EQ(closed_id, 0);

  // Slot freed. The cid it was on is quarantined, having just been released, so
  // the next connect takes a different one - hence the id is not asserted.
  ExpectOpenOnAnyId(channel_);
  auto next = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(next.has_value()) << "error: " << next.error().context;
  EXPECT_GE(next.value(), 0);
}

TEST_F(Ml307HalTest, MipUrcDisconnectReleasesSlotAndNotifies) {
  ExpectTcpOpen(channel_, 0);
  auto id = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(id.has_value());

  bool closed = false;
  hal_->SubscribeTcp(0, nullptr, [&](int) { closed = true; });

  // Peer-initiated close arrives as +MIPURC: "disconn",<id>.
  channel_.InjectUrc("+MIPURC: \"disconn\",0\r\n");

  EXPECT_TRUE(closed);

  // Pooled again, on a cid other than the one the peer just closed.
  ExpectOpenOnAnyId(channel_);
  auto next = hal_->TcpConnect("example.com", 80);
  ASSERT_TRUE(next.has_value()) << "error: " << next.error().context;
  EXPECT_GE(next.value(), 0);
}

// --- Flight mode & sleep ---

TEST_F(Ml307HalTest, SetFlightModeOn) {
  channel_.ExpectCommand("AT+CFUN=0").Respond("OK\r\n");
  EXPECT_TRUE(hal_->SetFlightMode(true).has_value());
  EXPECT_TRUE(channel_.WasCommandSent("AT+CFUN=0"));
}

TEST_F(Ml307HalTest, SetFlightModeOff) {
  channel_.ExpectCommand("AT+CFUN=1").Respond("OK\r\n");
  EXPECT_TRUE(hal_->SetFlightMode(false).has_value());
}

TEST_F(Ml307HalTest, LowPowerLifecycle) {
  EXPECT_TRUE(hal_->SupportsLowPower());

  channel_.ExpectCommand("AT+SLEEP=2").Respond("OK\r\n");
  SleepConfig config;
  EXPECT_TRUE(hal_->EnterSleep(config).has_value());

  channel_.ExpectCommand("AT+SLEEP=0").Respond("OK\r\n");
  EXPECT_TRUE(hal_->ExitSleep().has_value());
}

TEST_F(Ml307HalTest, Reset) {
  channel_.ExpectCommand("AT+CFUN=1,1").Respond("OK\r\n");
  EXPECT_TRUE(hal_->Reset().has_value());
  EXPECT_TRUE(channel_.WasCommandSent("AT+CFUN=1,1"));
}

// --- Detection ---

TEST(Ml307DetectTest, DetectsMl307Revision) {
  MockAtChannel channel;
  channel.ExpectCommand("AT\r\n").Respond("AT\r\nOK\r\n");
  channel.ExpectCommand("AT+CGMR").Respond(kCgmrMl307);

  EXPECT_TRUE(DetectMl307(channel));
}

TEST(Ml307DetectTest, RejectsOtherModule) {
  MockAtChannel channel;
  channel.ExpectCommand("AT\r\n").Respond("AT\r\nOK\r\n");
  channel.ExpectCommand("AT+CGMR").Respond("+CGMR: EC801E_V1.0\r\nOK\r\n");

  EXPECT_FALSE(DetectMl307(channel));
}

TEST(Ml307DetectTest, RejectsUnresponsiveModule) {
  MockAtChannel channel;
  channel.FailCommand("AT");

  EXPECT_FALSE(DetectMl307(channel));
}

}  // namespace
