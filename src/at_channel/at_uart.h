#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

#include "at_channel/iat_channel.h"
#include "at_channel/response_parser.h"
#include "at_channel/urc_dispatcher.h"
#include "platform/ievent_group.h"
#include "platform/itask.h"
#include "platform/iuart.h"

namespace esp_modem_link::at_channel {

class AtUart : public IAtChannel {
 public:
  AtUart(platform::IUart& uart, const platform::UartConfig& config);
  ~AtUart() override;

  AtUart(const AtUart&) = delete;
  AtUart& operator=(const AtUart&) = delete;

  // IAtChannel implementation
  AtResult SendCommand(std::string_view cmd,
                       std::chrono::milliseconds timeout) override;

  AtResult SendCommandWithData(std::string_view cmd_prefix,
                               const void* data,
                               size_t len,
                               std::chrono::milliseconds timeout,
                               std::string_view cmd_suffix) override;

  AtResult SendLine(std::string_view cmd) override;
  AtResult SendDataAfterPrompt(std::string_view prefix,
                               const void* data,
                               size_t len,
                               std::chrono::milliseconds timeout) override;

  std::string_view GetResponse() const override;
  std::vector<std::string> GetResponseLines() const override;

  UrcHandle SubscribeUrc(std::string_view prefix,
                         UrcHandler handler) override;
  void UnsubscribeUrc(UrcHandle handle) override;

  AtResult EnterDataMode(int connect_id,
                         std::chrono::milliseconds timeout) override;
  AtResult ExitDataMode(std::chrono::milliseconds timeout) override;
  Result<int> SendRaw(const void* data, size_t len) override;

  int GetBaudRate() const override;
  AtResult SetBaudRate(int baud) override;

  void SetDebugLog(bool enable) override;
  bool IsDebugLogEnabled() const override;

 private:
  enum EventBits : uint32_t {
    kEventResponseDone = 1 << 0,
    kEventDataPrompt = 1 << 1,
    kEventStop = 1 << 2,
  };

  void ReceiveTask();
  void ProcessLine(std::string_view line);

  // True when the bytes pending at the end of the receive buffer are a data
  // prompt: ">" followed only by blanks. The module writes it without a line
  // terminator, so it can never be recognised as a complete line.
  static bool IsDataPrompt(std::string_view pending);

  // Emits one wire line when debug logging is on; a no-op otherwise. Called
  // from both the command thread and the receive task, so it only reads
  // atomics.
  void LogWire(char direction, std::string_view text);

  platform::IUart& uart_;
  platform::UartConfig config_;

  std::unique_ptr<platform::ITask> recv_task_;
  std::unique_ptr<platform::IEventGroup> event_group_;

  UrcDispatcher urc_dispatcher_;

  mutable std::mutex response_mutex_;
  std::string response_buffer_;
  ParsedAtResponse parsed_response_;

  std::mutex command_mutex_;
  std::atomic<bool> command_in_progress_{false};

  // Set by SendDataAfterPrompt before it writes its command, cleared when the
  // prompt is consumed or that call gives up. Without it, a prompt left over
  // from a command that timed out would satisfy the next command's wait before
  // its own prompt had arrived.
  std::atomic<bool> prompt_armed_{false};

  std::atomic<bool> debug_log_{false};
  // Set by the first line logged, so the trace can be read as a timeline from
  // the start of the session rather than from an arbitrary epoch.
  std::chrono::steady_clock::time_point log_started_at_{};
  std::atomic<long long> log_last_ms_{-1};
  std::atomic<bool> running_{false};

  std::atomic<int> data_mode_connect_id_{-1};

  // Set by SetBaudRate and consumed by the receive task, which clears the
  // partial line it is holding. The partial line lives on the task's stack, so
  // this is the only way the caller that changed the rate can reach it.
  std::atomic<bool> discard_partial_line_{false};
};

}  // namespace esp_modem_link::at_channel
