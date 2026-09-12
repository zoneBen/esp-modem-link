#include <gtest/gtest.h>

#include <string>

#include "hal/hal_udp_client.h"
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

  Result<int> TcpConnect(std::string_view host,
                         uint16_t port,
                         bool ssl = false) override {
    (void)host;
    (void)port;
    (void)ssl;
    return next_connect_id++;
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

  // UDP
  Result<int> UdpOpen(std::string_view host, uint16_t port) override {
    last_open_host = std::string(host);
    last_open_port = port;
    udp_open_calls++;
    if (fail_udp_open) {
      return std::unexpected(
          NetworkError(NetworkErrc::kConnectFailed, 0, "mock fail"));
    }
    return next_connect_id++;
  }
  Result<> UdpClose(int connect_id) override {
    last_close_id = connect_id;
    udp_close_calls++;
    return {};
  }
  Result<int> UdpSend(int connect_id, const void* data, size_t len) override {
    last_send_id = connect_id;
    last_send_data.assign(static_cast<const char*>(data), len);
    udp_send_calls++;
    if (fail_udp_send) {
      return std::unexpected(
          NetworkError(NetworkErrc::kTransmitFailed, 0, "mock fail"));
    }
    return static_cast<int>(len);
  }

  // Drive the URC path the way the AT layer does when a +MIPURC: "rudp" line
  // arrives. The HAL does the routing, so this calls the same helper Ml307Hal
  // calls; going around it would test a path the hardware never takes.
  void EmitUdpData(int connect_id,
                   std::string_view host,
                   uint16_t port,
                   std::string_view data) {
    DispatchUdpData(connect_id, host, port, data);
  }

  bool fail_udp_open = false;
  bool fail_udp_send = false;

  // Lets a test hand the next connection a cid an earlier one used, which is what
  // the pool does once a socket is released.
  void SetNextConnectId(int id) { next_connect_id = id; }

  std::string last_open_host;
  uint16_t last_open_port = 0;
  int last_close_id = -1;
  int last_send_id = -1;
  std::string last_send_data;

  int udp_open_calls = 0;
  int udp_close_calls = 0;
  int udp_send_calls = 0;

 private:
  int next_connect_id = 0;
};

TEST(HalUdpClientTest, ConnectCallsHalUdpOpen) {
  MockHal hal;
  HalUdpClient client(hal);

  auto result = client.Connect("example.com", 53);
  EXPECT_TRUE(result.has_value());
  EXPECT_TRUE(client.IsConnected());
  EXPECT_EQ(hal.udp_open_calls, 1);
  EXPECT_EQ(hal.last_open_host, "example.com");
  EXPECT_EQ(hal.last_open_port, 53);
}

TEST(HalUdpClientTest, ConnectFailureReturnsError) {
  MockHal hal;
  hal.fail_udp_open = true;
  HalUdpClient client(hal);

  auto result = client.Connect("example.com", 53);
  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(client.IsConnected());
}

TEST(HalUdpClientTest, DoubleConnectFails) {
  MockHal hal;
  HalUdpClient client(hal);

  ASSERT_TRUE(client.Connect("example.com", 53).has_value());
  auto result = client.Connect("other.com", 53);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kAlreadyConnected);
}

TEST(HalUdpClientTest, DisconnectCallsUdpClose) {
  MockHal hal;
  HalUdpClient client(hal);

  ASSERT_TRUE(client.Connect("example.com", 53).has_value());
  client.Disconnect();

  EXPECT_FALSE(client.IsConnected());
  EXPECT_EQ(hal.udp_close_calls, 1);
}

TEST(HalUdpClientTest, DestructorDisconnects) {
  MockHal hal;
  {
    HalUdpClient client(hal);
    ASSERT_TRUE(client.Connect("example.com", 53).has_value());
    EXPECT_EQ(hal.udp_close_calls, 0);
  }
  EXPECT_EQ(hal.udp_close_calls, 1);
}

TEST(HalUdpClientTest, SendCallsUdpSend) {
  MockHal hal;
  HalUdpClient client(hal);

  ASSERT_TRUE(client.Connect("example.com", 53).has_value());
  const char data[] = "hello";
  auto result = client.Send(data, 5);

  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 5);
  EXPECT_EQ(hal.udp_send_calls, 1);
  EXPECT_EQ(hal.last_send_data, "hello");
}

TEST(HalUdpClientTest, SendWhenNotConnectedFails) {
  MockHal hal;
  HalUdpClient client(hal);

  const char data[] = "hello";
  auto result = client.Send(data, 5);
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotConnected);
}

