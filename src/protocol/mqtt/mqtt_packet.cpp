#include "protocol/mqtt/mqtt_packet.h"

namespace esp_modem_link::protocol {
namespace {

// The largest remaining length the four-byte encoding can express.
constexpr size_t kMaxRemainingLength = 268435455;

uint8_t FlagsFor(const MqttFixedHeader& header) {
  uint8_t flags = 0;
  if (header.dup) flags |= 0x08;
  flags |= static_cast<uint8_t>((header.qos & 0x03) << 1);
  if (header.retain) flags |= 0x01;
  return flags;
}

void AppendUint16(std::string& out, uint16_t value) {
  out.push_back(static_cast<char>((value >> 8) & 0xFF));
  out.push_back(static_cast<char>(value & 0xFF));
}

// Wraps an already-built body in its fixed header.
std::string Frame(MqttPacketType type, uint8_t flags, std::string body) {
  std::string out;
  out.reserve(body.size() + 4);
  out.push_back(static_cast<char>(
      (static_cast<uint8_t>(type) << 4) | (flags & 0x0F)));
  out += EncodeRemainingLength(body.size());
  out += body;
  return out;
}

std::string AckBody(uint16_t packet_id) {
  std::string body;
  AppendUint16(body, packet_id);
  return body;
}

}  // namespace

const char* DescribeConnectReturnCode(MqttConnectReturnCode code) {
  switch (code) {
    case MqttConnectReturnCode::kAccepted:
      return "connection accepted";
    case MqttConnectReturnCode::kUnacceptableProtocol:
      return "unacceptable protocol version";
    case MqttConnectReturnCode::kIdentifierRejected:
      return "client identifier rejected";
    case MqttConnectReturnCode::kServerUnavailable:
      return "server unavailable";
    case MqttConnectReturnCode::kBadCredentials:
      return "bad user name or password";
    case MqttConnectReturnCode::kNotAuthorized:
      return "not authorized";
  }
  return "unknown return code";
}

std::string EncodeRemainingLength(size_t value) {
  if (value > kMaxRemainingLength) return {};
  std::string out;
  do {
    uint8_t byte = static_cast<uint8_t>(value % 128);
    value /= 128;
    // The continuation bit says another byte follows, so it is set on every
    // byte but the last.
    if (value > 0) byte |= 0x80;
    out.push_back(static_cast<char>(byte));
  } while (value > 0);
  return out;
}

std::optional<size_t> DecodeRemainingLength(std::string_view buffer,
                                            size_t& consumed) {
  size_t value = 0;
  size_t multiplier = 1;
  for (size_t i = 0; i < 4; ++i) {
    if (i >= buffer.size()) return std::nullopt;  // incomplete, not malformed
    const auto byte = static_cast<uint8_t>(buffer[i]);
    value += static_cast<size_t>(byte & 0x7F) * multiplier;
    if ((byte & 0x80) == 0) {
      consumed = i + 1;
      return value;
    }
    multiplier *= 128;
  }
  // A fourth byte with the continuation bit still set has no fifth byte to
  // continue into, so the stream is not merely short.
  return std::nullopt;
}

void AppendString(std::string& out, std::string_view value) {
  AppendUint16(out, static_cast<uint16_t>(value.size()));
  out.append(value);
}

std::optional<std::string_view> ReadString(std::string_view buffer,
                                           size_t& offset) {
  if (offset + 2 > buffer.size()) return std::nullopt;
  const size_t length =
      (static_cast<size_t>(static_cast<uint8_t>(buffer[offset])) << 8) |
      static_cast<uint8_t>(buffer[offset + 1]);
  const size_t start = offset + 2;
  if (start + length > buffer.size()) return std::nullopt;
  offset = start + length;
  return buffer.substr(start, length);
}

std::optional<MqttPacket> TakePacket(std::string& buffer) {
  if (buffer.empty()) return std::nullopt;

  const auto first = static_cast<uint8_t>(buffer[0]);
  MqttPacket packet;
  packet.header.type = static_cast<MqttPacketType>(first >> 4);
  packet.header.dup = (first & 0x08) != 0;
  packet.header.qos = static_cast<uint8_t>((first >> 1) & 0x03);
  packet.header.retain = (first & 0x01) != 0;

  size_t length_bytes = 0;
  auto remaining = DecodeRemainingLength(std::string_view(buffer).substr(1),
                                         length_bytes);
  if (!remaining.has_value()) return std::nullopt;

  const size_t header_size = 1 + length_bytes;
  if (buffer.size() < header_size + *remaining) return std::nullopt;

  packet.header.remaining_length = *remaining;
  packet.body = buffer.substr(header_size, *remaining);
  buffer.erase(0, header_size + *remaining);
  return packet;
}

std::optional<MqttConnack> ParseConnack(std::string_view body) {
  // Fixed at two bytes by section 3.2: a different length is not a CONNACK this
  // version of the protocol produces.
  if (body.size() != 2) return std::nullopt;
  const auto flags = static_cast<uint8_t>(body[0]);
  const auto code = static_cast<uint8_t>(body[1]);
  if ((flags & 0xFE) != 0) return std::nullopt;  // reserved bits must be zero
  if (code > 5) return std::nullopt;
  return MqttConnack{(flags & 0x01) != 0,
                     static_cast<MqttConnectReturnCode>(code)};
}

std::optional<MqttPublishMessage> ParsePublish(const MqttFixedHeader& header,
                                               std::string_view body) {
  if (header.qos > 2) return std::nullopt;

  size_t offset = 0;
  auto topic = ReadString(body, offset);
  if (!topic.has_value()) return std::nullopt;

  MqttPublishMessage message;
  message.topic = std::string(*topic);
  message.qos = header.qos;
  message.retain = header.retain;
  if (header.qos > 0) {
    if (offset + 2 > body.size()) return std::nullopt;
    message.packet_id =
        static_cast<uint16_t>((static_cast<uint8_t>(body[offset]) << 8) |
                              static_cast<uint8_t>(body[offset + 1]));
    offset += 2;
  }
  message.payload = std::string(body.substr(offset));
  return message;
}

std::optional<MqttSuback> ParseSuback(std::string_view body) {
  if (body.size() < 3) return std::nullopt;  // id plus at least one code
  MqttSuback suback;
  suback.packet_id =
      static_cast<uint16_t>((static_cast<uint8_t>(body[0]) << 8) |
                            static_cast<uint8_t>(body[1]));
  for (size_t i = 2; i < body.size(); ++i) {
    suback.return_codes.push_back(static_cast<uint8_t>(body[i]));
  }
  return suback;
}

std::optional<uint16_t> ParsePacketId(std::string_view body) {
  if (body.size() != 2) return std::nullopt;
  return static_cast<uint16_t>((static_cast<uint8_t>(body[0]) << 8) |
                               static_cast<uint8_t>(body[1]));
}

std::string EncodeConnect(const MqttConnectOptions& options) {
  std::string body;
  AppendString(body, "MQTT");
  body.push_back(4);  // protocol level, 3.1.1

  uint8_t flags = 0;
  if (options.clean_session) flags |= 0x02;
  if (options.has_will) {
    flags |= 0x04;
    flags |= static_cast<uint8_t>((options.will.qos & 0x03) << 3);
    if (options.will.retain) flags |= 0x20;
  }
  if (options.has_password) flags |= 0x40;
  if (options.has_username) flags |= 0x80;
  body.push_back(static_cast<char>(flags));

  AppendUint16(body, options.keep_alive_seconds);
  AppendString(body, options.client_id);
  if (options.has_will) {
    AppendString(body, options.will.topic);
    AppendString(body, options.will.payload);
  }
  if (options.has_username) AppendString(body, options.username);
  if (options.has_password) AppendString(body, options.password);

  return Frame(MqttPacketType::kConnect, 0, std::move(body));
}

std::string EncodePublish(uint16_t packet_id,
                          std::string_view topic,
                          std::string_view payload,
                          uint8_t qos,
                          bool retain,
                          bool dup) {
  std::string body;
  AppendString(body, topic);
  if (qos > 0) AppendUint16(body, packet_id);
  body.append(payload);

  MqttFixedHeader header;
  header.dup = dup;
  header.qos = qos;
  header.retain = retain;
  return Frame(MqttPacketType::kPublish, FlagsFor(header), std::move(body));
}

std::string EncodeSubscribe(uint16_t packet_id,
                            std::string_view topic,
                            uint8_t qos) {
  std::string body = AckBody(packet_id);
  AppendString(body, topic);
  body.push_back(static_cast<char>(qos & 0x03));
  // Flags are fixed at 0b0010 for SUBSCRIBE and UNSUBSCRIBE alike (sections
  // 3.8.1 and 3.10.1); a broker is entitled to drop the connection over any
  // other value.
  return Frame(MqttPacketType::kSubscribe, 0x02, std::move(body));
}

std::string EncodeUnsubscribe(uint16_t packet_id, std::string_view topic) {
  std::string body = AckBody(packet_id);
  AppendString(body, topic);
  return Frame(MqttPacketType::kUnsubscribe, 0x02, std::move(body));
}

std::string EncodePacketIdAck(MqttPacketType type, uint16_t packet_id) {
  // PUBREL is the one acknowledgement whose flags are not zero (section 3.6.1).
  const uint8_t flags = type == MqttPacketType::kPubrel ? 0x02 : 0x00;
  return Frame(type, flags, AckBody(packet_id));
}

std::string EncodePacket(const MqttFixedHeader& header, std::string_view body) {
  return Frame(header.type, FlagsFor(header), std::string(body));
}

std::string EncodePingreq() {
  return Frame(MqttPacketType::kPingreq, 0, {});
}

std::string EncodeDisconnect() {
  return Frame(MqttPacketType::kDisconnect, 0, {});
}

}  // namespace esp_modem_link::protocol
