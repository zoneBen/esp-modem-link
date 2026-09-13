#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "mock_tcp_client.h"
#include "protocol/websocket/software_ws_client.h"
#include "protocol/websocket/ws_handshake.h"

using namespace esp_modem_link;
using namespace esp_modem_link::protocol;
using namespace esp_modem_link::testing;

namespace {

// RFC 6455's own example key, so the accept value that goes back is the one the
// specification prints and the upgrade request is fixed in advance.
constexpr std::string_view kTestKey = "dGhlIHNhbXBsZSBub25jZQ==";

std::string AcceptedUpgrade(std::string_view key) {
  std::string response = "HTTP/1.1 101 Switching Protocols\r\n";
  response += "Upgrade: websocket\r\n";
  response += "Connection: Upgrade\r\n";
  response += "Sec-WebSocket-Accept: ";
  response += ComputeSecWebSocketAccept(key);
  response += "\r\n\r\n";
  return response;
}

// A frame as a server sees it. The client's frames are masked, which the
// production decoder refuses, so this is deliberately a second implementation
// rather than a call into the code under test: a decoder that agreed with a
// broken encoder would make a round-trip assertion meaningless.
struct ServerFrame {
  bool fin = true;
  WsOpcode opcode = WsOpcode::kText;
  bool masked = false;
  std::string payload;
};

std::optional<ServerFrame> DecodeClientFrame(std::string_view bytes) {
  if (bytes.size() < 2) return std::nullopt;
  const auto* b = reinterpret_cast<const uint8_t*>(bytes.data());

  ServerFrame frame;
  frame.fin = (b[0] & 0x80) != 0;
  frame.opcode = static_cast<WsOpcode>(b[0] & 0x0F);
  frame.masked = (b[1] & 0x80) != 0;

  uint64_t length = b[1] & 0x7F;
  size_t offset = 2;
  if (length == 126) {
    if (bytes.size() < 4) return std::nullopt;
    length = (static_cast<uint64_t>(b[2]) << 8) | b[3];
    offset = 4;
  } else if (length == 127) {
    if (bytes.size() < 10) return std::nullopt;
    length = 0;
    for (size_t i = 0; i < 8; ++i) length = (length << 8) | b[2 + i];
    offset = 10;
  }

  uint32_t key = 0;
  if (frame.masked) {
    if (bytes.size() < offset + 4) return std::nullopt;
    key = (static_cast<uint32_t>(b[offset]) << 24) |
          (static_cast<uint32_t>(b[offset + 1]) << 16) |
          (static_cast<uint32_t>(b[offset + 2]) << 8) |
          static_cast<uint32_t>(b[offset + 3]);
    offset += 4;
  }
  if (static_cast<uint64_t>(bytes.size()) <
      static_cast<uint64_t>(offset) + length) {
    return std::nullopt;
  }

  frame.payload =
      std::string(bytes.substr(offset, static_cast<size_t>(length)));
  if (frame.masked) {
    for (size_t i = 0; i < frame.payload.size(); ++i) {
      const uint8_t key_byte = static_cast<uint8_t>(key >> (8 * (3 - (i % 4))));
      frame.payload[i] =
          static_cast<char>(static_cast<uint8_t>(frame.payload[i]) ^ key_byte);
    }
  }
  return frame;
}

// A frame as a server would send it: unmasked, which is what the codec's
// encoder produces when it is given no key.
std::string ServerDataFrame(WsOpcode opcode, std::string_view payload,
                            bool fin = true) {
  auto encoded = EncodeFrame(opcode, payload, fin);
  return encoded.value_or(std::string());
}

// What a test records from a callback. Atomic because a callback does not
// necessarily run on the thread that called in: here the heartbeat and the
// reconnection logic both report from the maintenance task.
struct RecordedError {
  std::atomic<bool> seen{false};
  std::atomic<int> code{0};

