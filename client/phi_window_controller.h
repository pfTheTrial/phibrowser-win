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

// A stored row (pinned tab or bookmark) as the chrome consumes it — the
// favicon already decoded to raw premultiplied BGRA.
struct PhiStoredRowView {
  std::string guid;
  std::string title;  // UTF-8.
  std::string url;
  bool folder = false;
  std::vector<unsigned char> favicon_bgra;
  int favicon_width = 0;
  int favicon_height = 0;
  // Split view (the Mac's model): a bookmark split is ONE row carrying the
  // second pane here; a pinned split is TWO rows cross-linked by partner.
  std::string url_b;
  std::string title_b;  // Empty when it duplicates the primary's.
  std::string partner_guid;
};

// A Space as the chips row and menus consume it.
struct PhiSpaceView {
  std::string id;
  std::string name;       // UTF-8.
  std::string profile_id;
  std::string icon;       // Storage form ("emoji:<id>" / "phi:<id>").
  std::string icon_text;  // Resolved display glyph, UTF-8 (may be PUA).
};

// What a slot needs from its owner (the coordinator): the full Space list to
// draw, and requests that only the owner can service (switching to a Space
// that has no browser yet must create one in that Space's profile).
class PhiSlotHost {
 public:
  virtual ~PhiSlotHost() = default;
  // Every Space, in display order.
  virtual std::vector<PhiSpaceView> Spaces() const = 0;
  virtual void RequestSwitchSpace(int64_t slot, const std::string& space_id) = 0;
  // Creates a Space in this slot's current profile (`new_profile` = false) or
  // in a freshly created profile (true), then switches to it.
  virtual void RequestCreateSpace(int64_t slot, bool new_profile) = 0;
  // The layout every slot renders in — one setting for the whole client, as on
  // Mac, so a new window never disagrees with the one beside it.
  virtual PhiLayoutMode LayoutMode() const = 0;
  virtual void RequestLayoutMode(PhiLayoutMode mode) = 0;
  // The slot's pinned tabs, in sibling order.
  virtual std::vector<PhiStoredRowView> PinnedRows(int64_t slot) const = 0;
  // Bookmark rows under `parent_guid` (empty = the root), so the menu bar can
  // descend into folders the way the Mac's Bookmarks menu does.
  virtual std::vector<PhiStoredRowView> BookmarkRows(
      int64_t slot,
      const std::string& parent_guid) const = 0;
  // Opens `url` in the slot (a pinned tab or bookmark was clicked).
  virtual void RequestOpen(int64_t slot, const std::string& url) = 0;
  // Bookmarks the slot's active tab — the menu bar's Bookmark This Tab.
  virtual void RequestBookmarkActiveTab(int64_t slot) = 0;
  // Stores tab `index` of the slot's active browser as a pinned tab
  // (`as_bookmark` false) or a bookmark — the row context menus.
  virtual void RequestStoreTab(int64_t slot, int index, bool as_bookmark) = 0;
  // Stores a live SPLIT (panes at `index_a` — the leading member — and
  // `index_b`) the Mac's two ways: as a bookmark, ONE row with a second
  // url; as a pinned tab, TWO cross-linked rows with both live panes bound.
  virtual void RequestStoreSplit(int64_t slot, int index_a, int index_b,
                                 bool as_bookmark) = 0;
  // Removes a stored row (a pinned tab or bookmark) by its guid.
  virtual void RemoveStoredRow(const std::string& guid) = 0;
  // Retitles a stored row — the context menus' Rename….
  virtual bool RenameStoredRow(const std::string& guid,
                               const std::string& name) = 0;
  // Moves a stored row to `new_index` among its siblings (drag-to-reorder in
  // the pinned grid and bookmark list).
  virtual void ReorderStoredRow(const std::string& guid, int new_index) = 0;
  // Moves a Space to `new_index` in the Space list (chip drag).
  virtual void ReorderSpace(const std::string& space_id, int new_index) = 0;
  // The sidebar width in DIP — one app-wide value like the layout mode,
  // clamped to the Mac split range (193–500).
  virtual int SidebarWidth() const = 0;
  virtual void RequestSidebarWidth(int dip) = 0;
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

// The Mac sidebar's default and drag range (MainSplitViewController).
inline constexpr int kSidebarWidthDefault = 240;
inline constexpr int kSidebarWidthMin = 193;
inline constexpr int kSidebarWidthMax = 500;

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
  // Re-derives the geometry and repositions the browser child + corner caps
  // (public so the owner can push app-wide layout changes, e.g. the sidebar
  // width, into every window).
  void LayoutContents();

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
    // The client identity tag (PhiTabData, session-persisted): a tab tagged
    // with a live pin/bookmark guid IS that row's bound tab.
    std::string tag;
    // chrome:// pages show the Phi mark when no favicon exists.
    bool internal_page = false;
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

