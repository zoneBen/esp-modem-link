#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>

#include "protocol/mqtt/mqtt_packet.h"

using namespace esp_modem_link;
using namespace esp_modem_link::protocol;

namespace {

// Byte sequences written out rather than escaped, so a wrong byte is visible as
// a wrong number instead of as mojibake.
std::string Bytes(std::initializer_list<int> bytes) {
  std::string out;
  for (int byte : bytes) out.push_back(static_cast<char>(byte));
  return out;
}

TEST(MqttRemainingLengthTest, EncodesTheSpecificationsTable) {
  // Section 2.2.3's own table, which is the only place the boundaries are
  // pinned down: each of these is the last value of one width or the first of
  // the next.
  EXPECT_EQ(EncodeRemainingLength(0), Bytes({0x00}));
  EXPECT_EQ(EncodeRemainingLength(127), Bytes({0x7F}));
  EXPECT_EQ(EncodeRemainingLength(128), Bytes({0x80, 0x01}));
  EXPECT_EQ(EncodeRemainingLength(16383), Bytes({0xFF, 0x7F}));
  EXPECT_EQ(EncodeRemainingLength(16384), Bytes({0x80, 0x80, 0x01}));
  EXPECT_EQ(EncodeRemainingLength(2097151), Bytes({0xFF, 0xFF, 0x7F}));
  EXPECT_EQ(EncodeRemainingLength(2097152),
            Bytes({0x80, 0x80, 0x80, 0x01}));
  EXPECT_EQ(EncodeRemainingLength(268435455),
            Bytes({0xFF, 0xFF, 0xFF, 0x7F}));
}

// A length no legal packet can carry is refused rather than truncated, because
// truncating would put a well-formed header in front of the wrong body length.
TEST(MqttRemainingLengthTest, RefusesMoreThanFourBytesCanHold) {
  EXPECT_TRUE(EncodeRemainingLength(268435456).empty());
}

TEST(MqttRemainingLengthTest, DecodesWhatItEncoded) {
  const size_t values[] = {0, 1, 127, 128, 16383, 16384, 2097151, 2097152,
                           268435455};
  for (size_t value : values) {
    const std::string encoded = EncodeRemainingLength(value);
    size_t consumed = 0;
    auto decoded = DecodeRemainingLength(encoded, consumed);
    ASSERT_TRUE(decoded.has_value()) << "value " << value;
    EXPECT_EQ(*decoded, value);
    EXPECT_EQ(consumed, encoded.size());
  }
}

TEST(MqttRemainingLengthTest, IncompleteEncodingIsNotAnAnswer) {
  size_t consumed = 0;
  EXPECT_FALSE(DecodeRemainingLength(Bytes({}), consumed).has_value());
  EXPECT_FALSE(DecodeRemainingLength(Bytes({0x80}), consumed).has_value());
  EXPECT_FALSE(DecodeRemainingLength(Bytes({0xFF, 0x80}), consumed).has_value());
}

// Four bytes all promising a fifth: the stream is not short, it is malformed.
TEST(MqttRemainingLengthTest, RefusesAnEncodingThatNeverEnds) {
  size_t consumed = 0;
  EXPECT_FALSE(
      DecodeRemainingLength(Bytes({0x80, 0x80, 0x80, 0x80}), consumed)
          .has_value());
  EXPECT_FALSE(
      DecodeRemainingLength(Bytes({0x80, 0x80, 0x80, 0x80, 0x01}), consumed)
          .has_value());
}

TEST(MqttPacketTest, TakesAPacketOffTheFront) {
  std::string buffer = Bytes({0xC0, 0x00});
  auto packet = TakePacket(buffer);
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->header.type, MqttPacketType::kPingreq);
  EXPECT_TRUE(packet->body.empty());
  // What it consumed is what it left behind, which is how a stream advances.
  EXPECT_TRUE(buffer.empty());
}

TEST(MqttPacketTest, ATruncatedPacketIsNotTaken) {
  // The header says twenty bytes; only three have arrived. Taking them would
  // mean parsing a body that is still on the wire.
  std::string buffer = Bytes({0x30, 0x14, 'a'});
  EXPECT_FALSE(TakePacket(buffer).has_value());
  EXPECT_EQ(buffer, Bytes({0x30, 0x14, 'a'}));
}

