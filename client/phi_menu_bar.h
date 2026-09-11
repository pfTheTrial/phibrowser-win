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
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

class ChromePrefsHost;
struct PhiAgentPipView;
class PhiWindowModel;
class SidebarHost;
class SpacesHost;
class WindowHost;

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
  // Opens the Set Theme panel on the active Space (the Mac's Edit Theme,
  // which presents SpaceThemeEditorView rather than a list of themes).
  kCmdEditSpaceTheme,
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
  kCmdBookmarkManager,
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
  // APPEND ONLY below this line: the box test rigs post these ids as raw
  // WM_COMMAND numbers, so renumbering an existing entry silently rewires
  // their clicks.
  kCmdToggleReader,  // View — Reader View for the active tab.
  kCmdAgentAutoview,  // View — Agent Autoview (follows the working agent).
};

// One finished sidebar row, as the Tab menu lists it. A SNAPSHOT of what the
// last rebuild placed: a menu opening is not a reason to rebuild
// (docs/architecture-review.md §3.1 principle 3; phase 2 made rebuilds
// posted).
struct PhiMenuBarTab {
  TabId id;
  std::wstring title;
  bool active = false;
};

// The navigation state the View, History and Bookmarks menus enable from.
struct PhiMenuBarNavState {
  std::wstring url;
  bool can_go_back = false;
  bool can_go_forward = false;
  bool loading = false;
};

// WHAT THE BAR ASKS OF ITS SHELL (docs/architecture-review.md §3.5, lane 1
// §2.1 item 3). Until phase 6 the bar was a `friend` of PhiShellWindow
// and read seventeen private members by name — `host_`, `framework_`,
// `tabs_`, `url_field_`, `nav_url_`, `active_space_`, `active_tab_id_` and
// the rest — which is why the shell had to keep const reference VIEWS onto
// its own components just to stay compilable. This interface replaced all of
// it: nav state, the finished rows, the active Space, the address field, and
// command dispatch. No `friend`, and nothing here reaches back into a cache
// the bar could outlive.
class PhiMenuBarShell {
 public:
  // --- Identity and services ----------------------------------------------
  virtual HWND MenuShellWindow() const = 0;
  virtual SlotId MenuSlot() const = 0;
  virtual bool MenuIncognito() const = 0;
  // The bar genuinely spans bookmarks, Spaces, profiles and prefs — but not
  // the agent's: it takes those FOUR narrow services, never the set
  // (docs/architecture-review.md §3.5, lane 1 §4.3). Any of them may be
  // null, and the same guards that used to test one host now test the one
  // the item about to run needs.
  virtual SidebarHost* MenuSidebarHost() const = 0;
  virtual SpacesHost* MenuSpacesHost() const = 0;
  virtual ChromePrefsHost* MenuPrefsHost() const = 0;
  virtual WindowHost* MenuWindowHost() const = 0;
  virtual phi_framework::PhiFramework* MenuFramework() const = 0;
  virtual const PhiWindowModel* MenuModel() const = 0;
  // The browser the slot SHOWS — invalid when none is attached, and NEVER
  // the slot's own number.
  virtual BrowserId MenuActiveBrowser() const = 0;
  // The TREE's active tab, not the active ROW: a tab bound to a pinned cell
  // or a bookmark is no row in the list, and Ctrl+W must still close it.
  virtual TabId MenuActiveTab() const = 0;
  virtual std::string MenuActiveSpace() const = 0;
  virtual PhiPalette MenuPalette() const = 0;
  virtual int MenuScaled(int design_units) const = 0;

  // --- The omnibox --------------------------------------------------------
  // The address field's EDIT (null before the shell has one): Open Location
  // and the Edit menu's text commands operate on it.
  virtual HWND MenuAddressField() const = 0;
  // Open Location: focuses the field, selected whole — raising the floating
  // sidebar first when the collapsed performance shell has no field of its
  // own (the panel's is the one to type in).
  virtual void MenuFocusAddressField() = 0;
  // Re-reads navigation state from the window model. NOT a row rebuild —
  // the rows a menu shows are always the finished ones.
  virtual void MenuRefreshNavState() = 0;
  virtual PhiMenuBarNavState MenuNavState() const = 0;

  // --- The finished rows --------------------------------------------------
  virtual std::vector<PhiMenuBarTab> MenuTabRows() const = 0;

  // --- Reader View --------------------------------------------------------
  virtual bool MenuReaderActive() const = 0;
  virtual bool MenuReaderOfferable() const = 0;
  // Back/Forward over a tab the reader covers means "leave the reader".
  virtual bool MenuLeaveReaderForNavigation() = 0;

