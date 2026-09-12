#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_modem_link/network_error.h"
#include "protocol/http/http_request_builder.h"

namespace esp_modem_link::protocol {

// The value a server must echo in Sec-WebSocket-Accept: base64 of the SHA-1 of
// the client's key followed by the GUID RFC 6455 fixes (4.2.2, item 5.4).
//
// SHA-1 is here because the specification names it, not because it is fit for
// anything else - the key is a nonce and the digest is a handshake echo, so
// nothing rests on the hash's collision resistance.
std::string ComputeSecWebSocketAccept(std::string_view key);

// A key to offer: base64 of sixteen random bytes (4.1, item 7). The randomness
// is not load-bearing for the handshake, which any well-formed value would
// satisfy, but a byte-repeating key is the shape a proxy that caches by request
// would mis-handle.
std::string MakeSecWebSocketKey();

// What a server answered the upgrade with.
struct WsHandshakeResponse {
  int status = 0;
  std::string accept;
};

// Reads a response head - everything through the blank line that ends the
// headers, which the caller has already located - and reports the status and
// the accept value.
//
// This is deliberately not the place that decides whether the handshake
// succeeded: a 101 whose accept does not match the key is a different fault
// from a 404, and only the caller has the key to tell them apart.
Result<WsHandshakeResponse> ParseHandshakeResponse(std::string_view head);

// Renders the upgrade request, ready to hand straight to a socket.
//
// A caller header that collides with one of the generated defaults replaces it
// rather than appearing twice, the same rule the HTTP request builder follows.
// The defaults are the five the specification requires plus Host; `key` is
// written into Sec-WebSocket-Key, so a caller that set that header is taken at
// its word and must be checked against that same value.
std::string BuildHandshakeRequest(
    const ParsedUrl& url,
    std::string_view key,
    const std::vector<std::pair<std::string, std::string>>& headers);

}  // namespace esp_modem_link::protocol
