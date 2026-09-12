#pragma once

#include <string>
#include <string_view>

#include "esp_modem_link/http_client.h"
#include "protocol/http/http_request_builder.h"

namespace esp_modem_link::protocol {

// How many redirects one request may follow before the chain is treated as
// endless. Five is what browser clients settled on, and it is more than the
// http-to-https and missing-trailing-slash hops that real sites use.
constexpr int kMaxRedirects = 5;

// Whether a status tells the client to go somewhere else. 300 and 304 are
// deliberately not here: 300 is a choice for a human to make, and 304 is a cache
// answer that carries no Location to follow.
bool IsRedirectStatus(int status);

// The Location a response points at, matched case-insensitively as the field
// names in an HTTP message are. Empty when the response does not name one, which
// leaves the response as the server's final answer.
std::string RedirectLocation(const HttpResponse& response);

// The URL a Location value refers to, resolved against the URL the response came
// from. A relative Location is resolved against the path the request used and
// keeps the scheme, host and port already in use. Fails when the result is not a
// URL this client can send to.
Result<ParsedUrl> ResolveRedirect(const ParsedUrl& from,
                                  std::string_view location);

// Whether two URLs share an origin: the same scheme, host and port. Which origin
// a redirect crosses is what decides whether the credentials in a request may
// travel with it.
bool SameOrigin(const ParsedUrl& a, const ParsedUrl& b);

// Whether a header carries credentials that belong to one origin alone, so that
// they are not handed to whatever host a redirect names next.
bool IsCredentialHeader(std::string_view name);

}  // namespace esp_modem_link::protocol
