#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "at_channel/at_uart.h"
#include "platform/iuart.h"
#include "esp_modem_link/uart_config.h"

using namespace esp_modem_link;
using namespace esp_modem_link::at_channel;
using namespace esp_modem_link::platform;

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace {

class MockUart : public IUart {
 public:
  explicit MockUart(const UartConfig& config) : config_(config) {}

  void SetBaudRate(int baud) override { config_.baud_rate = baud; }
  int GetBaudRate() const override { return config_.baud_rate; }

  int Send(const void* data, size_t len) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    tx_buffer_.insert(tx_buffer_.end(), bytes, bytes + len);
    cv_tx_.notify_all();
    return static_cast<int>(len);
  }

  int Receive(void* buffer, size_t len,
              std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (rx_buffer_.empty()) {
      if (timeout == std::chrono::milliseconds::zero()) return 0;
      cv_rx_.wait_for(lock, timeout, [&] { return !rx_buffer_.empty(); });
    }
    size_t to_read = std::min(len, rx_buffer_.size());
    if (to_read == 0) return 0;
    std::memcpy(buffer, rx_buffer_.data(), to_read);
    rx_buffer_.erase(rx_buffer_.begin(), rx_buffer_.begin() + to_read);
    return static_cast<int>(to_read);
  }

  bool SupportsDma() const override { return false; }
  void StartDmaRx(size_t, DmaRxCallback) override {}
  void StopDmaRx() override {}
  void Flush() override {
    std::lock_guard<std::mutex> lock(mutex_);
    rx_buffer_.clear();
    tx_buffer_.clear();
  }

  void InjectRx(const std::string& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    rx_buffer_.insert(rx_buffer_.end(), data.begin(), data.end());
    cv_rx_.notify_all();
  }

  std::string GetTxData() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string result(tx_buffer_.begin(), tx_buffer_.end());
    tx_buffer_.clear();
    return result;
  }

  size_t GetTxSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tx_buffer_.size();
  }

  void WaitForTx(size_t min_bytes, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_tx_.wait_for(lock, timeout,
                    [&] { return tx_buffer_.size() >= min_bytes; });
  }

 private:
  UartConfig config_;
  mutable std::mutex mutex_;
  std::condition_variable cv_rx_;
  std::condition_variable cv_tx_;
  std::vector<uint8_t> rx_buffer_;
  std::vector<uint8_t> tx_buffer_;
};

class AtUartBasicTest : public ::testing::Test {
 protected:
  void SetUp() override {
    UartConfig cfg;
    cfg.baud_rate = 115200;
    cfg.rx_buffer_size = 4096;
    auto mock = std::make_unique<MockUart>(cfg);
    mock_uart_ = mock.get();
    at_uart_ = std::make_unique<AtUart>(*mock, cfg);
    mock_uart_owned_ = std::move(mock);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  void TearDown() override {
    at_uart_.reset();
    mock_uart_owned_.reset();
    mock_uart_ = nullptr;
  }

  MockUart* mock_uart_ = nullptr;
  std::unique_ptr<MockUart> mock_uart_owned_;
  std::unique_ptr<AtUart> at_uart_;
};

}  // namespace

TEST_F(AtUartBasicTest, SendCommandOk) {
  // Kick off the command in a separate thread, then inject the response
  std::thread cmd_thread([&] {
    auto result = at_uart_->SendCommand("AT", std::chrono::milliseconds(500));
    EXPECT_TRUE(result.has_value());
  });

  // Wait for AT to be sent
  mock_uart_->WaitForTx(4, std::chrono::milliseconds(200));
  std::string tx = mock_uart_->GetTxData();
  EXPECT_NE(tx.find("AT\r\n"), std::string::npos);

  // Inject response: echo + OK
  mock_uart_->InjectRx("AT\r\nOK\r\n");

  cmd_thread.join();
}

TEST_F(AtUartBasicTest, SendCommandError) {
  std::thread cmd_thread([&] {
    auto result = at_uart_->SendCommand("AT+BAD", std::chrono::milliseconds(500));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, AtErrc::kCommandError);
  });

  mock_uart_->WaitForTx(8, std::chrono::milliseconds(200));
  mock_uart_->InjectRx("AT+BAD\r\nERROR\r\n");
  cmd_thread.join();
}

