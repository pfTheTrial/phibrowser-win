// Host tests for the pure half of the PANEL family (phi_panel.h,
// docs/architecture-review.md §3.6 / lane3-ui-substrate.md §6.2): the card
// stack's z-order rules and the shaped-layer geometry that replaces the two
// SetWindowRgn surfaces (the agent pill's capsule, the peek controls' chip
// union). The Win32 half of phi_panel.cc is behind #if defined(_WIN32) and
// never reaches the host.
//
// Both are worth pinning here. The stack replaces nine scattered
// SetWindowPos(HWND_TOP) calls whose combined rule ("last raised is
// topmost") was never written down; the coverage function has to agree
// pixel for pixel with phi_dwrite's, or a chip drawn by PhiFillRoundRectAA
// inside a layer shaped by this one gets two different edges.

#include <cmath>
#include <cstdio>
#include <vector>

#include "phi_panel.h"

using namespace phi_client;

namespace {

int g_failures = 0;
#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                               \
    }                                                             \
  } while (0)

PhiRectI R(int left, int top, int right, int bottom) {
  PhiRectI r;
  r.left = left;
  r.top = top;
  r.right = right;
  r.bottom = bottom;
  return r;
}

PhiLayerShape Chip(const PhiRectI& bounds, int radius) {
  PhiLayerShape shape;
  shape.bounds = bounds;
  shape.radius = radius;
  return shape;
}

// Stand-ins for the panels that share one shell's card: the ids are opaque
// to the model (HWNDs on the target).
constexpr uintptr_t kNtp = 1;
constexpr uintptr_t kBookmarks = 2;
constexpr uintptr_t kPeekShadow = 3;
constexpr uintptr_t kPeekCard = 4;
constexpr uintptr_t kFog = 5;

void StackJoinsAtTheTop() {
  std::printf("card stack: a panel joins on top and stays where it is\n");
  PhiCardStackModel stack;
  stack.Add(kNtp);
  stack.Add(kBookmarks);
  CHECK(stack.size() == 2);
  CHECK(stack.IndexOf(kNtp) == 0);      // Bottom.
  CHECK(stack.IndexOf(kBookmarks) == 1);  // Top.
  // Mounting twice must not reorder what the user is looking at.
  stack.Add(kNtp);
  CHECK(stack.size() == 2);
  CHECK(stack.IndexOf(kNtp) == 0);
  CHECK(!stack.Contains(kFog));
  CHECK(stack.IndexOf(kFog) == -1);
  // A null id is not a window.
  stack.Add(0);
  CHECK(stack.size() == 2);
}

void StackRaiseIsLastWins() {
  std::printf("card stack: Raise() is 'last raised is topmost'\n");
  PhiCardStackModel stack;
  stack.Add(kNtp);
  stack.Add(kBookmarks);
  stack.Add(kFog);
  stack.Raise(kNtp);
  const std::vector<uintptr_t> expected = {kBookmarks, kFog, kNtp};
  CHECK(stack.order() == expected);
  // Raising the one already on top is a no-op, not a rotation.
  stack.Raise(kNtp);
  CHECK(stack.order() == expected);
  // Raising a non-member adds it, which is what a panel shown for the
  // first time does.
  stack.Raise(kPeekCard);
  CHECK(stack.IndexOf(kPeekCard) == 3);
  CHECK(stack.size() == 4);
}

void StackReproducesPeekOrder() {
  std::printf("card stack: shadow then card is the peek's own order\n");
  // PhiPeekPanel::Raise() lifts the shadow first and the card second, so
  // the card never ends up under its own shadow. Applied bottom-to-top,
  // the list has to say exactly that.
  PhiCardStackModel stack;
  stack.Add(kNtp);
  stack.Raise(kPeekShadow);
  stack.Raise(kPeekCard);
  CHECK(stack.IndexOf(kPeekShadow) < stack.IndexOf(kPeekCard));
  CHECK(stack.IndexOf(kNtp) == 0);
  // A panel that goes away leaves the rest in order.
  stack.Remove(kPeekShadow);
  stack.Remove(kPeekCard);
  CHECK(stack.size() == 1);
  CHECK(stack.IndexOf(kNtp) == 0);
  // Removing what is not there is a no-op.
  stack.Remove(kPeekCard);
  CHECK(stack.size() == 1);
  stack.Clear();
  CHECK(stack.empty());
}

