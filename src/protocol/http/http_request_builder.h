#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "esp_modem_link/network_error.h"

namespace esp_modem_link::protocol {

struct ParsedUrl {
  bool tls = false;      // https:// rather than http://
  std::string host;
  uint16_t port = 0;     // the scheme's default when the URL omits one
  std::string path;      // always begins with '/', includes any query
};

// Splits an absolute URL into the pieces a request line and a socket need.
// Only http and https are accepted: a scheme this client cannot speak is worth
// rejecting up front rather than sending somewhere unexpected.
Result<ParsedUrl> ParseUrl(std::string_view url);

struct HttpRequestOptions {
  std::string method = "GET";
  // Extra request headers, in the order they should be sent. A header that
  // collides with one of the generated defaults replaces it rather than
  // appearing twice.
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  bool keep_alive = false;
  std::string user_agent = "esp-modem-link/1.0";
};

// Renders a complete request, headers and body ready to hand straight to a
// socket. Content-Length is derived from the body, so a caller cannot send a
// length that disagrees with what follows it.
std::string BuildHttpRequest(const ParsedUrl& url,
                             const HttpRequestOptions& options);

}  // namespace esp_modem_link::protocol
