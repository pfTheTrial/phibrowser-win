// Host tests for the pure half of the UI substrate (phi_window.h,
// docs/architecture-review.md §3.6 / lane3-ui-substrate.md §6): popover
// placement with monitor clamping, hit-rect hover resolution, per-window
// timer id allocation and the animator's easing. The Win32 half of
// phi_window.cc is behind #if defined(_WIN32) and never reaches the host.
//
// Placement is worth pinning here because five hand-rolled copies were
// folded into one: each case below reproduces the call site it replaced.

#include <cstdio>
#include <string>
#include <vector>

#include "phi_window.h"

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

PhiRectI R(int left, int top, int right, int bottom) {
  PhiRectI r;
  r.left = left;
  r.top = top;
  r.right = right;
  r.bottom = bottom;
  return r;
}

// A 1920x1040 work area with a 40px taskbar at the bottom.
const PhiRectI kWork = {0, 0, 1920, 1040};

void Rects() {
  std::printf("rects: half-open containment, union ignores empties\n");
  const PhiRectI a = R(10, 10, 20, 20);
  CHECK(a.Contains(10, 10));
  CHECK(!a.Contains(20, 15));   // right/bottom are one past the last pixel.
  CHECK(!a.Contains(15, 20));
  CHECK(a.width() == 10 && a.height() == 10);
  CHECK(R(0, 0, 0, 5).empty());
  CHECK(PhiRectUnion(a, PhiRectI()) == a);
  CHECK(PhiRectUnion(PhiRectI(), a) == a);
  CHECK(PhiRectUnion(a, R(30, 5, 40, 15)) == R(10, 5, 40, 20));
}

void Hits() {
  std::printf("hits: last match wins, disabled hits fall through\n");
  std::vector<PhiHit> hits;
  PhiHit row;
  row.bounds = R(0, 0, 200, 40);
  row.key = 100;
  row.role = PhiHitRole::kListItem;
  hits.push_back(row);
  PhiHit pin;  // Added AFTER the row, so it paints on top and takes clicks.
  pin.bounds = R(160, 8, 190, 32);
  pin.key = 101;
  pin.role = PhiHitRole::kToggle;
  hits.push_back(pin);

  CHECK(PhiHitAt(hits, 10, 10) == 0);
  CHECK(PhiHitAt(hits, 170, 20) == 1);
  CHECK(PhiHitAt(hits, 170, 38) == 0);   // Below the pin, still the row.
  CHECK(PhiHitAt(hits, 300, 20) == -1);
  CHECK(PhiHitAt(hits, 10, 40) == -1);

  // A disabled hit is invisible to the pointer, so the row underneath
  // takes the hover — which is what the downloads popover's greyed-out
  // "show in folder" action needs.
  hits[1].enabled = false;
  CHECK(PhiHitAt(hits, 170, 20) == 0);
}

void PlaceBelow() {
  std::printf("place: below the anchor (folder picker, Space theme)\n");
  PhiPlacementSpec spec;
  spec.side = PhiPopoverSide::kBelow;
  spec.gap = 4;
  // A 264x150 picker under a 100x24 menu row.
  CHECK(PhiPlacePopover(R(300, 200, 400, 224), 264, 150, kWork, spec) ==
        R(300, 228, 564, 378));
  // Off the right edge: clamped, not flipped.
  CHECK(PhiPlacePopover(R(1850, 200, 1900, 224), 264, 150, kWork, spec) ==
        R(1656, 228, 1920, 378));
  // No room below: flips above the anchor.
  CHECK(PhiPlacePopover(R(300, 960, 400, 984), 264, 150, kWork, spec) ==
        R(300, 806, 564, 956));
  // No room either way: flips, then the clamp pins it to the work top.
  CHECK(PhiPlacePopover(R(300, 900, 400, 1020), 264, 1200, kWork, spec)
            .top == 0);
}

void PlaceAbove() {
  std::printf("place: above the anchor (downloads, extensions)\n");
  PhiPlacementSpec spec;
  spec.side = PhiPopoverSide::kAbove;
  spec.gap = 8;
  spec.flip = false;  // The downloads button always drops upward.
  // A 340x317 panel over a 24x24 bottom-bar button.
  CHECK(PhiPlacePopover(R(40, 900, 64, 924), 340, 317, kWork, spec) ==
        R(40, 575, 380, 892));
  // Pinned against the work top when the button sits too high.
  CHECK(PhiPlacePopover(R(40, 100, 64, 124), 340, 317, kWork, spec) ==
        R(40, 0, 380, 317));

  // The extensions popover picks its side from where the anchor sits and
  // lets the flip correct it.
  spec.flip = true;
  CHECK(PhiPlacePopover(R(40, 20, 64, 44), 300, 380, kWork, spec) ==
        R(40, 52, 340, 432));  // No room above: flipped below.
}

void PlaceCentring() {
  std::printf("place: leading / centred / centred-if-wider\n");
  PhiPlacementSpec spec;
  spec.side = PhiPopoverSide::kBelow;

  spec.align = PhiPopoverAlign::kCenter;
  CHECK(PhiPlacePopover(R(300, 200, 400, 224), 200, 100, kWork, spec).left ==
        250);

  // The Space theme panel: a point anchor hangs it off the left edge, a
  // caption-band anchor WIDER than the panel centres it there.
  spec.align = PhiPopoverAlign::kCenterIfWider;
  CHECK(PhiPlacePopover(R(300, 200, 400, 224), 328, 99, kWork, spec).left ==
        300);
  CHECK(PhiPlacePopover(R(300, 200, 800, 224), 328, 99, kWork, spec).left ==
        386);
}