  void operator()(const NetworkError& error) {
    code = static_cast<int>(error.code);
    seen = true;
  }
};

bool WaitFor(const std::function<bool()>& predicate,
             std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

class ClientUnderTest {
 public:
  explicit ClientUnderTest(
      std::chrono::milliseconds handshake_timeout = std::chrono::seconds(2)) {
    client_ = std::make_unique<SoftwareWsClient>(
        [this](bool tls) -> Result<std::unique_ptr<TcpClient>> {
          auto state = std::make_shared<MockState>();
          state->was_tls = tls;

          MockTcpClient::Script script = script_;
          size_t index = 0;
          {
            std::lock_guard<std::mutex> lock(states_mutex_);
            state_.push_back(state);
            index = state_.size();
          }
          // Failing from the Nth transport on is how a test tells a first
          // connection that works from one that is refused afterwards.
          if (fail_connect_ && index > fail_connect_from_) {
            script.fail_connect = true;
          }
          return std::unique_ptr<TcpClient>(
              std::make_unique<MockTcpClient>(state, script));
        },
        handshake_timeout);
  }

  SoftwareWsClient& client() { return *client_; }

  // Lets go of the client while the state it recorded stays readable, which is
  // how the destructor gets tested.
  void DestroyClient() { client_.reset(); }

  // What the transport answers to anything sent over it - once, for the upgrade
  // request, because that is the only answer a WebSocket server owes it. Traffic
  // from the server after the handshake is delivered explicitly.
  void RespondWith(std::string response) {
    script_.response = std::move(response);
    script_.respond = true;
    script_.respond_once = true;
  }

  void AcceptUpgrades(std::string_view key = kTestKey) {
    RespondWith(AcceptedUpgrade(key));
  }

  // Connections from the Nth transport on are refused. The script is copied per
  // transport, so this only affects the ones built after it is called.
  void SetFailConnectFrom(size_t index) {
    fail_connect_ = true;
    fail_connect_from_ = index;
  }

  void Configure() {
    client_->SetHeader("Sec-WebSocket-Key", std::string(kTestKey));
    AcceptUpgrades();
  }

  void ConnectAndAccept(std::string_view url = "ws://server.example/chat") {
    Configure();
    auto result = client_->Connect(url);
    ASSERT_TRUE(result.has_value()) << result.error().Message();
    ASSERT_TRUE(client_->IsConnected());
  }

  std::shared_ptr<MockState> state_ptr(size_t index = 0) {
    std::lock_guard<std::mutex> lock(states_mutex_);
    if (index >= state_.size()) {
      throw std::runtime_error("no such transport");
    }
    return state_.at(index);
  }

  MockState& state(size_t index = 0) { return *state_ptr(index); }

  size_t transport_count() {
    std::lock_guard<std::mutex> lock(states_mutex_);
    return state_.size();
  }

  // The live transport, through which a test pushes server traffic. Thrown
  // rather than dereferenced: a test that reaches for a transport the client
  // has already let go of should say so, not crash the runner.
  MockTcpClient& transport(size_t index = 0) {
    MockTcpClient* mock = state_ptr(index)->last_client;
    if (mock == nullptr) throw std::runtime_error("no live transport");
    return *mock;
  }

  const std::string& handshake_request(size_t index = 0) {
    return state_ptr(index)->sends.at(0);
  }

  // Every frame sent over the transport at `index`, with the upgrade request
  // skipped: the client sends one frame per call, so each entry is one frame.
  std::vector<ServerFrame> client_frames(size_t index = 0) {
    std::vector<ServerFrame> frames;
    const MockState& s = *state_ptr(index);
    for (size_t i = 1; i < s.sends.size(); ++i) {
      auto frame = DecodeClientFrame(s.sends[i]);
      if (frame.has_value()) frames.push_back(*frame);
    }
    return frames;
  }

  bool saw_frame(WsOpcode opcode, size_t index = 0) {
    for (const auto& frame : client_frames(index)) {
      if (frame.opcode == opcode) return true;
    }
    return false;
  }

 private:
  std::unique_ptr<SoftwareWsClient> client_;
  // A reconnect builds its transport on the maintenance task, so everything
  // that walks the list of transports has to hold this.
  std::mutex states_mutex_;
  std::vector<std::shared_ptr<MockState>> state_;
  MockTcpClient::Script script_;
  bool fail_connect_ = false;
  size_t fail_connect_from_ = 0;
};

}  // namespace

// --- Connecting -----------------------------------------------------------

TEST(SoftwareWsClientTest, ConnectsThroughTheUpgrade) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  EXPECT_EQ(test.state().connect_calls, 1);
  EXPECT_EQ(test.state().last_host, "server.example");
  EXPECT_EQ(test.state().last_port, 80);
  EXPECT_FALSE(test.state().was_tls);
}

// The request exactly as written, because a handshake is the one part of this
// protocol a server will not forgive a deviation in.
TEST(SoftwareWsClientTest, SendsTheUpgradeTheSpecificationDescribes) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  std::string expected = "GET /chat HTTP/1.1\r\n";
  expected += "Host: server.example\r\n";
  expected += "Upgrade: websocket\r\n";
  expected += "Connection: Upgrade\r\n";
  expected += "Sec-WebSocket-Key: ";
  expected += kTestKey;
  expected += "\r\n";
  expected += "Sec-WebSocket-Version: 13\r\n";
  expected += "\r\n";
  EXPECT_EQ(test.handshake_request(), expected);
}

TEST(SoftwareWsClientTest, ReadsTheSchemeForThePortAndForTls) {
  ClientUnderTest test;
  test.ConnectAndAccept("wss://server.example/chat");

  EXPECT_EQ(test.state().last_port, 443);
  EXPECT_TRUE(test.state().was_tls);
}

TEST(SoftwareWsClientTest, KeepsAPortThatIsNotTheSchemesDefault) {
  ClientUnderTest test;
  test.ConnectAndAccept("ws://server.example:8080/chat");

  EXPECT_EQ(test.state().last_port, 8080);
  // The port is not the scheme's default, so it belongs in the Host header.
  EXPECT_NE(test.handshake_request().find("Host: server.example:8080\r\n"),
            std::string::npos);
}

TEST(SoftwareWsClientTest, SendsCallerHeadersAfterTheDefaults) {
  ClientUnderTest test;
  test.client().SetHeader("X-Client", "esp32");
  test.ConnectAndAccept();

  const std::string& request = test.handshake_request();
  EXPECT_NE(request.find("X-Client: esp32\r\n"), std::string::npos);
  // After, not before: a caller adding headers should not be able to push the
  // ones the specification requires out of the order servers expect.
  EXPECT_LT(request.find("Sec-WebSocket-Version: 13"),
            request.find("X-Client: esp32"));
}

TEST(SoftwareWsClientTest, SettingTheSameHeaderTwiceReplacesIt) {
  ClientUnderTest test;
  test.client().SetHeader("Origin", "http://one.example");
  test.client().SetHeader("Origin", "http://two.example");
  test.ConnectAndAccept();

  const std::string& request = test.handshake_request();
  EXPECT_EQ(request.find("http://one.example"), std::string::npos);
  EXPECT_NE(request.find("Origin: http://two.example"), std::string::npos);
  // Once, not twice: a request with two Origin headers is one a server is
  // entitled to reject.
  EXPECT_EQ(request.find("Origin: http://two.example"),
            request.rfind("Origin: http://two.example"));
}

// A caller that names the key itself is taken at its word, and the accept value
// is checked against what actually went out rather than against a key the client
// kept to itself.
TEST(SoftwareWsClientTest, UsesAKeyTheCallerSupplied) {
  ClientUnderTest test;
  test.client().SetHeader("Sec-WebSocket-Key", "AAAAAAAAAAAAAAAAAAAAAA==");
  test.AcceptUpgrades("AAAAAAAAAAAAAAAAAAAAAA==");

  auto result = test.client().Connect("ws://server.example/");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_NE(test.handshake_request().find(
                "Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA=="),
            std::string::npos);
}

