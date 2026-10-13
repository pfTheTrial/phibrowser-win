// Host tests for the pure half of the omnibox session
// (docs/architecture-review.md §3.5, lane 1 cluster F): where the EDIT sits
// inside the address capsule, and the dropdown's arithmetic. The session
// itself is Win32 — the EDIT, its subclass, the popup window — and stays on
// the target; PHI_OMNIBOX_SESSION_MODEL_ONLY compiles the half that is not.

#define PHI_OMNIBOX_SESSION_MODEL_ONLY

#include <cstdio>

#include "phi_omnibox_session.h"

using namespace phi_client;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

// A performance-layout capsule at 100%: the sidebar's pill, 240 wide, 30
// tall, with nothing in it.
PhiOmniboxCapsule BareCapsule() {
  PhiOmniboxCapsule capsule;
  capsule.field = SidebarRect{12, 100, 252, 130};
  return capsule;
}

void TextRectCentresTheLine() {
  const SidebarRect r = PhiOmniboxTextRect(BareCapsule(), 20, 12, 8, 8);
  // A 20-tall line in a 30-tall box: 5 clear above and below.
  CHECK(r.top == 105);
  CHECK(r.bottom == 125);
  // Starts at the capsule's leading inset, runs to its trailing gap.
  CHECK(r.left == 24);
  CHECK(r.right == 244);
}

void TextRectStartsAfterThePageIcon() {
  PhiOmniboxCapsule capsule = BareCapsule();
  capsule.page_icon = SidebarRect{16, 106, 36, 124};
  const SidebarRect r = PhiOmniboxTextRect(capsule, 20, 12, 8, 8);
  CHECK(r.left == 44);  // icon.right + icon_gap, not the plain inset.
}

void TextRectStopsAtTheLeftMOSTAccessoryInsideTheCapsule() {
  PhiOmniboxCapsule capsule = BareCapsule();
  capsule.site_menu = SidebarRect{200, 104, 224, 126};
  capsule.reader = SidebarRect{172, 104, 196, 126};
  const SidebarRect r = PhiOmniboxTextRect(capsule, 20, 12, 8, 8);
  CHECK(r.right == 164);  // reader.left - trailing_gap, not the menu's.
}

void TextRectStopsBeforeTheHoverOnlyPair() {
  // Copy link and the site menu are laid out even while hidden: the text
  // must not run under them and jump when the pointer arrives.
  PhiOmniboxCapsule capsule = BareCapsule();
  capsule.site_menu = SidebarRect{200, 104, 224, 126};
  capsule.copy_link = SidebarRect{174, 104, 198, 126};
  const SidebarRect r = PhiOmniboxTextRect(capsule, 20, 12, 8, 8);
  CHECK(r.right == 166);  // copy_link.left - trailing_gap.
}

void TextRectIgnoresAccessoriesOutsideTheCapsule() {
  // Comfortable keeps its extensions button in the top bar, to the RIGHT of
  // the pill; it must not shorten the text.
  PhiOmniboxCapsule capsule = BareCapsule();
  capsule.extensions = SidebarRect{300, 104, 324, 126};
  const SidebarRect r = PhiOmniboxTextRect(capsule, 20, 12, 8, 8);
  CHECK(r.right == 244);
  // Nor may one to the LEFT of the pill (an empty rect at the origin).
  capsule.extensions = SidebarRect{0, 0, 0, 0};
  CHECK(PhiOmniboxTextRect(capsule, 20, 12, 8, 8).right == 244);
}

void TextRectNeverInvertsWhenTheCapsuleIsTiny() {
  PhiOmniboxCapsule capsule;
  capsule.field = SidebarRect{12, 100, 20, 130};
  const SidebarRect r = PhiOmniboxTextRect(capsule, 20, 12, 8, 8);
  CHECK(r.right == r.left + 1);  // One pixel, never a negative width.
}

void PopupHeightIsRowsPlusPadding() {
  CHECK(PhiOmniboxPopupHeight(0, 32, 12) == 12);
  CHECK(PhiOmniboxPopupHeight(3, 32, 12) == 108);
  CHECK(PhiOmniboxPopupHeight(8, 48, 18) == 402);  // 150% scaling.
}

