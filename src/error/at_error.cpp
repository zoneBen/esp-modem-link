#include "esp_modem_link/at_error.h"

namespace esp_modem_link {

NetworkError AtError::ToNetworkError() const {
  switch (code) {
    case AtErrc::kTimeout:
      return NetworkError(NetworkErrc::kAtTimeout, 0, context);
    case AtErrc::kCommandError:
      return NetworkError(NetworkErrc::kAtCommandError, 0, context);
    case AtErrc::kCmeError:
      return NetworkError(NetworkErrc::kAtCmeError, cme, context);
    case AtErrc::kCmsError:
      return NetworkError(NetworkErrc::kAtCmsError, cms, context);
    case AtErrc::kTransmitFailed:
      return NetworkError(NetworkErrc::kTransmitFailed, 0, context);
    case AtErrc::kNotInitialized:
      return NetworkError(NetworkErrc::kNotInitialized, 0, context);
    case AtErrc::kNotSupported:
      return NetworkError(NetworkErrc::kNotSupported, 0, context);
  }
  return NetworkError(NetworkErrc::kUnknown, 0, context);
}

std::string AtError::ToString() const {
  std::string result;
  switch (code) {
    case AtErrc::kTimeout: result = "AT_TIMEOUT"; break;
    case AtErrc::kCommandError: result = "AT_ERROR"; break;
    case AtErrc::kCmeError:
      result = "CME_ERROR(" + std::to_string(cme) + ")";
      break;
    case AtErrc::kCmsError:
      result = "CMS_ERROR(" + std::to_string(cms) + ")";
      break;
    case AtErrc::kTransmitFailed: result = "AT_TRANSMIT_FAILED"; break;
    case AtErrc::kNotInitialized: result = "AT_NOT_INITIALIZED"; break;
    case AtErrc::kNotSupported: result = "AT_NOT_SUPPORTED"; break;
  }
  if (!context.empty()) {
    result += " [";
    result += context;
    result += "]";
  }
  return result;
}

}  // namespace esp_modem_link
