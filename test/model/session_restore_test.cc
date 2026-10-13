// Host tests for the lazy-restore service's pure rules (docs/architecture-
// review.md §3.4, phase 4: phi_session_restore.h). Run with
// test/model/run.sh.
//
// Two decisions are pure and are the ones a regression would be silent
// about: which settle reason a reopen receipt names (it is what decides
// whether the session gets a rescue shell) and which Space the cold start
// rebuilds eagerly. Everything else in the service — the ghost store, the
// framework's DropGhostWindow calls, the fallback timer — needs the
// framework and the window registry and is exercised only in the client
// build.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "phibrowser-win/client/phi_session_restore.h"

using phi_client::PhiSessionRestore;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

// The receipt's verdict names the settle reason. An armed eager set that
// matched nothing parks the whole session with no window up; a replay that
// never restored anything leaves the same hole. Both must be distinguishable
// from an ordinary receipt in the log, and both reach the same rescue.
void TestSettleReason() {
  CHECK(std::string(PhiSessionRestore::SettleReasonFor(false, true)) ==
        "receipt");
  CHECK(std::string(PhiSessionRestore::SettleReasonFor(false, false)) ==
        "nothingrestored");
  // matchedNothing wins over restoredAny either way: it is the more precise
  // account of the same emptiness.
  CHECK(std::string(PhiSessionRestore::SettleReasonFor(true, true)) ==
        "matchednothing");
  CHECK(std::string(PhiSessionRestore::SettleReasonFor(true, false)) ==
        "matchednothing");
}

// The last-active Space: the live window always wins (a reopen mid-run must
// not be answered from the last run's file); a remembered id that no longer
// names a Space falls back to the default, never to an id nothing can open.
void TestLastActiveSpace() {
  CHECK(PhiSessionRestore::ResolveLastActiveSpace("3", "7", true, "1") == "3");
  // Even when the live Space is not the remembered one and the remembered
  // one still exists.
  CHECK(PhiSessionRestore::ResolveLastActiveSpace("3", "7", true, "7") == "3");
  // Cold start: no live window, the file's id still names a Space.
  CHECK(PhiSessionRestore::ResolveLastActiveSpace("", "7", true, "1") == "7");
  // The remembered Space was deleted between runs.
  CHECK(PhiSessionRestore::ResolveLastActiveSpace("", "7", false, "1") == "1");
  // Nothing remembered at all (a first run).
  CHECK(PhiSessionRestore::ResolveLastActiveSpace("", "", false, "1") == "1");
  // An empty remembered id is never returned even if "it exists".
  CHECK(PhiSessionRestore::ResolveLastActiveSpace("", "", true, "2") == "2");
}

// Release review B6: a Space's newly noted window stands BESIDE the
// remembered primary while that primary still holds tabs (parked, on its
// way from the eager set, live in another shell) — overwriting it got the
// primary orphan-dropped on the next restore. Otherwise it takes the row.
void TestKeepsPrimary() {
  // Args: has_primary, is_this_window, parked, awaited, live_elsewhere.
  CHECK(!PhiSessionRestore::KeepsPrimary(false, false, false, false, false));
  // Re-noting the primary itself never demotes it.
  CHECK(!PhiSessionRestore::KeepsPrimary(true, true, true, true, true));
  // A second window (Ctrl+N, tear-off) while the first shows the Space.
  CHECK(PhiSessionRestore::KeepsPrimary(true, false, false, false, true));
  // A window noted while the Space's own window sits parked.
  CHECK(PhiSessionRestore::KeepsPrimary(true, false, true, false, false));
  // A sibling restored before the Space's eager window arrived.
  CHECK(PhiSessionRestore::KeepsPrimary(true, false, false, true, false));
  // The primary is gone (closed, never coming back): the new window takes
  // the row.
  CHECK(!PhiSessionRestore::KeepsPrimary(true, false, false, false, false));
}

}  // namespace

int main() {
  TestSettleReason();
  TestLastActiveSpace();
  TestKeepsPrimary();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("session_restore_test: all passed\n");
  return EXIT_SUCCESS;
}
