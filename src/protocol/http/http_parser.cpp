#include "protocol/http/http_parser.h"

#include <algorithm>
#include <cctype>

#include "at_parser/at_parser.h"

namespace esp_modem_link::protocol {

namespace {

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string_view Trim(std::string_view s) {
  size_t begin = 0;
  while (begin < s.size() &&
         (s[begin] == ' ' || s[begin] == '\t')) {
    ++begin;
  }
  size_t end = s.size();
  while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
    --end;
  }
  return s.substr(begin, end - begin);
}

}  // namespace

Result<size_t> HttpParser::Feed(std::string_view incoming) {
  if (state_ == State::kComplete || state_ == State::kError) {
    return 0;
  }

  // Anything banked from a previous call is data that arrived split across two
  // reads; put it in front of the new bytes so the line-oriented states below
  // see one contiguous stream.
  const size_t banked = buffer_.size();
  std::string_view data;
  if (banked == 0) {
    data = incoming;
  } else {
    buffer_.append(incoming);
    data = buffer_;
  }

  size_t consumed = 0;
  while (consumed < data.size() && state_ != State::kComplete &&
         state_ != State::kError) {
    std::string_view rest = data.substr(consumed);

    Result<size_t> step = 0;
    switch (state_) {
      case State::kStatusLine: step = FeedStatusLine(rest); break;
      case State::kHeaders: step = FeedHeaders(rest); break;
      case State::kBody: step = FeedBody(rest); break;
      case State::kChunkSize: step = FeedChunkSize(rest); break;
      case State::kChunkData: step = FeedChunkData(rest); break;
      case State::kChunkDataEnd: step = FeedChunkDataEnd(rest); break;
      case State::kChunkTrailer: step = FeedChunkTrailer(rest); break;
      case State::kComplete:
      case State::kError:
        break;
    }

    if (!step) {
      state_ = State::kError;
      buffer_.clear();
      return std::unexpected(step.error());
    }
    consumed += *step;
    if (*step == 0) break;  // the state needs more bytes than are available
  }

  if (state_ == State::kComplete || state_ == State::kError) {
    buffer_.clear();
  } else {
    // Copy before assigning: `data` may point into buffer_ itself.
    std::string remainder(data.substr(consumed));
    buffer_ = std::move(remainder);
  }

  // The caller counts in its own bytes, so discount what came out of the bank.
  return consumed > banked ? consumed - banked : 0;
}

// Pulls one CRLF-terminated line out of `data`. Returns false when the line is
// not yet complete, leaving `data` untouched.
bool HttpParser::TakeLine(std::string_view data,
                          std::string& out,
                          size_t& consumed) {
  auto pos = data.find("\r\n");
  if (pos == std::string_view::npos) {
    // A bare LF also terminates a line; servers that emit it are rare but the
    // tolerance costs nothing.
    pos = data.find('\n');
    if (pos == std::string_view::npos) return false;
    out = std::string(data.substr(0, pos));
    consumed = pos + 1;
    return true;
  }
  out = std::string(data.substr(0, pos));
  consumed = pos + 2;
  return true;
}

Result<size_t> HttpParser::FeedStatusLine(std::string_view data) {
  std::string line;
  size_t consumed = 0;
  if (!TakeLine(data, line, consumed)) return 0;

  // HTTP-version SP status-code SP reason-phrase
  if (line.rfind("HTTP/", 0) != 0) {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "malformed HTTP status line"));
  }
  std::string_view rest = std::string_view(line).substr(5);
  auto space = rest.find(' ');
  if (space == std::string_view::npos) {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "HTTP status line has no status code"));
  }
  std::string_view version = rest.substr(0, space);
  auto dot = version.find('.');
  if (dot == std::string_view::npos) {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "malformed HTTP version"));
  }
  auto major = at_parser::ParseInt(version.substr(0, dot));
  auto minor = at_parser::ParseInt(version.substr(dot + 1));
  if (!major || !minor) {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "malformed HTTP version"));
  }
  http_major_ = *major;
  http_minor_ = *minor;

  std::string_view remainder = Trim(rest.substr(space + 1));
  // The reason phrase is optional and may itself contain spaces.
  auto code_end = remainder.find(' ');
  std::string_view code_field =
      code_end == std::string_view::npos ? remainder
                                         : remainder.substr(0, code_end);
  auto code = at_parser::ParseInt(code_field);
  if (!code) {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "malformed HTTP status code"));
  }
  status_code_ = *code;
  reason_ = code_end == std::string_view::npos
                ? std::string()
                : std::string(Trim(remainder.substr(code_end + 1)));

  state_ = State::kHeaders;
  return consumed;
}

