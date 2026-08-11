// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_menu_bar.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

// Static commands. Dynamic rows live in the ranges below; nothing overlaps.

// Dynamic ranges.
constexpr UINT kSpaceFirst = 2000;       // + index into space_ids_
constexpr UINT kSpaceLast = 2099;
constexpr UINT kThemeFollow = 2100;      // "Follow app theme"
constexpr UINT kThemeFirst = 2101;       // + index into PhiBuiltInThemes()
constexpr UINT kThemeLast = 2140;
constexpr UINT kProfileFirst = 2200;     // Change Profile rows (disabled)
constexpr UINT kProfileLast = 2299;
constexpr UINT kDeleteProfileFirst = 2300;  // Delete Profile rows (disabled)
constexpr UINT kDeleteProfileLast = 2399;
constexpr UINT kTabFirst = 2500;         // + index into tab_indices_
constexpr UINT kTabLast = 2799;
constexpr UINT kBookmarkFirst = 3000;    // + index into bookmark_urls_
constexpr UINT kBookmarkLast = 3499;

void Append(HMENU menu, UINT id, const wchar_t* title, bool enabled = true) {
  ::AppendMenuW(menu, MF_STRING | (enabled ? 0u : MF_DISABLED | MF_GRAYED), id,
                title);
}

void Separator(HMENU menu) {
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
}

void ClearMenu(HMENU menu) {
  // Deleting by position also destroys any submenu the item carried.
  while (::GetMenuItemCount(menu) > 0) {
    ::DeleteMenu(menu, 0, MF_BYPOSITION);
  }
}

void SetEnabled(HMENU menu, UINT id, bool enabled) {
  ::EnableMenuItem(menu, id,
                   MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_DISABLED |
                                                              MF_GRAYED));
}

// An item's display title, mnemonic marker and "\tCtrl+…" shortcut stripped —
// what a test addresses it by.
std::wstring PlainTitle(const std::wstring& raw) {
  std::wstring out;
  for (const wchar_t c : raw) {
    if (c == L'\t') {
      break;
    }
    if (c != L'&') {
      out.push_back(c);
    }
  }
  return out;
}

std::wstring ItemTitle(HMENU menu, int position) {
  MENUITEMINFOW info = {sizeof(info)};
  info.fMask = MIIM_STRING;
  if (!::GetMenuItemInfoW(menu, static_cast<UINT>(position), TRUE, &info) ||
      info.cch == 0) {
    return std::wstring();
  }
  std::wstring buffer(info.cch + 1, L'\0');  // Room for the terminator.
  info.cch = static_cast<UINT>(buffer.size());
  info.dwTypeData = buffer.data();
  ::GetMenuItemInfoW(menu, static_cast<UINT>(position), TRUE, &info);
  buffer.resize(info.cch);  // The copied length, terminator not included.
  return buffer;
}

}  // namespace

PhiMenuBar::PhiMenuBar(PhiWindowController* controller)
    : controller_(controller) {}

PhiMenuBar::~PhiMenuBar() {
  // Never attached (the shell draws the titles itself), so the tree is ours
  // to destroy — it takes every popup with it.
  if (bar_) {
    ::DestroyMenu(bar_);
  }
}

void PhiMenuBar::Install(HWND shell) {
  if (bar_) {
    return;
  }
  shell_ = shell;
  bar_ = ::CreateMenu();
  if (!bar_) {
    return;
  }
  phi_ = ::CreatePopupMenu();
  file_ = ::CreatePopupMenu();
  edit_ = ::CreatePopupMenu();
  view_ = ::CreatePopupMenu();
  spaces_ = ::CreatePopupMenu();
  history_ = ::CreatePopupMenu();
  bookmarks_ = ::CreatePopupMenu();
  tab_ = ::CreatePopupMenu();
  window_ = ::CreatePopupMenu();
  help_ = ::CreatePopupMenu();

  auto top = [&](HMENU menu, const wchar_t* title) {
    ::AppendMenuW(bar_, MF_POPUP | MF_STRING,
                  reinterpret_cast<UINT_PTR>(menu), title);
  };
  top(phi_, L"Phi");
  top(file_, L"&File");
  top(edit_, L"&Edit");
  top(view_, L"&View");
  top(spaces_, L"&Spaces");
  top(history_, L"Hi&story");
  top(bookmarks_, L"&Bookmarks");
  top(tab_, L"&Tab");
  top(window_, L"&Window");
  top(help_, L"&Help");

  BuildStatic();
  // No SetMenu: the titles render in the shell's caption band.
}

std::vector<std::wstring> PhiMenuBar::Titles() const {
  std::vector<std::wstring> titles;
  if (!bar_) {
    return titles;
  }
  const int count = ::GetMenuItemCount(bar_);
  for (int i = 0; i < count; ++i) {
    titles.push_back(PlainTitle(ItemTitle(bar_, i)));
  }
  return titles;
}

