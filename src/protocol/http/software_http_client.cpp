#include "protocol/http/software_http_client.h"

#include <algorithm>
#include <cctype>
#include <climits>

#include "at_parser/at_parser.h"
#include "protocol/http/http_request_builder.h"
#include "protocol/http/http_redirect.h"

namespace esp_modem_link::protocol {

namespace {

// The methods a request can be repeated for without asking the caller. These
// are the safe ones - defined to have no side effect - rather than every
// idempotent one: PUT and DELETE would be idempotent, but they still write, and
// the caller did not ask for that write to happen twice.
bool MethodIsRepeatable(std::string_view method) {
  return method == "GET" || method == "HEAD" || method == "OPTIONS";
}

std::string ToLower(std::string_view s) {
  std::string out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

// One chunk of a chunked request body: the size in hex, the payload, and the
// CRLF that ends the chunk (RFC 9112 section 7.1). Built as one buffer because
// each Send() is one packet to the transport, and under an AT module each one
// costs its own round of AT+MIPSEND.
std::string ChunkFrame(const void* buffer, size_t size) {
  std::string frame;
  frame.reserve(size + 24);
  frame += at_parser::ToHexString(size);
  frame += "\r\n";
  frame.append(static_cast<const char*>(buffer), size);
  frame += "\r\n";
  return frame;
}

}  // namespace

SoftwareHttpClient::SoftwareHttpClient(HttpTransportFactory factory)
    : factory_(std::move(factory)) {}

SoftwareHttpClient::~SoftwareHttpClient() {
  // The transport holds callbacks that capture this, so it has to go before the
  // members they touch.
  DropTransport();
}

void SoftwareHttpClient::SetTimeout(std::chrono::milliseconds timeout) {
  timeout_ = timeout;
}

void SoftwareHttpClient::SetHeader(std::string_view key,
                                   std::string_view value) {
  headers_.emplace_back(std::string(key), std::string(value));
}

void SoftwareHttpClient::SetBody(std::string body) {
  request_body_ = std::move(body);
}

void SoftwareHttpClient::SetKeepAlive(bool enable) { keep_alive_ = enable; }

void SoftwareHttpClient::SetFollowRedirects(bool enable) {
  follow_redirects_ = enable;
}

void SoftwareHttpClient::SetMaxRedirects(int max) {
  // A negative limit is no limit at all, which is not something a client may
  // offer a server: the chain has to end somewhere.
  max_redirects_ = max < 0 ? 0 : max;
}

void SoftwareHttpClient::SetChunkedUpload(bool enable) {
  // Only the request is recorded here. What decides how a Write() frames its
  // bytes is upload_chunked_, which Open() latches from this - see the note on
  // the members for why the two are not the same flag.
  //
  // Locked because Open() and Write() read it under mutex_: under the documented
  // one-driver-thread contract a setter racing them is a caller error, but a
  // bool read while it is being written is a torn read if it ever happens, and
  // SetTlsConfig locks for the same reason.
  std::lock_guard<std::mutex> lock(mutex_);
  chunked_upload_ = enable;
}

void SoftwareHttpClient::SetTlsConfig(const TlsConfig& config) {
  // Under the same lock EnsureConnected reads it with. A TlsConfig is four
  // strings, so a setter running against an in-flight request is a torn read
  // rather than a stale one - and the WebSocket and MQTT clients lock here for
  // the same reason.
  std::lock_guard<std::mutex> lock(mutex_);
  tls_ = config;
}

void SoftwareHttpClient::DropTransport() {
  if (transport_) {
    transport_->OnData(nullptr);
    transport_->OnDisconnected(nullptr);
    transport_->OnError(nullptr);
    transport_->Disconnect();
    transport_.reset();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  connected_ = false;
  // A dropped socket ends any upload that was running on it: the body is not
  // going anywhere, and a latch left armed would refuse every later request on a
  // client whose upload is already over. The caller would have to know to call
  // Close() after a send that failed, which is not something a failure should
  // require of them.
  upload_active_ = false;
  upload_chunked_ = false;
}

Result<> SoftwareHttpClient::EnsureConnected(const ParsedUrl& url) {
  if (connected_ && transport_ && keep_alive_ &&
      url.host == connected_host_ && url.port == connected_port_ &&
      url.tls == connected_tls_) {
    return {};
  }

  DropTransport();

  // Copied under the lock so the config handed to the factory cannot be a value
  // a SetTlsConfig is halfway through writing.
  TlsConfig tls_config;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tls_config = tls_;
  }

  // The held config describes the TLS connections this client makes, so it is
  // not passed on for a plaintext one. A socket that will not handshake has no
  // use for verification settings, and an honest HAL refuses certificate
  // material handed to one rather than let it come up unauthenticated - so
  // forwarding it here would turn a config set for a client's https:// requests
  // into a failure on the next http:// request, which is not what the caller
  // said by setting it.
  auto created = factory_(url.tls, url.tls ? tls_config : TlsConfig{});
  if (!created) {
    return std::unexpected(created.error());
  }
  transport_ = std::move(*created);

  // Registered before connecting so nothing the module sends during the open
  // handshake can be missed.
  transport_->OnData(
      [this](std::string_view data) { OnTransportData(data); });
  transport_->OnDisconnected([this] { OnTransportClosed(); });
  transport_->OnError([this](const NetworkError& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    parse_failed_ = true;
    parse_error_ = error;
    cv_.notify_all();
  });

  auto connect = transport_->Connect(url.host, url.port);
  if (!connect) {
    DropTransport();
    return std::unexpected(connect.error());
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    connected_host_ = url.host;
    connected_port_ = url.port;
    connected_tls_ = url.tls;
  }
  return {};
}

void SoftwareHttpClient::OnTransportData(std::string_view data) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (parser_.IsComplete()) return;

