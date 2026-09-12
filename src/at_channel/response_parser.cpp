#include "response_parser.h"

#include <algorithm>
#include <cctype>

namespace esp_modem_link::at_channel {

namespace {

bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() &&
         s.substr(0, prefix.size()) == prefix;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

// Returns true if the line is a final status line (OK, ERROR, +CME ERROR, etc.)
bool IsStatusLine(std::string_view line, AtError& out_error) {
  std::string_view trimmed = Trim(line);

  if (trimmed == "OK") {
    out_error = AtError(AtErrc::kCommandError);  // reset
    return true;
  }
  if (trimmed == "ERROR") {
    out_error = AtError(AtErrc::kCommandError, "");
    return true;
  }
  if (StartsWith(trimmed, "+CME ERROR:")) {
    std::string_view code_str = Trim(trimmed.substr(11));
    int code = 0;
    // Try to parse numeric CME error code
    for (char c : code_str) {
      if (c >= '0' && c <= '9') {
        code = code * 10 + (c - '0');
      } else {
        break;
      }
    }
    out_error = AtError(AtErrc::kCmeError, code, std::string(code_str));
    return true;
  }
  if (StartsWith(trimmed, "+CMS ERROR:")) {
    std::string_view code_str = Trim(trimmed.substr(11));
    int code = 0;
    for (char c : code_str) {
      if (c >= '0' && c <= '9') {
        code = code * 10 + (c - '0');
      } else {
        break;
      }
    }
    out_error = AtError(AtErrc::kCmsError, code, std::string(code_str));
    return true;
  }
  if (trimmed == ">") {
    // Data prompt - treat as success (waiting for data input)
    out_error = AtError(AtErrc::kCommandError);
    return true;
  }
  return false;
}

}  // namespace

ParsedAtResponse ParseResponse(std::string_view raw) {
  ParsedAtResponse result;

  size_t pos = 0;
  bool first_line = true;

  while (pos < raw.size()) {
    size_t line_end = raw.find("\r\n", pos);
    std::string_view line;
    if (line_end == std::string_view::npos) {
      line = Trim(raw.substr(pos));
      pos = raw.size();
    } else {
      line = Trim(raw.substr(pos, line_end - pos));
      pos = line_end + 2;
    }

    if (line.empty()) continue;

    if (first_line) {
      first_line = false;
      if (StartsWith(line, "AT") || StartsWith(line, "at")) {
        continue;
      }
    }

    AtError err(AtErrc::kCommandError);
    if (IsStatusLine(line, err)) {
      result.ok = (StartsWith(line, "OK") || StartsWith(line, ">"));
      if (!result.ok) {
        result.error = err;
      }
      break;
    }

    result.lines.emplace_back(line);
  }

  return result;
}

}  // namespace esp_modem_link::at_channel
