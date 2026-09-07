// Host tests for BuildSidebar (docs/sidebar-architecture.md §8). Run with
// test/model/run.sh. No framework: assertions are plain and print the case.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "phi_sidebar_model.h"

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

SidebarLiveTab Tab(int id, int index, std::string tag = {},
                   std::string split = {}, std::string group = {},
                   bool active = false) {
  SidebarLiveTab t;
  t.id = id;
  t.index = index;
  t.tag = std::move(tag);
  t.split_id = std::move(split);
  t.group_id = std::move(group);
  t.active = active;
  return t;
}

SidebarStoredItem Pin(std::string guid, int index, bool split = false) {
  return {std::move(guid), SidebarStoredKind::kPin, {}, index, split};
}
SidebarStoredItem Bookmark(std::string guid, std::string parent, int index,
                           bool split = false) {
  return {std::move(guid), SidebarStoredKind::kBookmark, std::move(parent),
          index, split};
}
SidebarStoredItem Folder(std::string guid, std::string parent, int index) {
  return {std::move(guid), SidebarStoredKind::kFolder, std::move(parent),
          index, false};
}

const SidebarContentNode* Node(const SidebarTree& t, const SidebarKey& key) {
  for (const auto& n : t.content) {
    if (n.key == key) return &n;
  }
  return nullptr;
}

int ContentRows(const SidebarTree& t) {
  int n = 0;
  for (const auto& node : t.content) {
    if (node.kind != SidebarNodeKind::kGroup) ++n;
  }
  return n;
}

void TagCodec() {
  std::printf("tag codec\n");
  SidebarTag parsed;
  CHECK(TagFor("g1", false) == "g1");
  CHECK(TagFor("g1", true) == "g1#b");
  CHECK(TagFor("", true).empty());
  CHECK(ParseTag("g1", &parsed) && parsed.guid == "g1" && !parsed.pane_b);
  CHECK(ParseTag("g1#b", &parsed) && parsed.guid == "g1" && parsed.pane_b);
  CHECK(!ParseTag("", &parsed));
  CHECK(!ParseTag("#b", &parsed));
  CHECK(SidebarKey::Tab(7).ToString() == "tab:7");
  CHECK(SidebarKey::Tab(7).tab_id() == 7);
  CHECK(SidebarKey::Cell("x") != SidebarKey::Folder("x"));
}

void PlainStrip() {
  std::printf("plain strip, no cells\n");
  SidebarInputs in;
  in.strip = {Tab(1, 0), Tab(2, 1, {}, {}, {}, true), Tab(3, 2)};
  const SidebarTree t = BuildSidebar(in);
  CHECK(ContentRows(t) == 3);
  CHECK(t.owner_of.empty());
  CHECK(t.active_tab == 2);
  CHECK(t.active_key == SidebarKey::Tab(2));
  CHECK(Node(t, SidebarKey::Tab(2))->active);
  CHECK(t.IndexOf(3) == 2 && t.IndexOf(9) == -1);
}

void PinBindsAndHides() {
  std::printf("pin binds its tagged tab and hides it\n");
  SidebarInputs in;
  in.stored = {Pin("p1", 0), Pin("p2", 1)};
  in.strip = {Tab(1, 0, "p1", {}, {}, true), Tab(2, 1)};
  const SidebarTree t = BuildSidebar(in);
  CHECK(t.pins.size() == 2);
  CHECK(t.pins[0].binding.state == SidebarBindingState::kOpen);
  CHECK(t.pins[0].binding.tab_a == 1 && t.pins[0].binding.active);
  CHECK(t.pins[1].binding.state == SidebarBindingState::kClosed);
  CHECK(t.Hidden(1) && !t.Hidden(2));
  CHECK(ContentRows(t) == 1);
  CHECK(t.active_key == SidebarKey::Cell("p1"));
}

void DuplicateTagFirstWins() {
  std::printf("duplicate tag: first in strip order binds, the rest stay plain\n");
  SidebarInputs in;
  in.stored = {Pin("p1", 0)};
  in.strip = {Tab(5, 1, "p1"), Tab(4, 0, "p1"), Tab(6, 2, "p1")};
  const SidebarTree t = BuildSidebar(in);
  CHECK(t.BindingFor("p1")->tab_a == 4);
  CHECK(t.Hidden(4) && !t.Hidden(5) && !t.Hidden(6));
  CHECK(ContentRows(t) == 2);
}

void SplitCellOpen() {
  std::printf("split cell: both panes tagged and in one live split -> kSplitOpen\n");
  SidebarInputs in;
  in.stored = {Bookmark("b1", "", 0, /*split=*/true)};
  in.strip = {Tab(1, 0, "b1", "S"), Tab(2, 1, "b1#b", "S", {}, true), Tab(3, 2)};
  const SidebarTree t = BuildSidebar(in);
  const SidebarBinding* b = t.BindingFor("b1");
  CHECK(b && b->state == SidebarBindingState::kSplitOpen);
  CHECK(b->tab_a == 1 && b->tab_b == 2 && b->split_id == "S" && b->active);
  CHECK(t.Hidden(1) && t.Hidden(2));
  CHECK(ContentRows(t) == 1);
  CHECK(t.active_key == SidebarKey::Cell("b1"));
}