void PhiMenuBar::ShowAt(size_t index, int screen_x, int screen_y) {
  if (!bar_ || !shell_) {
    return;
  }
  MENUITEMINFOW info = {sizeof(info)};
  info.fMask = MIIM_SUBMENU;
  if (!::GetMenuItemInfoW(bar_, static_cast<UINT>(index), TRUE, &info) ||
      !info.hSubMenu) {
    return;
  }
  // WM_INITMENUPOPUP fires for this popup exactly as it would for an
  // attached bar, so the dynamic rows refresh on the way down. Commands
  // arrive as WM_COMMAND on the shell.
  ::TrackPopupMenu(info.hSubMenu, TPM_LEFTALIGN | TPM_TOPALIGN, screen_x,
                   screen_y, 0, shell_, nullptr);
}

void PhiMenuBar::BuildStatic() {
  // Phi — the Mac app menu, minus Services/Hide/Show All (Mac-only).
  Append(phi_, kCmdAbout, L"About Phi");
  Separator(phi_);
  Append(phi_, kCmdPreferences, L"Preferences...\tCtrl+,");
  Append(phi_, kCmdCheckUpdate, L"Check for Update...", false);
  Separator(phi_);
  Append(phi_, kCmdDeleteBrowsingData, L"Delete Browsing Data...", false);
  Append(phi_, kCmdImportBrowser, L"Import from Another Browser...", false);
  Separator(phi_);
  Append(phi_, kCmdQuit, L"Quit Phi\tCtrl+Shift+Q");

  // File. Save Page As has no shortcut — the Mac strips Chromium's.
  Append(file_, kCmdNewTab, L"New Tab\tCtrl+T");
  Append(file_, kCmdNewWindow, L"New Window\tCtrl+N");
  Append(file_, kCmdNewIncognitoWindow, L"New Incognito Window\tCtrl+Shift+N",
         false);
  Append(file_, kCmdNewIncognitoSpace, L"New Incognito Space", false);
  Append(file_, kCmdReopenClosedTab, L"Reopen Closed Tab\tCtrl+Shift+T",
         false);
  Append(file_, kCmdOpenFile, L"Open File...\tCtrl+O", false);
  Append(file_, kCmdOpenLocation, L"Open Location...\tCtrl+L");
  Separator(file_);
  Append(file_, kCmdCloseWindow, L"Close Window\tCtrl+Shift+W");
  Append(file_, kCmdCloseTab, L"Close Tab\tCtrl+W");
  Append(file_, kCmdSavePageAs, L"Save Page As...", false);
  Separator(file_);
  Append(file_, kCmdPrint, L"Print...\tCtrl+P", false);

  // Edit. The text commands drive the address field when it has focus; the
  // page's own editing belongs to Chromium and is not routed from here yet.
  Append(edit_, kCmdUndo, L"Undo\tCtrl+Z");
  Append(edit_, kCmdRedo, L"Redo\tCtrl+Shift+Z", false);
  Separator(edit_);
  Append(edit_, kCmdCut, L"Cut\tCtrl+X");
  Append(edit_, kCmdCopy, L"Copy\tCtrl+C");
  Append(edit_, kCmdCopyUrl, L"Copy URL\tCtrl+Shift+C");
  Append(edit_, kCmdPaste, L"Paste\tCtrl+V");
  Append(edit_, kCmdPasteMatchStyle, L"Paste and Match Style\tCtrl+Shift+V",
         false);
  Append(edit_, kCmdDelete, L"Delete");
  Append(edit_, kCmdSelectAll, L"Select All\tCtrl+A");
  Separator(edit_);
  HMENU find = ::CreatePopupMenu();
  Append(find, kCmdSearchWeb, L"Search the Web...", false);
  Separator(find);
  Append(find, kCmdFind, L"Find...\tCtrl+F", false);
  Append(find, kCmdFindNext, L"Find Next\tCtrl+G", false);
  Append(find, kCmdFindPrevious, L"Find Previous\tCtrl+Shift+G", false);
  ::AppendMenuW(edit_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(find), L"Find");

  // View — Chromium's rows, then the Mac's Phi section (layout modes, bars).
  Append(view_, kCmdStop, L"Stop\tEsc");
  Append(view_, kCmdReload, L"Reload This Page\tCtrl+R");
  Append(view_, kCmdForceReload, L"Force Reload This Page\tCtrl+Shift+R");
  Separator(view_);
  Append(view_, kCmdFullScreen, L"Enter Full Screen\tF11", false);
  Append(view_, kCmdActualSize, L"Actual Size\tCtrl+0", false);
  Append(view_, kCmdZoomIn, L"Zoom In\tCtrl++", false);
  Append(view_, kCmdZoomOut, L"Zoom Out\tCtrl+-", false);
  Separator(view_);
  HMENU developer = ::CreatePopupMenu();
  Append(developer, kCmdViewSource, L"View Source\tCtrl+U", false);
  Append(developer, kCmdDevTools, L"Developer Tools\tCtrl+Shift+I", false);
  Append(developer, kCmdInspect, L"Inspect Elements", false);
  Append(developer, kCmdJsConsole, L"JavaScript Console\tCtrl+Shift+J",
         false);
  ::AppendMenuW(view_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(developer), L"Developer");
  Separator(view_);
  Append(view_, kCmdLayoutHeader, L"Layout Mode", false);  // Section header.
  Append(view_, kCmdLayoutBalanced, L"Balanced");
  Append(view_, kCmdLayoutPerformance, L"Performance");
  Append(view_, kCmdLayoutComfortable, L"Comfortable");
  Separator(view_);
  Append(view_, kCmdAlwaysShowBookmarkBar,
         L"Always Show Bookmark Bar\tCtrl+Shift+B", false);
  Append(view_, kCmdBookmarkBarOnNewTab, L"Show Bookmark Bar on New Tab",
         false);
  Separator(view_);
  Append(view_, kCmdToggleSidebar, L"Toggle Sidebar\tCtrl+S", false);
  Append(view_, kCmdToggleChatbar, L"Toggle Chatbar\tCtrl+Shift+S", false);
  Append(view_, kCmdNewConversation, L"New Conversation\tCtrl+Shift+O",
         false);

  // Spaces / Bookmarks / Tab hold dynamic rows; they are filled on open.

  // History. Only Back/Forward have a surface so far.
  Append(history_, kCmdHome, L"Home\tAlt+Home", false);
  Append(history_, kCmdBack, L"Back\tAlt+Left");
  Append(history_, kCmdForward, L"Forward\tAlt+Right");
  Separator(history_);
  Append(history_, kCmdRecentlyClosedHeader, L"Recently Closed", false);
  Separator(history_);
  Append(history_, kCmdShowFullHistory, L"Show Full History\tCtrl+H", false);

  // Window.
  Append(window_, kCmdMinimize, L"Minimize");
  Append(window_, kCmdZoomWindow, L"Zoom");
  Separator(window_);
  Append(window_, kCmdNameWindow, L"Name Window...", false);
  Separator(window_);
  Append(window_, kCmdDownloads, L"Downloads\tCtrl+J", false);
  Append(window_, kCmdExtensions, L"Extensions", false);
  Append(window_, kCmdTaskManager, L"Task Manager", false);

  // Help. Time Machine restore is a Mac app-bundle mechanism — skipped.
  Append(help_, kCmdReportIssue, L"Report an Issue...", false);
  Append(help_, kCmdWhatsNew, L"What's New", false);
  Append(help_, kCmdDocumentation, L"Documentation\tF1", false);
  Separator(help_);
  HMENU user_data = ::CreatePopupMenu();
  Append(user_data, kCmdExportUserData, L"Export User Data...", false);
  Append(user_data, kCmdImportUserData, L"Import User Data...", false);
  ::AppendMenuW(help_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(user_data), L"Manage User Data");

  RefreshSpaces();
  RefreshBookmarks();
  RefreshTab();
}