void RowAtYPicksTheRowUnderThePointer() {
  CHECK(PhiOmniboxRowAtY(6, 3, 32, 6) == 0);
  CHECK(PhiOmniboxRowAtY(37, 3, 32, 6) == 0);
  CHECK(PhiOmniboxRowAtY(38, 3, 32, 6) == 1);
  CHECK(PhiOmniboxRowAtY(101, 3, 32, 6) == 2);
  CHECK(PhiOmniboxRowAtY(102, 3, 32, 6) == -1);  // Past the last row.
  CHECK(PhiOmniboxRowAtY(20, 0, 32, 6) == -1);   // No rows at all.
  // The popup's own top padding belongs to the first row (truncating
  // division): a press on the border still commits a suggestion rather than
  // falling through to nothing.
  CHECK(PhiOmniboxRowAtY(0, 3, 32, 6) == 0);
}

void SelectionWrapsThroughTheFieldsOwnText() {
  // -1 is the field itself: down from it arms the first row.
  CHECK(PhiOmniboxNextSelection(-1, 1, 3) == 0);
  CHECK(PhiOmniboxNextSelection(0, 1, 3) == 1);
  CHECK(PhiOmniboxNextSelection(2, 1, 3) == -1);   // Past the last: the field.
  CHECK(PhiOmniboxNextSelection(-1, -1, 3) == 2);  // Up from it: the last row.
  CHECK(PhiOmniboxNextSelection(0, -1, 3) == -1);
}

void TrimTakesTheBlanksOffWhatWasTyped() {
  CHECK(PhiOmniboxTrimTyped(L"  example.com \t ") == L"example.com");
  CHECK(PhiOmniboxTrimTyped(L"a b") == L"a b");  // Inner blanks survive.
  CHECK(PhiOmniboxTrimTyped(L"   ").empty());
  CHECK(PhiOmniboxTrimTyped(L"").empty());
}

void TheNewTabPagesUrlShowsAsThePlaceholder() {
  CHECK(PhiOmniboxHidesUrl("chrome://newtab/"));
  CHECK(PhiOmniboxHidesUrl("chrome://newtab"));
  CHECK(!PhiOmniboxHidesUrl("https://example.com/chrome://newtab"));
  CHECK(!PhiOmniboxHidesUrl("chrome://settings"));
  CHECK(!PhiOmniboxHidesUrl(""));
}

}  // namespace

void PasteIsSanitizedLikeChromes() {
  // javascript: schemes are stripped, repeatedly and case-blind, past the
  // blanks and control characters in front of them.
  CHECK(PhiOmniboxSanitizePaste(L"javascript:alert(1)") == L"alert(1)");
  CHECK(PhiOmniboxSanitizePaste(L"  JavaScript:\tjavascript: x") == L"x");
  CHECK(PhiOmniboxStripJavascript(L"\x01javascript:a") == L"a");
  CHECK(PhiOmniboxSanitizePaste(L"https://javascript:x") ==
        L"https://javascript:x");
  // A URL wrapped over lines is joined; search text gets a space.
  CHECK(PhiOmniboxSanitizePaste(L"https://example.com/a\r\n/b") ==
        L"https://example.com/a/b");
  CHECK(PhiOmniboxSanitizePaste(L"hello big\nworld") == L"hello big world");
  // Blank runs without a line break survive; the ends are trimmed.
  CHECK(PhiOmniboxSanitizePaste(L"  a  b \n") == L"a  b");
  CHECK(PhiOmniboxSanitizePaste(L" \r\n ") == L" ");
  CHECK(PhiOmniboxSanitizePaste(L"").empty());
  // The line break cannot smuggle a scheme past the strip.
  CHECK(PhiOmniboxSanitizePaste(L"\njavascript:\nalert(1)") == L"alert(1)");
}

int main() {
  TextRectCentresTheLine();
  TextRectStartsAfterThePageIcon();
  TextRectStopsAtTheLeftMOSTAccessoryInsideTheCapsule();
  TextRectStopsBeforeTheHoverOnlyPair();
  TextRectIgnoresAccessoriesOutsideTheCapsule();
  TextRectNeverInvertsWhenTheCapsuleIsTiny();
  PopupHeightIsRowsPlusPadding();
  RowAtYPicksTheRowUnderThePointer();
  SelectionWrapsThroughTheFieldsOwnText();
  TrimTakesTheBlanksOffWhatWasTyped();
  TheNewTabPagesUrlShowsAsThePlaceholder();
  PasteIsSanitizedLikeChromes();
  if (g_failures == 0) {
    std::printf("omnibox_session_test: all checks passed\n");
  }
  return g_failures == 0 ? 0 : 1;
}
