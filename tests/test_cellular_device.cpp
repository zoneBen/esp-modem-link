#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>

#include "esp_modem_link/cellular_device.h"
#include "esp_modem_link/config_types.h"
#include "esp_modem_link/network_interface.h"
#include "esp_modem_link/uart_config.h"
#include "mock_at_channel.h"

using namespace esp_modem_link;
using esp_modem_link::testing::MockAtChannel;

namespace {

constexpr const char* kCgmrMl307 = "+CGMR: ML307R-DL-MBRH0S01\r\nOK\r\n";
constexpr const char* kCgmrOther = "+CGMR: EC801E_V1.0\r\nOK\r\n";

// Drives CellularDevice through a scripted AT channel. The device builds its
// own transport from a UART when given a config, so these tests use the
// channel-injection overload: without it the top-level class cannot be reached
// at all, which is why it stayed untested through three phases of this project.
class CellularDeviceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto mock = std::make_unique<MockAtChannel>();
    channel_ = mock.get();
    ProgramInit();
    device_ = MakeDevice(std::move(mock));
    ASSERT_TRUE(device_ != nullptr);
    // The command log is deliberately not cleared: CreateBringsTheModuleUp
    // asserts on the init sequence, which is the only record that construction
    // actually talked to the module.
  }

  // The two settings Ml307Hal::Initialize sends, which are global to the module.
  void ProgramInit() {
    channel_->ExpectCommand("ATE0").Respond("ATE0\r\nOK\r\n");
    channel_->ExpectCommand("AT+CFUN=1").Respond("AT+CFUN=1\r\nOK\r\n");
  }

  static std::unique_ptr<CellularDevice> MakeDevice(
      std::unique_ptr<MockAtChannel> mock) {
    auto result = CellularDevice::Create(ModuleType::kMl307, std::move(mock));
    EXPECT_TRUE(result.has_value())
        << (result ? "" : result.error().Message());
    return result ? std::move(*result) : nullptr;
  }

  MockAtChannel* channel_ = nullptr;
  std::unique_ptr<CellularDevice> device_;
};

// --- Construction ---

TEST_F(CellularDeviceTest, CreateBuildsDeviceWithModuleIdentity) {
  EXPECT_EQ(device_->GetModuleType(), ModuleType::kMl307);
  EXPECT_TRUE(device_->GetCapabilities().tcp);
  EXPECT_TRUE(device_->GetCapabilities().udp);
}

TEST_F(CellularDeviceTest, CreateBringsTheModuleUp) {
  EXPECT_TRUE(channel_->WasCommandSent("ATE0"));
  EXPECT_TRUE(channel_->WasCommandSent("AT+CFUN=1"));
}

TEST_F(CellularDeviceTest, CreateExposesInjectedChannel) {
  // The device must hand back the very channel it was given, not a substitute.
  EXPECT_EQ(&device_->GetAtChannel(), static_cast<void*>(channel_));
}

TEST_F(CellularDeviceTest, CreateFailsWhenModuleRejectsInit) {
  auto mock = std::make_unique<MockAtChannel>();
  mock->ExpectCommand("ATE0").Respond("ERROR\r\n");

  auto result = CellularDevice::Create(ModuleType::kMl307, std::move(mock));
  ASSERT_FALSE(result.has_value());
}

TEST_F(CellularDeviceTest, CreateRejectsUnregisteredModuleType) {
  auto mock = std::make_unique<MockAtChannel>();
  mock->ExpectCommand("ATE0").Respond("OK\r\n");
  mock->ExpectCommand("AT+CFUN=1").Respond("OK\r\n");

  // EC801E is declared in ModuleType but has no HAL yet.
  auto result = CellularDevice::Create(ModuleType::kEc801E, std::move(mock));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotSupported);
}

TEST_F(CellularDeviceTest, CreateRejectsNullChannel) {
  auto result = CellularDevice::Create(
      ModuleType::kMl307, std::unique_ptr<at_channel::IAtChannel>());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kInvalidArgument);
}

