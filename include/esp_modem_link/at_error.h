#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "esp_modem_link/network_error.h"

namespace esp_modem_link {

enum class AtErrc {
  kTimeout,
  kCommandError,
  kCmeError,
  kCmsError,
  kTransmitFailed,
  kNotInitialized,
  kNotSupported,
};

struct AtError {
  AtErrc code;
  int cme = 0;
  int cms = 0;
  std::string context;

  AtError(AtErrc code, std::string ctx = "")
      : code(code), context(std::move(ctx)) {}

  AtError(AtErrc code, int cme_or_cms, std::string ctx = "")
      : code(code), cme(code == AtErrc::kCmeError ? cme_or_cms : 0),
        cms(code == AtErrc::kCmsError ? cme_or_cms : 0),
        context(std::move(ctx)) {}

  NetworkError ToNetworkError() const;
  std::string ToString() const;
};

using AtResult = std::expected<void, AtError>;

template <typename T>
using AtValue = std::expected<T, AtError>;

}  // namespace esp_modem_link
