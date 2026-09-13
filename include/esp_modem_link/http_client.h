#pragma once

#include <chrono>
#include <map>
#include <string>
#include <string_view>

#include "esp_modem_link/config_types.h"
#include "esp_modem_link/network_error.h"

namespace esp_modem_link {

struct HttpResponse {
  int status_code = 0;
  std::map<std::string, std::string> headers;
  std::string body;
};

class HttpClient {
 public:
  virtual ~HttpClient() = default;

  virtual void SetTimeout(std::chrono::milliseconds timeout) = 0;
  virtual void SetHeader(std::string_view key, std::string_view value) = 0;
  virtual void SetBody(std::string body) = 0;
  virtual void SetKeepAlive(bool enable) = 0;
  virtual void SetTlsConfig(const TlsConfig& config) = 0;

  // Whether a 3xx is followed to the Location it names, and how many hops may
  // be taken before the chain is treated as endless. Following is on by default
  // with a small limit; a caller that wants to read the redirect itself - the
  // status and its Location are in the response - can turn it off.
  //
  // Not every engine can honour this. One that leaves HTTP to the module's
  // firmware has whatever redirect behaviour that firmware has, and a caller
  // setting these on it is asking for something it does not do.
  virtual void SetFollowRedirects(bool enable) { (void)enable; }
  virtual void SetMaxRedirects(int max) { (void)max; }

  // Sends the request body in pieces as Write() supplies it, rather than
  // measuring it first and declaring the total in Content-Length. Off by
  // default.
  //
  // Set before Open(). With it on, Open() returns as soon as the request head
  // has gone out, because a server does not answer a request whose body it has
  // not finished reading - waiting for the head there would wait out the
  // timeout on every upload. Write() then sends each piece as one chunk and
  // EndBody() ends the body, which is also what waits for the response head.
  // So everything after EndBody() is exactly what it is after a plain Open():
  // GetStatusCode() and GetResponseHeader() are valid, and Read() drains the
  // body.
  //
  // Two consequences worth knowing before reaching for this. A redirect cannot
  // be followed, because the body that would have to be sent again has already
  // been streamed, so the response is reported rather than chased. And a
  // HTTP/1.0 server does not understand the framing at all; it will usually
  // answer 4xx, or say nothing until EndBody() times out.
  //
  // Not every engine can honour this. One that leaves HTTP to the module's
  // firmware has whatever upload behaviour that firmware has, and its Write()
  // reports NotSupported rather than sending a request whose declared length
  // disagrees with what follows it.
  virtual void SetChunkedUpload(bool enable) { (void)enable; }

  // Ends a chunked request body: sends the terminating zero-length chunk, so
  // the server knows the request is complete, and then waits for the response
  // head. Without it a Close() abandons the request, which the server sees as
  // a truncated body.
  //
  // Reports NotSupported when SetChunkedUpload(true) has not been set, which is
  // not a no-op on purpose: a default that succeeded would report an upload
  // that never happened as complete.
  virtual Result<> EndBody() {
    return std::unexpected(NetworkError::NotSupported(
        "EndBody requires SetChunkedUpload(true) before Open()"));
  }

  virtual Result<HttpResponse> Execute(std::string_view method,
                                       std::string_view url) = 0;

  virtual Result<> Open(std::string_view method, std::string_view url) = 0;
  virtual Result<int> Read(void* buffer, size_t size) = 0;
  virtual Result<int> Write(const void* buffer, size_t size) = 0;
  virtual void Close() = 0;

  virtual Result<int> GetStatusCode() = 0;
  virtual std::string GetResponseHeader(std::string_view key) const = 0;
  virtual size_t GetContentLength() const = 0;

  // Whether the body is chunked, which is what tells a caller reading it
  // through Read() that GetContentLength() is not the answer: a chunked
  // response carries no Content-Length, so that returns 0 however much body
  // follows. Read() decodes the chunk framing either way - this is for a caller
  // that wants to know how the body is framed, not for one that wants it
  // decoded.
  //
  // This is about the response. It says nothing about how the request body went
  // out, which is SetChunkedUpload()'s business and is not readable back.
  virtual bool IsChunked() const = 0;
};

}  // namespace esp_modem_link
