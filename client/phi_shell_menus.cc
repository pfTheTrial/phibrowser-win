// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_shell_menus.h"

#include "phibrowser-win/client/phi_features.h"

#include <utility>

#include "phibrowser-win/client/phi_i18n.h"

#if defined(_WIN32)
#include "phibrowser-win/client/phi_glyph_bitmap.h"
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
    // The Mac's group block: an ungrouped tab may start a group or join one
    // that exists; a grouped tab may move to another, to a new one, or leave.
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
      {"performance", PhiLayoutMode::kPerformance, L"Performance"},
      {"balanced", PhiLayoutMode::kBalanced, L"Balanced"},
      {"comfortable", PhiLayoutMode::kComfortable, L"Comfortable"},
  };
  std::vector<PhiMenuItem> layouts;
  for (const auto& entry : kLayouts) {
    PhiMenuItem item = PhiMenuItem::Command("area.layout", entry.label);
    item.target.id = entry.id;
    item.checked = in.mode == entry.mode;
    layouts.push_back(std::move(item));
  }
  items.push_back(PhiMenuItem::Submenu(L"Layout Mode", std::move(layouts)));
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
                                      bool with_new_space) {
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
  spec.items.push_back(PhiMenuItem::Separator());
  spec.items.push_back(PhiMenuItem::Command("space.new", L"New Space"));
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

// Flattens the spec into an HMENU, numbering the actionable rows from 1 so
// TPM_RETURNCMD's answer indexes `actions` directly. Submenus are owned by
// their parent, so the caller destroys exactly one handle.
bool AppendSpec(HMENU menu,
                const std::vector<PhiMenuItem>& items,
                int glyph_px,
                std::vector<const PhiMenuItem*>* actions,
                std::vector<HBITMAP>* bitmaps) {
  for (const PhiMenuItem& item : items) {
    if (item.separator) {
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      continue;
    }
    if (item.submenu) {
      HMENU sub = ::CreatePopupMenu();
      if (!sub) {
        return false;
      }
      if (!AppendSpec(sub, item.items, glyph_px, actions, bitmaps)) {
        ::DestroyMenu(sub);
        return false;
      }
      ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                    reinterpret_cast<UINT_PTR>(sub), item.label.c_str());
      continue;
    }
    UINT flags = MF_STRING;
    if (!item.enabled) {
      flags |= MF_DISABLED;
      if (item.grey_when_disabled) {
        flags |= MF_GRAYED;
      }
    }
    if (item.checked) {
      flags |= MF_CHECKED;
    }
    if (item.default_item) {
      flags |= MF_DEFAULT;
    }
    // A label-only row (the "no profiles" placeholder) carries no key and
    // takes id 0, which TrackPopupMenu can never return.
    UINT id = 0;
    if (!item.key.empty()) {
      actions->push_back(&item);
      id = static_cast<UINT>(actions->size());
    }
    ::AppendMenuW(menu, flags, id, item.label.c_str());
    if (id && glyph_px > 0 && !item.glyph.empty()) {
      if (HBITMAP bitmap = PhiSpaceGlyphBitmap(item.glyph, glyph_px,
                                               ::GetSysColor(COLOR_MENUTEXT))) {
        bitmaps->push_back(bitmap);
        PhiSetMenuItemBitmap(menu, id, bitmap);
      }
    }
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
  std::vector<HBITMAP> bitmaps;
  const bool built =
      AppendSpec(menu, spec.items, request.glyph_px, &actions, &bitmaps);
  UINT chosen = 0;
  if (built) {
    // The nested loop: a PhiModalScope, so the coordinator parks pipe verbs
    // and the shell parks its own close until the menu is down.
    PhiModalScope modal;
    chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, request.align | TPM_RETURNCMD, request.x, request.y, 0,
        request.owner, nullptr));
  }
  ::DestroyMenu(menu);  // Owns every submenu it was given.
  for (HBITMAP bitmap : bitmaps) {
    ::DeleteObject(bitmap);
  }
  if (chosen >= 1 && chosen <= actions.size()) {
    const PhiMenuItem* item = actions[chosen - 1];
    action.key = item->key;
    action.target = item->target;
  }
  return action;
}

#endif  // defined(_WIN32)

}  // namespace phi_client
