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
  // Leaves every send before `index` alone, so a test can reach a Send() that
  // only a request with a body behind its head has.
  void SetFailSendAt(size_t index) { script_.fail_send_at = index; }

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
  EXPECT_FALSE(test.client().IsChunked());
}

// A chunked response has no Content-Length to give, so the two accessors have
// to disagree: GetContentLength() reports the zero the header field would have
// been, and IsChunked() is the one that says there is a body coming anyway. A
// caller that read only the length would stop at a body it never saw.
TEST(SoftwareHttpClientTest, ReportsAChunkedBodyAsChunked) {
  ClientUnderTest test;
  test.SetResponse(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n");

  ASSERT_TRUE(test.client().Open("GET", "http://example.com/").has_value());

  EXPECT_TRUE(test.client().IsChunked());
  EXPECT_EQ(test.client().GetContentLength(), 0u);
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

// Write() streams a request body, and the head it is streaming into declares
// no length and says how it is framed instead. That is the contract: the old
// one rejected Write() outright because a Content-Length was already on the
// wire, and it stays rejected for a request that did not ask for chunking.
TEST(SoftwareHttpClientTest, WriteIsRejectedRatherThanSilentlyIgnored) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  ASSERT_TRUE(test.client().Open("GET", "http://example.com/").has_value());

  auto result = test.client().Write("data", 4);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotSupported);
}

// Opening a chunked upload puts the framing in the head and stops there: a
// server does not answer a request whose body it has not finished reading, so
// waiting for a response here would wait out the timeout on every upload.
TEST(SoftwareHttpClientTest, ChunkedOpenSendsTheHeadAndDoesNotWait) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.client().SetChunkedUpload(true);

  ASSERT_TRUE(
      test.client().Open("POST", "http://example.com/upload").has_value());

  const std::string& head = test.transport().sends.at(0);
  EXPECT_EQ(head.rfind("POST /upload HTTP/1.1\r\n", 0), 0u);
  EXPECT_NE(head.find("Transfer-Encoding: chunked\r\n"), std::string::npos);
  EXPECT_EQ(head.find("Content-Length"), std::string::npos);
  // Nothing to read yet, and that is the point of returning here.
  EXPECT_EQ(test.client().GetStatusCode().error().code,
            NetworkErrc::kNotConnected);
}

// Each Write() is one chunk, and one packet: a size in hex, the bytes, and the
// CRLF that closes the chunk.
TEST(SoftwareHttpClientTest, EachChunkIsFramedAsOnePacket) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());

  ASSERT_EQ(test.client().Write("hello", 5).value(), 5);
  ASSERT_EQ(test.client().Write("world", 5).value(), 5);

  const auto& sends = test.transport().sends;
  ASSERT_EQ(sends.size(), 3u);
  EXPECT_EQ(sends.at(1), "5\r\nhello\r\n");
  EXPECT_EQ(sends.at(2), "5\r\nworld\r\n");
}

// The size line is a hex number, not the decimal one, and it is not padded: a
// chunk of 26 bytes says "1a".
TEST(SoftwareHttpClientTest, AChunkSizeIsWrittenInHex) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());

  const std::string payload(26, 'x');
  ASSERT_EQ(test.client().Write(payload.data(), payload.size()).value(), 26);

  EXPECT_EQ(test.transport().sends.at(1), "1a\r\n" + payload + "\r\n");
}

// A zero-length piece is legal input and must send nothing at all: a
// zero-length chunk *is* the terminator, so writing one here would end the body
// early and leave EndBody() sending a terminator into the response.
TEST(SoftwareHttpClientTest, WritingNothingSendsNothing) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  const int before = test.transport().send_count.load();

  auto result = test.client().Write("", 0);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);
  EXPECT_EQ(test.transport().send_count.load(), before);
}

