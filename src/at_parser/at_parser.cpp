#include "at_parser.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

namespace esp_modem_link::at_parser {

std::vector<std::string_view> SplitCsv(std::string_view line) {
  std::vector<std::string_view> fields;
  size_t i = 0;
  while (i <= line.size()) {
    if (i < line.size() && line[i] == '"') {
      ++i;
      size_t start = i;
      while (i < line.size() && line[i] != '"') {
        ++i;
      }
      fields.emplace_back(line.substr(start, i - start));
      if (i < line.size()) ++i;
      if (i < line.size() && line[i] == ',') {
        ++i;
        continue;
      }
      break;
    }
    size_t start = i;
    while (i < line.size() && line[i] != ',') {
      ++i;
    }
    fields.emplace_back(line.substr(start, i - start));
    if (i >= line.size()) break;
    ++i;
  }
  return fields;
}

std::string_view StripQuotes(std::string_view s) {
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

std::string_view StripKeyPrefix(std::string_view line) {
  auto colon = line.find(':');
  if (colon == std::string_view::npos) return line;
  auto value = line.substr(colon + 1);
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1);
  }
  return value;
}

std::optional<int> ParseInt(std::string_view s) {
  if (s.empty()) return std::nullopt;
  int value = 0;
  auto result = std::from_chars(s.data(), s.data() + s.size(), value);
  if (result.ec == std::errc{} && result.ptr == s.data() + s.size()) {
    return value;
  }
  return std::nullopt;
}

std::optional<double> ParseDouble(std::string_view s) {
  if (s.empty()) return std::nullopt;
  double value = 0.0;
  auto result = std::from_chars(s.data(), s.data() + s.size(), value);
  if (result.ec == std::errc{} && result.ptr == s.data() + s.size()) {
    return value;
  }
  return std::nullopt;
}

bool ParseKeyValue(std::string_view line,
                   std::string_view& key,
                   std::string_view& value) {
  auto colon_pos = line.find(':');
  if (colon_pos == std::string_view::npos) return false;
  key = line.substr(0, colon_pos);
  value = line.substr(colon_pos + 1);
  while (!value.empty() && value.front() == ' ') {
    value.remove_prefix(1);
  }
  return !key.empty();
}

static int HexCharToNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

std::vector<uint8_t> ParseHex(std::string_view hex) {
  std::vector<uint8_t> result;
  result.reserve(hex.size() / 2);
  size_t i = 0;
  while (i + 1 < hex.size()) {
    int hi = HexCharToNibble(hex[i]);
    int lo = HexCharToNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      ++i;
      continue;
    }
    result.push_back(static_cast<uint8_t>((hi << 4) | lo));
    i += 2;
  }
  return result;
}

std::string EncodeHex(std::string_view data) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (char c : data) {
    auto byte = static_cast<unsigned char>(c);
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0x0f]);
  }
  return out;
}

std::string ToHexString(uint64_t value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  if (value == 0) return "0";
  std::string out;
  while (value > 0) {
    out.push_back(kDigits[value & 0x0f]);
    value >>= 4;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<int> ParseIntList(std::string_view line) {
  std::vector<int> result;
  auto fields = SplitCsv(line);
  for (auto field : fields) {
    auto val = ParseInt(field);
    if (val) result.push_back(*val);
  }
  return result;
}

}  // namespace esp_modem_link::at_parser
