#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace esp_modem_link::at_channel {

class UrcDispatcher {
 public:
  using UrcHandler =
      std::function<void(std::string_view command,
                         std::string_view arguments)>;
  using UrcHandle = uint64_t;

  UrcDispatcher();

  UrcHandle Subscribe(std::string_view prefix, UrcHandler handler);
  void Unsubscribe(UrcHandle handle);

  void Dispatch(std::string_view line);

 private:
  struct HandlerEntry {
    UrcHandle handle;
    std::string prefix;
    UrcHandler handler;
  };

  std::unordered_map<std::string, std::vector<HandlerEntry>> handlers_;
  UrcHandle next_handle_ = 1;
};

}  // namespace esp_modem_link::at_channel
