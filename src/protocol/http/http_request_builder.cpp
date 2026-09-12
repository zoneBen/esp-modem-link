#include "protocol/http/http_request_builder.h"

#include <algorithm>
#include <cctype>

#include "at_parser/at_parser.h"

namespace esp_modem_link::protocol {

namespace {

constexpr uint16_t kDefaultHttpPort = 80;
constexpr uint16_t kDefaultHttpsPort = 443;

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

NetworkError UrlError(std::string_view context) {
  return NetworkError(NetworkErrc::kInvalidArgument, 0, std::string(context));
}

}  // namespace

Result<ParsedUrl> ParseUrl(std::string_view url) {
  ParsedUrl parsed;

  std::string_view rest;
  if (url.rfind("http://", 0) == 0) {
    parsed.tls = false;
    rest = url.substr(7);
  } else if (url.rfind("https://", 0) == 0) {
    parsed.tls = true;
    rest = url.substr(8);
  } else {
    return std::unexpected(
        UrlError("URL must start with http:// or https://"));
  }
  parsed.port = parsed.tls ? kDefaultHttpsPort : kDefaultHttpPort;

  // Authority runs to the first '/' or '?'; a URL with neither is all authority.
  size_t authority_end = rest.find_first_of("/?");
  std::string_view authority = rest.substr(0, authority_end);
  parsed.path = authority_end == std::string_view::npos
                    ? "/"
                    : std::string(rest.substr(authority_end));
  if (parsed.path.empty() || parsed.path[0] == '?') {
    parsed.path.insert(0, "/");
  }

  if (authority.empty()) {
    return std::unexpected(UrlError("URL has no host"));
  }
  // Userinfo is not used by this client and is discarded rather than sent as
  // part of the Host header, where it would be malformed.
  auto at = authority.find('@');
  if (at != std::string_view::npos) {
    authority = authority.substr(at + 1);
  }

  auto colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    parsed.host = std::string(authority);
  } else {
    parsed.host = std::string(authority.substr(0, colon));
    auto port = at_parser::ParseInt(authority.substr(colon + 1));
    if (!port || *port <= 0 || *port > 65535) {
      return std::unexpected(UrlError("URL has an invalid port"));
    }
    parsed.port = static_cast<uint16_t>(*port);
  }

  if (parsed.host.empty()) {
    return std::unexpected(UrlError("URL has no host"));
  }
  return parsed;
}

std::string BuildHttpRequest(const ParsedUrl& url,
                             const HttpRequestOptions& options) {
  // The Host header carries the port only when it is not the scheme default,
  // which is what servers expect and what virtual-host routing keys on.
  const bool default_port =
      (url.tls && url.port == kDefaultHttpsPort) ||
      (!url.tls && url.port == kDefaultHttpPort);
  std::string host_header = url.host;
  if (!default_port) {
    host_header += ':';
    host_header += std::to_string(url.port);
  }

  // Headers are assembled as a list rather than written straight out, because a
  // caller-supplied header has to be able to replace a generated one in place
  // instead of appearing twice.
  std::vector<std::pair<std::string, std::string>> headers = {
      {"Host", host_header},
      {"User-Agent", options.user_agent},
      {"Connection", options.keep_alive ? "keep-alive" : "close"},
  };

  auto set_header = [&headers](const std::string& key,
                               const std::string& value) {
    std::string lowered = ToLower(key);
    for (auto& [existing_key, existing_value] : headers) {
      if (ToLower(existing_key) == lowered) {
        existing_value = value;
        return;
      }
    }
    headers.emplace_back(key, value);
  };

  for (const auto& [key, value] : options.headers) {
    set_header(key, value);
  }
  // Derived from the body, so the two can never disagree.
  set_header("Content-Length", std::to_string(options.body.size()));

  std::string request;
  request += options.method;
  request += ' ';
  request += url.path.empty() ? "/" : url.path;
  request += " HTTP/1.1\r\n";
  for (const auto& [key, value] : headers) {
    request += key;
    request += ": ";
    request += value;
    request += "\r\n";
  }
  request += "\r\n";
  request += options.body;
  return request;
}

}  // namespace esp_modem_link::protocol
