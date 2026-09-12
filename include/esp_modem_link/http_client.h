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

  virtual Result<HttpResponse> Execute(std::string_view method,
                                       std::string_view url) = 0;

  virtual Result<> Open(std::string_view method, std::string_view url) = 0;
  virtual Result<int> Read(void* buffer, size_t size) = 0;
  virtual Result<int> Write(const void* buffer, size_t size) = 0;
  virtual void Close() = 0;

  virtual Result<int> GetStatusCode() = 0;
  virtual std::string GetResponseHeader(std::string_view key) const = 0;
  virtual size_t GetContentLength() const = 0;
};

}  // namespace esp_modem_link
