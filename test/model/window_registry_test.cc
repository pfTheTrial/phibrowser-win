// Host tests for PhiWindowRegistry (docs/architecture-review.md §3.4). Run
// with test/model/run.sh. The registry only forward-declares the two window
// types it owns, so this file supplies stand-ins: what the registry does
// with them is store, hand back and delete.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "phibrowser-win/client/phi_window_registry.h"

namespace phi_client {

// Stand-ins for the real (Win32) classes. Each counts its destructions so
// the tests can assert the registry deletes what it owns, exactly once.
int g_controllers_destroyed = 0;
int g_kiosks_destroyed = 0;

class PhiShellWindow {
 public:
  explicit PhiShellWindow(int tag) : tag_(tag) {}
  ~PhiShellWindow() { ++g_controllers_destroyed; }
  int tag() const { return tag_; }

 private:
  int tag_;
};

class PhiKioskWindow {
 public:
  ~PhiKioskWindow() { ++g_kiosks_destroyed; }
};

}  // namespace phi_client

using namespace phi_client;
using phi_framework::RequestToken;
using phi_framework::WindowKind;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

void TestSlotsAndBindings() {
  g_controllers_destroyed = 0;
  {
    PhiWindowRegistry registry;
    CHECK(registry.empty());
    CHECK(!registry.FirstUserSlot().valid());
    CHECK(registry.FirstUserController() == nullptr);

    PhiShellWindow* a = registry.Add(
        SlotId{10}, std::make_unique<PhiShellWindow>(10), false, false);
    CHECK(a && a->tag() == 10);
    CHECK(registry.Get(SlotId{10}) == a);
    CHECK(registry.Has(SlotId{10}));
    CHECK(!registry.Has(SlotId{11}));
    CHECK(registry.size() == 1);

    // The opening browser is bound to the slot it opened; a Space browser
    // attached later resolves to the same slot.
    registry.Bind(BrowserId{10}, SlotId{10});
    registry.Bind(BrowserId{42}, SlotId{10});
    CHECK(registry.SlotFor(BrowserId{10}) == SlotId{10});
    CHECK(registry.SlotFor(BrowserId{42}) == SlotId{10});
    CHECK(!registry.SlotFor(BrowserId{43}).has_value());
    CHECK(registry.ControllerForBrowser(BrowserId{42}) == a);
    CHECK(registry.ControllerForBrowser(BrowserId{43}) == nullptr);
    CHECK(registry.IsBound(BrowserId{42}));
    CHECK(registry.Browsers().size() == 2);

    // Unbind drops one browser and leaves the slot standing.
    registry.Unbind(BrowserId{42});
    CHECK(!registry.SlotFor(BrowserId{42}).has_value());
    CHECK(registry.Get(SlotId{10}) == a);
    CHECK(registry.Browsers().size() == 1);

    // Invalid ids never bind: a 0 browser or slot is "nothing".
    registry.Bind(BrowserId{}, SlotId{10});
    registry.Bind(BrowserId{7}, SlotId{});
    CHECK(registry.Browsers().size() == 1);

    // Remove destroys the controller and every binding to the slot.
    registry.Bind(BrowserId{42}, SlotId{10});
    CHECK(registry.Remove(SlotId{10}));
    CHECK(g_controllers_destroyed == 1);
    CHECK(registry.Get(SlotId{10}) == nullptr);
    CHECK(!registry.SlotFor(BrowserId{10}).has_value());
    CHECK(!registry.SlotFor(BrowserId{42}).has_value());
    CHECK(!registry.Remove(SlotId{10}));
    CHECK(registry.empty());

    registry.Add(SlotId{20}, std::make_unique<PhiShellWindow>(20), false,
                 false);
  }
  // The registry's destructor owns the rest.
  CHECK(g_controllers_destroyed == 2);
}

void TestFirstUserSlot() {
  PhiWindowRegistry registry;
  // Lowest id wins among USER slots: an agent window (which usually has the
  // lowest id when the skill opened first) and an incognito shell never
  // answer for "the window".
  registry.Add(SlotId{3}, std::make_unique<PhiShellWindow>(3),
               /*agent=*/true, false);
  registry.Add(SlotId{5}, std::make_unique<PhiShellWindow>(5), false,
               /*incognito=*/true);
  CHECK(!registry.FirstUserSlot().valid());
  CHECK(registry.UserControllers().empty());
  CHECK(registry.Controllers().size() == 2);

  registry.Add(SlotId{9}, std::make_unique<PhiShellWindow>(9), false,
               false);
  registry.Add(SlotId{7}, std::make_unique<PhiShellWindow>(7), false,
               false);
  CHECK(registry.FirstUserSlot() == SlotId{7});
  CHECK(registry.FirstUserController()->tag() == 7);
  CHECK(registry.UserControllers().size() == 2);
  CHECK(registry.UserControllers()[0]->tag() == 7);
  CHECK(registry.UserControllers()[1]->tag() == 9);
  CHECK(registry.IsAgentSlot(SlotId{3}));
  CHECK(!registry.IsAgentSlot(SlotId{7}));
  CHECK(registry.IsIncognitoSlot(SlotId{5}));
  CHECK(!registry.IsIncognitoSlot(SlotId{3}));

  // Iteration is in slot order, whatever the insertion order was.
  std::vector<PhiShellWindow*> all = registry.Controllers();
  CHECK(all.size() == 4);
  CHECK(all[0]->tag() == 3 && all[1]->tag() == 5 && all[2]->tag() == 7 &&
        all[3]->tag() == 9);

  registry.Remove(SlotId{7});
  CHECK(registry.FirstUserSlot() == SlotId{9});
}

