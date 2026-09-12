#include "esp_modem_link/network_error.h"

#include <cerrno>
#include <cstring>
#include <string>

namespace esp_modem_link {

const char* NetworkError::Name() const {
  switch (code) {
    case NetworkErrc::kInvalidArgument: return "invalid_argument";
    case NetworkErrc::kNotInitialized: return "not_initialized";
    case NetworkErrc::kNotSupported: return "not_supported";
    case NetworkErrc::kTimeout: return "timeout";
    case NetworkErrc::kCanceled: return "canceled";
    case NetworkErrc::kResourceBusy: return "resource_busy";
    case NetworkErrc::kNoResources: return "no_resources";
    case NetworkErrc::kDnsFailed: return "dns_failed";
    case NetworkErrc::kAlreadyConnected: return "already_connected";
    case NetworkErrc::kNotConnected: return "not_connected";
    case NetworkErrc::kConnectFailed: return "connect_failed";
    case NetworkErrc::kConnectionRefused: return "connection_refused";
    case NetworkErrc::kConnectionReset: return "connection_reset";
    case NetworkErrc::kConnectionLost: return "connection_lost";
    case NetworkErrc::kTlsHandshakeFailed: return "tls_handshake_failed";
    case NetworkErrc::kTlsCertificateInvalid: return "tls_certificate_invalid";
    case NetworkErrc::kTlsCertificateExpired: return "tls_certificate_expired";
    case NetworkErrc::kTlsHostnameMismatch: return "tls_hostname_mismatch";
    case NetworkErrc::kAuthRejected: return "auth_rejected";
    case NetworkErrc::kAuthTimeout: return "auth_timeout";
    case NetworkErrc::kTransmitFailed: return "transmit_failed";
    case NetworkErrc::kReceiveFailed: return "receive_failed";
    case NetworkErrc::kBufferOverflow: return "buffer_overflow";
    case NetworkErrc::kNetworkUnavailable: return "network_unavailable";
    case NetworkErrc::kNetworkDetached: return "network_detached";
    case NetworkErrc::kRoamingNotAllowed: return "roaming_not_allowed";
    case NetworkErrc::kProtocolError: return "protocol_error";
    case NetworkErrc::kHttpErrorStatus: return "http_error_status";
    case NetworkErrc::kMqttNotAuthorized: return "mqtt_not_authorized";
    case NetworkErrc::kWsHandshakeFailed: return "ws_handshake_failed";
    case NetworkErrc::kAtCommandError: return "at_command_error";
    case NetworkErrc::kAtCmeError: return "at_cme_error";
    case NetworkErrc::kAtCmsError: return "at_cms_error";
    case NetworkErrc::kAtTimeout: return "at_timeout";
    case NetworkErrc::kModemNotResponding: return "modem_not_responding";
    case NetworkErrc::kSimNotDetected: return "sim_not_detected";
    case NetworkErrc::kSimPinRequired: return "sim_pin_required";
    case NetworkErrc::kSimPukRequired: return "sim_puk_required";
    case NetworkErrc::kUnknown: return "unknown";
  }
  return "unknown";
}

const char* NetworkError::Message() const {
  switch (code) {
    case NetworkErrc::kInvalidArgument: return "Invalid argument";
    case NetworkErrc::kNotInitialized: return "Not initialized";
    case NetworkErrc::kNotSupported: return "Not supported";
    case NetworkErrc::kTimeout: return "Operation timed out";
    case NetworkErrc::kCanceled: return "Operation canceled";
    case NetworkErrc::kResourceBusy: return "Resource busy";
    case NetworkErrc::kNoResources: return "No resources available";
    case NetworkErrc::kDnsFailed: return "DNS resolution failed";
    case NetworkErrc::kAlreadyConnected: return "Already connected";
    case NetworkErrc::kNotConnected: return "Not connected";
    case NetworkErrc::kConnectFailed: return "Connection failed";
    case NetworkErrc::kConnectionRefused: return "Connection refused";
    case NetworkErrc::kConnectionReset: return "Connection reset";
    case NetworkErrc::kConnectionLost: return "Connection lost";
    case NetworkErrc::kTlsHandshakeFailed: return "TLS handshake failed";
    case NetworkErrc::kTlsCertificateInvalid: return "TLS certificate invalid";
    case NetworkErrc::kTlsCertificateExpired: return "TLS certificate expired";
    case NetworkErrc::kTlsHostnameMismatch: return "TLS hostname mismatch";
    case NetworkErrc::kAuthRejected: return "Authentication rejected";
    case NetworkErrc::kAuthTimeout: return "Authentication timed out";
    case NetworkErrc::kTransmitFailed: return "Transmission failed";
    case NetworkErrc::kReceiveFailed: return "Reception failed";
    case NetworkErrc::kBufferOverflow: return "Buffer overflow";
    case NetworkErrc::kNetworkUnavailable: return "Network unavailable";
    case NetworkErrc::kNetworkDetached: return "Network detached";
    case NetworkErrc::kRoamingNotAllowed: return "Roaming not allowed";
    case NetworkErrc::kProtocolError: return "Protocol error";
    case NetworkErrc::kHttpErrorStatus: return "HTTP error status";
    case NetworkErrc::kMqttNotAuthorized: return "MQTT not authorized";
    case NetworkErrc::kWsHandshakeFailed: return "WebSocket handshake failed";
    case NetworkErrc::kAtCommandError: return "AT command error";
    case NetworkErrc::kAtCmeError: return "AT CME error";
    case NetworkErrc::kAtCmsError: return "AT CMS error";
    case NetworkErrc::kAtTimeout: return "AT command timeout";
    case NetworkErrc::kModemNotResponding: return "Modem not responding";
    case NetworkErrc::kSimNotDetected: return "SIM not detected";
    case NetworkErrc::kSimPinRequired: return "SIM PIN required";
    case NetworkErrc::kSimPukRequired: return "SIM PUK required";
    case NetworkErrc::kUnknown: return "Unknown error";
  }
  return "Unknown error";
}

std::string NetworkError::ToString() const {
  std::string result = Name();
  result += ": ";
  result += Message();
  if (native != 0) {
    result += " (native=";
    result += std::to_string(native);
    result += ")";
  }
  if (!context.empty()) {
    result += " [";
    result += context;
    result += "]";
  }
  return result;
}

NetworkError NetworkError::Timeout(std::string_view ctx) {
  return NetworkError(NetworkErrc::kTimeout, 0, std::string(ctx));
}

NetworkError NetworkError::NotSupported(std::string_view ctx) {
  return NetworkError(NetworkErrc::kNotSupported, 0, std::string(ctx));
}

NetworkError NetworkError::DnsFailed(int herr, std::string_view host) {
  NetworkError err(NetworkErrc::kDnsFailed, herr);
  if (!host.empty()) {
    err.context = "host=" + std::string(host);
  }
  return err;
}

NetworkError NetworkError::FromErrno(int err) {
  NetworkErrc code = NetworkErrc::kUnknown;
  switch (err) {
    case EINVAL: code = NetworkErrc::kInvalidArgument; break;
    case ETIMEDOUT: code = NetworkErrc::kTimeout; break;
    case ECONNREFUSED: code = NetworkErrc::kConnectionRefused; break;
    case ECONNRESET: code = NetworkErrc::kConnectionReset; break;
    case ENOBUFS:
    case ENOMEM: code = NetworkErrc::kResourceBusy; break;
    case ENOTCONN: code = NetworkErrc::kConnectFailed; break;
    case EPERM:
    case EACCES: code = NetworkErrc::kAuthRejected; break;
    default: break;
  }
  NetworkError result(code, err);
  result.context = std::strerror(err);
  return result;
}

}  // namespace esp_modem_link