void PhiMenuBar::OnMenuOpening(HMENU menu) {
  if (menu == phi_) {
    RefreshPhi();
  } else if (menu == file_) {
    RefreshFile();
  } else if (menu == edit_) {
    RefreshEdit();
  } else if (menu == view_) {
    RefreshView();
  } else if (menu == spaces_) {
    RefreshSpaces();
  } else if (menu == history_) {
    RefreshHistory();
  } else if (menu == bookmarks_) {
    RefreshBookmarks();
  } else if (menu == tab_) {
    RefreshTab();
  }
}

void PhiMenuBar::RefreshPhi() {
  SetEnabled(phi_, kCmdPreferences, controller_->host_ != nullptr);
}

void PhiMenuBar::RefreshFile() {
  controller_->RebuildTabs();
  SetEnabled(file_, kCmdCloseTab, !controller_->tabs_.empty());
  SetEnabled(file_, kCmdOpenLocation, controller_->url_field_ != nullptr);
}

void PhiMenuBar::RefreshEdit() {
  // The text commands act on the address field; they sleep while the page has
  // the keyboard (Chromium runs its own edit commands there).
  const bool field = controller_->url_field_ &&
                     ::GetFocus() == controller_->url_field_;
  SetEnabled(edit_, kCmdUndo,
             field && ::SendMessageW(controller_->url_field_, EM_CANUNDO, 0,
                                     0) != 0);
  SetEnabled(edit_, kCmdCut, field);
  SetEnabled(edit_, kCmdCopy, field);
  SetEnabled(edit_, kCmdPaste, field);
  SetEnabled(edit_, kCmdDelete, field);
  SetEnabled(edit_, kCmdSelectAll, field);
  SetEnabled(edit_, kCmdCopyUrl, !controller_->nav_url_.empty());
}

void PhiMenuBar::RefreshView() {
  controller_->RebuildNavState();
  SetEnabled(view_, kCmdStop, controller_->nav_loading_);
  PhiLayoutMode mode = PhiLayoutMode::kPerformance;
  if (controller_->host_) {
    mode = controller_->host_->LayoutMode();
  }
  const UINT chosen = mode == PhiLayoutMode::kBalanced ? kCmdLayoutBalanced
                      : mode == PhiLayoutMode::kComfortable
                          ? kCmdLayoutComfortable
                          : kCmdLayoutPerformance;
  ::CheckMenuRadioItem(view_, kCmdLayoutBalanced, kCmdLayoutComfortable,
                       chosen, MF_BYCOMMAND);
}

