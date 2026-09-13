#include "at_channel/at_uart.h"

#include <cstdio>
#include <cstring>

namespace esp_modem_link::at_channel {

// Every line in and out, tagged by direction. The traffic is the only place
// some faults show: a socket that dies on the first request of a process does
// so before any caller-visible check has anything to report, and the ordering
// of the lines around it is what identifies the cause.
//
// Each line carries milliseconds since the first line logged and since the one
// before it, so the shape of a transfer in time is visible. A body that takes
// seconds to come off the wire is a throughput problem, which is a different
// fix from a body that arrives promptly and is then truncated.
void AtUart::LogWire(char direction, std::string_view text) {
  if (!debug_log_.load()) return;

  const auto now = std::chrono::steady_clock::now();
  if (log_started_at_.time_since_epoch().count() == 0) {
    log_started_at_ = now;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - log_started_at_)
                           .count();
  const long long previous = log_last_ms_.exchange(elapsed);

  std::printf("[%7lld +%5lld] %c %.*s\n", elapsed,
              previous < 0 ? 0 : elapsed - previous, direction,
              static_cast<int>(text.size()), text.data());
  std::fflush(stdout);
}

AtUart::AtUart(platform::IUart& uart, const platform::UartConfig& config)
    : uart_(uart), config_(config) {
  event_group_ = platform::CreateEventGroup();

  recv_task_ = platform::CreateTask(
      "at_uart_rx", [this]() { ReceiveTask(); },
      config.rx_buffer_size + 2048, 6);

  running_ = true;
  recv_task_->Start();
}

AtUart::~AtUart() {
  running_ = false;
  event_group_->SetBits(kEventStop);
  if (recv_task_) {
    recv_task_->Stop();
  }
}

AtResult AtUart::SendCommand(std::string_view cmd,
                              std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> lock(command_mutex_);
  command_in_progress_ = true;

  {
    std::lock_guard<std::mutex> resp_lock(response_mutex_);
    response_buffer_.clear();
    parsed_response_ = ParsedAtResponse();
  }

  event_group_->ClearBits(kEventResponseDone);

  LogWire('>', cmd);

  std::string full_cmd = std::string(cmd) + "\r\n";
  int sent = uart_.Send(full_cmd.data(), full_cmd.size());
  if (sent < 0 || static_cast<size_t>(sent) != full_cmd.size()) {
    command_in_progress_ = false;
    return std::unexpected(
        AtError(AtErrc::kTransmitFailed, "failed to send command"));
  }

  uint32_t bits = event_group_->WaitBits(
      kEventResponseDone, true, true, timeout);

  command_in_progress_ = false;

  if (!(bits & kEventResponseDone)) {
    return std::unexpected(
        AtError(AtErrc::kTimeout, "command timeout: " + std::string(cmd)));
  }

  std::lock_guard<std::mutex> resp_lock(response_mutex_);
  if (parsed_response_.ok) {
    return {};
  }
  return std::unexpected(parsed_response_.error);
}

AtResult AtUart::SendCommandWithData(std::string_view cmd_prefix,
                                     const void* data,
                                     size_t len,
                                     std::chrono::milliseconds timeout,
                                     std::string_view cmd_suffix) {
  std::lock_guard<std::mutex> lock(command_mutex_);
  command_in_progress_ = true;

  {
    std::lock_guard<std::mutex> resp_lock(response_mutex_);
    response_buffer_.clear();
    parsed_response_ = ParsedAtResponse();
  }

  event_group_->ClearBits(kEventResponseDone | kEventDataPrompt);

  // Send command prefix
  std::string prefix_str(cmd_prefix);
  if (!cmd_suffix.empty()) {
    prefix_str += "\r\n";
  }
  int sent = uart_.Send(prefix_str.data(), prefix_str.size());
  if (sent < 0 || static_cast<size_t>(sent) != prefix_str.size()) {
    command_in_progress_ = false;
    return std::unexpected(
        AtError(AtErrc::kTransmitFailed, "failed to send command prefix"));
  }

  if (!cmd_suffix.empty()) {
    // Wait for ">" prompt
    uint32_t bits = event_group_->WaitBits(
        kEventDataPrompt, true, true, timeout);
    if (!(bits & kEventDataPrompt)) {
      command_in_progress_ = false;
      return std::unexpected(
          AtError(AtErrc::kTimeout, "data prompt timeout"));
    }

    // Send data
    sent = uart_.Send(data, len);
    if (sent < 0 || static_cast<size_t>(sent) != len) {
      command_in_progress_ = false;
      return std::unexpected(
          AtError(AtErrc::kTransmitFailed, "failed to send data"));
    }

    // Send suffix (e.g. Ctrl+Z)
    std::string suffix_str(cmd_suffix);
    uart_.Send(suffix_str.data(), suffix_str.size());
  }

  // Wait for final response
  uint32_t bits = event_group_->WaitBits(
      kEventResponseDone, true, true, timeout);

  command_in_progress_ = false;

  if (!(bits & kEventResponseDone)) {
    return std::unexpected(
        AtError(AtErrc::kTimeout, "command with data timeout"));
  }

  std::lock_guard<std::mutex> resp_lock(response_mutex_);
  if (parsed_response_.ok) {
    return {};
  }
  return std::unexpected(parsed_response_.error);
}

AtResult AtUart::SendLine(std::string_view cmd) {
  std::lock_guard<std::mutex> lock(command_mutex_);

  LogWire('>', cmd);

  std::string full_cmd = std::string(cmd) + "\r\n";
  int sent = uart_.Send(full_cmd.data(), full_cmd.size());
  if (sent < 0 || static_cast<size_t>(sent) != full_cmd.size()) {
    return std::unexpected(
        AtError(AtErrc::kTransmitFailed, "failed to send command"));
  }

  // The response buffer and command_in_progress_ are left alone on purpose. No
  // response is awaited, so there is no window to own: whatever the module says
  // back arrives as an idle line and is dispatched to the URC handlers, which is
  // where the caller's rendezvous is listening.
  return {};
}

AtResult AtUart::SendDataAfterPrompt(std::string_view prefix,
                                     const void* data,
                                     size_t len,
                                     std::chrono::milliseconds timeout) {
  std::lock_guard<std::mutex> lock(command_mutex_);
  command_in_progress_ = true;

  {
    std::lock_guard<std::mutex> resp_lock(response_mutex_);
    response_buffer_.clear();
    parsed_response_ = ParsedAtResponse();
  }

  event_group_->ClearBits(kEventResponseDone | kEventDataPrompt);

  // Armed before the command goes out, not after. A prompt left over from an
  // earlier command that timed out would otherwise satisfy the wait below before
  // this command had prompted, and the payload would be written to a module that
  // is not collecting bytes - where it is read as the start of the next command.
  prompt_armed_ = true;

  LogWire('>', prefix);

  std::string full_cmd = std::string(prefix) + "\r\n";
  int sent = uart_.Send(full_cmd.data(), full_cmd.size());
  if (sent < 0 || static_cast<size_t>(sent) != full_cmd.size()) {
    prompt_armed_ = false;
    command_in_progress_ = false;
    return std::unexpected(
        AtError(AtErrc::kTransmitFailed, "failed to send command prefix"));
  }

  // Either bit ends the wait. The prompt is the expected one; a response is how
  // a refusal arrives, and it carries the module's own reason - worth reporting
  // over a timeout the caller can only guess at.
  uint32_t bits = event_group_->WaitBits(
      kEventDataPrompt | kEventResponseDone, true, false, timeout);

  prompt_armed_ = false;

  if (bits & kEventDataPrompt) {
    int written = uart_.Send(data, len);
    command_in_progress_ = false;
    if (written < 0 || static_cast<size_t>(written) != len) {
      return std::unexpected(
          AtError(AtErrc::kTransmitFailed, "failed to send data"));
    }
    // No wait for the acknowledgement. It is matched against the cid by the
    // caller, which is the only place a late one can be told from this one's.
    return {};
  }

  command_in_progress_ = false;

  if (bits & kEventResponseDone) {
    std::lock_guard<std::mutex> resp_lock(response_mutex_);
    if (parsed_response_.ok) {
      // Accepted, but silent: no prompt ever came. Nothing may be written, so
      // this is a failure even though the module answered OK.
      return std::unexpected(AtError(
          AtErrc::kTimeout, "no data prompt after a successful command"));
    }
    return std::unexpected(parsed_response_.error);
  }

  return std::unexpected(
      AtError(AtErrc::kTimeout, "data prompt timeout: " + std::string(prefix)));
}

std::string_view AtUart::GetResponse() const {
  std::lock_guard<std::mutex> lock(response_mutex_);
  return response_buffer_;
}

std::vector<std::string> AtUart::GetResponseLines() const {
  std::lock_guard<std::mutex> lock(response_mutex_);
  return parsed_response_.lines;
}

IAtChannel::UrcHandle AtUart::SubscribeUrc(std::string_view prefix,
                                            UrcHandler handler) {
  return urc_dispatcher_.Subscribe(prefix, std::move(handler));
}

void AtUart::UnsubscribeUrc(UrcHandle handle) {
  urc_dispatcher_.Unsubscribe(handle);
}

AtResult AtUart::EnterDataMode(int connect_id,
                                std::chrono::milliseconds timeout) {
  // Basic implementation - modules that support data mode will override behavior
  (void)connect_id;
  (void)timeout;
  return std::unexpected(
      AtError(AtErrc::kNotInitialized, "data mode not supported"));
}

AtResult AtUart::ExitDataMode(std::chrono::milliseconds timeout) {
  (void)timeout;
  return std::unexpected(
      AtError(AtErrc::kNotInitialized, "data mode not supported"));
}

Result<int> AtUart::SendRaw(const void* data, size_t len) {
  int sent = uart_.Send(data, len);
  if (sent < 0) {
    return std::unexpected(
        NetworkError(NetworkErrc::kTransmitFailed, 0, "uart send failed"));
  }
  return sent;
}

int AtUart::GetBaudRate() const {
  return uart_.GetBaudRate();
}

AtResult AtUart::SetBaudRate(int baud) {
  uart_.SetBaudRate(baud);
  config_.baud_rate = baud;

  // Whatever is in the receive queue was clocked at the old rate and decodes as
  // noise at the new one. Dropping it here is what keeps the next command from
  // being answered with the previous rate's garbage.
  uart_.Flush();
  discard_partial_line_ = true;
  return {};
}

void AtUart::SetDebugLog(bool enable) {
  debug_log_ = enable;
}

bool AtUart::IsDebugLogEnabled() const {
  return debug_log_.load();
}

void AtUart::ReceiveTask() {
  constexpr size_t kBufferSize = 1024;
  std::vector<uint8_t> buffer(kBufferSize);
  std::string line_buffer;

  while (running_) {
    // Check for stop signal
    auto stop_bits = event_group_->WaitBits(
        kEventStop, false, false, std::chrono::milliseconds(0));
    if (stop_bits & kEventStop) break;

    // A rate change invalidates whatever is half-assembled here: those bytes
    // were clocked at the old rate, and a line with no "\r\n" behind it yet
    // would otherwise be glued to the front of the next real line and parsed as
    // part of it.
    if (discard_partial_line_.exchange(false)) {
      line_buffer.clear();
    }

    int bytes_read = uart_.Receive(
        buffer.data(), kBufferSize, std::chrono::milliseconds(100));

    // The check above only covers a change that had already happened when this
    // iteration started. A change that lands while the task is blocked in the
    // read is caught here instead, and it has to be: Flush() cannot reach into
    // an in-flight read, so these bytes may be the tail of the old rate and they
    // would be appended to the very partial the flag exists to drop - one line
    // assembled from two rates, which parses as neither.
    //
    // Dropping them costs at most the first answer after a change. Every caller
    // that changes a rate already tolerates a lost answer: the confirmation
    // probe retries, and the scan that finds the module tolerates one silence.
    // Keeping them costs a URC, which nothing retries.
    if (discard_partial_line_.exchange(false)) {
      line_buffer.clear();
      continue;
    }

    if (bytes_read <= 0) continue;

    line_buffer.append(reinterpret_cast<char*>(buffer.data()), bytes_read);

    // Process complete lines
    size_t pos = 0;
    while (pos < line_buffer.size()) {
      size_t line_end = line_buffer.find("\r\n", pos);
      if (line_end == std::string::npos) break;

      std::string_view line(line_buffer.data() + pos, line_end - pos);
      ProcessLine(line);
      pos = line_end + 2;
    }

    // Remove processed data
    if (pos > 0) {
      line_buffer.erase(0, pos);
    }

    // A prompt for a payload is not a line and never will be: the module writes
    // ">" with no CRLF behind it, so these bytes would sit here until the *next*
    // line's CRLF arrived and then be parsed as the front of that line. AIR780E
    // answers AT+CIPSEND with "\r\n> " - a trailing space, no terminator - so it
    // is recognised and consumed from the partial buffer here.
    //
    // Consuming is unconditional; signalling is not. A prompt from a command that
    // already timed out is still consumed, because otherwise it becomes a "> "
    // line inside the next command's response, and ML307's
    // ReadSingleLineResponse returns lines[0] - so GetImei() would start
    // answering "> ". But only a prompt this channel is waiting for may raise the
    // event, or that stale one would satisfy the next command's wait before its
    // own prompt had come.
    if (IsDataPrompt(line_buffer)) {
      if (prompt_armed_.exchange(false)) {
        event_group_->SetBits(kEventDataPrompt);
      }
      line_buffer.clear();
    }
  }
}

bool AtUart::IsDataPrompt(std::string_view pending) {
  if (pending.empty() || pending.front() != '>') return false;
  for (char c : pending.substr(1)) {
    if (c != ' ' && c != '\t') return false;
  }
  return true;
}

void AtUart::ProcessLine(std::string_view line) {
  // Trim trailing \r
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }

