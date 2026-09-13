#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "at_channel/baud_alignment.h"
#include "mock_at_channel.h"

using namespace esp_modem_link;
using namespace esp_modem_link::at_channel;
using esp_modem_link::testing::MockAtChannel;

namespace {

// A module listening at a rate other than the one the channel is using does not
// answer, and does not say so - every command just times out. The rate has to be
// settled before the module can even be identified, and the module has to be
// told rather than followed: with AT+IPR=0, its factory setting,
// ML307R-DL-MBRH0S01 stayed put while three commands at 921600 all timed out.
//
// These cases were Ml307Hal's, and moved here with the code. They had to: they
// called Ml307Hal::Initialize directly, which is the one path that still reached
// the negotiation after it had become unreachable from CellularDevice - every
// one of them passed while the feature was dead on hardware.

// The two stages the device runs, in the same order and with the same error
// handling: the scan's failure is dropped, the write's is not. A test that wants
// to see inside one stage calls it directly.
AtValue<int> AlignLikeDevice(MockAtChannel& channel) {
  const int requested = channel.GetBaudRate();
  (void)FindModuleBaudRate(channel, BaudRateCandidates(requested));
  return SetModuleBaudRate(channel, requested);
}

int CountCommands(const MockAtChannel& channel, std::string_view cmd) {
  const auto& sent = channel.SentCommands();
  return static_cast<int>(
      std::count(sent.begin(), sent.end(), std::string(cmd)));
}

}  // namespace

// --- Candidate list ---

TEST(BaudRateCandidatesTest, TriesTheRequestedRateFirst) {
  // First because a module already on it costs nothing extra, which is the
  // normal case once a rate has been established.
  EXPECT_EQ(BaudRateCandidates(115200), (std::vector<int>{115200, 921600}));
}

TEST(BaudRateCandidatesTest, TriesTheRaisedRateSecondAndTheFactoryRateLast) {
  EXPECT_EQ(BaudRateCandidates(921600), (std::vector<int>{921600, 115200}));
}

TEST(BaudRateCandidatesTest, IncludesARateThatIsNeitherOfTheTwoKnownOnes) {
  EXPECT_EQ(BaudRateCandidates(9600), (std::vector<int>{9600, 921600, 115200}));
}

TEST(BaudRateCandidatesTest, DropsARateThatIsNotARate) {
  // A stub channel reports 0, and there is no such rate to probe.
  EXPECT_EQ(BaudRateCandidates(0), (std::vector<int>{921600, 115200}));
}

// --- Finding the module ---

// The case the negotiation exists for: the module is on the rate it shipped
// with, the caller asked for a faster one, and the port was opened at the faster
// one. Every command at that rate is silence - including the query that would
// have revealed the mismatch - so the query has to be placed at a rate the
// module might be on rather than at the one the caller wants.
TEST(BaudAlignmentTest, AlignsTheModuleToTheConfiguredRate) {
  MockAtChannel channel;
  channel.SetModuleBaudRate(115200);
  channel.SetBaudRate(921600);
  channel.ExpectCommand("AT+IPR?")
      .RespondInTurn({"+IPR: 115200\r\nOK\r\n",   // asked at 115200 and answered
                      "+IPR: 921600\r\nOK\r\n"});  // and again once it has moved
  channel.ExpectCommand("AT+IPR=?")
      .Respond("+IPR: (1200,4800,115200),(0,300,115200,921600)\r\nOK\r\n");
  channel.ExpectCommand("AT+IPR=921600").Respond("OK\r\n");

  auto aligned = AlignLikeDevice(channel);

  ASSERT_TRUE(aligned.has_value());
  EXPECT_EQ(*aligned, 921600);
  EXPECT_TRUE(channel.WasCommandSent("AT+IPR=921600"));
  EXPECT_EQ(channel.GetBaudRate(), 921600);
}

