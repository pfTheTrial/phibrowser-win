// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_

#include <windows.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

// Where the browser's chrome lives. The Mac client's LayoutMode, same names
// and same meanings (PhiPreferences.swift), so a user moving between platforms
// gets the same three choices:
//
//   kPerformance  vertical tabs, address bar IN the sidebar (the default)
//   kBalanced     vertical tabs, address bar across the TOP of the web content
//   kComfortable  horizontal tabs — the traditional stacked layout, no sidebar
enum class PhiLayoutMode { kPerformance, kBalanced, kComfortable };

// Wire names, matching the Mac enum's raw values so the two clients can share
// a stored setting: "performance", "balanced", "comfortable".
const char* PhiLayoutModeName(PhiLayoutMode mode);
bool PhiLayoutModeFromName(const std::string& name, PhiLayoutMode* out);

// What a slot needs from its owner (the coordinator): the full Space list to
// draw, and requests that only the owner can service (switching to a Space
// that has no browser yet must create one in that Space's profile).
class PhiSlotHost {
 public:
  virtual ~PhiSlotHost() = default;
  // All Spaces as a JSON array string: [{"id","name","profileId"},…]
  virtual std::string AllSpacesJson() const = 0;
  virtual void RequestSwitchSpace(int64_t slot, const std::string& space_id) = 0;
  // Creates a Space in this slot's current profile (`new_profile` = false) or
  // in a freshly created profile (true), then switches to it.
  virtual void RequestCreateSpace(int64_t slot, bool new_profile) = 0;
  // The layout every slot renders in — one setting for the whole client, as on
  // Mac, so a new window never disagrees with the one beside it.
  virtual PhiLayoutMode LayoutMode() const = 0;
  virtual void RequestLayoutMode(PhiLayoutMode mode) = 0;
  // Pinned tabs and bookmarks for the slot, as JSON arrays:
  //   [{"guid","title","url","folder"?},…]
  virtual std::string PinnedJson(int64_t slot) const = 0;
  // Bookmark rows under `parent_guid` (empty = the root), so the menu bar can
  // descend into folders the way the Mac's Bookmarks menu does.
  virtual std::string BookmarksJson(int64_t slot,
                                    const std::string& parent_guid) const = 0;
  // Opens `url` in the slot (a pinned tab or bookmark was clicked).
  virtual void RequestOpen(int64_t slot, const std::string& url) = 0;
  // Bookmarks the slot's active tab — the menu bar's Bookmark This Tab.
  virtual void RequestBookmarkActiveTab(int64_t slot) = 0;
  // Stores tab `index` of the slot's active browser as a pinned tab
  // (`as_bookmark` false) or a bookmark — the row context menus.
  virtual void RequestStoreTab(int64_t slot, int index, bool as_bookmark) = 0;
  // Removes a stored row (a pinned tab or bookmark) by its guid.
  virtual void RemoveStoredRow(const std::string& guid) = 0;
  // The app-wide resolved theme.
  virtual const PhiPalette& Palette() const = 0;
  // The theme THIS SLOT paints with: a Space may pin its own (the Mac's
  // per-Space theme override), otherwise the app-wide one.
  virtual const PhiPalette& PaletteForSlot(int64_t slot) const = 0;
  // A Space's pinned theme id (empty = follows the app-wide one), and setting
  // it — the sidebar's Space menu writes through here.
  virtual std::string SpaceThemeId(const std::string& space_id) const = 0;
  virtual void SetSpaceTheme(const std::string& space_id,
                             const std::string& theme_id) = 0;
  // Space management (the menu bar and the chip context menu). Deleting the
  // default Space is refused; deletion takes the Space's bookmarks and
  // space-scoped pins with it, as on Mac.
  virtual bool RenameSpace(const std::string& space_id,
                           const std::string& name) = 0;
  virtual bool DeleteSpace(const std::string& space_id) = 0;
  // Profile management. Deleting Default, or a profile still bound to a
  // Space, is refused (bindings are immutable).
  virtual bool DeleteProfileById(const std::string& profile_id) = 0;
  // Opens the settings window (theme, appearance, layout).
  virtual void RequestSettings() = 0;
};

class PhiMenuBar;