  // Model freshness: rebuilds happen on notifications and intents
  // (ScheduleRefresh/RefreshTabs/LayoutContents mark this), not per paint.
  // WM_PRINTCLIENT still forces a rebuild — captures must be current.
  bool rows_dirty_ = true;
  // Cached paint resources, recreated on resize / palette change instead of
  // per WM_PAINT: the full-window backbuffer bitmap and the ground-gradient
  // band brushes.
  HBITMAP back_buffer_ = nullptr;
  SIZE back_buffer_size_{};
  std::vector<HBRUSH> band_brushes_;
  COLORREF band_top_ = 0;
  COLORREF band_bottom_ = 0;
  // The slot palette's window ground — what uncovered backbuffer pixels show.
  COLORREF BackgroundFill() const;

  // Sidebar tab-list scrolling (bookmark rows and the pinned band stay put;
  // the group/tab rows scroll). Offset in pixels, clamped to the overflow.
  int scroll_offset_ = 0;
  int tabs_scroll_max_ = 0;
  // Wheel over the sidebar: true when consumed (vertical layouts only).
  bool OnSidebarWheel(POINT client_pt, int wheel_delta);

  // Drag-to-reorder: armed on button-down over a draggable element, live
  // once the cursor passes the system drag threshold, committed on button-up
  // through MoveTab / ReorderStoredRow / ReorderSpace. The insertion slot is
  // painted as a 2px accent line while dragging.
  enum class DragKind { kNone, kTab, kPin, kBookmark, kChip };
  DragKind drag_kind_ = DragKind::kNone;
  int drag_index_ = -1;   // Position in the element's own vector.
  POINT drag_origin_{};
  bool drag_active_ = false;
  int drag_insert_ = -1;  // Insertion slot 0..n, -1 = none.

  // Sidebar edge resize (the Mac's split drag): a 4-DIP grab band in the
  // sidebar's trailing gutter; live while dragging, persisted on release.
  int sidebar_dip_ = kSidebarWidthDefault;
  bool sidebar_resizing_ = false;
  bool InSidebarResizeBand(POINT client_pt);

  void ArmDrag(DragKind kind, int index, POINT pt);
  void UpdateDrag(POINT pt);
  void CommitDrag();
  void CancelDrag();
  int DragInsertSlot(POINT pt);
  void DrawDragIndicator(HDC dc);

  // Hover feedback (the Mac leans on hover: row washes, close affordances).
  // Which chrome element the cursor rests on, as element-kind + position in
  // its vector; repaints fire only when it changes.
  enum class HoverKind { kNone, kTab, kPin, kBookmark, kGroupHeader, kChip };
  HoverKind hover_kind_ = HoverKind::kNone;
  int hover_index_ = -1;
  void OnChromeMouseMove(POINT client_pt);
  void ClearHover();

