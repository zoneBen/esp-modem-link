#include "at_channel/baud_alignment.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "at_parser/at_parser.h"

namespace esp_modem_link::at_channel {

using at_parser::ParseInt;
using at_parser::StripKeyPrefix;

namespace {

// Where a module sits if nothing has ever set its rate: AT+IPR=0, the factory
// setting, which is automatic and which the reference hardware answers at
// 115200. It is also the rate the library's own UartConfig default opens the
// port at.
constexpr int kFactoryRate = 115200;

// The fastest rate this library ever asks a module for, and therefore the
// fastest an earlier session could have left one at. It is the ML307 HAL's
// advertised maximum; a module that cannot go this fast simply stays silent at
// this candidate, which costs one probe timeout.
constexpr int kHighestKnownRate = 921600;

// A rate query either answers at once or not at all - there is nothing in
// flight that could still be delivered - so the short budget is safe here. It
// is also what each failed candidate costs, and a module that is not there must
// not cost five seconds a candidate.
constexpr auto kQueryTimeout = std::chrono::milliseconds(500);

// Setting the rate is a state change rather than a query: the module has work
// to do before it answers, and a refusal has to arrive as a refusal rather than
// as a timeout.
constexpr auto kSetTimeout = std::chrono::milliseconds(5000);

// Confirming a new rate. The module does not answer the first command it
// receives at a rate it has just switched to - observed on ML307R-DL-MBRH0S01
// as a timeout on "AT", and then again on "AT+IPR?", at 921600 - so the check is
// retried rather than taken as a refusal. The timeout is short because each
// failed attempt costs it in full, and a module that will answer does so at
// once.
constexpr int kConfirmAttempts = 4;

// Sends a command and copies out its response lines. The lines are read only
// after the command succeeded: both a mock and the hardware leave
// GetResponseLines() holding the previous command's answer when a command
// fails, so reading them regardless would parse a stale rate as the current one.
AtValue<std::vector<std::string>> QueryLines(IAtChannel& channel,
                                             std::string_view cmd,
                                             std::chrono::milliseconds timeout) {
  auto sent = channel.SendCommand(cmd, timeout);
  if (!sent) return std::unexpected(sent.error());

  std::vector<std::string> lines;
  for (auto line : channel.GetResponseLines()) {
    lines.emplace_back(line);
  }
  return lines;
}

// The line carrying the "+IPR" answer, or nullptr if there is none. Found by
// name rather than by position: a URC can be folded into the same response
// ahead of the answer, which is the defect Ml307Hal::QuerySocketActive
// documents for AT+MIPSTATE.
const std::string* FindIprLine(const std::vector<std::string>& lines) {
  for (const auto& line : lines) {
    std::string_view view(line);
    while (!view.empty() && (view.front() == ' ' || view.front() == '\t')) {
      view.remove_prefix(1);
    }
    if (view.rfind("+IPR", 0) == 0) return &line;
  }
  return nullptr;
}

// "+IPR: 921600" -> 921600. Nothing means the module answered without saying
// what its rate is, which is not the same as a rate of zero.
std::optional<int> ParseRate(std::string_view line) {
  auto value = ParseInt(StripKeyPrefix(line));
  if (!value) return std::nullopt;
  return *value;
}

// AT+IPR=? answers with two tuples: the rates the module can be set to, then
// the rates it will accept. The match is on whole numbers, because 115200 is a
// prefix of 1152000 and a bare substring search would accept a rate the module
// turns out to reject - and a rejected rate leaves the link silent.
bool ListsRate(std::string_view supported, int rate) {
  const std::string needle = std::to_string(rate);
  for (size_t pos = supported.find(needle); pos != std::string_view::npos;
       pos = supported.find(needle, pos + needle.size())) {
    const bool starts = pos == 0 ||
                        !std::isdigit(static_cast<unsigned char>(supported[pos - 1]));
    const size_t after = pos + needle.size();
    const bool ends = after >= supported.size() ||
                      !std::isdigit(static_cast<unsigned char>(supported[after]));
    if (starts && ends) return true;
  }
  return false;
}

// Moves the host to `rate` unless it is already there. A channel that cannot be
// moved to a rate could not read an answer given at it either, so the caller
// skips that candidate rather than waiting on a reply that cannot arrive.
bool MoveHostTo(IAtChannel& channel, int rate) {
  if (channel.GetBaudRate() == rate) return true;
  return channel.SetBaudRate(rate).has_value();
}

}  // namespace

std::vector<int> BaudRateCandidates(int requested) {
  std::vector<int> rates;
  for (const int rate : {requested, kHighestKnownRate, kFactoryRate}) {
    if (rate <= 0) continue;
    if (std::find(rates.begin(), rates.end(), rate) != rates.end()) continue;
    rates.push_back(rate);
  }
  return rates;
}

AtValue<int> FindModuleBaudRate(IAtChannel& channel,
                                const std::vector<int>& candidates) {
  const int entry_rate = channel.GetBaudRate();
  // A channel that does not name a rate is not one that could be moved to a
  // candidate and put back, so there is nothing here to align. A stub channel
  // reports 0.
  if (entry_rate <= 0) {
    return std::unexpected(AtError(
        AtErrc::kNotInitialized, "the channel does not report a baud rate"));
  }

  std::vector<int> tried;
  for (const int rate : candidates) {
    if (rate <= 0) continue;
    if (std::find(tried.begin(), tried.end(), rate) != tried.end()) continue;
    tried.push_back(rate);

    if (!MoveHostTo(channel, rate)) continue;

    auto lines = QueryLines(channel, "AT+IPR?", kQueryTimeout);
    if (!lines) continue;  // silence: the module is not listening at this rate

    // Answered, so the two ends agree: a mismatch is not a wrong answer but no
    // answer, which is why the query is what was used to look. Either the
    // module reported its rate or it does not report one at all, and both mean
    // this is where to stop. Scanning on past an answer would walk the host off
    // the one rate that works and leave the link silent - the same failure the
    // caller was trying to avoid.
    if (const std::string* ipr = FindIprLine(*lines)) {
      // A module reporting "+IPR: 0" is reporting automatic baud, which is a
      // setting rather than a rate the host could be moved to. The answer still
      // means the module was found here, so it falls through to `rate` instead
      // of returning a zero the caller would hand straight to SetBaudRate.
      if (auto parsed = ParseRate(*ipr); parsed && *parsed > 0) return *parsed;
    }
    return rate;
  }

  // Nothing answered at any of them, so there is nothing to align to and the
  // channel is put back where the caller had it. A device whose rate cannot be
  // read is not a device that cannot be used: the firmware may not implement
  // AT+IPR? at all. The caller carries on at the rate it asked for, and a
  // caller that reads GetBaudRate() afterwards must not be told a rate the port
  // was only passing through.
  channel.SetBaudRate(entry_rate);
  return std::unexpected(AtError(
      AtErrc::kTimeout,
      "the module did not answer at any of the " + std::to_string(tried.size()) +
          " rates probed"));
}

AtValue<int> SetModuleBaudRate(IAtChannel& channel, int desired) {
  const int current = channel.GetBaudRate();
  if (desired <= 0 || current <= 0) {
    return std::unexpected(AtError(
        AtErrc::kNotInitialized,
        "cannot set a rate on a channel that does not report one"));
  }

  // Already in line. Nothing is written, and the channel stays on the rate that
  // is working, which costs nothing when the two agree.
  if (current == desired) return current;

  // Ask before setting. A rate the module does not accept is a rate that
  // silences it, which is worse than the slow link the caller was trying to
  // improve on. Every "cannot confirm" below leaves the pair on `current`,
  // which is by definition a rate that works.
  auto supported = QueryLines(channel, "AT+IPR=?", kQueryTimeout);
  if (!supported) return current;
  const std::string* ipr = FindIprLine(*supported);
  if (!ipr || !ListsRate(*ipr, desired)) return current;

  auto set = channel.SendCommand("AT+IPR=" + std::to_string(desired), kSetTimeout);
  // Refused, and the host stays put. A module that does not implement AT+IPR=
  // answers ERROR here, which is the ordinary way this returns, and it must keep
  // working: failing instead would refuse to bring up every module whose
  // firmware lacks the command.
  //
  // The cost is one ambiguous case. A module that applied the write and lost the
  // OK - the deafness noted above, one command earlier - leaves the module at
  // `desired` and the host here, blind, and this reports `current`. That is not
  // detectable from this side: the two ends cannot be told apart without asking
  // the module at a rate it may no longer be listening at. It is preferred to
  // the alternative anyway, because a refused write and a lost acknowledgement
  // look identical from here and a refusal is far more likely, so erroring would
  // discard a working link on the common path to catch a rare one.
  if (!set) return current;  // refused; the module is still where it was

  // The module switches on its OK, so from here the two ends only talk again
  // once the host has followed. A host that cannot follow leaves nothing able
  // to talk, which is the one alignment outcome worth reporting: everything
  // else leaves a working link at some rate.
  if (auto moved = channel.SetBaudRate(desired); !moved) {
    return std::unexpected(AtError(
        AtErrc::kNotInitialized,
        "module switched to " + std::to_string(desired) +
            " baud but the channel could not follow"));
  }

  for (int attempt = 0; attempt < kConfirmAttempts; ++attempt) {
    auto probe = QueryLines(channel, "AT+IPR?", kQueryTimeout);
    if (!probe) continue;
    const std::string* now = FindIprLine(*probe);
    if (now && ParseRate(*now) == desired) return desired;
  }

  // Unconfirmed, which covers two states that are indistinguishable from inside
  // the confirm loop: the module took the write and went deaf to the probes, or
  // it never took it and the pair has been apart since. Both ends are put back
  // on a rate that works, but not by assuming which one this is.
  //
  // Assuming is what breaks this. Following the module down without asking
  // leaves the channel on `current` while the module sits on `desired` - a
  // silent link reported as a success, which is the failure this whole function
  // exists to prevent. Staying put without asking has the mirror image. And the
  // module's answer cannot settle it either: the command that asks it to come
  // back is sent over the link that is in question, so in the state where the
  // two ends disagree it is never heard at all.
  //
  // So the pair is not followed, it is searched for: the same read-only scan
  // that found the module at the start runs again over the candidates and
  // returns the rate the module is answering at, which is a rate that works by
  // construction. That is where the host ends, whether it turns out to be the
  // rate the caller asked for or the one it started on.
  (void)channel.SendCommand("AT+IPR=" + std::to_string(current), kSetTimeout);
  auto landed =
      FindModuleBaudRate(channel, BaudRateCandidates(channel.GetBaudRate()));
  if (!landed) {
    return std::unexpected(AtError(
        AtErrc::kTimeout,
        "the module did not confirm " + std::to_string(desired) +
            " baud and could not be found at any rate afterwards"));
  }
  if (!MoveHostTo(channel, *landed)) {
    return std::unexpected(AtError(
        AtErrc::kNotInitialized,
        "the module is answering at " + std::to_string(*landed) +
            " baud but the channel could not follow"));
  }
  return *landed;
}

}  // namespace esp_modem_link::at_channel
