#pragma once

#include <string>
#include <vector>

#include "esp_modem_link/at_error.h"

namespace esp_modem_link::at_channel {

struct ParsedAtResponse {
  bool ok = false;
  AtError error{AtErrc::kCommandError};
  std::vector<std::string> lines;
};

ParsedAtResponse ParseResponse(std::string_view raw_response);

}  // namespace esp_modem_link::at_channel