TEST_F(CellularDeviceTest, DetectIdentifiesModuleFromItsRevision) {
  auto mock = std::make_unique<MockAtChannel>();
  mock->ExpectCommand("AT+CGMR").Respond(kCgmrMl307);
  mock->ExpectCommand("ATE0").Respond("OK\r\n");
  mock->ExpectCommand("AT+CFUN=1").Respond("OK\r\n");

  auto result = CellularDevice::Detect(std::move(mock));
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ((*result)->GetModuleType(), ModuleType::kMl307);
}

TEST_F(CellularDeviceTest, DetectFailsForAnUnknownModule) {
  auto mock = std::make_unique<MockAtChannel>();
  mock->ExpectCommand("AT+CGMR").Respond(kCgmrOther);
  mock->SetDefaultResponse("ERROR\r\n");

  auto result = CellularDevice::Detect(std::move(mock));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotSupported);
}

TEST_F(CellularDeviceTest, DetectRejectsNullChannel) {
  auto result = CellularDevice::Detect(
      std::unique_ptr<at_channel::IAtChannel>());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kInvalidArgument);
}

// --- Rate alignment at the seam ---
//
// This is the ordering the library used to get wrong. The AT+IPR negotiation
// ran inside Ml307Hal::Initialize, which Detect only reaches after
// DetectModule has already tried to identify the module - and a module on
// another rate answers nothing at all, so detection failed before the
// negotiation could run. Every unit test of the negotiation passed regardless,
// because they called Initialize directly and stepped over this seam. These
// tests go through it.

// Against the old ordering this fails with "no compatible module detected":
// AT+CGMR is sent while the module is still listening at 115200.
TEST_F(CellularDeviceTest, DetectSettlesTheRateBeforeIdentifyingTheModule) {
  auto mock = std::make_unique<MockAtChannel>();
  mock->SetModuleBaudRate(115200);
  mock->SetBaudRate(921600);
  mock->ExpectCommand("AT+IPR?")
      .RespondInTurn({"+IPR: 115200\r\nOK\r\n",    // asked at 115200 and answered
                      "+IPR: 921600\r\nOK\r\n"});  // and again once it has moved
  mock->ExpectCommand("AT+IPR=?")
      .Respond("+IPR: (1200,4800,115200),(0,300,115200,921600)\r\nOK\r\n");
  mock->ExpectCommand("AT+IPR=921600").Respond("OK\r\n");
  mock->ExpectCommand("AT+CGMR").Respond(kCgmrMl307);
  mock->ExpectCommand("ATE0").Respond("OK\r\n");
  mock->ExpectCommand("AT+CFUN=1").Respond("OK\r\n");
  auto* channel = mock.get();

  auto result = CellularDevice::Detect(std::move(mock));

  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ((*result)->GetModuleType(), ModuleType::kMl307);
  EXPECT_TRUE(channel->WasCommandSent("AT+IPR=921600"));
  EXPECT_EQ(channel->GetBaudRate(), 921600);
}

// The reverse direction, which the negotiation's own comment calls the normal
// outcome of a previous run: the caller asks for the default and finds the
// module an earlier session left at 921600.
TEST_F(CellularDeviceTest, CreateBringsAModuleLeftAtARaisedRateBack) {
  auto mock = std::make_unique<MockAtChannel>();
  mock->SetModuleBaudRate(921600);
  mock->ExpectCommand("AT+IPR?")
      .RespondInTurn({"+IPR: 921600\r\nOK\r\n", "+IPR: 115200\r\nOK\r\n"});
  mock->ExpectCommand("AT+IPR=?")
      .Respond("+IPR: (1200,115200),(0,300,115200,921600)\r\nOK\r\n");
  mock->ExpectCommand("AT+IPR=115200").Respond("OK\r\n");
  mock->ExpectCommand("ATE0").Respond("OK\r\n");
  mock->ExpectCommand("AT+CFUN=1").Respond("OK\r\n");
  auto* channel = mock.get();

  auto result = CellularDevice::Create(ModuleType::kMl307, std::move(mock));

  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_TRUE(channel->WasCommandSent("AT+IPR=115200"));
  EXPECT_EQ(channel->GetBaudRate(), 115200);
}