TEST(MqttPacketTest, ATruncatedHeaderIsNotTaken) {
  std::string buffer = Bytes({0x30});
  EXPECT_FALSE(TakePacket(buffer).has_value());
  EXPECT_EQ(buffer.size(), 1u);

  // The length field itself split across two reads.
  buffer = Bytes({0x30, 0x80});
  EXPECT_FALSE(TakePacket(buffer).has_value());
  EXPECT_EQ(buffer.size(), 2u);
}

TEST(MqttPacketTest, TwoPacketsInOneReadComeOutInOrder) {
  std::string buffer = EncodePingreq() + EncodeDisconnect();
  auto first = TakePacket(buffer);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->header.type, MqttPacketType::kPingreq);

  auto second = TakePacket(buffer);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->header.type, MqttPacketType::kDisconnect);
  EXPECT_TRUE(buffer.empty());
}

TEST(MqttPacketTest, FixedHeaderFlagsSurviveTheRoundTrip) {
  std::string buffer =
      EncodePublish(7, "t", "p", /*qos=*/1, /*retain=*/true, /*dup=*/true);
  auto packet = TakePacket(buffer);
  ASSERT_TRUE(packet.has_value());
  EXPECT_EQ(packet->header.type, MqttPacketType::kPublish);
  EXPECT_TRUE(packet->header.dup);
  EXPECT_EQ(packet->header.qos, 1);
  EXPECT_TRUE(packet->header.retain);
}

TEST(MqttConnectTest, EncodesACleanSessionWithNoCredentials) {
  MqttConnectOptions options;
  options.client_id = "cid";
  options.keep_alive_seconds = 60;
  const std::string expected = Bytes({0x10, 0x0F, 0x00, 0x04}) + "MQTT" +
                               Bytes({0x04, 0x02, 0x00, 0x3C, 0x00, 0x03}) +
                               "cid";
  EXPECT_EQ(EncodeConnect(options), expected);
}

TEST(MqttConnectTest, SetsEveryConnectFlagItsOptionsAskFor) {
  MqttConnectOptions options;
  options.client_id = "cid";
  options.clean_session = false;
  options.has_username = true;
  options.username = "u";
  options.has_password = true;
  options.password = "p";
  options.has_will = true;
  options.will = MqttWill{"wt", "wp", 1, true};

  const std::string packet = EncodeConnect(options);
  ASSERT_GT(packet.size(), 1u);
  // one byte past the protocol name: reserved, clean, will, will qos, will
  // retain, password, username.
  EXPECT_EQ(static_cast<uint8_t>(packet[9]),
            0x04 | 0x08 | 0x20 | 0x40 | 0x80);
}

TEST(MqttConnectTest, ConnectsWithNoClientIdAtAll) {
  // A broker-tolerated empty client id, which is legal with a clean session and
  // is what a device with no identity of its own sends.
  MqttConnectOptions options;
  options.client_id.clear();
  const std::string packet = EncodeConnect(options);
  ASSERT_GE(packet.size(), 2u);
  EXPECT_EQ(static_cast<uint8_t>(packet[1]),
            static_cast<uint8_t>(packet.size() - 2));
}

TEST(MqttPublishTest, EncodesAQoS0PayloadWithNoPacketId) {
  // Seven bytes shorter than the QoS 1 form of the same message: the packet id
  // is not there at all, and the remaining length says so.
  const std::string expected =
      Bytes({0x30, 0x0C, 0x00, 0x03}) + "abc" + "payload";
  // QoS 0 carries no packet id, so an id passed in must not reach the wire.
  EXPECT_EQ(EncodePublish(7, "abc", "payload", 0, false), expected);
}

TEST(MqttPublishTest, EncodesAQoS1PayloadWithItsPacketId) {
  const std::string expected =
      Bytes({0x32, 0x0E, 0x00, 0x03}) + "abc" + Bytes({0x00, 0x07}) +
      "payload";
  EXPECT_EQ(EncodePublish(7, "abc", "payload", 1, false), expected);
}

