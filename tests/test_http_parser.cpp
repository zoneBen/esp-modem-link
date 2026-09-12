#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "protocol/http/http_parser.h"

using namespace esp_modem_link;
using namespace esp_modem_link::protocol;

namespace {

constexpr const char* kSimple =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 5\r\n"
    "\r\n"
    "hello";

// Feeds one byte at a time. Every split point a real transport can produce -
// between the CR and the LF, inside a header name, mid chunk-size - shows up
// here, which is the whole reason the parser keeps a partial-line buffer.
::testing::AssertionResult FeedOneByteAtATime(HttpParser& parser,
                                              std::string_view data) {
  for (size_t i = 0; i < data.size(); ++i) {
    auto result = parser.Feed(data.substr(i, 1));
    if (!result) {
      return ::testing::AssertionFailure()
             << "byte " << i << ": " << result.error().Message();
    }
  }
  return ::testing::AssertionSuccess();
}

TEST(HttpParserTest, ParsesStatusLine) {
  HttpParser parser;
  ASSERT_TRUE(parser.Feed(kSimple).has_value());

  EXPECT_EQ(parser.GetStatusCode(), 200);
  EXPECT_EQ(parser.GetReasonPhrase(), "OK");
  EXPECT_EQ(parser.GetHttpMajor(), 1);
  EXPECT_EQ(parser.GetHttpMinor(), 1);
  EXPECT_TRUE(parser.IsComplete());
}

// The reason phrase is free text and routinely contains spaces, so splitting
// the status line on the first space would truncate it.
TEST(HttpParserTest, KeepsMultiWordReasonPhrase) {
  HttpParser parser;
  ASSERT_TRUE(parser.Feed("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
                  .has_value());

  EXPECT_EQ(parser.GetStatusCode(), 404);
  EXPECT_EQ(parser.GetReasonPhrase(), "Not Found");
}

TEST(HttpParserTest, HandlesEmptyReasonPhrase) {
  HttpParser parser;
  ASSERT_TRUE(
      parser.Feed("HTTP/1.1 204\r\nContent-Length: 0\r\n\r\n").has_value());

  EXPECT_EQ(parser.GetStatusCode(), 204);
  EXPECT_TRUE(parser.GetReasonPhrase().empty());
  EXPECT_TRUE(parser.IsComplete());
}

TEST(HttpParserTest, ParsesHeaders) {
  HttpParser parser;
  ASSERT_TRUE(parser.Feed(kSimple).has_value());

  EXPECT_EQ(parser.GetHeader("Content-Type"), "text/plain");
  EXPECT_EQ(parser.GetHeader("content-length"), "5");
  EXPECT_EQ(parser.GetBody(), "hello");
}

// Header names are case-insensitive per RFC 9110, so a lookup must be too -
// otherwise a server sending "CONTENT-LENGTH" reads as no length at all.
TEST(HttpParserTest, HeaderLookupIsCaseInsensitive) {
  HttpParser parser;
  ASSERT_TRUE(parser
                  .Feed("HTTP/1.1 200 OK\r\nCONTENT-LENGTH: 2\r\nX-Thing: v\r\n"
                        "\r\nhi")
                  .has_value());

  EXPECT_EQ(parser.GetHeader("content-length"), "2");
  EXPECT_EQ(parser.GetHeader("Content-Length"), "2");
  EXPECT_EQ(parser.GetHeader("x-THING"), "v");
  EXPECT_TRUE(parser.GetHeader("absent").empty());
}

TEST(HttpParserTest, StripsWhitespaceAroundHeaderValues) {
  HttpParser parser;
  ASSERT_TRUE(parser
                  .Feed("HTTP/1.1 200 OK\r\nContent-Length:    2   \r\n"
                        "Server:   nginx  \r\n\r\nhi")
                  .has_value());

  EXPECT_EQ(parser.GetHeader("Content-Length"), "2");
  EXPECT_EQ(parser.GetHeader("Server"), "nginx");
}

TEST(HttpParserTest, SplitsAcrossEveryByteBoundary) {
  HttpParser parser;
  ASSERT_TRUE(FeedOneByteAtATime(parser, kSimple));

  EXPECT_EQ(parser.GetStatusCode(), 200);
  EXPECT_EQ(parser.GetBody(), "hello");
  EXPECT_TRUE(parser.IsComplete());
}

TEST(HttpParserTest, BuffersUntilTheWholeResponseArrives) {
  HttpParser parser;
  // A partial status line must not be reported as a malformed one.
  ASSERT_TRUE(parser.Feed("HTTP/1.1 20").has_value());
  EXPECT_EQ(parser.GetStatusCode(), 0);
  EXPECT_FALSE(parser.IsComplete());

  ASSERT_TRUE(parser.Feed("0 OK\r\nContent-Length: 5\r\n").has_value());
  ASSERT_TRUE(parser.Feed("\r\nhello").has_value());

  EXPECT_EQ(parser.GetStatusCode(), 200);
  EXPECT_EQ(parser.GetBody(), "hello");
  EXPECT_TRUE(parser.IsComplete());
}

TEST(HttpParserTest, ReportsBytesConsumedSoTrailingDataIsLeftAlone) {
  HttpParser parser;
  std::string stream = std::string(kSimple) + "NEXT RESPONSE";

  auto consumed = parser.Feed(stream);
  ASSERT_TRUE(consumed.has_value());

  EXPECT_TRUE(parser.IsComplete());
  // Everything past the response body belongs to the next response.
  EXPECT_EQ(*consumed, std::string(kSimple).size());
}

TEST(HttpParserTest, HandlesContentLengthZero) {
  HttpParser parser;
  ASSERT_TRUE(parser.Feed("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n")
                  .has_value());

  EXPECT_TRUE(parser.IsComplete());
  EXPECT_TRUE(parser.GetBody().empty());
}

// A body may itself contain CRLF sequences - a binary payload or an HTML
// document - which must not be mistaken for framing.
TEST(HttpParserTest, BodyMayContainCrlf) {
  const std::string body = "line1\r\nline2\r\n";
  HttpParser parser;
  ASSERT_TRUE(parser
                  .Feed("HTTP/1.1 200 OK\r\nContent-Length: 14\r\n\r\n" + body)
                  .has_value());

  EXPECT_EQ(parser.GetBody(), body);
}

TEST(HttpParserTest, BodyMayContainNulBytes) {
  std::string body("\x00\x01\x02", 3);
  HttpParser parser;
  ASSERT_TRUE(parser
                  .Feed("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\n" + body)
                  .has_value());

  EXPECT_EQ(parser.GetBody(), body);
}

// --- Chunked transfer encoding ---

constexpr const char* kChunked =
    "HTTP/1.1 200 OK\r\n"
    "Transfer-Encoding: chunked\r\n"
    "\r\n"
    "5\r\nhello\r\n"
    "6\r\n world\r\n"
    "0\r\n"
    "\r\n";

TEST(HttpParserTest, ParsesChunkedBody) {
  HttpParser parser;
  ASSERT_TRUE(parser.Feed(kChunked).has_value());

  EXPECT_TRUE(parser.IsChunked());
  EXPECT_EQ(parser.GetBody(), "hello world");
  EXPECT_TRUE(parser.IsComplete());
}

TEST(HttpParserTest, ChunkedSplitsAcrossEveryByteBoundary) {
  HttpParser parser;
  ASSERT_TRUE(FeedOneByteAtATime(parser, kChunked));

  EXPECT_EQ(parser.GetBody(), "hello world");
  EXPECT_TRUE(parser.IsComplete());
}

// Chunk-size lines may carry extensions after a semicolon.
TEST(HttpParserTest, IgnoresChunkExtensions) {
  HttpParser parser;
  ASSERT_TRUE(parser
                  .Feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                        "5;name=value\r\nhello\r\n"
                        "0\r\n\r\n")
                  .has_value());

  EXPECT_EQ(parser.GetBody(), "hello");
  EXPECT_TRUE(parser.IsComplete());
}

TEST(HttpParserTest, AcceptsUppercaseHexChunkSize) {
  HttpParser parser;
  ASSERT_TRUE(parser
                  .Feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                        "A\r\n0123456789\r\n"
                        "0\r\n\r\n")
                  .has_value());