// A device that has not been proved to be one of ours is never written to.
// AT+IPR=<rate> survives a power cycle, so writing it to an unidentified device
// would leave a stranger reconfigured - which is why the scan that finds the
// rate is read-only and the write waits for identification.
//
// Observed through a Detect that succeeds, because a Detect that fails destroys
// the channel it was handed and takes the command log with it: what is asserted
// is that the identification is on the wire before the write is.
TEST_F(CellularDeviceTest, IdentifiesTheModuleBeforeWritingARate) {
  auto mock = std::make_unique<MockAtChannel>();
  mock->SetModuleBaudRate(115200);
  mock->SetBaudRate(921600);
  mock->ExpectCommand("AT+IPR?")
      .RespondInTurn({"+IPR: 115200\r\nOK\r\n", "+IPR: 921600\r\nOK\r\n"});
  mock->ExpectCommand("AT+IPR=?")
      .Respond("+IPR: (1200,4800,115200),(0,300,115200,921600)\r\nOK\r\n");
  mock->ExpectCommand("AT+IPR=921600").Respond("OK\r\n");
  mock->ExpectCommand("AT+CGMR").Respond(kCgmrMl307);
  mock->ExpectCommand("ATE0").Respond("OK\r\n");
  mock->ExpectCommand("AT+CFUN=1").Respond("OK\r\n");
  auto* channel = mock.get();

  auto result = CellularDevice::Detect(std::move(mock));

  ASSERT_TRUE(result.has_value()) << result.error().Message();
  const auto& sent = channel->SentCommands();
  auto at_cgmr = std::find(sent.begin(), sent.end(), "AT+CGMR");
  auto write = std::find(sent.begin(), sent.end(), "AT+IPR=921600");
  ASSERT_NE(at_cgmr, sent.end());
  ASSERT_NE(write, sent.end());
  EXPECT_LT(at_cgmr, write)
      << "the rate was written before the module was identified as one of ours";
}

// The shared fixture's module answers with a bare OK and never reports a rate.
// That is a module that has been found, not one that was missing, so the scan
// stops on it and nothing is written. Reading it as "not found" would send the
// host on to a rate nothing is listening at.
TEST_F(CellularDeviceTest, LeavesTheRateAloneWhenTheModuleDoesNotReportOne) {
  EXPECT_TRUE(channel_->WasCommandSent("AT+IPR?"));
  EXPECT_FALSE(channel_->WasCommandSent("AT+IPR="));
  EXPECT_EQ(channel_->GetBaudRate(), 115200);
}

// --- Network waiting ---

TEST_F(CellularDeviceTest, WaitForNetworkReturnsHomeState) {
  channel_->ExpectCommand("AT+CEREG?").Respond("+CEREG: 0,1\r\nOK\r\n");

  auto result = device_->WaitForNetwork(std::chrono::milliseconds(500));
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(*result, RegistrationState::kRegisteredHome);
}

TEST_F(CellularDeviceTest, WaitForNetworkReturnsRoamingState) {
  channel_->ExpectCommand("AT+CEREG?").Respond("+CEREG: 0,5\r\nOK\r\n");

  auto result = device_->WaitForNetwork(std::chrono::milliseconds(500));
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(*result, RegistrationState::kRegisteredRoaming);
}

// An unregistered module must time out rather than spin forever. The poll
// interval is a second, so this costs about that much wall clock.
TEST_F(CellularDeviceTest, WaitForNetworkTimesOutWhenUnregistered) {
  channel_->ExpectCommand("AT+CEREG?").Respond("+CEREG: 0,2\r\nOK\r\n");

  auto result = device_->WaitForNetwork(std::chrono::milliseconds(1));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kTimeout);
}

TEST_F(CellularDeviceTest, WaitForNetworkReportsTransportFailure) {
  channel_->ExpectCommand("AT+CEREG?").Respond("ERROR\r\n");

  // A transport error is indistinguishable from "not yet registered" here, so
  // it keeps polling and ends in the same timeout. What matters is that it
  // returns an error instead of claiming success.
  auto result = device_->WaitForNetwork(std::chrono::milliseconds(1));
  ASSERT_FALSE(result.has_value());
}

