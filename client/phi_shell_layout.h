// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SHELL_LAYOUT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SHELL_LAYOUT_H_

#include <string>

#include "phibrowser-win/client/phi_sidebar_layout.h"

// The shell window's chrome geometry as a VALUE (docs/architecture-review.md
// §3.1 principle 6, lane 1 D4, lane 3 §3.2): a pure function of the client
// rect, the DPI, the layout mode and a handful of counts, over plain ints.
// No HWND, no DPI queries, no font creation, no host round-trips — so the
// controller can cache one per turn and the Linux host can test it
// (test/model/shell_layout_test.cc). The controller converts to RECT.

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

// The design units the shell is laid out in (points at 100%), taken from
// the Mac client so the two look the same: WebContentConstant,
// SidebarHeaderView, SpacesStripView, PinnedTabLayout, TabStripMetrics and
// SidebarBottomBar. ONE definition, here, so the pure layout and the
// controller's paint code cannot drift apart.
namespace shell_metrics {
// The caption band: the OS title bar is removed (WM_NCCALCSIZE) and this row
// holds the MENU TITLES and the minimize / maximize / close buttons together
// — the menu bar lives on the caption row, not under it.
inline constexpr int kTitleBarHeight = 32;
// Comfortable's band is taller: it carries the tab strip as well as the
// window controls, and a tab is 34 tall over the strip's 6pt top inset.
inline constexpr int kTitleBarHeightTabs = 40;
inline constexpr int kCaptionButtonWidth = 46;  // The Win11 caption metric.
// WebContentConstant.edgesSpacing — THE horizontal gutter. There is no other
// sidebar padding on Mac; every row applies 8 on each side itself.
inline constexpr int kEdgeInset = 8;
inline constexpr int kNavButton = 24;       // Every header button is 24x24.
inline constexpr int kNavGroupSpacing = 1;  // back/forward/reload spacing.
// The caption band's trailing control cluster. The vertical layouts moved
// their header controls up here: search-tabs sits one 16pt gap before
// minimize (close enough to reach, far enough that a miss does not minimize
// the window), and performance leads it with back / forward / reload. Both
// modes park search-tabs at the SAME x, so switching layout does not move
// the button under the pointer.
inline constexpr int kBandTrailingGap = 16;  // cluster → the caption buttons.
inline constexpr int kBandGroupGap = 8;      // the nav trio → search-tabs.
// The hairline standing in that gap: short enough to read as a seam between
// the app's controls and the window's, not as a border round either.
inline constexpr int kBandDividerHeight = 16;
// The sidebar's own top inset: the Spaces strip starts at the gutter.
inline constexpr int kSpacesStripTop = 8;
inline constexpr int kSpacesStripHeight = 32;  // SpacesStripView.sidebarHeight
// The strip shares the sidebar's gutter, so the row of pip backgrounds spans
// EXACTLY the width of the address bar under it.
inline constexpr int kSpacesStripPad = kEdgeInset;
inline constexpr int kSpaceChip = 24;  // stripItemWidth/Height: a 24pt pip.
// The vertical layouts' cell: the painted card (the Mac's 24pt chip) plus a
// point of hit area on each side. Comfortable keeps the plain 24pt pip.
inline constexpr int kSpaceChipVertical = 26;
inline constexpr int kSpaceChipGap = 4;  // stripSpacing.
inline constexpr int kAddressBarTop = 52;  // spacesStrip.bottom + 12
inline constexpr int kAddressBarHeight = 32;
inline constexpr int kHeaderHeight = 88;    // 48 base + 32 strip + 8 gap
inline constexpr int kHeaderSpacer = 5;     // header → pinned band spacer.
inline constexpr int kFavoritesSpacer = 3;  // pinned band → tab list spacer.
inline constexpr int kTabRowHeight = 36;    // Row pitch adds kTabRowGap.
inline constexpr int kTabRowGap = 2;        // AppKit's default intercell.
inline constexpr int kBottomBarHeight = 24;  // SidebarBottomBarState row.
inline constexpr int kBottomSpacer = 8;
// The web content is an inset rounded card floating on the tint. Margins are
// the Mac's: LEFT FLUSH against the sidebar, 8 right/bottom, and a
// titleAwareArea breath on top.
inline constexpr int kContentMargin = 8;
// The breath between the caption band and the content card.
inline constexpr int kContentTopGap = 4;
inline constexpr int kFaviconSlot = 16;  // The 16pt slot in the row.
// The Mac's SeparatorItem row between the bookmark outline and the tab list.
inline constexpr int kBookmarkSeparatorHeight = 16;
inline constexpr int kNewTabRowHeight = 36;
// The empty pinned band's drop zone while a tab drag is in flight: the
// Mac's SidebarViewController.updateFavoriteHeight clamps a sub-20pt
// container to 100pt for the duration of the drag, so the "Drag tabs here"
// view has room under the cursor.
inline constexpr int kPinnedDropZoneHeight = 100;
// Pinned tabs: PinnedTabLayout — 45pt cells, 8pt gaps and insets.
inline constexpr int kPinnedCellHeight = 45;
inline constexpr int kPinnedGap = 8;
// Bands used by the two layouts that put navigation above the web content.
inline constexpr int kTopBarHeight = 40;  // WebContentConstant.headerHeight
// Balanced's top bar is slimmer: 24pt controls with 4 above and below, the
// same 32pt band the Spaces strip runs at, so the sidebar's strip and the
// card's address row sit on ONE line across the window.
inline constexpr int kTopBarHeightNav = 32;
inline constexpr int kBookmarkBarHeight = 32;  // The Mac BookmarkBar height.
inline constexpr int kTopBarLeading = 16;      // header leading padding.
inline constexpr int kComfortableLeading = 12;  // …and comfortable's.
inline constexpr int kOmniboxHeight = 28;  // comfortable's omnibox capsule.
inline constexpr int kTopBarSpacing = 8;   // header button spacing.
inline constexpr int kTopBarField = 24;    // balanced's field.
// The comfortable layout's tab strip shares the caption band with the
// window controls, Chrome-style, so the band is taller there and a tab runs
// from the strip's top inset all the way to the band's bottom edge.
inline constexpr int kHTabStripTop = 6;      // Strip top inset (8 − 2).
inline constexpr int kHTabStripLeading = 8;  // The Space pip → the first tab.
// The bottom bar's two capsules (Feedback bordered, Chat on the accent).
inline constexpr int kFeedbackIconWidth = 32;  // Icon-only variant.
inline constexpr int kChatWidth = 58;
inline constexpr int kPillHeight = 22;
}  // namespace shell_metrics