  auto result = parser_.Feed(data);
  if (!result) {
    parse_failed_ = true;
    parse_error_ = result.error();
  }
  cv_.notify_all();
}

void SoftwareHttpClient::OnTransportClosed() {
  std::lock_guard<std::mutex> lock(mutex_);
  closed_ = true;
  // A response with neither Content-Length nor chunked encoding is only known
  // to be finished when the peer hangs up.
  parser_.Finish();
  cv_.notify_all();
}

void SoftwareHttpClient::HandleBody(std::string_view data) {
  if (streaming_) {
    pending_.insert(pending_.end(), data.begin(), data.end());
  } else {
    response_.body.append(data);
  }
}

NetworkError SoftwareHttpClient::FailedError() const {
  // parse_failed_ and parse_error_ are set together, so the fallback is only a
  // guard against a future path that sets one without the other.
  return parse_error_.value_or(
      NetworkError(NetworkErrc::kProtocolError, 0, "HTTP request failed"));
}

Result<> SoftwareHttpClient::StartRequest(const RequestState& request,
                                          bool chunked_upload) {
  // A request cannot start on a socket that is in the middle of a chunked body.
  // With keep-alive the transport is reused, so a second head would be rendered
  // straight into the open body - where the server reads it as the next
  // chunk-size line - and the caller would be told the request succeeded. With
  // keep-alive off the socket would merely be dropped first, but the upload
  // would still be forgotten with no word about it. Checked before anything is
  // opened, so a refusal leaves no connection behind.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (upload_active_) {
      return std::unexpected(NetworkError(
          NetworkErrc::kInvalidArgument, 0,
          "a chunked request body is still open; call EndBody() to finish it or "
          "Close() to abandon it before starting another request"));
    }
  }

  if (auto connected = EnsureConnected(request.url); !connected) {
    return std::unexpected(connected.error());
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    parser_.Reset();
    parser_.OnBody([this](std::string_view data) { HandleBody(data); });
    parse_failed_ = false;
    parse_error_.reset();
    closed_ = false;
    response_ = HttpResponse{};
    if (streaming_) pending_.clear();
    // An upload that reached this point has already ended - an open one is
    // refused above - so what is left is the latch of a finished upload, which
    // must not survive into the next request: a Write() framed by a stale latch
    // would put chunk bytes behind a head that declared a length instead.
    // BeginChunkedUpload arms these again after this returns.
    upload_chunked_ = false;
    upload_active_ = false;
  }

  HttpRequestOptions options;
  options.method = request.method;
  options.headers = request.headers;
  options.body = request.body;
  options.keep_alive = keep_alive_;
  // Passed in rather than read off the member: Execute() reaches the socket
  // through here too, and it has read its body already. A chunked request head
  // with no Write() and no EndBody() behind it would leave the server waiting
  // for a body that never comes.
  options.chunked_upload = chunked_upload;

  const std::string rendered = BuildHttpRequest(request.url, options);

  auto sent = transport_->Send(rendered.data(), rendered.size());
  if (!sent) {
    DropTransport();
    return std::unexpected(sent.error());
  }
  return {};
}

