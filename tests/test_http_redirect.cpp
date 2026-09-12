#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "protocol/http/http_redirect.h"

using namespace esp_modem_link;
using namespace esp_modem_link::protocol;

namespace {

// The URL every resolution case starts from: a directory, a query, and no
// default port to hide behind.
ParsedUrl BaseUrl() {
  auto parsed = ParseUrl("http://example.com:8080/a/b?x=1");
  EXPECT_TRUE(parsed.has_value());
  return *parsed;
}

std::string ResolvedPath(std::string_view location) {
  auto resolved = ResolveRedirect(BaseUrl(), location);
  EXPECT_TRUE(resolved.has_value())
      << (resolved ? "" : resolved.error().Message());
  return resolved ? resolved->path : std::string("<none>");
}

}  // namespace

// --- Which statuses are followed ------------------------------------------

TEST(HttpRedirectTest, IsRedirectStatusCoversTheStatusesWithALocation) {
  EXPECT_TRUE(IsRedirectStatus(301));
  EXPECT_TRUE(IsRedirectStatus(302));
  EXPECT_TRUE(IsRedirectStatus(303));
  EXPECT_TRUE(IsRedirectStatus(307));
  EXPECT_TRUE(IsRedirectStatus(308));
}

// 300 is a choice for a human to make and 304 is a cache answer, neither of
// which names a resource to go and fetch. Following one would turn an answer
// into a request for something the server never pointed at.
TEST(HttpRedirectTest, IsRedirectStatusLeavesTheOthersAlone) {
  EXPECT_FALSE(IsRedirectStatus(300));
  EXPECT_FALSE(IsRedirectStatus(304));
  EXPECT_FALSE(IsRedirectStatus(305));
  EXPECT_FALSE(IsRedirectStatus(200));
  EXPECT_FALSE(IsRedirectStatus(404));
  EXPECT_FALSE(IsRedirectStatus(500));
}

// --- Reading the Location -------------------------------------------------

TEST(HttpRedirectTest, RedirectLocationIsFoundWhateverItsCase) {
  HttpResponse response;
  response.headers["location"] = "/lower";
  EXPECT_EQ(RedirectLocation(response), "/lower");

  HttpResponse upper;
  upper.headers["LOCATION"] = "/upper";
  EXPECT_EQ(RedirectLocation(upper), "/upper");
}

TEST(HttpRedirectTest, RedirectLocationIsEmptyWhenThereIsNone) {
  HttpResponse response;
  response.headers["Content-Type"] = "text/html";
  EXPECT_EQ(RedirectLocation(response), "");
}

// --- Resolving a Location -------------------------------------------------

TEST(HttpRedirectTest, AnAbsoluteLocationIsTakenAsItStands) {
  auto resolved = ResolveRedirect(BaseUrl(), "https://other.example/landing");
  ASSERT_TRUE(resolved.has_value()) << resolved.error().Message();
  EXPECT_TRUE(resolved->tls);
  EXPECT_EQ(resolved->host, "other.example");
  EXPECT_EQ(resolved->port, 443);
  EXPECT_EQ(resolved->path, "/landing");
}

// A network-path reference names another authority and keeps the scheme, which
// is how a redirect to a sibling service on the same scheme is written.
TEST(HttpRedirectTest, ANetworkPathLocationKeepsTheScheme) {
  auto resolved = ResolveRedirect(BaseUrl(), "//other.example/landing");
  ASSERT_TRUE(resolved.has_value()) << resolved.error().Message();
  EXPECT_FALSE(resolved->tls);
  EXPECT_EQ(resolved->host, "other.example");
  EXPECT_EQ(resolved->port, 80);
  EXPECT_EQ(resolved->path, "/landing");

  auto secure = ParseUrl("https://example.com/a");
  ASSERT_TRUE(secure.has_value());
  auto from_https = ResolveRedirect(*secure, "//other.example/landing");
  ASSERT_TRUE(from_https.has_value()) << from_https.error().Message();
  EXPECT_TRUE(from_https->tls);
  EXPECT_EQ(from_https->port, 443);
}

TEST(HttpRedirectTest, AnAbsolutePathReplacesThePathAndKeepsTheAuthority) {
  auto resolved = ResolveRedirect(BaseUrl(), "/z");
  ASSERT_TRUE(resolved.has_value()) << resolved.error().Message();
  EXPECT_EQ(resolved->host, "example.com");
  EXPECT_EQ(resolved->port, 8080);
  EXPECT_EQ(resolved->path, "/z");
}

TEST(HttpRedirectTest, ARelativeLocationResolvesAgainstTheRequestPath) {
  // The base path is "/a/b?x=1", so the directory the request was for is "/a/".
  EXPECT_EQ(ResolvedPath("c"), "/a/c");
  EXPECT_EQ(ResolvedPath("./c"), "/a/c");
  EXPECT_EQ(ResolvedPath("../c"), "/c");
  // Above the root is still the root.
  EXPECT_EQ(ResolvedPath("../../c"), "/c");
  EXPECT_EQ(ResolvedPath("%2Fc"), "/a/%2Fc");
}

// A trailing slash says "directory", and a redirect that asks for one has to
// send the slash or the server answers with a redirect back to it.
TEST(HttpRedirectTest, ATrailingSlashIsKept) {
  EXPECT_EQ(ResolvedPath("c/"), "/a/c/");
  EXPECT_EQ(ResolvedPath("."), "/a/");
  EXPECT_EQ(ResolvedPath(".."), "/");
  EXPECT_EQ(ResolvedPath("/z/"), "/z/");
  EXPECT_EQ(ResolvedPath("/z/../y/"), "/y/");
}