std::vector<std::pair<std::string, std::wstring>> PhiMenuBar::SpaceList()
    const {
  std::vector<std::pair<std::string, std::wstring>> spaces;
  if (!controller_->host_) {
    return spaces;
  }
  for (const PhiSpaceView& space : controller_->host_->Spaces()) {
    std::wstring title = base::UTF8ToWide(
        !space.name.empty() ? space.name : "Space " + space.id);
    const std::wstring glyph = base::UTF8ToWide(space.icon_text);
    // MDL2 stand-ins only render in the MDL2 face; menus use the UI font.
    if (!glyph.empty() && !PhiIsPrivateGlyph(glyph)) {
      title = glyph + L"  " + title;
    }
    spaces.emplace_back(space.id, std::move(title));
  }
  return spaces;
}

void PhiMenuBar::RefreshSpaces() {
  ClearMenu(spaces_);
  space_ids_.clear();
  profile_ids_.clear();

  Append(spaces_, kCmdNewSpace, L"New Space...",
         controller_->host_ != nullptr);
  Append(spaces_, kCmdRenameSpace, L"Rename Space...",
         controller_->host_ != nullptr);
  // The icon picker lives in Settings > Spaces; this item waits for an
  // inline surface.
  Append(spaces_, kCmdChangeIcon, L"Change Icon...", false);

  // Edit Theme — the same choices the Space chip's context menu offers,
  // applied to the ACTIVE Space.
  const std::string active = controller_->active_space_;
  HMENU themes = ::CreatePopupMenu();
  const std::string pinned =
      controller_->host_ ? controller_->host_->SpaceThemeId(active)
                         : std::string();
  ::AppendMenuW(themes, MF_STRING | (pinned.empty() ? MF_CHECKED : 0),
                kThemeFollow, L"Follow app theme");
  Separator(themes);
  const auto& built_in = PhiBuiltInThemes();
  for (size_t i = 0; i < built_in.size(); ++i) {
    ::AppendMenuW(themes,
                  MF_STRING | (pinned == built_in[i].id ? MF_CHECKED : 0),
                  kThemeFirst + static_cast<UINT>(i),
                  base::UTF8ToWide(built_in[i].name).c_str());
  }
  ::AppendMenuW(spaces_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(themes), L"Edit Theme");

  // Change Profile — the Mac's radio list; switching is not implemented, so
  // every row is disabled but the binding still reads truthfully. Delete
  // Profile rows ARE live, guarded the Mac's way: Default and any profile
  // still bound to a Space refuse.
  std::string active_profile;
  std::vector<std::string> bound_profiles;
  if (controller_->host_) {
    for (const PhiSpaceView& space : controller_->host_->Spaces()) {
      bound_profiles.push_back(space.profile_id);
      if (space.id == active) {
        active_profile = space.profile_id;
      }
    }
  }
  HMENU profiles = ::CreatePopupMenu();
  HMENU delete_profiles = ::CreatePopupMenu();
  int profile_count = 0;
  if (controller_->framework_) {
    std::optional<base::Value> list = base::JSONReader::Read(
        controller_->framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
    if (list && list->is_list()) {
      for (const base::Value& v : list->GetList()) {
        if (!v.is_dict()) {
          continue;
        }
        const std::string* id = v.GetDict().FindString("id");
        const std::string* name = v.GetDict().FindString("name");
        if (!id || profile_count >= static_cast<int>(kProfileLast -
                                                     kProfileFirst)) {
          continue;
        }
        const std::wstring title =
            base::UTF8ToWide(name && !name->empty() ? *name : *id);
        ::AppendMenuW(profiles,
                      MF_STRING | MF_DISABLED | MF_GRAYED |
                          (*id == active_profile ? MF_CHECKED : 0),
                      kProfileFirst + static_cast<UINT>(profile_count),
                      title.c_str());
        const bool in_use =
            std::find(bound_profiles.begin(), bound_profiles.end(), *id) !=
            bound_profiles.end();
        const bool deletable = *id != "Default" && !in_use;
        // The Mac marks in-use rows "<name> — in use by a Space".
        const std::wstring delete_title =
            in_use ? title + L" — in use by a Space" : title;
        ::AppendMenuW(delete_profiles,
                      MF_STRING |
                          (deletable ? 0u : MF_DISABLED | MF_GRAYED),
                      kDeleteProfileFirst + static_cast<UINT>(profile_count),
                      delete_title.c_str());
        profile_ids_.push_back(*id);
        ++profile_count;
      }
    }
  }
  if (profile_count == 0) {
    ::AppendMenuW(profiles, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                  L"No Profiles");
    ::AppendMenuW(delete_profiles, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                  L"No Profiles to Delete");
  }
  ::AppendMenuW(spaces_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(profiles), L"Change Profile");
  Append(spaces_, kCmdDeleteSpace, L"Delete Space...",
         controller_->host_ && active != "1");
  Separator(spaces_);
  const std::vector<std::pair<std::string, std::wstring>> spaces =
      SpaceList();
  const bool several = spaces.size() > 1;
  Append(spaces_, kCmdNextSpace, L"Next Space\tCtrl+Alt+Right", several);
  Append(spaces_, kCmdPreviousSpace, L"Previous Space\tCtrl+Alt+Left",
         several);
  Separator(spaces_);
  for (size_t i = 0; i < spaces.size() && kSpaceFirst + i <= kSpaceLast;
       ++i) {
    std::wstring title = spaces[i].second;
    if (i < 9) {
      title += L"\tCtrl+" + std::to_wstring(i + 1);
    }
    ::AppendMenuW(spaces_,
                  MF_STRING | (spaces[i].first == active ? MF_CHECKED : 0),
                  kSpaceFirst + static_cast<UINT>(i), title.c_str());
    space_ids_.push_back(spaces[i].first);
  }
  Separator(spaces_);
  Append(spaces_, kCmdUrlRules, L"URL Rules...", false);
  Separator(spaces_);
  Append(spaces_, kCmdNewProfile, L"New Profile...",
         controller_->framework_ != nullptr);
  ::AppendMenuW(spaces_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(delete_profiles),
                L"Delete Profile");
}

void PhiMenuBar::RefreshHistory() {
  controller_->RebuildNavState();
  SetEnabled(history_, kCmdBack, controller_->can_go_back_);
  SetEnabled(history_, kCmdForward, controller_->can_go_forward_);
}

void PhiMenuBar::AppendBookmarkRows(HMENU menu, const std::string& parent_guid,
                                    int depth) {
  if (!controller_->host_ || depth > 5) {
    return;
  }
  for (const PhiStoredRowView& row :
       controller_->host_->BookmarkRows(controller_->window_id_,
                                        parent_guid)) {
    const std::wstring shown =
        base::UTF8ToWide(!row.title.empty() ? row.title : row.url);
    if (row.folder) {
      HMENU sub = ::CreatePopupMenu();
      AppendBookmarkRows(sub, row.guid, depth + 1);
      if (::GetMenuItemCount(sub) == 0) {
        ::AppendMenuW(sub, MF_STRING | MF_DISABLED | MF_GRAYED, 0, L"Empty");
      }
      ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                    reinterpret_cast<UINT_PTR>(sub), shown.c_str());
    } else if (!row.url.empty() &&
               bookmark_urls_.size() <= kBookmarkLast - kBookmarkFirst) {
      ::AppendMenuW(menu, MF_STRING,
                    kBookmarkFirst +
                        static_cast<UINT>(bookmark_urls_.size()),
                    shown.c_str());
      bookmark_urls_.push_back(row.url);
    }
  }
}

