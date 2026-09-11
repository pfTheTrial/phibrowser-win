// Host tests for the onboarding's analytics session (phi_oobe_analytics.h),
// the port of the Mac's OOBEAnalyticsSession — the same cases as its
// OOBEAnalyticsSessionTests, so the two funnels are proved to agree on
// names, indices and the clock discipline. Run with test/model/run.sh.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_oobe_analytics.h"

using namespace phi_client;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

using Step = PhiOobeAnalyticsSession::Step;
using Event = PhiOobeAnalyticsSession::Event;

struct Recorder {
  std::vector<Event> events;
  PhiOobeAnalyticsSession session{
      [this](const Event& event) { events.push_back(event); }};
  std::vector<std::string> names() const {
    std::vector<std::string> out;
    for (const Event& event : events) {
      out.emplace_back(event.name);
    }
    return out;
  }
};

void TestStepLifecycleCapturesViewAndCompletion() {
  Recorder r;
  r.session.Present(Step::kLogin, 10);
  r.session.CompleteCurrentStep(13.5);
  CHECK(r.names() ==
        std::vector<std::string>({"oobe_step_viewed", "oobe_step_completed"}));
  CHECK(std::string(r.events[0].step) == "login");
  CHECK(r.events[0].step_index == 1);
  CHECK(!r.events[0].is_guest);
  CHECK(r.events[1].duration_seconds == 3.5);
}

void TestStepMappingUsesStableNamesAndIndices() {
  struct Row {
    Step step;
    const char* name;
    int index;
    bool guest;
  };
  const Row rows[] = {
      {Step::kLogin, "login", 1, false},
      {Step::kSetName, "set_name", 2, false},
      {Step::kSetTheme, "set_theme", 3, false},
      {Step::kLayoutSelection, "layout_selection", 4, false},
      {Step::kPasswordManager, "password_manager", 5, false},
      {Step::kNextStep, "next_step", 6, false},
      {Step::kGuestPrivacy, "guest_privacy", 2, true},
  };
  for (const Row& row : rows) {
    CHECK(std::string(PhiOobeAnalyticsSession::StepName(row.step)) == row.name);
    CHECK(PhiOobeAnalyticsSession::StepIndex(row.step) == row.index);
    CHECK(PhiOobeAnalyticsSession::StepIsGuest(row.step) == row.guest);
  }
}

void TestPresentingSameStepTwiceDoesNotDuplicateView() {
  Recorder r;
  r.session.Present(Step::kSetTheme, 10);
  r.session.Present(Step::kSetTheme, 20);
  CHECK(r.names() == std::vector<std::string>({"oobe_step_viewed"}));
}

void TestGuestFinishCompletesCurrentStepAndCapturesSummary() {
  Recorder r;
  r.session.Present(Step::kLogin, 10);
  r.session.CompleteCurrentStep(12);
  r.session.Present(Step::kGuestPrivacy, 12);
  r.session.Finish(/*is_guest=*/true, 2, 15);
  CHECK(r.names() ==
        std::vector<std::string>({"oobe_step_viewed", "oobe_step_completed",
                                  "oobe_step_viewed", "oobe_step_completed",
                                  "oobe_finished"}));
  const Event& summary = r.events[4];
  CHECK(summary.is_guest);
  CHECK(summary.steps_completed == 2);
  CHECK(summary.total_duration_seconds == 5);
  CHECK(r.session.ended());
}

void TestAuthenticatedFinishUsesLastPresentedStepIndex() {
  Recorder r;
  r.session.Present(Step::kNextStep, 8);
  r.session.Finish(/*is_guest=*/false, std::nullopt, 10);
  const Event& summary = r.events.back();
  CHECK(std::string(summary.name) == "oobe_finished");
  CHECK(summary.steps_completed == 6);
  CHECK(!summary.is_guest);
  CHECK(summary.total_duration_seconds == 2);
}

void TestInterruptionCapturesLastStepAndAccumulatedDuration() {
  Recorder r;
  r.session.Present(Step::kLogin, 10);
  r.session.CompleteCurrentStep(12);
  r.session.Present(Step::kSetName, 12);
  r.session.Interrupt(PhiOobeAnalyticsSession::Interruption::kWindowClosed,
                      15);
  const Event& interruption = r.events.back();
  CHECK(std::string(interruption.name) == "oobe_interrupted");
  CHECK(std::string(interruption.step) == "set_name");
  CHECK(interruption.step_index == 2);
  CHECK(std::string(interruption.reason) == "window_closed");
  CHECK(interruption.total_duration_seconds == 5);
  // Ended: a later termination reports nothing more.
  r.session.Interrupt(PhiOobeAnalyticsSession::Interruption::kAppTerminated,
                      20);
  CHECK(r.events.size() == 4);
}

void TestSuppressionPreventsInterruptionCapture() {
  Recorder r;
  r.session.Present(Step::kLogin, 10);
  r.session.SuppressInterruption();
  r.session.Interrupt(PhiOobeAnalyticsSession::Interruption::kWindowClosed,
                      20);
  CHECK(r.names() == std::vector<std::string>({"oobe_step_viewed"}));
}

// A step-back (Cancel on the sign-in page) pauses the step without
// completing it: its time counts toward the total, no completion fires,
// and interrupting with nothing ever presented reports nothing.
void TestStepBackPausesWithoutCompletion() {
  Recorder r;
  r.session.Interrupt(PhiOobeAnalyticsSession::Interruption::kWindowClosed,
                      1);
  CHECK(r.events.empty());
  r.session.Present(Step::kLogin, 10);
  r.session.Present(Step::kGuestPrivacy, 14);
  r.session.Present(Step::kLogin, 20);
  CHECK(r.names() ==
        std::vector<std::string>({"oobe_step_viewed", "oobe_step_viewed",
                                  "oobe_step_viewed"}));
  r.session.Finish(/*is_guest=*/false, std::nullopt, 21);
  CHECK(r.events.back().total_duration_seconds == 11);
  CHECK(r.events.back().steps_completed == 1);
}

}  // namespace

int main() {
  TestStepLifecycleCapturesViewAndCompletion();
  TestStepMappingUsesStableNamesAndIndices();
  TestPresentingSameStepTwiceDoesNotDuplicateView();
  TestGuestFinishCompletesCurrentStepAndCapturesSummary();
  TestAuthenticatedFinishUsesLastPresentedStepIndex();
  TestInterruptionCapturesLastStepAndAccumulatedDuration();
  TestSuppressionPreventsInterruptionCapture();
  TestStepBackPausesWithoutCompletion();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("oobe_analytics_test: all passed\n");
  return EXIT_SUCCESS;
}
