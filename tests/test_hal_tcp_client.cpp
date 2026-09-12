#include <gtest/gtest.h>

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
                         bool ssl = false) override {
    last_host = std::string(host);
    last_port = port;
    last_ssl = ssl;
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

  // Drive the callbacks the client installed, the way the AT layer does when a
  // +MIPURC / +MIPCLOSE URC arrives. Without this the client's unsolicited data
  // path is untestable, which is how it once shipped complete but unconnected.
  void EmitTcpData(int connect_id, std::string_view data) {
    if (on_tcp_data_) on_tcp_data_(connect_id, data);
  }
  void EmitTcpClose(int connect_id) {
    if (on_tcp_close_) on_tcp_close_(connect_id);
  }
  bool HasTcpCallbacks() const {
    return static_cast<bool>(on_tcp_data_) &&
           static_cast<bool>(on_tcp_close_);
  }

  // Last call records
  std::string last_host;
  uint16_t last_port = 0;
  bool last_ssl = false;
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
// the HAL decodes but the client never subscribes to is silently dropped, so
// these assert delivery end to end rather than that a setter did not crash.

TEST(HalTcpClientTest, ConnectRegistersCallbacksWithHal) {
  MockHal hal;
  HalTcpClient client(hal);

  ASSERT_TRUE(client.Connect("example.com", 80).has_value());
  EXPECT_TRUE(hal.HasTcpCallbacks());
}

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

  // The HAL reports every socket; a payload for someone else's is not ours.
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

TEST(HalTcpClientTest, DestructorUnregistersCallbacks) {
  MockHal hal;
  {
    HalTcpClient client(hal);
    client.Connect("example.com", 80);
    EXPECT_TRUE(hal.HasTcpCallbacks());
  }
  // The HAL outlives the client, so a leftover closure would dangle.
  EXPECT_FALSE(hal.HasTcpCallbacks());
}

}  // namespace
