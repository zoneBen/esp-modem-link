#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "esp_modem_link/http_client.h"
#include "esp_modem_link/tcp_client.h"
#include "protocol/http/http_parser.h"
#include "protocol/http/http_redirect.h"
#include "protocol/http/http_request_builder.h"

namespace esp_modem_link::protocol {

// HTTP/1.1 over any TcpClient, so every module gets the same protocol behaviour
// regardless of what its firmware does or does not implement natively.
//
// The transport is created per request rather than injected once, because
// whether a request needs TLS is only known after the URL is parsed. The TLS
// settings go to the factory for the same reason: a module that manages TLS in
// firmware ignores them, while a transport stack that terminates TLS itself
// needs them.
using HttpTransportFactory = std::function<Result<std::unique_ptr<TcpClient>>(
    bool tls,
    const TlsConfig& config)>;

class SoftwareHttpClient : public HttpClient {
 public:
  explicit SoftwareHttpClient(HttpTransportFactory factory);
  ~SoftwareHttpClient() override;

  void SetTimeout(std::chrono::milliseconds timeout) override;
  void SetHeader(std::string_view key, std::string_view value) override;
  void SetBody(std::string body) override;
  void SetKeepAlive(bool enable) override;
  void SetTlsConfig(const TlsConfig& config) override;
  void SetFollowRedirects(bool enable) override;
  void SetMaxRedirects(int max) override;

  // Sends the request and buffers the entire response. Returns once the body is
  // complete, so the result owns its data.
  Result<HttpResponse> Execute(std::string_view method,
                               std::string_view url) override;

  // Streams instead of buffering. Open() returns once the response headers have
  // arrived, so GetStatusCode() and GetResponseHeader() are valid from then on,
  // and Read() then drains the body as it is received.
  Result<> Open(std::string_view method, std::string_view url) override;
  Result<int> Read(void* buffer, size_t size) override;
  Result<int> Write(const void* buffer, size_t size) override;
  void Close() override;

  Result<int> GetStatusCode() override;
  std::string GetResponseHeader(std::string_view key) const override;
  size_t GetContentLength() const override;

 private:
  // The request as it changes down a redirect chain: the method and body a 303
  // rewrites, the headers a cross-origin hop prunes of its credentials, and the
  // URL the next request goes to. It starts as a copy of what the caller
  // configured, so nothing a redirect decides can be seen by the caller's
  // setters.
  struct RequestState {
    std::string method;
    ParsedUrl url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
  };

  // Opens a transport for `url`, reusing the existing one when keep-alive is on
  // and it already points at the same host, port and security.
  Result<> EnsureConnected(const ParsedUrl& url);
  void DropTransport();

  Result<> StartRequest(const RequestState& request);

  // Waits until the response head has arrived and hands back its status and
  // headers.
  Result<HttpResponse> AwaitHeaders();

  // One attempt at the request. Execute() owns the decision to try again; this
  // is the whole of the work for a single try.
  Result<HttpResponse> ExecuteOnce(const RequestState& request);

  // ExecuteOnce plus the single repeat a request with no side effect is allowed
  // when the connection dies under it.
  Result<HttpResponse> ExecuteAttempt(const RequestState& request);

  // Applies one redirect to `request`, leaving it ready for the next hop. False
  // when the response is not a redirect to follow, which leaves it as the
  // caller's final answer.
  Result<bool> ApplyRedirect(const HttpResponse& response,
                             int hop,
                             RequestState& request);

  // Transport callbacks, invoked from the AT layer's receive thread.
  void OnTransportData(std::string_view data);
  void OnTransportClosed();

  void HandleBody(std::string_view data);

  // What stopped the request in flight. Only meaningful while parse_failed_ is
  // set, which is the only time it is non-empty.
  NetworkError FailedError() const;

  std::unique_ptr<TcpClient> transport_;
  HttpTransportFactory factory_;

  std::chrono::milliseconds timeout_{10000};
  std::vector<std::pair<std::string, std::string>> headers_;
  std::string request_body_;
  bool keep_alive_ = false;
  bool follow_redirects_ = true;
  int max_redirects_ = kMaxRedirects;
  TlsConfig tls_;

  HttpParser parser_;

  // Guards everything the receive thread touches. Never held across a call
  // into the transport, which is what keeps a synchronous Disconnect() from
  // re-entering a locked mutex. Mutable so the const accessors can take it too.
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  bool parse_failed_ = false;
  std::optional<NetworkError> parse_error_;
  bool closed_ = false;

  // Streaming consumers drain this; Execute() leaves it empty and reads the
  // body off the parser instead.
  bool streaming_ = false;
  std::deque<char> pending_;

  HttpResponse response_;

  bool connected_ = false;
  std::string connected_host_;
  uint16_t connected_port_ = 0;
  bool connected_tls_ = false;
};

}  // namespace esp_modem_link::protocol
