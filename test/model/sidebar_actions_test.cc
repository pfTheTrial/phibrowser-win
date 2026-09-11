// Host tests for the place helpers and the drop resolvers
// (docs/sidebar-architecture.md §8).

#include <cstdio>
#include <string>
#include <vector>

#include "phi_sidebar_actions.h"
#include "phi_sidebar_drop.h"
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
                   std::string split = {}, std::string group = {}) {
  SidebarLiveTab t;
  t.id = id;
  t.index = index;
  t.tag = std::move(tag);
  t.split_id = std::move(split);
  t.group_id = std::move(group);
  return t;
}
SidebarStoredItem Pin(std::string guid, int index, bool split = false) {
  return {std::move(guid), SidebarStoredKind::kPin, {}, index, split};
}
SidebarStoredItem Bookmark(std::string guid, std::string parent, int index) {
  return {std::move(guid), SidebarStoredKind::kBookmark, std::move(parent),
          index, false};
}
SidebarStoredItem Folder(std::string guid, std::string parent, int index) {
  return {std::move(guid), SidebarStoredKind::kFolder, std::move(parent),
          index, false};
}
DropRow Row(SidebarKey key, int top, int bottom, std::string parent = {},
            int depth = 0, bool folder = false, bool expanded = false) {
  DropRow r;
  r.key = std::move(key);
  r.bounds = {0, top, 200, bottom};
  r.parent_guid = std::move(parent);
  r.depth = depth;
  r.folder = folder;
  r.expanded = expanded;
  return r;
}
DropRow TabRowAt(SidebarKey key, int top, std::string group = {},
                 bool header = false) {
  DropRow r;
  r.key = std::move(key);
  r.bounds = {0, top, 200, top + 36};
  r.group_id = std::move(group);
  r.group_header = header;
  return r;
}

void PlaceHelpers() {
  std::printf("place helpers: sibling index, anchor tab, no-op detection\n");
  SidebarInputs in;
  in.stored = {Pin("p1", 0), Pin("p2", 1), Pin("p3", 2), Folder("f", "", 0),
               Bookmark("b1", "f", 0), Bookmark("b2", "f", 1),
               Bookmark("b0", "", 1)};
  in.strip = {Tab(1, 0, "p1"), Tab(2, 1), Tab(3, 2, {}, "S"), Tab(4, 3, {}, "S"),
              Tab(5, 4)};
  const SidebarTree t = BuildSidebar(in);

  SidebarPlace pins;
  pins.section = SidebarPlace::Section::kPinned;
  pins.before = SidebarKey::Cell("p3");
  CHECK(PlaceSiblingIndex(t, pins, "") == 2);
  CHECK(PlaceSiblingIndex(t, pins, "p1") == 1);  // p1 leaving: p3 is slot 1.
  pins.before = {};
  CHECK(PlaceSiblingIndex(t, pins, "") > 100);   // End.

  SidebarPlace folder;
  folder.section = SidebarPlace::Section::kBookmarks;
  folder.parent_guid = "f";
  folder.before = SidebarKey::Cell("b2");
  CHECK(PlaceSiblingIndex(t, folder, "") == 1);
  folder.before = SidebarKey::Cell("b0");  // Not in f -> end.
  CHECK(PlaceSiblingIndex(t, folder, "") > 100);

  SidebarPlace tabs;
  tabs.section = SidebarPlace::Section::kTabs;
  tabs.before = SidebarKey::Split("S");
  CHECK(PlaceAnchorTab(t, tabs) == 3);  // First pane.
  tabs.before = SidebarKey::Cell("p1");
  CHECK(PlaceAnchorTab(t, tabs) == 1);  // Open cell -> pane A.
  tabs.before = SidebarKey::Cell("p2");
  CHECK(PlaceAnchorTab(t, tabs) == 0);  // Closed cell -> end.

  CHECK(TabOfKey(t, SidebarKey::Split("S"), 1) == 4);
  CHECK(TabOfKey(t, SidebarKey::Tab(5)) == 5);
  CHECK(TabOfKey(t, SidebarKey::Cell("p1")) == 1);

  // No-ops: tab 2 before the split (its successor), the split before tab 5,
  // tab 5 to the end; pins p1 before p2.
  SidebarPlace before_split = tabs;
  before_split.before = SidebarKey::Split("S");
  before_split.group_explicit = false;
  CHECK(MoveIsNoop(t, SidebarKey::Tab(2), before_split));
  SidebarPlace before5 = before_split;
  before5.before = SidebarKey::Tab(5);
  CHECK(MoveIsNoop(t, SidebarKey::Split("S"), before5));
  CHECK(!MoveIsNoop(t, SidebarKey::Tab(2), before5));
  SidebarPlace end = before_split;
  end.before = {};
  CHECK(MoveIsNoop(t, SidebarKey::Tab(5), end));
  CHECK(!MoveIsNoop(t, SidebarKey::Tab(2), end));
  SidebarPlace p1_before_p2;
  p1_before_p2.section = SidebarPlace::Section::kPinned;
  p1_before_p2.before = SidebarKey::Cell("p2");
  CHECK(MoveIsNoop(t, SidebarKey::Cell("p1"), p1_before_p2));
  CHECK(!MoveIsNoop(t, SidebarKey::Cell("p3"), p1_before_p2));
  // A cell entering the list is never a no-op.
  CHECK(!MoveIsNoop(t, SidebarKey::Cell("p1"), end));
}

