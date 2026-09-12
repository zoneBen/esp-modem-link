#include "protocol/http/http_redirect.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace esp_modem_link::protocol {

namespace {

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

// Removes the "." and ".." segments a Location is allowed to contain, so that a
// redirect to "/a/b/../c" is the same request as one to "/a/c" rather than a
// path the server has to interpret. The query is carried through untouched, and
// a path that named a directory keeps its trailing slash: "/a/b/" and "/a/b" are
// different resources, and a redirect to the former has to ask for it. A path
// ending at a "." or ".." segment named a directory too.
std::string NormalizePath(std::string_view path) {
  const size_t query_at = path.find('?');
  const std::string_view path_part = path.substr(0, query_at);
  const std::string_view query =
      query_at == std::string_view::npos ? std::string_view() : path.substr(query_at);

  std::vector<std::string_view> segments;
  size_t i = 0;
  while (i < path_part.size()) {
    size_t next = path_part.find('/', i);
    if (next == std::string_view::npos) next = path_part.size();
    const std::string_view segment = path_part.substr(i, next - i);
    if (segment == "..") {
      if (!segments.empty()) segments.pop_back();
    } else if (segment != "." && !segment.empty()) {
      segments.push_back(segment);
    }
    i = next + 1;
  }

  std::string out = "/";
  for (size_t s = 0; s < segments.size(); ++s) {
    if (s != 0) out += '/';
    out.append(segments[s]);
  }
  if (!path_part.empty() && out.size() > 1) {
    const bool directory =
        path_part.back() == '/' || path_part == "." || path_part == ".." ||
        path_part.ends_with("/.") || path_part.ends_with("/..");
    if (directory) out += '/';
  }
  out.append(query);
  return out;
}

// The path a relative Location resolves to. A Location that begins with '/' is
// absolute and replaces the whole path; anything else is relative to the
// directory the request was for.
std::string ResolvePath(const ParsedUrl& from, std::string_view location) {
  if (!location.empty() && location.front() == '?') {
    // A query-only Location replaces the query and leaves the path alone.
    const size_t base_query = from.path.find('?');
    return from.path.substr(0, base_query == std::string::npos
                                   ? from.path.size()
                                   : base_query) +
           std::string(location);
  }
  if (!location.empty() && location.front() == '/') {
    return std::string(location);
  }
  // Everything after the last '/' of the base path, which for "/a" is the root
  // and for "/a/b?c" is "/a/". The view is taken from the string rather than
  // from a string returned by substr(), which would be a temporary and gone by
  // the time it is used.
  const std::string_view path(from.path);
  const size_t base_query = path.find('?');
  const std::string_view base_path =
      path.substr(0, base_query == std::string_view::npos ? path.size()
                                                          : base_query);
  const size_t last_slash = base_path.rfind('/');
  std::string resolved(base_path.substr(
      0, last_slash == std::string_view::npos ? 0 : last_slash + 1));
  resolved.append(location);
  return resolved;
}

}  // namespace

bool IsRedirectStatus(int status) {
  switch (status) {
    case 301:
    case 302:
    case 303:
    case 307:
    case 308:
      return true;
    default:
      return false;
  }
}

std::string RedirectLocation(const HttpResponse& response) {
  // The response holds its headers in a plain map, which has no notion of the
  // case-insensitivity HTTP field names are defined with, so the lookup is done
  // here rather than by the container.
  for (const auto& [key, value] : response.headers) {
    if (EqualsIgnoreCase(key, "Location")) return value;
  }
  return {};
}

Result<ParsedUrl> ResolveRedirect(const ParsedUrl& from,
                                 std::string_view location) {
  // A fragment is the client's business and never goes on the wire, so it is
  // dropped before anything else is decided.
  const size_t hash = location.find('#');
  if (hash != std::string_view::npos) location = location.substr(0, hash);
  if (location.empty()) {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "the redirect has an empty Location"));
  }

  std::string resolved;
  if (location.rfind("http://", 0) == 0 || location.rfind("https://", 0) == 0) {
    resolved = std::string(location);
  } else if (location.rfind("//", 0) == 0) {
    // A network-path reference: another authority, with the scheme kept.
    resolved = from.tls ? "https:" : "http:";
    resolved.append(location);
  } else {
    // A relative reference may not have a colon in its first segment: one there
    // means the Location names a scheme, and a scheme this client does not speak
    // - http and https having already been ruled out above - is not one it can
    // follow. Requesting "mailto:someone@example.com" as a path is not what the
    // server meant.
    const size_t first_segment = location.find_first_of("/?");
    if (location.substr(0, first_segment).find(':') != std::string_view::npos) {
      return std::unexpected(NetworkError(
          NetworkErrc::kProtocolError, 0,
          "the redirect names a scheme this client cannot follow: " +
              std::string(location)));
    }
    resolved = from.tls ? "https://" : "http://";
    resolved.append(from.host);
    // The port is only written when it is not the one the scheme implies, so
    // that the URL handed to ParseUrl stays one it reads back the same way.
    const uint16_t default_port = from.tls ? 443 : 80;
    if (from.port != default_port) {
      resolved += ':';
      resolved += std::to_string(from.port);
    }
    resolved.append(NormalizePath(ResolvePath(from, location)));
  }

  // Parsing the result rather than assembling the pieces by hand is what
  // rejects a Location this client cannot follow: a scheme it does not speak,
  // or an authority with no host in it.
  auto parsed = ParseUrl(resolved);
  if (!parsed) {
    return std::unexpected(NetworkError(
        NetworkErrc::kProtocolError, 0,
        "the redirect points somewhere this client cannot go: " + resolved));
  }
  return parsed;
}

bool SameOrigin(const ParsedUrl& a, const ParsedUrl& b) {
  return a.tls == b.tls && a.port == b.port && EqualsIgnoreCase(a.host, b.host);
}

bool IsCredentialHeader(std::string_view name) {
  const std::string lower = ToLower(name);
  return lower == "authorization" || lower == "cookie" ||
         lower == "proxy-authorization";
}

}  // namespace esp_modem_link::protocol
