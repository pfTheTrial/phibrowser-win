// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_menu_bar.h"

#include "phibrowser-win/client/phi_bookmark_manager.h"

#include <commdlg.h>

#include <algorithm>
#include <optional>
#include <utility>

#pragma comment(lib, "comdlg32.lib")

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/app/chrome_command_ids.h"
#include "phibrowser-win/client/phi_glyph_bitmap.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_updater.h"
#include "phibrowser-win/client/phi_version.h"
#include "phibrowser-win/client/phi_search_tabs_panel.h"
#include "phibrowser-win/client/phi_space_theme_panel.h"
#include "phibrowser-win/client/phi_toast.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

// Static commands. Dynamic rows live in the ranges below; nothing overlaps.

// Dynamic ranges.
constexpr UINT kSpaceFirst = 2000;       // + index into space_ids_
constexpr UINT kSpaceLast = 2099;
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

// Menu items that route straight through the framework's generic Chromium
// command wire — features Chromium already implements; the hollowed chrome
// only lacked the wire. Enablement mirrors the live command state on every
// menu open, so these behave exactly like Chromium's own menu items.
struct IdcRoute {
  UINT cmd;
  int idc;
};
constexpr IdcRoute kIdcRoutes[] = {
    {kCmdDeleteBrowsingData, IDC_CLEAR_BROWSING_DATA},
    {kCmdNewIncognitoWindow, IDC_NEW_INCOGNITO_WINDOW},
    {kCmdReopenClosedTab, IDC_RESTORE_TAB},
    {kCmdOpenFile, IDC_OPEN_FILE},
    {kCmdSavePageAs, IDC_SAVE_PAGE},
    {kCmdPrint, IDC_PRINT},
    {kCmdFind, IDC_FIND},
    {kCmdFindNext, IDC_FIND_NEXT},
    {kCmdFindPrevious, IDC_FIND_PREVIOUS},
    // IDC_FULLSCREEN is NOT routed: BrowserView fullscreens the reparented
    // CHILD inside the shell (chrome vanishes, nothing truly fullscreens)
    // and the exit leaves the child mispositioned. Fullscreen needs the
    // SHELL to drive it (monitor-sized borderless shell + child relayout).
    {kCmdActualSize, IDC_ZOOM_NORMAL},
    {kCmdZoomIn, IDC_ZOOM_PLUS},
    {kCmdZoomOut, IDC_ZOOM_MINUS},
    {kCmdViewSource, IDC_VIEW_SOURCE},
    {kCmdDevTools, IDC_DEV_TOOLS},
    {kCmdInspect, IDC_DEV_TOOLS_INSPECT},
    {kCmdJsConsole, IDC_DEV_TOOLS_CONSOLE},
    {kCmdHome, IDC_HOME},
    {kCmdShowFullHistory, IDC_SHOW_HISTORY},
    // IDC_NEW_TAB_TO_RIGHT keys off FEATURE_TABSTRIP, which the hollowed
    // chrome reports false — kCmdNewTabToRight composes NewTab+MoveTab
    // instead (see OnCommand).
    {kCmdDuplicateTab, IDC_DUPLICATE_TAB},
    {kCmdMuteSite, IDC_WINDOW_MUTE_SITE},
    {kCmdTaskManager, IDC_TASK_MANAGER},
};

