#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "esp_modem_link/tcp_client.h"
#include "mock_tcp_client.h"
#include "protocol/http/http_redirect.h"
#include "protocol/http/software_http_client.h"

using namespace esp_modem_link;
using namespace esp_modem_link::protocol;
using namespace esp_modem_link::testing;

namespace {

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

  // One response per Send() on every transport, in order. This is what stages a
  // redirect chain that keep-alive carries over a single connection, where the
  // per-connection supply above would answer every hop with the same redirect.
  void SetResponseSequence(std::vector<std::string> responses) {
    script_.sequence = std::move(responses);
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

// A response that points somewhere else. The length is declared and zero, so
// the client sees a whole response rather than one that is only finished when
// the peer hangs up.
std::string RedirectTo(int status, std::string_view location) {
  return "HTTP/1.1 " + std::to_string(status) +
         " Moved\r\n"
         "Location: " +
         std::string(location) +
         "\r\n"
         "Content-Length: 0\r\n"
         "\r\n";
}

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

// --- Redirects ---

TEST(SoftwareHttpClientTest, ExecuteFollowsARedirectToAnotherHost) {
  ClientUnderTest test;
  test.SetResponsesInTurn(
      {RedirectTo(302, "http://other.example/landing"), kOkResponse});

  auto result = test.client().Execute("GET", "http://example.com/start");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  // The caller is handed the answer to the request that was finally made, not
  // the redirect that pointed at it.
  EXPECT_EQ(result->status_code, 200);
  EXPECT_EQ(result->body, "hello");

  ASSERT_EQ(test.transport_count(), 2u);
  EXPECT_EQ(test.transport(1).last_host, "other.example");
  EXPECT_NE(test.transport(1).sent.find("GET /landing HTTP/1.1\r\n"),
            std::string::npos);
  EXPECT_NE(test.transport(1).sent.find("Host: other.example\r\n"),
            std::string::npos);
}

TEST(SoftwareHttpClientTest, ExecuteFollowsARelativeRedirectOnTheSameHost) {
  ClientUnderTest test;
  test.SetResponsesInTurn({RedirectTo(302, "sibling"), kOkResponse});

  auto result = test.client().Execute("GET", "http://example.com/dir/page");
  ASSERT_TRUE(result.has_value()) << result.error().Message();

  ASSERT_EQ(test.transport_count(), 2u);
  EXPECT_EQ(test.transport(1).last_host, "example.com");
  EXPECT_NE(test.transport(1).sent.find("GET /dir/sibling HTTP/1.1\r\n"),
            std::string::npos);
}

// A chain that stays on one origin is carried over one socket, which is only
// possible because the body of each response is read before the next request
// goes out on it.
TEST(SoftwareHttpClientTest, ExecuteFollowsAChainOverOneKeepAliveConnection) {
  ClientUnderTest test;
  test.client().SetKeepAlive(true);
  test.SetResponseSequence(
      {RedirectTo(302, "/first"), RedirectTo(308, "/second"), kOkResponse});

  auto result = test.client().Execute("GET", "http://example.com/start");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result->status_code, 200);

  EXPECT_EQ(test.transport_count(), 1u);
  EXPECT_EQ(test.transport().connect_calls, 1);
  ASSERT_EQ(test.transport().sends.size(), 3u);
  EXPECT_NE(test.transport().sent.find("GET /first HTTP/1.1\r\n"),
            std::string::npos);
  EXPECT_NE(test.transport().sent.find("GET /second HTTP/1.1\r\n"),
            std::string::npos);
}

TEST(SoftwareHttpClientTest, A301TurnsAPostIntoAGetWithoutItsBody) {
  ClientUnderTest test;
  test.SetResponsesInTurn({RedirectTo(301, "/moved"), kOkResponse});
  test.client().SetBody("payload");

  auto result = test.client().Execute("POST", "http://example.com/submit");
  ASSERT_TRUE(result.has_value()) << result.error().Message();

  ASSERT_EQ(test.transport_count(), 2u);
  EXPECT_EQ(test.transport(0).sent.rfind("POST /submit", 0), 0u);
  EXPECT_EQ(test.transport(1).sent.rfind("GET /moved", 0), 0u);
  // The body belongs to the request that was not made.
  EXPECT_EQ(test.transport(1).sent.find("payload"), std::string::npos);
  EXPECT_NE(test.transport(1).sent.find("Content-Length: 0\r\n"),
            std::string::npos);
}

TEST(SoftwareHttpClientTest, A302AlsoTurnsAPostIntoAGet) {
  ClientUnderTest test;
  test.SetResponsesInTurn({RedirectTo(302, "/moved"), kOkResponse});
  test.client().SetBody("payload");

  ASSERT_TRUE(test.client().Execute("POST", "http://example.com/submit")
                  .has_value());
  EXPECT_EQ(test.transport(1).sent.rfind("GET /moved", 0), 0u);
}

// 303 is defined to mean "the answer is here, go and read it", whatever the
// method was, so even a PUT that would hold its method elsewhere becomes a GET.
TEST(SoftwareHttpClientTest, A303TurnsAnyMethodIntoAGet) {
  ClientUnderTest test;
  test.SetResponsesInTurn({RedirectTo(303, "/see-other"), kOkResponse});
  test.client().SetBody("payload");

  ASSERT_TRUE(test.client().Execute("PUT", "http://example.com/thing")
                  .has_value());
  EXPECT_EQ(test.transport(1).sent.rfind("GET /see-other", 0), 0u);
  EXPECT_EQ(test.transport(1).sent.find("payload"), std::string::npos);
}

// The other half of the rule: 307 and 308 say the request itself is to be
// repeated somewhere else, so the method and the body go with it. A POST resent
// as a bodyless GET would land as a different request altogether.
TEST(SoftwareHttpClientTest, A307AndA308SendTheSameRequestSomewhereElse) {
  for (int status : {307, 308}) {
    ClientUnderTest test;
    test.SetResponsesInTurn({RedirectTo(status, "/again"), kOkResponse});
    test.client().SetBody("payload");

    auto result = test.client().Execute("POST", "http://example.com/submit");
    ASSERT_TRUE(result.has_value()) << result.error().Message();

    ASSERT_EQ(test.transport_count(), 2u) << "status " << status;
    EXPECT_EQ(test.transport(1).sent.rfind("POST /again", 0), 0u)
        << "status " << status;
    EXPECT_NE(test.transport(1).sent.find("payload"), std::string::npos)
        << "status " << status;
    EXPECT_NE(test.transport(1).sent.find("Content-Length: 7\r\n"),
              std::string::npos)
        << "status " << status;
  }
}

// A server that keeps redirecting is not a resource, and a client that keeps
// asking forever is worse than one that gives up.
TEST(SoftwareHttpClientTest, ARedirectLoopStopsAtTheHopLimit) {
  ClientUnderTest test;
  test.SetResponse(RedirectTo(302, "/loop"));

  auto result = test.client().Execute("GET", "http://example.com/loop");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kProtocolError);
  EXPECT_NE(result.error().context.find("redirected more than"),
            std::string::npos);
  // The first request plus the allowed hops, and not one more.
  EXPECT_EQ(test.transport_count(), static_cast<size_t>(kMaxRedirects) + 1);
}