void BookmarkDrop() {
  std::printf("bookmark drop: siblings, into collapsed folder, expanded folder, gaps\n");
  // Outline: f (expanded) > [b1, b2]; g (collapsed); b0. Row pitch 38.
  std::vector<DropRow> rows = {
      Row(SidebarKey::Folder("f"), 0, 36, "", 0, true, true),
      Row(SidebarKey::Cell("b1"), 38, 74, "f", 1),
      Row(SidebarKey::Cell("b2"), 76, 112, "f", 1),
      Row(SidebarKey::Folder("g"), 114, 150, "", 0, true, false),
      Row(SidebarKey::Cell("b0"), 152, 188, "", 0)};
  DropResolution r = ResolveBookmarkDrop(rows, 10, {});      // On f: first child.
  CHECK(r.place.parent_guid == "f" && r.place.before == SidebarKey::Cell("b1"));
  CHECK(r.gap_slot == 1 && r.folder_key.empty());
  r = ResolveBookmarkDrop(rows, 120, {});                    // Upper half of g: INTO.
  CHECK(r.place.parent_guid == "g" && r.place.before.empty());
  CHECK(r.gap_slot == -1 && r.folder_key == SidebarKey::Folder("g"));
  r = ResolveBookmarkDrop(rows, 145, {});                    // Lower half of g: after g.
  CHECK(r.place.parent_guid.empty() && r.place.before == SidebarKey::Cell("b0"));
  CHECK(r.gap_slot == 4);
  r = ResolveBookmarkDrop(rows, 100, {});                    // Lower half of b2: end of f.
  CHECK(r.place.parent_guid == "f" && r.place.before.empty() && r.gap_slot == 3);
  r = ResolveBookmarkDrop(rows, 45, {});                     // Upper half of b1.
  CHECK(r.place.parent_guid == "f" && r.place.before == SidebarKey::Cell("b1") && r.gap_slot == 1);
  r = ResolveBookmarkDrop(rows, 75, SidebarKey::Cell("b1")); // Gap b1|b2 while dragging b1.
  CHECK(r.place.parent_guid == "f" && r.place.before == SidebarKey::Cell("b2"));
  r = ResolveBookmarkDrop(rows, 45, SidebarKey::Cell("b1")); // On itself: next sibling.
  CHECK(r.place.before == SidebarKey::Cell("b2"));
  r = ResolveBookmarkDrop(rows, 400, {});                    // Past the end.
  CHECK(r.place.parent_guid.empty() && r.place.before.empty() && r.gap_slot == 5);
  r = ResolveBookmarkDrop(rows, 120, SidebarKey::Folder("g"));  // A folder over itself.
  CHECK(r.folder_key.empty());
  // An EMPTY outline (the reserved drop zone): the root's end, gap 0.
  r = ResolveBookmarkDrop({}, 20, {});
  CHECK(r.place.section == SidebarPlace::Section::kBookmarks);
  CHECK(r.place.parent_guid.empty() && r.place.before.empty());
  CHECK(r.gap_slot == 0 && r.folder_key.empty());
}