TEST(HttpRedirectTest, ADotSegmentInsideThePathIsRemoved) {
  EXPECT_EQ(ResolvedPath("/a/b/../c"), "/a/c");
  EXPECT_EQ(ResolvedPath("/a/./b/./c"), "/a/b/c");
  // Popping an empty stack is not allowed to walk above the root.
  EXPECT_EQ(ResolvedPath("/../../c"), "/c");
}

// A query-only Location changes the query and leaves the path alone. Resolving
// it as a path would look for a file named "?x=2" in the base directory.
TEST(HttpRedirectTest, AQueryOnlyLocationReplacesTheQuery) {
  EXPECT_EQ(ResolvedPath("?y=2"), "/a/b?y=2");

  auto no_query = ParseUrl("http://example.com/a/b");
  ASSERT_TRUE(no_query.has_value());
  auto resolved = ResolveRedirect(*no_query, "?y=2");
  ASSERT_TRUE(resolved.has_value()) << resolved.error().Message();
  EXPECT_EQ(resolved->path, "/a/b?y=2");
}

// A fragment names a place inside the resource and never goes on the wire.
TEST(HttpRedirectTest, AFragmentIsStripped) {
  EXPECT_EQ(ResolvedPath("/p#section"), "/p");
  EXPECT_EQ(ResolvedPath("c#section"), "/a/c");
  EXPECT_EQ(ResolvedPath("/p?q=1#section"), "/p?q=1");
}

TEST(HttpRedirectTest, AMissingLocationIsRejected) {
  auto resolved = ResolveRedirect(BaseUrl(), "");
  ASSERT_FALSE(resolved.has_value());
  EXPECT_EQ(resolved.error().code, NetworkErrc::kProtocolError);
}

// A Location of nothing but a fragment names the document the response already
// came from, so there is nothing to request and no end to a chain that keeps
// pointing there.
TEST(HttpRedirectTest, ALocationThatIsOnlyAFragmentIsRejected) {
  auto resolved = ResolveRedirect(BaseUrl(), "#section");
  ASSERT_FALSE(resolved.has_value());
  EXPECT_EQ(resolved.error().code, NetworkErrc::kProtocolError);
}

// A colon in the first segment of a relative reference is a scheme, not a path:
// requesting "mailto:someone@example.com" as though it were a file is not what
// the server meant, and no answer to it would be the resource asked for.
TEST(HttpRedirectTest, ALocationNamingAnotherSchemeIsRejected) {
  for (const char* location : {"mailto:someone@example.com",
                               "javascript:alert(1)", "ftp://example.com/f"}) {
    auto resolved = ResolveRedirect(BaseUrl(), location);
    ASSERT_FALSE(resolved.has_value()) << location;
    EXPECT_EQ(resolved.error().code, NetworkErrc::kProtocolError);
  }
  // The colon rule applies to the first segment only: a later one is a path
  // character like any other.
  EXPECT_EQ(ResolvedPath("/a:b/c"), "/a:b/c");
}

// --- Origins --------------------------------------------------------------

TEST(HttpRedirectTest, SameOriginComparesSchemeHostAndPort) {
  auto first = ParseUrl("https://example.com:8443/a");
  auto second = ParseUrl("https://example.com:8443/b");
  ASSERT_TRUE(first.has_value() && second.has_value());
  EXPECT_TRUE(SameOrigin(*first, *second));

  // Host names are not case sensitive, so a redirect that spells the same host
  // differently has not left the origin.
  auto shouty = ParseUrl("https://EXAMPLE.com:8443/b");
  ASSERT_TRUE(shouty.has_value());
  EXPECT_TRUE(SameOrigin(*first, *shouty));
}

TEST(HttpRedirectTest, ADifferentPortOrSchemeIsADifferentOrigin) {
  auto https = ParseUrl("https://example.com:8443/a");
  auto other_port = ParseUrl("https://example.com:9443/a");
  auto other_host = ParseUrl("https://other.example:8443/a");
  auto other_scheme = ParseUrl("http://example.com:8443/a");
  ASSERT_TRUE(https && other_port && other_host && other_scheme);

  EXPECT_FALSE(SameOrigin(*https, *other_port));
  EXPECT_FALSE(SameOrigin(*https, *other_host));
  EXPECT_FALSE(SameOrigin(*https, *other_scheme));
}

// --- Credentials ----------------------------------------------------------

TEST(HttpRedirectTest, CredentialHeadersAreTheOnesBoundToOneOrigin) {
  EXPECT_TRUE(IsCredentialHeader("Authorization"));
  EXPECT_TRUE(IsCredentialHeader("authorization"));
  EXPECT_TRUE(IsCredentialHeader("AUTHORIZATION"));
  EXPECT_TRUE(IsCredentialHeader("Cookie"));
  EXPECT_TRUE(IsCredentialHeader("Proxy-Authorization"));
}

TEST(HttpRedirectTest, OtherHeadersAreNotCredentials) {
  EXPECT_FALSE(IsCredentialHeader("X-Auth"));
  EXPECT_FALSE(IsCredentialHeader("Content-Type"));
  EXPECT_FALSE(IsCredentialHeader("Authorization-Token"));
  EXPECT_FALSE(IsCredentialHeader(""));
}
