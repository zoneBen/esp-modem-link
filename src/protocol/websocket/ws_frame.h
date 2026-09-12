#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "esp_modem_link/network_error.h"

namespace esp_modem_link::protocol {

// The six opcodes RFC 6455 defines. The reserved ones (0x3-0x7 and 0xB-0xF) are
// not represented: a frame carrying one is a protocol error rather than a frame
// of an unfamiliar kind, so there is nothing to name it.
enum class WsOpcode : uint8_t {
  kContinuation = 0x0,
  kText = 0x1,
  kBinary = 0x2,
  kClose = 0x8,
  kPing = 0x9,
  kPong = 0xA,
};

// Close, ping and pong carry connection control rather than application data,
// and they are the frames the specification constrains: neither may be
// fragmented and neither may carry more than 125 bytes (5.5).
bool IsControlOpcode(WsOpcode opcode);

// The most a control frame's payload may hold.
constexpr size_t kMaxControlPayload = 125;

struct WsFrame {
  bool fin = true;
  WsOpcode opcode = WsOpcode::kText;
  // Always the unmasked bytes. Masking is a property of how a frame travelled,
  // not of what it means, so no layer above this one sees a key.
  std::string payload;
};

// Encodes one frame, ready to hand to a socket.
//
// A masking key is required of a client frame and forbidden in a server frame
// (5.1), so whether one is supplied is how the caller says which of the two it
// is building. The key is a parameter rather than drawn internally to keep this
// a pure function of its arguments: a frame can then be asserted byte for byte
// against the examples in the specification.
//
// Refused: a control frame with fin clear, and a control frame past the 125
// byte limit. Both are frames a conforming sender must not emit, and an encoder
// that produced one would put the fault on the wire instead of at the call.
Result<std::string> EncodeFrame(
    WsOpcode opcode,
    std::string_view payload,
    bool fin = true,
    std::optional<uint32_t> mask_key = std::nullopt);

// What taking a frame off a buffer found.
enum class WsFrameStatus {
  // The buffer does not yet hold a whole frame. Not an error: a frame arrives
  // in as many pieces as the transport cares to hand it over in.
  kIncomplete,
  kReady,
  // Bytes no frame can be made of: a reserved opcode, a set RSV bit, a
  // fragmented or oversized control frame, a masked server frame, or a length
  // that is not a length. RFC 6455 requires the connection to be failed on
  // these rather than waited out, so they are kept apart from kIncomplete.
  kInvalid,
};

struct WsFrameRead {
  WsFrameStatus status = WsFrameStatus::kIncomplete;
  // Which rule was broken, when the status is kInvalid. A string literal, so it
  // stays valid for as long as the caller holds it.
  const char* reason = "";
  WsFrame frame;
};

// Takes one frame off the front of `buffer`, erasing exactly the bytes it
// consumed. Nothing is erased when the status is kInvalid: the caller is
// expected to fail the connection, and taking those bytes off again would only
// fail the same way.
WsFrameRead TakeFrame(std::string& buffer);

}  // namespace esp_modem_link::protocol
