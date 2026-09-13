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

  // Writes "cmd" + CRLF and returns once the bytes are out. No response is
  // awaited: the caller has its own rendezvous for the outcome, fed by a URC
  // handler, and blocking here would only race that handler. The command mutex
  // is still held for the duration of the write, so this cannot interleave with
  // another command's bytes.
  //
  // For commands whose answer is an event rather than a response line - the
  // ones that end in "CONNECT OK", "CLOSE OK" or "SHUT OK" instead of "OK".
  virtual AtResult SendLine(std::string_view cmd) = 0;

  // Writes "prefix" + CRLF, waits for the module's ">" data prompt, writes the
  // payload, and returns.
  //
  // The wait covers the response as well as the prompt, so a command the module
  // refuses fails here with the module's own reason instead of burning the whole
  // timeout. It is also a hard rule that the payload is not written unless the
  // prompt arrived: a module that never prompted is not collecting bytes, and
  // anything sent then is read as the next command line.
  //
  // The trailing acknowledgement ("SEND OK") is deliberately not awaited. The
  // caller waits for it against its own cid, where a late acknowledgement
  // belonging to an earlier chunk can be told apart from this one's.
  virtual AtResult SendDataAfterPrompt(std::string_view prefix,
                                       const void* data,
                                       size_t len,
                                       std::chrono::milliseconds timeout) = 0;

  virtual std::string_view GetResponse() const = 0;
  // Owned copies, not views. The lines live in the channel's response buffer
  // under a mutex, so a view would dangle the moment this returned - and a
  // caller reading them after the next command would read that command's lines.
  virtual std::vector<std::string> GetResponseLines() const = 0;

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
