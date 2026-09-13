#pragma once

#include <vector>

#include "at_channel/iat_channel.h"
#include "esp_modem_link/at_error.h"

namespace esp_modem_link::at_channel {

// AT+IPR is an AT convention rather than a module-specific command, so the
// negotiation lives here rather than in a HAL: every module that has the
// command reuses this, and the HAL never sees it.
//
// Reference readings, ML307R-DL-MBRH0S01:
//
//   AT+IPR?   "+IPR: 115200"
//   AT+IPR=?  "+IPR: (1200,4800,9600,14400,19200,28800,38400,57600,115200),
//                     (0,300,1200,4800,9600,14400,19200,28800,38400,57600,
//                      115200,230400,460800,921600)"
//
// The two tuples differ: the first lists the rates the module can be set to,
// the second the rates it will accept. The command is checked against the
// second by AT+IPR=? itself, which reports one line containing both.

// The rates to probe, in the order to try them, with the requested rate first:
// a module already on it costs nothing extra, which is the normal case once a
// rate has been established.
//
// The other two are where a module an earlier session moved can be found.
// kHighestKnownRate is where this library would have left it - ML307R-DL
// accepts 921600 and the HAL advertises it as the module maximum - and
// kFactoryRate is where a module that has never been reconfigured sits, since
// its factory setting is AT+IPR=0 (automatic), which the reference hardware
// answers at 115200.
//
// Intermediate rates (460800, 230400) are deliberately absent: this library
// only ever writes the rate its caller asked for, and the only rates a caller
// has asked for are these, so a probe at anything else is a second of silence
// on every failed detect.
std::vector<int> BaudRateCandidates(int requested);

// Moves the channel to each candidate in turn and asks the module for its rate
// there, returning the rate it answered at. Read-only: nothing is written to
// the module, so this is safe to run against a device that has not been
// identified yet.
//
// On success the channel is left on a rate the module answers at - which is by
// definition the module's own rate - so the caller may follow with
// SetModuleBaudRate(). When no candidate answers, the channel is put back on
// the rate it was on at entry and an error is returned: a caller reading
// GetBaudRate() afterwards must not be told a rate the port was only passing
// through.
//
// A module that answers but does not report a rate counts as found. Its
// firmware may not implement AT+IPR? even though it answers commands, and
// treating that as "not found" would keep scanning past the one rate that
// works and leave the link silent.
AtValue<int> FindModuleBaudRate(IAtChannel& channel,
                                const std::vector<int>& candidates);

// Sets the module to `desired` and moves the channel after it, returning the
// rate both ends end up on. The channel must already be on a rate the module
// answers at - FindModuleBaudRate establishes that - and that rate is the one
// the pair falls back to.
//
// The module is asked before it is told: AT+IPR=? is consulted first, because a
// rate the module does not accept is a rate that silences it, which is worse
// than the slow link the caller was trying to improve on.
//
// Postcondition, on every path that returns a rate: the channel sits on a rate
// the module is answering at, never on `desired` merely because it was asked for.
// The early returns hold it because they leave the channel where the caller put
// it; a rewrite that restores `desired` on the way out would break this. A path
// that cannot establish the rate at all returns an error instead of one of the
// guesses - which is why an unconfirmed change ends by searching for the module
// again rather than by following it down or by staying put, since either of
// those can leave the two ends apart and call it a success.
//
// One case is genuinely undecidable from this side and is documented at the
// return: if the module applies the write but its OK is lost, it is left at
// `desired` while the channel stays put, and this still reports the old rate.
AtValue<int> SetModuleBaudRate(IAtChannel& channel, int desired);

}  // namespace esp_modem_link::at_channel
