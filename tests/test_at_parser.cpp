#include <gtest/gtest.h>

#include "at_parser/at_parser.h"

using namespace esp_modem_link::at_parser;

TEST(AtParserTest, SplitCsvSimple) {
  auto fields = SplitCsv("1,2,3,4");
  ASSERT_EQ(fields.size(), 4);
  EXPECT_EQ(fields[0], "1");
  EXPECT_EQ(fields[1], "2");
  EXPECT_EQ(fields[2], "3");
  EXPECT_EQ(fields[3], "4");
}

TEST(AtParserTest, SplitCsvQuotedField) {
  auto fields = SplitCsv("\"hello\",world");
  ASSERT_EQ(fields.size(), 2);
  EXPECT_EQ(fields[0], "hello");
  EXPECT_EQ(fields[1], "world");
}

TEST(AtParserTest, SplitCsvQuotedWithComma) {
  auto fields = SplitCsv("\"a,b,c\",d");
  ASSERT_EQ(fields.size(), 2);
  EXPECT_EQ(fields[0], "a,b,c");
  EXPECT_EQ(fields[1], "d");
}

TEST(AtParserTest, SplitCsvEmptyField) {
  auto fields = SplitCsv("a,,b");
  ASSERT_EQ(fields.size(), 3);
  EXPECT_EQ(fields[0], "a");
  EXPECT_EQ(fields[1], "");
  EXPECT_EQ(fields[2], "b");
}

TEST(AtParserTest, SplitCsvEmptyString) {
  auto fields = SplitCsv("");
  ASSERT_EQ(fields.size(), 1);
  EXPECT_EQ(fields[0], "");
}

TEST(AtParserTest, StripQuotesWithQuotes) {
  EXPECT_EQ(StripQuotes("\"hello\""), "hello");
}

TEST(AtParserTest, StripQuotesNoQuotes) {
  EXPECT_EQ(StripQuotes("hello"), "hello");
}

TEST(AtParserTest, StripQuotesSingleQuote) {
  EXPECT_EQ(StripQuotes("\"hello"), "\"hello");
}

TEST(AtParserTest, ParseIntValid) {
  auto val = ParseInt("123");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, 123);
}

TEST(AtParserTest, ParseIntNegative) {
  auto val = ParseInt("-456");
  ASSERT_TRUE(val.has_value());
  EXPECT_EQ(*val, -456);
}

TEST(AtParserTest, ParseIntInvalid) {
  EXPECT_FALSE(ParseInt("abc").has_value());
  EXPECT_FALSE(ParseInt("12a").has_value());
  EXPECT_FALSE(ParseInt("").has_value());
}

TEST(AtParserTest, ParseDoubleValid) {
  auto val = ParseDouble("3.14");
  ASSERT_TRUE(val.has_value());
  EXPECT_NEAR(*val, 3.14, 0.001);
}

TEST(AtParserTest, ParseDoubleInvalid) {
  EXPECT_FALSE(ParseDouble("abc").has_value());
  EXPECT_FALSE(ParseDouble("").has_value());
}

TEST(AtParserTest, ParseKeyValueBasic) {
  std::string_view key, value;
  EXPECT_TRUE(ParseKeyValue("+CGMM: ML307", key, value));
  EXPECT_EQ(key, "+CGMM");
  EXPECT_EQ(value, "ML307");
}

TEST(AtParserTest, ParseKeyValueWithSpaces) {
  std::string_view key, value;
  EXPECT_TRUE(ParseKeyValue("+CSQ: 31,99", key, value));
  EXPECT_EQ(key, "+CSQ");
  EXPECT_EQ(value, "31,99");
}

TEST(AtParserTest, ParseKeyValueNoColon) {
  std::string_view key, value;
  EXPECT_FALSE(ParseKeyValue("hello world", key, value));
}

TEST(AtParserTest, ParseHexEmpty) {
  auto result = ParseHex("");
  EXPECT_TRUE(result.empty());
}

TEST(AtParserTest, ParseHexEvenLength) {
  auto result = ParseHex("48656c6c6f");
  ASSERT_EQ(result.size(), 5);
  EXPECT_EQ(result[0], 0x48);
  EXPECT_EQ(result[1], 0x65);
  EXPECT_EQ(result[2], 0x6c);
  EXPECT_EQ(result[3], 0x6c);
  EXPECT_EQ(result[4], 0x6f);
}

TEST(AtParserTest, ParseHexUpperCase) {
  auto result = ParseHex("FF00AA");
  ASSERT_EQ(result.size(), 3);
  EXPECT_EQ(result[0], 0xFF);
  EXPECT_EQ(result[1], 0x00);
  EXPECT_EQ(result[2], 0xAA);
}

TEST(AtParserTest, ParseIntListBasic) {
  auto result = ParseIntList("1,2,3,4");
  ASSERT_EQ(result.size(), 4);
  EXPECT_EQ(result[0], 1);
  EXPECT_EQ(result[1], 2);
  EXPECT_EQ(result[2], 3);
  EXPECT_EQ(result[3], 4);
}

TEST(AtParserTest, ParseIntListSkipsNonNumeric) {
  auto result = ParseIntList("1,abc,3");
  ASSERT_EQ(result.size(), 2);
  EXPECT_EQ(result[0], 1);
  EXPECT_EQ(result[1], 3);
}

// The size line of a chunked body is a number in hex, which is what separates
// this from EncodeHex: that renders the bytes of a value, so the same 10 comes
// back as "0a" from there and "a" from here.
TEST(AtParserTest, ToHexStringRendersAnIntegerNotItsBytes) {
  EXPECT_EQ(ToHexString(10), "a");
  EXPECT_EQ(ToHexString(255), "ff");
  EXPECT_EQ(ToHexString(4096), "1000");
}

// Zero is the one value where a single digit is right rather than a leading-zero
// artefact, and it is the value that has to come out exactly right: it is the
// terminator of every chunked body.
TEST(AtParserTest, ToHexStringRendersZeroAsASingleDigit) {
  EXPECT_EQ(ToHexString(0), "0");
}

TEST(AtParserTest, ToHexStringHasNoLeadingZeros) {
  EXPECT_EQ(ToHexString(1), "1");
  EXPECT_EQ(ToHexString(16), "10");
  EXPECT_EQ(ToHexString(0x100), "100");
}

TEST(AtParserTest, ToHexStringUsesLowercase) {
  EXPECT_EQ(ToHexString(0xabcdef), "abcdef");
}

// A four-gigabyte chunk is not a thing anyone sends, but the top of the range is
// where the shift loop would break first if it used a signed type.
TEST(AtParserTest, ToHexStringHandlesTheTopOfTheRange) {
  EXPECT_EQ(ToHexString(0xffffffffffffffffULL), "ffffffffffffffff");
}