void TestKiosks() {
  g_kiosks_destroyed = 0;
  {
    PhiWindowRegistry registry;
    PhiKioskWindow* k = registry.AddKiosk(BrowserId{77},
                                          std::make_unique<PhiKioskWindow>());
    CHECK(registry.Kiosk(BrowserId{77}) == k);
    CHECK(registry.Kiosk(BrowserId{78}) == nullptr);
    // A kiosk is not a slot and binds nothing.
    CHECK(registry.empty());
    CHECK(!registry.SlotFor(BrowserId{77}).has_value());
    CHECK(registry.RemoveKiosk(BrowserId{77}));
    CHECK(g_kiosks_destroyed == 1);
    CHECK(!registry.RemoveKiosk(BrowserId{77}));
    registry.AddKiosk(BrowserId{78}, std::make_unique<PhiKioskWindow>());
  }
  CHECK(g_kiosks_destroyed == 2);
}

void TestPendingAddTakeExpire() {
  PhiWindowRegistry registry;
  CHECK(!registry.HasPending());

  PendingCreation space;
  space.kind = WindowKind::kSpace;
  space.slot = SlotId{10};
  space.space_id = "2";
  space.open_urls = {"https://a.example/"};
  space.deadline_tick = 1000;
  registry.AddPending(RequestToken{1}, space);

  PendingCreation agent;
  agent.kind = WindowKind::kAgent;
  agent.task_id = "task-A";
  agent.pending_reply_id = 5;
  agent.deadline_tick = 2000;
  registry.AddPending(RequestToken{2}, agent);

  // An invalid token files nothing.
  registry.AddPending(RequestToken{}, agent);
  CHECK(registry.HasPending());

  // Two cold switches no longer cross: each token gets its own request.
  CHECK(registry.FindPendingSpace(SlotId{10}, "2") != nullptr);
  CHECK(registry.FindPendingSpace(SlotId{10}, "3") == nullptr);
  CHECK(registry.FindPendingSpace(SlotId{11}, "2") == nullptr);
  registry.FindPendingSpace(SlotId{10}, "2")->open_urls.push_back("https://b/");
  CHECK(registry.HasPendingTask("task-A"));
  CHECK(!registry.HasPendingTask("task-B"));
  CHECK(!registry.HasPendingTask(""));

  std::optional<PendingCreation> taken = registry.TakePending(RequestToken{2});
  CHECK(taken.has_value());
  CHECK(taken->kind == WindowKind::kAgent);
  CHECK(taken->task_id == "task-A");
  CHECK(taken->pending_reply_id == 5);
  // Taken is taken: the same token answers nothing twice, and an unknown
  // token answers nothing at all.
  CHECK(!registry.TakePending(RequestToken{2}).has_value());
  CHECK(!registry.TakePending(RequestToken{99}).has_value());
  CHECK(!registry.HasPendingTask("task-A"));

  // Expiry is by deadline: not yet at 999, gone at 1000.
  CHECK(registry.ExpirePending(999).empty());
  std::vector<PendingCreation> expired = registry.ExpirePending(1000);
  CHECK(expired.size() == 1);
  CHECK(expired.size() == 1 && expired[0].space_id == "2");
  CHECK(expired.size() == 1 && expired[0].open_urls.size() == 2);
  CHECK(!registry.TakePending(RequestToken{1}).has_value());
  CHECK(!registry.HasPending());

  // A request with no deadline never expires.
  PendingCreation forever;
  forever.kind = WindowKind::kShadow;
  forever.task_id = "shadow";
  registry.AddPending(RequestToken{3}, forever);
  CHECK(registry.ExpirePending(1u << 30).empty());
  CHECK(registry.HasPending());
}

void TestPendingGhosts() {
  PhiWindowRegistry registry;
  PendingCreation ghost;
  ghost.kind = WindowKind::kSpace;
  ghost.slot = SlotId{10};
  ghost.space_id = "4";
  ghost.deadline_tick = 500;
  // Keyed by the previous session's window id, which is what the
  // materialized window is announced with.
  registry.AddPendingGhost(BrowserId{31337}, ghost);
  registry.AddPendingGhost(BrowserId{}, ghost);  // Invalid: filed nowhere.
  CHECK(registry.HasPending());
  CHECK(!registry.TakePendingGhost(BrowserId{1}).has_value());
  std::optional<PendingCreation> taken =
      registry.TakePendingGhost(BrowserId{31337});
  CHECK(taken.has_value() && taken->space_id == "4" &&
        taken->slot == SlotId{10});
  CHECK(!registry.TakePendingGhost(BrowserId{31337}).has_value());
  CHECK(!registry.HasPending());

  registry.AddPendingGhost(BrowserId{5}, ghost);
  CHECK(registry.ExpirePending(499).empty());
  CHECK(registry.ExpirePending(500).size() == 1);
  CHECK(!registry.HasPending());
}

}  // namespace

int main() {
  TestSlotsAndBindings();
  TestFirstUserSlot();
  TestKiosks();
  TestPendingAddTakeExpire();
  TestPendingGhosts();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("window_registry_test: ok\n");
  return EXIT_SUCCESS;
}
