// Lightweight smoke test - no gtest dependency
// Compile: cl /std:c++23 /EHsc /I include /I src tests/smoke_test.cpp src/error/network_error.cpp src/error/at_error.cpp src/at_parser/at_parser.cpp
#include <cassert>
#include <cstdio>
#include <string_view>

#include "esp_modem_link/at_error.h"
#include "esp_modem_link/network_error.h"
#include "at_parser/at_parser.h"

using namespace esp_modem_link;

int tests_passed = 0;
int tests_failed = 0;

#define TEST(name) do { printf("  %s... ", name); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); tests_failed++; } while(0)

void test_network_error() {
  TEST("NetworkError construction");
  NetworkError err(NetworkErrc::kTimeout, 0, "test");
  if (err.code == NetworkErrc::kTimeout && err.context == "test") PASS();
  else FAIL("wrong values");

  TEST("NetworkError Name/Message");
  NetworkError err2(NetworkErrc::kDnsFailed, 3);
  if (std::string_view(err2.Name()) == "dns_failed") PASS();
  else FAIL("wrong name");

  TEST("Result<int> ok");
  Result<int> r = 42;
  if (r.has_value() && *r == 42) PASS();
  else FAIL("wrong result");

  TEST("Result<int> error");
  Result<int> r2 = std::unexpected(NetworkError(NetworkErrc::kTimeout));
  if (!r2.has_value() && r2.error().code == NetworkErrc::kTimeout) PASS();
  else FAIL("wrong error");

  TEST("Result<void> ok");
  Result<> rv;
  if (rv.has_value()) PASS();
  else FAIL("void result should be ok");
}

void test_at_error() {
  TEST("AtError basic");
  AtError err(AtErrc::kCommandError, "AT+X");
  if (err.code == AtErrc::kCommandError && err.context == "AT+X") PASS();
  else FAIL("wrong values");

  TEST("AtError CME");
  AtError err2(AtErrc::kCmeError, 10);
  if (err2.cme == 10 && err2.cms == 0) PASS();
  else FAIL("wrong cme/cms");

  TEST("AtError to NetworkError");
  auto ne = AtError(AtErrc::kTimeout).ToNetworkError();
  if (ne.code == NetworkErrc::kAtTimeout) PASS();
  else FAIL("wrong mapping");

  TEST("AtResult ok/error");
  AtResult r;
  if (r.has_value()) PASS();
  else FAIL("AtResult should be ok");
}

void test_at_parser() {
  using namespace at_parser;

  TEST("SplitCsv simple");
  auto fields = SplitCsv("1,2,3");
  if (fields.size() == 3 && fields[0] == "1" && fields[2] == "3") PASS();
  else FAIL("wrong split");

  TEST("SplitCsv quoted");
  auto fields2 = SplitCsv("\"hello,world\",foo");
  if (fields2.size() == 2 && fields2[0] == "hello,world") PASS();
  else FAIL("quoted split failed");

  TEST("StripQuotes");
  if (StripQuotes("\"abc\"") == "abc") PASS();
  else FAIL("strip quotes failed");

  TEST("ParseInt valid");
  auto v = ParseInt("123");
  if (v && *v == 123) PASS();
  else FAIL("parse int failed");

  TEST("ParseInt invalid");
  auto v2 = ParseInt("abc");
  if (!v2) PASS();
  else FAIL("should fail");

  TEST("ParseKeyValue");
  std::string_view k, val;
  bool ok = ParseKeyValue("+CSQ: 31,99", k, val);
  if (ok && k == "+CSQ" && val == "31,99") PASS();
  else FAIL("key value failed");

  TEST("ParseHex");
  auto hex = ParseHex("48656c6c6f");
  if (hex.size() == 5 && hex[0] == 0x48 && hex[4] == 0x6f) PASS();
  else FAIL("parse hex failed");

  TEST("ParseIntList");
  auto list = ParseIntList("1,2,abc,4");
  if (list.size() == 3 && list[0] == 1 && list[2] == 4) PASS();
  else FAIL("parse int list failed");
}

int main() {
  printf("=== Smoke Tests ===\n\n");

  printf("NetworkError:\n");
  test_network_error();

  printf("\nAtError:\n");
  test_at_error();

  printf("\nAtParser:\n");
  test_at_parser();

  printf("\n=== Results: %d passed, %d failed ===\n",
         tests_passed, tests_failed);
  return tests_failed > 0 ? 1 : 0;
}
