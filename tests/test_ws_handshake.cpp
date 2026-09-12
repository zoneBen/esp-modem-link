#include <gtest/gtest.h>

#include <string>

#include "protocol/websocket/ws_handshake.h"

using namespace esp_modem_link;
using namespace esp_modem_link::protocol;

// RFC 6455 section 1.3 works this key through the whole handshake, and prints
// the accept value the server must answer with. It is the only place the two
// halves - the SHA-1 and the base64 - are pinned from outside this code, so a
// mistake in either one that the other happened to cancel out would still show
// up here.
TEST(WsHandshakeTest, ComputesTheAcceptValueFromTheSpecification) {
  EXPECT_EQ(ComputeSecWebSocketAccept("dGhlIHNhbXBsZSBub25jZQ=="),
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

// A second vector, from a key of a different length than the example above, so
// a digest that was only accidentally right for a twenty-four character key
// would be caught. Every SHA-1 digest leaves two bytes in its last group, so
// both of these end in a single '='.
TEST(WsHandshakeTest, ComputesTheAcceptValueForAShortKey) {
  EXPECT_EQ(ComputeSecWebSocketAccept("x"), "JVxkmtKFjDcI/Pm/AGPEKSN6wT8=");
}

TEST(WsHandshakeTest, GeneratesAKeyOfTheRequiredShape) {
  const std::string key = MakeSecWebSocketKey();
  // Sixteen bytes of base64 is twenty-four characters, the last of which are
  // the padding for the one byte of the last group.
  ASSERT_EQ(key.size(), 24u);
  EXPECT_EQ(key.substr(22), "==");
  for (char c : key.substr(0, 22)) {
    EXPECT_TRUE((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '+' || c == '/')
        << "unexpected character in key: " << key;
  }
}

// The key is drawn from entropy, not from a counter: two keys that were equal
// would mean the generator had degenerated into something predictable.
TEST(WsHandshakeTest, GeneratesADifferentKeyEachTime) {
  EXPECT_NE(MakeSecWebSocketKey(), MakeSecWebSocketKey());
}

// --- Response parsing -----------------------------------------------------

namespace {

constexpr std::string_view kAccept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

std::string ResponseHead(std::string_view status, std::string_view accept_line) {
  std::string head = "HTTP/1.1 ";
  head.append(status);
  head.append("\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n");
  head.append(accept_line);
  head.append("\r\n\r\n");
  return head;
}

}  // namespace

TEST(WsHandshakeParseTest, ReadsTheStatusAndTheAcceptValue) {
  const auto parsed = ParseHandshakeResponse(
      ResponseHead("101 Switching Protocols",
                   std::string("Sec-WebSocket-Accept: ") + std::string(kAccept)));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().Message();
  EXPECT_EQ(parsed->status, 101);
  EXPECT_EQ(parsed->accept, kAccept);
}

// Header names are case-insensitive per RFC 9110, and a server that spells it
// differently is still a server that completed the handshake.
TEST(WsHandshakeParseTest, MatchesTheHeaderNameCaseInsensitively) {
  const auto parsed = ParseHandshakeResponse(
      ResponseHead("101 Switching Protocols",
                   std::string("sec-websocket-accept: ") + std::string(kAccept)));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().Message();
  EXPECT_EQ(parsed->accept, kAccept);
}

// A refusal is reported as its status rather than as a parse failure: the
// caller has the key and can say what went wrong, and 403 and 404 are different
// problems for whoever is looking at the device.
TEST(WsHandshakeParseTest, ReportsARefusalAsItsStatusCode) {
  const auto parsed = ParseHandshakeResponse(
      ResponseHead("403 Forbidden", "Content-Length: 0"));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().Message();
  EXPECT_EQ(parsed->status, 403);
  EXPECT_TRUE(parsed->accept.empty());
}

TEST(WsHandshakeParseTest, ReportsAMissingAcceptAsEmpty) {
  const auto parsed =
      ParseHandshakeResponse(ResponseHead("101 Switching Protocols", ""));
  ASSERT_TRUE(parsed.has_value()) << parsed.error().Message();
  EXPECT_EQ(parsed->status, 101);
  EXPECT_TRUE(parsed->accept.empty());
}

TEST(WsHandshakeParseTest, RefusesBytesThatAreNotAHead) {
  const auto parsed = ParseHandshakeResponse("not a response at all\r\n\r\n");
  EXPECT_FALSE(parsed.has_value());
  EXPECT_EQ(parsed.error().code, NetworkErrc::kProtocolError);
}