// Owns the client-drawn shell window for one browser (the Windows twin of the
// Mac MainBrowserWindowController), using the Option-B model
// (windows-framework-design.md §2.4): the CLIENT owns a top-level window and
// its own painting, and Chromium's browser window is reparented in as a
// WS_CHILD filling the content rect. The client's sidebar is drawn into the
// client's own window (no DirectComposition conflict, unlike the falsified
// overlay approach), while web content renders via DComp inside the reparented
// child's own HWND — two non-overlapping regions, both visible.
class PhiWindowController {
 public:
  PhiWindowController(int64_t window_id,
                      HWND browser_hwnd,
                      phi_framework::PhiFramework* framework,
                      PhiSlotHost* host);
  ~PhiWindowController();

  PhiWindowController(const PhiWindowController&) = delete;
  PhiWindowController& operator=(const PhiWindowController&) = delete;

  int64_t window_id() const { return window_id_; }

  // Re-reads tab/nav state from the framework and repaints the sidebar NOW.
  // For changes the client itself just made, where the browser state has
  // already settled.
  void RefreshTabs();
  // Click on a pin or a sidebar bookmark row (cell click, context-menu
  // Open, or the pinned.open verb): focus the bound live tab if it still
  // exists, otherwise open the row's url as a new bound tab — the Mac's
  // BrowserState.openOrFocusPinnedTab / bookmarkGuidBacking model.
  void OpenOrFocusPinned(const std::string& guid, const std::string& url);

  // Paints one rounded-corner cap (called by its window proc).
  void PaintCornerCap(HWND cap, HDC dc);

  // Marks the sidebar dirty and lets the paint happen at the next idle. Used
  // for framework-driven notifications, which arrive DURING the change they
  // describe (a tab being removed is still in the strip) — painting inline
  // would render the pre-change state and then never correct it.
  void ScheduleRefresh();

  // Rebuilds `tabs_` from framework state WITHOUT invalidating (paint-time
  // use; RefreshTabs() would recurse through InvalidateRect).
  void RebuildTabs();
  // Rebuilds the Space chips row from the owner's full Space list.
  void RebuildChips();

  // --- Spaces (client-owned, as on Mac) ------------------------------------
  // A slot shows ONE Space at a time. Each Space has its own Browser; the
  // controller swaps which browser child is visible in the shell window.

  // Adopts an additional browser as `space_id`'s content for this slot and
  // shows it (hiding the previous). Called when the framework reports a
  // browser created for this slot.
  void AttachSpaceBrowser(const std::string& space_id,
                          int64_t browser_window_id,
                          HWND browser_hwnd);
  // Shows the Space's browser, hiding the current one. False if unknown.
  bool ShowSpace(const std::string& space_id);
  bool HasSpace(const std::string& space_id) const;
  const std::string& active_space() const { return active_space_; }
  // The browser window id currently displayed (for tab/nav commands).
  int64_t active_browser() const { return active_browser_; }
  // Spaces this slot has a browser for, in creation order.
  const std::vector<std::string>& space_order() const { return space_order_; }
  // Drops a browser that went away; returns true if the slot still has one.
  bool ForgetBrowser(int64_t browser_window_id);
  // Closes the browser this slot holds for `space_id` (a Space being
  // deleted), switching to another open Space first so the window never
  // shows a dead one. No-op when the Space is not open here. If it was the
  // slot's ONLY browser, the slot closes with it.
  void CloseSpaceBrowser(const std::string& space_id);
  // Starts closing the whole slot (its Space browsers, one per Space). MAY
  // DESTROY `this` before returning — the last browser going away tears the
  // slot down — so touch nothing on this object afterwards.
  void RequestClose();
  // Gives the currently shown browser Win32 focus and Chromium activation.
  void FocusActiveBrowser();

  // The shell window's menu bar (null before the shell exists). The control
  // socket reads and clicks it through here.
  PhiMenuBar* menu_bar() { return menu_bar_.get(); }

