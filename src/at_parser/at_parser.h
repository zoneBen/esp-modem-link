#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace esp_modem_link::at_parser {

std::vector<std::string_view> SplitCsv(std::string_view line);

std::string_view StripQuotes(std::string_view s);

// Strip the "+KEY:" prefix from a response line, returning just the value part.
// AT responses look like "+CSQ: 23,99" and the key must not be fed to field
// parsers, or every field shifts by one. A line with no colon is returned as it
// is, so a caller that is not sure whether it holds a keyed line can strip
// unconditionally.
std::string_view StripKeyPrefix(std::string_view line);

std::optional<int> ParseInt(std::string_view s);

std::optional<double> ParseDouble(std::string_view s);

bool ParseKeyValue(std::string_view line,
                   std::string_view& key,
                   std::string_view& value);

std::vector<uint8_t> ParseHex(std::string_view hex);

// Inverse of ParseHex: render binary data as lowercase hex, two digits per
// byte. Needed because ML307 carries payload inline in AT+MIPSEND rather than
// after a ">" prompt, so the bytes have to be text-safe before they go out.
std::string EncodeHex(std::string_view data);

std::vector<int> ParseIntList(std::string_view line);

}  // namespace esp_modem_link::at_parser