void ShapeDistance() {
  std::printf("layer shape: signed distance, negative inside\n");
  const PhiLayerShape square = Chip(R(0, 0, 100, 40), 0);
  CHECK(PhiLayerShapeDistance(square, 50, 20) < 0.0);   // Middle.
  CHECK(PhiLayerShapeDistance(square, -5, 20) > 0.0);   // Left of it.
  CHECK(PhiLayerShapeDistance(square, 105, 20) > 0.0);  // Right of it.
  // A capsule: radius clamps to half the short side, so a corner point sits
  // outside the curve even though it is inside the bounding box.
  const PhiLayerShape capsule = Chip(R(0, 0, 100, 40), 20);
  CHECK(PhiLayerShapeDistance(capsule, 0.5, 0.5) > 0.0);
  CHECK(PhiLayerShapeDistance(capsule, 50, 20) < 0.0);
  // A degenerate shape is infinitely far from everything.
  CHECK(PhiLayerShapeDistance(Chip(R(0, 0, 0, 0), 4), 0, 0) > 1e8);
}

void ShapeCoverage() {
  std::printf("layer shape: half-pixel ramp, union takes the max\n");
  std::vector<PhiLayerShape> shapes;
  shapes.push_back(Chip(R(0, 0, 100, 40), 0));
  // Deep inside is opaque, well outside is nothing, and ON the edge is a
  // half — phi_dwrite's FillCoverage(0.5 - d), which is what makes a
  // layer's edge a ramp instead of a staircase.
  CHECK(PhiLayerShapesCoverage(shapes, 50, 20) == 1.0);
  CHECK(PhiLayerShapesCoverage(shapes, -2, 20) == 0.0);
  CHECK(std::fabs(PhiLayerShapesCoverage(shapes, 0.0, 20) - 0.5) < 1e-9);
  CHECK(std::fabs(PhiLayerShapesCoverage(shapes, 100.0, 20) - 0.5) < 1e-9);
  // An empty silhouette covers nothing at all.
  CHECK(PhiLayerShapesCoverage({}, 10, 10) == 0.0);

  // The peek's three chips: 28px squares of radius 8, stacked with 6px of
  // air between them. The gaps must be UNCOVERED — that air is the whole
  // point of the region union this replaces.
  std::vector<PhiLayerShape> chips;
  chips.push_back(Chip(R(8, 6, 36, 34), 8));
  chips.push_back(Chip(R(8, 40, 36, 68), 8));
  chips.push_back(Chip(R(8, 74, 36, 102), 8));
  CHECK(PhiLayerShapesCoverage(chips, 22, 20) == 1.0);   // In the first.
  CHECK(PhiLayerShapesCoverage(chips, 22, 54) == 1.0);   // In the second.
  CHECK(PhiLayerShapesCoverage(chips, 22, 88) == 1.0);   // In the third.
  CHECK(PhiLayerShapesCoverage(chips, 22, 37) == 0.0);   // Between 1 and 2.
  CHECK(PhiLayerShapesCoverage(chips, 2, 20) == 0.0);    // Left of them all.
  // A chip's rounded corner is cut away, the way the region cut it.
  CHECK(PhiLayerShapesCoverage(chips, 8.5, 6.5) < 0.5);
}

void ShapeHitTest() {
  std::printf("layer shape: hit test follows the silhouette\n");
  std::vector<PhiLayerShape> chips;
  chips.push_back(Chip(R(8, 6, 36, 34), 8));
  chips.push_back(Chip(R(8, 40, 36, 68), 8));
  CHECK(PhiLayerShapesContain(chips, 22, 20));
  CHECK(PhiLayerShapesContain(chips, 22, 54));
  // The air between the chips is the page's, not the layer's: this is what
  // answers HTTRANSPARENT so a click there dismisses the peek.
  CHECK(!PhiLayerShapesContain(chips, 22, 37));
  CHECK(!PhiLayerShapesContain(chips, 0, 0));
  // The pixel CENTRE decides, so the test and the coverage agree on the
  // same boundary — a pixel is "in" exactly when it is at least half lit.
  const std::vector<PhiLayerShape> square = {Chip(R(0, 0, 10, 10), 0)};
  CHECK(PhiLayerShapesContain(square, 0, 0));
  CHECK(PhiLayerShapesContain(square, 9, 9));
  CHECK(!PhiLayerShapesContain(square, 10, 5));
  CHECK(!PhiLayerShapesContain(square, -1, 5));
}

}  // namespace

int main() {
  StackJoinsAtTheTop();
  StackRaiseIsLastWins();
  StackReproducesPeekOrder();
  ShapeDistance();
  ShapeCoverage();
  ShapeHitTest();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("panel: all passed\n");
  return 0;
}