// --- State forwarding ---

TEST_F(CellularDeviceTest, GetRegistrationStateForwards) {
  channel_->ExpectCommand("AT+CEREG?").Respond("+CEREG: 0,3\r\nOK\r\n");
  EXPECT_EQ(device_->GetRegistrationState(),
            RegistrationState::kRegistrationDenied);
}

TEST_F(CellularDeviceTest, GetRegistrationStateOnErrorIsUnknown) {
  channel_->ExpectCommand("AT+CEREG?").Respond("ERROR\r\n");
  EXPECT_EQ(device_->GetRegistrationState(), RegistrationState::kUnknown);
}

TEST_F(CellularDeviceTest, GetSignalStrengthReturnsRssi) {
  channel_->ExpectCommand("AT+CSQ").Respond("+CSQ: 23,99\r\nOK\r\n");

  auto result = device_->GetSignalStrength();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 23);
}

TEST_F(CellularDeviceTest, GetSimStateForwards) {
  channel_->ExpectCommand("AT+CPIN?").Respond("+CPIN: READY\r\nOK\r\n");
  EXPECT_EQ(device_->GetSimState(), SimState::kReady);
}

TEST_F(CellularDeviceTest, GetSimStateReportsMissingSim) {
  channel_->ExpectCommand("AT+CPIN?").Respond("+CPIN: NOT INSERTED\r\nOK\r\n");
  EXPECT_EQ(device_->GetSimState(), SimState::kNotInserted);
}

TEST_F(CellularDeviceTest, GetImeiForwards) {
  channel_->ExpectCommand("AT+CGSN=1")
      .Respond("+CGSN: 861234567890123\r\nOK\r\n");

  auto result = device_->GetImei();
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(*result, "861234567890123");
}

// ReadSingleLineResponse hands back the whole first line, so a HAL that forgot
// to strip its "+CGSN:" prefix would surface here as a corrupted identifier.
TEST_F(CellularDeviceTest, GetImeiStripsTheResponseKey) {
  channel_->ExpectCommand("AT+CGSN=1")
      .Respond("+CGSN: \"861234567890123\"\r\nOK\r\n");

  auto result = device_->GetImei();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "861234567890123");
}

TEST_F(CellularDeviceTest, GetImeiPropagatesFailure) {
  channel_->ExpectCommand("AT+CGSN=1").Respond("ERROR\r\n");

  auto result = device_->GetImei();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kAtCommandError);
}

TEST_F(CellularDeviceTest, GetIccidForwards) {
  channel_->ExpectCommand("AT+ICCID")
      .Respond("+ICCID: 89860012345678901234\r\nOK\r\n");

  auto result = device_->GetIccid();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "89860012345678901234");
}

TEST_F(CellularDeviceTest, GetModuleRevisionForwards) {
  channel_->ExpectCommand("AT+CGMR").Respond(kCgmrMl307);

  auto result = device_->GetModuleRevision();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "+CGMR: ML307R-DL-MBRH0S01");
}

TEST_F(CellularDeviceTest, GetCarrierNameForwards) {
  channel_->ExpectCommand("AT+COPS?")
      .Respond("+COPS: 0,0,\"CHN-UNICOM\",7\r\nOK\r\n");

  auto result = device_->GetCarrierName();
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(*result, "CHN-UNICOM");
}

TEST_F(CellularDeviceTest, GetCarrierNameFailsOnShortResponse) {
  channel_->ExpectCommand("AT+COPS?").Respond("+COPS: 0\r\nOK\r\n");

  auto result = device_->GetCarrierName();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kProtocolError);
}

// --- Control ---

TEST_F(CellularDeviceTest, SetFlightModeEntersFlightMode) {
  channel_->ExpectCommand("AT+CFUN=0").Respond("OK\r\n");

  EXPECT_TRUE(device_->SetFlightMode(true).has_value());
  EXPECT_TRUE(channel_->WasCommandSent("AT+CFUN=0"));
}