// Design units → physical pixels at `dpi` (96 = 100%). MulDiv's rounding.
int ShellScaled(int design_units, int dpi);

// Everything the layout depends on. Plain values, so the controller can
// compare one turn's inputs with the last and skip the recompute — the
// cache key (lane 3 §3.2: the layout used to be re-derived, with DPI
// queries and host round-trips, eight times per pointer move).
struct ShellLayoutInputs {
  PhiLayoutMode mode = PhiLayoutMode::kPerformance;
  int client_width = 0;
  int client_height = 0;
  int dpi = 96;
  int sidebar_dip = 240;  // Already clamped to the drag range.
  // Collapsed AND not being drawn for the floating panel: the content card
  // widens to the left margin and the sidebar's bands are empty.
  bool sidebar_hidden = false;
  // The hosted child owns every pixel; every chrome rect stays empty.
  bool content_fullscreen = false;
  bool bookmark_bar = false;      // The bar takes the content's first band.
  bool reader_offerable = false;  // The address capsule gets a reader slot.
  bool ai_enabled = false;        // The Chat / memory controls exist at all.
  int field_ext_count = 0;   // Pinned-extension icons inside the capsule.
  int pinned_row_count = 0;  // The pinned grid's rows (0 = no band).
  int bookmark_row_count = 0;  // Visible outline rows (0 = no separator).
  // A drag that could land in an EMPTY stored band is in flight: the band
  // reserves a drop zone (kPinnedDropZoneHeight / one outline row) so the
  // drag has somewhere to go. Ignored while the band has rows of its own.
  bool pinned_drop_zone = false;
  bool bookmark_drop_zone = false;
  // There is something to switch TO: more than one Space (agent pips
  // included), or a Space is being created. With a single Space the strip
  // row is pure noise (the Mac's updateSpaceSwitchVisibility): the vertical
  // layouts drop the row and reclaim its 40pt, comfortable drops the
  // compact pip.
  bool spaces_switch = true;

