// Host test for the agent Space switch route (client/phi_agent_space_route.h)
// — the Windows twin of the Mac's AgentSpaceSwitchRouteTests. Run with
// test/model/run.sh.
//
// An agent task operates one browser, shown in ONE user window at a time:
// the slot that hosts it for a watch. That slot alone presents the agent
// Space — its strip and switcher offer the pip, every other window's do not
// — and a switch that resolves against another slot (the handoff prompt,
// Agent Autoview, a stale strip) surfaces the Space in the hosting window
// rather than moving the browser there. The rule used to be "end whatever
// watch is up, then host it here": asked from the hosting window that was
// the toggle-off, asked from another it ripped the page out of the hosting
// window's frame. Pinning the route by table keeps a later "just take it"
// from creeping back in.
//
// What the table cannot reach: that PhiAgentSpaceManager::RequestWatch
// consults it ahead of every side effect, that Presents reads the same
// answer, and that the in-flight half (a swap back that has not landed)
// counts as hosting. Those rest on the comments at each site; the manager
// itself needs the shells and stays on the target.

#include <cstdio>
#include <cstdlib>

#include "phibrowser-win/client/phi_agent_space_route.h"

using phi_client::PhiAgentSpaceSwitchRoute;
using phi_client::PhiAgentSpaceSwitchRouteFor;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

PhiAgentSpaceSwitchRoute Route(bool hosts_here = false,
                               bool another_hosts = false,
                               bool another_in_flight = false) {
  return PhiAgentSpaceSwitchRouteFor(hosts_here, another_hosts,
                                     another_in_flight);
}

void TestHostingSlotSwitchesLocally() {
  CHECK(Route(/*hosts_here=*/true) == PhiAgentSpaceSwitchRoute::kLocal);
}

void TestHostingSlotWinsEvenIfAnotherSlotClaimsTheBrowser() {
  // Two hosts cannot happen — a browser sits in one frame — but the asking
  // slot's own claim must never be redirected away from itself.
  CHECK(Route(/*hosts_here=*/true, /*another_hosts=*/true) ==
        PhiAgentSpaceSwitchRoute::kLocal);
  CHECK(Route(/*hosts_here=*/true, /*another_hosts=*/false,
              /*another_in_flight=*/true) ==
        PhiAgentSpaceSwitchRoute::kLocal);
}

void TestBrowserHostedElsewhereSurfacesInThatSlot() {
  CHECK(Route(/*hosts_here=*/false, /*another_hosts=*/true) ==
        PhiAgentSpaceSwitchRoute::kSurfaceInHost);
}

void TestInFlightElsewhereIsDropped() {
  CHECK(Route(/*hosts_here=*/false, /*another_hosts=*/false,
              /*another_in_flight=*/true) ==
        PhiAgentSpaceSwitchRoute::kDropWhileInFlight);
}

void TestLiveWatchOutranksInFlightClaim() {
  CHECK(Route(/*hosts_here=*/false, /*another_hosts=*/true,
              /*another_in_flight=*/true) ==
        PhiAgentSpaceSwitchRoute::kSurfaceInHost);
}

void TestUnhostedAgentSpaceWatchesLocally() {
  // Parked in its own agent shell, nobody watching: the slot that asks
  // hosts it, exactly as before.
  CHECK(Route() == PhiAgentSpaceSwitchRoute::kLocal);
}

}  // namespace

int main() {
  TestHostingSlotSwitchesLocally();
  TestHostingSlotWinsEvenIfAnotherSlotClaimsTheBrowser();
  TestBrowserHostedElsewhereSurfacesInThatSlot();
  TestInFlightElsewhereIsDropped();
  TestLiveWatchOutranksInFlightClaim();
  TestUnhostedAgentSpaceWatchesLocally();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("agent_space_route_test: all passed\n");
  return EXIT_SUCCESS;
}