// A server is entitled to answer before the body is finished - a 302 to a POST,
// a 401, a 413 - and one that has answered stops reading. Writing further either
// fills a socket nobody is draining or, measured against www.baidu.com, reaches
// for one the peer has already closed: there the module answers the next
// AT+MIPSEND with "CME ERROR: 550", and the 302 that was already in the parser
// is lost behind a transport error that names nothing useful. So the write is
// refused with the status in hand instead, and the caller reads what stopped it.
TEST(SoftwareHttpClientTest, AServerThatAnswersMidBodyStopsTheUpload) {
  ClientUnderTest test;
  // The response lands on the second Send, which is the first chunk: the head
  // went out, and the server answered without waiting for the rest of the body.
  test.SetResponseSequence({"", RedirectTo(302, "https://other.example/")});
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  ASSERT_TRUE(test.client().Write("hello", 5).has_value());
  const int after_first_chunk = test.transport().send_count.load();

  auto refused = test.client().Write("world", 5);

  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code, NetworkErrc::kProtocolError);
  EXPECT_EQ(refused.error().native, 302);
  // Nothing further went out, and the answer that stopped the upload is there
  // to be read rather than swallowed.
  EXPECT_EQ(test.transport().send_count.load(), after_first_chunk);
  EXPECT_EQ(*test.client().GetStatusCode(), 302);
}

// EndBody() closes the body with the terminating chunk. The response that
// follows is staged on the fourth Send, which is where a real server puts it -
// so this also pins the number of packets the whole upload takes.
TEST(SoftwareHttpClientTest, EndBodyTerminatesTheBodyAndCollectsTheResponse) {
  ClientUnderTest test;
  test.SetResponseSequence({"", "", "", kOkResponse});
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  ASSERT_TRUE(test.client().Write("hello", 5).has_value());
  ASSERT_TRUE(test.client().Write("world", 5).has_value());

  auto ended = test.client().EndBody();

  ASSERT_TRUE(ended.has_value()) << ended.error().Message();
  EXPECT_EQ(test.transport().sends.size(), 4u);
  EXPECT_EQ(test.transport().sends.at(3), "0\r\n\r\n");
  // EndBody() waits for the head, so everything the caller does next is exactly
  // what it does after a plain Open().
  EXPECT_EQ(*test.client().GetStatusCode(), 200);
  auto body = Drain(test.client());
  ASSERT_TRUE(body.has_value()) << body.error().Message();
  EXPECT_EQ(*body, "hello");
}

// The terminator goes out once. A second one would be read by the server as the
// size line of the next request.
TEST(SoftwareHttpClientTest, EndingTheBodyTwiceIsRejected) {
  ClientUnderTest test;
  test.SetResponseSequence({"", "", kOkResponse});
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  ASSERT_TRUE(test.client().Write("hello", 5).has_value());
  ASSERT_TRUE(test.client().EndBody().has_value());
  const int after_first = test.transport().send_count.load();

  auto second = test.client().EndBody();

  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code, NetworkErrc::kInvalidArgument);
  EXPECT_EQ(test.transport().send_count.load(), after_first);
}

// Once the body has ended the server is reading the response, so further body
// bytes would land inside it.
TEST(SoftwareHttpClientTest, WritingAfterEndBodyIsRejected) {
  ClientUnderTest test;
  test.SetResponseSequence({"", "", kOkResponse});
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  ASSERT_TRUE(test.client().Write("hello", 5).has_value());
  ASSERT_TRUE(test.client().EndBody().has_value());

  auto late = test.client().Write("more", 4);

  ASSERT_FALSE(late.has_value());
  EXPECT_EQ(late.error().code, NetworkErrc::kInvalidArgument);
}

TEST(SoftwareHttpClientTest, WritingWithoutAnOpenRequestIsRejected) {
  ClientUnderTest test;
  test.client().SetChunkedUpload(true);

  auto result = test.client().Write("data", 4);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotConnected);
}

// After Close() the upload is over, so a Write() that follows is a caller who
// has lost track of the flow rather than one still sending.
TEST(SoftwareHttpClientTest, WritingAfterCloseIsRejected) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  test.client().Close();

  auto result = test.client().Write("data", 4);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotConnected);
}