  friend bool operator==(const ShellLayoutInputs& a,
                         const ShellLayoutInputs& b) {
    return a.mode == b.mode && a.client_width == b.client_width &&
           a.client_height == b.client_height && a.dpi == b.dpi &&
           a.sidebar_dip == b.sidebar_dip &&
           a.sidebar_hidden == b.sidebar_hidden &&
           a.content_fullscreen == b.content_fullscreen &&
           a.bookmark_bar == b.bookmark_bar &&
           a.reader_offerable == b.reader_offerable &&
           a.ai_enabled == b.ai_enabled &&
           a.field_ext_count == b.field_ext_count &&
           a.pinned_row_count == b.pinned_row_count &&
           a.bookmark_row_count == b.bookmark_row_count &&
           a.pinned_drop_zone == b.pinned_drop_zone &&
           a.bookmark_drop_zone == b.bookmark_drop_zone &&
           a.spaces_switch == b.spaces_switch;
  }
  friend bool operator!=(const ShellLayoutInputs& a,
                         const ShellLayoutInputs& b) {
    return !(a == b);
  }
};

// Every rect the chrome needs, for one layout mode, in physical pixels.
struct ShellLayout {
  PhiLayoutMode mode = PhiLayoutMode::kPerformance;
  SidebarRect client;
  // The caption band: menu titles leading, then drag space, then the three
  // caption buttons. The OS caption is gone (WM_NCCALCSIZE) — this row IS
  // the title bar, so the menu shares it with minimize / maximize / close.
  SidebarRect title_bar;
  SidebarRect caption_min, caption_max, caption_close;
  // The x the menu titles may not cross: the caption band's trailing
  // control cluster (back / forward / reload and search-tabs in the
  // vertical layouts), or the caption buttons where there is no cluster.
  int band_limit = 0;
  SidebarRect sidebar;  // Empty in Comfortable.
  SidebarRect top_bar;  // The band above the content; empty in Performance.
  SidebarRect content;  // Where the reparented browser child goes.
  // The bookmark bar band, carved off the content's top when visible.
  SidebarRect bookmark_bar;
  SidebarRect search_tabs;
  // The sidebar toggle (the Mac SidebarHeaderView's leftSidebarToggle): the
  // caption band's LEADING control in the vertical layouts, at the window's
  // top-left corner, docked or collapsed alike — so a collapsed sidebar is
  // always one click from coming back. The Phi button follows it. Empty in
  // comfortable, which has no sidebar to toggle.
  SidebarRect sidebar_toggle;
  // The hairline closing the band's trailing cluster.
  SidebarRect band_divider;
  SidebarRect back, forward, reload;
  SidebarRect spaces_strip;  // The band the Space chips live in.
  SidebarRect url_field;
  SidebarRect favorites;  // Pinned-tab band; empty without pins.
  // The bookmark outline's band — its rows (or the reserved drop zone)
  // and the separator row under them — empty without bookmarks. A drag
  // over any of it lands in the outline; below the last row means the
  // root's end.
  SidebarRect bookmarks;
  // The 16pt separator row between the bookmark outline and "+ New Tab".
  SidebarRect bookmark_separator;
  SidebarRect new_tab_row;  // The sidebar's "+ New Tab" row.
  SidebarRect bottom_bar;
  // Bottom-bar buttons, the Mac's order: downloads and memory leading, the
  // Feedback and Chat capsules trailing.
  SidebarRect downloads, memory, feedback, chat;
  SidebarRect extensions;  // The puzzle button.
  // Comfortable's overflow menu (Chrome's ⋮): the toolbar's trailing button.
  SidebarRect overflow;
  // Comfortable's Phi button: the logo alone at the band's leading edge.
  SidebarRect phi_menu;
  // The globe leading comfortable's omnibox pill.
  SidebarRect page_icon;
  // SideAddressBar's in-field copy-link button (performance only).
  SidebarRect copy_link;
  // Reader View's entry, leading the copy-link button; empty unless offered.
  SidebarRect reader;
  // The pinned-extension icon strip inside the capsule; empty when nothing
  // is pinned or the icons would squeeze the text below 84pt.
  SidebarRect field_ext_strip;
  SidebarRect add_space;  // The strip's trailing "+" chip.
  // The pip's edge in points: the vertical layouts fill the strip band with
  // it, comfortable keeps the Mac's compact caption pip.
  int chip_size = 0;
  int chips_left = 0, chips_top = 0;
  int chips_limit = 0;  // Right edge chips may not cross.
  int tabs_left = 0, tabs_top = 0;
  int tabs_limit = 0;  // Bottom (vertical) or right (horizontal) edge.
  bool horizontal_tabs = false;
  // The Spaces switch exists in this layout (see ShellLayoutInputs).
  bool spaces_switch = true;
};

ShellLayout ComputeShellLayout(const ShellLayoutInputs& in);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SHELL_LAYOUT_H_