Result<HttpResponse> SoftwareHttpClient::Execute(std::string_view method,
                                                 std::string_view url) {
  auto parsed = ParseUrl(url);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }

  RequestState request;
  request.method = std::string(method);
  request.url = *parsed;
  request.body = request_body_;
  request.headers = headers_;

  for (int hop = 0;; ++hop) {
    auto result = ExecuteAttempt(request);
    if (!result) {
      return result;
    }
    auto followed = ApplyRedirect(*result, hop, request);
    if (!followed) {
      return std::unexpected(followed.error());
    }
    // Nothing left to follow: the response the server gave is the answer.
    if (!*followed) {
      return result;
    }
  }
}

Result<HttpResponse> SoftwareHttpClient::ExecuteAttempt(
    const RequestState& request) {
  // A connection that ends with the body unfinished is a transport failure the
  // server may never have seen, so one repeat is worth trying - but only for a
  // request with no side effect. A POST cut short may well have been acted on
  // already, and repeating it could apply that action twice. PUT and DELETE are
  // idempotent by definition but still write, so they are not repeated either.
  const bool repeatable = MethodIsRepeatable(request.method);
  for (int attempt = 0;; ++attempt) {
    auto result = ExecuteOnce(request);
    if (result || attempt > 0 || !repeatable ||
        result.error().code != NetworkErrc::kConnectionLost) {
      return result;
    }
    // ExecuteOnce drops the transport on this path, so the repeat opens a fresh
    // connection rather than reusing the one that failed.
  }
}

Result<bool> SoftwareHttpClient::ApplyRedirect(const HttpResponse& response,
                                               int hop,
                                               RequestState& request) {
  if (!IsRedirectStatus(response.status_code)) {
    return false;
  }

  // Not following leaves the response as the server sent it, Location and all,
  // which is what a caller that turned this off asked for. A limit of zero is
  // the same answer as turning it off.
  if (!follow_redirects_ || max_redirects_ == 0) {
    return false;
  }

  // A redirect that names nowhere to go is the server's final answer, and the
  // status and body that came with it are handed back rather than followed.
  const std::string location = RedirectLocation(response);
  if (location.empty()) {
    return false;
  }

  // Checked before the hop is taken, so the limit bounds how many requests go
  // out, and counted from zero for the first request: one initial request plus
  // max_redirects redirects is the most that is ever sent.
  if (hop >= max_redirects_) {
    return std::unexpected(NetworkError(
        NetworkErrc::kProtocolError, response.status_code,
        "the request was redirected more than " + std::to_string(max_redirects_) +
            " times"));
  }

  auto next = ResolveRedirect(request.url, location);
  if (!next) {
    return std::unexpected(next.error());
  }

  // A redirect that crosses origins does not take the credentials with it: an
  // Authorization header belongs to the host it was written for, and a Location
  // naming somebody else's server would hand that server the key. Everything
  // else is the request the caller wrote, which still applies wherever it goes.
  if (!SameOrigin(request.url, *next)) {
    request.headers.erase(
        std::remove_if(request.headers.begin(), request.headers.end(),
                       [](const auto& header) {
                         return IsCredentialHeader(header.first);
                       }),
        request.headers.end());
  }

  // 303, and 301/302 as clients have always read them, turn anything that was
  // not a plain read into a GET: the server is saying the answer is elsewhere,
  // and the body that was sent is not wanted there. 307 and 308 are the ones
  // that mean "send exactly this again, somewhere else". Content-Length is
  // derived from the body on the way out, so a dropped body drops it too.
  if (response.status_code == 303 ||
      ((response.status_code == 301 || response.status_code == 302) &&
       request.method != "GET" && request.method != "HEAD")) {
    request.method = "GET";
    request.body.clear();
  }

  // A streaming caller has not read the body of the response being abandoned, so
  // that connection cannot carry the next request: those bytes would arrive
  // ahead of its answer. Execute() has read the body and may keep the socket for
  // the next hop, which is what makes a chain of redirects one connection.
  if (streaming_) {
    DropTransport();
  }

  request.url = *next;
  return true;
}