int IdcForCommand(UINT id) {
  for (const IdcRoute& route : kIdcRoutes) {
    if (route.cmd == id) {
      return route.idc;
    }
  }
  return 0;
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
  ReleaseSpaceBitmaps();
  if (overflow_) {
    // Detach first: its submenus are `bar_`'s, and DestroyMenu would take
    // them down with it, leaving `bar_` holding freed handles.
    while (::GetMenuItemCount(overflow_) > 0) {
      ::RemoveMenu(overflow_, 0, MF_BYPOSITION);
    }
    ::DestroyMenu(overflow_);
  }
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
  top(phi_, PhiStr("app.phi"));
  top(file_, PhiStr("menuBar.file"));
  top(edit_, PhiStr("menuBar.edit"));
  top(view_, PhiStr("menuBar.view"));
  top(spaces_, PhiStr("menuBar.spaces"));
  top(history_, PhiStr("menuBar.history"));
  top(bookmarks_, PhiStr("menuBar.bookmarks"));
  top(tab_, PhiStr("menuBar.tab"));
  top(window_, PhiStr("menuBar.window"));
  top(help_, PhiStr("menuBar.help"));

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

void PhiMenuBar::ShowOverflow(int screen_x, int screen_y) {
  if (!bar_ || !shell_) {
    return;
  }
  // Rebuilt each time: the bar's titles are fixed, but rebuilding is how the
  // popup picks up a submenu handle that a refresh replaced.
  if (overflow_) {
    while (::GetMenuItemCount(overflow_) > 0) {
      ::RemoveMenu(overflow_, 0, MF_BYPOSITION);
    }
  } else {
    overflow_ = ::CreatePopupMenu();
    if (!overflow_) {
      return;
    }
  }
  // The entries the comfortable layout has no band for. The memory chip is
  // not among them: it is a readout, with no command behind it. Chat is here
  // only while the AI suite is on (phi_features.h).
  if (kPhiAiEnabled) {
    ::AppendMenuW(overflow_, MF_STRING, kCmdToggleChatbar,
                  PhiStr("sidebar.chatButton.title"));
  }
  ::AppendMenuW(overflow_, MF_STRING, kCmdReportIssue, PhiStr("menuBar.sendFeedback"));
  ::AppendMenuW(overflow_, MF_SEPARATOR, 0, nullptr);
  const int count = ::GetMenuItemCount(bar_);
  for (int i = 0; i < count; ++i) {
    MENUITEMINFOW info = {sizeof(info)};
    info.fMask = MIIM_SUBMENU;
    if (!::GetMenuItemInfoW(bar_, static_cast<UINT>(i), TRUE, &info) ||
        !info.hSubMenu) {
      continue;
    }
    ::AppendMenuW(overflow_, MF_POPUP,
                  reinterpret_cast<UINT_PTR>(info.hSubMenu),
                  ItemTitle(bar_, i).c_str());
  }
  // Right-aligned: the button trails the toolbar, so a left-aligned popup
  // would run off the window.
  ::TrackPopupMenu(overflow_, TPM_RIGHTALIGN | TPM_TOPALIGN, screen_x,
                   screen_y, 0, shell_, nullptr);
}

void PhiMenuBar::BuildStatic() {
  // Phi — the Mac app menu, minus Services/Hide/Show All (Mac-only).
  Append(phi_, kCmdAbout, PhiStr("menuBar.aboutPhi"));
  Separator(phi_);
  Append(phi_, kCmdPreferences, PhiStr("menuBar.preferences"));
  // Live only when WinSparkle.dll is deployed beside the client.
  Append(phi_, kCmdCheckUpdate, PhiStr("app.phiMenu.checkForUpdatesMenuItem"),
         PhiUpdater::Available());
  Separator(phi_);
  Append(phi_, kCmdDeleteBrowsingData, PhiStr("menuBar.deleteBrowsingData"), false);
  Append(phi_, kCmdImportBrowser, PhiStr("app.phiMenu.importFromAnotherBrowser"));
  Separator(phi_);
  Append(phi_, kCmdQuit, PhiStr("menuBar.quitPhi"));

  // File. Save Page As has no shortcut — the Mac strips Chromium's.
  Append(file_, kCmdNewTab, PhiStr("browser.tabGroupOverview.newTabCardTitle"));
  Append(file_, kCmdNewWindow, PhiStr("settings.shortcuts.command.newWindow"));
  Append(file_, kCmdNewIncognitoWindow, PhiStr("settings.shortcuts.command.newIncognitoWindow"),
         false);
  Append(file_, kCmdNewIncognitoSpace, PhiStr("app.fileMenu.createNewIncognitoSpace"), false);
  Append(file_, kCmdReopenClosedTab, PhiStr("common.tabAreaContextMenu.reopenClosedTabAction"),
         false);
  Append(file_, kCmdOpenFile, PhiStr("menuBar.openFile"), false);
  Append(file_, kCmdOpenLocation, PhiStr("menuBar.openLocation"));
  Separator(file_);
  Append(file_, kCmdCloseWindow, PhiStr("settings.shortcuts.command.closeWindow"));
  Append(file_, kCmdCloseTab, PhiStr("browser.tabGroupOverview.splitCard.closePaneButtonAccessibilityLabel"));
  Append(file_, kCmdSavePageAs, PhiStr("menuBar.savePageAs"), false);
  Separator(file_);
  Append(file_, kCmdPrint, PhiStr("settings.shortcuts.command.printPage"), false);

  // Edit. The text commands drive the address field when it has focus; the
  // page's own editing belongs to Chromium and is not routed from here yet.
  Append(edit_, kCmdUndo, PhiStr("menuBar.undo"));
  Append(edit_, kCmdRedo, PhiStr("menuBar.redo"), false);
  Separator(edit_);
  Append(edit_, kCmdCut, PhiStr("menuBar.cut"));
  Append(edit_, kCmdCopy, PhiStr("menuBar.copy"));
  Append(edit_, kCmdCopyUrl, PhiStr("app.editMenu.copySelectedTabURLAction"));
  Append(edit_, kCmdPaste, PhiStr("menuBar.paste"));
  Append(edit_, kCmdPasteMatchStyle, PhiStr("menuBar.pasteAndMatchStyle"),
         false);
  Append(edit_, kCmdDelete, PhiStr("app.deleteProfileConfirmation.deleteButton"));
  Append(edit_, kCmdSelectAll, PhiStr("menuBar.selectAll"));
  Separator(edit_);
  HMENU find = ::CreatePopupMenu();
  Append(find, kCmdSearchWeb, PhiStr("settings.shortcuts.command.searchTheWeb"), false);
  Separator(find);
  Append(find, kCmdFind, PhiStr("menuBar.find"), false);
  Append(find, kCmdFindNext, PhiStr("settings.shortcuts.command.findNextMatch"), false);
  Append(find, kCmdFindPrevious, PhiStr("settings.shortcuts.command.findPreviousMatch"), false);
  ::AppendMenuW(edit_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(find), PhiStr("settings.shortcuts.command.findOnPage"));

  // View — Chromium's rows, then the Mac's Phi section (layout modes, bars).
  Append(view_, kCmdStop, PhiStr("browser.webContentHeader.stopButton.accessibilityLabel"));
  Append(view_, kCmdReload, PhiStr("menuBar.reloadThisPage"));
  Append(view_, kCmdForceReload, PhiStr("menuBar.forceReloadThisPage"));
  Separator(view_);
  Append(view_, kCmdFullScreen, PhiStr("menuBar.enterFullScreen"), false);
  Append(view_, kCmdActualSize, PhiStr("menuBar.actualSize"), false);
  Append(view_, kCmdZoomIn, PhiStr("menuBar.zoomIn"), false);
  Append(view_, kCmdZoomOut, PhiStr("menuBar.zoomOut"), false);
  Separator(view_);
  // Reader View — the Mirage extension's reading surface for the active tab.
  Append(view_, kCmdToggleReader, PhiStr("app.viewMenu.toggleReaderView"));
  Separator(view_);
  HMENU developer = ::CreatePopupMenu();
  Append(developer, kCmdViewSource, PhiStr("menuBar.viewSource"), false);
  Append(developer, kCmdDevTools, PhiStr("menuBar.developerTools"), false);
  Append(developer, kCmdInspect, PhiStr("menuBar.inspectElements"), false);
  Append(developer, kCmdJsConsole, PhiStr("menuBar.javascriptConsole"),
         false);
  ::AppendMenuW(view_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(developer), PhiStr("settings.general.developerMode.sectionTitle"));
  Separator(view_);
  Append(view_, kCmdLayoutHeader, PhiStr("app.viewMenu.layoutModeSectionTitle"), false);  // Section header.
  Append(view_, kCmdLayoutBalanced, PhiStr("oobe.layoutSelection.balancedOption"));
  Append(view_, kCmdLayoutPerformance, PhiStr("oobe.layoutSelection.performanceOption"));
  Append(view_, kCmdLayoutComfortable, PhiStr("oobe.layoutSelection.comfortableOption"));
  Separator(view_);
  Append(view_, kCmdAlwaysShowBookmarkBar,
         PhiStr("app.viewMenu.alwaysShowBookmarkBar"));
  Append(view_, kCmdBookmarkBarOnNewTab, PhiStr("app.viewMenu.showBookmarkBarOnNewTab"));
  Separator(view_);
  Append(view_, kCmdToggleSidebar, PhiStr("app.viewMenu.toggleSidebar"));
  // The chat pair belongs to the AI suite: greyed out on Mac because it is
  // signed-out, absent here because the suite is off.
  if (kPhiAiEnabled) {
    Append(view_, kCmdToggleChatbar, PhiStr("app.viewMenu.toggleChatbar"),
           false);
    Append(view_, kCmdNewConversation, PhiStr("app.viewMenu.newConversation"),
           false);
  }

  // Spaces / Bookmarks / Tab hold dynamic rows; they are filled on open.

  // History. Only Back/Forward have a surface so far.
  Append(history_, kCmdHome, PhiStr("settings.shortcuts.command.goHome"), false);
  Append(history_, kCmdBack, PhiStr("agentControl.connectionApproval.backButton"));
  Append(history_, kCmdForward, PhiStr("browser.webContentHeader.forwardButton.accessibilityLabel"));
  Separator(history_);
  Append(history_, kCmdRecentlyClosedHeader, PhiStr("searchTabs.section.recentlyClosed"), false);
  Separator(history_);
  Append(history_, kCmdShowFullHistory, PhiStr("menuBar.showFullHistory"), false);

  // Window.
  Append(window_, kCmdMinimize, PhiStr("menuBar.minimize"));
  Append(window_, kCmdZoomWindow, PhiStr("menuBar.zoom"));
  Separator(window_);
  Append(window_, kCmdNameWindow, PhiStr("menuBar.nameWindow"), false);
  Separator(window_);
  Append(window_, kCmdDownloads, PhiStr("browser.headerMoreMenu.downloadsAction"));
  Append(window_, kCmdExtensions, PhiStr("app.browserMigration.report.labelExtensions"), false);
  Append(window_, kCmdTaskManager, PhiStr("menuBar.taskManager"), false);

  // Help. Time Machine restore is a Mac app-bundle mechanism — skipped.
  Append(help_, kCmdReportIssue, PhiStr("menuBar.reportAnIssue"));
  Append(help_, kCmdWhatsNew, PhiStr("app.helpMenu.whatsNew"), false);
  Append(help_, kCmdDocumentation, PhiStr("menuBar.documentation"), false);
  Separator(help_);
  HMENU user_data = ::CreatePopupMenu();
  Append(user_data, kCmdExportUserData, PhiStr("app.helpMenu.exportUserData"), false);
  Append(user_data, kCmdImportUserData, PhiStr("app.helpMenu.importUserData"), false);
  ::AppendMenuW(help_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(user_data), PhiStr("app.helpMenu.manageUserData"));

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
  } else if (menu != window_ && menu != help_) {
    return;  // A submenu; its parent's open already refreshed it.
  }
  RefreshChromiumCommands();
}