  // Re-lays the chrome for `mode` and repaints. Cheap: all geometry is derived
  // from the client rect on demand, so nothing is cached across the switch.
  void ApplyLayoutMode(PhiLayoutMode mode);
  // Repaints for a palette the host just changed.
  void ApplyPalette();
  // The content region (client coords, physical pixels) the browser child
  // occupies.
  RECT content_rect();
  // The DPI the chrome is currently laid out for (96 = 100%).
  UINT dpi() const { return dpi_; }
  // Chrome geometry, published so tools and tests can locate what they are
  // asserting about instead of mirroring constants that then drift.
  RECT url_field_rect();
  int tabs_top();
  int tab_row_height() const;
  // The gap between rows (the Mac's 2pt intercell spacing); row pitch is
  // tab_row_height() + tab_row_gap().
  int tab_row_gap() const;
  int sidebar_width() const;
  // The caption band's height — the row holding the menu titles and the
  // caption buttons. Everything else starts below it.
  int title_bar_height() const;
  // The tinted margin around the content card.
  int content_margin() const;
  // Client origin minus window origin. PrintWindow(PW_CLIENTONLY) renders from
  // the WINDOW origin, so a capture is offset by exactly this — a caller
  // comparing against client coordinates has to add it.
  POINT frame_offset() const;

  // Shell window geometry (client-owned; no framework round-trip).
  bool GetBounds(int* x, int* y, int* w, int* h) const;
  bool SetBounds(int x, int y, int w, int h);
  HWND shell_hwnd() const { return shell_hwnd_; }

