// Host tests for the shell's menu builders (docs/architecture-review.md
// §3.5, §3.1 principle 4, lane 1 §3.2 and cluster P): every context menu the
// shell window drops is a PURE function of a snapshot, so its contents and
// its order can be pinned here instead of only by the box's suite-menu.
//
// The Win32 half of phi_shell_menus.cc (PhiRunPopupMenu — the HMENU tree and
// the TrackPopupMenu loop) is behind `#if defined(_WIN32)` and compiles away
// on the host. The one target dependency the builders keep is the string
// catalog, stubbed below: PhiStr echoes its key, so a localized label shows
// up in an assertion as the key that produced it.

#include <cstdio>
#include <string>
#include <vector>

#include "phi_shell_menus.h"

using namespace phi_client;

// The catalog lives in phi_i18n.cc, which is Win32 file I/O. Lookup never
// fails there and never fails here either.
namespace phi_client {
const wchar_t* PhiStr(const char* key) {
  static std::wstring held;
  held.assign(key, key + std::char_traits<char>::length(key));
  return held.c_str();
}
}  // namespace phi_client

namespace {

int g_failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                \
    }                                                              \
  } while (0)

std::string Joined(const PhiMenuSpec& spec) {
  std::string out;
  for (const std::string& key : spec.Keys()) {
    out += key.empty() ? "." : key;
    out += " ";
  }
  return out;
}

PhiMenuChoice Choice(const char* id, const wchar_t* label) {
  PhiMenuChoice choice;
  choice.id = id;
  choice.label = label;
  return choice;
}

// --- The group header ------------------------------------------------------

void GroupHeaderMenuListsColors() {
  std::vector<PhiMenuColorOption> colors = {{"grey", false}, {"blue", true}};
  const PhiMenuSpec spec = PhiBuildGroupHeaderMenu(colors);
  CHECK(Joined(spec) ==
        "group.rename . group.color group.color - group.ungroup group.close ");
  // The swatches ride the submenu, checked by NAME — the colour the header
  // wore when the menu was built, not an index into a rebuilt row.
  CHECK(spec.items[1].submenu);
  CHECK(spec.items[1].items[1].target.id == "blue");
  CHECK(spec.items[1].items[1].checked);
  CHECK(!spec.items[1].items[0].checked);
  CHECK(spec.items[1].items[0].label == L"tabs.tabGroup.colorName.grey");
}

// --- The tab row -----------------------------------------------------------

PhiTabMenuInputs PlainTab() {
  PhiTabMenuInputs in;
  in.bookmark_folders = true;
  return in;
}

void TabMenuFollowsTheMacOrder() {
  const PhiMenuSpec spec = PhiBuildTabRowMenu(PlainTab());
  CHECK(Joined(spec) ==
        "tab.pin tab.duplicate tab.copyUrl - tab.openAsSplit - "
        "tab.bookmark . tab.newFolder - tab.newGroup - "
        "tab.close tab.closeOthers ");
  // "Add to Folder" always exists (the fresh-folder row lives in it) but has
  // no separator above its only entry.
  const PhiMenuItem& folders = spec.items[7];
  CHECK(folders.submenu);
  CHECK(folders.items.size() == 1u);
}

void TabMenuPairsWithTheFocusedTab() {
  PhiTabMenuInputs in = PlainTab();
  in.current_can_pair = true;
  const PhiMenuSpec spec = PhiBuildTabRowMenu(in);
  CHECK(spec.Find("tab.splitCurrent") != nullptr);
  CHECK(spec.Find("tab.openAsSplit") == nullptr);
  CHECK(spec.Find("tab.splitCurrent")->label == L"Split with Current");
  // A row already in a split offers neither: it has a partner.
  in.split_bound = true;
  const PhiMenuSpec bound = PhiBuildTabRowMenu(in);
  CHECK(bound.Find("tab.splitCurrent") == nullptr);
  CHECK(bound.Find("tab.openAsSplit") == nullptr);
}

void TabMenuGreysWhatIncognitoCannotPersist() {
  PhiTabMenuInputs in = PlainTab();
  in.incognito = true;
  in.bookmark_folders = false;  // The shell skips the store entirely.
  const PhiMenuSpec spec = PhiBuildTabRowMenu(in);
  CHECK(!spec.Find("tab.pin")->enabled);
  CHECK(spec.Find("tab.pin")->grey_when_disabled);
  CHECK(!spec.Find("tab.bookmark")->enabled);
  CHECK(spec.Find("tab.addToFolder") == nullptr);
  CHECK(spec.Find("tab.newFolder") == nullptr);
  CHECK(spec.Find("tab.duplicate")->enabled);
}

