#include "protocol/websocket/ws_handshake.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <vector>

#include "platform/random.h"
#include "protocol/http/http_parser.h"

namespace esp_modem_link::protocol {
namespace {

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

// The GUID RFC 6455 appends to the client's key before hashing (4.2.2, item
// 5.4). It exists to make the digest prove the server read the key rather than
// echoing a value a caching proxy could have produced on its own.
constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr char kBase64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

uint32_t RotateLeft(uint32_t value, int bits) {
  return (value << bits) | (value >> (32 - bits));
}

std::string Base64Encode(std::string_view data) {
  std::string out;
  out.reserve(((data.size() + 2) / 3) * 4);

  size_t i = 0;
  while (i + 3 <= data.size()) {
    const uint32_t group = (static_cast<uint32_t>(
                                static_cast<uint8_t>(data[i]))
                            << 16) |
                           (static_cast<uint32_t>(
                                static_cast<uint8_t>(data[i + 1]))
                            << 8) |
                           static_cast<uint8_t>(data[i + 2]);
    for (int shift = 18; shift >= 0; shift -= 6) {
      out.push_back(kBase64Alphabet[(group >> shift) & 0x3F]);
    }
    i += 3;
  }

  // The tail is one or two bytes padded out to a whole group, with as many '='
  // as bytes were invented. Padding is not optional here: the accept value is
  // compared as a string, and a server pads.
  const size_t remaining = data.size() - i;
  if (remaining == 1) {
    const uint32_t group =
        static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << 16;
    out.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
    out.append("==");
  } else if (remaining == 2) {
    const uint32_t group =
        (static_cast<uint32_t>(static_cast<uint8_t>(data[i])) << 16) |
        (static_cast<uint32_t>(static_cast<uint8_t>(data[i + 1])) << 8);
    out.push_back(kBase64Alphabet[(group >> 18) & 0x3F]);
    out.push_back(kBase64Alphabet[(group >> 12) & 0x3F]);
    out.push_back(kBase64Alphabet[(group >> 6) & 0x3F]);
    out.push_back('=');
  }
  return out;
}

std::array<uint8_t, 20> Sha1(std::string_view data) {
  std::vector<uint8_t> message(data.begin(), data.end());
  // Padding: a one bit, then zeroes until the length is 56 mod 64, then the
  // original length in bits as a big-endian 64-bit field.
  const uint64_t bit_length = static_cast<uint64_t>(data.size()) * 8;
  message.push_back(0x80);
  while (message.size() % 64 != 56) message.push_back(0x00);
  for (int shift = 56; shift >= 0; shift -= 8) {
    message.push_back(static_cast<uint8_t>((bit_length >> shift) & 0xFF));
  }

  uint32_t digest[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476,
                        0xC3D2E1F0};

  for (size_t block = 0; block < message.size(); block += 64) {
    uint32_t words[80];
    for (size_t i = 0; i < 16; ++i) {
      const size_t at = block + i * 4;
      words[i] = (static_cast<uint32_t>(message[at]) << 24) |
                 (static_cast<uint32_t>(message[at + 1]) << 16) |
                 (static_cast<uint32_t>(message[at + 2]) << 8) |
                 static_cast<uint32_t>(message[at + 3]);
    }
    for (size_t i = 16; i < 80; ++i) {
      words[i] = RotateLeft(
          words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    }

    uint32_t a = digest[0];
    uint32_t b = digest[1];
    uint32_t c = digest[2];
    uint32_t d = digest[3];
    uint32_t e = digest[4];

    for (size_t i = 0; i < 80; ++i) {
      uint32_t f = 0;
      uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDC;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6;
      }
      const uint32_t temp = RotateLeft(a, 5) + f + e + k + words[i];
      e = d;
      d = c;
      c = RotateLeft(b, 30);
      b = a;
      a = temp;
    }

    digest[0] += a;
    digest[1] += b;
    digest[2] += c;
    digest[3] += d;
    digest[4] += e;
  }

  std::array<uint8_t, 20> out{};
  for (size_t i = 0; i < 5; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      out[i * 4 + j] = static_cast<uint8_t>((digest[i] >> (24 - 8 * j)) & 0xFF);
    }
  }
  return out;
}

}  // namespace

std::string ComputeSecWebSocketAccept(std::string_view key) {
  std::string material(key);
  material.append(kWebSocketGuid);
  const auto digest = Sha1(material);
  return Base64Encode(
      std::string_view(reinterpret_cast<const char*>(digest.data()),
                       digest.size()));
}

std::string MakeSecWebSocketKey() {
  std::string key;
  key.reserve(16);
  for (int i = 0; i < 4; ++i) {
    const uint32_t word = platform::RandomUint32();
    for (int shift = 24; shift >= 0; shift -= 8) {
      key.push_back(static_cast<char>((word >> shift) & 0xFF));
    }
  }
  return Base64Encode(key);
}

Result<WsHandshakeResponse> ParseHandshakeResponse(std::string_view head) {
  HttpParser parser;
  auto fed = parser.Feed(head);
  if (!fed.has_value()) {
    return std::unexpected(fed.error());
  }
  // The caller hands over a head that ends at the blank line, so the status
  // line and every header have been read by now. A parser still waiting for its
  // status line was given something that is not a head at all.
  if (parser.GetStatusCode() == 0) {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "the response has no status line"));
  }

  WsHandshakeResponse response;
  response.status = parser.GetStatusCode();
  response.accept = std::string(parser.GetHeader("Sec-WebSocket-Accept"));
  return response;
}

std::string BuildHandshakeRequest(
    const ParsedUrl& url,
    std::string_view key,
    const std::vector<std::pair<std::string, std::string>>& headers) {
  // The Host header carries the port only when it is not the scheme default,
  // which is the rule the HTTP request builder applies and what virtual-host
  // routing on the far end keys on.
  const bool default_port = (url.tls && url.port == 443) ||
                            (!url.tls && url.port == 80);
  std::string host_header = url.host;
  if (!default_port) {
    host_header += ':';
    host_header += std::to_string(url.port);
  }

  // Assembled as a list rather than written straight out, so a caller header
  // can replace a generated one in place instead of appearing twice.
  std::vector<std::pair<std::string, std::string>> assembled = {
      {"Host", host_header},
      {"Upgrade", "websocket"},
      {"Connection", "Upgrade"},
      {"Sec-WebSocket-Key", std::string(key)},
      {"Sec-WebSocket-Version", "13"},
  };
  for (const auto& [name, value] : headers) {
    const std::string lowered = ToLower(name);
    bool replaced = false;
    for (auto& [existing_name, existing_value] : assembled) {
      if (ToLower(existing_name) == lowered) {
        existing_value = value;
        replaced = true;
        break;
      }
    }
    if (!replaced) assembled.emplace_back(name, value);
  }

  std::string request = "GET ";
  request += url.path.empty() ? "/" : url.path;
  request += " HTTP/1.1\r\n";
  for (const auto& [name, value] : assembled) {
    request += name;
    request += ": ";
    request += value;
    request += "\r\n";
  }
  request += "\r\n";
  return request;
}

}  // namespace esp_modem_link::protocol
