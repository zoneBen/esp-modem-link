#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "esp_modem_link/network_error.h"

namespace esp_modem_link::protocol {

// Incremental HTTP/1.1 response parser.
//
// Bytes arrive from the socket in whatever sizes the transport happens to hand
// over, so every split point is legal: mid-status-line, between the CR and LF
// of a header terminator, in the middle of a chunk-size line. Feed() takes
// whatever arrived and keeps the unconsumed remainder internally, which is why
// it is safe to call with a single byte at a time.
class HttpParser {
 public:
  enum class State {
    kStatusLine,
    kHeaders,
    kBody,        // reading a Content-Length or close-delimited body
    kChunkSize,
    kChunkData,
    kChunkDataEnd,  // the CRLF that closes a chunk's data
    kChunkTrailer,
    kComplete,
    kError,
  };

  // Body bytes are handed to the callback as they arrive. Setting one also
  // turns off the internal accumulation, so a streaming consumer never holds a
  // whole response in memory; without one the body collects in GetBody().
  using BodyCallback = std::function<void(std::string_view data)>;

  HttpParser() = default;

  // Consumes as much of `data` as it can. Returns the number of bytes consumed,
  // so a caller feeding a larger buffer learns where the response ended. An
  // error means the stream is not valid HTTP; bytes past the consumed point are
  // untouched.
  Result<size_t> Feed(std::string_view data);

  // Signals that the peer closed the connection. This is what completes a
  // response that carried neither Content-Length nor chunked encoding, and it
  // is the only way such a body can be known to be finished.
  void Finish();

  State GetState() const { return state_; }
  bool IsComplete() const { return state_ == State::kComplete; }
  bool HasError() const { return state_ == State::kError; }

  int GetStatusCode() const { return status_code_; }
  int GetHttpMajor() const { return http_major_; }
  int GetHttpMinor() const { return http_minor_; }
  std::string_view GetReasonPhrase() const { return reason_; }

  // Header names are case-insensitive per RFC 9110, so lookup is too. Returns
  // an empty view when absent.
  std::string_view GetHeader(std::string_view name) const;
  const std::map<std::string, std::string>& GetHeaders() const {
    return headers_;
  }

  const std::string& GetBody() const { return body_; }
  // 0 when the response carried no Content-Length.
  size_t GetContentLength() const { return content_length_; }
  bool IsChunked() const { return chunked_; }

  void OnBody(BodyCallback cb) { on_body_ = std::move(cb); }

  // Returns the parser to its initial state. Header and body storage is
  // released, which is what makes one parser reusable across keep-alive
  // responses.
  void Reset();

 private:
  Result<size_t> FeedStatusLine(std::string_view data);
  Result<size_t> FeedHeaders(std::string_view data);
  Result<size_t> FeedBody(std::string_view data);
  Result<size_t> FeedChunkSize(std::string_view data);
  Result<size_t> FeedChunkData(std::string_view data);
  Result<size_t> FeedChunkDataEnd(std::string_view data);
  Result<size_t> FeedChunkTrailer(std::string_view data);

  // Appends body bytes to whichever sink is active.
  void EmitBody(std::string_view data);

  // Retains a partial line so the next Feed() can continue it.
  bool TakeLine(std::string_view data, std::string& out, size_t& consumed);

  State state_ = State::kStatusLine;
  std::string buffer_;

  int http_major_ = 0;
  int http_minor_ = 0;
  int status_code_ = 0;
  std::string reason_;

  std::map<std::string, std::string> headers_;
  std::string body_;
  BodyCallback on_body_;

  size_t content_length_ = 0;
  bool has_content_length_ = false;
  bool chunked_ = false;

  size_t body_remaining_ = 0;   // for kBody
  size_t chunk_remaining_ = 0;  // for kChunkData
};

}  // namespace esp_modem_link::protocol