TEST_F(AtUartBasicTest, SendCommandTimeout) {
  auto result = at_uart_->SendCommand("AT", std::chrono::milliseconds(50));
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, AtErrc::kTimeout);
}

TEST_F(AtUartBasicTest, SendCommandWithCmeError) {
  std::thread cmd_thread([&] {
    auto result = at_uart_->SendCommand("AT+CFUN=1", std::chrono::milliseconds(500));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, AtErrc::kCmeError);
    EXPECT_EQ(result.error().cme, 13);
  });

  mock_uart_->WaitForTx(12, std::chrono::milliseconds(200));
  mock_uart_->InjectRx("AT+CFUN=1\r\n+CME ERROR: 13\r\n");
  cmd_thread.join();
}

TEST_F(AtUartBasicTest, CommandWithIntermediateLines) {
  std::thread cmd_thread([&] {
    auto result = at_uart_->SendCommand("AT+CGMM", std::chrono::milliseconds(500));
    EXPECT_TRUE(result.has_value());

    auto lines = at_uart_->GetResponseLines();
    ASSERT_EQ(lines.size(), 1);
    EXPECT_EQ(lines[0], "ML307R-DNLM");
  });

  mock_uart_->WaitForTx(10, std::chrono::milliseconds(200));
  mock_uart_->InjectRx("AT+CGMM\r\nML307R-DNLM\r\nOK\r\n");
  cmd_thread.join();
}

TEST_F(AtUartBasicTest, UrcWhileIdle) {
  bool urc_received = false;
  std::string cmd;
  std::string args;
  auto handle = at_uart_->SubscribeUrc(
      "+CSQ", [&](std::string_view c, std::string_view a) {
        urc_received = true;
        cmd = std::string(c);
        args = std::string(a);
      });

  // Inject URC while idle (no command in progress)
  mock_uart_->InjectRx("+CSQ: 31,99\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  EXPECT_TRUE(urc_received);
  EXPECT_EQ(cmd, "+CSQ");
  EXPECT_EQ(args, "31,99");

  at_uart_->UnsubscribeUrc(handle);
}

// A URC that lands while a command is in flight is delivered, not swallowed.
//
// It used to reach nobody. AtUart folded a mid-command line into that command's
// response buffer and skipped the handlers on account of the command being in
// progress, so a socket event arriving during a send was never seen - and a
// client could wait out a timeout for data that had already arrived. Two bugs
// were archived against that, and ML307 carried two separate workarounds for it.
//
// The line still belongs in the response as well: the command's own parsing
// depends on the buffer's shape. Both have to happen.
TEST_F(AtUartBasicTest, UrcDuringCommandReachesTheHandler) {
  std::atomic<bool> command_returned{false};
  bool urc_seen = false;
  bool urc_beat_the_command = false;
  std::string cmd;
  std::string args;

  auto handle = at_uart_->SubscribeUrc(
      "+CSQ", [&](std::string_view c, std::string_view a) {
        urc_seen = true;
        urc_beat_the_command = !command_returned.load();
        cmd = std::string(c);
        args = std::string(a);
      });

  std::thread cmd_thread([&] {
    auto result =
        at_uart_->SendCommand("AT+CGMM", std::chrono::milliseconds(500));
    EXPECT_TRUE(result.has_value());

    auto lines = at_uart_->GetResponseLines();
    EXPECT_NE(std::find(lines.begin(), lines.end(), "+CSQ: 31,99"),
              lines.end())
        << "the URC must still be part of the command's response";

    command_returned = true;
  });

  mock_uart_->WaitForTx(10, std::chrono::milliseconds(200));
  mock_uart_->InjectRx("AT+CGMM\r\n+CSQ: 31,99\r\nML307R-DNLM\r\nOK\r\n");
  cmd_thread.join();

  EXPECT_TRUE(urc_seen);
  EXPECT_EQ(cmd, "+CSQ");
  EXPECT_EQ(args, "31,99");
  // The point of the test: the handler ran while the command was still in
  // flight, which is exactly the case that used to skip it.
  EXPECT_TRUE(urc_beat_the_command);

  at_uart_->UnsubscribeUrc(handle);
}

TEST_F(AtUartBasicTest, UrcUnsubscribe) {
  int count = 0;
  auto handle = at_uart_->SubscribeUrc(
      "+CSQ", [&](std::string_view, std::string_view) { count++; });

  mock_uart_->InjectRx("+CSQ: 31,99\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(count, 1);

  at_uart_->UnsubscribeUrc(handle);
  mock_uart_->InjectRx("+CSQ: 30,99\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  EXPECT_EQ(count, 1);
}

TEST_F(AtUartBasicTest, SetBaudRate) {
  at_uart_->SetBaudRate(9600);
  EXPECT_EQ(at_uart_->GetBaudRate(), 9600);
  EXPECT_EQ(mock_uart_->GetBaudRate(), 9600);
}

// A rate change drops whatever is half-assembled in the receiver. Those bytes
// were clocked at the old rate, and left in place they are glued to the front of
// the next real line - one line assembled from two rates, which parses as
// neither. This runs on the critical path of every Detect and Create now, since
// the rate negotiation sits there.
//
// A URC is the observable case, and the one that matters: it is the only thing
// arriving here with nothing behind it to retry. A swallowed "+CSQ" never
// arrives again, whereas a command's answer that is lost to a rate change costs
// a retry that the alignment already performs.
TEST_F(AtUartBasicTest, RateChangeDropsAHalfReceivedLine) {
  int calls = 0;
  std::string args;
  at_uart_->SubscribeUrc("+CSQ", [&](std::string_view, std::string_view a) {
    ++calls;
    args = std::string(a);
  });

  // A line with no terminator behind it yet: the receiver is holding it.
  mock_uart_->InjectRx("+CS");
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  at_uart_->SetBaudRate(9600);
  // Long enough for the receive task to come out of its read and act on the
  // change before the next line arrives.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  mock_uart_->InjectRx("+CSQ: 31,99\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // Without the discard the line arrives as "+CS+CSQ: 31,99", matches no
  // subscription, and is dropped as an unrecognised line.
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(args, "31,99");
}

TEST_F(AtUartBasicTest, SendRaw) {
  const char data[] = "hello";
  auto result = at_uart_->SendRaw(data, 5);
  EXPECT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 5);

  std::string tx = mock_uart_->GetTxData();
  EXPECT_EQ(tx, "hello");
}

TEST_F(AtUartBasicTest, EnterDataModeNotSupportedByDefault) {
  auto result = at_uart_->EnterDataMode(0, std::chrono::milliseconds(50));
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, AtErrc::kNotInitialized);
}

TEST_F(AtUartBasicTest, ExitDataModeNotSupportedByDefault) {
  auto result = at_uart_->ExitDataMode(std::chrono::milliseconds(50));
  EXPECT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, AtErrc::kNotInitialized);
}