  // Window procedure for the client shell window.
  static LRESULT CALLBACK ShellWndProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wparam,
                                       LPARAM lparam);

 private:
  // The menu bar drives navigation, tabs and Spaces through the controller's
  // own internals — it is this class's UI, split into its own file for size.
  friend class PhiMenuBar;

  // Every constant in this class is a DESIGN UNIT — a pixel at 100% scaling.
  // Win32 places windows and paints in PHYSICAL pixels, and the client is
  // per-monitor DPI aware (it inherits Chrome's manifest), so nothing reaches
  // the screen without going through here first. Without it the whole chrome
  // renders 20% small at 125% and half-size at 200%, while the system font —
  // which IS scaled — outgrows the boxes drawn for it.
  int Scaled(int design_units) const;
  RECT Scaled(const RECT& design_units) const;
  // Rebuilds `ui_font_` for `dpi_`. Idempotent for an unchanged DPI.
  void EnsureFont();

  // Every rect the chrome needs, for one layout mode. Derived from the client
  // rect on demand rather than stored, so a resize or a mode switch needs no
  // invalidation of its own.
  struct Layout {
    PhiLayoutMode mode = PhiLayoutMode::kPerformance;
    RECT client{};
    // The caption band: menu titles leading, then drag space, then the three
    // caption buttons. The OS caption is gone (WM_NCCALCSIZE) — this row IS
    // the title bar, so the menu shares it with minimize / maximize / close.
    RECT title_bar{};
    RECT caption_min{}, caption_max{}, caption_close{};
    RECT sidebar{};  // Empty in Comfortable.
    RECT top_bar{};  // The band above the content; empty in Performance.
    RECT content{};  // Where the reparented browser child goes.
    // Nav row: the two leading buttons, then the trailing back/forward/reload
    // group — the Mac header's arrangement (SidebarHeaderView).
    RECT sidebar_toggle{}, search_tabs{};
    RECT back{}, forward{}, reload{};
    RECT spaces_strip{};   // The band the Space chips live in.
    RECT url_field{};
    RECT favorites{};      // Pinned-tab band; empty until the feature lands.
    RECT new_tab_row{};    // The sidebar's "+ New Tab" row.
    RECT bottom_bar{};
    // Bottom-bar buttons, the Mac's order: downloads and memory leading, the
    // Feedback and Chat capsules trailing. Settings has NO bottom-bar button —
    // it opens from the menu bar, as on Mac. All placeholders except through
    // the menu.
    RECT downloads{}, memory{}, feedback{}, chat{};
    RECT add_space{};      // The strip's trailing "+" chip.
    int chips_left = 0, chips_top = 0;
    int chips_limit = 0;  // Right edge chips may not cross.
    int tabs_left = 0, tabs_top = 0;
    int tabs_limit = 0;   // Bottom (vertical) or right (horizontal) edge.
    bool horizontal_tabs = false;
  };
  Layout ComputeLayout();

  void AdoptBrowserAsChild();
  void LayoutContents();
  // Moves the address field's EDIT child into `layout`'s slot for it.
  void PositionUrlField(const Layout& layout);
  // `include_child_controls` draws what child windows would draw themselves —
  // set only on the WM_PRINTCLIENT (PrintWindow) path, which renders the
  // parent alone. In WM_PAINT the URL field's EDIT child paints that region.
  void PaintChrome(HDC dc, const Layout& layout, bool include_child_controls);
  // The caption band's menu titles, measured for the current DPI: one rect
  // per top-level menu, in bar order. Rebuilt when the DPI changes.
  void EnsureMenuTitleRects();
  // What a point in the caption band hits: HTCLOSE / HTMAXBUTTON /
  // HTMINBUTTON, HTCLIENT over a menu title, HTCAPTION anywhere else in the
  // band, 0 below it.
  int CaptionHitTest(POINT client_pt);
  // Hot-state setters; each repaints the band when the state changes.
  void SetCaptionHot(int code);
  void SetMenuHot(int index);
  // Handles a click in the chrome (tab rows, close boxes, buttons, toolbar).
  void OnChromeClick(int x, int y);
  // Right-click in the chrome. On a Space chip this raises the Mac's Space
  // menu (activate / rename / change icon / set theme / delete); elsewhere it
  // does nothing, as on Mac.
  void OnChromeContextMenu(int x, int y);
  // Re-reads navigation state (url/title/canGoBack/…) for the toolbar.
  void RebuildNavState();

  // --- Address field --------------------------------------------------------
  // A real Win32 EDIT child, so typing, selection, clipboard, IME and the
  // caret all behave natively (Chromium's own omnibox is hollowed away under
  // IS_WIN_PHI_CLIENT, and hand-rolling text editing would reproduce it badly).
  void EnsureUrlField();
  // Pushes the current page's URL into the field. Skipped while the user is
  // typing unless `force` (the Escape-reverts path).
  void SyncUrlField(bool force);
  // Navigates to whatever is typed, then hands focus back to the page.
  void CommitUrlField();
  // Draws a favicon's BGRA pixels into `dest`, composited over `background`.
  // GDI has no alpha blit without msimg32 (AlphaBlend), and we always know what
  // is behind the icon, so the blend is done in software and the result blitted
  // opaque.
  void DrawFavicon(HDC dc,
                   const RECT& dest,
                   const unsigned char* bgra,
                   int width,
                   int height,
                   COLORREF background);
  // Subclass proc: Enter commits, Escape reverts, focus selects all.
  static LRESULT CALLBACK UrlFieldProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wparam,
                                       LPARAM lparam);
  HWND url_field_ = nullptr;
  // The EDIT paints itself in SYSTEM colours, which clash under a dark theme.
  // WM_CTLCOLOREDIT hands it the palette instead; the brush must outlive the
  // message, so it is cached and rebuilt only when the colour changes.
  HBRUSH url_brush_ = nullptr;
  COLORREF url_brush_color_ = CLR_INVALID;
  // The shell's UI font at `ui_font_dpi_`, used both for the address field and
  // for every label PaintChrome draws, so text and boxes scale together.
  HFONT ui_font_ = nullptr;
  UINT ui_font_dpi_ = 0;
  // Space icons are emoji, which the UI font has no glyphs for; Segoe UI Emoji
  // does. GDI renders it MONOCHROME — colour emoji need DirectWrite, which is
  // a much larger change than the icons are worth right now.
  HFONT emoji_font_ = nullptr;
  UINT emoji_font_dpi_ = 0;
  // Segoe MDL2 Assets for the caption glyphs (minimize / maximize / restore /
  // close) — the same glyphs the OS buttons use.
  HFONT caption_font_ = nullptr;
  UINT caption_font_dpi_ = 0;
  // MDL2 at the emoji size, for Space-pip catalog glyphs.
  HFONT chip_icon_font_ = nullptr;
  UINT dpi_ = 96;

  // Caption-band state: which caption button is hot / pressed (an HT* code,
  // 0 = none) and which menu title is hot (-1 = none). Purely visual except
  // pressed_caption_, which arms the button-up action.
  int hot_caption_ = 0;
  int pressed_caption_ = 0;
  int hot_menu_ = -1;
  // Menu titles with their band rects, measured for menu_titles_dpi_.
  std::vector<std::pair<std::wstring, RECT>> menu_titles_;
  UINT menu_titles_dpi_ = 0;

  // One rendered tab row: what to draw and what a click at that row does.
  struct TabRow {
    int index = 0;
    int id = 0;  // Session id — stable while indices shift; pins bind to it.
    // The tab's favicon, copied out of the framework's scratch buffer (which
    // only survives until its next call).
    std::vector<unsigned char> favicon;
    int favicon_width = 0;
    int favicon_height = 0;
    bool active = false;
    std::wstring title;
    // Chromium native side-by-side: members share a splitId. The leading
    // member renders the MERGED row and absorbs its partner as pane B; the
    // partner never becomes its own row.
    std::string split_id;
    // Tab-group membership (Chromium native model); members sit under their
    // group's header row and hide when it is collapsed.
    std::string group_id;
    int b_index = -1;  // -1: not a merged row.
    bool b_active = false;
    std::wstring b_title;
    std::vector<unsigned char> b_favicon;
    int b_favicon_width = 0;
    int b_favicon_height = 0;
    RECT bounds{};       // Row rect in client coords.
    RECT pane_b{};       // Right half of a merged row (activates pane B).
    RECT close_box{};    // The row's close affordance.
  };
  std::vector<TabRow> tabs_;

  // Sidebar tab-group headers (TabGroupHeaderView): chevron + color dot +
  // title on the group's 18% color tint, clicking collapses/expands.
  struct GroupHeader {
    std::string id;
    std::wstring title;
    COLORREF color = 0;
    bool collapsed = false;
    RECT bounds{};     // The header row (the collapse toggle).
    RECT container{};  // Header plus visible members — the tinted card.
  };
  std::vector<GroupHeader> group_headers_;
  // Pinned tabs (the band under the header) and bookmarks (the section above
  // the open tabs) — both come from the client's own store, as on Mac.
  struct ShortcutRow {
    std::string guid;  // The store row, so the context menu can remove it.
    std::string url;
    std::wstring title;
    bool folder = false;
    // A pinned tab bound to a LIVE tab (openOrFocusPinnedTab): the tab lives
    // in the pin's cell — never in the tab list — and the active one gets the
    // Mac's selected wash + 2pt accent ring.
    bool open = false;
    bool active = false;
    RECT bounds{};
    // The favicon stored with the row (a pinned tab has no live tab to ask).
    std::vector<unsigned char> favicon;
    int favicon_width = 0;
    int favicon_height = 0;
  };
  std::vector<ShortcutRow> pinned_;
  std::vector<ShortcutRow> bookmarks_;
  // Counted before the layout is derived, because the bands they occupy push
  // everything below them down.
  int pinned_row_count_ = 0;
  int bookmark_row_count_ = 0;
  void RebuildShortcuts();

  // Live stored-row bindings, per Space browser: pin OR bookmark guid → tab
  // session id. The Mac binds by giving the created tab the row's guid as
  // its identity; here the session id from ListTabsJson carries that role.
  // Entries are pruned on every rebuild (row removed, tab closed) —
  // runtime-only, so a restart returns rows to the unopened state. Bookmark
  // bindings hide their tab from the list only in vertical layouts.
  std::map<int64_t, std::map<std::string, int>> pin_bindings_;
  // The tab's current index by session id, or -1 when it is gone.
  int IndexOfTabId(int64_t browser, int id) const;

  // Navigation state for the client toolbar, refreshed with the tab list.
  std::wstring nav_url_;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
  bool nav_loading_ = false;

  const int64_t window_id_;
  HWND browser_hwnd_;  // The VISIBLE Chromium window, reparented as our child.
  // Space id -> its browser window id / HWND, plus creation order.
  std::map<std::string, int64_t> space_browsers_;
  std::map<int64_t, HWND> browser_hwnds_;
  std::vector<std::string> space_order_;
  std::string active_space_;
  int64_t active_browser_ = 0;
  HWND shell_hwnd_ = nullptr;  // Our client-owned top-level.
  std::unique_ptr<PhiMenuBar> menu_bar_;  // Attached to shell_hwnd_.
  // The four content-corner caps (TL, TR, BL, BR), siblings above the
  // browser child — the only rounding DComp content respects.
  std::array<HWND, 4> corner_caps_ = {};
  // Owned by the framework (a singleton in chrome.dll); non-owning, and across
  // a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* const framework_;
  RAW_PTR_EXCLUSION PhiSlotHost* const host_;  // Owns this controller.
  // The Space list as drawn (id + whether this slot has it open).
  struct SpaceChip {
    std::string id;
    std::wstring label;
    std::wstring icon;  // The Space's emoji, empty when it has none.
    bool open = false;
    RECT bounds{};
  };
  std::vector<SpaceChip> chips_;
  RECT add_space_button_{};
  PhiLayoutMode layout_mode_ = PhiLayoutMode::kPerformance;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_