void PhiMenuBar::RefreshBookmarks() {
  ClearMenu(bookmarks_);
  bookmark_urls_.clear();
  controller_->RebuildNavState();
  Append(bookmarks_, kCmdBookmarkThisTab, L"Bookmark This Tab...\tCtrl+D",
         controller_->host_ && !controller_->nav_url_.empty());
  Append(bookmarks_, kCmdBookmarkAllTabs,
         L"Bookmark All Tabs...\tCtrl+Shift+D", false);
  Separator(bookmarks_);
  Append(bookmarks_, kCmdExportBookmarks, L"Export Bookmarks...", false);
  const int fixed = ::GetMenuItemCount(bookmarks_);
  AppendBookmarkRows(bookmarks_, std::string(), 0);
  if (::GetMenuItemCount(bookmarks_) > fixed) {
    // A separator between the commands and the rows, as on Mac — inserted
    // after the fact so an empty list gets no dangling rule.
    ::InsertMenuW(bookmarks_, static_cast<UINT>(fixed),
                  MF_BYPOSITION | MF_SEPARATOR, 0, nullptr);
  }
}

void PhiMenuBar::RefreshTab() {
  ClearMenu(tab_);
  tab_indices_.clear();
  controller_->RebuildTabs();
  const bool several = controller_->tabs_.size() > 1;
  Append(tab_, kCmdNewTabToRight, L"New Tab to the Right", false);
  Append(tab_, kCmdNextTab, L"Select Next Tab\tCtrl+Tab", several);
  Append(tab_, kCmdPreviousTab, L"Select Previous Tab\tCtrl+Shift+Tab",
         several);
  Append(tab_, kCmdDuplicateTab, L"Duplicate Tab", false);
  Append(tab_, kCmdMuteSite, L"Mute Site", false);
  Append(tab_, kCmdSearchTabs, L"Search Tabs...\tCtrl+Shift+A", false);
  if (controller_->tabs_.empty()) {
    return;
  }
  Separator(tab_);
  for (size_t i = 0; i < controller_->tabs_.size() &&
                     kTabFirst + i <= kTabLast;
       ++i) {
    const auto& row = controller_->tabs_[i];
    ::AppendMenuW(tab_, MF_STRING | (row.active ? MF_CHECKED : 0),
                  kTabFirst + static_cast<UINT>(i),
                  row.title.empty() ? L"Untitled" : row.title.c_str());
    tab_indices_.push_back(row.index);
  }
}