Result<HttpResponse> SoftwareHttpClient::ExecuteOnce(
    const RequestState& request) {
  streaming_ = false;
  if (auto started = StartRequest(request, false); !started) {
    return std::unexpected(started.error());
  }

  {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, timeout_, [this] {
      return parser_.IsComplete() || parse_failed_ || closed_;
    });

    if (parse_failed_) {
      NetworkError error = FailedError();
      lock.unlock();
      DropTransport();
      return std::unexpected(error);
    }
    // A response that announced a length but delivered less is truncated. It
    // must not be handed back as though it were whole.
    if (!parser_.IsComplete()) {
      const bool timed_out = !closed_;
      lock.unlock();
      DropTransport();
      return std::unexpected(NetworkError(
          timed_out ? NetworkErrc::kTimeout : NetworkErrc::kConnectionLost, 0,
          timed_out ? "timed out waiting for the HTTP response"
                    : "connection closed before the response was complete"));
    }

    response_.status_code = parser_.GetStatusCode();
    for (const auto& [key, value] : parser_.GetHeaders()) {
      response_.headers[key] = value;
    }
  }

  // Written to a local before the transport goes away, since the returned
  // response must outlive it.
  HttpResponse result = std::move(response_);
  response_ = HttpResponse{};

  if (!keep_alive_) {
    DropTransport();
  }
  return result;
}

Result<> SoftwareHttpClient::Open(std::string_view method,
                                  std::string_view url) {
  auto parsed = ParseUrl(url);
  if (!parsed) {
    return std::unexpected(parsed.error());
  }

  RequestState request;
  request.method = std::string(method);
  request.url = *parsed;
  request.body = request_body_;
  request.headers = headers_;

  streaming_ = true;

  if (chunked_upload_) {
    if (auto begun = BeginChunkedUpload(request); !begun) {
      // Nothing was started, so nothing may be left claiming that it was: a
      // client with no request open must not look like one mid-stream.
      streaming_ = false;
      return std::unexpected(begun.error());
    }
    // No AwaitHeaders() here, and no redirect loop around it: the body has not
    // been sent, so there is nothing for the server to answer yet, and a
    // response that does arrive is EndBody()'s to collect.
    return {};
  }

  for (int hop = 0;; ++hop) {
    if (auto started = StartRequest(request, false); !started) {
      return std::unexpected(started.error());
    }
    auto head = AwaitHeaders();
    if (!head) {
      return std::unexpected(head.error());
    }
    auto followed = ApplyRedirect(*head, hop, request);
    if (!followed) {
      return std::unexpected(followed.error());
    }
    // The headers of the final response are the ones in the parser, which is
    // what GetStatusCode() and GetResponseHeader() read from here on.
    if (!*followed) {
      return {};
    }
  }
}

Result<> SoftwareHttpClient::BeginChunkedUpload(const RequestState& request) {
  // A body already measured is the thing chunked upload exists to replace, and
  // the two cannot both be right: StartRequest would render a request head from
  // one while Write() appended framed bytes from the other.
  if (!request.body.empty()) {
    return std::unexpected(NetworkError(
        NetworkErrc::kInvalidArgument, 0,
        "a body was set with SetBody() and chunked upload is on; a chunked "
        "body is written with Write(), not set in advance. SetBody(\"\") "
        "clears the one that is held"));
  }

  // Expect: 100-continue asks the server for permission before the body is
  // sent, and nothing here waits for the interim response. Worse, "HTTP/1.1 100
  // Continue" carries neither a length nor a Transfer-Encoding, so the parser
  // reads it as a body that ends when the peer hangs up - and the real response
  // head is then inside that body. Rejected rather than ignored.
  for (const auto& [key, value] : request.headers) {
    (void)value;
    if (ToLower(key) == "expect") {
      return std::unexpected(NetworkError(
          NetworkErrc::kInvalidArgument, 0,
          "Expect: 100-continue cannot be used with a chunked upload; the "
          "interim response is not handled"));
    }
  }

  if (auto started = StartRequest(request, true); !started) {
    return std::unexpected(started.error());
  }

  std::lock_guard<std::mutex> lock(mutex_);
  upload_chunked_ = true;
  upload_active_ = true;
  return {};
}

