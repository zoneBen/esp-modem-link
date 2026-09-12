#include <gtest/gtest.h>

#include <string>

#include "protocol/http/http_request_builder.h"

using namespace esp_modem_link;
using namespace esp_modem_link::protocol;

namespace {

TEST(ParseUrlTest, PlainHttpDefaults) {
  auto parsed = ParseUrl("http://example.com/");
  ASSERT_TRUE(parsed.has_value()) << parsed.error().Message();

  EXPECT_FALSE(parsed->tls);
  EXPECT_EQ(parsed->host, "example.com");
  EXPECT_EQ(parsed->port, 80);
  EXPECT_EQ(parsed->path, "/");
}

TEST(ParseUrlTest, HttpsSelectsTlsAndPort443) {
  auto parsed = ParseUrl("https://example.com/");
  ASSERT_TRUE(parsed.has_value());

  EXPECT_TRUE(parsed->tls);
  EXPECT_EQ(parsed->port, 443);
}

TEST(ParseUrlTest, KeepsPathAndQuery) {
  auto parsed = ParseUrl("http://example.com/api/v1?x=1&y=2");
  ASSERT_TRUE(parsed.has_value());

  EXPECT_EQ(parsed->host, "example.com");
  EXPECT_EQ(parsed->path, "/api/v1?x=1&y=2");
}

// A URL with no path still needs one on the request line; "GET  HTTP/1.1"
// would be malformed.
TEST(ParseUrlTest, MissingPathBecomesSlash) {
  auto parsed = ParseUrl("http://example.com");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->path, "/");
}

TEST(ParseUrlTest, BareQueryGetsAPath) {
  auto parsed = ParseUrl("http://example.com?q=1");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->path, "/?q=1");
}

TEST(ParseUrlTest, ExplicitPort) {
  auto parsed = ParseUrl("http://example.com:8080/x");
  ASSERT_TRUE(parsed.has_value());

  EXPECT_EQ(parsed->host, "example.com");
  EXPECT_EQ(parsed->port, 8080);
}

TEST(ParseUrlTest, HttpsWithExplicitPort) {
  auto parsed = ParseUrl("https://example.com:8443/x");
  ASSERT_TRUE(parsed.has_value());

  EXPECT_TRUE(parsed->tls);
  EXPECT_EQ(parsed->port, 8443);
}

// A trailing colon is how an IPv6 literal without brackets looks; treating it
// as a port would silently send the request nowhere.
TEST(ParseUrlTest, HostWithNoPortAfterColonIsRejected) {
  auto parsed = ParseUrl("http://example.com:/x");
  EXPECT_FALSE(parsed.has_value());
}

TEST(ParseUrlTest, RejectsMissingScheme) {
  auto parsed = ParseUrl("example.com/");
  ASSERT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().code, NetworkErrc::kInvalidArgument);
}

TEST(ParseUrlTest, RejectsUnsupportedScheme) {
  EXPECT_FALSE(ParseUrl("ftp://example.com/").has_value());
  EXPECT_FALSE(ParseUrl("file:///etc/passwd").has_value());
}

TEST(ParseUrlTest, RejectsEmptyHost) {
  EXPECT_FALSE(ParseUrl("http://").has_value());
  EXPECT_FALSE(ParseUrl("http:///path").has_value());
}

TEST(ParseUrlTest, RejectsNonNumericPort) {
  EXPECT_FALSE(ParseUrl("http://example.com:http/").has_value());
}

TEST(ParseUrlTest, RejectsOutOfRangePort) {
  EXPECT_FALSE(ParseUrl("http://example.com:70000/").has_value());
  EXPECT_FALSE(ParseUrl("http://example.com:0/").has_value());
}

TEST(ParseUrlTest, RejectsEmptyUrl) {
  EXPECT_FALSE(ParseUrl("").has_value());
}

// --- Request rendering ---

ParsedUrl MustParse(std::string_view url) {
  auto parsed = ParseUrl(url);
  EXPECT_TRUE(parsed.has_value()) << parsed.error().Message();
  return parsed ? *parsed : ParsedUrl{};
}

TEST(BuildHttpRequestTest, RendersRequestLineAndHost) {
  HttpRequestOptions options;
  options.method = "GET";

  std::string request = BuildHttpRequest(MustParse("http://example.com/a"), options);

  EXPECT_EQ(request.rfind("GET /a HTTP/1.1\r\n", 0), 0u);
  EXPECT_NE(request.find("Host: example.com\r\n"), std::string::npos);
  EXPECT_NE(request.find("\r\n\r\n"), std::string::npos);
}

TEST(BuildHttpRequestTest, OmitsDefaultPortFromHostHeader) {
  HttpRequestOptions options;

  std::string http = BuildHttpRequest(MustParse("http://example.com/"), options);
  EXPECT_NE(http.find("Host: example.com\r\n"), std::string::npos);

  std::string https = BuildHttpRequest(MustParse("https://example.com/"), options);
  EXPECT_NE(https.find("Host: example.com\r\n"), std::string::npos);
}