void PinnedDrop() {
  std::printf("pinned drop: cell midpoints, rows, end\n");
  std::vector<DropRow> cells;
  const auto cell = [&](const char* guid, int l, int t, int r, int b) {
    DropRow c;
    c.key = SidebarKey::Cell(guid);
    c.bounds = {l, t, r, b};
    cells.push_back(c);
  };
  cell("a", 0, 0, 100, 45);
  cell("b", 108, 0, 208, 45);
  cell("c", 0, 53, 100, 98);
  DropResolution r = ResolvePinnedDrop(cells, 20, 20, {});
  CHECK(r.gap_slot == 0 && r.place.before == SidebarKey::Cell("a"));
  r = ResolvePinnedDrop(cells, 90, 20, {});   // Right of a's midpoint.
  CHECK(r.gap_slot == 1 && r.place.before == SidebarKey::Cell("b"));
  r = ResolvePinnedDrop(cells, 200, 20, {});  // Right of b.
  CHECK(r.gap_slot == 2 && r.place.before == SidebarKey::Cell("c"));
  r = ResolvePinnedDrop(cells, 20, 70, {});   // Left of c.
  CHECK(r.gap_slot == 2);
  r = ResolvePinnedDrop(cells, 20, 300, {});  // Below everything.
  CHECK(r.gap_slot == 3 && r.place.before.empty());
  r = ResolvePinnedDrop(cells, 20, 20, SidebarKey::Cell("a"));  // Anchor skips itself.
  CHECK(r.place.before == SidebarKey::Cell("b"));
  // An EMPTY grid (the reserved drop zone): slot 0, no anchor = the end.
  r = ResolvePinnedDrop({}, 20, 20, {});
  CHECK(r.place.section == SidebarPlace::Section::kPinned);
  CHECK(r.place.before.empty() && r.gap_slot == 0);
}

void TabDrop() {
  std::printf("tab drop: slots, group runs, headers, horizontal\n");
  // t1; [G header, g1, g2]; t4.
  std::vector<DropRow> rows = {
      TabRowAt(SidebarKey::Tab(1), 0),
      TabRowAt(SidebarKey::Group("G"), 38, "G", true),
      TabRowAt(SidebarKey::Tab(2), 76, "G"),
      TabRowAt(SidebarKey::Tab(3), 114, "G"),
      TabRowAt(SidebarKey::Tab(4), 152)};
  DropResolution r = ResolveTabDrop(rows, 10, 5, false, {});
  CHECK(r.gap_slot == 0 && r.place.before == SidebarKey::Tab(1) && r.place.group_id.empty());
  r = ResolveTabDrop(rows, 10, 30, false, {});   // Below t1's mid, before header.
  CHECK(r.gap_slot == 1 && r.place.before == SidebarKey::Tab(2) && r.place.group_id.empty());
  r = ResolveTabDrop(rows, 10, 70, false, {});   // Under the header: into G, first.
  CHECK(r.gap_slot == 1 && r.place.before == SidebarKey::Tab(2) && r.place.group_id == "G");
  r = ResolveTabDrop(rows, 10, 110, false, {});  // Between g1 and g2: in G.
  CHECK(r.gap_slot == 2 && r.place.before == SidebarKey::Tab(3) && r.place.group_id == "G");
  r = ResolveTabDrop(rows, 10, 140, false, {});  // After g2, before t4: ungrouped.
  CHECK(r.gap_slot == 3 && r.place.before == SidebarKey::Tab(4) && r.place.group_id.empty());
  r = ResolveTabDrop(rows, 10, 400, false, {});  // End.
  CHECK(r.gap_slot == 4 && r.place.before.empty() && r.place.group_id.empty());
  // The card's bottom inset (members carry the card's bottom edge): past
  // g2's midpoint but above the card's bottom is G's LAST slot; below the
  // card it is after the group. The horizontal strip has no such zone.
  rows[2].group_bottom = rows[3].group_bottom = 154;
  r = ResolveTabDrop(rows, 10, 140, false, {});
  CHECK(r.gap_slot == 3 && r.place.before == SidebarKey::Tab(4) && r.place.group_id == "G");
  r = ResolveTabDrop(rows, 10, 160, false, {});
  CHECK(r.gap_slot == 3 && r.place.before == SidebarKey::Tab(4) && r.place.group_id.empty());
  r = ResolveTabDrop(rows, 10, 140, false, SidebarKey::Tab(3));  // The last member itself.
  CHECK(r.gap_slot == 3 && r.place.before == SidebarKey::Tab(4) && r.place.group_id == "G");
  rows[2].group_bottom = rows[3].group_bottom = 0;
  r = ResolveTabDrop(rows, 10, 110, false, SidebarKey::Tab(3));  // Anchor is the dragged row.
  CHECK(r.place.before == SidebarKey::Tab(4));
  CHECK(r.place.group_explicit);
  // Horizontal strip compares x.
  std::vector<DropRow> strip;
  for (int i = 0; i < 3; ++i) {
    DropRow c;
    c.key = SidebarKey::Tab(10 + i);
    c.bounds = {i * 100, 0, i * 100 + 90, 30};
    strip.push_back(c);
  }
  r = ResolveTabDrop(strip, 160, 10, true, {});
  CHECK(r.gap_slot == 2 && r.place.before == SidebarKey::Tab(12));
}