  // Skip empty lines
  bool all_space = true;
  for (char c : line) {
    if (c != ' ' && c != '\t') {
      all_space = false;
      break;
    }
  }
  if (all_space) return;

  // 'R' for a line absorbed into a command's response, 'U' for one arriving
  // while idle. Both are dispatched to the URC handlers; the marker says which
  // response buffer the line also went into, not whether a handler saw it.
  LogWire(command_in_progress_ ? 'R' : 'U', line);

  // Every completed line reaches the handlers, whether or not a command is in
  // flight. A line arriving mid-command is still folded into that command's
  // response below - that is what 'R' means - but it is no longer withheld from
  // the handlers on that account.
  //
  // Withholding it was the root cause of a recurring bug: a socket event that
  // landed while a send was in flight reached nobody, so a client could wait out
  // a timeout for data that had already arrived. ML307 patched around it twice -
  // reading "+MIPOPEN" back out of its own response, and DispatchAbsorbedTraffic
  // after every send - and both patches are gone now that the line is delivered
  // once, where it arrives.
  //
  // INVARIANT: a handler runs on the receive task. While a command is in flight
  // that task is the only one that can deliver the response the sender is
  // blocked on, so a handler that sent its own AT command would block on
  // command_mutex_ and deadlock the channel. Handlers may record state and
  // dispatch callbacks; they must not send.
  urc_dispatcher_.Dispatch(line);