TEST_F(AtUartBasicTest, DebugLogToggle) {
  EXPECT_FALSE(at_uart_->IsDebugLogEnabled());
  at_uart_->SetDebugLog(true);
  EXPECT_TRUE(at_uart_->IsDebugLogEnabled());
  at_uart_->SetDebugLog(false);
  EXPECT_FALSE(at_uart_->IsDebugLogEnabled());
}

TEST_F(AtUartBasicTest, MultipleCommandsSequential) {
  // First command
  std::thread t1([&] {
    auto r = at_uart_->SendCommand("AT+CMD1", std::chrono::milliseconds(500));
    EXPECT_TRUE(r.has_value());
  });
  mock_uart_->WaitForTx(10, std::chrono::milliseconds(200));
  mock_uart_->InjectRx("AT+CMD1\r\nOK\r\n");
  t1.join();

  // Second command
  std::thread t2([&] {
    auto r = at_uart_->SendCommand("AT+CMD2", std::chrono::milliseconds(500));
    EXPECT_FALSE(r.has_value());
  });
  mock_uart_->WaitForTx(10, std::chrono::milliseconds(200));
  mock_uart_->InjectRx("AT+CMD2\r\nERROR\r\n");
  t2.join();
}

TEST_F(AtUartBasicTest, SendCommandWithData) {
  std::thread cmd_thread([&] {
    std::string data = "HelloWorld";
    auto result = at_uart_->SendCommandWithData(
        "AT+MIPSEND=1,10", data.data(), data.size(),
        std::chrono::milliseconds(500), "\x1a");
    EXPECT_TRUE(result.has_value());
  });

  // Wait for the command prefix
  mock_uart_->WaitForTx(18, std::chrono::milliseconds(200));
  std::string tx1 = mock_uart_->GetTxData();
  EXPECT_NE(tx1.find("AT+MIPSEND=1,10"), std::string::npos);

  // Send data prompt ">"
  mock_uart_->InjectRx(">\r\n");

  // The AtUart should now send data + Ctrl+Z
  mock_uart_->WaitForTx(11, std::chrono::milliseconds(200));
  std::string tx2 = mock_uart_->GetTxData();
  EXPECT_NE(tx2.find("HelloWorld"), std::string::npos);
  EXPECT_NE(tx2.find("\x1a"), std::string::npos);

  // Send final OK
  mock_uart_->InjectRx("SEND OK\r\n");
  mock_uart_->InjectRx("OK\r\n");

  cmd_thread.join();
}