TEST(MqttPublishTest, EncodesAnEmptyPayload) {
  std::string buffer = EncodePublish(0, "t", "", 0, false);
  auto packet = TakePacket(buffer);
  ASSERT_TRUE(packet.has_value());
  auto message = ParsePublish(packet->header, packet->body);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->topic, "t");
  EXPECT_TRUE(message->payload.empty());
}

TEST(MqttPacketTest, EncodesSubscribeWithTheFixedFlagsTheSpecRequires) {
  const std::string expected =
      Bytes({0x82, 0x08, 0x00, 0x01, 0x00, 0x03}) + "abc" + Bytes({0x01});
  EXPECT_EQ(EncodeSubscribe(1, "abc", 1), expected);
}

TEST(MqttPacketTest, EncodesUnsubscribeWithTheFixedFlagsTheSpecRequires) {
  const std::string expected =
      Bytes({0xA2, 0x07, 0x00, 0x01, 0x00, 0x03}) + "abc";
  EXPECT_EQ(EncodeUnsubscribe(1, "abc"), expected);
}

TEST(MqttPacketTest, EncodesTheAcknowledgements) {
  EXPECT_EQ(EncodePacketIdAck(MqttPacketType::kPuback, 1),
            Bytes({0x40, 0x02, 0x00, 0x01}));
  EXPECT_EQ(EncodePacketIdAck(MqttPacketType::kPubrec, 1),
            Bytes({0x50, 0x02, 0x00, 0x01}));
  // PUBREL is the one acknowledgement with non-zero flags.
  EXPECT_EQ(EncodePacketIdAck(MqttPacketType::kPubrel, 1),
            Bytes({0x62, 0x02, 0x00, 0x01}));
  EXPECT_EQ(EncodePacketIdAck(MqttPacketType::kPubcomp, 1),
            Bytes({0x70, 0x02, 0x00, 0x01}));
}

TEST(MqttPacketTest, EncodesTheBodylessPackets) {
  EXPECT_EQ(EncodePingreq(), Bytes({0xC0, 0x00}));
  EXPECT_EQ(EncodeDisconnect(), Bytes({0xE0, 0x00}));
}

TEST(MqttConnackTest, ParsesAnAcceptedConnection) {
  auto connack = ParseConnack(Bytes({0x00, 0x00}));
  ASSERT_TRUE(connack.has_value());
  EXPECT_FALSE(connack->session_present);
  EXPECT_EQ(connack->code, MqttConnectReturnCode::kAccepted);

  connack = ParseConnack(Bytes({0x01, 0x00}));
  ASSERT_TRUE(connack.has_value());
  EXPECT_TRUE(connack->session_present);
}

TEST(MqttConnackTest, CarriesTheBrokersOwnRefusal) {
  auto connack = ParseConnack(Bytes({0x00, 0x05}));
  ASSERT_TRUE(connack.has_value());
  EXPECT_EQ(connack->code, MqttConnectReturnCode::kNotAuthorized);
  // The distinction matters to the caller: a rejection on credentials and a
  // rejection on protocol version are not the same problem to report.
  EXPECT_NE(std::string(DescribeConnectReturnCode(connack->code)),
            std::string(DescribeConnectReturnCode(
                MqttConnectReturnCode::kUnacceptableProtocol)));
}

TEST(MqttConnackTest, RefusesShapesTheProtocolDoesNotProduce) {
  EXPECT_FALSE(ParseConnack(Bytes({0x00})).has_value());
  EXPECT_FALSE(ParseConnack(Bytes({0x00, 0x00, 0x00})).has_value());
  // Reserved bits are zero in every CONNACK a 3.1.1 broker sends.
  EXPECT_FALSE(ParseConnack(Bytes({0x02, 0x00})).has_value());
  // Codes past the six the specification defines.
  EXPECT_FALSE(ParseConnack(Bytes({0x00, 0x06})).has_value());
}