// A module an earlier session raised stays there until it is told otherwise, so
// the next caller asking for the default finds it up at the raised rate. The
// requested rate is silence, and so is the factory setting; the module's own
// maximum is where it answers.
TEST(BaudAlignmentTest, FindsTheModuleOnARaisedRateAfterOpeningAtTheDefault) {
  MockAtChannel channel;
  channel.SetModuleBaudRate(921600);
  channel.ExpectCommand("AT+IPR?")
      .RespondInTurn({"+IPR: 921600\r\nOK\r\n",    // asked at 921600 and answered
                      "+IPR: 115200\r\nOK\r\n"});  // and again once it has moved
  channel.ExpectCommand("AT+IPR=?")
      .Respond("+IPR: (1200,115200),(0,300,115200,921600)\r\nOK\r\n");
  channel.ExpectCommand("AT+IPR=115200").Respond("OK\r\n");

  auto aligned = AlignLikeDevice(channel);

  ASSERT_TRUE(aligned.has_value());
  EXPECT_TRUE(channel.WasCommandSent("AT+IPR=115200"));
  EXPECT_EQ(channel.GetBaudRate(), 115200);
}

TEST(BaudAlignmentTest, LeavesAMatchingRateAlone) {
  MockAtChannel channel;
  channel.ExpectCommand("AT+IPR?").Respond("+IPR: 115200\r\nOK\r\n");

  auto aligned = AlignLikeDevice(channel);

  ASSERT_TRUE(aligned.has_value());
  EXPECT_EQ(*aligned, 115200);
  // The probe is asserted, not just its absence of effect: every other check
  // here would still hold if the scan stopped running altogether, which is the
  // shape of test that let the original defect live.
  EXPECT_EQ(CountCommands(channel, "AT+IPR?"), 1);
  EXPECT_FALSE(channel.WasCommandSent("AT+IPR="));
  EXPECT_EQ(channel.GetBaudRate(), 115200);
}

// The answer is not necessarily the first line: a URC can be folded into the
// same response ahead of it, which is why the rate is read by name.
//
// The rate reported is deliberately not the rate being probed. With the two
// equal, a reader that took the first line, or the last, and gave up would fall
// through to the candidate and produce the expected number anyway - the test
// would hold for the wrong reason. A rate that is neither forces the "+IPR"
// line to be the one that was read.
TEST(BaudAlignmentTest, ReadsTheRateByNameRatherThanByPosition) {
  MockAtChannel channel;
  channel.ExpectCommand("AT+IPR?")
      .Respond("+CEREG: 1\r\n+IPR: 460800\r\nOK\r\n");

  auto found = FindModuleBaudRate(channel, BaudRateCandidates(115200));

  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, 460800);
}

// "+IPR: 0" is the factory setting - automatic baud - rather than a rate the
// host could be moved to. The module answered here, so the scan stops, but what
// it hands back is the rate it was probed at. Returning the zero would put a
// rate the caller could pass straight to SetBaudRate in its hands.
TEST(BaudAlignmentTest, DoesNotReportAutomaticBaudAsARate) {
  MockAtChannel channel;
  channel.SetModuleBaudRate(921600);
  channel.SetBaudRate(921600);
  channel.ExpectCommand("AT+IPR?").Respond("+IPR: 0\r\nOK\r\n");

  auto found = FindModuleBaudRate(channel, BaudRateCandidates(921600));

  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, 921600);
  EXPECT_EQ(channel.GetBaudRate(), 921600);
}

// A module that answers but does not report a rate has been found. Its firmware
// may not implement AT+IPR? even though it answers commands, and treating that
// as "not found" would scan past the one rate that works, put the host back on a
// rate nothing is listening at, and leave the link silent - the same failure
// this whole negotiation exists to prevent, reached through another door.
TEST(BaudAlignmentTest, StopsAtARateThatAnswersWithoutReportingOne) {
  MockAtChannel channel;
  channel.SetModuleBaudRate(115200);
  // The mock's default answer: OK, and no +IPR line.
  ASSERT_EQ(channel.GetBaudRate(), 115200);

  // 921600 is a candidate that would answer nothing; reaching it would show as
  // a second probe.
  auto found = FindModuleBaudRate(channel, {115200, 921600});

  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(*found, 115200);
  EXPECT_EQ(channel.GetBaudRate(), 115200);
  EXPECT_EQ(CountCommands(channel, "AT+IPR?"), 1);
  EXPECT_FALSE(channel.WasCommandSent("AT+IPR="));
}