Result<HttpResponse> SoftwareHttpClient::AwaitHeaders() {
  // Returns once the headers are in, so the status and headers can be read
  // before any of the body has been consumed.
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, timeout_, [this] {
    // The parentheses are what the operators already did - headers are done
    // when the parser has left both the status line and the header block - and
    // saying so keeps -Werror=parentheses quiet, which is on in the ESP-IDF
    // build and would otherwise stop the library from compiling for the target.
    return (parser_.GetState() != HttpParser::State::kStatusLine &&
            parser_.GetState() != HttpParser::State::kHeaders) ||
           parse_failed_ || closed_;
  });

  if (parse_failed_) {
    return std::unexpected(FailedError());
  }
  if (parser_.GetState() == HttpParser::State::kError) {
    return std::unexpected(NetworkError(NetworkErrc::kProtocolError, 0,
                                        "malformed HTTP response"));
  }
  if (parser_.GetState() == HttpParser::State::kStatusLine ||
      parser_.GetState() == HttpParser::State::kHeaders) {
    // A peer that hung up before answering is not a timeout, and a caller acts
    // on the difference: one is worth waiting out, the other never will be. The
    // wait above returns for either, so the state has to be read here to tell
    // them apart - which is what ExecuteOnce() does with the same two codes.
    if (closed_) {
      return std::unexpected(
          NetworkError(NetworkErrc::kConnectionLost, 0,
                       "connection closed before the HTTP response headers "
                       "arrived"));
    }
    return std::unexpected(
        NetworkError(NetworkErrc::kTimeout, 0,
                     "timed out waiting for the HTTP response headers"));
  }

  // The head is copied out here because a redirect is decided on the status and
  // the Location, and for a response whose body has not been read those exist
  // only in the parser, which the next attempt resets.
  HttpResponse head;
  head.status_code = parser_.GetStatusCode();
  head.headers = parser_.GetHeaders();
  return head;
}

Result<int> SoftwareHttpClient::Read(void* buffer, size_t size) {
  std::unique_lock<std::mutex> lock(mutex_);

  cv_.wait_for(lock, timeout_, [this] {
    return !pending_.empty() || parse_failed_ || closed_ || parser_.IsComplete();
  });

  if (pending_.empty()) {
    if (parse_failed_) return std::unexpected(FailedError());
    // What has already arrived is served first - the check above only runs once
    // the buffer is drained - so a close cannot hide bytes that were delivered.
    if (parser_.IsComplete()) return 0;  // end of body

    // The peer went away without finishing. Reporting that as a clean end of
    // body would hand the caller a short read as though it were the whole
    // response, which is the same defect Execute() refuses to make. Only a
    // close-delimited response is completed by the peer hanging up, and the
    // parser has already said so above.
    if (closed_) {
      return std::unexpected(NetworkError(
          NetworkErrc::kConnectionLost, 0,
          "connection closed before the response was complete"));
    }

    return std::unexpected(
        NetworkError(NetworkErrc::kTimeout, 0, "timed out reading HTTP body"));
  }

  size_t count = std::min(size, pending_.size());
  char* out = static_cast<char*>(buffer);
  for (size_t i = 0; i < count; ++i) {
    out[i] = pending_.front();
    pending_.pop_front();
  }
  return static_cast<int>(count);
}

