#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "esp_modem_link/at_error.h"
#include "esp_modem_link/network_error.h"

namespace esp_modem_link::at_channel {

class IAtChannel {
 public:
  virtual ~IAtChannel() = default;

  virtual AtResult SendCommand(std::string_view cmd,
                               std::chrono::milliseconds timeout) = 0;

  virtual AtResult SendCommandWithData(std::string_view cmd_prefix,
                                       const void* data,
                                       size_t len,
                                       std::chrono::milliseconds timeout,
                                       std::string_view cmd_suffix = "") = 0;

  virtual std::string_view GetResponse() const = 0;
  virtual std::vector<std::string_view> GetResponseLines() const = 0;

  using UrcHandler =
      std::function<void(std::string_view command,
                         std::string_view arguments)>;
  using UrcHandle = uint64_t;

  virtual UrcHandle SubscribeUrc(std::string_view prefix,
                                 UrcHandler handler) = 0;
  virtual void UnsubscribeUrc(UrcHandle handle) = 0;

  virtual AtResult EnterDataMode(
      int connect_id,
      std::chrono::milliseconds timeout) = 0;
  virtual AtResult ExitDataMode(
      std::chrono::milliseconds timeout) = 0;
  virtual Result<int> SendRaw(const void* data, size_t len) = 0;

  virtual int GetBaudRate() const = 0;
  virtual AtResult SetBaudRate(int baud) = 0;

  virtual void SetDebugLog(bool enable) = 0;
  virtual bool IsDebugLogEnabled() const = 0;
};

}  // namespace esp_modem_link::at_channel