void SplitCellPartial() {
  std::printf("split cell: one pane live outside any split -> kPartial\n");
  SidebarInputs in;
  in.stored = {Pin("p1", 0, /*split=*/true)};
  in.strip = {Tab(1, 0, "p1"), Tab(3, 1)};
  const SidebarTree t = BuildSidebar(in);
  const SidebarBinding* b = t.BindingFor("p1");
  CHECK(b && b->state == SidebarBindingState::kPartial);
  CHECK(b->tab_a == 1 && b->tab_b == 0);
  CHECK(t.Hidden(1));
}

void TaggedPaneInUnrelatedSplitNotHidden() {
  std::printf("tagged pane in an unrelated split is never hidden\n");
  SidebarInputs in;
  in.stored = {Pin("p1", 0), Pin("p2", 1, /*split=*/true)};
  // p1's tab split with a plain tab; p2's pane A split with a plain tab.
  in.strip = {Tab(1, 0, "p1", "S"), Tab(2, 1, {}, "S"), Tab(3, 2, "p2", "T"),
              Tab(4, 3, {}, "T")};
  const SidebarTree t = BuildSidebar(in);
  CHECK(t.BindingFor("p1")->state == SidebarBindingState::kClosed);
  CHECK(t.BindingFor("p2")->state == SidebarBindingState::kClosed);
  CHECK(t.owner_of.empty());
  CHECK(ContentRows(t) == 2);  // Two split rows.
  CHECK(Node(t, SidebarKey::Split("S")) && Node(t, SidebarKey::Split("T")));
  CHECK(Node(t, SidebarKey::Split("S"))->tab_a == 1 &&
        Node(t, SidebarKey::Split("S"))->tab_b == 2);
}

void StalePaneBTagOnSingleCell() {
  std::printf("a #b tag whose cell has no pane B stays a plain tab\n");
  SidebarInputs in;
  in.stored = {Pin("p1", 0)};
  in.strip = {Tab(1, 0, "p1#b")};
  const SidebarTree t = BuildSidebar(in);
  CHECK(t.BindingFor("p1")->state == SidebarBindingState::kClosed);
  CHECK(!t.Hidden(1));
}

void SplitByIdentityNotAdjacency() {
  std::printf("split rows resolve by split id; adjacency is irrelevant\n");
  SidebarInputs in;
  in.stored = {Pin("p1", 0)};
  // 1 and 3 share a split; 2 (bound to a pin) sits between them in the strip.
  in.strip = {Tab(1, 0, {}, "S"), Tab(2, 1, "p1"), Tab(3, 2, {}, "S")};
  const SidebarTree t = BuildSidebar(in);
  CHECK(ContentRows(t) == 1);
  const auto* s = Node(t, SidebarKey::Split("S"));
  CHECK(s && s->tab_a == 1 && s->tab_b == 3);
}

void SplitStraddlingGroupBoundary() {
  std::printf("split whose panes are in different groups -> two rows\n");
  SidebarInputs in;
  in.groups = {{"G", false}};
  in.strip = {Tab(1, 0, {}, "S", "G"), Tab(2, 1, {}, "S")};
  const SidebarTree t = BuildSidebar(in);
  CHECK(ContentRows(t) == 2);
  CHECK(Node(t, SidebarKey::Tab(1)) && Node(t, SidebarKey::Tab(2)));
  CHECK(Node(t, SidebarKey::Group("G")));
}

void GroupHeaderOnceAndCollapse() {
  std::printf("group header once, members hidden when collapsed\n");
  SidebarInputs in;
  in.groups = {{"G", true}, {"H", false}};
  in.strip = {Tab(1, 0), Tab(2, 1, {}, {}, "G", true), Tab(3, 2, {}, {}, "G"),
              Tab(4, 3, {}, {}, "H"), Tab(5, 4, {}, {}, "unknown")};
  const SidebarTree t = BuildSidebar(in);
  int headers = 0;
  for (const auto& n : t.content) headers += n.kind == SidebarNodeKind::kGroup;
  CHECK(headers == 2);
  CHECK(t.content[1].key == SidebarKey::Group("G") && t.content[1].collapsed);
  CHECK(t.content[1].contains_active);
  CHECK(Node(t, SidebarKey::Tab(2))->hidden && Node(t, SidebarKey::Tab(3))->hidden);
  CHECK(!Node(t, SidebarKey::Tab(4))->hidden);
  CHECK(Node(t, SidebarKey::Tab(5))->group_id.empty());  // Unknown -> flat.
  CHECK(t.active_key == SidebarKey::Tab(2));
}