TEST(BuildHttpRequestTest, IncludesNonDefaultPortInHostHeader) {
  HttpRequestOptions options;
  std::string request = BuildHttpRequest(MustParse("http://example.com:8080/"), options);

  EXPECT_NE(request.find("Host: example.com:8080\r\n"), std::string::npos);
}

TEST(BuildHttpRequestTest, DefaultsToConnectionClose) {
  HttpRequestOptions options;
  std::string request = BuildHttpRequest(MustParse("http://example.com/"), options);

  EXPECT_NE(request.find("Connection: close\r\n"), std::string::npos);
}

TEST(BuildHttpRequestTest, KeepAliveIsRequested) {
  HttpRequestOptions options;
  options.keep_alive = true;
  std::string request = BuildHttpRequest(MustParse("http://example.com/"), options);

  EXPECT_NE(request.find("Connection: keep-alive\r\n"), std::string::npos);
}

// The framing is only correct if the length matches what actually follows it,
// so it is derived rather than accepted from the caller.
TEST(BuildHttpRequestTest, ContentLengthMatchesTheBody) {
  HttpRequestOptions options;
  options.method = "POST";
  options.body = "hello";

  std::string request = BuildHttpRequest(MustParse("http://example.com/"), options);

  EXPECT_NE(request.find("Content-Length: 5\r\n"), std::string::npos);
  EXPECT_EQ(request.substr(request.size() - 5), "hello");
}

TEST(BuildHttpRequestTest, EmptyBodyStillSendsAZeroLength) {
  HttpRequestOptions options;
  std::string request = BuildHttpRequest(MustParse("http://example.com/"), options);

  EXPECT_NE(request.find("Content-Length: 0\r\n"), std::string::npos);
}

TEST(BuildHttpRequestTest, BodyFollowsTheBlankLine) {
  HttpRequestOptions options;
  options.method = "PUT";
  options.body = "{\"a\":1}";

  std::string request = BuildHttpRequest(MustParse("http://example.com/"), options);

  auto blank = request.find("\r\n\r\n");
  ASSERT_NE(blank, std::string::npos);
  EXPECT_EQ(request.substr(blank + 4), "{\"a\":1}");
}

TEST(BuildHttpRequestTest, AppendsCustomHeaders) {
  HttpRequestOptions options;
  options.headers = {{"Accept", "application/json"},
                     {"X-Trace", "abc"}};

  std::string request = BuildHttpRequest(MustParse("http://example.com/"), options);

  EXPECT_NE(request.find("Accept: application/json\r\n"), std::string::npos);
  EXPECT_NE(request.find("X-Trace: abc\r\n"), std::string::npos);
}

// A caller-supplied header must replace the generated one in place; sending
// "Host" twice produces a request servers reject.
TEST(BuildHttpRequestTest, CustomHeaderReplacesGeneratedOne) {
  HttpRequestOptions options;
  options.headers = {{"Host", "override.example"}};

  std::string request = BuildHttpRequest(MustParse("http://example.com/"), options);

  EXPECT_NE(request.find("Host: override.example\r\n"), std::string::npos);
  EXPECT_EQ(request.find("Host: example.com"), std::string::npos);
  EXPECT_EQ(request.find("override.example"), request.rfind("override.example"));
}

TEST(BuildHttpRequestTest, HeaderOverrideIsCaseInsensitive) {
  HttpRequestOptions options;
  options.headers = {{"connection", "keep-alive"}};

  std::string request = BuildHttpRequest(MustParse("http://example.com/"), options);

  EXPECT_NE(request.find("Connection: keep-alive\r\n"), std::string::npos);
  EXPECT_EQ(request.find("Connection: close"), std::string::npos);
}

TEST(BuildHttpRequestTest, CustomUserAgentIsUsed) {
  HttpRequestOptions options;
  options.user_agent = "MyDevice/2.0";

  std::string request = BuildHttpRequest(MustParse("http://example.com/"), options);

  EXPECT_NE(request.find("User-Agent: MyDevice/2.0\r\n"), std::string::npos);
}

TEST(BuildHttpRequestTest, EveryHeaderLineUsesCrlf) {
  HttpRequestOptions options;
  options.headers = {{"Accept", "*/*"}};

  std::string request = BuildHttpRequest(MustParse("http://example.com/"), options);

  // No bare LF anywhere: every '\n' must be preceded by '\r'.
  for (size_t i = 0; i < request.size(); ++i) {
    if (request[i] == '\n') {
      ASSERT_GT(i, 0u);
      EXPECT_EQ(request[i - 1], '\r') << "bare LF at offset " << i;
    }
  }
}

}  // namespace