// The check that tells a WebSocket server from anything else that answers on the
// port: only one that read the key can produce this value.
TEST(SoftwareWsClientTest, RefusesAnAcceptValueThatDoesNotMatchTheKey) {
  ClientUnderTest test;
  test.client().SetHeader("Sec-WebSocket-Key", std::string(kTestKey));
  test.AcceptUpgrades("some-other-key==");

  auto result = test.client().Connect("ws://server.example/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kWsHandshakeFailed);
  EXPECT_FALSE(test.client().IsConnected());
}

TEST(SoftwareWsClientTest, ReportsARefusedUpgradeAsItsStatusCode) {
  ClientUnderTest test;
  test.RespondWith("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");

  auto result = test.client().Connect("ws://server.example/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kWsHandshakeFailed);
  EXPECT_EQ(result.error().native, 403);
}

TEST(SoftwareWsClientTest, ReportsAResponseThatIsNotHttpAtAll) {
  ClientUnderTest test;
  test.RespondWith("this is not a response\r\n\r\n");

  auto result = test.client().Connect("ws://server.example/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kProtocolError);
}

// A server that accepts the socket and then says nothing is the case a timeout
// exists for, and the socket has to be let go of rather than left open.
TEST(SoftwareWsClientTest, GivesUpWhenTheUpgradeIsNeverAnswered) {
  ClientUnderTest test(std::chrono::milliseconds(150));
  auto result = test.client().Connect("ws://server.example/");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kTimeout);
  EXPECT_EQ(test.state().disconnect_calls, 1);
  EXPECT_FALSE(test.client().IsConnected());
}

TEST(SoftwareWsClientTest, RefusesAUrlThatIsNotAWebSocketUrl) {
  ClientUnderTest test;
  for (std::string_view url :
       {"http://server.example/", "server.example", "wss:/server.example"}) {
    auto result = test.client().Connect(url);
    ASSERT_FALSE(result.has_value()) << std::string(url);
    EXPECT_EQ(result.error().code, NetworkErrc::kInvalidArgument)
        << std::string(url);
  }
  // Nothing was opened, so there is no transport to have been left behind.
  EXPECT_EQ(test.transport_count(), 0u);
}

TEST(SoftwareWsClientTest, RefusesASecondConnect) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  auto result = test.client().Connect("ws://server.example/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kAlreadyConnected);
}

TEST(SoftwareWsClientTest, ReportsATransportThatWillNotConnect) {
  ClientUnderTest test;
  test.SetFailConnectFrom(0);

  auto result = test.client().Connect("ws://server.example/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kConnectFailed);
}

// The handshake and the first frames can arrive in one read, and the bytes past
// the blank line belong to frames rather than to the response.
TEST(SoftwareWsClientTest, KeepsTheFramesThatFollowTheHandshake) {
  ClientUnderTest test;
  test.client().SetHeader("Sec-WebSocket-Key", std::string(kTestKey));

  std::string message;
  test.client().OnMessage(
      [&message](std::string_view data, bool) { message = std::string(data); });

  // The server's frame is unmasked, which is what a server is required to send.
  const std::string greeting = "hello";
  std::string frame;
  frame.push_back(static_cast<char>(0x81));
  frame.push_back(static_cast<char>(greeting.size()));
  frame += greeting;
  test.RespondWith(AcceptedUpgrade(kTestKey) + frame);

  ASSERT_TRUE(test.client().Connect("ws://server.example/").has_value());
  EXPECT_EQ(message, "hello");
}

// --- Sending --------------------------------------------------------------

TEST(SoftwareWsClientTest, SendsOneMaskedTextFrame) {
  ClientUnderTest test;
  test.ConnectAndAccept();
  ASSERT_TRUE(test.client().Send("hello").has_value());

  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_TRUE(frames[0].fin);
  EXPECT_EQ(frames[0].opcode, WsOpcode::kText);
  // Masking is not optional for a client, and a server that sees an unmasked
  // frame is required to close the connection over it.
  EXPECT_TRUE(frames[0].masked);
  EXPECT_EQ(frames[0].payload, "hello");
}

TEST(SoftwareWsClientTest, SendsABinaryFrameWithTheBinaryOpcode) {
  ClientUnderTest test;
  test.ConnectAndAccept();
  ASSERT_TRUE(test.client().Send("bytes", true).has_value());

  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].opcode, WsOpcode::kBinary);
  EXPECT_EQ(frames[0].payload, "bytes");
}

// Two messages are two frames, each masked under its own key: a client that
// reused one key would pass a single-frame test and fail here.
TEST(SoftwareWsClientTest, MasksEachFrameUnderItsOwnKey) {
  ClientUnderTest test;
  test.ConnectAndAccept();
  ASSERT_TRUE(test.client().Send("same").has_value());
  ASSERT_TRUE(test.client().Send("same").has_value());

  const auto& sends = test.state().sends;
  ASSERT_EQ(sends.size(), 3u);
  EXPECT_NE(sends[1].substr(2, 4), sends[2].substr(2, 4));

  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].payload, "same");
  EXPECT_EQ(frames[1].payload, "same");
}

TEST(SoftwareWsClientTest, RefusesToSendBeforeConnecting) {
  ClientUnderTest test;
  auto result = test.client().Send("hello");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotConnected);
}

TEST(SoftwareWsClientTest, RefusesToSendAfterClosing) {
  ClientUnderTest test;
  test.ConnectAndAccept();
  test.client().Close();

  auto result = test.client().Send("hello");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotConnected);
}

// A message the caller splits up itself: the first frame names the kind and says
// it is not the last, and every frame after it is a continuation.
TEST(SoftwareWsClientTest, MarksAllButTheLastFragment) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  ASSERT_TRUE(test.client().SendFragment("he", 2, false, false).has_value());
  ASSERT_TRUE(test.client().SendFragment("llo", 3, false, true).has_value());

  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].opcode, WsOpcode::kText);
  EXPECT_FALSE(frames[0].fin);
  EXPECT_EQ(frames[0].payload, "he");
  EXPECT_EQ(frames[1].opcode, WsOpcode::kContinuation);
  EXPECT_TRUE(frames[1].fin);
  EXPECT_EQ(frames[1].payload, "llo");
}