// A module that never answers the query is not a device that cannot be used: the
// firmware may not implement AT+IPR? at all, or may sit on a rate this library
// does not know to probe. The scan reports that it found nothing, and the
// channel is left where the caller configured it rather than on the last rate
// probed - a caller reading GetBaudRate() afterwards must not be told a rate the
// port was only passing through.
TEST(BaudAlignmentTest, LeavesTheChannelOnTheConfiguredRateWhenNothingAnswers) {
  MockAtChannel channel;
  channel.SetBaudRate(921600);
  channel.FailCommand("AT+IPR?");

  auto found = FindModuleBaudRate(channel, BaudRateCandidates(921600));

  EXPECT_FALSE(found.has_value());
  EXPECT_EQ(channel.GetBaudRate(), 921600);
  EXPECT_FALSE(channel.WasCommandSent("AT+IPR="));
}

// The same silence, reached from the default rate. Nothing is written either
// way: a device whose rate cannot be read is not a device to reconfigure, and
// the caller carries on - the identification that follows is what decides
// whether it is usable.
TEST(BaudAlignmentTest, FindsNothingWhenTheModuleWillNotReportItsRate) {
  MockAtChannel channel;
  channel.FailCommand("AT+IPR?");

  auto found = FindModuleBaudRate(channel, BaudRateCandidates(115200));

  EXPECT_FALSE(found.has_value());
  EXPECT_EQ(channel.GetBaudRate(), 115200);
  EXPECT_FALSE(channel.WasCommandSent("AT+IPR="));
}

TEST(BaudAlignmentTest, DoesNotProbeAChannelThatNamesNoRate) {
  MockAtChannel channel;
  channel.SetBaudRate(0);

  auto found = FindModuleBaudRate(channel, BaudRateCandidates(115200));

  EXPECT_FALSE(found.has_value());
  EXPECT_EQ(CountCommands(channel, "AT+IPR?"), 0);
}

// --- Setting the module's rate ---

// A rate the module does not offer silences it, which is worse than the slow
// link the caller was trying to improve on. The link is left on the rate the
// module is actually on, so a caller who asked for more than the module can do
// gets a working connection at the module's top rate rather than a dead port at
// the rate they asked for.
TEST(BaudAlignmentTest, DoesNotAskForARateTheModuleRejects) {
  MockAtChannel channel;
  channel.SetModuleBaudRate(115200);
  channel.SetBaudRate(921600);
  channel.ExpectCommand("AT+IPR?").Respond("+IPR: 115200\r\nOK\r\n");
  channel.ExpectCommand("AT+IPR=?")
      .Respond("+IPR: (1200,4800),(0,300,115200)\r\nOK\r\n");

  auto aligned = AlignLikeDevice(channel);

  ASSERT_TRUE(aligned.has_value());
  EXPECT_EQ(*aligned, 115200);
  EXPECT_FALSE(channel.WasCommandSent("AT+IPR=921600"));
  EXPECT_EQ(channel.GetBaudRate(), 115200);
}

// 115200 is a prefix of 1152000, so a substring search accepts a rate the module
// does not offer - and the module then goes silent.
TEST(BaudAlignmentTest, DoesNotMatchARateThatIsOnlyAPrefix) {
  MockAtChannel channel;
  channel.SetModuleBaudRate(921600);
  channel.SetBaudRate(921600);
  channel.ExpectCommand("AT+IPR=?")
      .Respond("+IPR: (921600,1152000),(0,921600,1152000)\r\nOK\r\n");

  auto aligned = SetModuleBaudRate(channel, 115200);

  ASSERT_TRUE(aligned.has_value());
  EXPECT_EQ(*aligned, 921600);
  const auto& sent = channel.SentCommands();
  EXPECT_EQ(std::find(sent.begin(), sent.end(), "AT+IPR=115200"), sent.end());
  EXPECT_EQ(channel.GetBaudRate(), 921600);
}

