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
  virtual bool IsChunked() const = 0;
};

}  // namespace esp_modem_link