// The kind is fixed by the frame that opened the message, so a caller naming a
// different one halfway through is not obeyed - a continuation frame has
// nowhere to say it.
TEST(SoftwareWsClientTest, TheKindIsFixedByTheFirstFragment) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  ASSERT_TRUE(test.client().SendFragment("he", 2, true, false).has_value());
  ASSERT_TRUE(test.client().SendFragment("llo", 3, false, true).has_value());

  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].opcode, WsOpcode::kBinary);
  EXPECT_EQ(frames[1].opcode, WsOpcode::kContinuation);
}

TEST(SoftwareWsClientTest, SendsAPingTheCallerAskedFor) {
  ClientUnderTest test;
  test.ConnectAndAccept();
  test.client().Ping("are you there");

  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].opcode, WsOpcode::kPing);
  EXPECT_EQ(frames[0].payload, "are you there");
}

TEST(SoftwareWsClientTest, RefusesAPingPayloadPastTheControlLimit) {
  ClientUnderTest test;
  RecordedError error;
  test.client().OnError(std::ref(error));
  test.ConnectAndAccept();

  test.client().Ping(std::string(126, 'x'));

  ASSERT_TRUE(error.seen);
  EXPECT_EQ(error.code.load(), static_cast<int>(NetworkErrc::kInvalidArgument));
  EXPECT_TRUE(test.client_frames().empty());
}

// --- Receiving ------------------------------------------------------------

TEST(SoftwareWsClientTest, DeliversAMessageFromTheServer) {
  ClientUnderTest test;
  std::string message;
  test.client().OnMessage(
      [&message](std::string_view data, bool) { message = std::string(data); });
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kText, "hello"));
  EXPECT_EQ(message, "hello");
}

TEST(SoftwareWsClientTest, DeliversTwoMessagesOutOfOneArrival) {
  ClientUnderTest test;
  std::vector<std::string> messages;
  test.client().OnMessage([&messages](std::string_view data, bool) {
    messages.emplace_back(data);
  });
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kText, "one") +
                           ServerDataFrame(WsOpcode::kText, "two"));
  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0], "one");
  EXPECT_EQ(messages[1], "two");
}

// A frame is reassembled from however many reads it arrives in, including the
// cut that lands in the middle of the length field.
TEST(SoftwareWsClientTest, DeliversAMessageSplitAcrossArrivals) {
  ClientUnderTest test;
  std::vector<std::string> messages;
  test.client().OnMessage([&messages](std::string_view data, bool) {
    messages.emplace_back(data);
  });
  test.ConnectAndAccept();

  const std::string frame = ServerDataFrame(WsOpcode::kText, "hello world");
  for (size_t cut = 1; cut < frame.size(); ++cut) {
    test.transport().Deliver(frame.substr(0, cut));
    test.transport().Deliver(frame.substr(cut));
    ASSERT_EQ(messages.size(), cut) << "cut at " << cut;
    EXPECT_EQ(messages.back(), "hello world") << "cut at " << cut;
  }
}

TEST(SoftwareWsClientTest, ReassemblesAFragmentedMessage) {
  ClientUnderTest test;
  std::vector<std::string> messages;
  test.client().OnMessage([&messages](std::string_view data, bool) {
    messages.emplace_back(data);
  });
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kText, "he", false));
  // Nothing yet: half a message is not a message.
  EXPECT_TRUE(messages.empty());

  test.transport().Deliver(ServerDataFrame(WsOpcode::kContinuation, "llo"));
  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0], "hello");
}

TEST(SoftwareWsClientTest, ReassemblesThreeFragments) {
  ClientUnderTest test;
  std::string message;
  test.client().OnMessage(
      [&message](std::string_view data, bool) { message += std::string(data); });
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kText, "a", false));
  test.transport().Deliver(ServerDataFrame(WsOpcode::kContinuation, "b", false));
  test.transport().Deliver(ServerDataFrame(WsOpcode::kContinuation, "c"));
  EXPECT_EQ(message, "abc");
}

// A continuation frame carries no kind of its own, so the frame that opened the
// message is the only place it can come from. A receiver that guessed text
// would hand a binary payload to an application with no way to tell it from a
// text one - and Send()/SendFragment() let a sender say which it is sending, so
// the flag is the other half of something the API already offers.
//
// Both kinds are checked on the fragmented path. A reader that hardcoded either
// answer passes one of these and fails the other, which is the point of running
// them through the same shape.
TEST(SoftwareWsClientTest, KeepsTheKindOfAFragmentedMessage) {
  ClientUnderTest test;
  std::vector<std::pair<std::string, bool>> messages;
  test.client().OnMessage([&messages](std::string_view data, bool binary) {
    messages.emplace_back(std::string(data), binary);
  });
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kBinary, "he", false));
  test.transport().Deliver(ServerDataFrame(WsOpcode::kContinuation, "llo"));
  test.transport().Deliver(ServerDataFrame(WsOpcode::kText, "he", false));
  test.transport().Deliver(ServerDataFrame(WsOpcode::kContinuation, "llo"));

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].first, "hello");
  EXPECT_TRUE(messages[0].second);
  EXPECT_EQ(messages[1].first, "hello");
  EXPECT_FALSE(messages[1].second);
}

TEST(SoftwareWsClientTest, ReportsTheKindOfAnUnfragmentedMessage) {
  ClientUnderTest test;
  std::vector<std::pair<std::string, bool>> messages;
  test.client().OnMessage([&messages](std::string_view data, bool binary) {
    messages.emplace_back(std::string(data), binary);
  });
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kText, "one"));
  test.transport().Deliver(ServerDataFrame(WsOpcode::kBinary, "two"));

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_FALSE(messages[0].second);
  EXPECT_TRUE(messages[1].second);
}