void TabMenuSwitchesBlocksOnGroupMembership() {
  PhiTabMenuInputs in = PlainTab();
  in.groups = {Choice("g1", L"Research (2)")};
  const PhiMenuSpec loose = PhiBuildTabRowMenu(in);
  CHECK(loose.Find("tab.newGroup") != nullptr);
  CHECK(loose.Find("tab.joinGroup") != nullptr);
  CHECK(loose.Find("tab.moveNewGroup") == nullptr);
  CHECK(loose.Find("tab.leaveGroup") == nullptr);

  in.grouped = true;
  const PhiMenuSpec member = PhiBuildTabRowMenu(in);
  CHECK(member.Find("tab.newGroup") == nullptr);
  CHECK(member.Find("tab.joinGroup")->target.id == "g1");
  CHECK(member.Find("tab.moveNewGroup") != nullptr);
  CHECK(member.Find("tab.leaveGroup") != nullptr);

  // No other group to join: the submenu is not emitted at all (an empty
  // dropdown arrow was the old code's DestroyMenu branch).
  in.groups.clear();
  const PhiMenuSpec alone = PhiBuildTabRowMenu(in);
  CHECK(alone.Find("tab.joinGroup") == nullptr);
  CHECK(alone.Find("tab.moveNewGroup") != nullptr);
}

void TabMenuCarriesSpaceTargetsByIdNotIndex() {
  PhiTabMenuInputs in = PlainTab();
  in.spaces = {Choice("2", L"Work"), Choice("5", L"Home")};
  const PhiMenuSpec spec = PhiBuildTabRowMenu(in);
  const PhiMenuItem* move = spec.Find("tab.moveToSpace");
  const PhiMenuItem* clone = spec.Find("tab.cloneToSpace");
  CHECK(move && clone);
  CHECK(move->target.id == "2");
  CHECK(move->label == L"Work");
  // Move and clone list the SAME Spaces; the action tells them apart, so a
  // reordered Space list cannot make one act as the other.
  CHECK(clone->target.id == "2");
  CHECK(spec.Keys().size() > 0u);
}

void MergedTabMenuActsOnThePair() {
  PhiTabMenuInputs in;
  in.merged = true;
  const PhiMenuSpec spec = PhiBuildTabRowMenu(in);
  CHECK(Joined(spec) ==
        "tab.pin tab.duplicate tab.copyLeft tab.copyRight - "
        "tab.reversePanes tab.convertSplit tab.removeSplit - "
        "tab.bookmark - tab.close tab.closeOthers ");
  // One key, two labels: pinning a split is still "pin this row".
  CHECK(spec.Find("tab.pin")->label == L"Pin Split");
  CHECK(spec.Find("tab.bookmark")->label == L"Add Split to Bookmark");
  // The convert row is labelled by where it TAKES you.
  CHECK(spec.Find("tab.convertSplit")->label == L"Convert to Vertical Split");
  in.split_stacked = true;
  CHECK(PhiBuildTabRowMenu(in).Find("tab.convertSplit")->label ==
        L"Convert to Horizontal Split");
}

void AgentDrivenTabLeadsWithTheTakeover() {
  PhiTabMenuInputs in = PlainTab();
  in.agent_driven = true;
  const PhiMenuSpec taken = PhiBuildTabRowMenu(in);
  CHECK(taken.items[0].key == "tab.takeControl");
  CHECK(taken.items[1].separator);
  in.agent_reclaimed = true;
  const PhiMenuSpec back = PhiBuildTabRowMenu(in);
  CHECK(back.items[0].key == "tab.handBack");
  CHECK(back.items[0].label == L"Hand Back to Agent");
}

// --- Pinned cells and bookmark rows ---------------------------------------

void PinnedCellMenuTracksItsCell() {
  PhiPinnedMenuInputs in;
  const PhiMenuSpec plain = PhiBuildPinnedCellMenu(in);
  CHECK(Joined(plain) ==
        "pin.open pin.duplicate pin.copyUrl pin.rename pin.editUrl - "
        "pin.unpin - pin.close ");
  // Nothing bound: Close is dead but the row stays.
  CHECK(!plain.Find("pin.close")->enabled);

  in.split_cell = true;
  in.open = true;
  const PhiMenuSpec split = PhiBuildPinnedCellMenu(in);
  CHECK(Joined(split) ==
        "pin.open pin.duplicate pin.copyLeft pin.copyRight pin.rename - "
        "pin.convertSplit - pin.unpin - pin.close ");
  CHECK(split.Find("pin.close")->enabled);
  CHECK(split.Find("pin.editUrl") == nullptr);  // A split pin keeps its panes.
  CHECK(split.Find("pin.unpin")->label == L"Unpin Split");
}

