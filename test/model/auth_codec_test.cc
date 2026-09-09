// Host tests for the pure half of the Auth0 sign-in (phi_auth_codec.h): the
// base64url codec, SHA-256 and the PKCE challenge against RFC 7636's
// vectors, percent-coding, the intercepted callback URL, the JWT
// payload split and the tenant -> account-service rule. Run with
// test/model/run.sh. The sockets, WinHTTP, DPAPI and the message window in
// phi_auth.cc are exercised on the box (a real sign-in).

#include <cstdio>
#include <cstdlib>
#include <string>

#include "phibrowser-win/client/phi_auth_codec.h"

using namespace phi_client;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

std::string Hex(const std::array<uint8_t, 32>& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (uint8_t b : digest) {
    out += kHex[b >> 4];
    out += kHex[b & 0xF];
  }
  return out;
}

void TestBase64Url() {
  // RFC 4648 §10 vectors, in the url-safe alphabet without padding.
  CHECK(PhiBase64UrlEncode("") == "");
  CHECK(PhiBase64UrlEncode("f") == "Zg");
  CHECK(PhiBase64UrlEncode("fo") == "Zm8");
  CHECK(PhiBase64UrlEncode("foo") == "Zm9v");
  CHECK(PhiBase64UrlEncode("foob") == "Zm9vYg");
  CHECK(PhiBase64UrlEncode("fooba") == "Zm9vYmE");
  CHECK(PhiBase64UrlEncode("foobar") == "Zm9vYmFy");
  // The two characters that differ from standard base64.
  CHECK(PhiBase64UrlEncode("\xfb\xff") == "-_8");
  CHECK(PhiBase64UrlDecode("-_8").value() == "\xfb\xff");
  CHECK(PhiBase64UrlDecode("+/8").value() == "\xfb\xff");  // Tolerated.
  // Round trips, padded and unpadded.
  CHECK(PhiBase64UrlDecode("Zm9vYg").value() == "foob");
  CHECK(PhiBase64UrlDecode("Zm9vYg==").value() == "foob");
  CHECK(PhiBase64UrlDecode("Zm9vYmE").value() == "fooba");
  CHECK(PhiBase64UrlDecode("").value() == "");
  // Bad input.
  CHECK(!PhiBase64UrlDecode("Zm9v!").has_value());
  CHECK(!PhiBase64UrlDecode("Z").has_value());  // 4k+1 is never a length.
  std::string all;
  for (int i = 0; i < 256; ++i) {
    all += static_cast<char>(i);
  }
  CHECK(PhiBase64UrlDecode(PhiBase64UrlEncode(all)).value() == all);
}