// A second request cannot be started on a socket that is in the middle of a
// chunked body. With keep-alive the transport is reused, so the new head would
// be rendered straight into the open body and the server would read it as the
// next chunk-size line - while the caller was told the request succeeded.
TEST(SoftwareHttpClientTest, ASecondRequestCannotStartInsideAnOpenBody) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.client().SetKeepAlive(true);
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  ASSERT_TRUE(test.client().Write("hello", 5).has_value());

  auto second = test.client().Open("POST", "http://example.com/other");

  ASSERT_FALSE(second.has_value());
  EXPECT_EQ(second.error().code, NetworkErrc::kInvalidArgument);
  // The head and one chunk, and nothing else: the second head never reached the
  // socket, and no second connection was opened to carry it.
  EXPECT_EQ(test.transport().sends.size(), 2u);
  EXPECT_EQ(test.transport_count(), 1u);
}

// Execute() reaches the socket through the same send, so it has to be refused
// for the same reason - and its body was read before the upload began, so there
// is no version of this that could have been sent correctly.
TEST(SoftwareHttpClientTest, ExecuteCannotStartInsideAnOpenBody) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.client().SetKeepAlive(true);
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  ASSERT_TRUE(test.client().Write("hello", 5).has_value());

  auto executed = test.client().Execute("GET", "http://example.com/other");

  ASSERT_FALSE(executed.has_value());
  EXPECT_EQ(executed.error().code, NetworkErrc::kInvalidArgument);
  EXPECT_EQ(test.transport().sends.size(), 2u);
}

// The peer hanging up is known to the client as soon as it happens, so a chunk
// written afterwards is refused rather than reported as sent into a socket that
// is already gone.
TEST(SoftwareHttpClientTest, WritingAfterThePeerHangsUpIsRejected) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  ASSERT_TRUE(test.transport().last_client->DeliverClose());
  const int after_head = test.transport().send_count.load();

  auto result = test.client().Write("data", 4);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kNotConnected);
  EXPECT_EQ(test.transport().send_count.load(), after_head);
}

// EndBody() still sends its terminator - the body may well have reached a peer
// that answered and left - but a response that never arrives because the peer is
// gone has to take the socket with it. The request is complete by then, so
// leaving the socket in place would answer the *next* request with this one's
// status.
TEST(SoftwareHttpClientTest, EndingTheBodyAfterThePeerHangsUpLetsTheSocketGo) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  ASSERT_TRUE(test.transport().last_client->DeliverClose());

  auto ended = test.client().EndBody();

  ASSERT_FALSE(ended.has_value());
  EXPECT_EQ(ended.error().code, NetworkErrc::kConnectionLost);
  EXPECT_EQ(test.transport().disconnect_calls, 1);
}

// Asking for chunking and then never opening a request is a caller who has lost
// track of the flow, and the answer says so - it does not name the flag they
// have already set.
TEST(SoftwareHttpClientTest, EndingABodyThatWasNeverOpenedIsRejected) {
  ClientUnderTest test;
  test.client().SetChunkedUpload(true);

  auto ended = test.client().EndBody();

  ASSERT_FALSE(ended.has_value());
  EXPECT_EQ(ended.error().code, NetworkErrc::kNotConnected);
}

// Turning the flag off part way through cannot be allowed to strand the body:
// the head is on the wire declaring chunks, so the only ways out are the
// terminator and closing the connection. The latch Open() took is what governs.
TEST(SoftwareHttpClientTest, TurningChunkingOffMidUploadStillLetsTheBodyEnd) {
  ClientUnderTest test;
  test.SetResponseSequence({"", "", "", kOkResponse});
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  ASSERT_TRUE(test.client().Write("hello", 5).has_value());

  test.client().SetChunkedUpload(false);

  ASSERT_TRUE(test.client().Write("world", 5).has_value());
  auto ended = test.client().EndBody();
  ASSERT_TRUE(ended.has_value()) << ended.error().Message();
  EXPECT_EQ(test.transport().sends.at(3), "0\r\n\r\n");
}

// A body measured up front and a body streamed afterwards cannot both be the
// request's, and guessing which one the caller meant would send a head that
// disagrees with what follows it.
TEST(SoftwareHttpClientTest, AChunkedUploadRejectsABodySetInAdvance) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  test.client().SetChunkedUpload(true);
  test.client().SetBody("already measured");

  auto result = test.client().Open("POST", "http://example.com/");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kInvalidArgument);
  EXPECT_EQ(test.transport_count(), 0u);
}

