// Host tests for PhiWindowModel (docs/architecture-review.md §3.3 "client
// side"). Run with test/model/run.sh. The model is STL + the framework's
// snapshot structs, so everything but Refresh (a three-line pass-through to
// PhiFramework::Snapshot, which needs the real framework) is covered here.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "phibrowser-win/client/phi_window_model.h"

using namespace phi_client;
using phi_framework::GroupSnapshot;
using phi_framework::SplitLayout;
using phi_framework::SplitSnapshot;
using phi_framework::TabSnapshot;
using phi_framework::TabSurface;
using phi_framework::WindowSnapshot;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

TabSnapshot Tab(int id, int index, const char* url, const char* tag = "") {
  TabSnapshot tab;
  tab.id = TabId{id};
  tab.index = index;
  tab.url = url;
  tab.title = std::string("title ") + url;
  tab.tag = tag;
  return tab;
}

WindowSnapshot Window(int64_t window, uint64_t version) {
  WindowSnapshot snapshot;
  snapshot.window = BrowserId{window};
  snapshot.version = version;
  snapshot.profile_id = "Default";
  return snapshot;
}

void TestEmptyModel() {
  PhiWindowModel model;
  CHECK(!model.Has(BrowserId{7}));
  CHECK(model.Get(BrowserId{7}) == nullptr);
  CHECK(model.Version(BrowserId{7}) == 0);
  CHECK(model.FindTab(BrowserId{7}, TabId{1}) == nullptr);
  CHECK(model.FindTabByTag(BrowserId{7}, "x") == nullptr);
  CHECK(model.FindTabAt(BrowserId{7}, 0) == nullptr);
  CHECK(model.ActiveTab(BrowserId{7}) == nullptr);
  CHECK(model.FindSplit(BrowserId{7}, "s") == nullptr);
  CHECK(model.SplitOfTab(BrowserId{7}, TabId{1}) == nullptr);
  CHECK(model.FindGroup(BrowserId{7}, "g") == nullptr);
  CHECK(!model.BrowserOfTab(TabId{1}).has_value());
  CHECK(model.TabsWithTag("x").empty());
  CHECK(model.Browsers().empty());
  // An invalid window id is never stored.
  model.Update(WindowSnapshot());
  CHECK(model.Browsers().empty());
}

void TestUpdateAndIndexes() {
  PhiWindowModel model;
  WindowSnapshot w = Window(10, 3);
  w.tabs.push_back(Tab(101, 0, "https://a/", "guid-a"));
  w.tabs.push_back(Tab(102, 1, "https://b/"));
  w.tabs.push_back(Tab(103, 2, "https://c/", "guid-c#b"));
  w.tabs[1].active = true;
  w.tabs[2].split_id = "split-1";
  w.tabs[1].split_id = "split-1";
  SplitSnapshot split;
  split.id = "split-1";
  split.primary = TabId{102};
  split.secondary = TabId{103};
  split.layout = SplitLayout::kHorizontal;
  split.ratio = 0.4;
  w.splits.push_back(split);
  GroupSnapshot group;
  group.id = "group-1";
  group.title = "Work";
  w.groups.push_back(group);
  model.Update(w);

  CHECK(model.Has(BrowserId{10}));
  CHECK(model.Version(BrowserId{10}) == 3);
  const WindowSnapshot* stored = model.Get(BrowserId{10});
  CHECK(stored && stored->tabs.size() == 3);
  CHECK(stored && stored->profile_id == "Default");

  const TabSnapshot* by_id = model.FindTab(BrowserId{10}, TabId{103});
  CHECK(by_id && by_id->url == "https://c/");
  CHECK(model.FindTab(BrowserId{10}, TabId{999}) == nullptr);
  CHECK(model.FindTab(BrowserId{10}, TabId{}) == nullptr);

  const TabSnapshot* by_tag = model.FindTabByTag(BrowserId{10}, "guid-c#b");
  CHECK(by_tag && by_tag->id == TabId{103});
  CHECK(model.FindTabByTag(BrowserId{10}, "") == nullptr);
  CHECK(model.FindTabByTag(BrowserId{10}, "nope") == nullptr);

  const TabSnapshot* at = model.FindTabAt(BrowserId{10}, 1);
  CHECK(at && at->id == TabId{102});
  CHECK(model.FindTabAt(BrowserId{10}, 3) == nullptr);
  CHECK(model.FindTabAt(BrowserId{10}, -1) == nullptr);

  const TabSnapshot* active = model.ActiveTab(BrowserId{10});
  CHECK(active && active->id == TabId{102});

  const SplitSnapshot* found_split = model.FindSplit(BrowserId{10}, "split-1");
  CHECK(found_split && found_split->layout == SplitLayout::kHorizontal);
  const SplitSnapshot* of_tab = model.SplitOfTab(BrowserId{10}, TabId{103});
  CHECK(of_tab && of_tab->id == "split-1");
  CHECK(model.SplitOfTab(BrowserId{10}, TabId{101}) == nullptr);

  const GroupSnapshot* found_group = model.FindGroup(BrowserId{10}, "group-1");
  CHECK(found_group && found_group->title == "Work");
  CHECK(model.FindGroup(BrowserId{10}, "") == nullptr);

  const std::optional<BrowserId> owner = model.BrowserOfTab(TabId{101});
  CHECK(owner && *owner == BrowserId{10});
  CHECK(!model.BrowserOfTab(TabId{5}).has_value());
}