void TestSha256() {
  // FIPS 180-4 / NIST vectors.
  CHECK(Hex(PhiSha256("")) ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(Hex(PhiSha256("abc")) ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  // 56 bytes: the length where the padding spills into a second block.
  CHECK(Hex(PhiSha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  // 64 bytes exactly: one full block plus a padding-only block.
  CHECK(Hex(PhiSha256(std::string(64, 'a'))) ==
        "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  CHECK(Hex(PhiSha256(std::string(1000, 'a'))) ==
        "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

void TestPkce() {
  // RFC 7636 appendix B.
  CHECK(PhiPkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") ==
        "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
  // A 32-byte verifier encodes to 43 characters, RFC 7636's minimum.
  CHECK(PhiBase64UrlEncode(std::string(32, '\x7f')).size() == 43);
}

void TestUrlCoding() {
  CHECK(PhiUrlEncode("abcXYZ019-_.~") == "abcXYZ019-_.~");
  CHECK(PhiUrlEncode("http://127.0.0.1:5000/callback") ==
        "http%3A%2F%2F127.0.0.1%3A5000%2Fcallback");
  CHECK(PhiUrlEncode("openid profile email offline_access") ==
        "openid%20profile%20email%20offline_access");
  CHECK(PhiUrlEncode("\xc3\xa9") == "%C3%A9");
  CHECK(PhiUrlDecode("a%20b+c") == "a b c");
  CHECK(PhiUrlDecode("%C3%A9") == "\xc3\xa9");
  CHECK(PhiUrlDecode("%zz%4") == "%zz%4");  // Malformed escapes pass through.
  CHECK(PhiUrlDecode("100%") == "100%");
  CHECK(PhiUrlDecode(PhiUrlEncode("x=1&y=2/3?")) == "x=1&y=2/3?");
}

void TestCallbackUri() {
  CHECK(PhiMacCallbackUri("com.phibrowser.Mac", "auth.stag.phibrowser.com") ==
        "com.phibrowser.Mac://auth.stag.phibrowser.com/macos/"
        "com.phibrowser.Mac/callback");
}

void TestCallbackUrl() {
  {
    // What Auth0 redirects to after the hosted login; GURL lower-cases the
    // scheme and host on the way through Chromium.
    const auto r = PhiParseCallbackUrl(
        "com.phibrowser.mac://auth.stag.phibrowser.com/macos/"
        "com.phibrowser.Mac/callback?code=AbC-123_xyz&state=s7-_Q");
    CHECK(r.has_value());
    CHECK(r->scheme == "com.phibrowser.mac");
    CHECK(r->host == "auth.stag.phibrowser.com");
    CHECK(r->path == "/macos/com.phibrowser.Mac/callback");
    CHECK(r->code == "AbC-123_xyz");
    CHECK(r->state == "s7-_Q");
    CHECK(r->error.empty());
  }
  {
    // As the client built it (mixed case) parses the same.
    const auto r = PhiParseCallbackUrl(
        "com.phibrowser.Mac://Auth.Stag.Phibrowser.com/macos/"
        "com.phibrowser.Mac/callback?state=a%2Bb&foo=bar&code=c");
    CHECK(r.has_value());
    CHECK(r->scheme == "com.phibrowser.mac");
    CHECK(r->host == "auth.stag.phibrowser.com");
    CHECK(r->state == "a+b");
    CHECK(r->code == "c");
  }
  {
    // Auth0's refusal shape: error + error_description, no code.
    const auto r = PhiParseCallbackUrl(
        "com.phibrowser.mac://auth.stag.phibrowser.com/macos/"
        "com.phibrowser.Mac/callback?error=access_denied"
        "&error_description=User%20cancelled&state=abc");
    CHECK(r.has_value());
    CHECK(r->code.empty());
    CHECK(r->error == "access_denied");
    CHECK(r->state == "abc");
  }
  {
    // The logout return: the bare callback, no query.
    const auto r = PhiParseCallbackUrl(
        "com.phibrowser.mac://auth.phibrowser.com/macos/"
        "com.phibrowser.Mac/callback");
    CHECK(r.has_value());
    CHECK(r->path == "/macos/com.phibrowser.Mac/callback");
    CHECK(r->code.empty() && r->state.empty() && r->error.empty());
  }
  {
    // Host only, and a fragment is ignored.
    const auto r = PhiParseCallbackUrl("com.phibrowser.mac://host#frag");
    CHECK(r.has_value());
    CHECK(r->host == "host");
    CHECK(r->path == "/");
  }
  {
    // A query right after the host still yields path "/".
    const auto r = PhiParseCallbackUrl("x://h?code=1");
    CHECK(r.has_value());
    CHECK(r->path == "/");
    CHECK(r->code == "1");
  }
  CHECK(!PhiParseCallbackUrl("no-scheme-here").has_value());
  CHECK(!PhiParseCallbackUrl("://nohost").has_value());
  CHECK(!PhiParseCallbackUrl("x:///path").has_value());
  CHECK(!PhiParseCallbackUrl("").has_value());
}

void TestJwtPayload() {
  // {"alg":"none"} . {"sub":"auth0|1","name":"Ada"} . (empty signature)
  const std::string header = PhiBase64UrlEncode("{\"alg\":\"none\"}");
  const std::string claims = "{\"sub\":\"auth0|1\",\"name\":\"Ada\"}";
  const std::string jwt = header + "." + PhiBase64UrlEncode(claims) + ".sig";
  CHECK(PhiJwtPayload(jwt).value() == claims);
  CHECK(!PhiJwtPayload("onlyonesegment").has_value());
  CHECK(!PhiJwtPayload("a.b").has_value());
  CHECK(!PhiJwtPayload("a.!!!.c").has_value());
}

void TestAccountBaseUrl() {
  CHECK(PhiAccountBaseUrlForDomain("auth.stag.phibrowser.com") ==
        "https://account.stag.phibrowser.com");
  CHECK(PhiAccountBaseUrlForDomain("auth.phibrowser.com") ==
        "https://account.phibrowser.com");
}

}  // namespace

int main() {
  TestBase64Url();
  TestSha256();
  TestPkce();
  TestUrlCoding();
  TestCallbackUri();
  TestCallbackUrl();
  TestJwtPayload();
  TestAccountBaseUrl();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::puts("auth_codec_test: ok");
  return EXIT_SUCCESS;
}