Result<int> SoftwareHttpClient::Write(const void* buffer, size_t size) {
  TcpClient* transport = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // The latched mode is what governs, not the caller's flag: an upload that
    // was opened while chunking was on is still framed as chunks even if the
    // flag has been turned off since, and refusing to write into it would make
    // the body unfinishable - there would be no way to send the terminator.
    // Three distinct states, each with its own answer, because the caller's
    // mistake is different in each: they never opted in, they have not opened
    // the request yet, or they have already finished it.
    if (!upload_chunked_) {
      if (!chunked_upload_) {
        return std::unexpected(NetworkError::NotSupported(
            "streaming a request body requires SetChunkedUpload(true) before "
            "Open(); a body that is known up front belongs in SetBody()"));
      }
      return std::unexpected(NetworkError(
          NetworkErrc::kNotConnected, 0,
          "no chunked request is open; call Open() before Write()"));
    }
    if (!upload_active_) {
      return std::unexpected(NetworkError(
          NetworkErrc::kInvalidArgument, 0,
          "the chunked request body is no longer being written - it was ended by "
          "EndBody(), or the server answered before it was finished"));
    }
    // A peer that has hung up is known before the write is attempted, and a
    // chunk reported as sent into a closed socket is worse than a refusal: the
    // caller would go on building a body nobody is reading.
    if (closed_) {
      return std::unexpected(NetworkError(
          NetworkErrc::kNotConnected, 0,
          "the connection was closed while the chunked body was being written"));
    }
    // A server may answer before the body is finished - a 302, a 401, a 413 -
    // and one that has done so stops reading. Writing further either fills a
    // socket nobody is draining or, as measured against www.baidu.com, writes
    // into one the server has already closed: the module answers AT+MIPSEND
    // with CME 550, and the real answer - a 302 already sitting in the parser -
    // is lost behind a transport error that names nothing useful. Stopping here
    // keeps it, and the caller can read the response that stopped them.
    if (parser_.GetState() != HttpParser::State::kStatusLine) {
      const int status = parser_.GetStatusCode();
      // The upload is over: no more of the body will be written, and the
      // response is already here, so EndBody() has nothing left to do either.
      // Left armed, the latch would refuse every later request on this client
      // until the caller happened to call Close().
      upload_active_ = false;
      return std::unexpected(NetworkError(
          NetworkErrc::kProtocolError, status,
          "the server answered with status " + std::to_string(status) +
              " before the request body was finished, so no more was sent; the "
              "response is readable"));
    }
    // A zero-length chunk is the terminator itself, so this is the one size
    // that must never reach the wire from here: it would end the body early and
    // EndBody() would then be sending a terminator into the response. Reported
    // as the no-op it is - a caller looping over a buffer may legitimately hand
    // over an empty piece - and nothing is sent.
    if (size == 0) return 0;
    if (buffer == nullptr) {
      return std::unexpected(NetworkError(NetworkErrc::kInvalidArgument, 0,
                                          "Write() was given no buffer"));
    }
    // The chunk size has to be reported back as an int, and so does the framing
    // of it; a piece that cannot be counted is refused before anything goes out
    // rather than after.
    if (size > static_cast<size_t>(INT_MAX)) {
      return std::unexpected(NetworkError(
          NetworkErrc::kInvalidArgument, 0,
          "a chunk larger than INT_MAX bytes cannot be reported as a length"));
    }
    transport = transport_.get();
  }

  // Taken under the lock and used without it: this class never calls into the
  // transport while holding mutex_, because a synchronous Disconnect() from a
  // callback would re-enter it. One thread drives Open/Write/EndBody/Close and
  // another only Reads, so the pointer cannot be dropped between here and the
  // Send - but that is the contract, not something the lock enforces.
  if (transport == nullptr) {
    return std::unexpected(NetworkError(
        NetworkErrc::kNotConnected, 0,
        "the connection was dropped while the chunked body was being written"));
  }

  const std::string frame = ChunkFrame(buffer, size);
  auto sent = transport->Send(frame.data(), frame.size());
  if (!sent) {
    DropTransport();
    return std::unexpected(sent.error());
  }
  // A short write leaves the size line claiming more bytes than the socket
  // carries, and no amount of further writing can put the stream back in step.
  // The connection is finished either way.
  if (static_cast<size_t>(*sent) != frame.size()) {
    DropTransport();
    return std::unexpected(NetworkError(
        NetworkErrc::kTransmitFailed, 0,
        "the transport accepted only part of a chunk, so the framing no longer "
        "matches the bytes on the wire"));
  }
  return static_cast<int>(size);
}