void BookmarkRowMenuSplitsLeavesFromFolders() {
  PhiBookmarkMenuInputs in;
  in.spaces = {Choice("2", L"Work")};
  const PhiMenuSpec leaf = PhiBuildBookmarkRowMenu(in);
  CHECK(Joined(leaf) ==
        "bm.open bm.openSplit bm.copyUrl bm.rename bm.editUrl - "
        ". bm.moveToSpace . bm.cloneToSpace - bm.remove ");
  CHECK(leaf.Find("bm.remove")->label == L"Delete");

  in.folder = true;
  const PhiMenuSpec folder = PhiBuildBookmarkRowMenu(in);
  CHECK(Joined(folder) ==
        "bm.rename bm.changeIcon bm.newFolder - bm.remove ");
  // A folder's subtree stays where it is: no cross-Space rows.
  CHECK(folder.Find("bm.moveToSpace") == nullptr);
  CHECK(folder.Find("bm.remove")->label == L"Delete Folder");
}

// --- The tab area ----------------------------------------------------------

void TabAreaMenuChecksTheLayoutInUse() {
  PhiTabAreaMenuInputs in;
  in.mode = PhiLayoutMode::kBalanced;
  in.can_delete_space = true;
  in.can_reprofile = true;
  in.active_profile = "p2";
  in.profiles = {Choice("p1", L"Personal"), Choice("p2", L"Work")};
  const PhiMenuSpec spec = PhiBuildTabAreaMenu(in);
  CHECK(Joined(spec) ==
        "area.newTab area.reopen - area.newRootFolder area.bookmarkAll - "
        ". area.layout area.layout area.layout - "
        "area.newSpace area.renameSpace area.changeIcon area.setTheme "
        ". area.profile area.profile area.deleteSpace ");
  const PhiMenuItem& layouts = spec.items[6];
  CHECK(layouts.items[0].target.id == "performance");
  CHECK(!layouts.items[0].checked);
  CHECK(layouts.items[1].checked);
  // The profile rows carry the name the confirmation prompt repeats.
  const PhiMenuItem* profile = spec.Find("area.profile");
  CHECK(profile->target.id == "p1");
  CHECK(profile->target.text == L"Personal");
  CHECK(!profile->checked);
  CHECK(spec.items[12].items[1].checked);
}

void TabAreaMenuDisablesWithoutGreyingAnEmptyStrip() {
  PhiTabAreaMenuInputs in;
  in.tabs_empty = true;
  const PhiMenuSpec spec = PhiBuildTabAreaMenu(in);
  const PhiMenuItem* all = spec.Find("area.bookmarkAll");
  CHECK(!all->enabled);
  CHECK(!all->grey_when_disabled);  // Inert, not greyed off the record.
  CHECK(spec.Find("area.newRootFolder")->enabled);
}

void TabAreaMenuStopsAtTheLayoutForIncognito() {
  PhiTabAreaMenuInputs in;
  in.incognito = true;
  const PhiMenuSpec spec = PhiBuildTabAreaMenu(in);
  CHECK(Joined(spec) ==
        "area.newTab area.reopen - area.newRootFolder area.bookmarkAll - "
        ". area.layout area.layout area.layout ");
  CHECK(!spec.Find("area.newRootFolder")->enabled);
  CHECK(spec.Find("area.newRootFolder")->grey_when_disabled);
  CHECK(spec.Find("area.newSpace") == nullptr);
}

void TabAreaMenuSaysWhenThereAreNoProfiles() {
  PhiTabAreaMenuInputs in;
  const PhiMenuSpec spec = PhiBuildTabAreaMenu(in);
  const PhiMenuItem& profiles = spec.items[12];
  CHECK(profiles.submenu);
  CHECK(profiles.items.size() == 1u);
  CHECK(profiles.items[0].key.empty());  // Never returnable.
  CHECK(!profiles.items[0].enabled);
  CHECK(profiles.items[0].label == L"menuBar.noProfiles");
  CHECK(!spec.Find("area.deleteSpace")->enabled);
}

// --- The Spaces strip ------------------------------------------------------

void SpaceSwitcherMarksTheActiveSpace() {
  std::vector<PhiMenuChoice> spaces = {Choice("2", L"Work"),
                                       Choice("5", L"Home")};
  spaces[1].glyph = L"\U0001F3E0";
  const PhiMenuSpec compact = PhiBuildSpaceSwitcherMenu(spaces, "5", false);
  CHECK(Joined(compact) == "space.switch space.switch ");
  CHECK(!compact.items[0].checked);
  CHECK(compact.items[1].checked);
  CHECK(compact.items[1].default_item);
  CHECK(compact.items[1].glyph == L"\U0001F3E0");

  const PhiMenuSpec overflow = PhiBuildSpaceSwitcherMenu(spaces, "5", true);
  CHECK(Joined(overflow) ==
        "space.switch space.switch - space.new space.newWithProfile ");
}