bool PhiMenuBar::OnCommand(UINT id) {
  PhiWindowController* c = controller_;
  const int64_t target =
      c->active_browser_ ? c->active_browser_ : c->window_id_;

  // Dynamic ranges first.
  if (id >= kSpaceFirst && id <= kSpaceLast) {
    const size_t i = id - kSpaceFirst;
    if (i < space_ids_.size()) {
      const std::string space_id = space_ids_[i];
      if (c->HasSpace(space_id)) {
        c->ShowSpace(space_id);
      } else if (c->host_) {
        c->host_->RequestSwitchSpace(c->window_id_, space_id);
      }
      c->RefreshTabs();
    }
    return true;
  }
  if (id >= kCmdSpace1 && id <= kCmdSpace9) {
    // Ctrl+N from the keyboard hook: resolved against the LIVE Space list
    // (space_ids_ only refreshes when the Spaces menu opens).
    const std::vector<std::pair<std::string, std::wstring>> spaces =
        SpaceList();
    const size_t i = id - kCmdSpace1;
    if (i < spaces.size() && spaces[i].first != c->active_space_) {
      const std::string space_id = spaces[i].first;
      if (c->HasSpace(space_id)) {
        c->ShowSpace(space_id);
      } else if (c->host_) {
        c->host_->RequestSwitchSpace(c->window_id_, space_id);
      }
      c->RefreshTabs();
    }
    return true;
  }
  if (id == kThemeFollow) {
    if (c->host_) {
      c->host_->SetSpaceTheme(c->active_space_, std::string());
    }
    return true;
  }
  if (id >= kThemeFirst && id <= kThemeLast) {
    const auto& built_in = PhiBuiltInThemes();
    const size_t i = id - kThemeFirst;
    if (c->host_ && i < built_in.size()) {
      c->host_->SetSpaceTheme(c->active_space_, built_in[i].id);
    }
    return true;
  }
  if (id >= kTabFirst && id <= kTabLast) {
    const size_t i = id - kTabFirst;
    if (c->framework_ && i < tab_indices_.size()) {
      c->framework_->ActivateTab(target, tab_indices_[i]);
      c->RefreshTabs();
    }
    return true;
  }
  if (id >= kBookmarkFirst && id <= kBookmarkLast) {
    const size_t i = id - kBookmarkFirst;
    if (c->host_ && i < bookmark_urls_.size()) {
      c->host_->RequestOpen(c->window_id_, bookmark_urls_[i]);
    }
    return true;
  }
  if (id >= kProfileFirst && id <= kProfileLast) {
    return true;  // Change Profile rows are drawn disabled; nothing to do.
  }
  if (id >= kDeleteProfileFirst && id <= kDeleteProfileLast) {
    const size_t i = id - kDeleteProfileFirst;
    if (c->host_ && i < profile_ids_.size() &&
        PhiConfirm(c->shell_hwnd_, L"Delete Profile",
                   L"All cookies, history, extensions, and saved data on "
                   L"this profile will be permanently removed. This cannot "
                   L"be undone.")) {
      c->host_->DeleteProfileById(profile_ids_[i]);
    }
    return true;
  }

  switch (id) {
    case kCmdAbout:
      ::MessageBoxW(c->shell_hwnd_,
                    L"Phi Browser for Windows\n"
                    L"The Phi client shell over the Phi framework.",
                    L"About Phi", MB_OK | MB_ICONINFORMATION);
      return true;
    case kCmdPreferences:
      if (c->host_) {
        c->host_->RequestSettings();
      }
      return true;
    case kCmdQuit:
    case kCmdCloseWindow:
      // Through the normal close path; posted so the menu unwinds first.
      ::PostMessageW(c->shell_hwnd_, WM_CLOSE, 0, 0);
      return true;
    case kCmdNewTab:
      if (c->framework_) {
        c->framework_->NewTab(target, "");
        c->RefreshTabs();
      }
      return true;
    case kCmdNewWindow:
      if (c->framework_) {
        c->framework_->NewWindow(c->window_id_);
      }
      return true;
    case kCmdOpenLocation:
      if (c->url_field_) {
        ::SetFocus(c->url_field_);
        ::SendMessageW(c->url_field_, EM_SETSEL, 0, -1);
      }
      return true;
    case kCmdCloseTab: {
      for (const auto& row : c->tabs_) {
        if (row.active && c->framework_) {
          c->framework_->CloseTab(target, row.index);
          c->RefreshTabs();
          break;
        }
      }
      return true;
    }
    case kCmdUndo:
    case kCmdCut:
    case kCmdCopy:
    case kCmdPaste:
    case kCmdDelete:
    case kCmdSelectAll: {
      HWND field = c->url_field_;
      if (!field || ::GetFocus() != field) {
        return true;
      }
      switch (id) {
        case kCmdUndo:
          ::SendMessageW(field, WM_UNDO, 0, 0);
          break;
        case kCmdCut:
          ::SendMessageW(field, WM_CUT, 0, 0);
          break;
        case kCmdCopy:
          ::SendMessageW(field, WM_COPY, 0, 0);
          break;
        case kCmdPaste:
          ::SendMessageW(field, WM_PASTE, 0, 0);
          break;
        case kCmdDelete:
          ::SendMessageW(field, WM_CLEAR, 0, 0);
          break;
        case kCmdSelectAll:
          ::SendMessageW(field, EM_SETSEL, 0, -1);
          break;
      }
      return true;
    }
    case kCmdCopyUrl: {
      const std::wstring& url = c->nav_url_;
      if (url.empty() || !::OpenClipboard(c->shell_hwnd_)) {
        return true;
      }
      ::EmptyClipboard();
      const size_t bytes = (url.size() + 1) * sizeof(wchar_t);
      if (HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (wchar_t* dest = static_cast<wchar_t*>(::GlobalLock(handle))) {
          std::copy_n(url.c_str(), url.size() + 1, dest);
          ::GlobalUnlock(handle);
          if (!::SetClipboardData(CF_UNICODETEXT, handle)) {
            ::GlobalFree(handle);
          }
        } else {
          ::GlobalFree(handle);
        }
      }
      ::CloseClipboard();
      return true;
    }
    case kCmdStop:
      if (c->framework_) {
        c->framework_->StopLoading(target);
        c->RefreshTabs();
      }
      return true;
    case kCmdReload:
    case kCmdForceReload:
      if (c->framework_) {
        c->framework_->Reload(target, id == kCmdForceReload);
        c->RefreshTabs();
      }
      return true;
    case kCmdLayoutBalanced:
    case kCmdLayoutPerformance:
    case kCmdLayoutComfortable:
      if (c->host_) {
        c->host_->RequestLayoutMode(
            id == kCmdLayoutBalanced    ? PhiLayoutMode::kBalanced
            : id == kCmdLayoutComfortable ? PhiLayoutMode::kComfortable
                                          : PhiLayoutMode::kPerformance);
      }
      return true;
    case kCmdNewSpace:
      if (c->host_) {
        c->host_->RequestCreateSpace(c->window_id_, /*new_profile=*/false);
        c->RefreshTabs();
      }
      return true;
    case kCmdRenameSpace: {
      if (!c->host_) {
        return true;
      }
      // Prefill with the Space's current name (from the owner's list; the
      // chip label is the same string).
      std::wstring name;
      for (const PhiSpaceView& space : c->host_->Spaces()) {
        if (space.id == c->active_space_) {
          name = base::UTF8ToWide(space.name);
          break;
        }
      }
      if (PhiPromptText(c->shell_hwnd_, L"Rename Space",
                        L"Enter a new name for this Space.", &name)) {
        c->host_->RenameSpace(c->active_space_, base::WideToUTF8(name));
      }
      return true;
    }
    case kCmdDeleteSpace:
      if (c->host_ &&
          PhiConfirm(c->shell_hwnd_, L"Delete Space",
                     L"Bookmarks and pinned tabs belonging to this Space "
                     L"will also be removed. This action cannot be "
                     L"undone.")) {
        c->host_->DeleteSpace(c->active_space_);
      }
      return true;
    case kCmdNewProfile: {
      std::wstring name;
      if (c->framework_ &&
          PhiPromptText(c->shell_hwnd_, L"New Profile",
                        L"Enter a name for the new profile. Each profile "
                        L"has its own cookies, history, and extensions.",
                        &name)) {
        c->framework_->CreateProfile(base::WideToUTF8(name).c_str());
      }
      return true;
    }
    case kCmdNextSpace:
    case kCmdPreviousSpace: {
      const std::vector<std::pair<std::string, std::wstring>> spaces =
          SpaceList();
      if (spaces.size() < 2) {
        return true;
      }
      size_t at = 0;
      for (size_t i = 0; i < spaces.size(); ++i) {
        if (spaces[i].first == c->active_space_) {
          at = i;
          break;
        }
      }
      const size_t next = id == kCmdNextSpace
                              ? (at + 1) % spaces.size()
                              : (at + spaces.size() - 1) % spaces.size();
      const std::string space_id = spaces[next].first;
      if (c->HasSpace(space_id)) {
        c->ShowSpace(space_id);
      } else if (c->host_) {
        c->host_->RequestSwitchSpace(c->window_id_, space_id);
      }
      c->RefreshTabs();
      return true;
    }
    case kCmdBack:
      if (c->framework_) {
        c->framework_->GoBack(target);
        c->RefreshTabs();
      }
      return true;
    case kCmdForward:
      if (c->framework_) {
        c->framework_->GoForward(target);
        c->RefreshTabs();
      }
      return true;
    case kCmdBookmarkThisTab:
      if (c->host_) {
        c->host_->RequestBookmarkActiveTab(c->window_id_);
      }
      return true;
    case kCmdNextTab:
    case kCmdPreviousTab: {
      c->RebuildTabs();
      const auto& tabs = c->tabs_;
      if (tabs.size() < 2 || !c->framework_) {
        return true;
      }
      size_t at = 0;
      for (size_t i = 0; i < tabs.size(); ++i) {
        if (tabs[i].active) {
          at = i;
          break;
        }
      }
      const size_t next = id == kCmdNextTab
                              ? (at + 1) % tabs.size()
                              : (at + tabs.size() - 1) % tabs.size();
      c->framework_->ActivateTab(target, tabs[next].index);
      c->RefreshTabs();
      return true;
    }
    case kCmdMinimize:
      ::ShowWindow(c->shell_hwnd_, SW_MINIMIZE);
      return true;
    case kCmdZoomWindow:
      ::ShowWindow(c->shell_hwnd_,
                   ::IsZoomed(c->shell_hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
      return true;
    default:
      break;
  }
  // Disabled placeholders land here when invoked programmatically.
  return id >= kCmdAbout && id <= kCmdImportUserData;
}

std::string PhiMenuBar::ListJson() {
  RefreshPhi();
  RefreshFile();
  RefreshEdit();
  RefreshView();
  RefreshSpaces();
  RefreshHistory();
  RefreshBookmarks();
  RefreshTab();

  // Serialize what the user would see, straight from the HMENUs.
  struct Walker {
    static base::ListValue Items(HMENU menu, int depth) {
      base::ListValue items;
      const int count = ::GetMenuItemCount(menu);
      for (int i = 0; i < count && depth < 8; ++i) {
        MENUITEMINFOW info = {sizeof(info)};
        info.fMask = MIIM_ID | MIIM_STATE | MIIM_SUBMENU | MIIM_FTYPE;
        if (!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) {
          continue;
        }
        base::DictValue item;
        if (info.fType & MFT_SEPARATOR) {
          item.Set("separator", true);
        } else {
          item.Set("title",
                   base::WideToUTF8(PlainTitle(ItemTitle(menu, i))));
          item.Set("enabled", (info.fState & MFS_DISABLED) == 0);
          item.Set("checked", (info.fState & MFS_CHECKED) != 0);
          if (info.hSubMenu) {
            item.Set("items", Items(info.hSubMenu, depth + 1));
          } else {
            item.Set("id", static_cast<int>(info.wID));
          }
        }
        items.Append(std::move(item));
      }
      return items;
    }
  };

  base::ListValue menus;
  const int count = ::GetMenuItemCount(bar_);
  for (int i = 0; i < count; ++i) {
    MENUITEMINFOW info = {sizeof(info)};
    info.fMask = MIIM_SUBMENU;
    if (!::GetMenuItemInfoW(bar_, static_cast<UINT>(i), TRUE, &info) ||
        !info.hSubMenu) {
      continue;
    }
    base::DictValue entry;
    entry.Set("title", base::WideToUTF8(PlainTitle(ItemTitle(bar_, i))));
    entry.Set("items", Walker::Items(info.hSubMenu, 0));
    menus.Append(std::move(entry));
  }
  std::string json;
  base::JSONWriter::Write(menus, &json);
  return json;
}

bool PhiMenuBar::InvokePath(const std::vector<std::string>& path) {
  if (!bar_ || path.empty()) {
    return false;
  }
  HMENU menu = nullptr;
  const std::wstring want_top = base::UTF8ToWide(path[0]);
  const int tops = ::GetMenuItemCount(bar_);
  for (int i = 0; i < tops; ++i) {
    if (PlainTitle(ItemTitle(bar_, i)) == want_top) {
      MENUITEMINFOW info = {sizeof(info)};
      info.fMask = MIIM_SUBMENU;
      if (::GetMenuItemInfoW(bar_, static_cast<UINT>(i), TRUE, &info)) {
        menu = info.hSubMenu;
      }
      break;
    }
  }
  if (!menu) {
    return false;
  }
  OnMenuOpening(menu);  // What a click would have refreshed.
  for (size_t step = 1; step < path.size(); ++step) {
    const std::wstring want = base::UTF8ToWide(path[step]);
    const int count = ::GetMenuItemCount(menu);
    bool found = false;
    for (int i = 0; i < count; ++i) {
      if (PlainTitle(ItemTitle(menu, i)) != want) {
        continue;
      }
      MENUITEMINFOW info = {sizeof(info)};
      info.fMask = MIIM_ID | MIIM_STATE | MIIM_SUBMENU;
      if (!::GetMenuItemInfoW(menu, static_cast<UINT>(i), TRUE, &info)) {
        return false;
      }
      if (info.hSubMenu) {
        if (step + 1 >= path.size()) {
          return false;  // A submenu itself cannot be activated.
        }
        menu = info.hSubMenu;
        found = true;
        break;
      }
      if (step + 1 != path.size() || (info.fState & MFS_DISABLED)) {
        return false;
      }
      return OnCommand(info.wID);
    }
    if (!found) {
      return false;
    }
  }
  return false;
}

}  // namespace phi_client
