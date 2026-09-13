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

  // What a real HAL does when it gives a cid back to its pool. The mock's pool is
  // the test's - cids come from SetNextConnectId - so the test is what says when a
  // cid has been released.
  void ReleaseCid(int connect_id) { DiscardUndelivered(connect_id); }

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

// Everything below exercises the unsolicited half of the client. What a HAL
// decodes goes to the route registered for that cid, or - when there is none yet
// - is held for the route that is about to be registered, so these assert
// delivery end to end rather than that a subscription call did not crash.

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

// The client subscribes after TcpConnect returns, so the events a peer sends in
// the instant the socket opens have nowhere to go. They are held and handed over
// by the subscription instead of being dropped, which is the difference between a
// short response arriving whole and a client reporting a connection lost on a
// request that in fact succeeded.
TEST(HalTcpClientTest, PayloadFromBeforeTheSubscriptionIsDelivered) {
  MockHal hal;
  HalTcpClient client(hal);

  std::string received;
  client.OnData([&](std::string_view data) { received = std::string(data); });

  // The peer answered while TcpConnect was still running - i.e. before this
  // object had a route registered for its cid.
  hal.EmitTcpData(0, "HTTP/1.1 200 OK");

  ASSERT_TRUE(client.Connect("example.com", 80).has_value());

  EXPECT_EQ(received, "HTTP/1.1 200 OK");
  EXPECT_TRUE(client.IsConnected());
}

// The other half of the same window, and the one that used to leave the client
// stuck: a peer that closes immediately. The connect succeeded and reports
// success, but the connection is over before the caller can use it, and the
// object must not claim otherwise.
TEST(HalTcpClientTest, ACloseFromBeforeTheSubscriptionLeavesTheClientDisconnected) {
  MockHal hal;
  HalTcpClient client(hal);

  int closed = 0;
  client.OnDisconnected([&] { ++closed; });

  hal.EmitTcpClose(0);

  ASSERT_TRUE(client.Connect("example.com", 80).has_value());

  EXPECT_FALSE(client.IsConnected());
  EXPECT_EQ(closed, 1);
}

// Both events can be waiting at once, and the order they are handed over in is
// the order the module produced them: a body followed by an end, never an end
// followed by payload the client would have no connection to deliver to.
TEST(HalTcpClientTest, HeldPayloadIsDeliveredBeforeTheHeldClose) {
  MockHal hal;
  HalTcpClient client(hal);

  std::string order;
  client.OnData([&](std::string_view) { order += "data,"; });
  client.OnDisconnected([&] { order += "close"; });

  hal.EmitTcpData(0, "body");
  hal.EmitTcpClose(0);

  ASSERT_TRUE(client.Connect("example.com", 80).has_value());

  EXPECT_EQ(order, "data,close");
  EXPECT_FALSE(client.IsConnected());
}

// Why a hold has to be voided where the cid is released: a cid names all of its
// generations equally, so a hold left over from a finished socket would be handed
// to the next client on that cid as if it were its own, and that client would see
// bytes it never asked for on a request it just made.
TEST(HalTcpClientTest, AHoldDoesNotOutliveTheCidItWasHeldFor) {
  MockHal hal;
  auto leaving = std::make_unique<HalTcpClient>(hal);
  ASSERT_TRUE(leaving->Connect("a.example", 80).has_value());
  hal.EmitTcpClose(0);
  ASSERT_FALSE(leaving->IsConnected());

  // Payload for the finished socket that nobody will ever claim.
  hal.EmitTcpData(0, "bytes for the socket that ended");

  // Giving the cid back is what voids it - a released cid is one the pool hands
  // out again.
  hal.ReleaseCid(0);
  leaving.reset();

  hal.SetNextConnectId(0);
  HalTcpClient replacement(hal);
  std::string received;
  replacement.OnData(
      [&](std::string_view data) { received = std::string(data); });
  ASSERT_TRUE(replacement.Connect("b.example", 80).has_value());
  ASSERT_EQ(hal.last_connect_id, 0);

  EXPECT_TRUE(received.empty())
      << "the previous socket's bytes were handed to the socket that replaced it";
}

// A hold is bounded, and what it drops on overflow is the tail: TCP is a stream,
// so losing the oldest bytes would deliver a hole as though the peer had sent it,
// while a short tail is visible as a stream that stopped early.
TEST(HalTcpClientTest, AHoldTooLargeForItsBufferEndsTheStreamInsteadOfLying) {
  MockHal hal;
  HalTcpClient client(hal);

  size_t received = 0;
  bool closed = false;
  client.OnData([&](std::string_view data) { received += data.size(); });
  client.OnDisconnected([&] { closed = true; });

  // Comfortably past the 4 KiB a hold may carry.
  const std::string flood(8192, 'x');
  hal.EmitTcpData(0, flood);

  ASSERT_TRUE(client.Connect("example.com", 80).has_value());

  EXPECT_LT(received, flood.size());
  EXPECT_EQ(received, 4096u);
  // The close is what keeps the short prefix from passing for the whole response.
  EXPECT_TRUE(closed);
  // And it is a real close, not just a report of one. Nothing ended this socket,
  // so a close that existed only in the report would leave the module's socket
  // open with the caller told not to bother closing it - which strands the cid in
  // the pool for the life of the process.
  EXPECT_EQ(hal.tcp_close_calls, 1);
  EXPECT_EQ(hal.last_close_id, 0);
}

// Connect hands control to the user's callbacks while it is still on the stack,
// because the delivery of what arrived early happens inside the call that
// registers the route. A client that reconnects from its own disconnect callback
// is therefore a nested Connect, and the outer one must not take the object back:
// it would overwrite the live route's handle with the dead one it replaced,
// leaving nothing able to withdraw the route the callbacks still point through.
TEST(HalTcpClientTest, AConnectIssuedFromInsideConnectKeepsItsOwnRoute) {
  MockHal hal;
  HalTcpClient client(hal);

  bool reconnected = false;
  client.OnDisconnected([&] {
    if (reconnected) return;  // Disconnect() calls this too, and once is enough
    reconnected = true;
    // The reconnect a client does when it is told the peer went away.
    hal.SetNextConnectId(7);
    EXPECT_TRUE(client.Connect("again.example", 80).has_value());
  });

  // Already waiting when the first Connect subscribes, so the close lands in the
  // middle of Connect and the callback runs from there.
  hal.EmitTcpClose(0);
  ASSERT_TRUE(client.Connect("example.com", 80).has_value());
  ASSERT_TRUE(client.IsConnected());

  std::string received;
  client.OnData([&](std::string_view data) { received = std::string(data); });

  // The object belongs to the nested connect and its route is the live one.
  hal.EmitTcpData(0, "for the socket that ended");
  hal.EmitTcpData(7, "for the live one");
  EXPECT_EQ(received, "for the live one");

  // Withdrawal is the thing that has to reach the live route, so it is what the
  // two handles are told apart by.
  client.Disconnect();
  received.clear();
  hal.EmitTcpData(7, "after the disconnect");
  EXPECT_TRUE(received.empty())
      << "the live route outlived the client that owned it";
}

}  // namespace