void PhiMenuBar::RefreshChromiumCommands() {
  const int64_t target = controller_->active_browser_
                             ? controller_->active_browser_
                             : controller_->window_id_;
  for (const IdcRoute& route : kIdcRoutes) {
    // MF_BYCOMMAND searches submenus, so one call from the bar reaches items
    // inside Find / Developer.
    const bool on =
        controller_->framework_ &&
        controller_->framework_->IsChromiumCommandEnabled(target, route.idc);
    ::EnableMenuItem(
        bar_, route.cmd,
        MF_BYCOMMAND | (on ? MF_ENABLED : MF_DISABLED | MF_GRAYED));
  }
}

void PhiMenuBar::RefreshPhi() {
  SetEnabled(phi_, kCmdPreferences, controller_->host_ != nullptr);
  SetEnabled(phi_, kCmdImportBrowser,
             controller_->framework_ != nullptr &&
                 !controller_->is_incognito());
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
  // Reader View is offered where it has something to show, and ticked while
  // its surface is up — which is also the state that makes the item mean
  // "leave" rather than "enter".
  const bool reader_active = controller_->ActiveTabShowsReader();
  SetEnabled(view_, kCmdToggleReader,
             reader_active || controller_->ActiveTabCanUseReader());
  ::CheckMenuItem(view_, kCmdToggleReader,
                  MF_BYCOMMAND | (reader_active ? MF_CHECKED : MF_UNCHECKED));
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
  if (controller_->host_) {
    ::CheckMenuItem(view_, kCmdAlwaysShowBookmarkBar,
                    MF_BYCOMMAND |
                        (controller_->host_->AlwaysShowBookmarkBar()
                             ? MF_CHECKED
                             : MF_UNCHECKED));
    ::CheckMenuItem(view_, kCmdBookmarkBarOnNewTab,
                    MF_BYCOMMAND | (controller_->host_->BookmarkBarOnNewTab()
                                        ? MF_CHECKED
                                        : MF_UNCHECKED));
  }
}