// The commands that end in an event rather than a response line - "CONNECT OK",
// "CLOSE OK", "SHUT OK" - have nothing to wait for here. Waiting would also be
// wrong: the event is matched against a cid by the caller, and only the caller
// can tell a late one from this one's.
TEST_F(AtUartBasicTest, SendLineWritesTheCommandAndDoesNotWait) {
  auto result = at_uart_->SendLine("AT+CIPSTART=0,\"TCP\",\"h\",80");
  ASSERT_TRUE(result.has_value());

  mock_uart_->WaitForTx(30, std::chrono::milliseconds(200));
  EXPECT_EQ(mock_uart_->GetTxData(),
            "AT+CIPSTART=0,\"TCP\",\"h\",80\r\n");
}

// AIR780E answers AT+CIPSEND with "\r\n> " - a trailing space and no CRLF. Those
// bytes can never form a complete line, so the prompt has to be recognised in
// the partial buffer; and because no CRLF follows, the payload goes out before
// any terminator exists.
TEST_F(AtUartBasicTest, SendDataAfterPromptWritesThePayloadOnAPromptWithTrailingSpace) {
  std::thread cmd_thread([&] {
    auto result = at_uart_->SendDataAfterPrompt(
        "AT+CIPSEND=0,5", "hello", 5, std::chrono::milliseconds(500));
    EXPECT_TRUE(result.has_value());
  });

  mock_uart_->WaitForTx(15, std::chrono::milliseconds(200));
  mock_uart_->InjectRx("\r\n> ");
  cmd_thread.join();

  // The command, then the payload, and nothing else: the trailing
  // acknowledgement is still the caller's to match.
  EXPECT_EQ(mock_uart_->GetTxData(), "AT+CIPSEND=0,5\r\nhello");

  // And the prompt was consumed rather than left to become a line of the next
  // response. A "> " line is not blank, so it would survive the blank-line skip
  // and be parsed as an answer.
  EXPECT_TRUE(at_uart_->GetResponseLines().empty());
}

// A refusal arrives instead of a prompt. Two things must happen: the caller
// hears the module's own reason rather than a timeout it has to guess at, and
// no payload is written - a module that did not prompt is not collecting bytes,
// so anything sent now is read as the front of the next command line.
TEST_F(AtUartBasicTest, SendDataAfterPromptWritesNothingWhenTheModuleRefuses) {
  std::thread cmd_thread([&] {
    auto result = at_uart_->SendDataAfterPrompt(
        "AT+CIPSEND=0,5", "hello", 5, std::chrono::milliseconds(500));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, AtErrc::kCmeError);
    EXPECT_EQ(result.error().cme, 3);
  });

  mock_uart_->WaitForTx(15, std::chrono::milliseconds(200));
  mock_uart_->InjectRx("\r\n+CME ERROR: 3\r\n");
  cmd_thread.join();

  EXPECT_EQ(mock_uart_->GetTxData(), "AT+CIPSEND=0,5\r\n");
}

// A prompt belonging to a command that already gave up must not stand in for the
// next command's prompt. If it did, the wait would report success and write a
// payload into a module that is not collecting one.
TEST_F(AtUartBasicTest, AStalePromptDoesNotSatisfyTheNextPayloadWait) {
  auto first = at_uart_->SendDataAfterPrompt(
      "AT+CIPSEND=0,1", "x", 1, std::chrono::milliseconds(50));
  ASSERT_FALSE(first.has_value());

  // The prompt for that command turns up late, with nobody waiting for it.
  mock_uart_->InjectRx("\r\n> ");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  auto second = at_uart_->SendDataAfterPrompt(
      "AT+CIPSEND=0,1", "y", 1, std::chrono::milliseconds(50));
  EXPECT_FALSE(second.has_value());
}