// Sending in fragments and receiving in fragments are two different messages,
// and one flag for both would confuse them. The outbound sequence is open here,
// so a peer's complete frame must still be a complete frame: read against the
// outbound state it looks like a new message starting inside one, which is a
// protocol violation the connection would be closed over - the peer's legal
// traffic answered with a teardown.
TEST(SoftwareWsClientTest, APeerFrameDoesNotEndAnOutboundFragmentSequence) {
  ClientUnderTest test;
  std::vector<std::string> messages;
  test.client().OnMessage(
      [&messages](std::string_view data, bool) { messages.emplace_back(data); });
  test.ConnectAndAccept();

  ASSERT_TRUE(test.client().SendFragment("half", 4, false, false).has_value());
  test.transport().Deliver(ServerDataFrame(WsOpcode::kText, "hello"));

  ASSERT_EQ(messages.size(), 1u);
  EXPECT_EQ(messages[0], "hello");
  EXPECT_TRUE(test.client().IsConnected());

  // And the sequence is still open on the way out, so the frame that ends it is
  // a continuation rather than a second message.
  ASSERT_TRUE(test.client().SendFragment(" half", 5, false, true).has_value());
  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[0].opcode, WsOpcode::kText);
  EXPECT_FALSE(frames[0].fin);
  EXPECT_EQ(frames[1].opcode, WsOpcode::kContinuation);
  EXPECT_TRUE(frames[1].fin);
}

// The mirror: an inbound message ending must not close an outbound sequence.
// The frame after it is a continuation the peer is waiting for, and sending it
// as a fresh message would put an unterminated message on the wire.
TEST(SoftwareWsClientTest, AnInboundFragmentDoesNotEndAnOutboundSequence) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  ASSERT_TRUE(test.client().SendFragment("half", 4, false, false).has_value());
  test.transport().Deliver(ServerDataFrame(WsOpcode::kBinary, "he", false));
  test.transport().Deliver(ServerDataFrame(WsOpcode::kContinuation, "llo"));
  ASSERT_TRUE(test.client().SendFragment(" half", 5, false, true).has_value());

  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 2u);
  EXPECT_EQ(frames[1].opcode, WsOpcode::kContinuation);
  EXPECT_TRUE(test.client().IsConnected());
}

// A ping is a question, and a client that does not answer it is one the peer
// will give up on.
TEST(SoftwareWsClientTest, AnswersAPingWithTheSamePayload) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kPing, "hi"));

  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].opcode, WsOpcode::kPong);
  EXPECT_EQ(frames[0].payload, "hi");
  EXPECT_TRUE(frames[0].masked);
}

TEST(SoftwareWsClientTest, ReportsAPongToTheApplication) {
  ClientUnderTest test;
  std::string pong;
  test.client().OnPong(
      [&pong](std::string_view data) { pong = std::string(data); });
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kPong, "here"));
  EXPECT_EQ(pong, "here");
}

// --- Refusals -------------------------------------------------------------

TEST(SoftwareWsClientTest, RefusesAContinuationWithNothingToContinue) {
  ClientUnderTest test;
  RecordedError error;
  std::atomic<bool> closed{false};
  std::atomic<int> close_code{0};
  test.client().OnError(std::ref(error));
  test.client().OnDisconnected([&](WebSocketCloseCode code, std::string_view) {
    close_code = static_cast<int>(code);
    closed = true;
  });
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kContinuation, "lost"));

  ASSERT_TRUE(error.seen);
  EXPECT_EQ(error.code.load(), static_cast<int>(NetworkErrc::kProtocolError));
  ASSERT_TRUE(closed);
  EXPECT_EQ(close_code.load(),
            static_cast<int>(WebSocketCloseCode::kProtocolError));

  // 1002 goes out as a close frame before the socket is dropped.
  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].opcode, WsOpcode::kClose);
  ASSERT_GE(frames[0].payload.size(), 2u);
  EXPECT_EQ(static_cast<uint8_t>(frames[0].payload[0]), 0x03);
  EXPECT_EQ(static_cast<uint8_t>(frames[0].payload[1]), 0xEA);
}

TEST(SoftwareWsClientTest, RefusesANewMessageDuringAFragmentedOne) {
  ClientUnderTest test;
  RecordedError error;
  test.client().OnError(std::ref(error));
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kText, "first", false));
  test.transport().Deliver(ServerDataFrame(WsOpcode::kText, "second"));

  ASSERT_TRUE(error.seen);
  EXPECT_EQ(error.code.load(), static_cast<int>(NetworkErrc::kProtocolError));
}

TEST(SoftwareWsClientTest, RefusesAReservedOpcode) {
  ClientUnderTest test;
  RecordedError error;
  test.client().OnError(std::ref(error));
  test.ConnectAndAccept();

  std::string frame;
  frame.push_back(static_cast<char>(0x83));
  frame.push_back(static_cast<char>(0x00));
  test.transport().Deliver(frame);

  ASSERT_TRUE(error.seen);
  EXPECT_EQ(error.code.load(), static_cast<int>(NetworkErrc::kProtocolError));
}

// A message past the limit is answered with 1009, which is the code the
// specification provides for exactly this, rather than being buffered until the
// device runs out of memory.
TEST(SoftwareWsClientTest, RefusesAFragmentedMessagePastTheLimit) {
  ClientUnderTest test;
  RecordedError error;
  std::atomic<int> close_code{0};
  test.client().OnError(std::ref(error));
  test.client().OnDisconnected(
      [&close_code](WebSocketCloseCode code, std::string_view) {
        close_code = static_cast<int>(code);
      });
  test.ConnectAndAccept();

  // Two frames of 40 KB each: neither trips the receive buffer on its own, and
  // together they are past what one message may hold.
  const std::string half(40 * 1024, 'x');
  test.transport().Deliver(ServerDataFrame(WsOpcode::kBinary, half, false));
  test.transport().Deliver(ServerDataFrame(WsOpcode::kContinuation, half));

  ASSERT_TRUE(error.seen);
  EXPECT_EQ(error.code.load(), static_cast<int>(NetworkErrc::kBufferOverflow));
  EXPECT_EQ(close_code.load(),
            static_cast<int>(WebSocketCloseCode::kMessageTooBig));
}

