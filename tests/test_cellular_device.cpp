#include <gtest/gtest.h>

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
