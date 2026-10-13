// Host tests for the pure half of the SECONDARY-WINDOW family
// (phi_secondary_window.h; docs/architecture-review.md §3.6 /
// lane3-ui-substrate.md §6.2): the caption frame's button geometry and
// hit-test, and the active-dialog list's two decisions. The Win32 half of
// phi_secondary_window.cc is behind #if defined(_WIN32) and never reaches
// the host.
//
// Both are worth pinning here because both replaced copies. The caption
// geometry was written four times (the shell, the kiosk, Settings and
// PhiPopupChrome — lane 3 §2); PhiPopupChrome is now the only one that
// paints it and this is the only one that computes it. The dialog list is
// what let the last two nested message loops go: a window says which keys
// it wants the dialog manager to see, and a thread hook runs
// IsDialogMessage for it inside the MAIN loop.

#include <cstdio>
#include <vector>

#include "phi_secondary_window.h"

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

// The prompt / feedback / transcript band at 100%: 800 wide, 32 tall,
// 46-wide buttons.
PhiRectI R(int left, int top, int right, int bottom) {
  PhiRectI r;
  r.left = left;
  r.top = top;
  r.right = right;
  r.bottom = bottom;
  return r;
}

PhiCaptionMetrics Band(bool close, bool maximize, bool minimize) {
  PhiCaptionMetrics m;
  m.width = 800;
  m.height = 32;
  m.button_width = 46;
  m.close = close;
  m.maximize = maximize;
  m.minimize = minimize;
  return m;
}

void CaptionButtons() {
  std::printf("caption: trailing cluster is close, maximize, minimize\n");
  // The prompt: close only.
  const PhiCaptionMetrics only_close = Band(true, false, false);
  const PhiRectI close = PhiCaptionButtonBounds(only_close,
                                                PhiCaptionPart::kClose);
  CHECK(close.left == 754 && close.right == 800);
  CHECK(close.top == 0 && close.bottom == 32);
  // An absent button takes no room and answers empty.
  CHECK(PhiCaptionButtonBounds(only_close, PhiCaptionPart::kMaximize).empty());
  CHECK(PhiCaptionButtonBounds(only_close, PhiCaptionPart::kMinimize).empty());
  // kCaption and kOutside are never buttons.
  CHECK(PhiCaptionButtonBounds(only_close, PhiCaptionPart::kCaption).empty());
  CHECK(PhiCaptionButtonBounds(only_close, PhiCaptionPart::kOutside).empty());

  // The feedback form: close + minimize, no maximize. The minimize sits
  // NEXT to close — the missing maximize leaves no gap.
  const PhiCaptionMetrics no_max = Band(true, false, true);
  CHECK(PhiCaptionButtonBounds(no_max, PhiCaptionPart::kClose) ==
        R(754, 0, 800, 32));
  CHECK(PhiCaptionButtonBounds(no_max, PhiCaptionPart::kMinimize) ==
        R(708, 0, 754, 32));

  // The transcript and the kiosk: the full cluster, minimize leftmost.
  const PhiCaptionMetrics all = Band(true, true, true);
  CHECK(PhiCaptionButtonBounds(all, PhiCaptionPart::kClose) ==
        R(754, 0, 800, 32));
  CHECK(PhiCaptionButtonBounds(all, PhiCaptionPart::kMaximize) ==
        R(708, 0, 754, 32));
  CHECK(PhiCaptionButtonBounds(all, PhiCaptionPart::kMinimize) ==
        R(662, 0, 708, 32));

  // A band with no width at all (a window mid-creation) answers empty
  // rather than a negative rect.
  PhiCaptionMetrics unborn = all;
  unborn.width = 0;
  CHECK(PhiCaptionButtonBounds(unborn, PhiCaptionPart::kClose).empty());
}

