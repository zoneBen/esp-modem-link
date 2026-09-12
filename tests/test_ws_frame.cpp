#include <gtest/gtest.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

#include "protocol/websocket/ws_frame.h"

using namespace esp_modem_link;
using namespace esp_modem_link::protocol;

namespace {

std::string Bytes(std::initializer_list<int> bytes) {
  std::string out;
  for (int byte : bytes) out.push_back(static_cast<char>(byte));
  return out;
}

std::string Encode(WsOpcode opcode,
                   std::string_view payload,
                   bool fin = true,
                   std::optional<uint32_t> mask_key = std::nullopt) {
  auto encoded = EncodeFrame(opcode, payload, fin, mask_key);
  EXPECT_TRUE(encoded.has_value());
  return encoded.value_or(std::string());
}

// The key's four bytes are all different, so a masked payload that was shifted
// against the wrong byte shows up as a wrong value rather than as a byte that
// happens to be unchanged.
constexpr uint32_t kKey = 0x01020304;

uint8_t KeyByte(uint32_t key, size_t index) {
  return static_cast<uint8_t>(key >> (8 * (3 - (index % 4))));
}

}  // namespace

// --- Encoding -------------------------------------------------------------

// Section 5.7's own example: a single unmasked text frame carrying "Hello".
TEST(WsFrameEncodeTest, EncodesTheSpecificationsExample) {
  EXPECT_EQ(Encode(WsOpcode::kText, "Hello"), Bytes({0x81, 0x05}) + "Hello");
}

// The three length encodings, at the boundaries where one gives way to the
// next. Nothing else in the codec has as much room to be subtly wrong.
TEST(WsFrameEncodeTest, UsesTheShortestLengthEncodingThatFits) {
  // 125 is the largest length the seven-bit field holds.
  const std::string short_payload(125, 'a');
  EXPECT_EQ(Encode(WsOpcode::kBinary, short_payload),
            Bytes({0x82, 125}) + short_payload);

  // 126 is the first that needs the two-byte form, and stays there through
  // 65535.
  const std::string medium_payload(126, 'b');
  EXPECT_EQ(Encode(WsOpcode::kBinary, medium_payload),
            Bytes({0x82, 126, 0x00, 0x7E}) + medium_payload);

  const std::string longest_medium(65535, 'c');
  EXPECT_EQ(Encode(WsOpcode::kBinary, longest_medium),
            Bytes({0x82, 126, 0xFF, 0xFF}) + longest_medium);

  // 65536 is the first that needs all eight bytes.
  const std::string long_payload(65536, 'd');
  EXPECT_EQ(Encode(WsOpcode::kBinary, long_payload),
            Bytes({0x82, 127, 0, 0, 0, 0, 0, 1, 0, 0}) + long_payload);
}

TEST(WsFrameEncodeTest, MasksAClientFrameWithTheKeyItWasGiven) {
  // 'H'^0x01 and 'i'^0x02: the second payload byte meets the second key byte,
  // which is what proves the key is applied in the order it was written.
  EXPECT_EQ(Encode(WsOpcode::kText, "Hi", true, kKey),
            Bytes({0x81, 0x82, 0x01, 0x02, 0x03, 0x04}) +
                Bytes({'H' ^ 0x01, 'i' ^ 0x02}));
}

TEST(WsFrameEncodeTest, MasksEveryFourthPayloadByteWithTheSameKeyByte) {
  const std::string payload = "0123456789";
  const std::string frame = Encode(WsOpcode::kBinary, payload, true, kKey);
  ASSERT_EQ(frame.size(), payload.size() + 6);
  for (size_t i = 0; i < payload.size(); ++i) {
    EXPECT_EQ(static_cast<uint8_t>(frame[6 + i]),
              static_cast<uint8_t>(payload[i]) ^ KeyByte(kKey, i))
        << "at payload byte " << i;
  }
}

