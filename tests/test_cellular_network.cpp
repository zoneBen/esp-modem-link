#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "hal/imodule_hal.h"
#include "network/cellular_network.h"

using namespace esp_modem_link;
using namespace esp_modem_link::hal;
using namespace esp_modem_link::network;

namespace {

class FullCapsHal : public IModuleHal {
 public:
  ModuleType GetModuleType() const override { return ModuleType::kMl307; }
  std::string_view GetModuleName() const override { return "FullCaps"; }
  const ModuleCapabilities& GetCapabilities() const override {
    static const ModuleCapabilities caps{
        .tcp = true,
        .udp = true,
        .ssl_tcp = true,
        .http = true,
        .https = true,
        .mqtt = true,
        .mqtts = true,
        .max_connections = 8,
        .max_baud_rate = 921600,
    };
    return caps;
  }
  Result<> Initialize(at_channel::IAtChannel& channel) override {
    (void)channel;
    return {};
  }
  Result<> Reset() override { return {}; }

  SimState GetSimState() override { return SimState::kReady; }
  Result<> ConfigureApn(const ApnConfig& config) override {
    (void)config;
    return {};
  }
  RegistrationState GetRegistrationState() override {
    return RegistrationState::kRegisteredHome;
  }
  SignalInfo GetSignalInfo() override { return {31, 99}; }

  Result<std::string> GetImei() override { return "123"; }
  Result<std::string> GetIccid() override { return "456"; }
  Result<std::string> GetRevision() override { return "1.0"; }
  Result<std::string> GetCarrier() override { return "Test"; }

  Result<> SetFlightMode(bool enable) override {
    (void)enable;
    return {};
  }

  Result<int> TcpConnect(std::string_view host, uint16_t port,
                         bool ssl = false) override {
    (void)host;
    (void)port;
    (void)ssl;
    return next_id++;
  }
  Result<> TcpClose(int connect_id) override {
    (void)connect_id;
    return {};
  }
  Result<int> TcpSend(int connect_id, const void* data, size_t len) override {
    (void)connect_id;
    (void)data;
    return static_cast<int>(len);
  }

  Result<int> UdpOpen(std::string_view host, uint16_t port) override {
    (void)host;
    (void)port;
    return next_id++;
  }
  Result<> UdpClose(int connect_id) override {
    (void)connect_id;
    return {};
  }
  Result<int> UdpSend(int connect_id, const void* data, size_t len) override {
    (void)connect_id;
    (void)data;
    return static_cast<int>(len);
  }

  bool HasBuiltinHttp() const override { return true; }
  bool HasBuiltinMqtt() const override { return true; }

  Result<std::unique_ptr<HttpClient>> CreateBuiltinHttp() override {
    return std::unexpected(
        NetworkError::NotSupported("mock builtin HTTP"));
  }
  Result<std::unique_ptr<MqttClient>> CreateBuiltinMqtt() override {
    return std::unexpected(
        NetworkError::NotSupported("mock builtin MQTT"));
  }

 private:
  int next_id = 0;
};

// Sockets but no protocol stacks of its own: the shape of module the software
// engines exist for. No TLS either, so HTTPS has no transport.
class TcpOnlyHal : public FullCapsHal {
 public:
  const ModuleCapabilities& GetCapabilities() const override {
    static const ModuleCapabilities caps{
        .tcp = true,
        .max_connections = 8,
    };
    return caps;
  }
  bool HasBuiltinHttp() const override { return false; }
  bool HasBuiltinMqtt() const override { return false; }
};

class NoCapsHal : public IModuleHal {
 public:
  ModuleType GetModuleType() const override { return ModuleType::kUnknown; }
  std::string_view GetModuleName() const override { return "NoCaps"; }
  const ModuleCapabilities& GetCapabilities() const override {
    static const ModuleCapabilities caps{};
    return caps;
  }
  Result<> Initialize(at_channel::IAtChannel& channel) override {
    (void)channel;
    return {};
  }
  Result<> Reset() override { return {}; }

  SimState GetSimState() override { return SimState::kUnknown; }
  Result<> ConfigureApn(const ApnConfig& config) override {
    (void)config;
    return {};
  }
  RegistrationState GetRegistrationState() override {
    return RegistrationState::kNotRegistered;
  }
  SignalInfo GetSignalInfo() override { return {99, 99}; }

  Result<std::string> GetImei() override { return ""; }
  Result<std::string> GetIccid() override { return ""; }
  Result<std::string> GetRevision() override { return ""; }
  Result<std::string> GetCarrier() override { return ""; }

  Result<> SetFlightMode(bool enable) override {
    (void)enable;
    return {};
  }

  Result<int> TcpConnect(std::string_view, uint16_t, bool = false) override {
    return std::unexpected(NetworkError::NotSupported("no tcp"));
  }
  Result<> TcpClose(int) override { return {}; }
  Result<int> TcpSend(int, const void*, size_t) override {
    return std::unexpected(NetworkError::NotSupported("no tcp"));
  }