  if (command_in_progress_) {
    {
      std::lock_guard<std::mutex> lock(response_mutex_);
      response_buffer_.append(line);
      response_buffer_.append("\r\n");
    }

    // Check for data prompt (">")
    if (line.size() == 1 && line[0] == '>') {
      event_group_->SetBits(kEventDataPrompt);
    }

    // Try parsing to check if we have a complete response
    ParsedAtResponse parsed;
    {
      std::lock_guard<std::mutex> lock(response_mutex_);
      parsed = ParseResponse(response_buffer_);
    }

    // If we found a status line, signal completion
    // We detect status lines by checking if the line is OK/ERROR/CME/CMS/>
    std::string trimmed(line);
    // trim whitespace
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
      trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
      trimmed.pop_back();
    }

    bool is_status = false;
    if (trimmed == "OK" || trimmed == "ERROR") {
      is_status = true;
    } else if (trimmed.size() >= 11 &&
               (trimmed.substr(0, 11) == "+CME ERROR:" ||
                trimmed.substr(0, 11) == "+CMS ERROR:")) {
      is_status = true;
    } else if (trimmed == ">") {
      // > is a prompt, not a final status for commands with data
      // But for simple commands without data, it might be
      is_status = false;
    }

    if (is_status) {
      // Update parsed response
      std::lock_guard<std::mutex> lock(response_mutex_);
      parsed_response_ = ParseResponse(response_buffer_);
      event_group_->SetBits(kEventResponseDone);
    }
  }
  // Nothing to do when idle: the dispatch above already ran. A second one here
  // would hand the same line to every handler twice.
}

}  // namespace esp_modem_link::at_channel
