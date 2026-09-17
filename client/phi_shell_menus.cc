// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_shell_menus.h"

#include "phibrowser-win/client/phi_features.h"

#include <utility>

#include "phibrowser-win/client/phi_i18n.h"

#if defined(_WIN32)
#include "phibrowser-win/client/phi_fluent.h"
#include "phibrowser-win/client/phi_menu_paint.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#endif

namespace phi_client {

// --- The item helpers ------------------------------------------------------

PhiMenuItem PhiMenuItem::Separator() {
  PhiMenuItem item;
  item.separator = true;
  return item;
}

PhiMenuItem PhiMenuItem::Command(std::string key, std::wstring label) {
  PhiMenuItem item;
  item.key = std::move(key);
  item.label = std::move(label);
  return item;
}

PhiMenuItem PhiMenuItem::Submenu(std::wstring label,
                                 std::vector<PhiMenuItem> items) {
  PhiMenuItem item;
  item.label = std::move(label);
  item.submenu = true;
  item.items = std::move(items);
  return item;
}

namespace {

const PhiMenuItem* FindIn(const std::vector<PhiMenuItem>& items,
                          std::string_view key) {
  for (const PhiMenuItem& item : items) {
    if (!item.key.empty() && item.key == key) {
      return &item;
    }
    if (const PhiMenuItem* found = FindIn(item.items, key)) {
      return found;
    }
  }
  return nullptr;
}

void KeysIn(const std::vector<PhiMenuItem>& items,
            std::vector<std::string>* out) {
  for (const PhiMenuItem& item : items) {
    out->push_back(item.separator ? "-" : item.key);
    KeysIn(item.items, out);
  }
}

// A dynamic row, one per choice, all sharing a key.
std::vector<PhiMenuItem> ChoiceRows(const std::vector<PhiMenuChoice>& choices,
                                    const char* key) {
  std::vector<PhiMenuItem> rows;
  rows.reserve(choices.size());
  for (const PhiMenuChoice& choice : choices) {
    PhiMenuItem item = PhiMenuItem::Command(key, choice.label);
    item.target.id = choice.id;
    item.glyph = choice.glyph;
    rows.push_back(std::move(item));
  }
  return rows;
}

// An item that persists to the Space store: greyed off the record in an
// incognito shell, exactly as the old `persist_flags` did.
PhiMenuItem PersistCommand(const char* key,
                           std::wstring label,
                           bool incognito) {
  PhiMenuItem item = PhiMenuItem::Command(key, std::move(label));
  item.enabled = !incognito;
  return item;
}

}  // namespace

const PhiMenuItem* PhiMenuSpec::Find(std::string_view key) const {
  return FindIn(items, key);
}

std::vector<std::string> PhiMenuSpec::Keys() const {
  std::vector<std::string> keys;
  KeysIn(items, &keys);
  return keys;
}

// --- Group header ----------------------------------------------------------

PhiMenuSpec PhiBuildGroupHeaderMenu(
    const std::vector<PhiMenuColorOption>& colors) {
  std::vector<PhiMenuItem> swatches;
  swatches.reserve(colors.size());
  for (const PhiMenuColorOption& color : colors) {
    PhiMenuItem item = PhiMenuItem::Command(
        "group.color",
        PhiStr((std::string("tabs.tabGroup.colorName.") + color.name).c_str()));
    item.target.id = color.name;
    item.checked = color.checked;
    swatches.push_back(std::move(item));
  }
  PhiMenuSpec spec;
  spec.items.push_back(
      PhiMenuItem::Command("group.rename", L"Rename Group..."));
  spec.items.push_back(
      PhiMenuItem::Submenu(L"Change Color", std::move(swatches)));
  spec.items.push_back(PhiMenuItem::Separator());
  spec.items.push_back(PhiMenuItem::Command("group.ungroup", L"Ungroup"));
  spec.items.push_back(PhiMenuItem::Command("group.close", L"Close Group"));
  return spec;
}

// --- Tab row ---------------------------------------------------------------

namespace {

// The Mac's group block: an ungrouped row may start a group or join one that
// exists; a grouped row may move to another, to a new one, or leave. A
// merged split row takes the same block — the pair moves as one.
void AppendGroupBlock(const PhiTabMenuInputs& in,
                      std::vector<PhiMenuItem>& items) {
  if (!in.grouped) {
    items.push_back(PhiMenuItem::Command("tab.newGroup", L"New Tab Group"));
    if (!in.groups.empty()) {
      items.push_back(PhiMenuItem::Submenu(
          L"Add to Group", ChoiceRows(in.groups, "tab.joinGroup")));
    }
  } else {
    if (!in.groups.empty()) {
      items.push_back(PhiMenuItem::Submenu(
          L"Move to Group", ChoiceRows(in.groups, "tab.joinGroup")));
    }
    items.push_back(
        PhiMenuItem::Command("tab.moveNewGroup", L"Move to New Group"));
    items.push_back(
        PhiMenuItem::Command("tab.leaveGroup", L"Remove from Group"));
  }
}

}  // namespace

PhiMenuSpec PhiBuildTabRowMenu(const PhiTabMenuInputs& in) {
  PhiMenuSpec spec;
  std::vector<PhiMenuItem>& items = spec.items;
  // A tab an agent is holding leads with the takeover: it is the action the
  // badge is asking about.
  if (in.agent_driven) {
    items.push_back(PhiMenuItem::Command(
        in.agent_reclaimed ? "tab.handBack" : "tab.takeControl",
        in.agent_reclaimed ? L"Hand Back to Agent" : L"Take Control"));
    items.push_back(PhiMenuItem::Separator());
  }
  if (!in.merged) {
    items.push_back(PersistCommand("tab.pin", L"Pin", in.incognito));
    items.push_back(PhiMenuItem::Command("tab.duplicate", L"Duplicate"));
    items.push_back(PhiMenuItem::Command("tab.copyUrl", L"Copy Link"));
    items.push_back(PhiMenuItem::Separator());
    if (!in.split_bound) {
      // "Split with Current" pairs this row with the FOCUSED tab; on the
      // focused row itself — or when the focused tab is already split — it
      // becomes "Open as Split", a fresh pane (the Mac's two labels).
      items.push_back(PhiMenuItem::Command(
          in.current_can_pair ? "tab.splitCurrent" : "tab.openAsSplit",
          in.current_can_pair ? L"Split with Current" : L"Open as Split"));
      items.push_back(PhiMenuItem::Separator());
    }
    items.push_back(
        PersistCommand("tab.bookmark", L"Add to Bookmark", in.incognito));
    // "Add to Folder": this Space's bookmark folders in outline order,
    // nesting indented, then a fresh folder — the Mac's addToFolder submenu.
    // Nothing persists from incognito.
    if (in.bookmark_folders) {
      std::vector<PhiMenuItem> folder_items =
          ChoiceRows(in.folders, "tab.addToFolder");
      if (!folder_items.empty()) {
        folder_items.push_back(PhiMenuItem::Separator());
      }
      folder_items.push_back(
          PhiMenuItem::Command("tab.newFolder", L"New Folder..."));
      items.push_back(
          PhiMenuItem::Submenu(L"Add to Folder", std::move(folder_items)));
    }
    items.push_back(PhiMenuItem::Separator());
    AppendGroupBlock(in, items);
    items.push_back(PhiMenuItem::Separator());
    // Move/Clone to Space — the Mac's cross-Space submenus (the tab's own
    // Space skipped; nothing persists from incognito).
    if (!in.spaces.empty()) {
      items.push_back(PhiMenuItem::Submenu(
          L"Move to Space", ChoiceRows(in.spaces, "tab.moveToSpace")));
      items.push_back(PhiMenuItem::Submenu(
          L"Clone to Space", ChoiceRows(in.spaces, "tab.cloneToSpace")));
      items.push_back(PhiMenuItem::Separator());
    }
  } else {
    // The merged split row: every action operates on the pair.
    items.push_back(PersistCommand("tab.pin", L"Pin Split", in.incognito));
    items.push_back(PhiMenuItem::Command("tab.duplicate", L"Duplicate Split"));
    items.push_back(PhiMenuItem::Command("tab.copyLeft", L"Copy Left URL"));
    items.push_back(PhiMenuItem::Command("tab.copyRight", L"Copy Right URL"));
    items.push_back(PhiMenuItem::Separator());
    // The Mac's "Reverse Panes": swap the two sides of this split.
    items.push_back(
        PhiMenuItem::Command("tab.reversePanes", L"Reverse Panes"));
    // The Mac's convert item, labelled by where it takes you: a side-by-side
    // pair offers the stacked arrangement and vice versa.
    items.push_back(PhiMenuItem::Command("tab.convertSplit",
                                         in.split_stacked
                                             ? L"Convert to Horizontal Split"
                                             : L"Convert to Vertical Split"));
    items.push_back(
        PhiMenuItem::Command("tab.removeSplit", L"Remove from Split"));
    items.push_back(PhiMenuItem::Separator());
    items.push_back(PersistCommand("tab.bookmark", L"Add Split to Bookmark",
                                   in.incognito));
    items.push_back(PhiMenuItem::Separator());
    // The pair groups as one: both panes join (or leave) together, as the
    // Mac's per-tab group block does for a split member.
    AppendGroupBlock(in, items);
    items.push_back(PhiMenuItem::Separator());
  }
  items.push_back(PhiMenuItem::Command("tab.close", L"Close"));
  items.push_back(PhiMenuItem::Command("tab.closeOthers", L"Close Other Tabs"));
  return spec;
}

// --- Pinned cell -----------------------------------------------------------

PhiMenuSpec PhiBuildPinnedCellMenu(const PhiPinnedMenuInputs& in) {
  PhiMenuSpec spec;
  std::vector<PhiMenuItem>& items = spec.items;
  items.push_back(PhiMenuItem::Command("pin.open", L"Open"));
  items.push_back(PhiMenuItem::Command(
      "pin.duplicate", in.split_cell ? L"Duplicate Split" : L"Duplicate"));
  if (in.split_cell) {
    items.push_back(PhiMenuItem::Command("pin.copyLeft", L"Copy Left URL"));
    items.push_back(PhiMenuItem::Command("pin.copyRight", L"Copy Right URL"));
  } else {
    items.push_back(PhiMenuItem::Command("pin.copyUrl", L"Copy Link"));
  }
  items.push_back(PhiMenuItem::Command("pin.rename", L"Rename..."));
  // The Mac's Edit…: the pin's address (a split pin keeps its panes).
  if (!in.split_cell) {
    items.push_back(PhiMenuItem::Command("pin.editUrl", L"Edit URL..."));
  }
  if (in.split_cell) {
    items.push_back(PhiMenuItem::Separator());
    items.push_back(PhiMenuItem::Command("pin.convertSplit",
                                         in.horizontal
                                             ? L"Convert to Horizontal Split"
                                             : L"Convert to Vertical Split"));
  }
  items.push_back(PhiMenuItem::Separator());
  items.push_back(PhiMenuItem::Command(
      "pin.unpin", in.split_cell ? L"Unpin Split" : L"Unpin"));
  items.push_back(PhiMenuItem::Separator());
  PhiMenuItem close = PhiMenuItem::Command("pin.close", L"Close");
  close.enabled = in.open;
  items.push_back(std::move(close));
  return spec;
}

// --- Bookmark row ----------------------------------------------------------

PhiMenuSpec PhiBuildBookmarkBarAreaMenu(bool incognito) {
  PhiMenuSpec spec;
  spec.items.push_back(
      PersistCommand("bar.newRootFolder", L"New Folder...", incognito));
  return spec;
}

PhiMenuSpec PhiBuildBookmarkRowMenu(const PhiBookmarkMenuInputs& in) {
  PhiMenuSpec spec;
  std::vector<PhiMenuItem>& items = spec.items;
  if (!in.folder) {
    items.push_back(PhiMenuItem::Command("bm.open", L"Open in New Tab"));
    items.push_back(PhiMenuItem::Command("bm.openSplit", L"Open as Split"));
    if (in.split_row) {
      items.push_back(PhiMenuItem::Command("bm.copyLeft", L"Copy Left URL"));
      items.push_back(PhiMenuItem::Command("bm.copyRight", L"Copy Right URL"));
    } else {
      items.push_back(PhiMenuItem::Command("bm.copyUrl", L"Copy Link"));
    }
  }
  items.push_back(PhiMenuItem::Command("bm.rename", L"Rename..."));
  if (!in.folder && !in.split_row) {
    // The Mac's Edit…
    items.push_back(PhiMenuItem::Command("bm.editUrl", L"Edit URL..."));
  }
  if (in.split_row) {
    items.push_back(PhiMenuItem::Command("bm.convertSplit",
                                         in.horizontal
                                             ? L"Convert to Horizontal Split"
                                             : L"Convert to Vertical Split"));
  }
  if (in.folder) {
    // The Mac's folder menu: Rename, then the icon picker.
    items.push_back(PhiMenuItem::Command("bm.changeIcon", L"Change Icon..."));
    items.push_back(
        PhiMenuItem::Command("bm.newFolder", L"New Nested Folder..."));
  }
  // Move/Clone to Space — the Mac's appendSpaceTransferMenuItems, leaf rows
  // only (a folder's subtree stays where it is).
  if (!in.folder && !in.spaces.empty()) {
    items.push_back(PhiMenuItem::Separator());
    items.push_back(PhiMenuItem::Submenu(
        L"Move to Space", ChoiceRows(in.spaces, "bm.moveToSpace")));
    items.push_back(PhiMenuItem::Submenu(
        L"Clone to Space", ChoiceRows(in.spaces, "bm.cloneToSpace")));
  }
  items.push_back(PhiMenuItem::Separator());
  items.push_back(PhiMenuItem::Command(
      "bm.remove", in.folder ? L"Delete Folder" : L"Delete"));
  return spec;
}

// --- The tab area ----------------------------------------------------------

PhiMenuSpec PhiBuildTabAreaMenu(const PhiTabAreaMenuInputs& in) {
  PhiMenuSpec spec;
  std::vector<PhiMenuItem>& items = spec.items;
  if (in.space_only) {
    // The pip's menu (§5.4): the Space block without its leading separator
    // and without the "New Space" row (the "+" pip and the switcher do that).
    PhiTabAreaMenuInputs rest = in;
    rest.space_only = false;
    PhiMenuSpec full = PhiBuildTabAreaMenu(rest);
    bool past_layout = false;
    for (PhiMenuItem& item : full.items) {
      if (item.key == "area.renameSpace") {
        past_layout = true;
      }
      if (past_layout) {
        items.push_back(std::move(item));
      }
    }
    return spec;
  }
  items.push_back(PhiMenuItem::Command("area.newTab", L"New Tab"));
  items.push_back(PhiMenuItem::Command("area.reopen", L"Reopen Closed Tab"));
  items.push_back(PhiMenuItem::Separator());
  items.push_back(
      PersistCommand("area.newRootFolder", L"New Folder...", in.incognito));
  PhiMenuItem bookmark_all =
      PersistCommand("area.bookmarkAll", L"Bookmark All Tabs", in.incognito);
  // Two different disabled states: incognito greys the row off the record,
  // an empty tab list only makes it inert.
  bookmark_all.enabled = !in.incognito && !in.tabs_empty;
  bookmark_all.grey_when_disabled = in.incognito;
  items.push_back(std::move(bookmark_all));
  items.push_back(PhiMenuItem::Separator());
  const struct {
    const char* id;
    PhiLayoutMode mode;
    const wchar_t* label;
  } kLayouts[] = {
      // Performance is retired on Windows (impl log D9): two layouts.
      {"comfortable", PhiLayoutMode::kComfortable, L"Horizontal tabs"},
      {"balanced", PhiLayoutMode::kBalanced, L"Vertical tabs"},
  };
  std::vector<PhiMenuItem> layouts;
  for (const auto& entry : kLayouts) {
    PhiMenuItem item = PhiMenuItem::Command("area.layout", entry.label);
    item.target.id = entry.id;
    item.checked = in.mode == entry.mode;
    layouts.push_back(std::move(item));
  }
  items.push_back(PhiMenuItem::Submenu(L"Layout", std::move(layouts)));
  if (in.incognito) {
    // Incognito shells have no Spaces: the active-Space block never appears.
    return spec;
  }
  // The active Space's own theme and profile, in the Mac's order
  // (appendActiveSpaceMenuItems): rename, icon, theme, profile, delete.
  //
  // Change Profile — the Mac's radio list. Re-binding respawns the Space's
  // window, which the default Space's cannot be, so its rows read the
  // binding out but stay disabled.
  std::vector<PhiMenuItem> profiles;
  for (const PhiMenuChoice& profile : in.profiles) {
    PhiMenuItem item = PhiMenuItem::Command("area.profile", profile.label);
    item.target.id = profile.id;
    item.target.text = profile.label;
    item.checked = profile.id == in.active_profile;
    item.enabled = in.can_reprofile;
    profiles.push_back(std::move(item));
  }
  if (profiles.empty()) {
    PhiMenuItem none;
    none.label = PhiStr("menuBar.noProfiles");
    none.enabled = false;
    profiles.push_back(std::move(none));
  }
  items.push_back(PhiMenuItem::Separator());
  items.push_back(PhiMenuItem::Command("area.newSpace", L"New Space..."));
  items.push_back(
      PhiMenuItem::Command("area.renameSpace", L"Rename Space..."));
  items.push_back(PhiMenuItem::Command("area.changeIcon", L"Change Icon..."));
  // Set Theme opens the Mac's editor panel (swatch row + saturation slider)
  // rather than a list of theme names.
  items.push_back(
      PhiMenuItem::Command("area.setTheme", PhiStr("sidebar.setTheme")));
  items.push_back(PhiMenuItem::Submenu(
      PhiStr("app.activeSpaceContextMenu.profileSubmenuTitle"),
      std::move(profiles)));
  PhiMenuItem del =
      PhiMenuItem::Command("area.deleteSpace", L"Delete Space...");
  del.enabled = in.can_delete_space;
  items.push_back(std::move(del));
  return spec;
}

// --- The Spaces strip ------------------------------------------------------

PhiMenuSpec PhiBuildAgentChipMenu() {
  PhiMenuSpec spec;
  if (kPhiTranscriptEnabled) {
    spec.items.push_back(
        PhiMenuItem::Command("agent.transcript", L"Show Transcript"));
    spec.items.push_back(PhiMenuItem::Separator());
  }
  spec.items.push_back(PhiMenuItem::Command("agent.finish", L"Finish Task"));
  return spec;
}

PhiMenuSpec PhiBuildSpaceSwitcherMenu(const std::vector<PhiMenuChoice>& spaces,
                                      const std::vector<PhiMenuChoice>& agents,
                                      const std::string& watched_agent,
                                      const std::string& active_space,
                                      bool with_new_space,
                                      bool with_new_profile) {
  PhiMenuSpec spec;
  spec.items = ChoiceRows(spaces, "space.switch");
  for (PhiMenuItem& item : spec.items) {
    if (item.target.id == active_space) {
      // Bold (the default item) marks the active Space: a row with a bitmap
      // shows no check mark.
      item.checked = true;
      item.default_item = true;
    }
  }
  if (!agents.empty()) {
    // The agent Spaces after the user's, past a rule, as the strip groups
    // them — the compact pip's only road to watch mode.
    spec.items.push_back(PhiMenuItem::Separator());
    std::vector<PhiMenuItem> rows = ChoiceRows(agents, "agent.watch");
    for (PhiMenuItem& item : rows) {
      if (item.target.id == watched_agent) {
        item.checked = true;
        item.default_item = true;
      }
      spec.items.push_back(std::move(item));
    }
  }
  if (!with_new_space) {
    return spec;
  }
  // "New Space…" after a rule, wearing the plus (the Mac's spaceSwitcherMenu
  // newSpaceItem): it opens the create form — inline in a docked sidebar,
  // else in the standalone window.
  spec.items.push_back(PhiMenuItem::Separator());
  PhiMenuItem new_space = PhiMenuItem::Command("space.new", L"New Space\u2026");
  new_space.glyph = L"\uE710";  // MDL2 Add.
  spec.items.push_back(std::move(new_space));
  if (!with_new_profile) {
    return spec;
  }
  // The Mac's create panel offers a fresh Profile for the new Space; here it
  // is the second row (profile creation is asynchronous — the Space follows
  // once the profile exists).
  spec.items.push_back(PhiMenuItem::Command(
      "space.newWithProfile", L"New Space with New Profile..."));
  return spec;
}

// --- The bookmark bar ------------------------------------------------------

namespace {

void AppendBookmarkNodes(const std::vector<PhiMenuBookmarkNode>& rows,
                         size_t skip,
                         int depth,
                         size_t* leaves,
                         std::vector<PhiMenuItem>* into) {
  size_t at = 0;
  for (const PhiMenuBookmarkNode& row : rows) {
    if (at++ < skip || *leaves >= 200) {
      continue;
    }
    if (row.folder) {
      if (depth >= 4) {
        continue;
      }
      std::vector<PhiMenuItem> children;
      AppendBookmarkNodes(row.children, 0, depth + 1, leaves, &children);
      into->push_back(PhiMenuItem::Submenu(row.title, std::move(children)));
    } else if (!row.url.empty()) {
      ++*leaves;
      PhiMenuItem item = PhiMenuItem::Command("bar.open", row.title);
      item.target.id = row.guid;
      item.target.url = row.url;
      into->push_back(std::move(item));
    }
  }
}

}  // namespace

PhiMenuSpec PhiBuildBookmarkBarMenu(
    const std::vector<PhiMenuBookmarkNode>& roots,
    size_t skip_first) {
  PhiMenuSpec spec;
  size_t leaves = 0;
  AppendBookmarkNodes(roots, skip_first, 0, &leaves, &spec.items);
  return spec;
}

// --- Showing one -----------------------------------------------------------

#if defined(_WIN32)

namespace {

// The Fluent decorations by KEY (docs/windows-native-ui.md §3.5): the
// chord in the right column, the destructive rows in red, an icon on the
// primary rows. Kept out of the pure builders so the host test pins the
// order alone; the painter reads them from the row.
struct RowDecor {
  const char* key;
  const wchar_t* shortcut;
  bool danger;
  const wchar_t* icon;
};
constexpr RowDecor kRowDecor[] = {
    {"area.newTab", L"Ctrl+T", false, fluent::kAdd},
    {"area.reopen", L"Ctrl+Shift+T", false, fluent::kClock},
    {"area.newRootFolder", nullptr, false, fluent::kFolder},
    {"area.bookmarkAll", L"Ctrl+Shift+D", false, fluent::kFavoriteStar},
    {"area.newSpace", nullptr, false, fluent::kAdd},
    {"area.renameSpace", nullptr, false, fluent::kRename},
    {"area.changeIcon", nullptr, false, fluent::kEmoji},
    {"area.setTheme", nullptr, false, fluent::kColor},
    {"area.deleteSpace", nullptr, true, fluent::kDelete},
    {"space.new", nullptr, false, fluent::kAdd},
    {"tab.pin", nullptr, false, fluent::kPin},
    {"tab.duplicate", nullptr, false, fluent::kCopy},
    {"tab.copyUrl", L"Ctrl+Shift+C", false, fluent::kLink},
    {"tab.bookmark", L"Ctrl+D", false, fluent::kFavoriteStar},
    {"tab.close", L"Ctrl+W", false, fluent::kCancel},
    {"tab.closeOthers", nullptr, false, nullptr},
    {"tab.removeSplit", nullptr, false, nullptr},
    {"pin.open", nullptr, false, fluent::kOpenInNewWindow},
    {"pin.copyUrl", nullptr, false, fluent::kLink},
    {"pin.rename", nullptr, false, fluent::kRename},
    {"pin.unpin", nullptr, false, fluent::kUnpin},
    {"pin.close", L"Ctrl+W", false, fluent::kCancel},
    {"bm.open", nullptr, false, fluent::kOpenInNewWindow},
    {"bm.copyUrl", nullptr, false, fluent::kLink},
    {"bm.rename", nullptr, false, fluent::kRename},
    {"bm.changeIcon", nullptr, false, fluent::kEmoji},
    {"bm.newFolder", nullptr, false, fluent::kFolder},
    {"bm.remove", nullptr, true, fluent::kDelete},
    {"bar.newRootFolder", nullptr, false, fluent::kFolder},
    {"bar.open", nullptr, false, fluent::kGlobe},
    {"group.rename", nullptr, false, fluent::kRename},
    {"group.ungroup", nullptr, false, nullptr},
    {"group.close", nullptr, true, fluent::kCancel},
    {"agent.finish", nullptr, true, nullptr},
};

const RowDecor* DecorFor(const std::string& key) {
  for (const RowDecor& decor : kRowDecor) {
    if (key == decor.key) {
      return &decor;
    }
  }
  return nullptr;
}

// Flattens the spec into an HMENU of owner-drawn rows, numbering the
// actionable rows from 1 so TPM_RETURNCMD's answer indexes `actions`
// directly. Submenus are owned by their parent, so the caller destroys
// exactly one handle; the rows live in `store` for the loop.
bool AppendSpec(HMENU menu,
                const std::vector<PhiMenuItem>& items,
                std::vector<const PhiMenuItem*>* actions,
                PhiMenuRowStore* store) {
  for (const PhiMenuItem& item : items) {
    if (item.separator) {
      store->AppendSeparator(menu);
      continue;
    }
    if (item.submenu) {
      HMENU sub = ::CreatePopupMenu();
      if (!sub) {
        return false;
      }
      if (!AppendSpec(sub, item.items, actions, store)) {
        ::DestroyMenu(sub);
        return false;
      }
      PhiMenuRow* row = store->AppendSubmenu(menu, sub, item.label);
      row->height_dip = 32;
      row->picture = item.glyph;
      continue;
    }
    // A label-only row (the "no profiles" placeholder) carries no key and
    // takes id 0, which TrackPopupMenu can never return.
    UINT id = 0;
    if (!item.key.empty()) {
      actions->push_back(&item);
      id = static_cast<UINT>(actions->size());
    }
    PhiMenuRow* row = store->AppendCommand(menu, id, item.label, nullptr,
                                           item.enabled);
    row->height_dip = 32;
    row->picture = item.glyph;
    if (const RowDecor* decor = DecorFor(item.key)) {
      if (decor->shortcut && row->shortcut.empty()) {
        row->shortcut = decor->shortcut;
      }
      row->danger = decor->danger;
      if (decor->icon) {
        row->icon = decor->icon;
      }
    }
    UINT state = item.enabled ? MFS_ENABLED : MFS_DISABLED;
    if (!item.enabled && !item.grey_when_disabled) {
      // Inert but not greyed: the design retires that state — a disabled
      // row reads disabled (docs/windows-native-ui.md §3.5).
      state = MFS_DISABLED;
    }
    if (item.checked) {
      state |= MFS_CHECKED;
    }
    if (item.default_item) {
      state |= MFS_DEFAULT;
    }
    MENUITEMINFOW info = {sizeof(info)};
    info.fMask = MIIM_STATE;
    info.fState = state;
    ::SetMenuItemInfoW(menu, ::GetMenuItemCount(menu) - 1, TRUE, &info);
  }
  return true;
}

}  // namespace

PhiMenuAction PhiRunPopupMenu(const PhiMenuSpec& spec,
                              const PhiPopupMenuRequest& request) {
  PhiMenuAction action;
  if (!request.owner) {
    return action;
  }
  HMENU menu = ::CreatePopupMenu();
  if (!menu) {
    return action;
  }
  // Pointers into `spec`, which outlives the loop: the SPEC is a value the
  // caller built from a snapshot, so nothing here reaches into a rebuildable
  // cache (docs/architecture-review.md §3.1 principle 4).
  std::vector<const PhiMenuItem*> actions;
  PhiMenuRowStore rows;
  const bool built = AppendSpec(menu, spec.items, &actions, &rows);
  UINT chosen = 0;
  if (built) {
    if (request.palette) {
      PhiMenuApplyTheme(menu, *request.palette);
    }
    // The nested loop: a PhiModalScope, so the coordinator parks pipe verbs
    // and the shell parks its own close until the menu is down.
    PhiModalScope modal;
    chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, request.align | TPM_RETURNCMD, request.x, request.y, 0,
        request.owner, nullptr));
  }
  ::DestroyMenu(menu);  // Owns every submenu it was given.
  if (chosen >= 1 && chosen <= actions.size()) {
    const PhiMenuItem* item = actions[chosen - 1];
    action.key = item->key;
    action.target = item->target;
  }
  return action;
}

#endif  // defined(_WIN32)

}  // namespace phi_client