TEST(SoftwareWsClientTest, RefusesAnInboundStreamPastTheBufferLimit) {
  ClientUnderTest test;
  RecordedError error;
  test.client().OnError(std::ref(error));
  test.ConnectAndAccept();

  // A header promising far more than the limit, followed by enough bytes to pass
  // it: a client that kept buffering would hold this until it was killed.
  std::string frame;
  frame.push_back(static_cast<char>(0x82));
  frame.push_back(static_cast<char>(127));
  for (int shift = 56; shift >= 0; shift -= 8) {
    frame.push_back(static_cast<char>((100000ULL >> shift) & 0xFF));
  }
  frame.append(std::string(65 * 1024, 'x'));
  test.transport().Deliver(frame);

  ASSERT_TRUE(error.seen);
  EXPECT_EQ(error.code.load(), static_cast<int>(NetworkErrc::kBufferOverflow));
  EXPECT_FALSE(test.client().IsConnected());
}

// A peer that hangs up without a close handshake is the reason 1006 exists, and
// it must reach the application as that rather than as a clean close.
TEST(SoftwareWsClientTest, ReportsADroppedConnectionAsAbnormal) {
  ClientUnderTest test;
  std::atomic<bool> closed{false};
  std::atomic<int> close_code{0};
  test.client().OnDisconnected([&](WebSocketCloseCode code, std::string_view) {
    close_code = static_cast<int>(code);
    closed = true;
  });
  test.ConnectAndAccept();

  ASSERT_TRUE(test.transport().DeliverClose());
  ASSERT_TRUE(closed);
  EXPECT_EQ(close_code.load(),
            static_cast<int>(WebSocketCloseCode::kAbnormalClosure));
  EXPECT_FALSE(test.client().IsConnected());
}

// --- Closing --------------------------------------------------------------

TEST(SoftwareWsClientTest, CloseSaysWhyAndReportsThePeersAnswer) {
  ClientUnderTest test;
  std::atomic<bool> closed{false};
  std::atomic<int> close_code{0};
  std::string close_reason;
  test.client().OnDisconnected(
      [&](WebSocketCloseCode code, std::string_view reason) {
        close_code = static_cast<int>(code);
        close_reason = std::string(reason);
        closed = true;
      });
  test.ConnectAndAccept();

  // The peer answers, which is what ends the wait inside Close() early. A close
  // is a conversation, so the code the application hears is the one the peer
  // agreed to.
  MockTcpClient& peer_transport = test.transport();
  MockState& peer_state = test.state();
  std::thread peer([&peer_transport, &peer_state]() {
    // The atomic send counter rather than the list of sends: two frames have
    // gone out by now (the upgrade and the close), and reading the list itself
    // from here would be reading a vector the other thread is appending to.
    WaitFor([&peer_state]() { return peer_state.send_count.load() >= 2; });
    peer_transport.Deliver(
        ServerDataFrame(WsOpcode::kClose, std::string("\x03\xE8" "bye", 5)));
  });

  test.client().Close(WebSocketCloseCode::kNormal, "bye");
  peer.join();

  ASSERT_TRUE(closed);
  EXPECT_EQ(close_code.load(), static_cast<int>(WebSocketCloseCode::kNormal));
  EXPECT_EQ(close_reason, "bye");
  EXPECT_FALSE(test.client().IsConnected());
  EXPECT_EQ(test.state().disconnect_calls, 1);

  // The close frame carried the reason, after the two status bytes.
  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].opcode, WsOpcode::kClose);
  EXPECT_EQ(frames[0].payload, std::string("\x03\xE8" "bye", 5));
}

// The peer says nothing, so the wait runs out and the close is reported with the
// code that was asked for: the socket is dropped rather than held open on the
// chance that an answer is coming.
TEST(SoftwareWsClientTest, CloseReportsTheRequestedCodeWhenThePeerIsSilent) {
  ClientUnderTest test;
  std::atomic<bool> closed{false};
  std::atomic<int> close_code{0};
  test.client().OnDisconnected([&](WebSocketCloseCode code, std::string_view) {
    close_code = static_cast<int>(code);
    closed = true;
  });
  test.ConnectAndAccept();

  test.client().Close(WebSocketCloseCode::kGoingAway, "shutting down");

  ASSERT_TRUE(closed);
  EXPECT_EQ(close_code.load(), static_cast<int>(WebSocketCloseCode::kGoingAway));
  EXPECT_EQ(test.state().disconnect_calls, 1);
}

TEST(SoftwareWsClientTest, ReportsThePeersCodeWhenThePeerClosesFirst) {
  ClientUnderTest test;
  std::atomic<int> close_code{0};
  std::string close_reason;
  test.client().OnDisconnected(
      [&](WebSocketCloseCode code, std::string_view reason) {
        close_code = static_cast<int>(code);
        close_reason = std::string(reason);
      });
  test.ConnectAndAccept();

  test.transport().Deliver(
      ServerDataFrame(WsOpcode::kClose, std::string("\x03\xE9" "gone", 6)));

  EXPECT_EQ(close_code.load(), static_cast<int>(WebSocketCloseCode::kGoingAway));
  EXPECT_EQ(close_reason, "gone");
  EXPECT_FALSE(test.client().IsConnected());

  // Answered with a close frame of its own, which is the other half of the
  // handshake the peer began.
  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].opcode, WsOpcode::kClose);
}

// An empty close payload is how the wire says 1005: there was no status to give,
// which is not the same as a status of zero.
TEST(SoftwareWsClientTest, ReadsACloseWithNoStatus) {
  ClientUnderTest test;
  std::atomic<int> close_code{0};
  test.client().OnDisconnected(
      [&close_code](WebSocketCloseCode code, std::string_view) {
        close_code = static_cast<int>(code);
      });
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kClose, ""));
  EXPECT_EQ(close_code.load(), static_cast<int>(WebSocketCloseCode::kNoStatus));
}