void TestReplaceRebuildsIndexes() {
  PhiWindowModel model;
  WindowSnapshot w = Window(10, 1);
  w.tabs.push_back(Tab(101, 0, "https://a/", "guid-a"));
  w.tabs[0].active = true;
  model.Update(w);
  CHECK(model.ActiveTab(BrowserId{10}) != nullptr);

  // The next push replaces the strip wholesale: the old tab, tag and
  // active position must all be gone.
  WindowSnapshot next = Window(10, 2);
  next.tabs.push_back(Tab(202, 0, "https://z/"));
  model.Update(next);
  CHECK(model.Version(BrowserId{10}) == 2);
  CHECK(model.FindTab(BrowserId{10}, TabId{101}) == nullptr);
  CHECK(model.FindTabByTag(BrowserId{10}, "guid-a") == nullptr);
  CHECK(model.ActiveTab(BrowserId{10}) == nullptr);
  CHECK(model.FindTab(BrowserId{10}, TabId{202}) != nullptr);
  CHECK(!model.BrowserOfTab(TabId{101}).has_value());
}

void TestAcrossBrowsers() {
  PhiWindowModel model;
  WindowSnapshot a = Window(20, 1);
  a.tabs.push_back(Tab(1, 0, "https://a/", "row-1"));
  a.tabs.push_back(Tab(2, 1, "https://a2/", "row-1"));  // Restore duplicate.
  WindowSnapshot b = Window(5, 1);
  b.tabs.push_back(Tab(3, 0, "https://b/", "row-1#b"));
  b.tabs.push_back(Tab(4, 1, "https://b2/", "row-1"));
  model.Update(a);
  model.Update(b);

  const std::vector<BrowserId> browsers = model.Browsers();
  CHECK(browsers.size() == 2);
  CHECK(browsers.size() == 2 && browsers[0] == BrowserId{5} &&
        browsers[1] == BrowserId{20});  // Id order, not insertion order.

  const std::optional<BrowserId> owner = model.BrowserOfTab(TabId{3});
  CHECK(owner && *owner == BrowserId{5});

  // Every holder, in browser order then strip order — the index alone
  // would hide tab 2 behind tab 1.
  const auto holders = model.TabsWithTag("row-1");
  CHECK(holders.size() == 3);
  if (holders.size() == 3) {
    CHECK(holders[0].first == BrowserId{5} && holders[0].second == TabId{4});
    CHECK(holders[1].first == BrowserId{20} && holders[1].second == TabId{1});
    CHECK(holders[2].first == BrowserId{20} && holders[2].second == TabId{2});
  }
  CHECK(model.TabsWithTag("row-1#b").size() == 1);
  CHECK(model.TabsWithTag("").empty());
  // The per-browser tag index answers the FIRST holder.
  const TabSnapshot* first = model.FindTabByTag(BrowserId{20}, "row-1");
  CHECK(first && first->id == TabId{1});
}

void TestRemove() {
  PhiWindowModel model;
  WindowSnapshot w = Window(30, 1);
  w.tabs.push_back(Tab(7, 0, "https://a/"));
  model.Update(w);
  CHECK(model.Has(BrowserId{30}));
  model.Remove(BrowserId{30});
  CHECK(!model.Has(BrowserId{30}));
  CHECK(model.Get(BrowserId{30}) == nullptr);
  CHECK(!model.BrowserOfTab(TabId{7}).has_value());
  model.Remove(BrowserId{30});  // Idempotent.
  CHECK(model.Browsers().empty());
}

void TestRefreshWithoutFramework() {
  PhiWindowModel model;
  WindowSnapshot w = Window(40, 1);
  model.Update(w);
  // No framework: nothing to pull from, and what is held stays.
  CHECK(!model.Refresh(nullptr, BrowserId{40}));
  CHECK(model.Has(BrowserId{40}));
  CHECK(!model.Refresh(nullptr, BrowserId{}));
}

void TestSurfaceFieldsSurvive() {
  PhiWindowModel model;
  WindowSnapshot w = Window(50, 1);
  TabSnapshot surface = Tab(9, 0, "chrome-extension://reader/reader.html");
  surface.surface = TabSurface::kReader;
  surface.origin = TabId{8};
  surface.unloaded = true;
  w.tabs.push_back(surface);
  model.Update(w);
  const TabSnapshot* stored = model.FindTab(BrowserId{50}, TabId{9});
  CHECK(stored && stored->surface == TabSurface::kReader);
  CHECK(stored && stored->origin == TabId{8});
  CHECK(stored && stored->unloaded);
}

}  // namespace

int main() {
  TestEmptyModel();
  TestUpdateAndIndexes();
  TestReplaceRebuildsIndexes();
  TestAcrossBrowsers();
  TestRemove();
  TestRefreshWithoutFramework();
  TestSurfaceFieldsSurvive();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("window_model_test: all passed\n");
  return EXIT_SUCCESS;
}