  // --- Commands the bar hands back ----------------------------------------
  // True while an agent holds this (agent) window: the tab, navigation
  // and bookmark commands are refused (see PhiShellWindow::AgentHoldsInput).
  virtual bool MenuAgentHoldsInput() const = 0;
  // The agent Spaces this window's switchers offer, after the user Spaces
  // — the Mac's slot.presentedSpaces, which the Spaces menu rows, Ctrl+N
  // and Next/Previous Space index into (empty in agent and incognito
  // windows). See PhiAgentPipView (phi_shell_window.h).
  virtual std::vector<PhiAgentPipView> MenuAgentSpaces() const = 0;
  // A Spaces menu row / Ctrl+N pick: PhiShellWindow::SwitchToSpace.
  virtual void MenuSwitchSpace(const std::string& space_id) = 0;
  virtual void MenuNewTab() = 0;
  virtual void MenuToggleSidebar() = 0;
  // `from_accelerator`: the chord rather than the View menu row (the
  // reader's usage metric names the two differently).
  virtual void MenuToggleReader(bool from_accelerator) = 0;
  virtual void MenuToggleDownloads() = 0;
  virtual void MenuToggleSearchTabs() = 0;
  virtual void MenuOpenFeedback() = 0;
  virtual void MenuCreateSpace() = 0;
  // The Mac's Edit Theme: the panel drops under the caption, centred on the
  // window (the menu bar has no trigger point to hang it off).
  virtual void MenuEditSpaceTheme() = 0;

 protected:
  // The shell owns the bar, never the reverse.
  ~PhiMenuBarShell() = default;
};

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
  explicit PhiMenuBar(PhiMenuBarShell* shell);
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
  // Drops the comfortable layout's overflow menu — Chrome's ⋮. That layout
  // gives the caption band to the tab strip, so it has no menu titles: this
  // is the only way to the bar there. Every top-level menu hangs off it as
  // a submenu, under the two entries that lost their bands with the sidebar
  // (Chat and Feedback). Right-aligned, since the button trails the toolbar.
  void ShowOverflow(int screen_x, int screen_y);

  // WM_INITMENUPOPUP: refresh `menu` if it is one of ours. TrackPopupMenu
  // fires it too, so dropped-down menus refresh exactly like attached ones.
  void OnMenuOpening(HMENU menu);
  // WM_COMMAND with lparam == 0. True when `id` was a menu command.
  // `from_accelerator` is WM_COMMAND's HIWORD 1: the chord, not the row.
  bool OnCommand(UINT id, bool from_accelerator = false);

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
  // Re-reads the live enable state of every Chromium-routed item (the
  // kIdcRoutes table) from the framework — run on every menu open.
  void RefreshChromiumCommands();
  // The Netscape bookmark HTML the Mac exporter writes: a Favorites folder
  // of the pinned rows first, then the Space's bookmark tree.
  std::string BuildBookmarksHtml() const;
  // Rebuilds the bookmark rows of `menu` from the store's rows under
  // `parent_guid` — folders recurse into submenus, as on Mac.
  void AppendBookmarkRows(HMENU menu, const std::string& parent_guid,
                          int depth);
  // Every Space from the owner's full list — the strip's chips only mirror
  // what fits (one, in the comfortable layout). The title is the bare name;
  // the icon glyph rides beside it, rasterized into the row's bitmap.
  struct SpaceEntry {
    std::string id;
    std::wstring title;
    std::wstring glyph;
  };
  std::vector<SpaceEntry> SpaceList() const;
  void ReleaseSpaceBitmaps();

  RAW_PTR_EXCLUSION PhiMenuBarShell* const services_;  // Owns this.
  HMENU bar_ = nullptr;
  // The overflow popup. It borrows the SAME submenu handles `bar_` holds, so
  // its items are detached before it is destroyed — destroying it outright
  // would take the shared popups with it and leave `bar_` holding freed
  // handles.
  HMENU overflow_ = nullptr;
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
  std::vector<HBITMAP> space_bitmaps_;     // The Space rows' icons; ours.
  std::vector<std::string> profile_ids_;   // kProfileFirst / kDeleteProfileFirst + i
  std::vector<std::wstring> profile_names_;  // Their display names, in step.
  std::vector<std::string> bookmark_urls_; // kBookmarkFirst + i
  std::vector<TabId> tab_ids_;             // kTabFirst + i
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_MENU_BAR_H_