TEST(WsFrameEncodeTest, MasksWithWhateverKeyItWasGiven) {
  const std::string payload = "the quick brown fox";
  for (uint32_t key :
       {uint32_t{0}, uint32_t{0xFFFFFFFF}, uint32_t{0xDEADBEEF}}) {
    const std::string frame = Encode(WsOpcode::kText, payload, true, key);
    ASSERT_EQ(frame.size(), payload.size() + 6) << "key " << key;
    for (size_t i = 0; i < payload.size(); ++i) {
      EXPECT_EQ(static_cast<uint8_t>(frame[6 + i]),
                static_cast<uint8_t>(payload[i]) ^ KeyByte(key, i))
          << "key " << key << " at payload byte " << i;
    }
  }
}

TEST(WsFrameEncodeTest, ClearsTheFinBitForAFragment) {
  EXPECT_EQ(Encode(WsOpcode::kText, "ab", false), Bytes({0x01, 0x02}) + "ab");
}

// A continuation frame is opcode 0 and carries no meaning of its own: which
// message it continues was settled by the frame that started it.
TEST(WsFrameEncodeTest, EncodesAContinuationAsOpcodeZero) {
  EXPECT_EQ(Encode(WsOpcode::kContinuation, "ab"), Bytes({0x80, 0x02}) + "ab");
}

TEST(WsFrameEncodeTest, EncodesEachControlOpcode) {
  EXPECT_EQ(Encode(WsOpcode::kClose, Bytes({0x03, 0xE8})),
            Bytes({0x88, 0x02, 0x03, 0xE8}));
  EXPECT_EQ(Encode(WsOpcode::kPing, "hi"), Bytes({0x89, 0x02}) + "hi");
  EXPECT_EQ(Encode(WsOpcode::kPong, "hi"), Bytes({0x8A, 0x02}) + "hi");
}

TEST(WsFrameEncodeTest, EncodesAnEmptyPayload) {
  EXPECT_EQ(Encode(WsOpcode::kPing, ""), Bytes({0x89, 0x00}));
}

// A control frame that is fragmented or oversized is a frame no conforming
// sender may emit, so the encoder refuses rather than putting the fault on the
// wire where only the peer would find it.
TEST(WsFrameEncodeTest, RefusesToFragmentAControlFrame) {
  for (WsOpcode opcode : {WsOpcode::kClose, WsOpcode::kPing, WsOpcode::kPong}) {
    auto encoded = EncodeFrame(opcode, "x", false);
    ASSERT_FALSE(encoded.has_value());
    EXPECT_EQ(encoded.error().code, NetworkErrc::kInvalidArgument);
  }
}

TEST(WsFrameEncodeTest, RefusesAControlFramePastTheLimit) {
  auto at_limit = EncodeFrame(WsOpcode::kPing, std::string(125, 'x'));
  EXPECT_TRUE(at_limit.has_value());

  auto past_limit = EncodeFrame(WsOpcode::kPing, std::string(126, 'x'));
  ASSERT_FALSE(past_limit.has_value());
  EXPECT_EQ(past_limit.error().code, NetworkErrc::kInvalidArgument);
}

// A data frame is not subject to the control frame limit, so the refusal above
// must not have been written broadly enough to catch one.
TEST(WsFrameEncodeTest, AllowsADataFramePastTheControlLimit) {
  auto encoded = EncodeFrame(WsOpcode::kBinary, std::string(1000, 'x'));
  EXPECT_TRUE(encoded.has_value());
}

// --- Decoding -------------------------------------------------------------

TEST(WsFrameDecodeTest, ReadsTheSpecificationsExample) {
  std::string buffer = Bytes({0x81, 0x05}) + "Hello";
  const WsFrameRead read = TakeFrame(buffer);
  ASSERT_EQ(read.status, WsFrameStatus::kReady);
  EXPECT_TRUE(read.frame.fin);
  EXPECT_EQ(read.frame.opcode, WsOpcode::kText);
  EXPECT_EQ(read.frame.payload, "Hello");
  EXPECT_TRUE(buffer.empty());
}

TEST(WsFrameDecodeTest, ReadsEachLengthEncoding) {
  for (size_t length : {size_t{0}, size_t{125}, size_t{126}, size_t{65535},
                        size_t{65536}}) {
    const std::string payload(length, 'z');
    std::string buffer = Encode(WsOpcode::kBinary, payload);
    const WsFrameRead read = TakeFrame(buffer);
    ASSERT_EQ(read.status, WsFrameStatus::kReady) << "length " << length;
    EXPECT_EQ(read.frame.payload, payload);
    EXPECT_TRUE(buffer.empty());
  }
}

