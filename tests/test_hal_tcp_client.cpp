#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "hal/hal_tcp_client.h"
#include "hal/imodule_hal.h"

using namespace esp_modem_link;
using namespace esp_modem_link::hal;

namespace {

class MockHal : public IModuleHal {
 public:
  ModuleType GetModuleType() const override { return ModuleType::kMl307; }
  std::string_view GetModuleName() const override { return "Mock"; }
  const ModuleCapabilities& GetCapabilities() const override {
    static const ModuleCapabilities caps{
        .tcp = true, .udp = true, .max_connections = 5, .max_baud_rate = 115200,
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

  Result<std::string> GetImei() override { return "123456789012345"; }
  Result<std::string> GetIccid() override { return "89860012345678901234"; }
  Result<std::string> GetRevision() override { return "MockFW1.0"; }
  Result<std::string> GetCarrier() override { return "MockCarrier"; }

  Result<> SetFlightMode(bool enable) override {
    (void)enable;
    return {};
  }

  // TCP
  Result<int> TcpConnect(std::string_view host,
                         uint16_t port,
                         bool ssl = false,
                         const TlsConfig& config = {}) override {
    last_host = std::string(host);
    last_port = port;
    last_ssl = ssl;
    last_tls_config = config;
    last_connect_id = next_connect_id;
    tcp_connect_calls++;
    if (fail_tcp_connect) {
      return std::unexpected(
          NetworkError(NetworkErrc::kConnectFailed, 0, "mock fail"));
    }
    return next_connect_id++;
  }

  Result<> TcpClose(int connect_id) override {
    last_close_id = connect_id;
    tcp_close_calls++;
    return {};
  }

  Result<int> TcpSend(int connect_id, const void* data, size_t len) override {
    last_send_id = connect_id;
    last_send_data.assign(static_cast<const char*>(data), len);
    tcp_send_calls++;
    if (fail_tcp_send) {
      return std::unexpected(
          NetworkError(NetworkErrc::kTransmitFailed, 0, "mock fail"));
    }
    return static_cast<int>(len);
  }

  // UDP
  Result<int> UdpOpen(std::string_view host, uint16_t port) override {
    (void)host;
    (void)port;
    return next_connect_id++;
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

  // Test controls
  bool fail_tcp_connect = false;
  bool fail_tcp_send = false;

  // Lets a test hand the next connection a cid an earlier one used, which is what
  // the pool does once a socket is released.
  void SetNextConnectId(int id) { next_connect_id = id; }

  // Drive the URC path the way the AT layer does when a +MIPURC / +MIPCLOSE line
  // arrives. The HAL does the routing, so these call the same helpers Ml307Hal
  // calls; going around them would test a path the hardware never takes.
  void EmitTcpData(int connect_id, std::string_view data) {
    DispatchTcpData(connect_id, data);
  }
  void EmitTcpClose(int connect_id) { DispatchTcpClose(connect_id); }

  // Last call records
  std::string last_host;
  uint16_t last_port = 0;
  bool last_ssl = false;
  TlsConfig last_tls_config;
  int last_connect_id = -1;
  int last_close_id = -1;
  int last_send_id = -1;
  std::string last_send_data;

  int tcp_connect_calls = 0;
  int tcp_close_calls = 0;
  int tcp_send_calls = 0;

 private:
  int next_connect_id = 0;
};

TEST(HalTcpClientTest, ConnectCallsHalTcpConnect) {
  MockHal hal;
  HalTcpClient client(hal);

  auto result = client.Connect("example.com", 80);
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(client.IsConnected());
  EXPECT_EQ(hal.tcp_connect_calls, 1);
  EXPECT_EQ(hal.last_host, "example.com");
  EXPECT_EQ(hal.last_port, 80);
  EXPECT_FALSE(hal.last_ssl);
}

TEST(HalTcpClientTest, SslConnectSetsSslFlag) {
  MockHal hal;
  HalTcpClient client(hal, true);

  auto result = client.Connect("example.com", 443);
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(hal.last_ssl);
}

// The config is held from construction and spent on the next Connect, because
// the handshake happens inside TcpConnect and nothing here can re-key an open
// socket.
TEST(HalTcpClientTest, SslConnectPassesTheTlsConfigToTheHal) {
  MockHal hal;
  TlsConfig config;
  config.verify_certificate = true;
  config.verify_hostname = true;
  config.handshake_timeout = std::chrono::seconds(45);
  HalTcpClient client(hal, true, config);

  ASSERT_TRUE(client.Connect("example.com", 443).has_value());

  EXPECT_TRUE(hal.last_tls_config.verify_certificate);
  EXPECT_TRUE(hal.last_tls_config.verify_hostname);
  EXPECT_EQ(hal.last_tls_config.handshake_timeout, std::chrono::seconds(45));
}

// The transport is not the layer that decides what a plaintext socket may carry:
// it forwards what it was constructed with, and the HAL is what refuses
// certificate material handed to a socket that will not handshake. So the config
// does arrive here, with the TLS flag off, and the assertions below are on the
// two things this class is responsible for - forwarding the value as given and
// not turning TLS on.
TEST(HalTcpClientTest, APlaintextClientForwardsItsConfigWithTlsOff) {
  MockHal hal;
  TlsConfig config;
  config.ca_cert = "PEM";
  config.handshake_timeout = std::chrono::seconds(45);
  HalTcpClient client(hal, false, config);

  ASSERT_TRUE(client.Connect("example.com", 80).has_value());

  EXPECT_FALSE(hal.last_ssl);
  EXPECT_EQ(hal.last_tls_config.ca_cert, "PEM");
  EXPECT_EQ(hal.last_tls_config.handshake_timeout, std::chrono::seconds(45));
}

TEST(HalTcpClientTest, ConnectFailureReturnsError) {
  MockHal hal;
  hal.fail_tcp_connect = true;
  HalTcpClient client(hal);

  auto result = client.Connect("example.com", 80);
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(client.IsConnected());
}

TEST(HalTcpClientTest, DoubleConnectFails) {
  MockHal hal;
  HalTcpClient client(hal);

  client.Connect("example.com", 80);
  auto result = client.Connect("other.com", 443);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kAlreadyConnected);
}

TEST(HalTcpClientTest, DisconnectCallsTcpClose) {
  MockHal hal;
  HalTcpClient client(hal);

  client.Connect("example.com", 80);
  client.Disconnect();
  EXPECT_FALSE(client.IsConnected());
  EXPECT_EQ(hal.tcp_close_calls, 1);
  EXPECT_EQ(hal.last_close_id, 0);
}

TEST(HalTcpClientTest, SendCallsTcpSend) {
  MockHal hal;
  HalTcpClient client(hal);

  client.Connect("example.com", 80);
  const char data[] = "hello";
  auto result = client.Send(data, 5);
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 5);
  EXPECT_EQ(hal.tcp_send_calls, 1);
  EXPECT_EQ(hal.last_send_data, "hello");
}

TEST(HalTcpClientTest, SendWhenNotConnectedFails) {
  MockHal hal;
  HalTcpClient client(hal);

  const char data[] = "hello";
  auto result = client.Send(data, 5);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotConnected);
}

TEST(HalTcpClientTest, SendFailurePropagatesError) {
  MockHal hal;
  hal.fail_tcp_send = true;
  HalTcpClient client(hal);

  client.Connect("example.com", 80);
  const char data[] = "hello";
  auto result = client.Send(data, 5);
  EXPECT_FALSE(result.has_value());
}

TEST(HalTcpClientTest, DestructorDisconnects) {
  MockHal hal;
  {
    HalTcpClient client(hal);
    client.Connect("example.com", 80);
    EXPECT_EQ(hal.tcp_close_calls, 0);
  }
  EXPECT_EQ(hal.tcp_close_calls, 1);
}

TEST(HalTcpClientTest, DisconnectWhenNotConnectedIsNoop) {
  MockHal hal;
  HalTcpClient client(hal);
  client.Disconnect();
  EXPECT_EQ(hal.tcp_close_calls, 0);
}

TEST(HalTcpClientTest, GetSendBufferFreeReturnsMax) {
  MockHal hal;
  HalTcpClient client(hal);
  EXPECT_EQ(client.GetSendBufferFree(), SIZE_MAX);
}

// Everything below exercises the unsolicited half of the client. A payload that
// the HAL decodes but no route was registered for is silently dropped, so these
// assert delivery end to end rather than that a subscription call did not crash.

TEST(HalTcpClientTest, InboundDataReachesOnData) {
  MockHal hal;
  HalTcpClient client(hal);

  std::string received;
  client.OnData([&](std::string_view data) { received = std::string(data); });
  ASSERT_TRUE(client.Connect("example.com", 80).has_value());

  hal.EmitTcpData(0, "HTTP/1.1 200 OK");

  EXPECT_EQ(received, "HTTP/1.1 200 OK");
}

TEST(HalTcpClientTest, InboundDataForAnotherConnectionIsIgnored) {
  MockHal hal;
  HalTcpClient client(hal);

  bool called = false;
  client.OnData([&](std::string_view) { called = true; });
  ASSERT_TRUE(client.Connect("example.com", 80).has_value());

  // The cid is the only thing the module names, so the route is what keeps one
  // client's traffic away from another's.
  hal.EmitTcpData(1, "not ours");

  EXPECT_FALSE(called);
}

TEST(HalTcpClientTest, HalCloseReachesOnDisconnected) {
  MockHal hal;
  HalTcpClient client(hal);

  bool closed = false;
  client.OnDisconnected([&] { closed = true; });
  ASSERT_TRUE(client.Connect("example.com", 80).has_value());

  // Peer-initiated close, so the client learns about it without Disconnect().
  hal.EmitTcpClose(0);

  EXPECT_TRUE(closed);
  EXPECT_FALSE(client.IsConnected());
}

TEST(HalTcpClientTest, HalCloseForAnotherConnectionIsIgnored) {
  MockHal hal;
  HalTcpClient client(hal);

  bool closed = false;
  client.OnDisconnected([&] { closed = true; });
  ASSERT_TRUE(client.Connect("example.com", 80).has_value());

  hal.EmitTcpClose(3);

  EXPECT_FALSE(closed);
  EXPECT_TRUE(client.IsConnected());
}

// Our own Disconnect() also makes the module answer with a close, which must not
// surface as a second, apparently peer-initiated, disconnect.
TEST(HalTcpClientTest, DisconnectNotifiesOnDisconnectedOnce) {
  MockHal hal;
  HalTcpClient client(hal);

  int count = 0;
  client.OnDisconnected([&] { ++count; });
  ASSERT_TRUE(client.Connect("example.com", 80).has_value());

  client.Disconnect();
  hal.EmitTcpClose(0);  // the module's answer to AT+MIPCLOSE

  EXPECT_EQ(count, 1);
}

// Two clients on one HAL. With a single slot for inbound events the second
// Connect took the routes away from the first, leaving it connected and deaf -
// which is why routing is per connection rather than per HAL.
TEST(HalTcpClientTest, TwoClientsEachReceiveTheirOwnTraffic) {
  MockHal hal;
  HalTcpClient first(hal);
  HalTcpClient second(hal);

  std::string first_got;
  std::string second_got;
  first.OnData([&](std::string_view data) { first_got = std::string(data); });
  second.OnData([&](std::string_view data) { second_got = std::string(data); });
  ASSERT_TRUE(first.Connect("a.example", 80).has_value());
  ASSERT_TRUE(second.Connect("b.example", 80).has_value());

  hal.EmitTcpData(1, "for the second");
  hal.EmitTcpData(0, "for the first");

  EXPECT_EQ(first_got, "for the first");
  EXPECT_EQ(second_got, "for the second");
}

TEST(HalTcpClientTest, CloseOnOneConnectionLeavesTheOtherConnected) {
  MockHal hal;
  HalTcpClient first(hal);
  HalTcpClient second(hal);

  bool first_closed = false;
  bool second_closed = false;
  first.OnDisconnected([&] { first_closed = true; });
  second.OnDisconnected([&] { second_closed = true; });
  ASSERT_TRUE(first.Connect("a.example", 80).has_value());
  ASSERT_TRUE(second.Connect("b.example", 80).has_value());

  hal.EmitTcpClose(0);

  EXPECT_TRUE(first_closed);
  EXPECT_FALSE(second_closed);
  EXPECT_FALSE(first.IsConnected());
  EXPECT_TRUE(second.IsConnected());
}

// A route is looked up under a lock, but the handler is called after it is
// released: opening or closing a connection from inside a data callback is
// something a client legitimately does, and it takes that same lock. A dispatch
// that called under the lock would self-deadlock, which MSVC's mutex reports as a
// failure rather than blocking on.
TEST(HalTcpClientTest, ADataHandlerMayOpenAnotherConnection) {
  MockHal hal;
  HalTcpClient first(hal);
  HalTcpClient second(hal);

  bool opened_from_callback = false;
  first.OnData([&](std::string_view) {
    opened_from_callback = second.Connect("b.example", 80).has_value();
  });
  ASSERT_TRUE(first.Connect("a.example", 80).has_value());

  hal.EmitTcpData(0, "hello");

  EXPECT_TRUE(opened_from_callback);
}

// Why a subscription is named by a handle rather than by its cid: a cid goes back
// into the pool the moment the module closes the socket, and the client that used
// to hold it is still around. Withdrawing is scoped to the handle, so the client
// that left cannot take the route belonging to whoever holds the cid now.
TEST(HalTcpClientTest, WithdrawingLeavesAnotherRouteOnTheSameCidAlone) {
  MockHal hal;
  auto leaving = std::make_unique<HalTcpClient>(hal);
  ASSERT_TRUE(leaving->Connect("a.example", 80).has_value());

  // The peer closes, so the client learns and the cid goes back into the pool -
  // without the client itself having been destroyed yet.
  hal.EmitTcpClose(0);
  ASSERT_FALSE(leaving->IsConnected());

  hal.SetNextConnectId(0);
  HalTcpClient replacement(hal);
  std::string received;
  replacement.OnData(
      [&](std::string_view data) { received = std::string(data); });
  ASSERT_TRUE(replacement.Connect("b.example", 80).has_value());
  ASSERT_EQ(hal.last_connect_id, 0);

  leaving.reset();

  hal.EmitTcpData(0, "for the replacement");

  EXPECT_EQ(received, "for the replacement");
}

// What withdrawal is for: the HAL outlives its clients, so a route that survives
// its client is a call into freed memory rather than a leak. Either of the two
// paths out of the destructor would clear this on its own; removing both is what
// this catches.
TEST(HalTcpClientTest, ADestroyedClientStopsReceiving) {
  MockHal hal;
  int observed = 0;
  {
    HalTcpClient dead(hal);
    // Written through only if the route outlives the client.
    dead.OnData([&](std::string_view) { observed = -1; });
    ASSERT_TRUE(dead.Connect("example.com", 80).has_value());
  }

  // The pool hands a released cid straight back out, so the next connection can
  // land on the same one.
  hal.SetNextConnectId(0);
  HalTcpClient live(hal);
  live.OnData([&](std::string_view) { ++observed; });
  ASSERT_TRUE(live.Connect("example.com", 80).has_value());

  hal.EmitTcpData(0, "hello");

  EXPECT_EQ(observed, 1);
}

}  // namespace
