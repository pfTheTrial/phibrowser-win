// Host tests for the PURE half of the sidebar view
// (docs/architecture-review.md §3.5, lane 1 clusters H and J): the MRU
// order the Ctrl+Tab switcher walks, and the bookmark outline's disclosure
// rule. The rest of PhiSidebarView is Win32 — the row pipeline's placement,
// the band's paint, the hit tests — and stays on the target;
// PHI_SIDEBAR_VIEW_MODEL_ONLY compiles the half that is not.

#define PHI_SIDEBAR_VIEW_MODEL_ONLY

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "phi_sidebar_view.h"

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

TabId T(int id) {
  return TabId(id);
}

std::vector<int> Ids(const PhiTabMru& mru) {
  std::vector<int> out;
  for (const TabId id : mru.order()) {
    out.push_back(id.value);
  }
  return out;
}

// --- The MRU order ---------------------------------------------------------

void MruPutsTheActiveTabFirst() {
  PhiTabMru mru;
  CHECK(mru.empty());

  mru.Sync({T(1), T(2), T(3)}, T(1));
  CHECK(Ids(mru) == std::vector<int>({1}));  // Only what was activated.

  mru.Sync({T(1), T(2), T(3)}, T(3));
  CHECK(Ids(mru) == std::vector<int>({3, 1}));

  mru.Sync({T(1), T(2), T(3)}, T(2));
  CHECK(Ids(mru) == std::vector<int>({2, 3, 1}));

  // Re-activating the tab that is already in front changes nothing — a
  // rebuild for an unrelated reason must not reshuffle the switcher.
  mru.Sync({T(1), T(2), T(3)}, T(2));
  CHECK(Ids(mru) == std::vector<int>({2, 3, 1}));

  // Activating one further down moves it to the front and keeps the rest in
  // their relative order (this is what makes Ctrl+Tab alternate).
  mru.Sync({T(1), T(2), T(3)}, T(1));
  CHECK(Ids(mru) == std::vector<int>({1, 2, 3}));
}

void MruDropsTabsThatLeftTheWindow() {
  PhiTabMru mru;
  mru.Sync({T(1), T(2), T(3)}, T(1));
  mru.Sync({T(1), T(2), T(3)}, T(2));
  mru.Sync({T(1), T(2), T(3)}, T(3));
  CHECK(Ids(mru) == std::vector<int>({3, 2, 1}));

  // Tab 2 closed (or moved to another window): it falls out, and the order
  // of the survivors is untouched.
  mru.Sync({T(1), T(3)}, T(3));
  CHECK(Ids(mru) == std::vector<int>({3, 1}));

  // The window lost its browser: no live tabs, no order, and no active id
  // to put in front.
  mru.Sync({}, TabId());
  CHECK(mru.empty());
}

void MruCountsBothPanesOfASplit() {
  PhiTabMru mru;
  mru.Sync({T(5)}, T(5));
  mru.Sync({T(5), T(6)}, T(6));
  CHECK(Ids(mru) == std::vector<int>({6, 5}));

  // Tab 6 is now pane B of a split whose merged row reports tab 5: the pane
  // is still a live tab of this window, so it stays in the order. (The
  // caller passes both panes; dropping pane B here was the bug the review
  // logged as the MRU filter's `row.id == id` where `row.b_id` was meant.)
  mru.Sync({T(5), T(6)}, T(5));
  CHECK(Ids(mru) == std::vector<int>({5, 6}));
}

// --- The bookmark outline --------------------------------------------------

SidebarCell Folder(const std::string& guid, int depth) {
  SidebarCell cell;
  cell.key = SidebarKey::Folder(guid);
  cell.guid = guid;
  cell.kind = SidebarStoredKind::kFolder;
  cell.depth = depth;
  return cell;
}

SidebarCell Bookmark(const std::string& guid, int depth) {
  SidebarCell cell;
  cell.key = SidebarKey::Cell(guid);
  cell.guid = guid;
  cell.kind = SidebarStoredKind::kBookmark;
  cell.depth = depth;
  return cell;
}

std::vector<std::string> Guids(const std::vector<SidebarCell>& cells,
                               const std::set<std::string>& expanded) {
  std::vector<std::string> out;
  for (const size_t row : PhiVisibleOutlineRows(cells, expanded)) {
    out.push_back(cells[row].guid);
  }
  return out;
}

void CollapsedFolderHidesItsWholeSubtree() {
  //  a            (folder)
  //    a1         (folder)
  //      a1x
  //    a2
  //  b
  const std::vector<SidebarCell> cells = {
      Folder("a", 0),   Folder("a1", 1), Bookmark("a1x", 2),
      Bookmark("a2", 1), Bookmark("b", 0),
  };

  // Nothing expanded: the two roots, and not a single child.
  CHECK(Guids(cells, {}) == std::vector<std::string>({"a", "b"}));

  // `a` open, `a1` still closed: a1 shows, its child does not.
  CHECK(Guids(cells, {"a"}) ==
        std::vector<std::string>({"a", "a1", "a2", "b"}));

  // The whole chain open.
  CHECK(Guids(cells, {"a", "a1"}) ==
        std::vector<std::string>({"a", "a1", "a1x", "a2", "b"}));

  // Expanding a folder whose PARENT is closed reveals nothing: disclosure
  // is a chain, not a per-folder flag.
  CHECK(Guids(cells, {"a1"}) == std::vector<std::string>({"a", "b"}));

  // A guid in the set that no longer names a folder is inert.
  CHECK(Guids(cells, {"a", "gone"}) ==
        std::vector<std::string>({"a", "a1", "a2", "b"}));
}

void AnEmptyOutlineHasNoRows() {
  CHECK(PhiVisibleOutlineRows({}, {}).empty());
  CHECK(PhiVisibleOutlineRows({}, {"a"}).empty());
}

// --- Keys, never indices ----------------------------------------------------

struct Row {
  SidebarKey key;
};

void AKeyFindsItsRowAcrossARebuild() {
  std::vector<Row> rows = {{SidebarKey::Tab(7)},
                           {SidebarKey::Cell("g1")},
                           {SidebarKey::Tab(9)}};
  CHECK(PhiIndexOfKey(rows, SidebarKey::Tab(7)) == 0);
  CHECK(PhiIndexOfKey(rows, SidebarKey::Cell("g1")) == 1);
  CHECK(PhiIndexOfKey(rows, SidebarKey::Tab(9)) == 2);

  // A row arrives above the one being tracked: the index moved, the key did
  // not, and that is the whole point of keying drag state by it.
  rows.insert(rows.begin(), Row{SidebarKey::Tab(3)});
  CHECK(PhiIndexOfKey(rows, SidebarKey::Tab(9)) == 3);

  // The tracked row went away entirely.
  rows.erase(rows.begin() + 3);
  CHECK(PhiIndexOfKey(rows, SidebarKey::Tab(9)) == -1);

  // An empty key matches nothing — never row 0.
  CHECK(PhiIndexOfKey(rows, SidebarKey()) == -1);
  // Kinds do not cross: a cell guid never matches a folder key.
  CHECK(PhiIndexOfKey(rows, SidebarKey::Folder("g1")) == -1);
}

}  // namespace

int main() {
  MruPutsTheActiveTabFirst();
  MruDropsTabsThatLeftTheWindow();
  MruCountsBothPanesOfASplit();
  CollapsedFolderHidesItsWholeSubtree();
  AnEmptyOutlineHasNoRows();
  AKeyFindsItsRowAcrossARebuild();
  if (g_failures == 0) {
    std::printf("sidebar_view_test: all checks passed\n");
  }
  return g_failures == 0 ? 0 : 1;
}