  EXPECT_EQ(parser.GetBody(), "0123456789");
}

TEST(HttpParserTest, ChunkedWinsOverContentLength) {
  // RFC 9112: when both are present, Transfer-Encoding decides the framing.
  HttpParser parser;
  ASSERT_TRUE(parser
                  .Feed("HTTP/1.1 200 OK\r\n"
                        "Content-Length: 99\r\n"
                        "Transfer-Encoding: chunked\r\n\r\n"
                        "2\r\nhi\r\n0\r\n\r\n")
                  .has_value());

  EXPECT_EQ(parser.GetBody(), "hi");
  EXPECT_TRUE(parser.IsComplete());
}

TEST(HttpParserTest, RejectsMalformedChunkSize) {
  HttpParser parser;
  auto result = parser.Feed(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\nhello");

  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(parser.HasError());
}

TEST(HttpParserTest, RejectsChunkDataWithoutCrlf) {
  HttpParser parser;
  auto result = parser.Feed(
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhiXX0\r\n\r\n");

  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(parser.HasError());
}

// --- Close-delimited bodies ---

// HTTP/1.0 and "Connection: close" responses carry no length; the body ends
// when the peer hangs up, which is the only signal available.
TEST(HttpParserTest, CloseDelimitedBodyCompletesOnFinish) {
  HttpParser parser;
  ASSERT_TRUE(
      parser.Feed("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nbody")
          .has_value());

  EXPECT_FALSE(parser.IsComplete());
  parser.Finish();

  EXPECT_TRUE(parser.IsComplete());
  EXPECT_EQ(parser.GetBody(), "body");
}

// A truncation must not be reported as a complete response: the module closing
// a socket mid-body is not the same as it delivering a short one.
TEST(HttpParserTest, TruncatedContentLengthIsNotComplete) {
  HttpParser parser;
  ASSERT_TRUE(parser
                  .Feed("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort")
                  .has_value());

  parser.Finish();

  EXPECT_FALSE(parser.IsComplete());
  EXPECT_FALSE(parser.HasError());
}

// --- Callbacks and reuse ---

TEST(HttpParserTest, BodyCallbackReceivesDataInArrivalOrder) {
  HttpParser parser;
  std::string streamed;
  parser.OnBody([&](std::string_view data) { streamed.append(data); });

  ASSERT_TRUE(parser
                  .Feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                        "3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n")
                  .has_value());

  EXPECT_EQ(streamed, "abcdef");
  // A streaming consumer asked not to accumulate, so the whole body is not
  // also sitting in memory.
  EXPECT_TRUE(parser.GetBody().empty());
}

TEST(HttpParserTest, ResetAllowsReuseForKeepAlive) {
  HttpParser parser;
  ASSERT_TRUE(parser.Feed(kSimple).has_value());
  parser.Reset();

  EXPECT_FALSE(parser.IsComplete());
  EXPECT_EQ(parser.GetStatusCode(), 0);
  EXPECT_TRUE(parser.GetBody().empty());
  EXPECT_TRUE(parser.GetHeaders().empty());

  ASSERT_TRUE(parser.Feed("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
                  .has_value());
  EXPECT_EQ(parser.GetStatusCode(), 404);
}

TEST(HttpParserTest, FeedsAfterCompletionAreIgnored) {
  HttpParser parser;
  ASSERT_TRUE(parser.Feed(kSimple).has_value());

  auto result = parser.Feed("garbage");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);
  EXPECT_EQ(parser.GetBody(), "hello");
}

// --- Malformed input ---

TEST(HttpParserTest, RejectsNonHttpStatusLine) {
  HttpParser parser;
  auto result = parser.Feed("+CME ERROR: 50\r\n");

  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(parser.HasError());
}

TEST(HttpParserTest, RejectsStatusLineWithoutCode) {
  HttpParser parser;
  auto result = parser.Feed("HTTP/1.1 \r\n");

  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(parser.HasError());
}

TEST(HttpParserTest, RejectsHeaderLineWithoutColon) {
  HttpParser parser;
  auto result = parser.Feed("HTTP/1.1 200 OK\r\nThisIsNotAHeader\r\n\r\n");

  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(parser.HasError());
}

TEST(HttpParserTest, RejectsMalformedContentLength) {
  HttpParser parser;
  auto result = parser.Feed("HTTP/1.1 200 OK\r\nContent-Length: abc\r\n\r\n");

  ASSERT_FALSE(result.has_value());
  EXPECT_TRUE(parser.HasError());
}

}  // namespace
