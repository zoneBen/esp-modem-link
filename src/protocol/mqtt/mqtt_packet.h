#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "esp_modem_link/config_types.h"

// MQTT 3.1.1 control packet encoding and decoding (OASIS spec, section 2).
//
// This is the wire format only: no connection state, no retries, no timeouts.
// Everything here is a pure function of its arguments, so a packet can be
// asserted byte for byte against the examples in the specification.
namespace esp_modem_link::protocol {

// The low nibble of the first byte (section 2.2.1). The values are the wire
// encoding, which is why they are named rather than used as literals.
enum class MqttPacketType : uint8_t {
  kConnect = 1,
  kConnack = 2,
  kPublish = 3,
  kPuback = 4,
  kPubrec = 5,
  kPubrel = 6,
  kPubcomp = 7,
  kSubscribe = 8,
  kSuback = 9,
  kUnsubscribe = 10,
  kUnsuback = 11,
  kPingreq = 12,
  kPingresp = 13,
  kDisconnect = 14,
};

// The CONNACK return code (section 3.2.2.3). Anything but kAccepted is a
// refusal, and the wording of the reason is the broker's to choose - so the
// value is carried through rather than flattened into one "refused" error.
enum class MqttConnectReturnCode : uint8_t {
  kAccepted = 0,
  kUnacceptableProtocol = 1,
  kIdentifierRejected = 2,
  kServerUnavailable = 3,
  kBadCredentials = 4,
  kNotAuthorized = 5,
};

const char* DescribeConnectReturnCode(MqttConnectReturnCode code);

struct MqttFixedHeader {
  MqttPacketType type = MqttPacketType::kConnect;
  bool dup = false;
  uint8_t qos = 0;
  bool retain = false;
  size_t remaining_length = 0;
};

// One whole packet: the fixed header plus its variable header and payload, still
// encoded. Decoding the body is left to the per-type helpers below, because the
// fixed header alone does not say which type the caller is waiting for.
struct MqttPacket {
  MqttFixedHeader header;
  std::string body;
};

// --- Decoding -------------------------------------------------------------

// Takes one whole packet off the front of `buffer`, erasing exactly the bytes it
// consumed. Returning nullopt means the buffer does not yet hold a complete
// packet, which is the normal state of a stream that was split across reads -
// not an error. A malformed remaining length consumes nothing and is reported
// the same way, so a caller that feeds the buffer again simply stalls.
std::optional<MqttPacket> TakePacket(std::string& buffer);

struct MqttConnack {
  bool session_present = false;
  MqttConnectReturnCode code = MqttConnectReturnCode::kAccepted;
};

std::optional<MqttConnack> ParseConnack(std::string_view body);

struct MqttPublishMessage {
  std::string topic;
  // Zero for QoS 0, which carries no packet id. The header's QoS is the only
  // thing that distinguishes the two layouts on the wire.
  uint16_t packet_id = 0;
  std::string payload;
};

std::optional<MqttPublishMessage> ParsePublish(const MqttFixedHeader& header,
                                               std::string_view body);

struct MqttSuback {
  uint16_t packet_id = 0;
  // One per requested filter, in order: 0x00/0x01/0x02 for the granted QoS, or
  // 0x80 for a filter the broker refused.
  std::vector<uint8_t> return_codes;
};

std::optional<MqttSuback> ParseSuback(std::string_view body);

// PUBACK, PUBREC, PUBREL, PUBCOMP and UNSUBACK all carry a packet id and
// nothing else.
std::optional<uint16_t> ParsePacketId(std::string_view body);

// --- Encoding -------------------------------------------------------------

struct MqttConnectOptions {
  std::string client_id;
  std::string username;
  std::string password;
  bool has_username = false;
  bool has_password = false;
  bool clean_session = true;
  uint16_t keep_alive_seconds = 60;
  bool has_will = false;
  MqttWill will;
};

std::string EncodeConnect(const MqttConnectOptions& options);

// `packet_id` is ignored for QoS 0, which has no place to put one.
std::string EncodePublish(uint16_t packet_id,
                          std::string_view topic,
                          std::string_view payload,
                          uint8_t qos,
                          bool retain,
                          bool dup = false);

std::string EncodeSubscribe(uint16_t packet_id,
                            std::string_view topic,
                            uint8_t qos);
std::string EncodeUnsubscribe(uint16_t packet_id, std::string_view topic);

// PUBACK, PUBREC, PUBREL or PUBCOMP.
std::string EncodePacketIdAck(MqttPacketType type, uint16_t packet_id);

// Wraps an already-built body in a fixed header, which is the inverse of
// TakePacket. A retransmit needs it: the DUP flag is not part of the body, so
// re-sending a PUBLISH that was built once means re-framing that body rather
// than re-encoding the message.
std::string EncodePacket(const MqttFixedHeader& header, std::string_view body);

std::string EncodePingreq();
std::string EncodeDisconnect();

// --- Primitives -----------------------------------------------------------

// Variable-length integer (section 2.2.3): seven bits of the value per byte,
// least significant group first, with the top bit set on every byte but the
// last. Returns an empty string for a value the four-byte encoding cannot hold
// (268435455), which no legal packet reaches.
std::string EncodeRemainingLength(size_t value);

// Returns nullopt if the buffer is too short, or if the encoding runs past four
// bytes - which is malformed rather than merely incomplete, and is why this is
// separate from the truncation TakePacket puts up with.
std::optional<size_t> DecodeRemainingLength(std::string_view buffer,
                                            size_t& consumed);

// A UTF-8 string on the wire is a two-byte big-endian length followed by that
// many bytes (section 1.5.3).
void AppendString(std::string& out, std::string_view value);

// Reads a length-prefixed string at `offset`, advancing it past the string.
std::optional<std::string_view> ReadString(std::string_view buffer,
                                           size_t& offset);

}  // namespace esp_modem_link::protocol
