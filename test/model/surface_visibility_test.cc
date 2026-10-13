// Host test for the attached-surface visibility rules
// (client/phi_surface_visibility.h) — the Mac's visiblePeek and
// visibleReaderOverlays (phibrowser-mac 89dd8a48). Run with
// test/model/run.sh.
//
// A peek is bound to ONE opener tab but covers that tab's whole split, so it
// stays up whichever pane is focused; a reader covers only its origin's pane,
// so both panes' readers show side by side; a peek in front hides every
// reader except one standing over the peek (or, for a single tab, the
// opener's own). What the table cannot reach: that the coordinator feeds it
// the window's active tab and its split partner from the window model, and
// shows/hides the panels by its answer
// (PhiSurfaceCoordinator::UpdateReaderPanelsForWindow).

#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "phibrowser-win/client/phi_surface_visibility.h"

using phi_client::PhiVisiblePeek;
using phi_client::PhiVisiblePeekFor;
using phi_client::PhiVisibleReaderOrigins;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

using Ids = std::vector<int>;

void TestVisiblePeek() {
  const std::map<int, int> none;
  CHECK(!PhiVisiblePeekFor(0, 0, none).valid());
  CHECK(!PhiVisiblePeekFor(1, 0, none).valid());

  // A single tab sees only its own peek.
  const std::map<int, int> on_one = {{1, 101}};
  PhiVisiblePeek peek = PhiVisiblePeekFor(1, 0, on_one);
  CHECK(peek.valid() && peek.opener == 1 && peek.peek_tab == 101);
  CHECK(!PhiVisiblePeekFor(2, 0, on_one).valid());

  // In a split the partner's peek covers the focused pane too.
  peek = PhiVisiblePeekFor(2, 1, on_one);
  CHECK(peek.valid() && peek.opener == 1 && peek.peek_tab == 101);
  // ...but a tab outside that split does not see it.
  CHECK(!PhiVisiblePeekFor(3, 4, on_one).valid());

  // Both panes have one: the focused pane's own wins.
  const std::map<int, int> on_both = {{1, 101}, {2, 102}};
  peek = PhiVisiblePeekFor(2, 1, on_both);
  CHECK(peek.opener == 2 && peek.peek_tab == 102);
  peek = PhiVisiblePeekFor(1, 2, on_both);
  CHECK(peek.opener == 1 && peek.peek_tab == 101);

  // A peek whose own tab is not known yet is still on screen.
  const std::map<int, int> unknown_tab = {{1, 0}};
  peek = PhiVisiblePeekFor(2, 1, unknown_tab);
  CHECK(peek.valid() && peek.opener == 1 && peek.peek_tab == 0);
}

void TestVisibleReaders() {
  const std::map<int, int> no_peeks;
  // A single tab: its own reader, nobody else's.
  CHECK(PhiVisibleReaderOrigins(1, 0, no_peeks, {1, 2}) == Ids({1}));
  CHECK(PhiVisibleReaderOrigins(3, 0, no_peeks, {1, 2}).empty());
  CHECK(PhiVisibleReaderOrigins(0, 0, no_peeks, {1}).empty());

  // A split: each pane's reader, whichever pane is focused — moving focus
  // between panes leaves both up.
  CHECK(PhiVisibleReaderOrigins(1, 2, no_peeks, {1, 2}) == Ids({1, 2}));
  CHECK(PhiVisibleReaderOrigins(2, 1, no_peeks, {1, 2}) == Ids({2, 1}));
  CHECK(PhiVisibleReaderOrigins(2, 1, no_peeks, {1}) == Ids({1}));
  // A reader of a tab outside the split stays down.
  CHECK(PhiVisibleReaderOrigins(1, 2, no_peeks, {3}).empty());

  // A peek up over a single tab: the opener's own reader still shows (it
  // covers the card, peek included).
  const std::map<int, int> peek_on_one = {{1, 101}};
  CHECK(PhiVisibleReaderOrigins(1, 0, peek_on_one, {1}) == Ids({1}));
  // ...or the reader over the peek.
  CHECK(PhiVisibleReaderOrigins(1, 0, peek_on_one, {101}) == Ids({101}));

  // A peek up over a split hides both panes' readers, whichever pane opened
  // it and whichever is focused; only a reader over the peek shows.
  CHECK(PhiVisibleReaderOrigins(1, 2, peek_on_one, {1, 2}).empty());
  CHECK(PhiVisibleReaderOrigins(2, 1, peek_on_one, {1, 2}).empty());
  CHECK(PhiVisibleReaderOrigins(2, 1, peek_on_one, {1, 2, 101}) ==
        Ids({101}));
}

}  // namespace

int main() {
  TestVisiblePeek();
  TestVisibleReaders();
  if (g_failures) {
    std::fprintf(stderr, "surface_visibility_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("surface_visibility_test: all passed\n");
  return 0;
}