  Result<int> UdpOpen(std::string_view, uint16_t) override {
    return std::unexpected(NetworkError::NotSupported("no udp"));
  }
  Result<> UdpClose(int) override { return {}; }
  Result<int> UdpSend(int, const void*, size_t) override {
    return std::unexpected(NetworkError::NotSupported("no udp"));
  }
};

TEST(CellularNetworkTest, CreateTcpSucceedsWithFullCaps) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  auto result = net.CreateTcp();
  EXPECT_TRUE(result.has_value());
  EXPECT_NE(result.value(), nullptr);
}

TEST(CellularNetworkTest, CreateTcpFailsWithoutTcpCap) {
  NoCapsHal hal;
  CellularNetwork net(hal);
  auto result = net.CreateTcp();
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotSupported);
}

TEST(CellularNetworkTest, CreateSslSucceedsWithSslCap) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  auto result = net.CreateSsl();
  EXPECT_TRUE(result.has_value());
}

TEST(CellularNetworkTest, CreateSslFailsWithoutSslCap) {
  NoCapsHal hal;
  CellularNetwork net(hal);
  auto result = net.CreateSsl();
  EXPECT_FALSE(result.has_value());
}

TEST(CellularNetworkTest, CreateUdpSucceedsWithUdpCap) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  auto result = net.CreateUdp();
  EXPECT_TRUE(result.has_value());
}

TEST(CellularNetworkTest, CreateUdpFailsWithoutUdpCap) {
  NoCapsHal hal;
  CellularNetwork net(hal);
  auto result = net.CreateUdp();
  EXPECT_FALSE(result.has_value());
}

TEST(CellularNetworkTest, CreateHttpUsesBuiltin) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  // FullCapsHal has builtin HTTP but CreateBuiltinHttp returns NotSupported
  auto result = net.CreateHttp();
  // Should try builtin, which returns NotSupported from our mock
  EXPECT_FALSE(result.has_value());
}

TEST(CellularNetworkTest, CreateWebSocketNotSupported) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  auto result = net.CreateWebSocket();
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotSupported);
}

TEST(CellularNetworkTest, HasCapabilityTcp) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  EXPECT_TRUE(net.HasCapability(NetworkProtocol::kTcp));
}

TEST(CellularNetworkTest, HasCapabilityWebSocketFalse) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  EXPECT_FALSE(net.HasCapability(NetworkProtocol::kWebSocket));
}

TEST(CellularNetworkTest, GetMaxConnections) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  EXPECT_EQ(net.GetMaxConnections(NetworkProtocol::kTcp), 8);
  EXPECT_EQ(net.GetMaxConnections(NetworkProtocol::kUdp), 8);
}

TEST(CellularNetworkTest, DefaultProtocolModeIsAuto) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  EXPECT_EQ(net.GetProtocolMode(), ProtocolMode::kAuto);
}

TEST(CellularNetworkTest, SetProtocolMode) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  net.SetProtocolMode(ProtocolMode::kSoftware);
  EXPECT_EQ(net.GetProtocolMode(), ProtocolMode::kSoftware);
}

// Software mode is a request to route around the firmware's HTTP stack, so the
// builtin one must not be consulted even though it is available.
TEST(CellularNetworkTest, SoftwareModeSkipsBuiltinHttp) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  net.SetProtocolMode(ProtocolMode::kSoftware);
  auto result = net.CreateHttp();
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_NE(result.value(), nullptr);
}

// A module with TCP but no HTTP stack of its own still gets HTTP, which is the
// whole point of having a software engine.
TEST(CellularNetworkTest, AutoModeFallsBackToSoftwareHttp) {
  TcpOnlyHal hal;
  CellularNetwork net(hal);
  auto result = net.CreateHttp();
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_NE(result.value(), nullptr);
}

// Without TCP there is nothing for the software engine to run on, and no
// builtin stack either, so this has to fail rather than hand back a client that
// cannot send anything.
TEST(CellularNetworkTest, HttpFailsWhenNeitherBuiltinNorTcpIsAvailable) {
  NoCapsHal hal;
  CellularNetwork net(hal);
  auto result = net.CreateHttp();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotSupported);
}

// HTTPS over a module without TLS has no transport, and the failure belongs to
// the request rather than to the client's construction.
TEST(CellularNetworkTest, HttpsOverAModuleWithoutTlsFails) {
  TcpOnlyHal hal;
  CellularNetwork net(hal);
  net.SetProtocolMode(ProtocolMode::kSoftware);
  auto client = net.CreateHttp();
  ASSERT_TRUE(client.has_value()) << client.error().Message();

  auto result = client.value()->Execute("GET", "https://example.com/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotSupported);
}

}  // namespace
