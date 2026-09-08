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

#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/app/chrome_command_ids.h"
#include "phibrowser-win/client/phi_glyph_bitmap.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_updater.h"
#include "phibrowser-win/client/phi_version.h"
#include "phibrowser-win/client/phi_toast.h"
#include "phibrowser-win/client/phi_theme.h"
// PhiSlotHost and PhiWindowModel still live in the shell's header; §4.3
// splits them out. The bar reaches nothing else in it — no `friend`, no
// private member: everything else comes through PhiMenuBarShell.
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

PhiMenuBar::PhiMenuBar(PhiMenuBarShell* shell)
    : services_(shell) {}

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
  // arrive as WM_COMMAND on the shell. The scope makes the coordinator defer
  // pipe verbs for as long as the menu loop runs (phi_modal_scope.h).
  PhiModalScope modal;
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
  PhiModalScope modal;
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
  const BrowserId target = services_->MenuActiveBrowser();
  for (const IdcRoute& route : kIdcRoutes) {
    // MF_BYCOMMAND searches submenus, so one call from the bar reaches items
    // inside Find / Developer. No browser attached: everything is off.
    const bool on = services_->MenuFramework() && target.valid() &&
                    services_->MenuFramework()->IsChromiumCommandEnabled(
                        target.value, route.idc);
    ::EnableMenuItem(
        bar_, route.cmd,
        MF_BYCOMMAND | (on ? MF_ENABLED : MF_DISABLED | MF_GRAYED));
  }
}

void PhiMenuBar::RefreshPhi() {
  SetEnabled(phi_, kCmdPreferences, services_->MenuHost() != nullptr);
  SetEnabled(phi_, kCmdImportBrowser,
             services_->MenuFramework() != nullptr &&
                 !services_->MenuIncognito());
}

void PhiMenuBar::RefreshFile() {
  // The rows as they are: the menu reads what the last rebuild placed
  // (docs/architecture-review.md §3.1 principle 7), it never rebuilds.
  SetEnabled(file_, kCmdCloseTab, services_->MenuActiveTab().valid());
  SetEnabled(file_, kCmdOpenLocation, services_->MenuAddressField() != nullptr);
}

void PhiMenuBar::RefreshEdit() {
  // The text commands act on the address field; they sleep while the page has
  // the keyboard (Chromium runs its own edit commands there).
  const bool field = services_->MenuAddressField() &&
                     ::GetFocus() == services_->MenuAddressField();
  SetEnabled(edit_, kCmdUndo,
             field && ::SendMessageW(services_->MenuAddressField(), EM_CANUNDO, 0,
                                     0) != 0);
  SetEnabled(edit_, kCmdCut, field);
  SetEnabled(edit_, kCmdCopy, field);
  SetEnabled(edit_, kCmdPaste, field);
  SetEnabled(edit_, kCmdDelete, field);
  SetEnabled(edit_, kCmdSelectAll, field);
  SetEnabled(edit_, kCmdCopyUrl, !services_->MenuNavState().url.empty());
}

void PhiMenuBar::RefreshView() {
  services_->MenuRefreshNavState();
  SetEnabled(view_, kCmdStop, services_->MenuNavState().loading);
  // Reader View is offered where it has something to show, and ticked while
  // its surface is up — which is also the state that makes the item mean
  // "leave" rather than "enter".
  const bool reader_active = services_->MenuReaderActive();
  SetEnabled(view_, kCmdToggleReader,
             reader_active || services_->MenuReaderOfferable());
  ::CheckMenuItem(view_, kCmdToggleReader,
                  MF_BYCOMMAND | (reader_active ? MF_CHECKED : MF_UNCHECKED));
  PhiLayoutMode mode = PhiLayoutMode::kPerformance;
  if (services_->MenuHost()) {
    mode = services_->MenuHost()->LayoutMode();
  }
  const UINT chosen = mode == PhiLayoutMode::kBalanced ? kCmdLayoutBalanced
                      : mode == PhiLayoutMode::kComfortable
                          ? kCmdLayoutComfortable
                          : kCmdLayoutPerformance;
  ::CheckMenuRadioItem(view_, kCmdLayoutBalanced, kCmdLayoutComfortable,
                       chosen, MF_BYCOMMAND);
  if (services_->MenuHost()) {
    ::CheckMenuItem(view_, kCmdAlwaysShowBookmarkBar,
                    MF_BYCOMMAND |
                        (services_->MenuHost()->AlwaysShowBookmarkBar()
                             ? MF_CHECKED
                             : MF_UNCHECKED));
    ::CheckMenuItem(view_, kCmdBookmarkBarOnNewTab,
                    MF_BYCOMMAND | (services_->MenuHost()->BookmarkBarOnNewTab()
                                        ? MF_CHECKED
                                        : MF_UNCHECKED));
  }
}