// A caller that wants to read the redirect itself - to see where it points, or
// to decide for their own reasons not to go there - can ask for it.
TEST(SoftwareHttpClientTest, TurningRedirectsOffHandsBackTheRedirect) {
  ClientUnderTest test;
  test.SetResponsesInTurn({RedirectTo(302, "http://other.example/landing"),
                           kOkResponse});
  test.client().SetFollowRedirects(false);

  auto result = test.client().Execute("GET", "http://example.com/start");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result->status_code, 302);
  // The parser lowercases field names, so that is how they come back.
  EXPECT_EQ(result->headers["location"], "http://other.example/landing");
  EXPECT_EQ(test.transport_count(), 1u);
}

TEST(SoftwareHttpClientTest, ALowerHopLimitStopsTheChainSooner) {
  ClientUnderTest test;
  test.SetResponse(RedirectTo(302, "/loop"));
  test.client().SetMaxRedirects(1);

  auto result = test.client().Execute("GET", "http://example.com/loop");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kProtocolError);
  EXPECT_NE(result.error().context.find("more than 1 times"), std::string::npos);
  // One request plus the one hop that was allowed.
  EXPECT_EQ(test.transport_count(), 2u);
}

// A limit of zero is not a licence to follow forever, and not an error either:
// it is the same answer as turning following off.
TEST(SoftwareHttpClientTest, AZeroHopLimitIsTheSameAsTurningRedirectsOff) {
  ClientUnderTest test;
  test.SetResponsesInTurn({RedirectTo(302, "/somewhere"), kOkResponse});
  test.client().SetMaxRedirects(0);

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result->status_code, 302);
  EXPECT_EQ(test.transport_count(), 1u);
}

// A negative limit is not a way to ask for an unbounded chain.
TEST(SoftwareHttpClientTest, ANegativeHopLimitFollowsNothing) {
  ClientUnderTest test;
  test.SetResponsesInTurn({RedirectTo(302, "/somewhere"), kOkResponse});
  test.client().SetMaxRedirects(-1);

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result->status_code, 302);
  EXPECT_EQ(test.transport_count(), 1u);
}

// There is nowhere to go, so the response the server sent is its answer. It is
// handed back rather than turned into an error: a caller reading the status can
// see for itself that it was a 302.
TEST(SoftwareHttpClientTest, ARedirectWithoutALocationIsTheFinalAnswer) {
  ClientUnderTest test;
  test.SetResponsesInTurn(
      {"HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n", kOkResponse});

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result->status_code, 302);
  EXPECT_EQ(test.transport_count(), 1u);
}

// 304 is a cache answer, not a redirect, even when it carries a Location.
TEST(SoftwareHttpClientTest, A304IsNotFollowed) {
  ClientUnderTest test;
  test.SetResponsesInTurn(
      {"HTTP/1.1 304 Not Modified\r\nLocation: /new\r\nContent-Length: 0\r\n"
       "\r\n",
       kOkResponse});

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result->status_code, 304);
  EXPECT_EQ(test.transport_count(), 1u);
}

