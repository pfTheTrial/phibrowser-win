// Host tests for the PURE half of the sidebar drag session
// (docs/architecture-review.md §3.5, lane 1 cluster I): what arming a drag
// on a key drags, when a press becomes a drag, and the reorder slide's
// easing. The rest of PhiSidebarDragSession is Win32 and framework — the
// capture, the drag image, the drop resolution — and stays on the target;
// PHI_SIDEBAR_DRAG_MODEL_ONLY compiles the half that is not. The real
// drags are driven end to end on the box by test/sv-drag.mjs.

#define PHI_SIDEBAR_DRAG_MODEL_ONLY

#include <cmath>
#include <cstdio>

#include "phi_sidebar_drag_session.h"

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

bool Near(double a, double b) {
  return std::fabs(a - b) < 1e-9;
}

// --- What a key drags -------------------------------------------------------

void EveryKindOfKeyMapsToOneDragKind() {
  // A tab row and a merged split row both drag as a tab: the split's key is
  // the row's, and the drop moves the pair.
  CHECK(PhiDragKindForKey(SidebarKey::Tab(7), false) == PhiDragKind::kTab);
  CHECK(PhiDragKindForKey(SidebarKey::Split("s1"), false) ==
        PhiDragKind::kTab);

  // A stored key is the pinned grid's or the bookmark outline's depending on
  // which section holds it — the same guid could be either after a
  // conversion, so the section, not the key, decides.
  CHECK(PhiDragKindForKey(SidebarKey::Cell("g1"), true) == PhiDragKind::kPin);
  CHECK(PhiDragKindForKey(SidebarKey::Cell("g1"), false) ==
        PhiDragKind::kBookmark);
  CHECK(PhiDragKindForKey(SidebarKey::Folder("g2"), true) ==
        PhiDragKind::kPin);
  CHECK(PhiDragKindForKey(SidebarKey::Folder("g2"), false) ==
        PhiDragKind::kBookmark);

  // The Spaces strip's pip rides the same machinery.
  CHECK(PhiDragKindForKey(SidebarKey::Space("work"), false) ==
        PhiDragKind::kChip);

  // A group header is not draggable, and neither is nothing at all.
  CHECK(PhiDragKindForKey(SidebarKey::Group("t1"), false) ==
        PhiDragKind::kNone);
  CHECK(PhiDragKindForKey(SidebarKey(), false) == PhiDragKind::kNone);
  CHECK(PhiDragKindForKey(SidebarKey(), true) == PhiDragKind::kNone);
}

// --- Press, then drag -------------------------------------------------------

void APressIsNotADragUntilItTravels() {
  const int cx = 4;  // The system's SM_CXDRAG / SM_CYDRAG box.
  const int cy = 4;

  // Inside the box in both axes: still a click, so the row activates on
  // release and no drag image is lifted.
  CHECK(!PhiDragPassedThreshold(100, 100, 100, 100, cx, cy));
  CHECK(!PhiDragPassedThreshold(100, 100, 103, 97, cx, cy));

  // Either axis alone is enough — the box is a box, not a radius — and a
  // press exactly on its edge has already left it.
  CHECK(PhiDragPassedThreshold(100, 100, 104, 100, cx, cy));
  CHECK(PhiDragPassedThreshold(100, 100, 100, 104, cx, cy));
  CHECK(PhiDragPassedThreshold(100, 100, 96, 100, cx, cy));
  CHECK(PhiDragPassedThreshold(100, 100, 100, 96, cx, cy));
  CHECK(PhiDragPassedThreshold(100, 100, 40, 400, cx, cy));

  // A zero-sized box (a system that reports none) makes any press a drag,
  // including one that never moved — the caller's own guard, not ours.
  CHECK(PhiDragPassedThreshold(10, 10, 10, 10, 0, 0));
}

// --- The reorder slide ------------------------------------------------------

void OneSlideLegEasesFromWhereTheElementIs() {
  const int ms = 200;

  // The ends are exact: a leg starts where the element is and finishes on
  // its target, so a settled row sits on a whole pixel.
  CHECK(Near(PhiDragSlideEase(0.0, 36.0, 0, ms), 0.0));
  CHECK(Near(PhiDragSlideEase(0.0, 36.0, ms, ms), 36.0));
  // Past the end it stays put rather than overshooting.
  CHECK(Near(PhiDragSlideEase(0.0, 36.0, ms * 4, ms), 36.0));

  // EASE_IN_OUT is symmetric about the midpoint.
  CHECK(Near(PhiDragSlideEase(0.0, 36.0, ms / 2, ms), 18.0));
  const double quarter = PhiDragSlideEase(0.0, 1.0, ms / 4, ms);
  const double three_quarters = PhiDragSlideEase(0.0, 1.0, 3 * ms / 4, ms);
  CHECK(Near(quarter + three_quarters, 1.0));
  CHECK(quarter < 0.25);          // Slow at the start,
  CHECK(three_quarters > 0.75);   // fast in the middle, slow at the end.

  // Retargeting mid-flight: the new leg starts from where the element
  // actually IS, so the gap can move under the cursor without a jump.
  const double midway = PhiDragSlideEase(0.0, 36.0, ms / 3, ms);
  CHECK(Near(PhiDragSlideEase(midway, -36.0, 0, ms), midway));

  // A leg that goes nowhere is flat throughout.
  CHECK(Near(PhiDragSlideEase(12.0, 12.0, ms / 3, ms), 12.0));

  // A zero-length animation is already over (a guard against dividing by
  // the duration).
  CHECK(Near(PhiDragSlideEase(0.0, 36.0, 0, 0), 36.0));
}

}  // namespace

int main() {
  EveryKindOfKeyMapsToOneDragKind();
  APressIsNotADragUntilItTravels();
  OneSlideLegEasesFromWhereTheElementIs();
  if (g_failures == 0) {
    std::printf("sidebar_drag_test: all checks passed\n");
  }
  return g_failures == 0 ? 0 : 1;
}