TEST(WsFrameDecodeTest, ReadsAFrameWhereverTheArrivalWasCut) {
  const std::string whole = Encode(WsOpcode::kText, "hello world");
  for (size_t cut = 0; cut < whole.size(); ++cut) {
    std::string buffer = whole.substr(0, cut);
    EXPECT_EQ(TakeFrame(buffer).status, WsFrameStatus::kIncomplete)
        << "cut at " << cut;
    EXPECT_EQ(buffer, whole.substr(0, cut)) << "cut at " << cut;

    buffer.append(whole.substr(cut));
    const WsFrameRead read = TakeFrame(buffer);
    ASSERT_EQ(read.status, WsFrameStatus::kReady) << "cut at " << cut;
    EXPECT_EQ(read.frame.payload, "hello world");
    EXPECT_TRUE(buffer.empty()) << "cut at " << cut;
  }
}

// A decoder that read past the header before knowing the length was present
// would index out of the buffer here, where the length arrives a byte at a
// time.
TEST(WsFrameDecodeTest, WaitsForEachPieceOfAnExtendedLength) {
  const std::string whole = Encode(WsOpcode::kBinary, std::string(70000, 'x'));
  for (size_t cut = 0; cut <= 10; ++cut) {
    std::string buffer = whole.substr(0, cut);
    EXPECT_EQ(TakeFrame(buffer).status, WsFrameStatus::kIncomplete)
        << "cut at " << cut;
  }
  std::string buffer = whole;
  EXPECT_EQ(TakeFrame(buffer).status, WsFrameStatus::kReady);
}

TEST(WsFrameDecodeTest, TakesTwoFramesOutOfOneArrival) {
  std::string buffer =
      Encode(WsOpcode::kText, "one") + Encode(WsOpcode::kText, "two");

  const WsFrameRead first = TakeFrame(buffer);
  ASSERT_EQ(first.status, WsFrameStatus::kReady);
  EXPECT_EQ(first.frame.payload, "one");

  const WsFrameRead second = TakeFrame(buffer);
  ASSERT_EQ(second.status, WsFrameStatus::kReady);
  EXPECT_EQ(second.frame.payload, "two");
  EXPECT_TRUE(buffer.empty());
}

TEST(WsFrameDecodeTest, ReadsAFragmentedMessageAsTwoFrames) {
  std::string buffer = Encode(WsOpcode::kText, "he", false) +
                       Encode(WsOpcode::kContinuation, "llo", true);

  const WsFrameRead first = TakeFrame(buffer);
  ASSERT_EQ(first.status, WsFrameStatus::kReady);
  EXPECT_FALSE(first.frame.fin);
  EXPECT_EQ(first.frame.opcode, WsOpcode::kText);
  EXPECT_EQ(first.frame.payload, "he");

  const WsFrameRead second = TakeFrame(buffer);
  ASSERT_EQ(second.status, WsFrameStatus::kReady);
  EXPECT_TRUE(second.frame.fin);
  EXPECT_EQ(second.frame.opcode, WsOpcode::kContinuation);
  EXPECT_EQ(second.frame.payload, "llo");
}

TEST(WsFrameDecodeTest, ReadsTheCloseCodeOutOfACloseFrame) {
  std::string buffer = Encode(WsOpcode::kClose, Bytes({0x03, 0xE8}) + "bye");
  const WsFrameRead read = TakeFrame(buffer);
  ASSERT_EQ(read.status, WsFrameStatus::kReady);
  EXPECT_EQ(read.frame.opcode, WsOpcode::kClose);
  EXPECT_EQ(read.frame.payload, Bytes({0x03, 0xE8}) + "bye");
}

