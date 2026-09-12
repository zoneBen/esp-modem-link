#include "urc_dispatcher.h"

#include <algorithm>
#include <cctype>

namespace esp_modem_link::at_channel {

UrcDispatcher::UrcDispatcher() = default;

UrcDispatcher::UrcHandle UrcDispatcher::Subscribe(std::string_view prefix,
                                                   UrcHandler handler) {
  UrcHandle handle = next_handle_++;
  std::string prefix_str(prefix);
  handlers_[prefix_str].push_back(
      HandlerEntry{handle, prefix_str, std::move(handler)});
  return handle;
}

void UrcDispatcher::Unsubscribe(UrcHandle handle) {
  for (auto& [prefix, entries] : handlers_) {
    auto it = std::remove_if(
        entries.begin(), entries.end(),
        [handle](const HandlerEntry& e) { return e.handle == handle; });
    if (it != entries.end()) {
      entries.erase(it, entries.end());
      return;
    }
  }
}

static std::string_view Trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

void UrcDispatcher::Dispatch(std::string_view line) {
  line = Trim(line);
  if (line.empty()) return;

  std::string_view command = line;
  std::string_view arguments;

  auto colon_pos = line.find(':');
  if (colon_pos != std::string_view::npos) {
    command = line.substr(0, colon_pos);
    arguments = Trim(line.substr(colon_pos + 1));
  }

  // Try exact command match first, then prefix matches
  for (auto& [prefix, entries] : handlers_) {
    if (command.size() >= prefix.size() &&
        command.substr(0, prefix.size()) ==
            std::string_view(prefix)) {
      for (auto& entry : entries) {
        if (entry.handler) {
          entry.handler(command, arguments);
        }
      }
    }
  }
}

}  // namespace esp_modem_link::at_channel