void CaptionHits() {
  std::printf("caption: hit-test walks the cluster, then the drag band\n");
  const PhiCaptionMetrics all = Band(true, true, true);
  CHECK(PhiCaptionHitTest(all, 780, 10) == PhiCaptionPart::kClose);
  CHECK(PhiCaptionHitTest(all, 730, 10) == PhiCaptionPart::kMaximize);
  CHECK(PhiCaptionHitTest(all, 680, 10) == PhiCaptionPart::kMinimize);
  CHECK(PhiCaptionHitTest(all, 400, 10) == PhiCaptionPart::kCaption);
  CHECK(PhiCaptionHitTest(all, 0, 0) == PhiCaptionPart::kCaption);
  // Half-open: the last row of the band is 31, and 32 is the client.
  CHECK(PhiCaptionHitTest(all, 400, 31) == PhiCaptionPart::kCaption);
  CHECK(PhiCaptionHitTest(all, 400, 32) == PhiCaptionPart::kOutside);
  CHECK(PhiCaptionHitTest(all, 400, -1) == PhiCaptionPart::kOutside);
  // The button edges, on the half-open convention.
  CHECK(PhiCaptionHitTest(all, 754, 0) == PhiCaptionPart::kClose);
  CHECK(PhiCaptionHitTest(all, 753, 0) == PhiCaptionPart::kMaximize);
  CHECK(PhiCaptionHitTest(all, 799, 31) == PhiCaptionPart::kClose);

  // The Kiosk's band is its 52dip toolbar: the buttons grow with it.
  PhiCaptionMetrics kiosk = all;
  kiosk.height = 52;
  CHECK(PhiCaptionButtonBounds(kiosk, PhiCaptionPart::kClose).bottom == 52);
  CHECK(PhiCaptionHitTest(kiosk, 400, 40) == PhiCaptionPart::kCaption);
  CHECK(PhiCaptionHitTest(kiosk, 400, 52) == PhiCaptionPart::kOutside);
}

void CaptionTitle() {
  std::printf("caption: the centred title clears both ends\n");
  // The title is centred on the WINDOW, so the reserve is symmetric and
  // the wider end wins. With the mark leading (40dip) and one 46 button
  // trailing, the buttons win.
  CHECK(PhiCaptionTitleInset(Band(true, false, false), 40) == 46);
  // The full cluster reserves all three.
  CHECK(PhiCaptionTitleInset(Band(true, true, true), 40) == 138);
  // A wide leading mark wins instead.
  CHECK(PhiCaptionTitleInset(Band(true, false, false), 60) == 60);
  // No buttons at all: only the leading inset.
  CHECK(PhiCaptionTitleInset(Band(false, false, false), 40) == 40);
}

void DialogKeys() {
  std::printf("dialogs: a window opts in to exactly the keys it wants\n");
  // The feedback form and the transcript: Tab only. Escape and Return
  // mean something else there, and IsDialogMessage would steal them.
  const unsigned tab_only = kPhiDialogTab;
  CHECK(PhiDialogWantsKey(tab_only, PhiDialogMessageKind::kKeyDown,
                          kPhiVkTab));
  // The WM_CHAR twin too, so the EDIT never beeps for a key that moved
  // the focus.
  CHECK(PhiDialogWantsKey(tab_only, PhiDialogMessageKind::kChar, kPhiVkTab));
  CHECK(!PhiDialogWantsKey(tab_only, PhiDialogMessageKind::kKeyDown,
                           kPhiVkEscape));
  CHECK(!PhiDialogWantsKey(tab_only, PhiDialogMessageKind::kKeyDown,
                           kPhiVkReturn));
  CHECK(!PhiDialogWantsKey(tab_only, PhiDialogMessageKind::kKeyDown, 'A'));

  // The prompt registers nothing: its own modal loop translates, and its
  // chord recorder must see raw keys first.
  CHECK(!PhiDialogWantsKey(kPhiDialogNone, PhiDialogMessageKind::kKeyDown,
                           kPhiVkTab));

  const unsigned everything = kPhiDialogTab | kPhiDialogEscape |
                              kPhiDialogEnter | kPhiDialogArrows;
  CHECK(PhiDialogWantsKey(everything, PhiDialogMessageKind::kSysKeyDown,
                          kPhiVkEscape));
  CHECK(PhiDialogWantsKey(everything, PhiDialogMessageKind::kKeyDown,
                          kPhiVkReturn));
  CHECK(PhiDialogWantsKey(everything, PhiDialogMessageKind::kKeyDown,
                          kPhiVkDown));
  // Arrows never arrive as characters.
  CHECK(!PhiDialogWantsKey(everything, PhiDialogMessageKind::kChar,
                           kPhiVkDown));
  // Anything that is not a key message is never a dialog message, whatever
  // the mask says.
  CHECK(!PhiDialogWantsKey(everything, PhiDialogMessageKind::kOther,
                           kPhiVkTab));
}