TEST(SoftwareWsClientTest, RefusesACloseFrameWithASingleByte) {
  ClientUnderTest test;
  RecordedError error;
  test.client().OnError(std::ref(error));
  test.ConnectAndAccept();

  test.transport().Deliver(ServerDataFrame(WsOpcode::kClose, "x"));
  ASSERT_TRUE(error.seen);
  EXPECT_EQ(error.code.load(), static_cast<int>(NetworkErrc::kProtocolError));
}

TEST(SoftwareWsClientTest, CloseOnAConnectionThatIsNotOpenIsSilent) {
  ClientUnderTest test;
  RecordedError error;
  std::atomic<bool> closed{false};
  test.client().OnDisconnected(
      [&closed](WebSocketCloseCode, std::string_view) { closed = true; });
  test.client().OnError(std::ref(error));

  test.client().Close();
  EXPECT_FALSE(closed);
  EXPECT_FALSE(error.seen);

  // And the second close of an already closed connection is silent too.
  test.ConnectAndAccept();
  test.client().Close();
  test.client().Close();
  EXPECT_FALSE(error.seen);
}

// 1006 describes a connection that ended without a handshake. A caller asking
// for it is asking for something that cannot be sent, and the honest answer is
// to drop the socket.
TEST(SoftwareWsClientTest, RefusesToAskForAnAbnormalClosure) {
  ClientUnderTest test;
  RecordedError error;
  test.client().OnError(std::ref(error));
  test.ConnectAndAccept();

  test.client().Close(WebSocketCloseCode::kAbnormalClosure, "gone");

  ASSERT_TRUE(error.seen);
  EXPECT_EQ(error.code.load(), static_cast<int>(NetworkErrc::kInvalidArgument));
  EXPECT_FALSE(test.client().IsConnected());
  // No close frame: 1006 is not a code that may appear on the wire.
  EXPECT_TRUE(test.client_frames().empty());
  EXPECT_EQ(test.state().disconnect_calls, 1);
}

// A client that is let go of closes its socket rather than leaving the peer to
// time the connection out.
TEST(SoftwareWsClientTest, DestructorClosesTheTransport) {
  ClientUnderTest test;
  test.ConnectAndAccept();
  EXPECT_EQ(test.state().disconnect_calls, 0);

  test.DestroyClient();

  EXPECT_EQ(test.state().disconnect_calls, 1);
  // Best effort, and not waited on: the close frame goes out and the socket is
  // dropped without a two-second wait for an answer that may never come.
  auto frames = test.client_frames();
  ASSERT_EQ(frames.size(), 1u);
  EXPECT_EQ(frames[0].opcode, WsOpcode::kClose);
  EXPECT_EQ(frames[0].payload, std::string("\x03\xE8", 2));  // 1000, no reason
}

// --- Heartbeat ------------------------------------------------------------

TEST(SoftwareWsClientTest, SendsAPingOnceTheConnectionIsIdle) {
  ClientUnderTest test;
  test.client().SetHeartbeat({.interval = std::chrono::seconds(1),
                              .timeout = std::chrono::seconds(5),
                              .enabled = true});
  test.ConnectAndAccept();

  EXPECT_TRUE(WaitFor([&test]() { return test.saw_frame(WsOpcode::kPing); },
                      std::chrono::seconds(4)));
}

// The heartbeat measures idleness from the moment the connection opened, not
// from a clock that starts at zero: a client that connects and then sits quiet
// has waited no time at all yet.
TEST(SoftwareWsClientTest, DoesNotPingImmediatelyAfterConnecting) {
  ClientUnderTest test;
  test.client().SetHeartbeat({.interval = std::chrono::seconds(2),
                              .timeout = std::chrono::seconds(5),
                              .enabled = true});
  test.ConnectAndAccept();

  std::this_thread::sleep_for(std::chrono::milliseconds(600));
  EXPECT_TRUE(test.client_frames().empty());
  EXPECT_TRUE(test.client().IsConnected());

  EXPECT_TRUE(WaitFor([&test]() { return test.saw_frame(WsOpcode::kPing); },
                      std::chrono::seconds(5)));
}

TEST(SoftwareWsClientTest, SendsNothingWhenTheHeartbeatIsOff) {
  ClientUnderTest test;
  test.ConnectAndAccept();

  // The default is no heartbeat, so an idle connection stays idle.
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_TRUE(test.client_frames().empty());
  EXPECT_TRUE(test.client().IsConnected());
}

// The switch is `enabled`, not a zero interval. A caller who turns the
// heartbeat off and leaves a perfectly ordinary schedule in the config must get
// no pings; reading a nonzero interval as "on" would resurrect the heartbeat
// they had just turned off.
TEST(SoftwareWsClientTest, SendsNothingWhenTheHeartbeatIsDisabled) {
  ClientUnderTest test;
  test.client().SetHeartbeat({.interval = std::chrono::seconds(1),
                              .timeout = std::chrono::seconds(1),
                              .enabled = false});
  test.ConnectAndAccept();

  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_TRUE(test.client_frames().empty());
  EXPECT_TRUE(test.client().IsConnected());
}

// A peer that has stopped answering is not a peer that is quiet: a connection
// that looks alive but is not is worse than one that is known to be gone, so it
// is given up on and reported.
TEST(SoftwareWsClientTest, GivesUpWhenAPingIsNotAnswered) {
  ClientUnderTest test;
  RecordedError error;
  std::atomic<int> close_code{0};
  test.client().OnError(std::ref(error));
  test.client().OnDisconnected(
      [&close_code](WebSocketCloseCode code, std::string_view) {
        close_code = static_cast<int>(code);
      });
  test.client().SetHeartbeat({.interval = std::chrono::seconds(1),
                              .timeout = std::chrono::seconds(1),
                              .enabled = true});
  test.ConnectAndAccept();

  ASSERT_TRUE(
      WaitFor([&error]() { return error.seen.load(); }, std::chrono::seconds(8)));
  EXPECT_EQ(error.code.load(), static_cast<int>(NetworkErrc::kTimeout));
  EXPECT_EQ(close_code.load(),
            static_cast<int>(WebSocketCloseCode::kAbnormalClosure));
  EXPECT_FALSE(test.client().IsConnected());
}

