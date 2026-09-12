#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "esp_modem_link/tcp_client.h"
#include "protocol/http/software_http_client.h"

using namespace esp_modem_link;
using namespace esp_modem_link::protocol;

namespace {

// What a transport recorded while it was alive. Kept apart from the transport
// itself because the client is entitled to destroy its transport - at the end
// of a non-keep-alive request, and again on Close() - and the test still has to
// see what happened first.
struct MockState {
  int connect_calls = 0;
  int disconnect_calls = 0;
  std::string last_host;
  uint16_t last_port = 0;
  bool was_tls = false;
  TlsConfig last_tls_config;
  std::string sent;
};

// A TcpClient that answers a request as soon as it is sent. Responding
// synchronously from Send() keeps these tests deterministic while still
// exercising the real path: the data callback is invoked from outside the
// request code, exactly as the AT layer's receive thread does it.
class MockTcpClient : public TcpClient {
 public:
  struct Script {
    std::string response;
    bool respond = false;
    bool close_after = false;
    size_t chunk_size = 0;  // 0 = deliver the response in one piece
    bool fail_connect = false;
    bool fail_send = false;
  };

  MockTcpClient(std::shared_ptr<MockState> state, const Script& script)
      : state_(std::move(state)), script_(script) {}

  Result<> Connect(std::string_view host, uint16_t port) override {
    state_->connect_calls++;
    state_->last_host = std::string(host);
    state_->last_port = port;
    if (script_.fail_connect) {
      return std::unexpected(
          NetworkError(NetworkErrc::kConnectFailed, 0, "mock connect failed"));
    }
    connected_ = true;
    return {};
  }

  void Disconnect() override {
    connected_ = false;
    state_->disconnect_calls++;
  }

  Result<int> Send(const void* data, size_t len) override {
    state_->sent.append(static_cast<const char*>(data), len);
    if (script_.fail_send) {
      return std::unexpected(
          NetworkError(NetworkErrc::kTransmitFailed, 0, "mock send failed"));
    }
    EmitResponse();
    return static_cast<int>(len);
  }

 private:
  void EmitResponse() {
    if (!script_.respond || !on_data_) return;
    if (script_.chunk_size == 0) {
      on_data_(script_.response);
    } else {
      std::string_view whole(script_.response);
      for (size_t i = 0; i < whole.size(); i += script_.chunk_size) {
        on_data_(whole.substr(i, script_.chunk_size));
      }
    }
    if (script_.close_after && on_disconnected_) {
      on_disconnected_();
    }
  }

  std::shared_ptr<MockState> state_;
  Script script_;
};

// The client builds its transport during the request, since only then is the
// URL parsed and TLS known, so the script lives here and every transport the
// factory hands out is programmed from it.
class ClientUnderTest {
 public:
  ClientUnderTest() {
    client_ = std::make_unique<SoftwareHttpClient>(
        [this](bool tls, const TlsConfig& config)
            -> Result<std::unique_ptr<TcpClient>> {
          auto state = std::make_shared<MockState>();
          state->was_tls = tls;
          state->last_tls_config = config;
          state_.push_back(state);

          // One script per connection, so a test can give a repeat of the
          // request a different answer from the attempt before it.
          MockTcpClient::Script script = script_;
          if (!responses_in_turn_.empty()) {
            const size_t index =
                std::min(attempt_, responses_in_turn_.size() - 1);
            script.response = responses_in_turn_[index];
            script.respond = true;
            ++attempt_;
          }

          return std::unique_ptr<TcpClient>(
              std::make_unique<MockTcpClient>(std::move(state), script));
        });
  }

  SoftwareHttpClient& client() { return *client_; }

  void SetResponse(std::string response) {
    script_.response = std::move(response);
    script_.respond = true;
  }
  void SuppressResponse() { script_.respond = false; }
  // Delivers the response in pieces of this size, forcing the parser to handle
  // headers and bodies split at arbitrary points.
  void SetResponseChunkSize(size_t size) { script_.chunk_size = size; }
  void SetCloseAfterResponse(bool enable) { script_.close_after = enable; }
  void SetFailConnect(bool enable) { script_.fail_connect = enable; }
  void SetFailSend(bool enable) { script_.fail_send = enable; }

