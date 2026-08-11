// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_MENU_BAR_H_
#define PHIBROWSER_WIN_CLIENT_PHI_MENU_BAR_H_

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_client {

// Menu command ids (WM_COMMAND). In the header so the keyboard-accelerator
// hook can post the same commands the menus fire.
enum : UINT {
  // Phi
  kCmdAbout = 1001,
  kCmdPreferences,
  kCmdCheckUpdate,
  kCmdDeleteBrowsingData,
  kCmdImportBrowser,
  kCmdQuit,
  // File
  kCmdNewTab,
  kCmdNewWindow,
  kCmdNewIncognitoWindow,
  kCmdNewIncognitoSpace,
  kCmdReopenClosedTab,
  kCmdOpenFile,
  kCmdOpenLocation,
  kCmdCloseWindow,
  kCmdCloseTab,
  kCmdSavePageAs,
  kCmdPrint,
  // Edit
  kCmdUndo,
  kCmdRedo,
  kCmdCut,
  kCmdCopy,
  kCmdCopyUrl,
  kCmdPaste,
  kCmdPasteMatchStyle,
  kCmdDelete,
  kCmdSelectAll,
  kCmdSearchWeb,
  kCmdFind,
  kCmdFindNext,
  kCmdFindPrevious,
  // View
  kCmdStop,
  kCmdReload,
  kCmdForceReload,
  kCmdFullScreen,
  kCmdActualSize,
  kCmdZoomIn,
  kCmdZoomOut,
  kCmdViewSource,
  kCmdDevTools,
  kCmdInspect,
  kCmdJsConsole,
  kCmdLayoutHeader,
  kCmdLayoutBalanced,
  kCmdLayoutPerformance,
  kCmdLayoutComfortable,
  kCmdAlwaysShowBookmarkBar,
  kCmdBookmarkBarOnNewTab,
  kCmdToggleSidebar,
  kCmdToggleChatbar,
  kCmdNewConversation,
  // Spaces
  kCmdNewSpace,
  kCmdRenameSpace,
  kCmdChangeIcon,
  kCmdDeleteSpace,
  kCmdNextSpace,
  kCmdPreviousSpace,
  // Switch to the Nth Space — the Ctrl+1..9 the Spaces menu advertises,
  // delivered by the coordinator's keyboard hook (the Mac's ⌃1..⌃9).
  kCmdSpace1,
  kCmdSpace2,
  kCmdSpace3,
  kCmdSpace4,
  kCmdSpace5,
  kCmdSpace6,
  kCmdSpace7,
  kCmdSpace8,
  kCmdSpace9,
  kCmdUrlRules,
  kCmdNewProfile,
  // History
  kCmdHome,
  kCmdBack,
  kCmdForward,
  kCmdRecentlyClosedHeader,
  kCmdShowFullHistory,
  // Bookmarks
  kCmdBookmarkThisTab,
  kCmdBookmarkAllTabs,
  kCmdExportBookmarks,
  // Tab
  kCmdNewTabToRight,
  kCmdNextTab,
  kCmdPreviousTab,
  kCmdDuplicateTab,
  kCmdMuteSite,
  kCmdSearchTabs,
  // Window
  kCmdMinimize,
  kCmdZoomWindow,
  kCmdNameWindow,
  kCmdDownloads,
  kCmdExtensions,
  kCmdTaskManager,
  // Help
  kCmdReportIssue,
  kCmdWhatsNew,
  kCmdDocumentation,
  kCmdExportUserData,
  kCmdImportUserData,
};

class PhiWindowController;

// The Mac menu bar (AppController+Menu.swift over Chromium's
// main_menu_builder.mm), ported as a native Win32 menu on the shell window:
// Phi | File | Edit | View | Spaces | History | Bookmarks | Tab | Window |
// Help — the Mac's visible top-level order. Mac-only surfaces (Services,
// Hide/Show All, Share, dictation, the AppKit window list) are skipped
// deliberately; items whose feature has no Windows surface yet are drawn
// DISABLED, so the bar reads like the Mac one without lying about what works.
//
// The four data-driven menus (Spaces, Bookmarks, Tab, and the enable states
// everywhere) are rebuilt each time their menu drops down — the Win32
// equivalent of the Mac's menuWillOpen rebuilds.
class PhiMenuBar {
 public:
  explicit PhiMenuBar(PhiWindowController* controller);
  ~PhiMenuBar();

  PhiMenuBar(const PhiMenuBar&) = delete;
  PhiMenuBar& operator=(const PhiMenuBar&) = delete;

  // Builds the menus. The bar HMENU is NOT attached with SetMenu: the shell
  // draws the titles itself, in the caption row beside minimize / maximize /
  // close, and opens each popup on click. The HMENU tree stays the single
  // source of truth for what the menus contain.
  void Install(HWND shell);

  // The top-level titles, in order, mnemonics stripped — what the shell draws.
  std::vector<std::wstring> Titles() const;
  // Drops menu `index` at screen coordinates. The popup routes WM_COMMAND to
  // the shell (no TPM_RETURNCMD), so the click path is one code path.
  void ShowAt(size_t index, int screen_x, int screen_y);

  // WM_INITMENUPOPUP: refresh `menu` if it is one of ours. TrackPopupMenu
  // fires it too, so dropped-down menus refresh exactly like attached ones.
  void OnMenuOpening(HMENU menu);
  // WM_COMMAND with lparam == 0. True when `id` was a menu command.
  bool OnCommand(UINT id);

  // The whole bar as JSON (refreshing dynamic menus first), for the control
  // socket: [{"title","items":[{"id","title","enabled","checked","items"?}]}].
  std::string ListJson();
  // Activates the item reached by `path` (top-level title, then item titles,
  // matched with mnemonics and "\t…" shortcut text stripped). False when no
  // such item exists or it is disabled.
  bool InvokePath(const std::vector<std::string>& path);

 private:
  void BuildStatic();
  void RefreshPhi();
  void RefreshFile();
  void RefreshEdit();
  void RefreshView();
  void RefreshSpaces();
  void RefreshHistory();
  void RefreshBookmarks();
  void RefreshTab();
  // Rebuilds the bookmark rows of `menu` from the store's rows under
  // `parent_guid` — folders recurse into submenus, as on Mac.
  void AppendBookmarkRows(HMENU menu, const std::string& parent_guid,
                          int depth);
  // Every Space as {id, display title} from the owner's full list — the
  // strip's chips only mirror what fits (one, in the comfortable layout).
  std::vector<std::pair<std::string, std::wstring>> SpaceList() const;

  RAW_PTR_EXCLUSION PhiWindowController* const controller_;  // Owns this.
  HMENU bar_ = nullptr;
  HWND shell_ = nullptr;  // Owner for TrackPopupMenu (commands route here).
  HMENU phi_ = nullptr;
  HMENU file_ = nullptr;
  HMENU edit_ = nullptr;
  HMENU view_ = nullptr;
  HMENU spaces_ = nullptr;
  HMENU history_ = nullptr;
  HMENU bookmarks_ = nullptr;
  HMENU tab_ = nullptr;
  HMENU window_ = nullptr;
  HMENU help_ = nullptr;

  // What the dynamic ids currently mean, rebuilt with their menus.
  std::vector<std::string> space_ids_;     // kSpaceFirst + i
  std::vector<std::string> profile_ids_;   // kDeleteProfileFirst + i
  std::vector<std::string> bookmark_urls_; // kBookmarkFirst + i
  std::vector<int> tab_indices_;           // kTabFirst + i
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_MENU_BAR_H_
