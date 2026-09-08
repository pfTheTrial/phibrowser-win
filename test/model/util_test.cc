// Host tests for the cross-cutting foundations
// (docs/architecture-review.md §3.9, lane 7 §5.5): the shared helpers in
// phi_util, the log redactors and PhiFw::Must in phi_log, and the theme
// tokens phi_theme resolves. Run with test/model/run.sh.
//
// What is NOT here, and why: PhiStrN, PhiFormatNumber and PhiFormatDate need
// a loaded catalog and GetNumberFormatEx / GetDateFormatEx, so they stay on
// the target. Their PURE halves — the plural rule that picks the branch, the
// substitution that fills {0}, and the digit grouping the number formatter
// falls back to — are all here, which is where the "1 days ago" class of bug
// actually lived.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "phibrowser-win/client/phi_log.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_util.h"

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

#define CHECK_EQ(actual, expected)                                        \
  do {                                                                    \
    const auto a_ = (actual);                                             \
    const auto e_ = (expected);                                           \
    if (!(a_ == e_)) {                                                    \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,        \
                   #actual " != " #expected);                             \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

// ---------------------------------------------------------------------------

void TestClamp() {
  CHECK_EQ(PhiClamp(5, 0, 10), 5);
  CHECK_EQ(PhiClamp(-1, 0, 10), 0);
  CHECK_EQ(PhiClamp(11, 0, 10), 10);
  CHECK_EQ(PhiClamp(0, 0, 0), 0);
  // The reason this is not std::clamp: an inverted range is undefined there
  // and reachable here (an empty rect, a zero-width column). It yields the
  // low bound instead of a debugger.
  CHECK_EQ(PhiClamp(5, 10, 0), 10);
  CHECK_EQ(PhiClamp01(-0.5), 0.0);
  CHECK_EQ(PhiClamp01(1.5), 1.0);
  CHECK_EQ(PhiClamp01(0.25), 0.25);

  CHECK_EQ(PhiLerpInt(0, 10, 0.0), 0);
  CHECK_EQ(PhiLerpInt(0, 10, 1.0), 10);
  CHECK_EQ(PhiLerpInt(0, 10, 0.5), 5);
  CHECK_EQ(PhiLerpInt(0, 10, 0.45), 5);   // Rounds, never truncates.
  CHECK_EQ(PhiLerpInt(10, 0, 0.25), 8);
  CHECK_EQ(PhiLerpInt(0, 10, 2.0), 10);   // t is clamped.
}

void TestDip() {
  // The 266 scattered pixel constants all scaled with ::MulDiv(design, dpi,
  // 96); this reproduces it, including its round-half-away-from-zero.
  CHECK_EQ(PhiDip(96, 32), 32);
  CHECK_EQ(PhiDip(192, 32), 64);
  CHECK_EQ(PhiDip(144, 32), 48);
  CHECK_EQ(PhiDip(120, 10), 13);   // 12.5 -> 13, not 12.
  CHECK_EQ(PhiDip(120, 1), 1);     // A hairline never vanishes at 125%.
  CHECK_EQ(PhiDip(0, 32), 32);     // A dpi nobody set reads as 96.
  CHECK_EQ(PhiDip(192, -8), -16);
}

void TestBlend() {
  const PhiColor black = PhiRgb(0, 0, 0);
  const PhiColor white = PhiRgb(0xFF, 0xFF, 0xFF);
  CHECK_EQ(PhiBlend(white, black, 255), white);
  CHECK_EQ(PhiBlend(white, black, 0), black);
  CHECK_EQ(PhiBlend(white, black, 128), PhiRgb(128, 128, 128));
  // Alpha out of range does not wrap a channel.
  CHECK_EQ(PhiBlend(white, black, 400), white);
  CHECK_EQ(PhiBlend(white, black, -5), black);
  // The argument order is (over, base): white over black at 25% is dark.
  CHECK(PhiRed(PhiBlend(white, black, 64)) <
        PhiRed(PhiBlend(black, white, 64)));
  // Channels do not leak into one another — the bug every hand-rolled copy
  // risked by reaching for GetRValue on the wrong argument.
  CHECK_EQ(PhiBlend(PhiRgb(0xFF, 0, 0), PhiRgb(0, 0, 0xFF), 128),
           PhiRgb(128, 0, 127));

  CHECK_EQ(PhiLerpColor(black, white, 0.0), black);
  CHECK_EQ(PhiLerpColor(black, white, 1.0), white);
  CHECK_EQ(PhiLerpColor(black, white, 2.0), white);
}

void TestTrimAndLower() {
  CHECK_EQ(PhiTrim(std::string("  hi \t\r\n")), std::string("hi"));
  CHECK_EQ(PhiTrim(std::string("   ")), std::string());
  CHECK_EQ(PhiTrim(std::string()), std::string());
  CHECK_EQ(PhiTrim(std::string("a b")), std::string("a b"));
  CHECK_EQ(PhiTrim(std::wstring(L"\n wide \t")), std::wstring(L"wide"));

  CHECK_EQ(PhiLower(std::string("MiXeD-42")), std::string("mixed-42"));
  CHECK_EQ(PhiLower(std::wstring(L"ZH-Hans")), std::wstring(L"zh-hans"));
}

void TestSubstitute() {
  const std::vector<std::wstring> two = {L"7", L"9"};
  CHECK_EQ(PhiSubstitute(L"{0} of {1}", two), std::wstring(L"7 of 9"));
  // A translator may reorder, repeat or drop a placeholder.
  CHECK_EQ(PhiSubstitute(L"{1} / {0}", two), std::wstring(L"9 / 7"));
  CHECK_EQ(PhiSubstitute(L"{0}{0}", two), std::wstring(L"77"));
  CHECK_EQ(PhiSubstitute(L"none", two), std::wstring(L"none"));
  // A placeholder with no argument stays VISIBLE rather than being dropped:
  // a mistranslated pattern must show up in a screenshot.
  CHECK_EQ(PhiSubstitute(L"{0} and {5}", two), std::wstring(L"7 and {5}"));
  // An argument that itself looks like a placeholder is NOT substituted a
  // second time — the reason this is one pass over the pattern rather than
  // one pass per argument, which is what PhiStrF used to do.
  CHECK_EQ(PhiSubstitute(L"{0}", {L"{1}", L"x"}), std::wstring(L"{1}"));
  // Malformed patterns survive intact.
  CHECK_EQ(PhiSubstitute(L"{}", two), std::wstring(L"{}"));
  CHECK_EQ(PhiSubstitute(L"{0", two), std::wstring(L"{0"));
  CHECK_EQ(PhiSubstitute(L"{0x}", two), std::wstring(L"{0x}"));
}

void TestPlural() {
  // English and its neighbours: one is one.
  for (const char* tag : {"en", "de", "es", "nl"}) {
    CHECK(PhiPluralFor(tag, 1) == PhiPlural::kOne);
    CHECK(PhiPluralFor(tag, 0) == PhiPlural::kOther);
    CHECK(PhiPluralFor(tag, 2) == PhiPlural::kOther);
    CHECK(PhiPluralFor(tag, 21) == PhiPlural::kOther);
  }
  // French counts zero as singular.
  CHECK(PhiPluralFor("fr", 0) == PhiPlural::kOne);
  CHECK(PhiPluralFor("fr", 1) == PhiPlural::kOne);
  CHECK(PhiPluralFor("fr", 2) == PhiPlural::kOther);
  // CJK has one form; a count never changes the noun.
  for (const char* tag : {"ja", "ko", "zh-Hans", "zh-Hant"}) {
    CHECK(PhiPluralFor(tag, 1) == PhiPlural::kOther);
    CHECK(PhiPluralFor(tag, 2) == PhiPlural::kOther);
  }
  // Region and case do not change the answer; an unknown tag gets English.
  CHECK(PhiPluralFor("en-GB", 1) == PhiPlural::kOne);
  CHECK(PhiPluralFor("DE-at", 1) == PhiPlural::kOne);
  CHECK(PhiPluralFor("pt-BR", 1) == PhiPlural::kOne);
  CHECK(PhiPluralFor("", 1) == PhiPlural::kOne);
  // Negative counts are plural everywhere the client can reach.
  CHECK(PhiPluralFor("en", -1) == PhiPlural::kOther);

  CHECK_EQ(std::string(PhiPluralSuffix(PhiPlural::kOne)), std::string("one"));
  CHECK_EQ(std::string(PhiPluralSuffix(PhiPlural::kOther)),
           std::string("other"));
}

void TestGroupDigits() {
  CHECK_EQ(PhiGroupDigits(0, L",", 3, 3), std::wstring(L"0"));
  CHECK_EQ(PhiGroupDigits(7, L",", 3, 3), std::wstring(L"7"));
  CHECK_EQ(PhiGroupDigits(999, L",", 3, 3), std::wstring(L"999"));
  CHECK_EQ(PhiGroupDigits(1000, L",", 3, 3), std::wstring(L"1,000"));
  CHECK_EQ(PhiGroupDigits(1234567, L",", 3, 3), std::wstring(L"1,234,567"));
  // German: the separator is a dot and the answer must not be "1,234,567".
  CHECK_EQ(PhiGroupDigits(1234567, L".", 3, 3), std::wstring(L"1.234.567"));
  // French: a narrow no-break space, which is more than one code unit.
  CHECK_EQ(PhiGroupDigits(1234567, L"\u202F", 3, 3),
           std::wstring(L"1\u202F234\u202F567"));
  // Indic 3;2 grouping — the reason the separator positions are a parameter.
  CHECK_EQ(PhiGroupDigits(1234567, L",", 3, 2), std::wstring(L"12,34,567"));
  // No separator groups nothing.
  CHECK_EQ(PhiGroupDigits(1234567, L"", 3, 3), std::wstring(L"1234567"));
  CHECK_EQ(PhiGroupDigits(-1234, L",", 3, 3), std::wstring(L"-1,234"));
}

void TestRelativeSpan() {
  const auto span = [](long long seconds) {
    return PhiRelativeSpanFor(seconds);
  };
  CHECK(span(0).unit == PhiRelativeUnit::kJustNow);
  CHECK(span(59).unit == PhiRelativeUnit::kJustNow);
  // A clock that went backwards reads as "just now", not as the future.
  CHECK(span(-500).unit == PhiRelativeUnit::kJustNow);

  CHECK(span(60).unit == PhiRelativeUnit::kMinutes);
  CHECK_EQ(span(60).count, 1LL);
  CHECK_EQ(span(3599).count, 59LL);

  CHECK(span(3600).unit == PhiRelativeUnit::kHours);
  CHECK_EQ(span(3600).count, 1LL);
  // THE BUG THIS EXISTS FOR: the old ladder said " hour ago" for every value
  // from one to eleven, so eleven hours read "11 hour ago". The count is
  // carried out now and PhiStrN picks the branch.
  CHECK_EQ(span(11 * 3600).count, 11LL);

  CHECK(span(12 * 3600).unit == PhiRelativeUnit::kToday);
  CHECK(span(86399).unit == PhiRelativeUnit::kToday);
  CHECK(span(86400).unit == PhiRelativeUnit::kYesterday);
  CHECK(span(2 * 86400 - 1).unit == PhiRelativeUnit::kYesterday);
  // The other half of the bug: 48 hours used to render "2 days ago" through
  // the same concatenation that produced "1 days ago" at the boundary below.
  CHECK(span(2 * 86400).unit == PhiRelativeUnit::kDays);
  CHECK_EQ(span(2 * 86400).count, 2LL);
  CHECK(span(3 * 86400).unit == PhiRelativeUnit::kAbsolute);
}

// ---------------------------------------------------------------------------
// Redaction. Every case here is a line the client writes today or a shape a
// caller could hand one by mistake.

void TestRedactUrl() {
  CHECK_EQ(PhiRedactUrl("https://mail.example.com/u/0/#inbox?q=taxes"),
           std::string("https://mail.example.com"));
  CHECK_EQ(PhiRedactUrl("http://example.com"), std::string("http://example.com"));
  CHECK_EQ(PhiRedactUrl("https://example.com"),
           std::string("https://example.com"));
  // A port is machine detail and a credential is a secret; neither survives.
  CHECK_EQ(PhiRedactUrl("http://127.0.0.1:53421/callback?code=abc"),
           std::string("http://127.0.0.1"));
  CHECK_EQ(PhiRedactUrl("https://ada:hunter2@example.com/x"),
           std::string("https://example.com"));
  // file:// has no host and its path is the whole secret.
  CHECK_EQ(PhiRedactUrl("file:///C:/Users/ada/tax-return.pdf"),
           std::string("file://"));
  // The schemes that carry nothing personal keep their name.
  CHECK_EQ(PhiRedactUrl("about:blank"), std::string("about:"));
  CHECK_EQ(PhiRedactUrl("chrome://downloads/"),
           std::string("chrome://downloads"));
  // Anything unrecognised is refused outright rather than guessed at.
  CHECK_EQ(PhiRedactUrl("mailto:ada@example.com"), std::string("<url>"));
  CHECK_EQ(PhiRedactUrl("not a url at all"), std::string("<url>"));
  CHECK_EQ(PhiRedactUrl(""), std::string("<url>"));
  CHECK_EQ(PhiRedactUrl("://example.com"), std::string("<url>"));
  // An IPv6 literal keeps its brackets and loses nothing else.
  CHECK_EQ(PhiRedactUrl("http://[::1]/x"), std::string("http://[::1]"));
  // A search term in the query is what rule 3 is protecting.
  CHECK_EQ(PhiRedactUrl("https://www.google.com/search?q=am+i+sick"),
           std::string("https://www.google.com"));
}

void TestRedactId() {
  CHECK_EQ(PhiRedactId("auth0|66f0c1d2e3b4a5968778"), std::string("...8778"));
  CHECK_EQ(PhiRedactId("abcd"), std::string("...abcd"));
  CHECK_EQ(PhiRedactId("abc"), std::string("..."));
  CHECK_EQ(PhiRedactId(""), std::string("..."));
  // Enough to correlate two lines in one log, never enough to name anybody.
  CHECK(PhiRedactId("google-oauth2|113355779911335577991").size() == 7);
}

void TestRedactEmail() {
  CHECK_EQ(PhiRedactEmail("ada.lovelace@example.com"),
           std::string("a***@example.com"));
  CHECK_EQ(PhiRedactEmail("a@b.co"), std::string("a***@b.co"));
  // A caller that thought it had an address and did not still leaks nothing.
  CHECK_EQ(PhiRedactEmail("not-an-address"), std::string("...ress"));
  CHECK_EQ(PhiRedactEmail("@example.com"), std::string("....com"));
}

void TestRedactPath() {
  CHECK_EQ(PhiRedactPath(
               "C:\\Users\\ada\\AppData\\Local\\Phi\\User Data\\Default\\"
               "PhiPrefs.json"),
           std::string("...\\Default\\PhiPrefs.json"));
  CHECK_EQ(PhiRedactPath("/home/ada/.config/phi/spool.json"),
           std::string(".../phi/spool.json"));
  // Which file went wrong is the useful half and survives.
  CHECK_EQ(PhiRedactPath("PhiPrefs.json"), std::string("PhiPrefs.json"));
  CHECK_EQ(PhiRedactPath("Default\\PhiPrefs.json"),
           std::string("...\\PhiPrefs.json"));
  CHECK_EQ(PhiRedactPath(""), std::string());
}

void TestFwMust() {
  // The contract the ~60 call sites depend on: Must RETURNS the bool, so
  // wrapping a call never changes what the caller decides.
  CHECK(PhiFw::Must(true, "ActivateTab", 7, 2));
  CHECK(!PhiFw::Must(false, "ActivateTab", 7, 2));
  CHECK(!PhiFw::Must(false, "SetTabTag", std::string("work")));
  CHECK(PhiFw::Must(true, "CloseTab"));
  // An id list that is empty, and one that mixes types, both compile.
  CHECK(!PhiFw::Must(false, "SplitActiveWith", 1, "left", 3.5));
}

// ---------------------------------------------------------------------------
// Theme tokens. PhiResolvePalette is pure once PhiSystemPrefersDark is
// answered, and on the host it answers "light" — which is also what an
// unconfigured Windows reports.

void TestPaletteRoles() {
  // Copied, not bound: PhiThemeById takes a std::string, so a reference to
  // its result would outlive the temporary the literal builds.
  const PhiTheme pure = PhiThemeById("pure");
  const PhiPalette light = PhiResolvePalette(pure, PhiAppearance::kLight);
  const PhiPalette dark = PhiResolvePalette(pure, PhiAppearance::kDark);

  CHECK(!light.dark);
  CHECK(dark.dark);
  // The ink the percentage overlays are made of, which nine files were
  // spelling as a literal.
  CHECK_EQ(light.ink, PhiRgb(0, 0, 0));
  CHECK_EQ(dark.ink, PhiRgb(0xFF, 0xFF, 0xFF));
  // The fixed ground the secondary windows stand on — the value
  // phi_popup_chrome.h and phi_settings_window.cc each carry today.
  CHECK_EQ(light.ground_fixed, PhiRgb(0xEA, 0xEA, 0xEA));
  CHECK_EQ(dark.ground_fixed, PhiRgb(0x25, 0x25, 0x25));
  // on_accent is legibility, not a fixed white: Amber's light accent is
  // bright enough to need dark ink.
  CHECK_EQ(light.on_accent, PhiRgb(0xFF, 0xFF, 0xFF));  // Pure's #3AA4D5
  const PhiTheme amber_theme = PhiThemeById("amber");
  const PhiPalette amber =
      PhiResolvePalette(amber_theme, PhiAppearance::kLight);
  CHECK_EQ(amber.on_accent, PhiRgb(0x1A, 0x1A, 0x1A));  // #D9B857
  CHECK(amber.accent != light.accent);
  // Hover deepens the accent rather than replacing it.
  CHECK(light.accent_hover != light.accent);
  CHECK(PhiRed(light.accent_hover) <= PhiRed(light.accent));
  // The scrim reads as "behind" against both grounds.
  CHECK(light.overlay_scrim != light.ground_top);
  CHECK(dark.overlay_scrim != dark.ground_top);
  CHECK_EQ(light.destructive, PhiRgb(0xFF, 0x3B, 0x30));
  CHECK(light.destructive_hover != light.destructive);
  CHECK(light.warning != dark.warning);
  CHECK(light.error != dark.error);

  // The saturation slider still spans the tint, and clamps.
  const PhiTheme mint = PhiThemeById("mint");
  const PhiPalette flat =
      PhiResolvePalette(mint, PhiAppearance::kLight, 0);
  const PhiPalette full =
      PhiResolvePalette(mint, PhiAppearance::kLight, 100);
  CHECK(flat.ground_top != full.ground_top);
  CHECK_EQ(PhiResolvePalette(mint, PhiAppearance::kLight, -20).ground_top,
           flat.ground_top);
  CHECK_EQ(PhiResolvePalette(mint, PhiAppearance::kLight, 500).ground_top,
           full.ground_top);

  // An unknown theme id is Pure, never a crash.
  const PhiTheme unknown = PhiThemeById("no-such-theme");
  CHECK_EQ(std::string(unknown.id),
           std::string(PhiBuiltInThemes().front().id));
}

void TestParseColorHex() {
  PhiColor out = 0;
  CHECK(PhiParseColorHex("#3AA4D5", &out));
  CHECK_EQ(out, PhiRgb(0x3A, 0xA4, 0xD5));
  CHECK(PhiParseColorHex("3aa4d5", &out));
  CHECK_EQ(out, PhiRgb(0x3A, 0xA4, 0xD5));
  CHECK(!PhiParseColorHex("#3AA4D", &out));
  CHECK(!PhiParseColorHex("#3AA4DZ", &out));
  CHECK(!PhiParseColorHex("", &out));
}

void TestMetrics() {
  // The design-unit table is constexpr, so a metric can be used in an array
  // bound or a switch label without a runtime read.
  static_assert(kPhiMetrics.corner == 8, "the pill radius, everywhere");
  static_assert(kPhiMetrics.hairline == 1, "");
  CHECK_EQ(PhiDip(192, kPhiMetrics.row_height), 64);
  CHECK_EQ(PhiDip(96, kPhiMetrics.button_width), 130);
  CHECK(kPhiMetrics.corner_chip < kPhiMetrics.corner);
  CHECK(kPhiMetrics.corner < kPhiMetrics.corner_card);
  CHECK(kPhiMetrics.corner_card < kPhiMetrics.corner_window);
  CHECK(kPhiMetrics.pad_tight < kPhiMetrics.pad);
  CHECK(kPhiMetrics.text_small < kPhiMetrics.text_body);
}

}  // namespace

int main() {
  TestClamp();
  TestDip();
  TestBlend();
  TestTrimAndLower();
  TestSubstitute();
  TestPlural();
  TestGroupDigits();
  TestRelativeSpan();
  TestRedactUrl();
  TestRedactId();
  TestRedactEmail();
  TestRedactPath();
  TestFwMust();
  TestPaletteRoles();
  TestParseColorHex();
  TestMetrics();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("util_test: all checks passed\n");
  return 0;
}