  // One response per connection, in order. The last entry repeats for any
  // supply that runs past the end, so a test ends on a definite answer.
  void SetResponsesInTurn(std::vector<std::string> responses) {
    responses_in_turn_ = std::move(responses);
  }

  MockState& transport(size_t index = 0) { return *state_.at(index); }
  size_t transport_count() const { return state_.size(); }

  // Runs the client's destructor while the recorded state stays readable,
  // which is the only way to observe what it cleans up.
  void DestroyClient() { client_.reset(); }

 private:
  std::vector<std::shared_ptr<MockState>> state_;
  MockTcpClient::Script script_;
  std::vector<std::string> responses_in_turn_;
  size_t attempt_ = 0;
  std::unique_ptr<SoftwareHttpClient> client_;
};

// Reads until end-of-body, which turns the streaming interface into the whole
// body. A failure part way through propagates rather than looking like an end.
Result<std::string> Drain(HttpClient& client) {
  std::string body;
  char buffer[8];
  while (true) {
    auto read = client.Read(buffer, sizeof(buffer));
    if (!read) return std::unexpected(read.error());
    if (*read == 0) break;
    body.append(buffer, static_cast<size_t>(*read));
  }
  return body;
}

constexpr const char* kOkResponse =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 5\r\n"
    "\r\n"
    "hello";

TEST(SoftwareHttpClientTest, ExecuteSendsAWellFormedRequest) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);

  auto result = test.client().Execute("GET", "http://example.com/index.html");
  ASSERT_TRUE(result.has_value()) << result.error().Message();

  const std::string& sent = test.transport().sent;
  EXPECT_EQ(sent.rfind("GET /index.html HTTP/1.1\r\n", 0), 0u);
  EXPECT_NE(sent.find("Host: example.com\r\n"), std::string::npos);
  EXPECT_NE(sent.find("\r\n\r\n"), std::string::npos);
}

TEST(SoftwareHttpClientTest, ExecuteParsesStatusHeadersAndBody) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_TRUE(result.has_value()) << result.error().Message();

  EXPECT_EQ(result->status_code, 200);
  EXPECT_EQ(result->headers["content-type"], "text/plain");
  EXPECT_EQ(result->body, "hello");
}

TEST(SoftwareHttpClientTest, ExecuteConnectsToTheUrlHostAndPort) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);

  ASSERT_TRUE(
      test.client().Execute("GET", "http://example.com:8080/").has_value());

  EXPECT_EQ(test.transport().last_host, "example.com");
  EXPECT_EQ(test.transport().last_port, 8080);
}

// The URL decides whether the transport needs TLS, so the factory is asked per
// request rather than the client being handed a transport up front.
TEST(SoftwareHttpClientTest, HttpsAsksForATlsTransport) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);

  ASSERT_TRUE(test.client().Execute("GET", "https://example.com/").has_value());

  EXPECT_TRUE(test.transport().was_tls);
}

TEST(SoftwareHttpClientTest, HttpAsksForAPlainTransport) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);

  ASSERT_TRUE(test.client().Execute("GET", "http://example.com/").has_value());

  EXPECT_FALSE(test.transport().was_tls);
}

TEST(SoftwareHttpClientTest, TlsConfigReachesTheTransport) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);

  TlsConfig config;
  config.verify_certificate = false;
  config.ca_cert = "PEM";
  test.client().SetTlsConfig(config);

  ASSERT_TRUE(test.client().Execute("GET", "https://example.com/").has_value());

  EXPECT_FALSE(test.transport().last_tls_config.verify_certificate);
  EXPECT_EQ(test.transport().last_tls_config.ca_cert, "PEM");
}

TEST(SoftwareHttpClientTest, ExecutePropagatesConnectFailure) {
  ClientUnderTest test;
  test.SetFailConnect(true);

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kConnectFailed);
}