// Every opcode goes out through the encoder and comes back through the decoder
// unchanged, flags and all. Anything the two disagree about is found here
// rather than by a peer.
TEST(WsFrameDecodeTest, EveryOpcodeRoundTripsThroughATake) {
  for (WsOpcode opcode :
       {WsOpcode::kContinuation, WsOpcode::kText, WsOpcode::kBinary,
        WsOpcode::kClose, WsOpcode::kPing, WsOpcode::kPong}) {
    for (bool fin : {true, false}) {
      // A control frame may not be fragmented, so only the data opcodes carry
      // the fin flag through both ways.
      if (!fin && IsControlOpcode(opcode)) continue;

      std::string buffer = Encode(opcode, "payload", fin);
      const WsFrameRead read = TakeFrame(buffer);
      ASSERT_EQ(read.status, WsFrameStatus::kReady);
      EXPECT_EQ(read.frame.opcode, opcode);
      EXPECT_EQ(read.frame.fin, fin);
      EXPECT_EQ(read.frame.payload, "payload");
      EXPECT_TRUE(buffer.empty());
    }
  }
}

// --- Refusals -------------------------------------------------------------

// Everything below is bytes RFC 6455 requires a client to fail the connection
// on (5.2, 5.5). None may be mistaken for a frame that is merely still
// arriving, which is the whole reason the two statuses are apart.
TEST(WsFrameDecodeTest, RefusesAReservedOpcode) {
  std::string buffer = Bytes({0x83, 0x00});
  EXPECT_EQ(TakeFrame(buffer).status, WsFrameStatus::kInvalid);
}

TEST(WsFrameDecodeTest, RefusesASetRsvBit) {
  std::string buffer = Bytes({0xC1, 0x00});
  EXPECT_EQ(TakeFrame(buffer).status, WsFrameStatus::kInvalid);
}

TEST(WsFrameDecodeTest, RefusesAMaskedFrameFromTheServer) {
  // A well-formed client frame, which a server is not allowed to send.
  std::string buffer = Encode(WsOpcode::kText, "hi", true, kKey);
  EXPECT_EQ(TakeFrame(buffer).status, WsFrameStatus::kInvalid);
}

TEST(WsFrameDecodeTest, RefusesAFragmentedControlFrame) {
  std::string buffer = Bytes({0x09, 0x01}) + "x";
  EXPECT_EQ(TakeFrame(buffer).status, WsFrameStatus::kInvalid);
}

TEST(WsFrameDecodeTest, RefusesAnOversizedControlFrame) {
  // Written out by hand, because the encoder refuses to produce one: the
  // decoder's own check is what stands between a misbehaving server and the
  // rest of the client.
  std::string buffer = Bytes({0x89, 126, 0x00, 0x7E}) + std::string(126, 'x');
  EXPECT_EQ(TakeFrame(buffer).status, WsFrameStatus::kInvalid);
}

TEST(WsFrameDecodeTest, RefusesALengthWithItsLeadingBitSet) {
  std::string buffer =
      Bytes({0x82, 127, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
  EXPECT_EQ(TakeFrame(buffer).status, WsFrameStatus::kInvalid);
}

// A refusal says which rule was broken: that string is what the application is
// told, and "invalid frame" alone does not separate a server bug from a fault
// in the link.
TEST(WsFrameDecodeTest, SaysWhichRuleARefusalBroke) {
  std::string buffer = Bytes({0xC1, 0x00});
  const WsFrameRead read = TakeFrame(buffer);
  ASSERT_EQ(read.status, WsFrameStatus::kInvalid);
  EXPECT_NE(std::string(read.reason).find("reserved bit"), std::string::npos);
}

// The bad bytes stay put. The caller is expected to fail the connection, and
// eating them would only mean the same failure could not be looked at again.
TEST(WsFrameDecodeTest, LeavesRefusedBytesInPlace) {
  std::string buffer = Bytes({0xC1, 0x00});
  TakeFrame(buffer);
  EXPECT_EQ(buffer.size(), 2u);
}

TEST(WsFrameDecodeTest, LeavesTheBytesOfAnIncompleteFrameAlone) {
  std::string buffer = Bytes({0x81, 0x0A}) + "abc";
  const WsFrameRead read = TakeFrame(buffer);
  EXPECT_EQ(read.status, WsFrameStatus::kIncomplete);
  EXPECT_EQ(buffer, Bytes({0x81, 0x0A}) + "abc");
}
