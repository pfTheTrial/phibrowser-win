// Host tests for the pure half of the content host
// (docs/architecture-review.md §3.5, lane 1 clusters E and G): the table of
// which browser backs each Space in one slot, and which of them the slot is
// showing. The rest of PhiBrowserHost is Win32 — adoption, the corner caps,
// the status bubble — and stays on the target;
// PHI_BROWSER_HOST_MODEL_ONLY compiles the half that is not.

#define PHI_BROWSER_HOST_MODEL_ONLY

#include <cstdio>

#include "phi_browser_host.h"

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

BrowserId Id(int64_t value) {
  return BrowserId{value};
}

void AttachBindsAndShows() {
  PhiSpaceBrowsers spaces;
  CHECK(spaces.empty());
  CHECK(!spaces.active().valid());  // No browser: NEVER the slot number.

  spaces.Attach("work", Id(7));
  CHECK(!spaces.empty());
  CHECK(spaces.Has("work"));
  CHECK(spaces.ForSpace("work") == Id(7));
  CHECK(spaces.SpaceOf(Id(7)) == "work");
  CHECK(spaces.active() == Id(7));
  CHECK(spaces.active_space() == "work");
  CHECK(spaces.order().size() == 1u);

  // A second Space becomes the shown one and joins the order.
  spaces.Attach("home", Id(9));
  CHECK(spaces.active() == Id(9));
  CHECK(spaces.active_space() == "home");
  CHECK(spaces.order()[0] == "work");
  CHECK(spaces.order()[1] == "home");

  // Re-attaching a Space (its browser was replaced) keeps its place.
  spaces.Attach("work", Id(11));
  CHECK(spaces.order().size() == 2u);
  CHECK(spaces.order()[0] == "work");
  CHECK(spaces.ForSpace("work") == Id(11));
  CHECK(spaces.active() == Id(11));
}

void UnknownSpacesAnswerEmpty() {
  PhiSpaceBrowsers spaces;
  spaces.Attach("work", Id(7));
  CHECK(!spaces.Has("gone"));
  CHECK(!spaces.ForSpace("gone").valid());
  CHECK(spaces.SpaceOf(Id(42)).empty());
  CHECK(!spaces.Show("gone"));
  CHECK(spaces.active_space() == "work");  // A failed Show changes nothing.
}

void ShowMovesTheSlotWithoutTouchingTheOrder() {
  PhiSpaceBrowsers spaces;
  spaces.Attach("work", Id(7));
  spaces.Attach("home", Id(9));
  CHECK(spaces.Show("work"));
  CHECK(spaces.active() == Id(7));
  CHECK(spaces.active_space() == "work");
  CHECK(spaces.order()[0] == "work");
  CHECK(spaces.order()[1] == "home");
}

void ForgetDropsTheBrowserAndEverySpaceItBacked() {
  PhiSpaceBrowsers spaces;
  spaces.Attach("work", Id(7));
  spaces.Attach("home", Id(9));
  // Forgetting one the slot is NOT showing leaves the shown one alone.
  CHECK(!spaces.Forget(Id(7)));
  CHECK(!spaces.Has("work"));
  CHECK(spaces.order().size() == 1u);
  CHECK(spaces.active() == Id(9));

  // Forgetting the shown one says so, so the caller shows another.
  CHECK(spaces.Forget(Id(9)));
  CHECK(spaces.empty());
  CHECK(!spaces.active().valid());
}

void ForgettingAStrangerChangesNothing() {
  PhiSpaceBrowsers spaces;
  spaces.Attach("work", Id(7));
  CHECK(!spaces.Forget(Id(99)));
  CHECK(spaces.Has("work"));
  CHECK(spaces.active() == Id(7));
}

void TheActiveBrowserFallsBackToTheFirstStillAttached() {
  // The slot between its shown browser going and the next being shown: the
  // answer is another Space's browser, in attach order — never the slot
  // number (phase 1 removed that fallback, and it stays removed).
  PhiSpaceBrowsers spaces;
  spaces.Attach("work", Id(7));
  spaces.Attach("home", Id(9));
  spaces.Attach("play", Id(11));
  CHECK(spaces.Forget(Id(11)));  // The shown one.
  CHECK(spaces.active() == Id(7));
  CHECK(spaces.Show("home"));
  CHECK(spaces.active() == Id(9));
}

}  // namespace

int main() {
  AttachBindsAndShows();
  UnknownSpacesAnswerEmpty();
  ShowMovesTheSlotWithoutTouchingTheOrder();
  ForgetDropsTheBrowserAndEverySpaceItBacked();
  ForgettingAStrangerChangesNothing();
  TheActiveBrowserFallsBackToTheFirstStillAttached();
  if (g_failures == 0) {
    std::printf("browser_host_test: all checks passed\n");
  }
  return g_failures == 0 ? 0 : 1;
}