std::vector<PhiMenuBar::SpaceEntry> PhiMenuBar::SpaceList() const {
  std::vector<SpaceEntry> spaces;
  if (!controller_->host_) {
    return spaces;
  }
  for (const PhiSpaceView& space : controller_->host_->Spaces()) {
    spaces.push_back(
        {space.id,
         base::UTF8ToWide(!space.name.empty() ? space.name
                                              : "Space " + space.id),
         base::UTF8ToWide(space.icon_text)});
  }
  return spaces;
}

void PhiMenuBar::ReleaseSpaceBitmaps() {
  for (HBITMAP bitmap : space_bitmaps_) {
    ::DeleteObject(bitmap);
  }
  space_bitmaps_.clear();
}

void PhiMenuBar::RefreshSpaces() {
  ClearMenu(spaces_);
  ReleaseSpaceBitmaps();  // The rows that held them are gone.
  space_ids_.clear();
  profile_ids_.clear();
  profile_names_.clear();

  Append(spaces_, kCmdNewSpace, PhiStr("app.activeSpaceContextMenu.newSpaceAction"),
         controller_->host_ != nullptr);
  Append(spaces_, kCmdRenameSpace, PhiStr("app.activeSpaceContextMenu.renameSpaceAction"),
         controller_->host_ != nullptr);
  // Lands on Settings > Spaces with the active Space selected — the
  // detail column there is the icon picker.
  Append(spaces_, kCmdChangeIcon, PhiStr("menuBar.changeIcon"),
         controller_->host_ != nullptr);

  // Edit Theme — the Mac's item of the same name: it presents the Space
  // theme editor (swatch row + saturation slider) on the ACTIVE Space
  // rather than listing theme names, so color and saturation are one
  // control and a Space always wears a concrete theme.
  const std::string active = controller_->active_space_;
  Append(spaces_, kCmdEditSpaceTheme,
         PhiStr("app.activeSpaceContextMenu.themeSubmenuTitle"),
         controller_->host_ != nullptr && !active.empty());

  // Change Profile — the Mac's radio list: picking a row re-binds the ACTIVE
  // Space, respawning its window in the new profile. The default Space's
  // binding is fixed, so its rows stay disabled and only read the binding
  // out. Delete Profile rows are guarded the Mac's way: Default and any
  // profile still bound to a Space refuse.
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
  const bool can_reprofile = controller_->host_ && !active.empty() &&
                             active != controller_->host_->DefaultSpaceId();
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
                      MF_STRING |
                          (can_reprofile ? 0u : MF_DISABLED | MF_GRAYED) |
                          (*id == active_profile ? MF_CHECKED : 0),
                      kProfileFirst + static_cast<UINT>(profile_count),
                      title.c_str());
        profile_names_.push_back(title);
        const bool in_use =
            std::find(bound_profiles.begin(), bound_profiles.end(), *id) !=
            bound_profiles.end();
        const bool deletable = *id != "Default" && !in_use;
        // The Mac marks in-use rows "<name> — in use by a Space".
        const std::wstring delete_title =
            in_use ? PhiStrF("menuBar.profileInUse", {title}) : title;
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
                  PhiStr("menuBar.noProfiles"));
    ::AppendMenuW(delete_profiles, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                  PhiStr("app.spacesMenu.deleteProfile.emptyPlaceholder"));
  }
  ::AppendMenuW(spaces_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(profiles), PhiStr("app.activeSpaceContextMenu.profileSubmenuTitle"));
  Append(spaces_, kCmdDeleteSpace, PhiStr("app.activeSpaceContextMenu.deleteSpaceAction"),
         controller_->host_ && active != "1");
  Separator(spaces_);
  const std::vector<SpaceEntry> spaces = SpaceList();
  const bool several = spaces.size() > 1;
  Append(spaces_, kCmdNextSpace, PhiStr("app.spacesMenu.nextSpace"), several);
  Append(spaces_, kCmdPreviousSpace, PhiStr("app.spacesMenu.previousSpace"),
         several);
  Separator(spaces_);
  for (size_t i = 0; i < spaces.size() && kSpaceFirst + i <= kSpaceLast;
       ++i) {
    std::wstring title = spaces[i].title;
    if (i < 9) {
      title += PhiStr("menuBar.text") + std::to_wstring(i + 1);
    }
    // The active Space is the menu's DEFAULT item too: a row with a bitmap
    // shows no check mark, and the default's bold text is what the menu
    // can still render to say "you are here".
    ::AppendMenuW(spaces_,
                  MF_STRING | (spaces[i].id == active
                                   ? MF_CHECKED | MF_DEFAULT
                                   : 0),
                  kSpaceFirst + static_cast<UINT>(i), title.c_str());
    space_ids_.push_back(spaces[i].id);
    // The Space's icon as the row's picture: catalog artwork or colour
    // emoji, which menu TEXT could show as neither.
    if (HBITMAP bitmap = PhiSpaceGlyphBitmap(
            spaces[i].glyph, controller_->Scaled(16),
            ::GetSysColor(COLOR_MENUTEXT))) {
      space_bitmaps_.push_back(bitmap);
      PhiSetMenuItemBitmap(spaces_, kSpaceFirst + static_cast<UINT>(i),
                           bitmap);
    }
  }
  Separator(spaces_);
  Append(spaces_, kCmdUrlRules, PhiStr("menuBar.urlRules"), false);
  Separator(spaces_);
  Append(spaces_, kCmdNewProfile, PhiStr("app.spacesMenu.newProfile"),
         controller_->framework_ != nullptr);
  ::AppendMenuW(spaces_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(delete_profiles),
                PhiStr("app.spacesMenu.deleteProfile.submenuTitle"));
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
        ::AppendMenuW(sub, MF_STRING | MF_DISABLED | MF_GRAYED, 0, PhiStr("app.bookmarksMenu.emptyFolderPlaceholder"));
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
  Append(bookmarks_, kCmdBookmarkThisTab, PhiStr("app.bookmarksMenu.addOrEditCurrentTab"),
         controller_->host_ && !controller_->nav_url_.empty() &&
             !controller_->is_incognito());
  Append(bookmarks_, kCmdBookmarkAllTabs,
         PhiStr("app.bookmarksMenu.addAllOpenTabs"),
         controller_->host_ && !controller_->tabs_.empty() &&
             !controller_->is_incognito());
  Separator(bookmarks_);
  Append(bookmarks_, kCmdExportBookmarks, PhiStr("app.bookmarksMenu.exportCurrentSpace"),
         controller_->host_ != nullptr);
  Separator(bookmarks_);
  // The Mac's Bookmarks-menu entry for the native manager; Chromium's own
  // shortcut for IDC_SHOW_BOOKMARK_MANAGER on Windows is Ctrl+Shift+O.
  Append(bookmarks_, kCmdBookmarkManager, PhiStr("menuBar.bookmarkManager"),
         controller_->framework_ != nullptr &&
             !controller_->is_incognito());
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
  Append(tab_, kCmdNewTabToRight, PhiStr("menuBar.newTabToTheRight"),
         controller_->framework_ != nullptr);
  Append(tab_, kCmdNextTab, PhiStr("settings.shortcuts.command.selectNextTab"), several);
  Append(tab_, kCmdPreviousTab, PhiStr("settings.shortcuts.command.selectPreviousTab"),
         several);
  Append(tab_, kCmdDuplicateTab, PhiStr("settings.shortcuts.command.duplicateTab"), false);
  Append(tab_, kCmdMuteSite, PhiStr("settings.shortcuts.command.muteSite"), false);
  Append(tab_, kCmdSearchTabs, PhiStr("menuBar.searchTabs"));
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
                  row.title.empty() ? PhiStr("common.bookmarkFolder.defaultName") : row.title.c_str());
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
    const std::vector<SpaceEntry> spaces = SpaceList();
    const size_t i = id - kCmdSpace1;
    if (i < spaces.size() && spaces[i].id != c->active_space_) {
      const std::string space_id = spaces[i].id;
      if (c->HasSpace(space_id)) {
        c->ShowSpace(space_id);
      } else if (c->host_) {
        c->host_->RequestSwitchSpace(c->window_id_, space_id);
      }
      c->RefreshTabs();
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
    // Re-binds the active Space. Confirmed like the Mac's: the Space's
    // window is replaced and its tabs reload in the new profile, where the
    // old profile's logins do not exist.
    const size_t i = id - kProfileFirst;
    if (c->host_ && i < profile_ids_.size() &&
        PhiConfirm(c->shell_hwnd_,
                   PhiStrF("app.changeSpaceProfileConfirmation.title",
                           {i < profile_names_.size() ? profile_names_[i]
                                                      : std::wstring()})
                       .c_str(),
                   PhiStr("app.changeSpaceProfileConfirmation."
                          "profileScopedMessage"),
                   nullptr, /*destructive=*/false)) {
      c->host_->SetSpaceProfile(c->active_space_, profile_ids_[i]);
    }
    return true;
  }
  if (id >= kDeleteProfileFirst && id <= kDeleteProfileLast) {
    const size_t i = id - kDeleteProfileFirst;
    if (c->host_ && i < profile_ids_.size() &&
        PhiConfirm(c->shell_hwnd_,
                   PhiStr("app.spacesMenu.deleteProfile.submenuTitle"),
                   PhiStr("menuBar.deleteProfileConfirmBody"), L"Delete")) {
      c->host_->DeleteProfileById(profile_ids_[i]);
    }
    return true;
  }

  if (const int idc = IdcForCommand(id)) {
    if (c->framework_) {
      c->framework_->ExecuteChromiumCommand(target, idc);
      c->RefreshTabs();
    }
    return true;
  }

  switch (id) {
    case kCmdAbout:
      PhiNotice(c->shell_hwnd_, PhiStr("menuBar.aboutPhi"),
                PhiStrF("menuBar.aboutBody", {PHI_WIN_VERSION_WIDE}).c_str());
      return true;
    case kCmdReportIssue:
      c->OpenFeedback();
      return true;
    case kCmdCheckUpdate:
      if (!PhiUpdater::CheckForUpdateWithUi()) {
        PhiNotice(c->shell_hwnd_, PhiStr("menuBar.checkForUpdate"),
                  PhiStr("menuBar.updaterUnavailableBody"));
      }
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
      c->StartNewTab();  // Honors the "New tab behavior" setting.
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
      // The Mac's copy confirmation toast (OverlayToastCenter).
      PhiShowToast(c->shell_hwnd_, c->CurrentPalette(), PhiStr("app.copyTabURLsToast.single"));
      return true;
    }
    case kCmdStop:
      if (c->framework_) {
        c->framework_->StopLoading(target);
        c->RefreshTabs();
      }
      return true;
    case kCmdAlwaysShowBookmarkBar:
      if (c->host_) {
        c->host_->SetAlwaysShowBookmarkBar(!c->host_->AlwaysShowBookmarkBar());
      }
      return true;
    case kCmdBookmarkBarOnNewTab:
      if (c->host_) {
        c->host_->SetBookmarkBarOnNewTab(!c->host_->BookmarkBarOnNewTab());
      }
      return true;
    case kCmdToggleSidebar:
      c->ToggleSidebar();
      return true;
    case kCmdToggleReader:
      c->ToggleReaderView();
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
      c->ShowCreateSpaceForm();  // The sidebar form, or at once without one.
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
      if (PhiPromptText(c->shell_hwnd_, PhiStr("app.renameSpaceDialog.title"),
                        PhiStr("app.renameSpaceDialog.message"), &name)) {
        c->host_->RenameSpace(c->active_space_, base::WideToUTF8(name));
      }
      return true;
    }
    case kCmdChangeIcon:
      if (c->host_) {
        c->host_->RequestSpaceIconPicker(c->active_space_);
      }
      return true;
    case kCmdEditSpaceTheme: {
      // The Mac's .windowTop placement: the menu bar has no trigger point
      // to hang the panel off, so it drops under the caption, centred on
      // the window (the panel centres itself in an anchor wider than it).
      if (!c->host_ || c->active_space_.empty() || !c->shell_hwnd_) {
        return true;
      }
      RECT shell = {};
      ::GetWindowRect(c->shell_hwnd_, &shell);
      const RECT anchor = {shell.left, shell.top, shell.right, shell.top};
      PhiShowSpaceThemePanel(c, anchor, c->active_space_);
      return true;
    }
    case kCmdDeleteSpace:
      if (c->host_ &&
          PhiConfirm(c->shell_hwnd_, PhiStr("menuBar.deleteSpace"),
                     PhiStr("menuBar.deleteSpaceConfirmBody"), L"Delete")) {
        c->host_->DeleteSpace(c->active_space_);
      }
      return true;
    case kCmdNewProfile: {
      std::wstring name;
      if (c->framework_ &&
          PhiPromptText(c->shell_hwnd_,
                        PhiStr("settings.profiles.createDialog.title"),
                        PhiStr("menuBar.newProfilePromptBody"), &name)) {
        c->framework_->CreateProfile(base::WideToUTF8(name).c_str());
      }
      return true;
    }
    case kCmdNextSpace:
    case kCmdPreviousSpace: {
      const std::vector<SpaceEntry> spaces = SpaceList();
      if (spaces.size() < 2) {
        return true;
      }
      size_t at = 0;
      for (size_t i = 0; i < spaces.size(); ++i) {
        if (spaces[i].id == c->active_space_) {
          at = i;
          break;
        }
      }
      const size_t next = id == kCmdNextSpace
                              ? (at + 1) % spaces.size()
                              : (at + spaces.size() - 1) % spaces.size();
      const std::string space_id = spaces[next].id;
      if (c->HasSpace(space_id)) {
        c->ShowSpace(space_id);
      } else if (c->host_) {
        c->host_->RequestSwitchSpace(c->window_id_, space_id);
      }
      c->RefreshTabs();
      return true;
    }
    case kCmdBack:
      // Back over a tab the reader is covering means "leave the reader" —
      // the same rule the toolbar's back button follows.
      if (c->LeaveReaderViewForNavigation()) {
        return true;
      }
      if (c->framework_) {
        c->framework_->GoBack(target);
        c->RefreshTabs();
      }
      return true;
    case kCmdForward:
      if (c->LeaveReaderViewForNavigation()) {
        return true;
      }
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
    case kCmdBookmarkAllTabs: {
      if (!c->host_ || c->is_incognito()) {
        return true;
      }
      std::wstring name = PhiStr("menuBar.savedTabs");
      if (!PhiPromptText(
              c->shell_hwnd_,
              PhiStr("common.tabAreaContextMenu.bookmarkAllTabsAction"),
              PhiStr("menuBar.bookmarkAllTabsPromptBody"), &name) ||
          name.empty()) {
        return true;
      }
      const std::string folder = c->host_->AddBookmarkFolder(
          c->window_id_, std::string(), base::WideToUTF8(name));
      if (folder.empty() || !c->framework_) {
        return true;
      }
      int added = 0;
      std::optional<base::Value> tabs = base::JSONReader::Read(
          c->framework_->ListTabsJson(target), base::JSON_PARSE_RFC);
      if (tabs && tabs->is_list()) {
        for (const base::Value& v : tabs->GetList()) {
          const base::DictValue* d = v.GetIfDict();
          if (!d) {
            continue;
          }
          const std::string* url = d->FindString("url");
          const std::string* title = d->FindString("title");
          if (url && !url->empty() &&
              !c->host_
                   ->AddBookmarkRow(c->window_id_, folder,
                                    title ? *title : std::string(), *url)
                   .empty()) {
            ++added;
          }
        }
      }
      PhiShowToast(
          c->shell_hwnd_, c->CurrentPalette(),
          PhiStrF("menuBar.tabsBookmarkedToast", {std::to_wstring(added)}));
      return true;
    }
    case kCmdBookmarkManager:
      // URL-driven, as on Mac: the manager IS a tab on the bookmark route,
      // and the client mounts its native page over that tab's content.
      if (c->framework_) {
        c->framework_->NewTab(c->active_browser_ ? c->active_browser_
                                                 : c->window_id_,
                              PhiBookmarkManager::ManagerUrl());
        c->RefreshTabs();
      }
      return true;
    case kCmdExportBookmarks: {
      if (!c->host_) {
        return true;
      }
      wchar_t path[MAX_PATH] = L"bookmarks.html";
      OPENFILENAMEW ofn = {sizeof(ofn)};
      ofn.hwndOwner = c->shell_hwnd_;
      ofn.lpstrFilter = L"HTML File\0*.html\0\0";
      ofn.lpstrFile = path;
      ofn.nMaxFile = MAX_PATH;
      ofn.Flags = OFN_OVERWRITEPROMPT;
      ofn.lpstrDefExt = L"html";
      if (!::GetSaveFileNameW(&ofn)) {
        return true;
      }
      const std::string html = BuildBookmarksHtml();
      HANDLE f = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
      if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        ::WriteFile(f, html.data(), static_cast<DWORD>(html.size()), &wrote,
                    nullptr);
        ::CloseHandle(f);
        PhiShowToast(c->shell_hwnd_, c->CurrentPalette(),
                     PhiStr("menuBar.bookmarksExported"));
      }
      return true;
    }
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
    case kCmdImportBrowser:
      if (c->framework_ &&
          PhiConfirm(c->shell_hwnd_, PhiStr("menuBar.importFromGoogleChrome"),
                     PhiStr("menuBar.importChromePromptBody"),
                     PhiStr("app.import"), /*destructive=*/false)) {
        if (!c->framework_->StartChromeImport(target)) {
          PhiNotice(c->shell_hwnd_, PhiStr("app.import"),
                    PhiStr("menuBar.importChromeFailedBody"));
        }
      }
      return true;
    case kCmdNewTabToRight: {
      // Composed from the primitives: IDC_NEW_TAB_TO_RIGHT stays disabled in
      // the hollowed chrome (it keys off FEATURE_TABSTRIP).
      if (!c->framework_) {
        return true;
      }
      c->RebuildTabs();
      int active_index = -1;
      for (const auto& row : c->tabs_) {
        if (row.active) {
          active_index = row.index;
          break;
        }
      }
      const int created = c->framework_->NewTab(target, "");
      if (created >= 0 && active_index >= 0 && created != active_index + 1) {
        c->framework_->MoveTab(target, created, active_index + 1);
      }
      c->RefreshTabs();
      return true;
    }
    case kCmdSearchTabs:
      PhiSearchTabsPanel::Toggle(c);
      return true;
    case kCmdDownloads:
      c->ToggleDownloadsPopover();
      return true;
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