void PlaceRightTop() {
  std::printf("place: beside the row (tab preview, fit-conditional flip)\n");
  PhiPlacementSpec spec;
  spec.side = PhiPopoverSide::kRightTop;
  spec.margin = 4;
  spec.flip_only_if_fits = true;

  // A 280x275 card off a sidebar row.
  CHECK(PhiPlacePopover(R(20, 300, 240, 336), 280, 275, kWork, spec) ==
        R(240, 300, 520, 575));
  // No room on the right, room on the left: flips.
  CHECK(PhiPlacePopover(R(1600, 300, 1800, 336), 280, 275, kWork, spec) ==
        R(1320, 300, 1600, 575));
  // No room on either side: stays put and clamps, because the flip target
  // does not fit (CustomTooltipGeometry's rule, and the one place it
  // differs from the vertical flip above).
  const PhiRectI narrow = {0, 0, 400, 1040};
  CHECK(PhiPlacePopover(R(100, 300, 380, 336), 280, 275, narrow, spec) ==
        R(116, 300, 396, 575));
  // The margin is honoured on the bottom edge too.
  CHECK(PhiPlacePopover(R(20, 1000, 240, 1036), 280, 275, kWork, spec)
            .bottom == 1036);
}

void PlaceExplicit() {
  std::printf("place: explicit, unclamped (toast, HUD, search palette)\n");
  PhiPlacementSpec spec;
  spec.side = PhiPopoverSide::kExplicit;
  spec.clamp = false;
  // The three owner-placed surfaces keep the exact position they compute,
  // even when the owner hangs off the screen.
  CHECK(PhiPlacePopover(R(-200, -50, -200, -50), 680, 520, kWork, spec) ==
        R(-200, -50, 480, 470));
  spec.clamp = true;
  CHECK(PhiPlacePopover(R(-200, -50, -200, -50), 680, 520, kWork, spec) ==
        R(0, 0, 680, 520));
}

void Timers() {
  std::printf("timers: lowest free id from the per-window pool\n");
  PhiTimerPool pool;
  const unsigned a = pool.Alloc();
  const unsigned b = pool.Alloc();
  const unsigned c = pool.Alloc();
  CHECK(a == kPhiTimerIdBase);
  CHECK(b == kPhiTimerIdBase + 1);
  CHECK(c == kPhiTimerIdBase + 2);
  CHECK(pool.live() == 3);
  CHECK(pool.InUse(b));

  pool.Release(b);
  CHECK(!pool.InUse(b));
  CHECK(pool.live() == 2);
  CHECK(pool.Alloc() == b);  // The hole is refilled before the tail grows.
  CHECK(pool.Alloc() == kPhiTimerIdBase + 3);

  // Ids never collide with the legacy 0x515-0x542 cross-file space, and a
  // stray release outside the pool is a no-op.
  CHECK(kPhiTimerIdBase > 0x542u);
  pool.Release(1);
  pool.Release(kPhiTimerIdBase + 99);
  CHECK(pool.live() == 4);
}

void Easing() {
  std::printf("easing: clamped, monotonic, exact at the ends\n");
  for (int i = 0; i < 4; ++i) {
    const PhiEasing easing = static_cast<PhiEasing>(i);
    CHECK(PhiEase(easing, 0.0) == 0.0);
    CHECK(PhiEase(easing, 1.0) == 1.0);
    CHECK(PhiEase(easing, -3.0) == 0.0);
    CHECK(PhiEase(easing, 7.0) == 1.0);
    double previous = -1.0;
    for (int step = 0; step <= 20; ++step) {
      const double value = PhiEase(easing, step / 20.0);
      CHECK(value >= previous);
      previous = value;
    }
  }
  CHECK(PhiEase(PhiEasing::kLinear, 0.25) == 0.25);
  CHECK(PhiEase(PhiEasing::kEaseIn, 0.5) == 0.25);
  CHECK(PhiEase(PhiEasing::kEaseOut, 0.5) == 0.75);
  CHECK(PhiEase(PhiEasing::kEaseInOut, 0.5) == 0.5);
  CHECK(PhiEase(PhiEasing::kEaseInOut, 0.25) == 0.125);

  std::printf("lerp: nearest pixel, ties away from the start\n");
  CHECK(PhiLerpInt(0, 100, 0.0) == 0);
  CHECK(PhiLerpInt(0, 100, 1.0) == 100);
  CHECK(PhiLerpInt(0, 1, 0.4) == 0);
  CHECK(PhiLerpInt(0, 1, 0.5) == 1);   // The tie lands on the far pixel...
  CHECK(PhiLerpInt(1, 0, 0.5) == 0);   // ...whichever way the slide runs.
  CHECK(PhiLerpInt(100, 0, 0.5) == 50);
  CHECK(PhiLerpInt(100, 0, 0.999) == 0);
}

}  // namespace

int main() {
  Rects();
  Hits();
  PlaceBelow();
  PlaceAbove();
  PlaceCentring();
  PlaceRightTop();
  PlaceExplicit();
  Timers();
  Easing();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("ui substrate: all passed\n");
  return 0;
}
