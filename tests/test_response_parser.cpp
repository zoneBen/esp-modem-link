#include <gtest/gtest.h>

#include "at_channel/response_parser.h"

using namespace esp_modem_link::at_channel;
using esp_modem_link::AtErrc;

TEST(ResponseParserTest, OkOnly) {
  auto r = ParseResponse("OK\r\n");
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.lines.empty());
}

TEST(ResponseParserTest, OkWithEcho) {
  auto r = ParseResponse("AT\r\nOK\r\n");
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.lines.empty());
}

TEST(ResponseParserTest, ErrorOnly) {
  auto r = ParseResponse("ERROR\r\n");
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.error.code, AtErrc::kCommandError);
}

TEST(ResponseParserTest, CmeErrorNumeric) {
  auto r = ParseResponse("+CME ERROR: 10\r\n");
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.error.code, AtErrc::kCmeError);
  EXPECT_EQ(r.error.cme, 10);
}

TEST(ResponseParserTest, CmeErrorText) {
  auto r = ParseResponse("+CME ERROR: SIM not inserted\r\n");
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.error.code, AtErrc::kCmeError);
}

TEST(ResponseParserTest, CmsErrorNumeric) {
  auto r = ParseResponse("+CMS ERROR: 500\r\n");
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.error.code, AtErrc::kCmsError);
  EXPECT_EQ(r.error.cms, 500);
}

TEST(ResponseParserTest, DataPrompt) {
  auto r = ParseResponse(">\r\n");
  EXPECT_TRUE(r.ok);
}

TEST(ResponseParserTest, IntermediateLinesBeforeOk) {
  auto r = ParseResponse("AT+CGMM\r\nML307R-DNLM\r\nOK\r\n");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.lines.size(), 1);
  EXPECT_EQ(r.lines[0], "ML307R-DNLM");
}

TEST(ResponseParserTest, MultipleIntermediateLines) {
  auto r = ParseResponse(
      "AT+COPS=?\r\n"
      "+COPS: (1,\"China Mobile\",\"CMCC\",\"46000\",7),\r\n"
      "(1,\"China Unicom\",\"CUCC\",\"46001\",7),\r\n"
      "OK\r\n");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.lines.size(), 2);
  EXPECT_NE(r.lines[0].find("China Mobile"), std::string::npos);
  EXPECT_NE(r.lines[1].find("China Unicom"), std::string::npos);
}

TEST(ResponseParserTest, ColonValueLine) {
  auto r = ParseResponse("AT+CSQ\r\n+CSQ: 31,99\r\nOK\r\n");
  EXPECT_TRUE(r.ok);
  ASSERT_EQ(r.lines.size(), 1);
  EXPECT_EQ(r.lines[0], "+CSQ: 31,99");
}

TEST(ResponseParserTest, EmptyResponse) {
  auto r = ParseResponse("");
  EXPECT_FALSE(r.ok);
  EXPECT_TRUE(r.lines.empty());
}

TEST(ResponseParserTest, WhitespaceOnlyLines) {
  auto r = ParseResponse("\r\n\r\nOK\r\n");
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.lines.empty());
}

TEST(ResponseParserTest, LowercaseAtEcho) {
  auto r = ParseResponse("at\r\nOK\r\n");
  EXPECT_TRUE(r.ok);
  EXPECT_TRUE(r.lines.empty());
}

TEST(ResponseParserTest, NoFinalStatus) {
  auto r = ParseResponse("+CSQ: 31,99\r\n");
  EXPECT_FALSE(r.ok);
  ASSERT_EQ(r.lines.size(), 1);
  EXPECT_EQ(r.lines[0], "+CSQ: 31,99");
}

TEST(ResponseParserTest, CmeErrorWithEchoAndData) {
  auto r = ParseResponse("AT+CFUN=1\r\n+CME ERROR: 13\r\n");
  EXPECT_FALSE(r.ok);
  EXPECT_EQ(r.error.code, AtErrc::kCmeError);
  EXPECT_EQ(r.error.cme, 13);
  EXPECT_TRUE(r.lines.empty());
}
