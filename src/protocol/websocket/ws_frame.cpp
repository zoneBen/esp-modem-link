#include "protocol/websocket/ws_frame.h"

namespace esp_modem_link::protocol {
namespace {

// A second byte with this bit set on a server frame means the server masked it,
// which 5.1 forbids: masking exists to keep a client's bytes from being
// predictable to an intermediary, and a server has no intermediary to hide
// from.
constexpr uint8_t kMaskBit = 0x80;
constexpr uint8_t kFinBit = 0x80;
constexpr uint8_t kRsvBits = 0x70;
constexpr uint8_t kOpcodeMask = 0x0F;
// The payload length field holds the real length up to 125, and 126 and 127 are
// the two escape values that say the length follows in 2 or 8 more bytes (5.2).
constexpr uint8_t kLengthIs16Bit = 126;
constexpr uint8_t kLengthIs64Bit = 127;

std::optional<WsOpcode> OpcodeFromRaw(uint8_t raw) {
  switch (raw) {
    case 0x0:
      return WsOpcode::kContinuation;
    case 0x1:
      return WsOpcode::kText;
    case 0x2:
      return WsOpcode::kBinary;
    case 0x8:
      return WsOpcode::kClose;
    case 0x9:
      return WsOpcode::kPing;
    case 0xA:
      return WsOpcode::kPong;
    default:
      return std::nullopt;
  }
}

void AppendMasked(std::string& out, std::string_view payload, uint32_t key) {
  // The key travels ahead of the payload in network order, and the payload's
  // octet i is exclusive-ored with the key's octet i mod 4. Reading the key out
  // once, in the order it was written, is what keeps the two sides agreeing.
  const uint8_t mask[4] = {
      static_cast<uint8_t>((key >> 24) & 0xFF),
      static_cast<uint8_t>((key >> 16) & 0xFF),
      static_cast<uint8_t>((key >> 8) & 0xFF),
      static_cast<uint8_t>(key & 0xFF),
  };
  for (size_t i = 0; i < payload.size(); ++i) {
    out.push_back(static_cast<char>(static_cast<uint8_t>(payload[i]) ^
                                    mask[i % 4]));
  }
}

WsFrameRead Invalid(const char* reason) {
  WsFrameRead read;
  read.status = WsFrameStatus::kInvalid;
  read.reason = reason;
  return read;
}

}  // namespace

bool IsControlOpcode(WsOpcode opcode) {
  return opcode == WsOpcode::kClose || opcode == WsOpcode::kPing ||
         opcode == WsOpcode::kPong;
}

Result<std::string> EncodeFrame(WsOpcode opcode,
                                std::string_view payload,
                                bool fin,
                                std::optional<uint32_t> mask_key) {
  if (IsControlOpcode(opcode)) {
    if (!fin) {
      return std::unexpected(NetworkError(NetworkErrc::kInvalidArgument, 0,
                                          "a control frame cannot be fragmented"));
    }
    if (payload.size() > kMaxControlPayload) {
      return std::unexpected(
          NetworkError(NetworkErrc::kInvalidArgument, 0,
                       "a control frame cannot carry more than 125 bytes"));
    }
  }

  std::string out;
  out.reserve(payload.size() + 14);

  out.push_back(static_cast<char>(static_cast<uint8_t>(opcode) |
                                  (fin ? kFinBit : 0x00)));

  // Masking a frame is a client's obligation, so the bit tracks the key rather
  // than being a separate parameter that could disagree with it.
  const uint8_t mask_bit = mask_key.has_value() ? kMaskBit : 0x00;
  const uint64_t length = payload.size();
  if (length < kLengthIs16Bit) {
    out.push_back(static_cast<char>(mask_bit | static_cast<uint8_t>(length)));
  } else if (length <= 0xFFFF) {
    out.push_back(static_cast<char>(mask_bit | kLengthIs16Bit));
    out.push_back(static_cast<char>((length >> 8) & 0xFF));
    out.push_back(static_cast<char>(length & 0xFF));
  } else {
    out.push_back(static_cast<char>(mask_bit | kLengthIs64Bit));
    for (int shift = 56; shift >= 0; shift -= 8) {
      out.push_back(static_cast<char>((length >> shift) & 0xFF));
    }
  }

  if (mask_key.has_value()) {
    const uint32_t key = *mask_key;
    out.push_back(static_cast<char>((key >> 24) & 0xFF));
    out.push_back(static_cast<char>((key >> 16) & 0xFF));
    out.push_back(static_cast<char>((key >> 8) & 0xFF));
    out.push_back(static_cast<char>(key & 0xFF));
    AppendMasked(out, payload, key);
  } else {
    out.append(payload);
  }
  return out;
}

WsFrameRead TakeFrame(std::string& buffer) {
  // Two bytes is the smallest a frame can be, and holds everything the header
  // decides: the flag byte and the first seven bits of the length.
  if (buffer.size() < 2) return {};

  const auto* bytes = reinterpret_cast<const uint8_t*>(buffer.data());
  const uint8_t first = bytes[0];
  const uint8_t second = bytes[1];

  if ((first & kRsvBits) != 0) {
    // RSV bits are reserved for extensions, and this client negotiates none, so
    // it has no way to know what they mean.
    return Invalid("a reserved bit is set in the frame header");
  }

  const auto opcode = OpcodeFromRaw(first & kOpcodeMask);
  if (!opcode.has_value()) {
    return Invalid("the frame header carries a reserved opcode");
  }

  if ((second & kMaskBit) != 0) {
    return Invalid("a frame from the server is masked");
  }

  uint64_t length = second & 0x7F;
  size_t offset = 2;
  if (length == kLengthIs16Bit) {
    if (buffer.size() < offset + 2) return {};
    length = (static_cast<uint64_t>(bytes[2]) << 8) | bytes[3];
    offset = 4;
  } else if (length == kLengthIs64Bit) {
    if (buffer.size() < offset + 8) return {};
    length = 0;
    for (size_t i = 0; i < 8; ++i) {
      length = (length << 8) | bytes[2 + i];
    }
    // The leading bit of a 64-bit length must be zero (5.2). A length that
    // large is not a length, and on a 32-bit target the shift above would
    // already have discarded it.
    if ((length & 0x8000000000000000ULL) != 0) {
      return Invalid("the frame length has its leading bit set");
    }
    offset = 10;
  }

  if (IsControlOpcode(*opcode)) {
    // Checked on the encoded value, so a control frame that spelled 5 as two
    // bytes is caught by the same rule as one that spelled it in seven bits.
    if ((first & kFinBit) == 0) {
      return Invalid("a control frame arrived fragmented");
    }
    if (length > kMaxControlPayload) {
      return Invalid("a control frame carries more than 125 bytes");
    }
  }

  // Compared in 64 bits: on a target where size_t is narrower, no buffer can
  // ever be this long, and the frame stays incomplete rather than the addition
  // wrapping into a length that would index past the end.
  if (static_cast<uint64_t>(buffer.size()) <
      static_cast<uint64_t>(offset) + length) {
    return {};
  }

  WsFrameRead read;
  read.status = WsFrameStatus::kReady;
  read.frame.fin = (first & kFinBit) != 0;
  read.frame.opcode = *opcode;
  read.frame.payload.assign(buffer, offset, static_cast<size_t>(length));
  buffer.erase(0, offset + static_cast<size_t>(length));
  return read;
}

}  // namespace esp_modem_link::protocol