// A strip row: a chip (`header`) or a tab, `left..right` on the strip.
DropRow StripRowAt(SidebarKey key, int left, int right, std::string group = {},
                   bool header = false) {
  DropRow r;
  r.key = std::move(key);
  r.bounds = {left, 0, right, 34};
  r.group_id = std::move(group);
  r.group_header = header;
  return r;
}

void StripTabDrop() {
  std::printf("strip tab drop: edge-based gap, chips, join and leave\n");
  // t0 | t1 (dragged) | t2 | t3 — 100 wide each, abutting.
  std::vector<DropRow> rows = {
      StripRowAt(SidebarKey::Tab(0), 0, 100),
      StripRowAt(SidebarKey::Tab(1), 100, 200),
      StripRowAt(SidebarKey::Tab(2), 200, 300),
      StripRowAt(SidebarKey::Tab(3), 300, 400)};
  // At home: the gap is its own slot — before t2, counted past t0 and t1.
  DropResolution r = ResolveStripTabDrop(rows, 1, 100, 200);
  CHECK(r.gap_slot == 2 && r.anchor_row == 2 &&
        r.place.before == SidebarKey::Tab(2) && r.place.group_id.empty());
  CHECK(r.place.group_explicit);
  // Rightward: t2 lets way when the tab's RIGHT edge reaches its centre
  // (250), whatever the grab offset — not before.
  r = ResolveStripTabDrop(rows, 1, 140, 240);
  CHECK(r.gap_slot == 2 && r.anchor_row == 2);
  r = ResolveStripTabDrop(rows, 1, 160, 260);
  CHECK(r.gap_slot == 3 && r.anchor_row == 3 &&
        r.place.before == SidebarKey::Tab(3));
  // Past everything: the end.
  r = ResolveStripTabDrop(rows, 1, 320, 420);
  CHECK(r.gap_slot == 4 && r.anchor_row == 4 && r.place.before.empty());
  // Leftward: t0 lets way when the tab's LEFT edge reaches its centre (50).
  r = ResolveStripTabDrop(rows, 1, 60, 160);
  CHECK(r.gap_slot == 2 && r.anchor_row == 2);
  r = ResolveStripTabDrop(rows, 1, 40, 140);
  CHECK(r.gap_slot == 0 && r.anchor_row == 0 &&
        r.place.before == SidebarKey::Tab(0));

  // A foreign group ahead of the dragged tab, approached from the right:
  // [G chip 0..40] g1 44..144 | g2 144..244 | u 244..344 | d 344..444.
  std::vector<DropRow> group = {
      StripRowAt(SidebarKey::Group("G"), 0, 40, "G", true),
      StripRowAt(SidebarKey::Tab(1), 44, 144, "G"),
      StripRowAt(SidebarKey::Tab(2), 144, 244, "G"),
      StripRowAt(SidebarKey::Tab(3), 244, 344),
      StripRowAt(SidebarKey::Tab(4), 344, 444)};
  r = ResolveStripTabDrop(group, 4, 344, 444);  // Home.
  CHECK(r.gap_slot == 4 && r.anchor_row == 5 && r.place.before.empty() &&
        r.place.group_id.empty());
  // u let way (left edge at its centre 294): the gap sits after G's last
  // member — outside the group while the tab covers g2 by under a third.
  r = ResolveStripTabDrop(group, 4, 280, 380);
  CHECK(r.gap_slot == 2 && r.anchor_row == 3 &&
        r.place.before == SidebarKey::Tab(3) && r.place.group_id.empty());
  // A third's cover of g2 (left edge at or before 244 - 33): joins as last.
  r = ResolveStripTabDrop(group, 4, 200, 300);
  CHECK(r.gap_slot == 2 && r.anchor_row == 3 &&
        r.place.before == SidebarKey::Tab(3) && r.place.group_id == "G");
  // Between g1 and g2: in the run.
  r = ResolveStripTabDrop(group, 4, 180, 280);
  CHECK(r.gap_slot == 1 && r.anchor_row == 2 &&
        r.place.before == SidebarKey::Tab(2) && r.place.group_id == "G");
  // Right after the chip (left edge past the chip's centre 20, before
  // g1's 94): the group's first member.
  r = ResolveStripTabDrop(group, 4, 30, 130);
  CHECK(r.gap_slot == 0 && r.anchor_row == 1 &&
        r.place.before == SidebarKey::Tab(1) && r.place.group_id == "G");
  // Before the chip: before the group, ungrouped, anchored on g1.
  r = ResolveStripTabDrop(group, 4, 10, 110);
  CHECK(r.gap_slot == 0 && r.anchor_row == 0 &&
        r.place.before == SidebarKey::Tab(1) && r.place.group_id.empty());

  // The dragged tab is its OWN group's last member: [G] g1 | d | u.
  std::vector<DropRow> last = {
      StripRowAt(SidebarKey::Group("G"), 0, 40, "G", true),
      StripRowAt(SidebarKey::Tab(1), 44, 144, "G"),
      StripRowAt(SidebarKey::Tab(2), 144, 244, "G"),
      StripRowAt(SidebarKey::Tab(3), 244, 344)};
  // Home, and a nudge right within a third of its width: still a member.
  r = ResolveStripTabDrop(last, 2, 144, 244);
  CHECK(r.anchor_row == 3 && r.place.before == SidebarKey::Tab(3) &&
        r.place.group_id == "G");
  r = ResolveStripTabDrop(last, 2, 170, 270);
  CHECK(r.anchor_row == 3 && r.place.group_id == "G");
  // A third past home: leaves the group in place (trailingLeave).
  r = ResolveStripTabDrop(last, 2, 180, 280);
  CHECK(r.anchor_row == 3 && r.place.before == SidebarKey::Tab(3) &&
        r.place.group_id.empty());
  // Past u: the end, ungrouped.
  r = ResolveStripTabDrop(last, 2, 200, 300);
  CHECK(r.anchor_row == 4 && r.place.before.empty() &&
        r.place.group_id.empty());
  // Leftward into its own run: before g1 (left edge at g1's centre 94).
  r = ResolveStripTabDrop(last, 2, 90, 190);
  CHECK(r.anchor_row == 1 && r.place.before == SidebarKey::Tab(1) &&
        r.place.group_id == "G");
  // Past its own chip's centre: before the group (leadingLeave).
  r = ResolveStripTabDrop(last, 2, 15, 115);
  CHECK(r.anchor_row == 0 && r.place.before == SidebarKey::Tab(1) &&
        r.place.group_id.empty());

  // The only member: [G] d | u — the chip is the trailing anchor.
  std::vector<DropRow> lone = {
      StripRowAt(SidebarKey::Group("G"), 0, 40, "G", true),
      StripRowAt(SidebarKey::Tab(1), 44, 144, "G"),
      StripRowAt(SidebarKey::Tab(2), 144, 244)};
  r = ResolveStripTabDrop(lone, 1, 44, 144);
  CHECK(r.anchor_row == 2 && r.place.group_id == "G");
  r = ResolveStripTabDrop(lone, 1, 80, 180);  // A third past the chip.
  CHECK(r.anchor_row == 2 && r.place.before == SidebarKey::Tab(2) &&
        r.place.group_id.empty());

  // A collapsed foreign chip stands in for its hidden run: before it is
  // before the group (anchored on the hidden first member), right after it
  // is after the group.
  std::vector<DropRow> collapsed = {
      StripRowAt(SidebarKey::Tab(0), 0, 100),
      StripRowAt(SidebarKey::Group("G"), 104, 144, "G", true),
      StripRowAt(SidebarKey::Tab(1), 0, 0, "G"),
      StripRowAt(SidebarKey::Tab(2), 0, 0, "G"),
      StripRowAt(SidebarKey::Tab(3), 148, 248),
      StripRowAt(SidebarKey::Tab(4), 248, 348)};
  r = ResolveStripTabDrop(collapsed, 5, 120, 220);
  CHECK(r.gap_slot == 1 && r.anchor_row == 1 &&
        r.place.before == SidebarKey::Tab(1) && r.place.group_id.empty());
  r = ResolveStripTabDrop(collapsed, 5, 130, 230);
  CHECK(r.gap_slot == 1 && r.anchor_row == 4 &&
        r.place.before == SidebarKey::Tab(3) && r.place.group_id.empty());
  // Nothing placed but the dragged tab: the end, slot past it.
  std::vector<DropRow> alone = {StripRowAt(SidebarKey::Tab(1), 0, 100)};
  r = ResolveStripTabDrop(alone, 0, 30, 130);
  CHECK(r.gap_slot == 1 && r.anchor_row == 1 && r.place.before.empty());
}

}  // namespace

int main() {
  PlaceHelpers();
  BookmarkDrop();
  PinnedDrop();
  TabDrop();
  StripTabDrop();
  if (g_failures) {
    std::printf("%d FAILED\n", g_failures);
    return 1;
  }
  std::printf("ALL PASS\n");
  return 0;
}