TEST(SoftwareHttpClientTest, ExecutePropagatesSendFailure) {
  ClientUnderTest test;
  test.SetFailSend(true);

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kTransmitFailed);
}

TEST(SoftwareHttpClientTest, ExecuteRejectsAnInvalidUrl) {
  ClientUnderTest test;

  auto result = test.client().Execute("GET", "example.com/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kInvalidArgument);
  // Nothing should have been opened for a URL that could not be parsed.
  EXPECT_EQ(test.transport_count(), 0u);
}

TEST(SoftwareHttpClientTest, ExecuteTimesOutWhenNoResponseArrives) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.client().SetTimeout(std::chrono::milliseconds(20));

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kTimeout);
}

// A response that announced more than it delivered must not be returned as
// though it were whole - a truncated body that looks complete is worse than an
// error, because the caller cannot tell.
TEST(SoftwareHttpClientTest, ExecuteReportsATruncatedBody) {
  ClientUnderTest test;
  test.SetResponse("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
  test.SetCloseAfterResponse(true);

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kConnectionLost);
}

// HTTP/1.0 servers and "Connection: close" responses carry no length; the peer
// hanging up is what ends the body.
TEST(SoftwareHttpClientTest, CloseDelimitedBodyIsCompletedByThePeerClosing) {
  ClientUnderTest test;
  test.SetResponse("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nbody");
  test.SetCloseAfterResponse(true);

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result->body, "body");
}

// A body cut short by the peer is a transport failure the server may never have
// seen, so a request with no side effect is worth repeating once. Observed on
// ML307R-DL-MBRH0S01, where a 29 KB response lost its last few hundred bytes in
// about one run in four at 115200 baud - the link speed, not the request, was
// the problem, which is exactly what a repeat covers.
TEST(SoftwareHttpClientTest, ExecuteRepeatsAGetThatWasCutShort) {
  ClientUnderTest test;
  test.SetResponsesInTurn({"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhe",
                           "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"});
  test.SetCloseAfterResponse(true);

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result->body, "hello");

  // The repeat is a fresh connection, and the request went out on it rather
  // than the transport having been reused after the failure.
  ASSERT_EQ(test.transport_count(), 2u);
  EXPECT_EQ(test.transport(1).sent.rfind("GET / HTTP/1.1\r\n", 0), 0u);
}

// The same failure on a POST must not be repeated. The request may have reached
// the server and been acted on before the response was cut short, and sending it
// again could apply that action twice.
TEST(SoftwareHttpClientTest, ExecuteDoesNotRepeatAPostThatWasCutShort) {
  ClientUnderTest test;
  test.SetResponsesInTurn({"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhe",
                           "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"});
  test.SetCloseAfterResponse(true);

  auto result = test.client().Execute("POST", "http://example.com/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kConnectionLost);
  EXPECT_EQ(test.transport_count(), 1u);
}

// One repeat, not a loop. A link that truncates every response has to surface
// the failure rather than multiply the requests against it.
TEST(SoftwareHttpClientTest, ExecuteGivesUpAfterOneRepeat) {
  ClientUnderTest test;
  test.SetResponse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhe");
  test.SetCloseAfterResponse(true);

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kConnectionLost);
  EXPECT_EQ(test.transport_count(), 2u);
}

// A stall is not a truncation. The connection is still up, so nothing suggests
// the request went astray, and the timeout is the caller's own budget being
// spent - repeating would only spend it again.
TEST(SoftwareHttpClientTest, ExecuteDoesNotRepeatATimeout) {
  ClientUnderTest test;
  test.SetResponse("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhe");
  test.client().SetTimeout(std::chrono::milliseconds(20));

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kTimeout);
  EXPECT_EQ(test.transport_count(), 1u);
}

TEST(SoftwareHttpClientTest, ExecuteHandlesChunkedResponses) {
  ClientUnderTest test;
  test.SetResponse(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n"
      "6\r\n world\r\n"
      "0\r\n\r\n");

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result->body, "hello world");
}