std::vector<PhiMenuBar::SpaceEntry> PhiMenuBar::SpaceList() const {
  std::vector<SpaceEntry> spaces;
  if (!services_->MenuHost()) {
    return spaces;
  }
  for (const PhiSpaceView& space : services_->MenuHost()->Spaces()) {
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
         services_->MenuHost() != nullptr);
  Append(spaces_, kCmdRenameSpace, PhiStr("app.activeSpaceContextMenu.renameSpaceAction"),
         services_->MenuHost() != nullptr);
  // Lands on Settings > Spaces with the active Space selected — the
  // detail column there is the icon picker.
  Append(spaces_, kCmdChangeIcon, PhiStr("menuBar.changeIcon"),
         services_->MenuHost() != nullptr);

  // Edit Theme — the Mac's item of the same name: it presents the Space
  // theme editor (swatch row + saturation slider) on the ACTIVE Space
  // rather than listing theme names, so color and saturation are one
  // control and a Space always wears a concrete theme.
  const std::string active = services_->MenuActiveSpace();
  Append(spaces_, kCmdEditSpaceTheme,
         PhiStr("app.activeSpaceContextMenu.themeSubmenuTitle"),
         services_->MenuHost() != nullptr && !active.empty());

  // Change Profile — the Mac's radio list: picking a row re-binds the ACTIVE
  // Space, respawning its window in the new profile. The default Space's
  // binding is fixed, so its rows stay disabled and only read the binding
  // out. Delete Profile rows are guarded the Mac's way: Default and any
  // profile still bound to a Space refuse.
  std::string active_profile;
  std::vector<std::string> bound_profiles;
  if (services_->MenuHost()) {
    for (const PhiSpaceView& space : services_->MenuHost()->Spaces()) {
      bound_profiles.push_back(space.profile_id);
      if (space.id == active) {
        active_profile = space.profile_id;
      }
    }
  }
  HMENU profiles = ::CreatePopupMenu();
  HMENU delete_profiles = ::CreatePopupMenu();
  const bool can_reprofile = services_->MenuHost() && !active.empty() &&
                             active != services_->MenuHost()->DefaultSpaceId();
  int profile_count = 0;
  if (services_->MenuHost()) {
    // The host's "id\tname" summaries — the one profile list every menu
    // and form reads, so none of them parses the framework's JSON.
    for (const std::string& summary : services_->MenuHost()->ProfileSummaries()) {
      const size_t tab = summary.find('\t');
      const std::string id = summary.substr(0, tab);
      const std::string name =
          tab == std::string::npos ? std::string() : summary.substr(tab + 1);
      if (id.empty() ||
          profile_count >= static_cast<int>(kProfileLast - kProfileFirst)) {
        continue;
      }
      const std::wstring title = base::UTF8ToWide(!name.empty() ? name : id);
      ::AppendMenuW(profiles,
                    MF_STRING |
                        (can_reprofile ? 0u : MF_DISABLED | MF_GRAYED) |
                        (id == active_profile ? MF_CHECKED : 0),
                    kProfileFirst + static_cast<UINT>(profile_count),
                    title.c_str());
      profile_names_.push_back(title);
      const bool in_use =
          std::find(bound_profiles.begin(), bound_profiles.end(), id) !=
          bound_profiles.end();
      const bool deletable = id != "Default" && !in_use;
      // The Mac marks in-use rows "<name> — in use by a Space".
      const std::wstring delete_title =
          in_use ? PhiStrF("menuBar.profileInUse", {title}) : title;
      ::AppendMenuW(delete_profiles,
                    MF_STRING | (deletable ? 0u : MF_DISABLED | MF_GRAYED),
                    kDeleteProfileFirst + static_cast<UINT>(profile_count),
                    delete_title.c_str());
      profile_ids_.push_back(id);
      ++profile_count;
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
         services_->MenuHost() && active != "1");
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
            spaces[i].glyph, services_->MenuScaled(16),
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
         services_->MenuFramework() != nullptr);
  ::AppendMenuW(spaces_, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(delete_profiles),
                PhiStr("app.spacesMenu.deleteProfile.submenuTitle"));
}