Result<> SoftwareHttpClient::EndBody() {
  TcpClient* transport = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // The same two gates Write() uses, in the same order and with the same
    // answers, so a caller who gets it wrong is told the same thing whichever
    // call they got it wrong in.
    if (!upload_chunked_) {
      if (!chunked_upload_) {
        return std::unexpected(NetworkError::NotSupported(
            "EndBody() ends a chunked request body, and none was asked for; "
            "SetChunkedUpload(true) before Open()"));
      }
      return std::unexpected(NetworkError(
          NetworkErrc::kNotConnected, 0,
          "no chunked request is open; call Open() before EndBody()"));
    }
    if (!upload_active_) {
      return std::unexpected(NetworkError(
          NetworkErrc::kInvalidArgument, 0,
          "the chunked request body is no longer being written - it was ended by "
          "EndBody(), or the server answered before it was finished"));
    }
    // Cleared before the terminator goes out, not after: if the send fails the
    // upload is finished either way, and a second EndBody() must not put
    // another zero-length chunk on the wire.
    upload_active_ = false;
    transport = transport_.get();
  }
  if (transport == nullptr) {
    return std::unexpected(NetworkError(
        NetworkErrc::kNotConnected, 0,
        "the connection was dropped before the request body was ended"));
  }

  static constexpr std::string_view kTerminator = "0\r\n\r\n";
  auto sent = transport->Send(kTerminator.data(), kTerminator.size());
  if (!sent) {
    DropTransport();
    return std::unexpected(sent.error());
  }
  if (static_cast<size_t>(*sent) != kTerminator.size()) {
    DropTransport();
    return std::unexpected(NetworkError(
        NetworkErrc::kTransmitFailed, 0,
        "the request body terminator could not be sent whole"));
  }

  auto head = AwaitHeaders();
  if (!head) {
    // The terminator is on the wire, so the request is complete and its response
    // is in flight. Whatever went wrong here - a timeout reading it, or a peer
    // that hung up - leaves the socket holding the head of a response this
    // caller never read. Handing it to the next request would answer that one
    // with this one's status, so the socket goes with the failure.
    DropTransport();
    return std::unexpected(head.error());
  }

  // A redirect asks for the request to be sent again somewhere else, and the
  // body that would have to go with it has already been streamed - there is
  // nothing left to send. Handing the 3xx back as though following were
  // satisfied would be a silent lie, so it is reported; the status and the
  // Location are in the parser, so GetStatusCode() and GetResponseHeader() read
  // where the server pointed. The response body cannot be read afterwards: the
  // abandoned body is what makes the connection unusable for the next request.
  if (follow_redirects_ && IsRedirectStatus(head->status_code)) {
    const int status = head->status_code;
    DropTransport();
    return std::unexpected(NetworkError(
        NetworkErrc::kProtocolError, status,
        "the server answered a streamed request body with a redirect, and a "
        "body that has already gone out cannot be sent again; read the status "
        "and the Location, or turn following off to treat it as an answer"));
  }
  return {};
}

void SoftwareHttpClient::Close() {
  // DropTransport also clears any upload latch: an abandoned upload leaves a
  // request head on the wire with no body behind it, so the next Open() must not
  // believe it is still in progress.
  DropTransport();
  std::lock_guard<std::mutex> lock(mutex_);
  pending_.clear();
  streaming_ = false;
}

Result<int> SoftwareHttpClient::GetStatusCode() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (parser_.GetState() == HttpParser::State::kStatusLine) {
    return std::unexpected(
        NetworkError(NetworkErrc::kNotConnected, 0, "no response started"));
  }
  return parser_.GetStatusCode();
}

std::string SoftwareHttpClient::GetResponseHeader(std::string_view key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return std::string(parser_.GetHeader(key));
}

size_t SoftwareHttpClient::GetContentLength() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return parser_.GetContentLength();
}

bool SoftwareHttpClient::IsChunked() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return parser_.IsChunked();
}

}  // namespace esp_modem_link::protocol
