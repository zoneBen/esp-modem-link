#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "at_channel/iat_channel.h"
#include "at_channel/response_parser.h"
#include "at_channel/urc_dispatcher.h"

namespace esp_modem_link::testing {

// A programmable IAtChannel for testing module HALs without a UART.
//
// Usage: queue a response for a command, then call it.
//   channel.ExpectCommand("AT+CGSN").Respond("+CGSN: 1234\r\nOK\r\n");
//   channel.ExpectCommand("AT+CSQ").Respond("+CSQ: 31,99\r\nOK\r\n");
class MockAtChannel : public at_channel::IAtChannel {
 public:
  // --- Programming API ---

  // Queue a canned response for a command prefix (matched by StartsWith).
  MockAtChannel& ExpectCommand(std::string_view prefix) {
    pending_prefix_ = std::string(prefix);
    return *this;
  }

  MockAtChannel& Respond(std::string raw_response) {
    responses_[pending_prefix_] = std::move(raw_response);
    pending_prefix_.clear();
    return *this;
  }

  // Queue one response per call, consumed in order and then repeating the last
  // one. A command whose answer changes between calls cannot be scripted with a
  // single response per prefix: "AT+MIPSTATE=0" reports a socket held on the
  // first query and released on the next, and that transition is the thing under
  // test.
  MockAtChannel& RespondInTurn(std::vector<std::string> raw_responses) {
    sequences_[pending_prefix_] = std::move(raw_responses);
    pending_prefix_.clear();
    return *this;
  }

  // Fire a URC while the next matching command is in flight, before its
  // response is returned. This is how a module reports an asynchronous result
  // (e.g. "+MIPOPEN: 0,0") that lands alongside the command's own OK, and it
  // exercises the ordering a real UART produces. Chain it between
  // ExpectCommand and Respond:
  //   channel.ExpectCommand("AT+MIPOPEN").ThenUrc("+MIPOPEN: 0,0\r\n")
  //          .Respond("OK\r\n");
  // Each call queues one URC and each matching command consumes one, so opening
  // several sockets takes one queued result apiece.
  MockAtChannel& ThenUrc(std::string line) {
    return ThenUrcFrom(
        [line = std::move(line)](const std::string&) { return line; });
  }

  // Queue a URC built from the command that triggers it. Needed when the event
  // has to name something the command carries but the test cannot know in
  // advance - an AT+MIPOPEN result names the cid, and which cid the allocator
  // picks is more than the test should have to predict.
  MockAtChannel& ThenUrcFrom(
      std::function<std::string(const std::string&)> make) {
    pending_urcs_.push_back(
        PendingUrc{std::string(pending_prefix_), std::move(make)});
    return *this;
  }

  // Queue a failure (timeout) for a command prefix.
  // Sticky until ClearFailures() is called.
  MockAtChannel& FailCommand(std::string_view prefix,
                             AtErrc code = AtErrc::kTimeout) {
    failures_[std::string(prefix)] = code;
    return *this;
  }

  // Remove all queued failures so subsequent commands succeed again.
  void ClearFailures() { failures_.clear(); }

  // Set the default response for any unmatched command.
  void SetDefaultResponse(std::string raw) { default_response_ = std::move(raw); }

  // The rate the module is listening at. Until this is called the mock answers
  // at whatever rate the channel is on, which is convenient but is not what the
  // hardware does: a module at another rate says nothing at all, on every
  // command, and that silence is the reason the rate has to be settled before
  // anything else is sent. Setting it makes the mock model the silence, so a
  // test of the rate negotiation exercises the same failure a module produces.
  MockAtChannel& SetModuleBaudRate(int baud) {
    module_baud_rate_ = baud;
    return *this;
  }

  // --- Inspection API ---

  const std::vector<std::string>& SentCommands() const { return sent_commands_; }

  bool WasCommandSent(std::string_view cmd) const {
    return std::any_of(sent_commands_.begin(), sent_commands_.end(),
                       [&](const std::string& s) {
                         return s.rfind(cmd, 0) == 0;
                       });
  }

  const std::vector<std::pair<std::string, std::string>>& SentDataCommands()
      const {
    return sent_data_commands_;
  }

  // Fire a URC to subscribers, as if the module sent it unsolicited.
  void InjectUrc(std::string_view line) { dispatcher_.Dispatch(line); }

  void Reset() {
    sent_commands_.clear();
    sent_data_commands_.clear();
  }

  // --- IAtChannel implementation ---

  AtResult SendCommand(std::string_view cmd,
                       std::chrono::milliseconds timeout) override {
    (void)timeout;
    last_command_ = std::string(cmd);
    sent_commands_.push_back(last_command_);

    if (SilencedByRate()) {
      return std::unexpected(AtError(AtErrc::kTimeout, last_command_));
    }

    for (const auto& [prefix, code] : failures_) {
      if (last_command_.rfind(prefix, 0) == 0) {
        return std::unexpected(AtError(code, last_command_));
      }
    }

    std::string raw = FindResponse(last_command_);
    last_response_ = raw;
    parsed_ = at_channel::ParseResponse(raw);

    // A module that accepts a rate change moves immediately, while the command
    // itself was still answered at the old rate. Without this the mock would keep
    // refusing to hear the host at the new rate, and the confirmation that
    // follows every change could never succeed.
    if (parsed_.ok && module_baud_rate_ &&
        last_command_.rfind("AT+IPR=", 0) == 0) {
      const std::string value = last_command_.substr(7);
      if (!value.empty() &&
          value.find_first_not_of("0123456789") == std::string::npos) {
        // "AT+IPR=0" is auto-baud, not a rate of zero; the module is left where
        // it is.
        const int rate = std::atoi(value.c_str());
        if (rate > 0) module_baud_rate_ = rate;
      }
    }

    // Deliver the URC scheduled for this command before reporting its result,
    // so a handler that parks waiting on one sees it arrive mid-command.
    if (auto urc = TakeUrc(last_command_)) {
      dispatcher_.Dispatch(*urc);
    }

    if (!parsed_.ok) {
      return std::unexpected(parsed_.error);
    }
    return {};
  }