void PhiMenuBar::RefreshHistory() {
  services_->MenuRefreshNavState();
  const PhiMenuBarNavState nav = services_->MenuNavState();
  SetEnabled(history_, kCmdBack, nav.can_go_back);
  SetEnabled(history_, kCmdForward, nav.can_go_forward);
}

void PhiMenuBar::AppendBookmarkRows(HMENU menu, const std::string& parent_guid,
                                    int depth) {
  if (!services_->MenuHost() || depth > 5) {
    return;
  }
  for (const PhiStoredRowView& row :
       services_->MenuHost()->BookmarkRows(services_->MenuSlot().value,
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
  services_->MenuRefreshNavState();
  // The rows as the last rebuild placed them; a menu opening never asks for
  // a fresh one (docs/architecture-review.md §3.1 principle 3).
  const bool has_rows = !services_->MenuTabRows().empty();
  Append(bookmarks_, kCmdBookmarkThisTab, PhiStr("app.bookmarksMenu.addOrEditCurrentTab"),
         services_->MenuHost() && !services_->MenuNavState().url.empty() &&
             !services_->MenuIncognito());
  Append(bookmarks_, kCmdBookmarkAllTabs,
         PhiStr("app.bookmarksMenu.addAllOpenTabs"),
         services_->MenuHost() && has_rows && !services_->MenuIncognito());
  Separator(bookmarks_);
  Append(bookmarks_, kCmdExportBookmarks, PhiStr("app.bookmarksMenu.exportCurrentSpace"),
         services_->MenuHost() != nullptr);
  Separator(bookmarks_);
  // The Mac's Bookmarks-menu entry for the native manager; Chromium's own
  // shortcut for IDC_SHOW_BOOKMARK_MANAGER on Windows is Ctrl+Shift+O.
  Append(bookmarks_, kCmdBookmarkManager, PhiStr("menuBar.bookmarkManager"),
         services_->MenuFramework() != nullptr &&
             !services_->MenuIncognito());
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
  tab_ids_.clear();
  // The rows as the last rebuild placed them — never rebuilt from a menu.
  const std::vector<PhiMenuBarTab> rows = services_->MenuTabRows();
  const bool several = rows.size() > 1;
  Append(tab_, kCmdNewTabToRight, PhiStr("menuBar.newTabToTheRight"),
         services_->MenuFramework() != nullptr);
  Append(tab_, kCmdNextTab, PhiStr("settings.shortcuts.command.selectNextTab"), several);
  Append(tab_, kCmdPreviousTab, PhiStr("settings.shortcuts.command.selectPreviousTab"),
         several);
  Append(tab_, kCmdDuplicateTab, PhiStr("settings.shortcuts.command.duplicateTab"), false);
  Append(tab_, kCmdMuteSite, PhiStr("settings.shortcuts.command.muteSite"), false);
  Append(tab_, kCmdSearchTabs, PhiStr("menuBar.searchTabs"));
  if (rows.empty()) {
    return;
  }
  Separator(tab_);
  for (size_t i = 0; i < rows.size() && kTabFirst + i <= kTabLast; ++i) {
    const PhiMenuBarTab& row = rows[i];
    ::AppendMenuW(tab_, MF_STRING | (row.active ? MF_CHECKED : 0),
                  kTabFirst + static_cast<UINT>(i),
                  row.title.empty() ? PhiStr("common.bookmarkFolder.defaultName") : row.title.c_str());
    tab_ids_.push_back(row.id);
  }
}

bool PhiMenuBar::OnCommand(UINT id) {
  PhiMenuBarShell* c = services_;
  // The browser the slot SHOWS — invalid when none is attached, and NEVER
  // the slot's own number. Every framework branch below tests it first.
  // Nothing here refreshes by hand after a framework mutation: the model's
  // push schedules the rebuild (docs/architecture-review.md §3.1
  // principle 3).
  const BrowserId target = c->MenuActiveBrowser();

  // Dynamic ranges first.
  if (id >= kSpaceFirst && id <= kSpaceLast) {
    const size_t i = id - kSpaceFirst;
    if (i < space_ids_.size()) {
      const std::string space_id = space_ids_[i];
      if (c->MenuHasSpace(space_id)) {
        c->MenuShowSpace(space_id);
      } else if (c->MenuHost()) {
        c->MenuHost()->RequestSwitchSpace(c->MenuSlot().value, space_id);
      }
    }
    return true;
  }
  if (id >= kCmdSpace1 && id <= kCmdSpace9) {
    // Ctrl+N from the keyboard hook: resolved against the LIVE Space list
    // (space_ids_ only refreshes when the Spaces menu opens).
    const std::vector<SpaceEntry> spaces = SpaceList();
    const size_t i = id - kCmdSpace1;
    if (i < spaces.size() && spaces[i].id != c->MenuActiveSpace()) {
      const std::string space_id = spaces[i].id;
      if (c->MenuHasSpace(space_id)) {
        c->MenuShowSpace(space_id);
      } else if (c->MenuHost()) {
        c->MenuHost()->RequestSwitchSpace(c->MenuSlot().value, space_id);
      }
    }
    return true;
  }
  if (id >= kTabFirst && id <= kTabLast) {
    const size_t i = id - kTabFirst;
    if (c->MenuFramework() && target.valid() && i < tab_ids_.size()) {
      c->MenuFramework()->ActivateTabById(target.value, tab_ids_[i].value);
    }
    return true;
  }
  if (id >= kBookmarkFirst && id <= kBookmarkLast) {
    const size_t i = id - kBookmarkFirst;
    if (c->MenuHost() && i < bookmark_urls_.size()) {
      c->MenuHost()->RequestOpen(c->MenuSlot().value, bookmark_urls_[i]);
    }
    return true;
  }
  if (id >= kProfileFirst && id <= kProfileLast) {
    // Re-binds the active Space. Confirmed like the Mac's: the Space's
    // window is replaced and its tabs reload in the new profile, where the
    // old profile's logins do not exist.
    const size_t i = id - kProfileFirst;
    if (c->MenuHost() && i < profile_ids_.size() &&
        PhiConfirm(c->MenuShellWindow(),
                   PhiStrF("app.changeSpaceProfileConfirmation.title",
                           {i < profile_names_.size() ? profile_names_[i]
                                                      : std::wstring()})
                       .c_str(),
                   PhiStr("app.changeSpaceProfileConfirmation."
                          "profileScopedMessage"),
                   nullptr, /*destructive=*/false)) {
      c->MenuHost()->SetSpaceProfile(c->MenuActiveSpace(), profile_ids_[i]);
    }
    return true;
  }
  if (id >= kDeleteProfileFirst && id <= kDeleteProfileLast) {
    const size_t i = id - kDeleteProfileFirst;
    if (c->MenuHost() && i < profile_ids_.size() &&
        PhiConfirm(c->MenuShellWindow(),
                   PhiStr("app.spacesMenu.deleteProfile.submenuTitle"),
                   PhiStr("menuBar.deleteProfileConfirmBody"), L"Delete")) {
      c->MenuHost()->DeleteProfileById(profile_ids_[i]);
    }
    return true;
  }

  if (const int idc = IdcForCommand(id)) {
    if (c->MenuFramework() && target.valid()) {
      c->MenuFramework()->ExecuteChromiumCommand(target.value, idc);
    }
    return true;
  }

  switch (id) {
    case kCmdAbout:
      PhiNotice(c->MenuShellWindow(), PhiStr("menuBar.aboutPhi"),
                PhiStrF("menuBar.aboutBody", {PHI_WIN_VERSION_WIDE}).c_str());
      return true;
    case kCmdReportIssue:
      c->MenuOpenFeedback();
      return true;
    case kCmdCheckUpdate:
      if (!PhiUpdater::CheckForUpdateWithUi()) {
        PhiNotice(c->MenuShellWindow(), PhiStr("menuBar.checkForUpdate"),
                  PhiStr("menuBar.updaterUnavailableBody"));
      }
      return true;
    case kCmdPreferences:
      if (c->MenuHost()) {
        c->MenuHost()->RequestSettings();
      }
      return true;
    case kCmdQuit:
    case kCmdCloseWindow:
      // Through the normal close path; posted so the menu unwinds first.
      ::PostMessageW(c->MenuShellWindow(), WM_CLOSE, 0, 0);
      return true;
    case kCmdNewTab:
      c->MenuNewTab();  // Honors the "New tab behavior" setting.
      return true;
    case kCmdNewWindow:
      if (c->MenuHost()) {
        c->MenuHost()->RequestNewWindow(c->MenuSlot().value);  // Shown profile.
      }
      return true;
    case kCmdOpenLocation:
      if (c->MenuAddressField()) {
        ::SetFocus(c->MenuAddressField());
        ::SendMessageW(c->MenuAddressField(), EM_SETSEL, 0, -1);
      }
      return true;
    case kCmdCloseTab:
      // The TREE's active tab, not the active ROW: a tab bound to a pinned
      // cell or a bookmark is no row in the list, and Ctrl+W must still
      // close it (lane 1 D2).
      if (c->MenuFramework() && target.valid() && c->MenuActiveTab().valid()) {
        c->MenuFramework()->CloseTabById(target.value, c->MenuActiveTab().value);
      }
      return true;
    case kCmdUndo:
    case kCmdCut:
    case kCmdCopy:
    case kCmdPaste:
    case kCmdDelete:
    case kCmdSelectAll: {
      HWND field = c->MenuAddressField();
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
      const std::wstring url = c->MenuNavState().url;
      if (url.empty() || !::OpenClipboard(c->MenuShellWindow())) {
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
      PhiShowToast(c->MenuShellWindow(), c->MenuPalette(), PhiStr("app.copyTabURLsToast.single"));
      return true;
    }
    case kCmdStop:
      if (c->MenuFramework() && target.valid()) {
        c->MenuFramework()->StopLoading(target.value);
      }
      return true;
    case kCmdAlwaysShowBookmarkBar:
      if (c->MenuHost()) {
        c->MenuHost()->SetAlwaysShowBookmarkBar(!c->MenuHost()->AlwaysShowBookmarkBar());
      }
      return true;
    case kCmdBookmarkBarOnNewTab:
      if (c->MenuHost()) {
        c->MenuHost()->SetBookmarkBarOnNewTab(!c->MenuHost()->BookmarkBarOnNewTab());
      }
      return true;
    case kCmdToggleSidebar:
      c->MenuToggleSidebar();
      return true;
    case kCmdToggleReader:
      c->MenuToggleReader();
      return true;
    case kCmdReload:
    case kCmdForceReload:
      if (c->MenuFramework() && target.valid()) {
        c->MenuFramework()->Reload(target.value, id == kCmdForceReload);
      }
      return true;
    case kCmdLayoutBalanced:
    case kCmdLayoutPerformance:
    case kCmdLayoutComfortable:
      if (c->MenuHost()) {
        c->MenuHost()->RequestLayoutMode(
            id == kCmdLayoutBalanced    ? PhiLayoutMode::kBalanced
            : id == kCmdLayoutComfortable ? PhiLayoutMode::kComfortable
                                          : PhiLayoutMode::kPerformance);
      }
      return true;
    case kCmdNewSpace:
      c->MenuCreateSpace();  // The sidebar form, or at once without one.
      return true;
    case kCmdRenameSpace: {
      if (!c->MenuHost()) {
        return true;
      }
      // Prefill with the Space's current name (from the owner's list; the
      // chip label is the same string).
      std::wstring name;
      for (const PhiSpaceView& space : c->MenuHost()->Spaces()) {
        if (space.id == c->MenuActiveSpace()) {
          name = base::UTF8ToWide(space.name);
          break;
        }
      }
      if (PhiPromptText(c->MenuShellWindow(), PhiStr("app.renameSpaceDialog.title"),
                        PhiStr("app.renameSpaceDialog.message"), &name)) {
        c->MenuHost()->RenameSpace(c->MenuActiveSpace(), base::WideToUTF8(name));
      }
      return true;
    }
    case kCmdChangeIcon:
      if (c->MenuHost()) {
        c->MenuHost()->RequestSpaceIconPicker(c->MenuActiveSpace());
      }
      return true;
    case kCmdEditSpaceTheme:
      // The Mac's .windowTop placement: the menu bar has no trigger point to
      // hang the panel off, so the shell drops it under the caption, centred
      // on the window.
      c->MenuEditSpaceTheme();
      return true;
    case kCmdDeleteSpace:
      if (c->MenuHost() &&
          PhiConfirm(c->MenuShellWindow(), PhiStr("menuBar.deleteSpace"),
                     PhiStr("menuBar.deleteSpaceConfirmBody"), L"Delete")) {
        c->MenuHost()->DeleteSpace(c->MenuActiveSpace());
      }
      return true;
    case kCmdNewProfile: {
      std::wstring name;
      if (c->MenuFramework() &&
          PhiPromptText(c->MenuShellWindow(),
                        PhiStr("settings.profiles.createDialog.title"),
                        PhiStr("menuBar.newProfilePromptBody"), &name)) {
        c->MenuFramework()->CreateProfile(base::WideToUTF8(name).c_str());
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
        if (spaces[i].id == c->MenuActiveSpace()) {
          at = i;
          break;
        }
      }
      const size_t next = id == kCmdNextSpace
                              ? (at + 1) % spaces.size()
                              : (at + spaces.size() - 1) % spaces.size();
      const std::string space_id = spaces[next].id;
      if (c->MenuHasSpace(space_id)) {
        c->MenuShowSpace(space_id);
      } else if (c->MenuHost()) {
        c->MenuHost()->RequestSwitchSpace(c->MenuSlot().value, space_id);
      }
      return true;
    }
    case kCmdBack:
      // Back over a tab the reader is covering means "leave the reader" —
      // the same rule the toolbar's back button follows.
      if (c->MenuLeaveReaderForNavigation()) {
        return true;
      }
      if (c->MenuFramework() && target.valid()) {
        c->MenuFramework()->GoBack(target.value);
      }
      return true;
    case kCmdForward:
      if (c->MenuLeaveReaderForNavigation()) {
        return true;
      }
      if (c->MenuFramework() && target.valid()) {
        c->MenuFramework()->GoForward(target.value);
      }
      return true;
    case kCmdBookmarkThisTab:
      if (c->MenuHost()) {
        c->MenuHost()->RequestBookmarkActiveTab(c->MenuSlot().value);
      }
      return true;
    case kCmdBookmarkAllTabs: {
      if (!c->MenuHost() || c->MenuIncognito()) {
        return true;
      }
      std::wstring name = PhiStr("menuBar.savedTabs");
      if (!PhiPromptText(
              c->MenuShellWindow(),
              PhiStr("common.tabAreaContextMenu.bookmarkAllTabsAction"),
              PhiStr("menuBar.bookmarkAllTabsPromptBody"), &name) ||
          name.empty()) {
        return true;
      }
      const std::string folder = c->MenuHost()->AddBookmarkFolder(
          c->MenuSlot().value, std::string(), base::WideToUTF8(name));
      if (folder.empty() || !c->MenuModel() || !target.valid()) {
        return true;
      }
      int added = 0;
      if (const phi_framework::WindowSnapshot* snapshot =
              c->MenuModel()->Get(target)) {
        for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
          if (!tab.url.empty() &&
              !c->MenuHost()
                   ->AddBookmarkRow(c->MenuSlot().value, folder, tab.title,
                                    tab.url)
                   .empty()) {
            ++added;
          }
        }
      }
      PhiShowToast(
          c->MenuShellWindow(), c->MenuPalette(),
          PhiStrF("menuBar.tabsBookmarkedToast", {std::to_wstring(added)}));
      return true;
    }
    case kCmdBookmarkManager:
      // URL-driven, as on Mac: the manager IS a tab on the bookmark route,
      // and the client mounts its native page over that tab's content.
      if (c->MenuFramework() && target.valid()) {
        c->MenuFramework()->NewTab(target.value, PhiBookmarkManager::ManagerUrl());
      }
      return true;
    case kCmdExportBookmarks: {
      if (!c->MenuHost()) {
        return true;
      }
      wchar_t path[MAX_PATH] = L"bookmarks.html";
      OPENFILENAMEW ofn = {sizeof(ofn)};
      ofn.hwndOwner = c->MenuShellWindow();
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
        PhiShowToast(c->MenuShellWindow(), c->MenuPalette(),
                     PhiStr("menuBar.bookmarksExported"));
      }
      return true;
    }
    case kCmdNextTab:
    case kCmdPreviousTab: {
      // As the last rebuild placed them.
      const std::vector<PhiMenuBarTab> tabs = c->MenuTabRows();
      if (tabs.size() < 2 || !c->MenuFramework() || !target.valid()) {
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
      c->MenuFramework()->ActivateTabById(target.value, tabs[next].id.value);
      return true;
    }
    case kCmdImportBrowser:
      if (c->MenuFramework() &&
          PhiConfirm(c->MenuShellWindow(), PhiStr("menuBar.importFromGoogleChrome"),
                     PhiStr("menuBar.importChromePromptBody"),
                     PhiStr("app.import"), /*destructive=*/false)) {
        if (!c->MenuFramework()->StartChromeImport(target.value)) {
          PhiNotice(c->MenuShellWindow(), PhiStr("app.import"),
                    PhiStr("menuBar.importChromeFailedBody"));
        }
      }
      return true;
    case kCmdNewTabToRight: {
      // Composed from the primitives: IDC_NEW_TAB_TO_RIGHT stays disabled in
      // the hollowed chrome (it keys off FEATURE_TABSTRIP).
      if (!c->MenuFramework() || !target.valid()) {
        return true;
      }
      // By stable ids: the new tab's own id from OpenTab, the anchor the
      // tree's active tab, and an anchored move that survives whatever the
      // insertion did to the indices.
      const TabId anchor = c->MenuActiveTab();
      const TabId created = c->MenuFramework()->OpenTab(target, "");
      if (created.valid() && anchor.valid()) {
        c->MenuFramework()->MoveTabAfter(target.value, created.value, anchor.value,
                                    nullptr);
      }
      return true;
    }
    case kCmdSearchTabs:
      c->MenuToggleSearchTabs();
      return true;
    case kCmdDownloads:
      c->MenuToggleDownloads();
      return true;
    case kCmdMinimize:
      ::ShowWindow(c->MenuShellWindow(), SW_MINIMIZE);
      return true;
    case kCmdZoomWindow:
      ::ShowWindow(c->MenuShellWindow(),
                   ::IsZoomed(c->MenuShellWindow()) ? SW_RESTORE : SW_MAXIMIZE);
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
  if (services_->MenuHost()) {
    const std::vector<PhiStoredRowView> pins =
        services_->MenuHost()->PinnedRows(services_->MenuSlot().value);
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
           services_->MenuHost()->BookmarkRows(services_->MenuSlot().value,
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
