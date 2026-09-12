#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace esp_modem_link {

enum class NetworkErrc {
  // Generic
  kInvalidArgument,
  kNotInitialized,
  kNotSupported,
  kTimeout,
  kCanceled,
  kResourceBusy,
  kNoResources,

  // DNS
  kDnsFailed,

  // Connection
  kAlreadyConnected,
  kNotConnected,
  kConnectFailed,
  kConnectionRefused,
  kConnectionReset,
  kConnectionLost,

  // TLS/SSL
  kTlsHandshakeFailed,
  kTlsCertificateInvalid,
  kTlsCertificateExpired,
  kTlsHostnameMismatch,

  // Authentication
  kAuthRejected,
  kAuthTimeout,

  // Data transfer
  kTransmitFailed,
  kReceiveFailed,
  kBufferOverflow,

  // Network state
  kNetworkUnavailable,
  kNetworkDetached,
  kRoamingNotAllowed,

  // Protocol
  kProtocolError,
  kHttpErrorStatus,
  kMqttNotAuthorized,
  kWsHandshakeFailed,

  // AT command
  kAtCommandError,
  kAtCmeError,
  kAtCmsError,
  kAtTimeout,

  // Modem
  kModemNotResponding,
  kSimNotDetected,
  kSimPinRequired,
  kSimPukRequired,

  // Other
  kUnknown,
};

class NetworkError {
 public:
  NetworkErrc code;
  int native;
  std::string context;

  NetworkError(NetworkErrc code, int native, std::string context)
      : code(code), native(native), context(std::move(context)) {}

  NetworkError(NetworkErrc code, int native) : code(code), native(native) {}

  explicit NetworkError(NetworkErrc code) : code(code), native(0) {}

  const char* Name() const;
  const char* Message() const;
  std::string ToString() const;

  static NetworkError Timeout(std::string_view ctx = "");
  static NetworkError NotSupported(std::string_view ctx = "");
  static NetworkError DnsFailed(int herr, std::string_view host = "");
  static NetworkError FromErrno(int err);
};

template <typename T = void>
using Result = std::expected<T, NetworkError>;

}  // namespace esp_modem_link
