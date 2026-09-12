#include <gtest/gtest.h>

#include "esp_modem_link/at_error.h"
#include "esp_modem_link/network_error.h"

using namespace esp_modem_link;

TEST(NetworkErrorTest, DefaultConstruction) {
  NetworkError err(NetworkErrc::kTimeout);
  EXPECT_EQ(err.code, NetworkErrc::kTimeout);
  EXPECT_EQ(err.native, 0);
  EXPECT_TRUE(err.context.empty());
}

TEST(NetworkErrorTest, FullConstruction) {
  NetworkError err(NetworkErrc::kDnsFailed, 5, "host=example.com");
  EXPECT_EQ(err.code, NetworkErrc::kDnsFailed);
  EXPECT_EQ(err.native, 5);
  EXPECT_EQ(err.context, "host=example.com");
}

TEST(NetworkErrorTest, NameIsNotEmpty) {
  NetworkError err(NetworkErrc::kTimeout);
  EXPECT_STREQ(err.Name(), "timeout");
}

TEST(NetworkErrorTest, MessageIsNotEmpty) {
  NetworkError err(NetworkErrc::kTimeout);
  EXPECT_STREQ(err.Message(), "Operation timed out");
}

TEST(NetworkErrorTest, ToStringContainsNameAndMessage) {
  NetworkError err(NetworkErrc::kTimeout, 0, "ctx");
  auto s = err.ToString();
  EXPECT_NE(s.find("timeout"), std::string::npos);
  EXPECT_NE(s.find("timed out"), std::string::npos);
  EXPECT_NE(s.find("ctx"), std::string::npos);
}

TEST(NetworkErrorTest, ToStringWithNativeCode) {
  NetworkError err(NetworkErrc::kDnsFailed, 123);
  auto s = err.ToString();
  EXPECT_NE(s.find("123"), std::string::npos);
}

TEST(NetworkErrorTest, StaticTimeoutFactory) {
  auto err = NetworkError::Timeout("test op");
  EXPECT_EQ(err.code, NetworkErrc::kTimeout);
  EXPECT_EQ(err.context, "test op");
}

TEST(NetworkErrorTest, StaticNotSupportedFactory) {
  auto err = NetworkError::NotSupported("feature");
  EXPECT_EQ(err.code, NetworkErrc::kNotSupported);
  EXPECT_EQ(err.context, "feature");
}

TEST(NetworkErrorTest, StaticDnsFailedFactory) {
  auto err = NetworkError::DnsFailed(3, "example.com");
  EXPECT_EQ(err.code, NetworkErrc::kDnsFailed);
  EXPECT_EQ(err.native, 3);
  EXPECT_NE(err.context.find("example.com"), std::string::npos);
}

TEST(ResultTest, OkValue) {
  Result<int> r = 42;
  EXPECT_TRUE(r.has_value());
  EXPECT_EQ(*r, 42);
}

TEST(ResultTest, ErrorValue) {
  Result<int> r =
      std::unexpected(NetworkError(NetworkErrc::kTimeout));
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, NetworkErrc::kTimeout);
}

TEST(ResultTest, VoidOk) {
  Result<> r;
  EXPECT_TRUE(r.has_value());
}

TEST(ResultTest, VoidError) {
  Result<> r =
      std::unexpected(NetworkError(NetworkErrc::kNotSupported));
  EXPECT_FALSE(r.has_value());
}

TEST(AtErrorTest, BasicConstruction) {
  AtError err(AtErrc::kCommandError, "AT+X");
  EXPECT_EQ(err.code, AtErrc::kCommandError);
  EXPECT_EQ(err.context, "AT+X");
  EXPECT_EQ(err.cme, 0);
  EXPECT_EQ(err.cms, 0);
}

TEST(AtErrorTest, CmeErrorConstruction) {
  AtError err(AtErrc::kCmeError, 10, "sim error");
  EXPECT_EQ(err.code, AtErrc::kCmeError);
  EXPECT_EQ(err.cme, 10);
  EXPECT_EQ(err.cms, 0);
}

TEST(AtErrorTest, CmsErrorConstruction) {
  AtError err(AtErrc::kCmsError, 500, "sms error");
  EXPECT_EQ(err.code, AtErrc::kCmsError);
  EXPECT_EQ(err.cms, 500);
  EXPECT_EQ(err.cme, 0);
}

TEST(AtErrorTest, ToNetworkErrorMapsCorrectly) {
  EXPECT_EQ(AtError(AtErrc::kTimeout).ToNetworkError().code,
            NetworkErrc::kAtTimeout);
  EXPECT_EQ(AtError(AtErrc::kCommandError).ToNetworkError().code,
            NetworkErrc::kAtCommandError);
  EXPECT_EQ(AtError(AtErrc::kCmeError, 5).ToNetworkError().code,
            NetworkErrc::kAtCmeError);
  EXPECT_EQ(AtError(AtErrc::kCmsError, 5).ToNetworkError().code,
            NetworkErrc::kAtCmsError);
}

TEST(AtErrorTest, ToStringIsNonEmpty) {
  AtError err(AtErrc::kTimeout);
  EXPECT_FALSE(err.ToString().empty());
}

TEST(AtResultTest, Ok) {
  AtResult r;
  EXPECT_TRUE(r.has_value());
}

TEST(AtResultTest, Error) {
  AtResult r = std::unexpected(AtError(AtErrc::kCommandError));
  EXPECT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code, AtErrc::kCommandError);
}

TEST(AtValueTest, OkValue) {
  AtValue<int> r = 100;
  EXPECT_TRUE(r.has_value());
  EXPECT_EQ(*r, 100);
}