TEST(HalUdpClientTest, SendFailurePropagatesError) {
  MockHal hal;
  hal.fail_udp_send = true;
  HalUdpClient client(hal);

  ASSERT_TRUE(client.Connect("example.com", 53).has_value());
  const char data[] = "hello";
  EXPECT_FALSE(client.Send(data, 5).has_value());
}

// The unsolicited half. A datagram the HAL decodes but no route was registered
// for is silently dropped, so these assert delivery end to end.

TEST(HalUdpClientTest, InboundDatagramReachesOnMessage) {
  MockHal hal;
  HalUdpClient client(hal);

  std::string host;
  uint16_t port = 0;
  std::string received;
  client.OnMessage([&](std::string_view h, uint16_t p, std::string_view data) {
    host = std::string(h);
    port = p;
    received = std::string(data);
  });
  ASSERT_TRUE(client.Connect("example.com", 53).has_value());

  hal.EmitUdpData(0, "1.1.1.1", 53, "hello");

  EXPECT_EQ(host, "1.1.1.1");
  EXPECT_EQ(port, 53);
  EXPECT_EQ(received, "hello");
}

// Modules that omit the source address must still deliver, attributed to the
// peer this socket was opened against.
TEST(HalUdpClientTest, MissingSourceFallsBackToRemote) {
  MockHal hal;
  HalUdpClient client(hal);

  std::string host;
  uint16_t port = 0;
  client.OnMessage([&](std::string_view h, uint16_t p, std::string_view) {
    host = std::string(h);
    port = p;
  });
  ASSERT_TRUE(client.Connect("example.com", 53).has_value());

  hal.EmitUdpData(0, "", 0, "hello");

  EXPECT_EQ(host, "example.com");
  EXPECT_EQ(port, 53);
}

TEST(HalUdpClientTest, DatagramForAnotherConnectionIsIgnored) {
  MockHal hal;
  HalUdpClient client(hal);

  bool called = false;
  client.OnMessage(
      [&](std::string_view, uint16_t, std::string_view) { called = true; });
  ASSERT_TRUE(client.Connect("example.com", 53).has_value());

  hal.EmitUdpData(2, "1.1.1.1", 53, "not ours");

  EXPECT_FALSE(called);
}

TEST(HalUdpClientTest, DisconnectedClientDoesNotReceiveDatagrams) {
  MockHal hal;
  HalUdpClient client(hal);

  bool called = false;
  client.OnMessage(
      [&](std::string_view, uint16_t, std::string_view) { called = true; });
  ASSERT_TRUE(client.Connect("example.com", 53).has_value());
  client.Disconnect();

  hal.EmitUdpData(0, "1.1.1.1", 53, "late");

  EXPECT_FALSE(called);
}

// Two sockets on one HAL. With a single slot for inbound datagrams the second
// Connect took the route away from the first.
TEST(HalUdpClientTest, TwoClientsEachReceiveTheirOwnDatagrams) {
  MockHal hal;
  HalUdpClient first(hal);
  HalUdpClient second(hal);

  std::string first_got;
  std::string second_got;
  first.OnMessage([&](std::string_view, uint16_t, std::string_view data) {
    first_got = std::string(data);
  });
  second.OnMessage([&](std::string_view, uint16_t, std::string_view data) {
    second_got = std::string(data);
  });
  ASSERT_TRUE(first.Connect("a.example", 53).has_value());
  ASSERT_TRUE(second.Connect("b.example", 53).has_value());

  hal.EmitUdpData(1, "1.1.1.1", 53, "for the second");
  hal.EmitUdpData(0, "1.1.1.1", 53, "for the first");

  EXPECT_EQ(first_got, "for the first");
  EXPECT_EQ(second_got, "for the second");
}

// What withdrawal is for: the HAL outlives its clients, so a route that survives
// its client is a call into freed memory rather than a leak. Either of the two
// paths out of the destructor would clear this on its own; removing both is what
// this catches.
TEST(HalUdpClientTest, ADestroyedClientStopsReceiving) {
  MockHal hal;
  int observed = 0;
  {
    HalUdpClient dead(hal);
    // Written through only if the route outlives the client.
    dead.OnMessage([&](std::string_view, uint16_t, std::string_view) {
      observed = -1;
    });
    ASSERT_TRUE(dead.Connect("example.com", 53).has_value());
  }

  // The pool hands a released cid straight back out, so the next connection can
  // land on the same one.
  hal.SetNextConnectId(0);
  HalUdpClient live(hal);
  live.OnMessage(
      [&](std::string_view, uint16_t, std::string_view) { ++observed; });
  ASSERT_TRUE(live.Connect("example.com", 53).has_value());

  hal.EmitUdpData(0, "1.1.1.1", 53, "hello");

  EXPECT_EQ(observed, 1);
}

}  // namespace