std::string PhiMenuBar::BuildBookmarksHtml() const {
  // The Mac BookmarkHTMLExporter's shape: standard Netscape file, pinned
  // rows first as one "Favorites" folder, then the bookmark tree.
  const auto escaped = [](const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
      switch (c) {
        case '&':
          out += "&amp;";
          break;
        case '<':
          out += "&lt;";
          break;
        case '>':
          out += "&gt;";
          break;
        case '"':
          out += "&quot;";
          break;
        default:
          out.push_back(c);
      }
    }
    return out;
  };
  std::string html =
      "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
      "<!-- This is an automatically generated file. -->\n"
      "<META HTTP-EQUIV=\"Content-Type\" "
      "CONTENT=\"text/html; charset=UTF-8\">\n"
      "<TITLE>Bookmarks</TITLE>\n"
      "<H1>Bookmarks</H1>\n"
      "<DL><p>\n";
  if (controller_->host_) {
    const std::vector<PhiStoredRowView> pins =
        controller_->host_->PinnedRows(controller_->window_id_);
    if (!pins.empty()) {
      html += "    <DT><H3>Favorites</H3>\n    <DL><p>\n";
      for (const PhiStoredRowView& pin : pins) {
        if (pin.url.empty()) {
          continue;
        }
        html += "        <DT><A HREF=\"" + escaped(pin.url) + "\">" +
                escaped(!pin.title.empty() ? pin.title : pin.url) +
                "</A>\n";
      }
      html += "    </DL><p>\n";
    }
    const auto descend = [&](const auto& self, const std::string& parent,
                             const std::string& indent) -> void {
      for (const PhiStoredRowView& row :
           controller_->host_->BookmarkRows(controller_->window_id_,
                                            parent)) {
        if (row.folder) {
          html += indent + "<DT><H3>" + escaped(row.title) + "</H3>\n" +
                  indent + "<DL><p>\n";
          self(self, row.guid, indent + "    ");
          html += indent + "</DL><p>\n";
        } else if (!row.url.empty()) {
          html += indent + "<DT><A HREF=\"" + escaped(row.url) + "\">" +
                  escaped(!row.title.empty() ? row.title : row.url) +
                  "</A>\n";
        }
      }
    };
    descend(descend, std::string(), "    ");
  }
  html += "</DL><p>\n";
  return html;
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
  RefreshChromiumCommands();

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