TEST(SoftwareHttpClientTest, ParsesResponsesSplitAtEveryBoundary) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  test.SetResponseChunkSize(1);

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result->status_code, 200);
  EXPECT_EQ(result->body, "hello");
}

TEST(SoftwareHttpClientTest, ExecuteReportsNonHttpGarbage) {
  ClientUnderTest test;
  test.SetResponse("+CME ERROR: 50\r\nOK\r\n");

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kProtocolError);
}

TEST(SoftwareHttpClientTest, SetHeaderAppearsInTheRequest) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  test.client().SetHeader("Accept", "application/json");

  ASSERT_TRUE(test.client().Execute("GET", "http://example.com/").has_value());

  EXPECT_NE(test.transport().sent.find("Accept: application/json\r\n"),
            std::string::npos);
}

TEST(SoftwareHttpClientTest, SetBodyIsSentWithItsLength) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  test.client().SetBody("{\"a\":1}");

  ASSERT_TRUE(test.client().Execute("POST", "http://example.com/").has_value());

  const std::string& sent = test.transport().sent;
  EXPECT_NE(sent.find("POST / HTTP/1.1\r\n"), std::string::npos);
  EXPECT_NE(sent.find("Content-Length: 7\r\n"), std::string::npos);
  EXPECT_EQ(sent.substr(sent.size() - 7), "{\"a\":1}");
}

TEST(SoftwareHttpClientTest, ClosesTheConnectionWhenKeepAliveIsOff) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);

  ASSERT_TRUE(test.client().Execute("GET", "http://example.com/").has_value());

  EXPECT_EQ(test.transport().disconnect_calls, 1);
}

TEST(SoftwareHttpClientTest, KeepAliveReusesTheConnection) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  test.client().SetKeepAlive(true);

  ASSERT_TRUE(test.client().Execute("GET", "http://example.com/a").has_value());
  ASSERT_TRUE(test.client().Execute("GET", "http://example.com/b").has_value());

  EXPECT_EQ(test.transport_count(), 1u);
  EXPECT_EQ(test.transport().connect_calls, 1);
  EXPECT_EQ(test.transport().disconnect_calls, 0);
  // Both requests went out on the same socket.
  EXPECT_NE(test.transport().sent.find("/a"), std::string::npos);
  EXPECT_NE(test.transport().sent.find("/b"), std::string::npos);
}

// Reusing a socket to a different host would send the request somewhere it does
// not belong, so the transport has to be replaced.
TEST(SoftwareHttpClientTest, KeepAliveReconnectsForADifferentHost) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  test.client().SetKeepAlive(true);

  ASSERT_TRUE(test.client().Execute("GET", "http://example.com/").has_value());
  ASSERT_TRUE(test.client().Execute("GET", "http://other.example/").has_value());

  EXPECT_EQ(test.transport_count(), 2u);
  EXPECT_EQ(test.transport(1).last_host, "other.example");
}

// --- Streaming ---

TEST(SoftwareHttpClientTest, OpenReturnsOnceHeadersArrive) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);

  auto opened = test.client().Open("GET", "http://example.com/");
  ASSERT_TRUE(opened.has_value()) << opened.error().Message();

  auto status = test.client().GetStatusCode();
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(*status, 200);
  EXPECT_EQ(test.client().GetResponseHeader("Content-Type"), "text/plain");
  EXPECT_EQ(test.client().GetContentLength(), 5u);
}

TEST(SoftwareHttpClientTest, ReadStreamsTheBody) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  test.SetResponseChunkSize(2);

  ASSERT_TRUE(test.client().Open("GET", "http://example.com/").has_value());

  auto body = Drain(test.client());
  ASSERT_TRUE(body.has_value()) << body.error().Message();
  EXPECT_EQ(*body, "hello");
}