TEST_F(CellularDeviceTest, SetFlightModeLeavesFlightMode) {
  channel_->ExpectCommand("AT+CFUN=1").Respond("OK\r\n");

  EXPECT_TRUE(device_->SetFlightMode(false).has_value());
}

TEST_F(CellularDeviceTest, SetSleepModeEnableEntersSleep) {
  channel_->ExpectCommand("AT+SLEEP=2").Respond("OK\r\n");

  SleepConfig config;
  EXPECT_TRUE(device_->SetSleepMode(true, config).has_value());
  EXPECT_TRUE(channel_->WasCommandSent("AT+SLEEP=2"));
}

TEST_F(CellularDeviceTest, SetSleepModeDisableWakesTheModule) {
  channel_->ExpectCommand("AT+SLEEP=0").Respond("OK\r\n");

  SleepConfig config;
  EXPECT_TRUE(device_->SetSleepMode(false, config).has_value());
  EXPECT_TRUE(channel_->WasCommandSent("AT+SLEEP=0"));
}

TEST_F(CellularDeviceTest, RebootResetsTheModule) {
  channel_->ExpectCommand("AT+CFUN=1,1").Respond("OK\r\n");

  EXPECT_TRUE(device_->Reboot().has_value());
  EXPECT_TRUE(channel_->WasCommandSent("AT+CFUN=1,1"));
}

TEST_F(CellularDeviceTest, ConfigureApnForwards) {
  channel_->ExpectCommand("AT+CGDCONT=1,\"IP\",\"cmnet\"").Respond("OK\r\n");
  channel_->ExpectCommand("AT+MIPCALL?").Respond("+MIPCALL: 1\r\nOK\r\n");

  ApnConfig config;
  config.apn = "cmnet";
  EXPECT_TRUE(device_->ConfigureApn(config).has_value());
}

TEST_F(CellularDeviceTest, ConfigureApnPropagatesFailure) {
  channel_->ExpectCommand("AT+CGDCONT").Respond("ERROR\r\n");

  ApnConfig config;
  config.apn = "cmnet";
  auto result = device_->ConfigureApn(config);
  ASSERT_FALSE(result.has_value());
  // The Context names the failing step; a bare CME error says nothing.
  EXPECT_NE(result.error().context.find("AT+CGDCONT"), std::string::npos);
}

// --- Unsolicited state ---

TEST_F(CellularDeviceTest, OnNetworkStateDeliversUrcState) {
  RegistrationState seen = RegistrationState::kUnknown;
  int calls = 0;
  device_->OnNetworkState([&](RegistrationState state) {
    seen = state;
    ++calls;
  });

  channel_->InjectUrc("+CEREG: 1\r\n");

  EXPECT_EQ(calls, 1);
  EXPECT_EQ(seen, RegistrationState::kRegisteredHome);
}

TEST_F(CellularDeviceTest, OnNetworkStateReportsDeregistration) {
  RegistrationState seen = RegistrationState::kRegisteredHome;
  device_->OnNetworkState([&](RegistrationState state) { seen = state; });

  channel_->InjectUrc("+CEREG: 0\r\n");

  EXPECT_EQ(seen, RegistrationState::kNotRegistered);
}

// --- Network interface ---

TEST_F(CellularDeviceTest, GetNetworkCreatesTcpClient) {
  auto client = device_->GetNetwork().CreateTcp();
  ASSERT_TRUE(client.has_value()) << client.error().Message();
  EXPECT_NE(client->get(), nullptr);
}

TEST_F(CellularDeviceTest, GetNetworkCreatesUdpClient) {
  auto client = device_->GetNetwork().CreateUdp();
  ASSERT_TRUE(client.has_value()) << client.error().Message();
  EXPECT_NE(client->get(), nullptr);
}

TEST_F(CellularDeviceTest, GetNetworkCreatesSslClient) {
  auto client = device_->GetNetwork().CreateSsl();
  ASSERT_TRUE(client.has_value()) << client.error().Message();
  EXPECT_NE(client->get(), nullptr);
}

}  // namespace