TEST(SoftwareWsClientTest, APongKeepsTheConnectionAlive) {
  ClientUnderTest test;
  RecordedError error;
  test.client().OnError(std::ref(error));
  test.client().SetHeartbeat({.interval = std::chrono::seconds(1),
                              .timeout = std::chrono::seconds(1),
                              .enabled = true});
  test.ConnectAndAccept();

  // Every ping is answered, which is what a working peer does. The test runs
  // past the point where an unanswered ping would have ended the connection.
  ASSERT_TRUE(WaitFor([&test]() { return test.saw_frame(WsOpcode::kPing); },
                      std::chrono::seconds(4)));
  test.transport().Deliver(ServerDataFrame(WsOpcode::kPong, ""));
  EXPECT_TRUE(WaitFor([&test]() { return test.saw_frame(WsOpcode::kPing); },
                      std::chrono::seconds(4)));

  EXPECT_FALSE(error.seen);
  EXPECT_TRUE(test.client().IsConnected());
}

// --- Reconnecting ---------------------------------------------------------

TEST(SoftwareWsClientTest, DoesNotReconnectUnlessAsked) {
  ClientUnderTest test;
  test.ConnectAndAccept();
  test.transport().DeliverClose();

  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  EXPECT_EQ(test.transport_count(), 1u);
  EXPECT_FALSE(test.client().IsConnected());
}

TEST(SoftwareWsClientTest, ReconnectsAfterADroppedConnection) {
  ClientUnderTest test;
  std::atomic<int> connected{0};
  test.client().OnConnected([&connected]() { connected++; });
  test.client().SetAutoReconnect({.enabled = true});
  test.ConnectAndAccept();
  ASSERT_EQ(connected.load(), 1);

  test.transport().DeliverClose();

  ASSERT_TRUE(WaitFor([&connected]() { return connected.load() == 2; },
                      std::chrono::seconds(8)));
  EXPECT_EQ(test.transport_count(), 2u);
  EXPECT_TRUE(test.client().IsConnected());
  EXPECT_EQ(test.state(1).connect_calls, 1);
}

// The schedule is the caller's rather than the engine's, and a first wait of
// zero means the retry goes out at once. The built-in default is a second, so
// this can only pass if the config was read rather than the old constant kept.
TEST(SoftwareWsClientTest, ReconnectsOnTheConfiguredSchedule) {
  ClientUnderTest test;
  std::atomic<int> connected{0};
  test.client().OnConnected([&connected]() { connected++; });
  test.client().SetAutoReconnect(
      {.enabled = true, .initial_delay = std::chrono::milliseconds(0)});
  test.ConnectAndAccept();

  test.transport().DeliverClose();

  EXPECT_TRUE(WaitFor([&connected]() { return connected.load() == 2; },
                      std::chrono::milliseconds(800)));
  EXPECT_TRUE(test.client().IsConnected());
}

// The application hears about the drop and then about the new connection, which
// is the whole of what auto-reconnect has to tell it.
TEST(SoftwareWsClientTest, ReportsBothEndsOfAReconnection) {
  ClientUnderTest test;
  std::atomic<int> connected{0};
  std::atomic<int> disconnects{0};
  std::atomic<int> close_code{0};
  test.client().OnConnected([&connected]() { connected++; });
  test.client().OnDisconnected([&](WebSocketCloseCode code, std::string_view) {
    close_code = static_cast<int>(code);
    disconnects++;
  });
  test.client().SetAutoReconnect({.enabled = true});
  test.ConnectAndAccept();

  test.transport().DeliverClose();
  ASSERT_TRUE(WaitFor([&connected]() { return connected.load() == 2; },
                      std::chrono::seconds(8)));

  EXPECT_EQ(disconnects.load(), 1);
  EXPECT_EQ(close_code.load(),
            static_cast<int>(WebSocketCloseCode::kAbnormalClosure));
}

// Retries are bounded, and running out of them is reported once rather than on
// every tick that follows.
TEST(SoftwareWsClientTest, GivesUpAfterTheRetriesRunOut) {
  ClientUnderTest test;
  std::atomic<int> connect_failures{0};
  test.client().OnError([&connect_failures](const NetworkError& e) {
    if (e.code == NetworkErrc::kConnectFailed) connect_failures++;
  });
  test.client().SetAutoReconnect({.enabled = true, .max_retries = 1});
  test.ConnectAndAccept();

  // The first transport works and the ones after it are refused, so the drop
  // below is followed by exactly one failed attempt.
  test.SetFailConnectFrom(1);
  test.transport().DeliverClose();

  ASSERT_TRUE(
      WaitFor([&connect_failures]() { return connect_failures.load() >= 1; },
              std::chrono::seconds(8)));
  // Two reports, not one: the attempt that failed, and then the decision to
  // stop. The second is what tells the application the retrying is over.
  ASSERT_TRUE(
      WaitFor([&connect_failures]() { return connect_failures.load() >= 2; },
              std::chrono::seconds(8)));
  const size_t attempts = test.transport_count();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  EXPECT_EQ(test.transport_count(), attempts) << "kept trying after giving up";
  // And the giving up is said once, not on every tick that follows it.
  EXPECT_EQ(connect_failures.load(), 2);
  EXPECT_FALSE(test.client().IsConnected());
}

// Closing is a decision, and reconnecting after one would be to overrule it.
TEST(SoftwareWsClientTest, DoesNotReconnectAfterClose) {
  ClientUnderTest test;
  test.client().SetAutoReconnect({.enabled = true});
  test.ConnectAndAccept();

  test.client().Close();
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));

  EXPECT_EQ(test.transport_count(), 1u);
  EXPECT_FALSE(test.client().IsConnected());
}