// The module does not answer the first command at a rate it has just switched
// to, observed at 921600 as a timeout on "AT". Reading that one lost probe as a
// refusal would drop back to the slow rate on every connect.
TEST(BaudAlignmentTest, RetriesAProbeLostToTheRateSwitch) {
  MockAtChannel channel;
  channel.SetModuleBaudRate(115200);
  channel.SetBaudRate(921600);
  channel.ExpectCommand("AT+IPR?")
      .RespondInTurn({"+IPR: 115200\r\nOK\r\n",    // before the switch
                      "OK\r\n",                    // the probe after it is lost
                      "+IPR: 921600\r\nOK\r\n"});  // answered on the retry
  channel.ExpectCommand("AT+IPR=?")
      .Respond("+IPR: (1200,115200),(0,300,115200,921600)\r\nOK\r\n");
  channel.ExpectCommand("AT+IPR=921600").Respond("OK\r\n");

  auto aligned = AlignLikeDevice(channel);

  ASSERT_TRUE(aligned.has_value());
  EXPECT_EQ(*aligned, 921600);
  EXPECT_EQ(channel.GetBaudRate(), 921600);
}

// Nothing confirms the new rate, so the pair is put back on the rate that was
// working. Abandoning the link instead would leave the caller with no device at
// all, which is worse than the slow rate it started on.
//
// This is also the case that rules out the obvious revert, which is to send the
// old rate and follow it down: here the module never took the write, so the two
// ends are apart and the command asking it to come back is never heard. It is
// the scan that puts them back together, by finding the module where it actually
// is rather than where it was told to go.
TEST(BaudAlignmentTest, ReturnsToTheWorkingRateWhenTheNewOneIsNotConfirmed) {
  MockAtChannel channel;
  channel.SetModuleBaudRate(115200);
  // Never reports the new rate, however often it is asked.
  channel.ExpectCommand("AT+IPR?").Respond("+IPR: 115200\r\nOK\r\n");
  channel.ExpectCommand("AT+IPR=?")
      .Respond("+IPR: (1200,115200),(0,300,115200,921600)\r\nOK\r\n");

  auto aligned = SetModuleBaudRate(channel, 921600);

  ASSERT_TRUE(aligned.has_value());
  EXPECT_EQ(*aligned, 115200);
  EXPECT_EQ(channel.GetBaudRate(), 115200);
  EXPECT_TRUE(channel.WasCommandSent("AT+IPR=115200"));
}

// The scan is what makes the revert truthful, so it is also the one step that
// can fail. A module that answers nothing at any rate by the end of a change
// leaves nothing that can be shown to work, and the caller is told that instead
// of being handed whichever rate the pair happened to be left on - the silent
// link, reported as a success.
TEST(BaudAlignmentTest, ReportsWhenTheModuleCannotBeFoundAfterAnUnconfirmedChange) {
  MockAtChannel channel;
  channel.ExpectCommand("AT+IPR=?")
      .Respond("+IPR: (1200,115200),(0,300,115200,921600)\r\nOK\r\n");
  // Answers everything except the query - so the write is taken, nothing can
  // confirm it, and the search afterwards has nothing to find at any candidate.
  channel.FailCommand("AT+IPR?");

  auto aligned = SetModuleBaudRate(channel, 921600);

  EXPECT_FALSE(aligned.has_value());
}

// Setting the rate with nothing to align to is not an error: the channel is
// already on a rate the module answers at, which is all this function needs.
TEST(BaudAlignmentTest, LeavesTheChannelAloneWhenItIsAlreadyOnTheRate) {
  MockAtChannel channel;
  channel.SetModuleBaudRate(115200);

  auto aligned = SetModuleBaudRate(channel, 115200);

  ASSERT_TRUE(aligned.has_value());
  EXPECT_EQ(*aligned, 115200);
  EXPECT_EQ(CountCommands(channel, "AT+IPR=?"), 0);
  EXPECT_FALSE(channel.WasCommandSent("AT+IPR="));
}

TEST(BaudAlignmentTest, DoesNotWriteWhenTheChannelNamesNoRate) {
  MockAtChannel channel;
  channel.SetBaudRate(0);

  auto aligned = SetModuleBaudRate(channel, 921600);

  EXPECT_FALSE(aligned.has_value());
  EXPECT_FALSE(channel.WasCommandSent("AT+IPR="));
}