void DialogOwner() {
  std::printf("dialogs: the innermost registered ancestor takes the key\n");
  const uintptr_t outer = 0x100;
  const uintptr_t inner = 0x200;
  const uintptr_t stray = 0x300;
  const uintptr_t edit = 0x201;

  // A control of a registered window.
  CHECK(PhiDialogOwner({outer}, {edit, inner, outer}) == 0);
  // Two open secondary windows, one over the other: the one the focus is
  // actually inside wins, whatever order the list is in.
  CHECK(PhiDialogOwner({outer, inner}, {edit, inner, outer}) == 1);
  CHECK(PhiDialogOwner({inner, outer}, {edit, inner, outer}) == 0);
  // The window itself, with no control focused.
  CHECK(PhiDialogOwner({outer}, {outer}) == 0);
  // A message for something nobody registered — the shell, a popover, the
  // browser child.
  CHECK(PhiDialogOwner({outer, inner}, {stray}) == -1);
  CHECK(PhiDialogOwner({}, {edit, inner, outer}) == -1);
  CHECK(PhiDialogOwner({outer}, {}) == -1);
  // A null in the chain (GetParent walked off the top) is not a match.
  CHECK(PhiDialogOwner({0}, {edit, 0}) == -1);
}

// Where a window opens: cut down to the work area, then moved inside it.
void Placement() {
  // 1920x1080 at 150 %: a 1032 px tall work area above the taskbar.
  const PhiRectI work = R(0, 0, 1920, 1032);
  // The 640x832 dip onboarding is 1248 px tall there: it is cut to the work
  // area and pinned to its top, so nothing sits under the taskbar.
  CHECK(PhiFitRectInto(R(100, 50, 1060, 1298), work) == R(100, 0, 1060, 1032));
  // A frame that fits is moved the least distance: up off the taskbar …
  CHECK(PhiFitRectInto(R(10, 900, 410, 1200), work) == R(10, 732, 410, 1032));
  // … and in from the left edge; one already inside is untouched.
  CHECK(PhiFitRectInto(R(-50, 10, 350, 310), work) == R(0, 10, 400, 310));
  CHECK(PhiFitRectInto(R(10, 10, 410, 310), work) == R(10, 10, 410, 310));
  // A monitor left of and above the primary: negative coordinates are
  // kept, not clamped to 0 (the old std::max(x, 0) dragged the dialog onto
  // the primary).
  const PhiRectI left = R(-1366, -200, 0, 528);
  CHECK(PhiFitRectInto(R(-1000, -100, -500, 300), left) ==
        R(-1000, -100, -500, 300));
  CHECK(PhiFitRectInto(R(-1000, -100, -500, 700), left) ==
        R(-1000, -200, -500, 528));
  // Centred across the anchor, one third down, then fitted.
  CHECK(PhiPlaceCentered(400, 300, R(0, 0, 1000, 900), work) ==
        R(300, 200, 700, 500));
  CHECK(PhiPlaceCentered(400, 300, left, left) ==
        R(-883, -58, -483, 242));
  // Taller than the work area: as tall as it, from its top.
  CHECK(PhiPlaceCentered(960, 1248, work, work) == R(480, 0, 1440, 1032));
}

}  // namespace

int main() {
  CaptionButtons();
  CaptionHits();
  CaptionTitle();
  DialogKeys();
  DialogOwner();
  Placement();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("secondary window: all passed\n");
  return 0;
}