Result<size_t> HttpParser::FeedHeaders(std::string_view data) {
  std::string line;
  size_t consumed = 0;
  if (!TakeLine(data, line, consumed)) return 0;

  // A blank line ends the header block; the body starts at the very next byte,
  // with no intervening separator to skip.
  if (line.empty()) {
    if (chunked_) {
      state_ = State::kChunkSize;
    } else if (has_content_length_) {
      body_remaining_ = content_length_;
      if (body_remaining_ == 0) {
        state_ = State::kComplete;
      } else {
        state_ = State::kBody;
      }
    } else {
      // No length and no chunking: the body runs to the end of the connection.
      state_ = State::kBody;
      body_remaining_ = 0;  // unbounded, ended by Finish()
    }
    return consumed;
  }

  auto colon = line.find(':');
  if (colon == std::string::npos) {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "malformed HTTP header line"));
  }
  std::string name = ToLower(Trim(std::string_view(line).substr(0, colon)));
  std::string value(Trim(std::string_view(line).substr(colon + 1)));
  if (name.empty()) {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "empty HTTP header name"));
  }

  if (name == "content-length") {
    auto len = at_parser::ParseInt(value);
    if (!len) {
      return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                          "malformed Content-Length"));
    }
    content_length_ = static_cast<size_t>(*len);
    has_content_length_ = true;
  } else if (name == "transfer-encoding") {
    // Only chunked matters; a server that lists several codings still ends in
    // chunked when it streams, and "identity" means plain framing.
    if (ToLower(value).find("chunked") != std::string::npos) {
      chunked_ = true;
    }
  }

  headers_[name] = std::move(value);
  return consumed;
}

Result<size_t> HttpParser::FeedBody(std::string_view data) {
  if (body_remaining_ == 0 && !has_content_length_) {
    // Close-delimited: everything until the peer hangs up is body.
    EmitBody(data);
    return data.size();
  }

  size_t take = std::min(data.size(), body_remaining_);
  EmitBody(data.substr(0, take));
  body_remaining_ -= take;
  if (body_remaining_ == 0) {
    state_ = State::kComplete;
  }
  return take;
}

Result<size_t> HttpParser::FeedChunkSize(std::string_view data) {
  std::string line;
  size_t consumed = 0;
  if (!TakeLine(data, line, consumed)) return 0;

  // A chunk-size line may carry extensions after a semicolon: "1a;name=value".
  std::string_view size_field = Trim(line);
  auto semi = size_field.find(';');
  if (semi != std::string_view::npos) {
    size_field = Trim(size_field.substr(0, semi));
  }
  if (size_field.empty()) {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "empty chunk size"));
  }

  // Chunk sizes are hex, which ParseInt does not cover.
  size_t value = 0;
  for (char c : size_field) {
    int digit = -1;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10;
    } else {
      return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                          "malformed chunk size"));
    }
    value = value * 16 + static_cast<size_t>(digit);
  }

  if (value == 0) {
    state_ = State::kChunkTrailer;
  } else {
    chunk_remaining_ = value;
    state_ = State::kChunkData;
  }
  return consumed;
}

Result<size_t> HttpParser::FeedChunkData(std::string_view data) {
  size_t take = std::min(data.size(), chunk_remaining_);
  EmitBody(data.substr(0, take));
  chunk_remaining_ -= take;
  if (chunk_remaining_ == 0) {
    state_ = State::kChunkDataEnd;
  }
  return take;
}

Result<size_t> HttpParser::FeedChunkDataEnd(std::string_view data) {
  // Chunk data is always followed by CRLF before the next size line. Consuming
  // it here rather than in the size state keeps an empty line from being
  // mistaken for a (malformed) zero-length chunk header.
  if (!data.empty() && data[0] == '\n') {
    state_ = State::kChunkSize;
    return 1;
  }
  if (data.size() < 2) return 0;  // need the rest of the terminator
  if (data[0] != '\r' || data[1] != '\n') {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "chunk data not terminated by CRLF"));
  }
  state_ = State::kChunkSize;
  return 2;
}

Result<size_t> HttpParser::FeedChunkTrailer(std::string_view data) {
  // After the zero-size chunk comes a trailer section, terminated by a blank
  // line. Trailers are rare and nothing here consumes them, so they are read
  // and discarded rather than parsed into the header map.
  std::string line;
  size_t consumed = 0;
  if (!TakeLine(data, line, consumed)) return 0;
  if (line.empty()) {
    state_ = State::kComplete;
  }
  return consumed;
}

void HttpParser::EmitBody(std::string_view data) {
  if (data.empty()) return;
  if (on_body_) {
    on_body_(data);
  } else {
    body_.append(data);
  }
}

void HttpParser::Finish() {
  if (state_ == State::kComplete || state_ == State::kError) return;

  // Only a close-delimited body can be completed by the peer going away. A
  // response that announced a length but delivered less is truncated, and
  // saying so is better than presenting a short body as a whole one.
  if (state_ == State::kBody && !has_content_length_ && !chunked_) {
    state_ = State::kComplete;
  }
}

std::string_view HttpParser::GetHeader(std::string_view name) const {
  auto it = headers_.find(ToLower(Trim(name)));
  if (it == headers_.end()) return {};
  return it->second;
}

void HttpParser::Reset() {
  state_ = State::kStatusLine;
  buffer_.clear();
  http_major_ = 0;
  http_minor_ = 0;
  status_code_ = 0;
  reason_.clear();
  headers_.clear();
  body_.clear();
  content_length_ = 0;
  has_content_length_ = false;
  chunked_ = false;
  body_remaining_ = 0;
  chunk_remaining_ = 0;
}

}  // namespace esp_modem_link::protocol