TEST(MqttPublishParseTest, ReadsATopicAndPayload) {
  MqttFixedHeader header;
  header.type = MqttPacketType::kPublish;
  const std::string body = Bytes({0x00, 0x03}) + "abc" + "hello";
  auto message = ParsePublish(header, body);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->topic, "abc");
  EXPECT_EQ(message->payload, "hello");
  EXPECT_EQ(message->packet_id, 0);
}

TEST(MqttPublishParseTest, AQoS1PacketIsReadByItsHeaderNotItsShape) {
  MqttFixedHeader header;
  header.type = MqttPacketType::kPublish;
  header.qos = 1;
  const std::string body = Bytes({0x00, 0x03}) + "abc" + Bytes({0x00, 0x2A}) +
                           "hello";
  auto message = ParsePublish(header, body);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->topic, "abc");
  EXPECT_EQ(message->packet_id, 42);
  EXPECT_EQ(message->payload, "hello");

  // The same bytes read as QoS 0 are a topic followed by a payload that starts
  // with the packet id - which is exactly why the header decides.
  header.qos = 0;
  message = ParsePublish(header, body);
  ASSERT_TRUE(message.has_value());
  EXPECT_EQ(message->packet_id, 0);
  EXPECT_EQ(message->payload, Bytes({0x00, 0x2A}) + "hello");
}

TEST(MqttPublishParseTest, RefusesABodyThatEndsMidField) {
  MqttFixedHeader header;
  header.type = MqttPacketType::kPublish;

  EXPECT_FALSE(ParsePublish(header, Bytes({0x00})).has_value());
  // Topic length claims five bytes that are not there.
  EXPECT_FALSE(ParsePublish(header, Bytes({0x00, 0x05}) + "abc").has_value());
  // A topic with no payload is legal, but a QoS 1 topic with no packet id is
  // not: the id is not optional at that QoS.
  header.qos = 1;
  EXPECT_FALSE(ParsePublish(header, Bytes({0x00, 0x03}) + "abc").has_value());
}

TEST(MqttSubackTest, ReadsTheGrantedQoSOfEveryFilter) {
  auto suback = ParseSuback(Bytes({0x00, 0x0A, 0x00, 0x01, 0x80}));
  ASSERT_TRUE(suback.has_value());
  EXPECT_EQ(suback->packet_id, 10);
  ASSERT_EQ(suback->return_codes.size(), 3u);
  EXPECT_EQ(suback->return_codes[0], 0x00);
  EXPECT_EQ(suback->return_codes[1], 0x01);
  // 0x80 is the broker refusing that one filter, not a QoS.
  EXPECT_EQ(suback->return_codes[2], 0x80);
}

TEST(MqttSubackTest, RefusesASubackWithNoReturnCode) {
  EXPECT_FALSE(ParseSuback(Bytes({0x00, 0x0A})).has_value());
  EXPECT_FALSE(ParseSuback(Bytes({0x00})).has_value());
}

TEST(MqttPacketTest, ReadsAPacketIdAcknowledgement) {
  auto id = ParsePacketId(Bytes({0x00, 0x2A}));
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(*id, 42);
  EXPECT_FALSE(ParsePacketId(Bytes({0x00})).has_value());
  EXPECT_FALSE(ParsePacketId(Bytes({0x00, 0x01, 0x02})).has_value());
  EXPECT_FALSE(ParsePacketId(Bytes({})).has_value());
}

TEST(MqttPacketTest, EveryTypeRoundTripsThroughATake) {
  const std::string stream =
      EncodeConnect(MqttConnectOptions{}) +
      EncodePublish(1, "topic", "body", 1, false) +
      EncodeSubscribe(2, "topic", 2) + EncodeUnsubscribe(3, "topic") +
      EncodePacketIdAck(MqttPacketType::kPuback, 4) + EncodePingreq() +
      EncodeDisconnect();

  std::string buffer = stream;
  std::string rebuilt;
  while (auto packet = TakePacket(buffer)) {
    // Re-framing what was taken has to reproduce the original bytes exactly, so
    // a decode that lost or invented a byte cannot pass.
    rebuilt += EncodePacket(packet->header, packet->body);
  }
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(rebuilt, stream);
}

}  // namespace
