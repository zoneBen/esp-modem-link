#include "protocol/http/software_http_client.h"

#include <algorithm>

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

void SoftwareHttpClient::SetTlsConfig(const TlsConfig& config) {
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
}

Result<> SoftwareHttpClient::EnsureConnected(const ParsedUrl& url) {
  if (connected_ && transport_ && keep_alive_ &&
      url.host == connected_host_ && url.port == connected_port_ &&
      url.tls == connected_tls_) {
    return {};
  }

  DropTransport();

  auto created = factory_(url.tls, tls_);
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

Result<> SoftwareHttpClient::StartRequest(const RequestState& request) {
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
  }

  HttpRequestOptions options;
  options.method = request.method;
  options.headers = request.headers;
  options.body = request.body;
  options.keep_alive = keep_alive_;

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
  if (auto started = StartRequest(request); !started) {
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
  for (int hop = 0;; ++hop) {
    if (auto started = StartRequest(request); !started) {
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

Result<HttpResponse> SoftwareHttpClient::AwaitHeaders() {
  // Returns once the headers are in, so the status and headers can be read
  // before any of the body has been consumed.
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, timeout_, [this] {
    return parser_.GetState() != HttpParser::State::kStatusLine &&
           parser_.GetState() != HttpParser::State::kHeaders ||
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
  (void)buffer;
  (void)size;
  // The request head, including its Content-Length, is already on the wire by
  // the time Write() could be called, so appending to the body afterwards would
  // contradict it. A body belongs in SetBody() before Open().
  return std::unexpected(NetworkError::NotSupported(
      "streaming a request body requires chunked upload, which is not "
      "implemented; set the body before opening the request"));
}

void SoftwareHttpClient::Close() {
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
