#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

#include "hal/imodule_hal.h"
#include "mock_at_channel.h"
#include "modules/ml307/ml307_hal.h"
#include "network/cellular_network.h"
#include "protocol/mqtt/software_mqtt_client.h"
#include "protocol/websocket/software_ws_client.h"

using namespace esp_modem_link;
using namespace esp_modem_link::hal;
using namespace esp_modem_link::network;
using esp_modem_link::modules::ml307::Ml307Hal;
using esp_modem_link::testing::MockAtChannel;

namespace {

class FullCapsHal : public IModuleHal {
 public:
  // What the last TcpConnect was asked for. Public because the subclasses below
  // inherit it and the tests read it through them.
  bool last_ssl = false;
  TlsConfig last_tls_config;
  bool fail_tcp_connect = false;

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

  Result<int> TcpConnect(std::string_view host,
                         uint16_t port,
                         bool ssl = false,
                         const TlsConfig& config = {}) override {
    (void)host;
    (void)port;
    last_ssl = ssl;
    // Recorded rather than ignored: what the engines above hand down is the
    // subject of the TLS tests, and a stub that dropped it could not tell a
    // config that was plumbed through from one that never left the caller.
    last_tls_config = config;
    // Refusing the socket is how a test that only cares what the engine asked
    // for avoids waiting out a response that no one is going to send.
    if (fail_tcp_connect) {
      return std::unexpected(
          NetworkError(NetworkErrc::kConnectFailed, 0, "mock refuses"));
    }
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

  Result<int> TcpConnect(std::string_view,
                         uint16_t,
                         bool = false,
                         const TlsConfig& = {}) override {
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

// The smallest thing that is not the software engine, so a test can tell which
// of the two CreateHttp produced.
class StubHttpClient : public HttpClient {
 public:
  void SetTimeout(std::chrono::milliseconds) override {}
  void SetHeader(std::string_view, std::string_view) override {}
  void SetBody(std::string) override {}
  void SetKeepAlive(bool) override {}
  void SetTlsConfig(const TlsConfig&) override {}
  Result<HttpResponse> Execute(std::string_view, std::string_view) override {
    return HttpResponse{};
  }
  Result<> Open(std::string_view, std::string_view) override { return {}; }
  Result<int> Read(void*, size_t) override { return 0; }
  Result<int> Write(const void*, size_t) override { return 0; }
  void Close() override {}
  Result<int> GetStatusCode() override { return 200; }
  std::string GetResponseHeader(std::string_view) const override {
    return {};
  }
  size_t GetContentLength() const override { return 0; }
  bool IsChunked() const override { return false; }
};

// A module whose firmware HTTP stack does come up.
class BuiltinHttpHal : public FullCapsHal {
 public:
  Result<std::unique_ptr<HttpClient>> CreateBuiltinHttp() override {
    return std::unique_ptr<HttpClient>(std::make_unique<StubHttpClient>());
  }
};

TEST(CellularNetworkTest, CreateHttpPrefersBuiltinWhenItComesUp) {
  BuiltinHttpHal hal;
  CellularNetwork net(hal);

  auto result = net.CreateHttp();
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_NE(dynamic_cast<StubHttpClient*>(result->get()), nullptr);
}

// FullCapsHal claims a builtin HTTP stack and then cannot produce a client - the
// state the ML307 HAL itself was in. Auto mode asked for the builtin as a
// preference, so a failure there must not fail the request: the module can still
// carry HTTP/1.1 over a raw socket.
TEST(CellularNetworkTest, CreateHttpFallsBackToSoftwareWhenBuiltinFails) {
  FullCapsHal hal;
  CellularNetwork net(hal);

  auto result = net.CreateHttp();
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  // Anything but the builtin path, which by construction produced nothing.
  EXPECT_EQ(dynamic_cast<StubHttpClient*>(result->get()), nullptr);
}

// The fallback is Auto's privilege, not a blanket rule. A caller who named the
// builtin engine asked for that engine, and silently running something else
// would hide the reason their request never reached the firmware stack.
TEST(CellularNetworkTest, CreateHttpReportsFailureWhenBuiltinIsRequired) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  net.SetProtocolMode(ProtocolMode::kBuiltin);

  auto result = net.CreateHttp();
  EXPECT_FALSE(result.has_value());
}

TEST(CellularNetworkTest, CreateWebSocketIsAlwaysTheSoftwareEngine) {
  FullCapsHal hal;
  CellularNetwork net(hal);

  auto client = net.CreateWebSocket();
  ASSERT_TRUE(client.has_value()) << client.error().Message();
  // Which engine came back is the whole point: no module has a WebSocket stack
  // of its own, so a protocol mode that asks for one changes nothing here.
  EXPECT_NE(dynamic_cast<protocol::SoftwareWsClient*>(client.value().get()),
            nullptr);

  net.SetProtocolMode(ProtocolMode::kBuiltin);
  auto forced = net.CreateWebSocket();
  ASSERT_TRUE(forced.has_value()) << forced.error().Message();
  EXPECT_NE(dynamic_cast<protocol::SoftwareWsClient*>(forced.value().get()),
            nullptr);
}

TEST(CellularNetworkTest, HasCapabilityTcp) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  EXPECT_TRUE(net.HasCapability(NetworkProtocol::kTcp));
}

// WebSocket has no builtin path on any module, so the only question the module
// can answer is whether it can carry the socket underneath one.
TEST(CellularNetworkTest, HasCapabilityWebSocket) {
  TcpOnlyHal hal;
  CellularNetwork net(hal);
  EXPECT_TRUE(net.HasCapability(NetworkProtocol::kWebSocket));

  // A module with TCP but no TLS cannot carry wss://, which is a different
  // question from whether it can carry ws://.
  EXPECT_FALSE(net.HasCapability(NetworkProtocol::kWss));

  NoCapsHal bare;
  CellularNetwork none(bare);
  EXPECT_FALSE(none.HasCapability(NetworkProtocol::kWebSocket));
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

// The whole defect, end to end and on the real HAL: ML307 claimed a builtin HTTP
// stack it could not construct, and Auto mode took that claim at face value, so
// CreateHttp() failed outright on the one module this library currently drives.
// The mock HALs above can only state the rule; this states that the module obeys
// it.
TEST(CellularNetworkTest, CreateHttpOnMl307GivesAWorkingClient) {
  MockAtChannel channel;
  channel.ExpectCommand("ATE0").Respond("OK\r\n");
  channel.ExpectCommand("AT+CFUN=1").Respond("OK\r\n");

  Ml307Hal hal;
  ASSERT_TRUE(hal.Initialize(channel).has_value());

  CellularNetwork net(hal);
  auto client = net.CreateHttp();
  ASSERT_TRUE(client.has_value()) << client.error().Message();
  EXPECT_NE(client.value(), nullptr);
}


// --- MQTT engine selection ------------------------------------------------

// Same rule as HTTP, on the MQTT path: a builtin stack that cannot be built
// does not end the request in Auto mode, because the module can still carry
// MQTT over a raw socket.
TEST(CellularNetworkTest, AutoFallsBackToSoftwareMqttWhenTheBuiltinFails) {
  FullCapsHal hal;  // claims a builtin MQTT stack, and cannot build one
  CellularNetwork net(hal);

  auto client = net.CreateMqtt();
  ASSERT_TRUE(client.has_value()) << client.error().Message();
  // Which engine came back is the whole point: a client that merely exists
  // could be a stub.
  EXPECT_NE(dynamic_cast<protocol::SoftwareMqttClient*>(client.value().get()),
            nullptr);
}

TEST(CellularNetworkTest, AnExplicitBuiltinMqttFailureIsReported) {
  FullCapsHal hal;
  CellularNetwork net(hal);
  net.SetProtocolMode(ProtocolMode::kBuiltin);

  auto client = net.CreateMqtt();
  ASSERT_FALSE(client.has_value());
  EXPECT_EQ(client.error().code, NetworkErrc::kNotSupported);
}

TEST(CellularNetworkTest, ATcpOnlyModuleGetsTheSoftwareMqttEngine) {
  TcpOnlyHal hal;
  CellularNetwork net(hal);

  auto client = net.CreateMqtt();
  ASSERT_TRUE(client.has_value()) << client.error().Message();
  EXPECT_NE(dynamic_cast<protocol::SoftwareMqttClient*>(client.value().get()),
            nullptr);
}

TEST(CellularNetworkTest, MqttOnAModuleWithNoTcpAtAllFails) {
  NoCapsHal hal;
  CellularNetwork net(hal);

  auto client = net.CreateMqtt();
  ASSERT_FALSE(client.has_value());
  EXPECT_EQ(client.error().code, NetworkErrc::kNotSupported);
  // The message says which of the two ways out was missing, because "MQTT not
  // available" on a module that has a socket but no MQTT is a different
  // problem to go and solve.
  EXPECT_NE(client.error().context.find("nor TCP"), std::string::npos);
}

TEST(CellularNetworkTest, MqttsOverAModuleWithoutTlsFails) {
  TcpOnlyHal hal;
  CellularNetwork net(hal);

  auto client = net.CreateMqtt();
  ASSERT_TRUE(client.has_value()) << client.error().Message();

  TlsConfig config;
  client.value()->SetTlsConfig(config);
  auto result = client.value()->Connect("broker.example", 8883);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotSupported);
}

// The one module this library currently drives, end to end: ML307 has no MQTT
// stack of its own, so it must get the software one rather than an error.
TEST(CellularNetworkTest, CreateMqttOnMl307GivesAWorkingClient) {
  MockAtChannel channel;
  channel.ExpectCommand("ATE0").Respond("OK\r\n");
  channel.ExpectCommand("AT+CFUN=1").Respond("OK\r\n");

  Ml307Hal hal;
  ASSERT_TRUE(hal.Initialize(channel).has_value());

  CellularNetwork net(hal);
  auto client = net.CreateMqtt();
  ASSERT_TRUE(client.has_value()) << client.error().Message();
  EXPECT_NE(dynamic_cast<protocol::SoftwareMqttClient*>(client.value().get()),
            nullptr);
}

// --- WebSocket engine selection -------------------------------------------

TEST(CellularNetworkTest, AWebSocketOnAModuleWithNoTcpAtAllFails) {
  NoCapsHal hal;
  CellularNetwork net(hal);

  auto client = net.CreateWebSocket();
  ASSERT_FALSE(client.has_value());
  EXPECT_EQ(client.error().code, NetworkErrc::kNotSupported);
}

// The scheme is what asks for TLS, and a module without it cannot carry wss://.
// The failure belongs to the connection rather than to the client's
// construction, because ws:// would still have worked.
TEST(CellularNetworkTest, WssOverAModuleWithoutTlsFails) {
  TcpOnlyHal hal;
  CellularNetwork net(hal);

  auto client = net.CreateWebSocket();
  ASSERT_TRUE(client.has_value()) << client.error().Message();

  auto result = client.value()->Connect("wss://server.example/chat");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotSupported);
}

TEST(CellularNetworkTest, CreateWebSocketOnMl307GivesAWorkingClient) {
  MockAtChannel channel;
  channel.ExpectCommand("ATE0").Respond("OK\r\n");
  channel.ExpectCommand("AT+CFUN=1").Respond("OK\r\n");

  Ml307Hal hal;
  ASSERT_TRUE(hal.Initialize(channel).has_value());

  CellularNetwork net(hal);
  auto client = net.CreateWebSocket();
  ASSERT_TRUE(client.has_value()) << client.error().Message();
  EXPECT_NE(dynamic_cast<protocol::SoftwareWsClient*>(client.value().get()),
            nullptr);
}

// --- TLS reaches the socket -------------------------------------------------

// A TlsConfig the caller sets on an engine is only worth anything if the engine
// hands it to the socket it opens. Both engines take it at the point the socket
// is made - HTTPS at the request's scheme, wss:// at Connect() - so this asserts
// it arrives, not merely that SetTlsConfig compiled.

TEST(CellularNetworkTest, HttpsCarriesTheCallersTlsConfigDownToTheSocket) {
  FullCapsHal hal;
  hal.fail_tcp_connect = true;  // nothing is going to answer a request anyway
  CellularNetwork net(hal);
  net.SetProtocolMode(ProtocolMode::kSoftware);
  auto client = net.CreateHttp();
  ASSERT_TRUE(client.has_value()) << client.error().Message();

  TlsConfig config;
  config.verify_certificate = true;
  config.verify_hostname = true;
  client.value()->SetTlsConfig(config);

  auto result = client.value()->Execute("GET", "https://example.com/");
  ASSERT_FALSE(result.has_value());

  EXPECT_TRUE(hal.last_ssl);
  EXPECT_TRUE(hal.last_tls_config.verify_certificate);
  EXPECT_TRUE(hal.last_tls_config.verify_hostname);
}

TEST(CellularNetworkTest, WssCarriesTheCallersTlsConfigDownToTheSocket) {
  FullCapsHal hal;
  hal.fail_tcp_connect = true;
  CellularNetwork net(hal);
  auto client = net.CreateWebSocket();
  ASSERT_TRUE(client.has_value()) << client.error().Message();

  TlsConfig config;
  config.verify_certificate = true;
  config.verify_hostname = true;
  client.value()->SetTlsConfig(config);

  auto result = client.value()->Connect("wss://server.example/chat");
  ASSERT_FALSE(result.has_value());

  EXPECT_TRUE(hal.last_ssl);
  EXPECT_TRUE(hal.last_tls_config.verify_certificate);
  EXPECT_TRUE(hal.last_tls_config.verify_hostname);
}

// The plaintext half of both pairs. The config a caller sets describes the TLS
// connections that client makes, so it must not reach a socket that is not going
// to handshake: an honest HAL refuses certificate material handed to a plaintext
// socket, so forwarding it would fail an http:// request with settings the
// caller never meant to apply to it.
TEST(CellularNetworkTest, APlaintextRequestIsNotGivenATlsConfig) {
  FullCapsHal hal;
  hal.fail_tcp_connect = true;
  CellularNetwork net(hal);
  net.SetProtocolMode(ProtocolMode::kSoftware);
  auto client = net.CreateHttp();
  ASSERT_TRUE(client.has_value()) << client.error().Message();

  TlsConfig config;
  config.ca_cert = "-----BEGIN CERTIFICATE-----";
  config.handshake_timeout = std::chrono::seconds(45);
  client.value()->SetTlsConfig(config);

  auto result = client.value()->Execute("GET", "http://example.com/");
  ASSERT_FALSE(result.has_value());

  EXPECT_FALSE(hal.last_ssl);
  EXPECT_TRUE(hal.last_tls_config.ca_cert.empty());
  EXPECT_EQ(hal.last_tls_config.handshake_timeout, std::chrono::seconds(10));
}

TEST(CellularNetworkTest, APlainWebSocketIsNotGivenATlsConfig) {
  FullCapsHal hal;
  hal.fail_tcp_connect = true;
  CellularNetwork net(hal);
  auto client = net.CreateWebSocket();
  ASSERT_TRUE(client.has_value()) << client.error().Message();

  TlsConfig config;
  config.ca_cert = "-----BEGIN CERTIFICATE-----";
  config.handshake_timeout = std::chrono::seconds(45);
  client.value()->SetTlsConfig(config);

  auto result = client.value()->Connect("ws://server.example/chat");
  ASSERT_FALSE(result.has_value());

  EXPECT_FALSE(hal.last_ssl);
  EXPECT_TRUE(hal.last_tls_config.ca_cert.empty());
  EXPECT_EQ(hal.last_tls_config.handshake_timeout, std::chrono::seconds(10));
}

}  // namespace
