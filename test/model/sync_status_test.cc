// Host tests for the Sync pane's status card (client/phi_sync_status.h): the
// Mac's SyncStatusSummary.reduce and ChromiumSyncStatus.decode cases.

#include <cstdio>
#include <cstdlib>

#include "phibrowser-win/client/phi_sync_status.h"

using namespace phi_client::sync;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

ContextStatus Ctx(const char* id, SummaryPhase phase,
                  std::optional<int64_t> last = std::nullopt,
                  uint64_t revision = 1) {
  return {id, phase, last, revision};
}

}  // namespace

int main() {
  const std::set<std::string> required = {"phi", "Default"};
  CHECK(ReduceStatus(false, required, {}).phase == SummaryPhase::kNotStarted);
  // A missing context keeps the card checking.
  CHECK(ReduceStatus(true, required,
                     {Ctx("phi", SummaryPhase::kUpToDate, 100)})
            .phase == SummaryPhase::kChecking);
  // All up to date: the oldest success is shown.
  StatusSummary up = ReduceStatus(
      true, required,
      {Ctx("phi", SummaryPhase::kUpToDate, 200),
       Ctx("Default", SummaryPhase::kUpToDate, 100)});
  CHECK(up.phase == SummaryPhase::kUpToDate && up.last_success_ms == 100);
  // needsAttention wins over everything; offline over syncing.
  CHECK(ReduceStatus(true, required,
                     {Ctx("phi", SummaryPhase::kNeedsAttention),
                      Ctx("Default", SummaryPhase::kChecking)})
            .phase == SummaryPhase::kNeedsAttention);
  CHECK(ReduceStatus(true, required,
                     {Ctx("phi", SummaryPhase::kSyncing, 5),
                      Ctx("Default", SummaryPhase::kOffline, 5)})
            .phase == SummaryPhase::kOffline);
  // An older revision of a context never replaces a newer one.
  CHECK(ReduceStatus(true, {"phi"},
                     {Ctx("phi", SummaryPhase::kSyncing, 5, 3),
                      Ctx("phi", SummaryPhase::kUpToDate, 9, 2)})
            .phase == SummaryPhase::kSyncing);
  // Decode: upToDate without a time is checking; a future time is dropped;
  // an unknown version is checking.
  CHECK(DecodeProfileStatus("Default", 1, "upToDate", std::nullopt, 1000)
            .phase == SummaryPhase::kChecking);
  CHECK(!DecodeProfileStatus("Default", 1, "syncing", 1e15, 1000)
             .last_success_ms);
  CHECK(DecodeProfileStatus("Default", 2, "upToDate", 500.0, 1000).phase ==
        SummaryPhase::kChecking);
  const ContextStatus ok =
      DecodeProfileStatus("Default", 1, "upToDate", 500.0, 1000);
  CHECK(ok.phase == SummaryPhase::kUpToDate && ok.last_success_ms == 500);
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("sync_status_test: all passed\n");
  return EXIT_SUCCESS;
}