void CollapsedFolderStillOwns() {
  std::printf("bookmark under a folder binds regardless of expansion; folder washes\n");
  SidebarInputs in;
  in.stored = {Folder("f", "", 0), Bookmark("b1", "f", 0), Bookmark("b0", "", 1)};
  in.strip = {Tab(1, 0, "b1", {}, {}, true), Tab(2, 1)};
  const SidebarTree t = BuildSidebar(in);
  CHECK(t.bookmarks.size() == 3);
  CHECK(t.bookmarks[0].key == SidebarKey::Folder("f") && t.bookmarks[0].depth == 0);
  CHECK(t.bookmarks[1].guid == "b1" && t.bookmarks[1].depth == 1);
  CHECK(t.bookmarks[2].guid == "b0" && t.bookmarks[2].depth == 0);
  CHECK(t.Hidden(1));
  CHECK(t.bookmarks[0].contains_active);
  CHECK(t.active_key == SidebarKey::Cell("b1"));
}

void OrphanBookmarkSurfacesAtRoot() {
  std::printf("a bookmark whose parent is missing is not lost\n");
  SidebarInputs in;
  in.stored = {Bookmark("b1", "missing", 0)};
  const SidebarTree t = BuildSidebar(in);
  CHECK(t.bookmarks.size() == 1 && t.bookmarks[0].depth == 0);
}

void HorizontalPolicy() {
  std::printf("comfortable: pins hidden, bookmark-bound visible\n");
  SidebarInputs in;
  in.policy.horizontal_layout = true;
  in.stored = {Pin("p1", 0), Bookmark("b1", "", 0)};
  in.strip = {Tab(1, 0, "p1"), Tab(2, 1, "b1", {}, {}, true)};
  const SidebarTree t = BuildSidebar(in);
  CHECK(t.Hidden(1) && !t.Hidden(2));
  CHECK(t.BindingFor("b1")->state == SidebarBindingState::kOpen);  // Bound, shown.
  CHECK(ContentRows(t) == 1);
  CHECK(t.active_key == SidebarKey::Tab(2));
}

void IncognitoHasNoCells() {
  std::printf("incognito: stored rows ignored\n");
  SidebarInputs in;
  in.policy.incognito = true;
  in.stored = {Pin("p1", 0)};
  in.strip = {Tab(1, 0, "p1")};
  const SidebarTree t = BuildSidebar(in);
  CHECK(t.pins.empty() && t.owner_of.empty() && ContentRows(t) == 1);
}

void ActiveSurfaceHandsSelectionToOrigin() {
  std::printf("active reader surface -> origin row highlights; surface never a row\n");
  SidebarInputs in;
  in.stored = {Pin("p1", 0)};
  SidebarLiveTab reader = Tab(9, 2, {}, {}, {}, true);
  reader.surface = SidebarSurface::kReader;
  reader.origin_id = 2;
  in.strip = {Tab(1, 0, "p1"), Tab(2, 1), reader};
  in.surfaces = {{2, SidebarSurface::kReader}, {1, SidebarSurface::kPeek}};
  const SidebarTree t = BuildSidebar(in);
  CHECK(ContentRows(t) == 1);
  CHECK(!Node(t, SidebarKey::Tab(9)));
  CHECK(t.active_tab == 2 && t.active_key == SidebarKey::Tab(2));
  CHECK(Node(t, SidebarKey::Tab(2))->attached_surface == SidebarSurface::kReader);
  CHECK(t.pins[0].attached_surface == SidebarSurface::kPeek);  // Hidden origin.
}

void OrderAndDedup() {
  std::printf("strip order by index; duplicate ids and invalid entries dropped\n");
  SidebarInputs in;
  in.strip = {Tab(3, 2), Tab(1, 0), Tab(2, 1), Tab(2, 5), Tab(0, 6), Tab(4, -1)};
  const SidebarTree t = BuildSidebar(in);
  CHECK(ContentRows(t) == 3);
  CHECK(t.content[0].key == SidebarKey::Tab(1));
  CHECK(t.content[1].key == SidebarKey::Tab(2));
  CHECK(t.content[2].key == SidebarKey::Tab(3));
}

}  // namespace

int main() {
  TagCodec();
  PlainStrip();
  PinBindsAndHides();
  DuplicateTagFirstWins();
  SplitCellOpen();
  SplitCellPartial();
  TaggedPaneInUnrelatedSplitNotHidden();
  StalePaneBTagOnSingleCell();
  SplitByIdentityNotAdjacency();
  SplitStraddlingGroupBoundary();
  GroupHeaderOnceAndCollapse();
  CollapsedFolderStillOwns();
  OrphanBookmarkSurfacesAtRoot();
  HorizontalPolicy();
  IncognitoHasNoCells();
  ActiveSurfaceHandsSelectionToOrigin();
  OrderAndDedup();
  if (g_failures) {
    std::printf("%d FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL PASS\n");
  return 0;
}