TEST(SoftwareHttpClientTest, ARedirectToASchemeItCannotSpeakFails) {
  ClientUnderTest test;
  test.SetResponsesInTurn({RedirectTo(302, "ftp://files.example/x"),
                           kOkResponse});

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kProtocolError);
  EXPECT_EQ(test.transport_count(), 1u);
}

// Credentials belong to the host they were written for. Following a Location
// onto somebody else's server with an Authorization header still attached would
// hand that server the key, so the header is dropped for the hop across.
TEST(SoftwareHttpClientTest, ACredentialIsNotSentToAnotherOrigin) {
  ClientUnderTest test;
  test.SetResponsesInTurn({RedirectTo(302, "http://other.example/landing"),
                           kOkResponse});
  test.client().SetHeader("Authorization", "Bearer secret");
  test.client().SetHeader("X-Trace", "keep-me");

  auto result = test.client().Execute("GET", "http://example.com/");
  ASSERT_TRUE(result.has_value()) << result.error().Message();

  EXPECT_NE(test.transport(0).sent.find("Authorization: Bearer secret"),
            std::string::npos);
  EXPECT_EQ(test.transport(1).sent.find("secret"), std::string::npos);
  EXPECT_EQ(test.transport(1).sent.find("Authorization"), std::string::npos);
  // Everything that is not bound to the origin still applies where it lands.
  EXPECT_NE(test.transport(1).sent.find("X-Trace: keep-me"), std::string::npos);
}

TEST(SoftwareHttpClientTest, ASameOriginRedirectKeepsTheCredential) {
  ClientUnderTest test;
  test.SetResponsesInTurn({RedirectTo(302, "http://example.com/other"),
                           kOkResponse});
  test.client().SetHeader("Authorization", "Bearer secret");

  ASSERT_TRUE(test.client().Execute("GET", "http://example.com/").has_value());

  ASSERT_EQ(test.transport_count(), 2u);
  EXPECT_NE(test.transport(1).sent.find("Authorization: Bearer secret"),
            std::string::npos);
}

// The same host at another port, or over another scheme, is another origin by
// definition: what the credential is good for is that server, not that name.
TEST(SoftwareHttpClientTest, AnotherPortOrSchemeIsAnotherOrigin) {
  for (const char* location : {"http://example.com:8080/x",
                               "https://example.com/x"}) {
    ClientUnderTest test;
    test.SetResponsesInTurn({RedirectTo(302, location), kOkResponse});
    test.client().SetHeader("Authorization", "Bearer secret");

    auto result = test.client().Execute("GET", "http://example.com/");
    ASSERT_TRUE(result.has_value()) << result.error().Message();

    ASSERT_EQ(test.transport_count(), 2u) << location;
    EXPECT_EQ(test.transport(1).sent.find("secret"), std::string::npos)
        << location;
  }
}

// A socket to the host that answered cannot carry a request to a different
// one, so a redirect that crosses hosts replaces the connection even when the
// caller asked for keep-alive.
TEST(SoftwareHttpClientTest, AKeepAliveRedirectToAnotherHostOpensASecondConnection) {
  ClientUnderTest test;
  test.client().SetKeepAlive(true);
  test.SetResponsesInTurn(
      {RedirectTo(302, "http://other.example/landing"), kOkResponse});

  auto result = test.client().Execute("GET", "http://example.com/start");
  ASSERT_TRUE(result.has_value()) << result.error().Message();
  EXPECT_EQ(result->status_code, 200);

  EXPECT_EQ(test.transport_count(), 2u);
  EXPECT_EQ(test.transport(0).disconnect_calls, 1);
  EXPECT_EQ(test.transport(1).last_host, "other.example");
}

TEST(SoftwareHttpClientTest, OpenFollowsARedirect) {  ClientUnderTest test;
  test.client().SetKeepAlive(true);
  test.SetResponsesInTurn({RedirectTo(302, "http://other.example/moved"),
                           kOkResponse});

  auto opened = test.client().Open("GET", "http://example.com/start");
  ASSERT_TRUE(opened.has_value()) << opened.error().Message();

  // What the caller reads after Open() is the final response's status, not the
  // redirect that led to it.
  auto status = test.client().GetStatusCode();
  ASSERT_TRUE(status.has_value()) << status.error().Message();
  EXPECT_EQ(*status, 200);

  ASSERT_EQ(test.transport_count(), 2u);
  // The connection the redirect came in on is closed rather than reused: its
  // body was never read, so nothing left on that socket could be told apart
  // from the answer to the next request.
  EXPECT_EQ(test.transport(0).disconnect_calls, 1);
  EXPECT_EQ(test.transport(1).last_host, "other.example");

  auto body = Drain(test.client());
  ASSERT_TRUE(body.has_value()) << body.error().Message();
  EXPECT_EQ(*body, "hello");
}

}  // namespace