TEST(SoftwareHttpClientTest, ReadStreamsAChunkedBody) {
  ClientUnderTest test;
  test.SetResponse(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n");

  ASSERT_TRUE(test.client().Open("GET", "http://example.com/").has_value());

  auto body = Drain(test.client());
  ASSERT_TRUE(body.has_value()) << body.error().Message();
  EXPECT_EQ(*body, "abcdef");
}

TEST(SoftwareHttpClientTest, ReadTimesOutWhenTheBodyStalls) {
  ClientUnderTest test;
  test.SetResponse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nab");
  test.client().SetTimeout(std::chrono::milliseconds(20));

  ASSERT_TRUE(test.client().Open("GET", "http://example.com/").has_value());

  char buffer[8];
  auto first = test.client().Read(buffer, sizeof(buffer));
  ASSERT_TRUE(first.has_value()) << first.error().Message();
  EXPECT_EQ(*first, 2);

  // The rest never arrives, so this must fail rather than report end-of-body.
  auto second = test.client().Read(buffer, sizeof(buffer));
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code, NetworkErrc::kTimeout);
}

// A peer that hangs up with the announced length unmet ends the read the same
// way a stall does, and must be reported the same way: as a failure. Returning
// zero there would present a short body as a whole response, which Execute()
// already refuses to do. Observed on ML307R-DL-MBRH0S01, where the module
// reported the socket closed with 442 bytes of a 29506-byte body outstanding.
TEST(SoftwareHttpClientTest, ReadReportsATruncatedBodyRatherThanEndOfBody) {
  ClientUnderTest test;
  test.SetResponse("HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nab");
  test.SetCloseAfterResponse(true);

  ASSERT_TRUE(test.client().Open("GET", "http://example.com/").has_value());

  // The two bytes that did arrive are served before the failure is reported, so
  // a close cannot hide data that was delivered.
  char buffer[8];
  auto first = test.client().Read(buffer, sizeof(buffer));
  ASSERT_TRUE(first.has_value()) << first.error().Message();
  EXPECT_EQ(*first, 2);

  auto second = test.client().Read(buffer, sizeof(buffer));
  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code, NetworkErrc::kConnectionLost);
}

// The exception the parser already draws: with no length and no chunking, the
// peer hanging up is the only way the body can end, so it completes the
// response rather than truncating it.
TEST(SoftwareHttpClientTest, ReadAcceptsAShortCloseDelimitedBody) {
  ClientUnderTest test;
  test.SetResponse("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nhello");
  test.SetCloseAfterResponse(true);

  ASSERT_TRUE(test.client().Open("GET", "http://example.com/").has_value());

  auto body = Drain(test.client());
  ASSERT_TRUE(body.has_value()) << body.error().Message();
  EXPECT_EQ(*body, "hello");
}

TEST(SoftwareHttpClientTest, OpenRejectsAnInvalidUrl) {
  ClientUnderTest test;
  auto result = test.client().Open("GET", "nonsense");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kInvalidArgument);
}

// The request head, Content-Length included, is already on the wire by the time
// Write() could be called, so appending a body would contradict it.
TEST(SoftwareHttpClientTest, WriteIsRejectedRatherThanSilentlyIgnored) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  ASSERT_TRUE(test.client().Open("GET", "http://example.com/").has_value());

  auto result = test.client().Write("data", 4);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotSupported);
}

TEST(SoftwareHttpClientTest, CloseReleasesTheTransport) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  ASSERT_TRUE(test.client().Open("GET", "http://example.com/").has_value());

  test.client().Close();

  EXPECT_EQ(test.transport().disconnect_calls, 1);
}

TEST(SoftwareHttpClientTest, GetStatusCodeBeforeAnyRequestFails) {
  ClientUnderTest test;
  EXPECT_FALSE(test.client().GetStatusCode().has_value());
}

TEST(SoftwareHttpClientTest, DestructorDisconnectsAnOpenTransport) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  ASSERT_TRUE(test.client().Open("GET", "http://example.com/").has_value());

  test.DestroyClient();

  EXPECT_EQ(test.transport().disconnect_calls, 1);
}

}  // namespace