// The interim "100 Continue" carries neither a length nor a Transfer-Encoding,
// so it would be read as a body that ends when the peer hangs up - with the
// real response head inside it.
TEST(SoftwareHttpClientTest, AChunkedUploadRejectsExpectContinue) {
  ClientUnderTest test;
  test.SetResponse(kOkResponse);
  test.client().SetChunkedUpload(true);
  test.client().SetHeader("Expect", "100-continue");

  auto result = test.client().Open("POST", "http://example.com/");

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kInvalidArgument);
  EXPECT_EQ(test.transport_count(), 0u);
}

// The rejection above has to be recoverable from, or a caller that set a body
// once can never use a chunked upload on the same client. An empty body is how
// that is said, and it has to be read as "no body" rather than as a body of
// length zero - which as a chunked upload is the same thing anyway, but as a
// Content-Length request would send "Content-Length: 0" where the caller meant
// to stream.
TEST(SoftwareHttpClientTest, AnEmptyBodyClearsTheWayForAChunkedUpload) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.client().SetChunkedUpload(true);
  test.client().SetBody("meant for another request");
  test.client().SetBody("");

  auto opened = test.client().Open("POST", "http://example.com/");

  ASSERT_TRUE(opened.has_value()) << opened.error().Message();
  const std::string& head = test.transport().sends.at(0);
  EXPECT_NE(head.find("Transfer-Encoding: chunked"), std::string::npos);
  EXPECT_EQ(head.find("Content-Length"), std::string::npos);
}

// A redirect asks for the request to be sent again somewhere else, and the body
// has already gone out. Following it is impossible, so it is reported rather
// than handed back as though it had been followed - and the connection is let
// go, since the redirect's own body was never read.
TEST(SoftwareHttpClientTest, AChunkedUploadDoesNotFollowARedirect) {
  ClientUnderTest test;
  test.SetResponseSequence({"", "", RedirectTo(307, "http://other.example/")});
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  ASSERT_TRUE(test.client().Write("hello", 5).has_value());

  auto ended = test.client().EndBody();

  ASSERT_FALSE(ended.has_value());
  EXPECT_EQ(ended.error().code, NetworkErrc::kProtocolError);
  EXPECT_EQ(ended.error().native, 307);
  // One transport: the redirect was not chased, and the second request was
  // never built.
  EXPECT_EQ(test.transport_count(), 1u);
  // Where the server pointed is still readable, which is what a caller acts on.
  EXPECT_EQ(test.client().GetResponseHeader("Location"),
            "http://other.example/");
}

// A chunk that will not go out leaves the body half-sent, and the request
// cannot be finished from there - the server is waiting for a terminator that
// is not coming. The connection goes with it.
//
// The send that fails is the chunk's, not the head's: a short write would leave
// the size line claiming more bytes than the socket carries, and neither the
// error path nor the short-write path can put the stream back in step, so both
// end here. The mock cannot produce a short write, so this covers the error.
TEST(SoftwareHttpClientTest, AChunkThatWillNotSendDropsTheConnection) {
  ClientUnderTest test;
  test.SuppressResponse();
  test.SetFailSendAt(1);  // the head goes out; the first chunk does not
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());

  auto result = test.client().Write("hello", 5);

  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, NetworkErrc::kTransmitFailed);
  EXPECT_EQ(test.transport().disconnect_calls, 1);
}

// A chunked upload is a streaming request, so it is Close() that gives the
// connection back, and only once the response has been read.
TEST(SoftwareHttpClientTest, CloseAfterAChunkedUploadReleasesTheTransport) {
  ClientUnderTest test;
  test.SetResponseSequence({"", "", kOkResponse});
  test.client().SetChunkedUpload(true);
  ASSERT_TRUE(test.client().Open("POST", "http://example.com/").has_value());
  ASSERT_TRUE(test.client().Write("hello", 5).has_value());
  ASSERT_TRUE(test.client().EndBody().has_value());

  test.client().Close();

  EXPECT_EQ(test.transport().disconnect_calls, 1u);
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