void AgentChipMenuHasTwoRows() {
  const PhiMenuSpec spec = PhiBuildAgentChipMenu();
  CHECK(Joined(spec) == "agent.transcript - agent.finish ");
}

// --- The bookmark bar ------------------------------------------------------

PhiMenuBookmarkNode Leaf(const char* guid, const char* url) {
  PhiMenuBookmarkNode node;
  node.guid = guid;
  node.url = url;
  node.title = L"row";
  return node;
}

void BookmarkBarMenuSkipsThePillsAlreadyShown() {
  std::vector<PhiMenuBookmarkNode> roots = {Leaf("a", "https://a"),
                                            Leaf("b", "https://b"),
                                            Leaf("c", "https://c")};
  const PhiMenuSpec all = PhiBuildBookmarkBarMenu(roots, 0);
  CHECK(all.items.size() == 3u);
  // The overflow dropdown starts where the bar's own pills stopped.
  const PhiMenuSpec rest = PhiBuildBookmarkBarMenu(roots, 2);
  CHECK(rest.items.size() == 1u);
  CHECK(rest.items[0].target.id == "c");
  // The address travels WITH the action: the row may be gone by the time the
  // click is applied, and it still opens what the user saw.
  CHECK(rest.items[0].target.url == "https://c");
}

void BookmarkBarMenuNestsFourDeepAndStopsAt200() {
  // Five nested folders: the fifth is dropped, so its leaf never appears.
  PhiMenuBookmarkNode deep;
  deep.folder = true;
  deep.guid = "d4";
  deep.title = L"d4";
  deep.children.push_back(Leaf("leaf", "https://leaf"));
  for (int i = 3; i >= 0; --i) {
    PhiMenuBookmarkNode up;
    up.folder = true;
    up.guid = "d" + std::to_string(i);
    up.title = L"up";
    up.children.push_back(deep);
    deep = up;
  }
  const PhiMenuSpec spec = PhiBuildBookmarkBarMenu({deep}, 0);
  const PhiMenuItem* at = &spec.items[0];
  for (int depth = 0; depth < 3; ++depth) {
    CHECK(at->submenu);
    CHECK(at->items.size() == 1u);
    at = &at->items[0];
  }
  // The fourth folder is the last one built: depth 4 is where folders stop,
  // so its dropdown is empty rather than carrying the fifth (and its leaf).
  CHECK(at->submenu);
  CHECK(at->items.empty());
  CHECK(at->key.empty());

  std::vector<PhiMenuBookmarkNode> many;
  for (int i = 0; i < 250; ++i) {
    many.push_back(Leaf("g", "https://x"));
  }
  CHECK(PhiBuildBookmarkBarMenu(many, 0).items.size() == 200u);
}

// An empty folder still drops a menu — the bar has always behaved that way,
// and `submenu` (not "has children") is what says so.
void EmptyFolderIsStillASubmenu() {
  PhiMenuBookmarkNode folder;
  folder.folder = true;
  folder.guid = "f";
  folder.title = L"Empty";
  const PhiMenuSpec spec = PhiBuildBookmarkBarMenu({folder}, 0);
  CHECK(spec.items.size() == 1u);
  CHECK(spec.items[0].submenu);
  CHECK(spec.items[0].items.empty());
}

}  // namespace

int main() {
  GroupHeaderMenuListsColors();
  TabMenuFollowsTheMacOrder();
  TabMenuPairsWithTheFocusedTab();
  TabMenuGreysWhatIncognitoCannotPersist();
  TabMenuSwitchesBlocksOnGroupMembership();
  TabMenuCarriesSpaceTargetsByIdNotIndex();
  MergedTabMenuActsOnThePair();
  AgentDrivenTabLeadsWithTheTakeover();
  PinnedCellMenuTracksItsCell();
  BookmarkRowMenuSplitsLeavesFromFolders();
  TabAreaMenuChecksTheLayoutInUse();
  TabAreaMenuDisablesWithoutGreyingAnEmptyStrip();
  TabAreaMenuStopsAtTheLayoutForIncognito();
  TabAreaMenuSaysWhenThereAreNoProfiles();
  SpaceSwitcherMarksTheActiveSpace();
  AgentChipMenuHasTwoRows();
  BookmarkBarMenuSkipsThePillsAlreadyShown();
  BookmarkBarMenuNestsFourDeepAndStopsAt200();
  EmptyFolderIsStillASubmenu();
  if (g_failures) {
    std::printf("shell_menus_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("shell_menus_test: OK\n");
  return 0;
}