  // The Space pips' hover card (the Mac SpacesStripView's delayed tooltip):
  // the dim wash is immediate (the hover state above); the info card — the
  // Space as a tinted pill, its bound profile, its Ctrl+N switch shortcut —
  // appears only after the cursor SETTLES on a pip, retargets instantly
  // while already up, and a click keeps it down until the pointer leaves
  // the clicked pip (so the card never blinks back right after a switch).
  std::string hover_card_space_;       // Card owner (empty = no card).
  std::string pending_card_space_;     // Scheduled, awaiting the delay.
  std::string card_suppressed_space_;  // Clicked; no card until hover-off.
  // Card content, resolved when the card arms — the profile name needs a
  // framework JSON round-trip, not per-paint work.
  std::wstring card_profile_;
  COLORREF card_accent_ = 0;  // The owner Space's theme accent.
  int card_shortcut_ = 0;     // 1-based Ctrl+N digit; 0 = past the ninth.
  void UpdateHoverCard();  // Re-aims card state at the hovered pip.
  void PromoteHoverCard();  // Delay elapsed: pending becomes shown.
  void DismissHoverCard(const std::string& suppress_space_id);
  void ResolveHoverCard(const std::string& space_id);  // Fills card_*.
  void DrawHoverCard(HDC dc, const Layout& layout, const PhiPalette& palette);
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
    // Split rows (the Mac's model). A pinned pair collapses into ONE cell
    // spanning two grid slots (this row is the leading member; the partner
    // row is consumed into the b_* fields). A bookmark split is one row
    // whose second pane rides url_b/title_b, drawn with both favicons and
    // a "primary • secondary" title.
    std::string url_b;
    std::wstring title_b;
    std::string partner_guid;  // Pinned pair only.
    std::vector<unsigned char> favicon_b;
    int favicon_b_width = 0;
    int favicon_b_height = 0;
  };
  std::vector<ShortcutRow> pinned_;
  std::vector<ShortcutRow> bookmarks_;
  // Counted before the layout is derived, because the bands they occupy push
  // everything below them down.
  int pinned_row_count_ = 0;
  int bookmark_row_count_ = 0;
  void RebuildShortcuts();

  // Space switch: the PAGE swaps instantly; the SIDEBAR BAND slides — and
  // natively, no macOS snapshot compositing: the leaving Space's laid-out
  // rows are RETAINED and painted live, shifted out at their frozen source
  // palette, while the entering rows paint shifted in at the ramping
  // palette. The chrome's colors ramp source→target on their own clock.
  struct BandModel {
    std::vector<TabRow> tabs;
    std::vector<ShortcutRow> pinned;
    std::vector<ShortcutRow> bookmarks;
    std::vector<GroupHeader> headers;
  };
  BandModel leaving_band_;
  // The page changes AFTER the band slide completes (the Mac reveals the
  // target on completion): the entering child stays hidden and the leaving
  // one visible until then.
  HWND swap_reveal_ = nullptr;   // Shown when the slide lands.
  HWND swap_conceal_ = nullptr;  // Hidden when the slide lands.
  void RevealSwappedContent();
  bool swap_forward_ = true;
  // The strip's selected pill slides from the leaving pip to the entering
  // one during a switch (the Mac's matchedGeometryEffect glass chip),
  // riding the band slide's clock. Empty when idle.
  RECT swap_chip_from_{};
  ULONGLONG swap_start_ = 0;  // Band slide clock; 0 = idle.
  PhiPalette swap_source_palette_{};
  ULONGLONG ramp_start_ = 0;  // Theme ramp clock; 0 = idle.
  double SwapEasedProgress() const;  // Slide progress; 1.0 when idle.
  double RampEasedProgress() const;  // Theme progress; 1.0 when idle.
  PhiPalette CurrentPalette() const; // Ramped while a switch is in flight.
  void ComposeFrame(HDC mem);        // Chrome + drag indicator.
  void BeginSpaceSwap(const std::string& entering_space_id);
  void CancelSpaceSwap();
  // The sidebar band (pinned grid through tab list): once normally, or the
  // two-pass slide while a switch animates.
  void PaintSidebarBand(HDC dc, const Layout& layout,
                        const PhiPalette& palette);
  void PaintBandOrSwipe(HDC dc, const Layout& layout,
                        const PhiPalette& palette);
  void SwapBandModel();  // leaving_band_ ↔ the live row vectors.

  // Stored-row bindings are DERIVED, not stored: a tab whose identity tag
  // equals a live pin/bookmark guid is that row's bound tab (the Mac's
  // customGuid model). Tags ride PhiTabData through session restore, so
  // bindings survive a restart. Bookmark bindings hide their tab from the
  // list only in vertical layouts.
  // The current index of the tab tagged `tag`, or -1 when none is.
  int IndexOfTagged(int64_t browser, const std::string& tag) const;
  // Reopens or refocuses a STORED split (a pinned pair or a split
  // bookmark): missing panes are created, tags applied, and the pair
  // married — the Mac's openPinnedSplit / openTwoURLsAsSplit.
  void OpenStoredSplit(const ShortcutRow& row, bool bookmark);

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