  AtResult SendCommandWithData(std::string_view cmd_prefix,
                               const void* data,
                               size_t len,
                               std::chrono::milliseconds timeout,
                               std::string_view cmd_suffix = "") override {
    (void)timeout;
    sent_commands_.push_back(std::string(cmd_prefix));
    sent_data_commands_.emplace_back(
        std::string(cmd_prefix),
        std::string(static_cast<const char*>(data), len) +
            std::string(cmd_suffix));

    std::string prefix_str(cmd_prefix);
    if (SilencedByRate()) {
      return std::unexpected(AtError(AtErrc::kTimeout, prefix_str));
    }

    for (const auto& [prefix, code] : failures_) {
      if (prefix_str.rfind(prefix, 0) == 0) {
        return std::unexpected(AtError(code, prefix_str));
      }
    }

    std::string raw = FindResponse(prefix_str);
    last_response_ = raw;
    parsed_ = at_channel::ParseResponse(raw);
    if (!parsed_.ok) {
      return std::unexpected(parsed_.error);
    }
    return {};
  }

  std::string_view GetResponse() const override { return last_response_; }

  std::vector<std::string_view> GetResponseLines() const override {
    std::vector<std::string_view> lines;
    lines.reserve(parsed_.lines.size());
    for (const auto& line : parsed_.lines) {
      lines.emplace_back(line);
    }
    return lines;
  }

  UrcHandle SubscribeUrc(std::string_view prefix,
                         UrcHandler handler) override {
    return dispatcher_.Subscribe(prefix, std::move(handler));
  }

  void UnsubscribeUrc(UrcHandle handle) override {
    dispatcher_.Unsubscribe(handle);
  }

  AtResult EnterDataMode(int connect_id,
                         std::chrono::milliseconds timeout) override {
    (void)connect_id;
    (void)timeout;
    return {};
  }

  AtResult ExitDataMode(std::chrono::milliseconds timeout) override {
    (void)timeout;
    return {};
  }

  Result<int> SendRaw(const void* data, size_t len) override {
    (void)data;
    return static_cast<int>(len);
  }

  int GetBaudRate() const override { return baud_rate_; }
  AtResult SetBaudRate(int baud) override {
    baud_rate_ = baud;
    return {};
  }

  void SetDebugLog(bool enable) override { debug_ = enable; }
  bool IsDebugLogEnabled() const override { return debug_; }

 private:
  // A module set to another rate is silent, not wrong: nothing it sends arrives
  // legibly, so the command simply times out.
  bool SilencedByRate() const {
    return module_baud_rate_ && baud_rate_ != *module_baud_rate_;
  }

  std::string FindResponse(const std::string& cmd) {
    // Longest-prefix match wins, so "AT+CGDCONT=1" beats "AT+".
    const std::string* best = nullptr;
    size_t best_len = 0;
    for (const auto& [prefix, response] : responses_) {
      if (cmd.rfind(prefix, 0) == 0 && prefix.size() > best_len) {
        best = &response;
        best_len = prefix.size();
      }
    }

    // A scripted sequence wins a tie against a single response on the same
    // prefix: a test that needs a changing answer has said what it expects.
    for (const auto& [prefix, sequence] : sequences_) {
      if (sequence.empty()) continue;
      if (cmd.rfind(prefix, 0) != 0 || prefix.size() < best_len) continue;

      size_t& index = sequence_indexes_[prefix];
      const std::string& value =
          sequence[std::min(index, sequence.size() - 1)];
      if (index + 1 < sequence.size()) ++index;
      return value;
    }

    if (best) return *best;
    return default_response_;
  }

  // A queued URC, kept as a builder so it can be shaped by the command that
  // triggers it.
  struct PendingUrc {
    std::string prefix;
    std::function<std::string(const std::string&)> make;
  };

  // Take the first URC queued for this command and consume it. A later call
  // with the same command sees the next queued one, which is what lets a test
  // open several sockets off one ExpectCommand.
  std::optional<std::string> TakeUrc(const std::string& cmd) {
    for (auto it = pending_urcs_.begin(); it != pending_urcs_.end(); ++it) {
      if (cmd.rfind(it->prefix, 0) == 0) {
        std::string line = it->make(cmd);
        pending_urcs_.erase(it);
        return line;
      }
    }
    return std::nullopt;
  }

  at_channel::UrcDispatcher dispatcher_;
  std::map<std::string, std::string> responses_;
  std::map<std::string, std::vector<std::string>> sequences_;
  std::map<std::string, size_t> sequence_indexes_;
  std::vector<PendingUrc> pending_urcs_;
  std::map<std::string, AtErrc> failures_;
  std::string default_response_ = "OK\r\n";
  std::string pending_prefix_;

  std::string last_command_;
  std::string last_response_;
  at_channel::ParsedAtResponse parsed_;

  std::vector<std::string> sent_commands_;
  std::vector<std::pair<std::string, std::string>> sent_data_commands_;

  int baud_rate_ = 115200;
  // Unset by default, so the mock keeps answering at any rate until a test asks
  // for the module's real behaviour.
  std::optional<int> module_baud_rate_;
  bool debug_ = false;
};

}  // namespace esp_modem_link::testing
