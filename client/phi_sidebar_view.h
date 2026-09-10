// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_VIEW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_VIEW_H_

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_sidebar_model.h"

namespace phi_client {

// ---------------------------------------------------------------------------
// THE PURE HALF — no Win32, no framework, no paint. Two rules of the view
// that are decisions rather than drawing, so they are tested on the Linux
// host (test/model/sidebar_view_test.cc). A host translation unit defines
// PHI_SIDEBAR_VIEW_MODEL_ONLY before including this and gets this half
// alone; the client build sees the whole file.

// Tab ids most-recently-used first — the Ctrl+Tab switcher's order.
//
// RULES (the Mac's tabHistory): the ACTIVE tab is always the front; ids that
// have left the window fall out; everything else keeps its relative order,
// so cycling back through the switcher does not reshuffle the list behind
// it. A pane of a split counts as present — it is a live tab of this window
// even though its row is the merged one.
class PhiTabMru {
 public:
  PhiTabMru();
  ~PhiTabMru();

  const std::vector<TabId>& order() const { return order_; }
  bool empty() const { return order_.empty(); }

  // One rebuild's worth of bookkeeping: `live` is every tab id the window
  // still shows (both panes of a split included) and `active` is the one on
  // screen (invalid when the window shows none).
  void Sync(const std::vector<TabId>& live, TabId active) {
    std::erase_if(order_, [&](TabId id) {
      return std::find(live.begin(), live.end(), id) == live.end();
    });
    if (active.valid()) {
      std::erase(order_, active);
      order_.insert(order_.begin(), active);
    }
  }

 private:
  std::vector<TabId> order_;
};

// Inline, so the host test needs no .cc of its own (the rest of this file
// is Win32).
inline PhiTabMru::PhiTabMru() = default;
inline PhiTabMru::~PhiTabMru() = default;

// The bookmark cells an expanded chain reveals, in outline order (the Mac's
// NSOutlineView): the tree is depth-first, so a COLLAPSED folder hides the
// run of cells that follows it while their depth stays greater than its
// own. Returns positions in `cells`.
//
// A folder not in `expanded` is collapsed — the set is the disclosure state
// and nothing else, so a folder that goes away and comes back under the
// same guid comes back open, exactly as the Mac outline does.
// Inline, so the host test needs no .cc of its own (the rest of this file
// is Win32).
inline std::vector<size_t> PhiVisibleOutlineRows(
    const std::vector<SidebarCell>& cells,
    const std::set<std::string>& expanded) {
  // The tree is depth-first, so a collapsed folder's descendants are the
  // cells that follow it while their depth stays greater than its own.
  std::vector<size_t> visible;
  int collapsed_at = -1;
  for (size_t i = 0; i < cells.size(); ++i) {
    const SidebarCell& cell = cells[i];
    if (collapsed_at >= 0 && cell.depth > collapsed_at) {
      continue;
    }
    collapsed_at = -1;
    visible.push_back(i);
    if (cell.kind == SidebarStoredKind::kFolder &&
        !expanded.count(cell.guid)) {
      collapsed_at = cell.depth;
    }
  }
  return visible;
}

// The dragged row's current position in `rows` (-1 when it is gone). Drag
// state is keyed by SidebarKey, never by index: rows rebuild under a drag —
// a folder toggle, a notification arriving — and a key survives that where
// a position does not.
template <typename Rows>
int PhiIndexOfKey(const Rows& rows, const SidebarKey& key) {
  if (key.empty()) {
    return -1;
  }
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].key == key) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

}  // namespace phi_client

#if !defined(PHI_SIDEBAR_VIEW_MODEL_ONLY)

#include <windows.h>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_drag_image.h"
#include "phibrowser-win/client/phi_sidebar_actions.h"
#include "phibrowser-win/client/phi_sidebar_drag_session.h"
#include "phibrowser-win/client/phi_sidebar_drop.h"
#include "phibrowser-win/client/phi_sidebar_layout.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window.h"

namespace phi_client {

class AgentHost;
class ChromePrefsHost;
class PhiSidebarDragSession;
class PhiSpacesStrip;
class SidebarHost;
class SpacesHost;
class WindowHost;
class PhiShellWindow;
class PhiWindowModel;

// The sidebar's paint constants — design units (points at 100%), from the
// Mac client so the two look the same. The ones the GEOMETRY is built from
// live in phi_shell_layout.h (the pure, host-tested layout reads them);
// these are the paint-only ones, and the shell's remaining chrome takes
// the handful it shares by `using` rather than keeping a second copy
// (docs/architecture-review.md §3.9).
namespace sidebar_metrics {

// 36pt row -> 32pt pill (the vertical inset).
inline constexpr int kRowPad = 2;
inline constexpr int kCornerRadius = 8;  // The pill radius, everywhere.
inline constexpr int kFaviconSize = 14;  // The 16pt slot holds a 14pt icon.
inline constexpr int kBookmarkIndent = 10;  // SideBarOutlineView.indentation.
inline constexpr int kFolderIconSize = 20;  // BookmarkFaviconView.folderSize.
// Pinned tabs: PinnedTabLayout — column limit by width breakpoint (3 at
// this sidebar width), 53pt minimum width.
inline constexpr int kPinnedMinWidth = 53;
inline constexpr int kPinnedFavicon = 18;
// The horizontal strip's pinned section (TabStripMetrics.PinnedTab): fixed
// 28pt square cells, 2pt spacing, leading the normal tabs.
inline constexpr int kStripPinSize = 28;
inline constexpr int kStripPinGap = 2;
inline constexpr int kStripPinToTabs = 6;
inline constexpr int kStripPinFavicon = 16;
// Tab groups: the Mac's 32pt TabGroupHeaderView on TabGroupCellView's card —
// the hover wash under a 1pt border hairline, radius 8; the group colour
// lives only in the header's dot. Members sit 4pt in from the header and
// the card's bottom (innerTableTop/BottomInset), 8pt in from its leading
// edge (the member cell's own edgesSpacing) and 4pt from its trailing one.
inline constexpr int kGroupHeaderHeight = 32;
inline constexpr int kGroupInnerInset = 4;
inline constexpr int kGroupMemberLeadInset = 8;
inline constexpr int kGroupMemberTrailInset = 4;
// The comfortable layout's tabs (the strip's band metrics are in
// phi_shell_layout.h): a tab runs from the strip's top inset all the way to
// the band's bottom edge — where the active one merges into the toolbar.
inline constexpr int kHTabHeight = 34;   // kTitleBarHeightTabs - kHTabStripTop
inline constexpr int kHTabWidth = 200;   // Tab.idealWidth (a resting tab)
inline constexpr int kHTabActiveWidth = 240;  // The active tab takes more room.
// Favicon, a breath, and the close box.
inline constexpr int kHTabMinWidth = 56;
inline constexpr int kHTabGap = 0;        // Tabs abut; a hairline divides them.
inline constexpr int kCloseBoxSize = 24;  // UnifiedTabCloseButton frame.

}  // namespace sidebar_metrics

// Chromium's TabGroupColorId palette (the Mac's GroupColor set is the same
// nine); the name keys the localized colour label
// (tabs.tabGroup.colorName.*) that titles an unnamed group, and the group
// header menu offers the same nine.
struct PhiGroupColor {
  const char* name;
  COLORREF color;
};
inline constexpr std::array<PhiGroupColor, 9> kPhiGroupColors = {{
    {"grey", RGB(0x80, 0x86, 0x8B)},
    {"blue", RGB(0x1A, 0x73, 0xE8)},
    {"red", RGB(0xD9, 0x30, 0x25)},
    {"yellow", RGB(0xF9, 0xAB, 0x00)},
    {"green", RGB(0x18, 0x80, 0x38)},
    {"pink", RGB(0xD0, 0x18, 0x84)},
    {"purple", RGB(0xA1, 0x42, 0xF4)},
    {"cyan", RGB(0x00, 0x7B, 0x83)},
    {"orange", RGB(0xFA, 0x90, 0x3E)},
}};
// kPhiGroupColors follows phi_framework::TabGroupColor's declaration order —
// that is what makes the enum an index into it.
inline const PhiGroupColor& PhiGroupColorFor(
    phi_framework::TabGroupColor color) {
  const size_t index = static_cast<size_t>(color);
  return index < kPhiGroupColors.size() ? kPhiGroupColors[index]
                                        : kPhiGroupColors[0];
}

// ---------------------------------------------------------------------------
// The shared antialiased shapes. EVERY curve in the client goes through
// phi_dwrite (see the fork's AA rule); these are the four call shapes the
// sidebar and the shell's remaining chrome both draw, spelled once so the
// two cannot drift apart on a radius.

// A filled rounded rect, the Mac client's selection/field shape.
void PhiFillRounded(HDC dc, const RECT& r, COLORREF fill, int radius);
// A fill rounded on its TOP corners only, square along the bottom — the
// comfortable tab's shape, which has to sit flush on the toolbar it merges
// into.
void PhiFillRoundedTop(HDC dc, const RECT& r, COLORREF fill, int radius);
void PhiStrokeRounded(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
                      int radius);
void PhiDrawGlyph(HDC dc, const RECT& r, const wchar_t* glyph, COLORREF ink);
// The same, for an MDL2 ICON: rasterized by DirectWrite at `pixel_size` and
// alpha-blended over what is already painted, with the GDI path as fallback.
void PhiDrawGlyphIcon(HDC dc, const RECT& r, const wchar_t* glyph,
                      int pixel_size, COLORREF ink);
// Reader View's icon: SF Symbol `doc.plaintext`, drawn rather than
// substituted (Segoe MDL2's nearest neighbour is a different picture).
void PhiDrawReaderGlyph(HDC dc, const RECT& button, COLORREF ink, UINT dpi);
// `over` at `alpha`/255 on `base` — the Mac's translucent overlays, opaque.
COLORREF PhiBlendOver(COLORREF over, COLORREF base, int alpha);

// ---------------------------------------------------------------------------
// Readers over a WindowSnapshot — the window model's copy or a freshly
// pulled one alike. The split-drop paths map ids to tags and back several
// times inside a single drop, so they read the window ONCE and walk these
// instead of asking the framework again.
const phi_framework::TabSnapshot* PhiFindTabIn(
    const phi_framework::WindowSnapshot& snapshot, TabId id);
const phi_framework::TabSnapshot* PhiFindTagIn(
    const phi_framework::WindowSnapshot& snapshot, const std::string& tag);
const phi_framework::SplitSnapshot* PhiFindSplitIn(
    const phi_framework::WindowSnapshot& snapshot, const std::string& split_id);
// The live tab bound to a stored row, or `fallback` when the row has none.
// A pinned tab or bookmark is OPENED by the very press that arms its drag,
// so the view row the drag was armed from can still say "closed" when the
// drop lands.
TabId PhiLiveTabForRow(const phi_framework::WindowSnapshot& snapshot,
                       const std::string& guid,
                       TabId fallback);
// The framework names surfaces (a reader page recognised from what it
// shows, committed or not); the client parses no reader URLs.
SidebarSurface PhiSurfaceOf(const phi_framework::TabSnapshot& tab);
RECT PhiToRect(const SidebarRect& r);

// ---------------------------------------------------------------------------
// Which chrome element the pointer rests on. The SHELL owns the cursor —
// one hover across the caption band, the Spaces strip, the nav cluster and
// the sidebar, so that resting on one band takes the wash off the others —
// and the sidebar answers the hit tests and the wash rects for its own
// rows (PhiSidebarView::RowHoverRect).
enum class PhiChromeHover {
  kNone,
  kTab,
  kPin,
  kBookmark,
  kGroupHeader,
  kChip,
  // The toolbar's back / forward / reload / search buttons — the index is
  // their position in the shell's own paint table.
  kNav,
  kNewTabRow,
  kFieldExt,
  // The Spaces strip's trailing slot — the "+" or, on overflow, the "..."
  // switcher standing in for it.
  kAddSpace,
};

struct PhiSidebarRects {
  RECT sidebar{};    // The whole band. Empty in Comfortable.
  RECT title_bar{};  // Where the horizontal strip's rows live instead.
  RECT content{};    // The page: a drag over it is a split drop.
  RECT favorites{};  // The pinned grid (or its reserved drop zone).
  // The bookmark outline's band: its rows (or the reserved drop zone) and
  // the separator row under them. Empty without bookmarks.
  RECT bookmarks{};
  RECT bookmark_separator{};
  RECT new_tab_row{};
  RECT bottom_bar{};
  RECT spaces_strip{};
  RECT search_tabs{};  // Comfortable's "+" may not cross it.
  int tabs_left = 0, tabs_top = 0;
  int tabs_limit = 0;  // Bottom (vertical) or right (horizontal) edge.
  bool horizontal_tabs = false;
};

// One rendered tab row: what to draw and what a click at that row does.
struct TabRow {
  SidebarKey key;  // The tree node this row paints (kTab or kSplit).
  int index = 0;
  TabId id;  // Session id — stable while indices shift; pins bind to it.
  // The tab's favicon, copied out of the framework's scratch buffer (which
  // only survives until its next call).
  std::vector<unsigned char> favicon;
  int favicon_width = 0;
  int favicon_height = 0;
  bool active = false;
  std::wstring title;
  // The page address, as the hover preview card's second line wants it.
  std::wstring url;
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
  // DORMANT: holds navigation history it has not loaded yet (restored,
  // duplicated, or discarded). Phi does not load restored background tabs,
  // so this is a steady state for most of them — the row is drawn dimmed.
  bool unloaded = false;
  int b_index = -1;  // -1: not a merged row.
  TabId b_id;        // Pane B's session id, for its hover snapshot.
  bool b_active = false;
  std::wstring b_title;
  std::wstring b_url;
  std::vector<unsigned char> b_favicon;
  int b_favicon_width = 0;
  int b_favicon_height = 0;
  // A Peek or Reader View attached to this row's tab (either pane): the
  // trailing indicator, whose minus closes it (docs/sidebar-architecture
  // §4.6). While one is attached the row's own close box is withheld —
  // the indicator is the one close affordance, as on the Mac.
  SidebarSurface attached_surface = SidebarSurface::kContent;
  RECT bounds{};       // Row rect in client coords.
  RECT pane_b{};       // Right half of a merged row (activates pane B).
  RECT close_box{};    // The row's close affordance.
  RECT surface_box{};  // The attached-surface indicator.
};

// Sidebar tab-group headers (TabGroupHeaderView): chevron + color dot +
// title on the group's 18% color tint, clicking collapses/expands.
struct GroupHeader {
  std::string id;
  std::wstring name;   // The stored title; empty for an unnamed group.
  std::wstring title;  // What the row shows: the name, or "Grey · 2 tabs".
  COLORREF color = 0;
  bool collapsed = false;
  RECT bounds{};     // The header row (the collapse toggle).
  RECT container{};  // Header plus visible members — the tinted card.
};

// Pinned tabs (the band under the header) and bookmarks (the section above
// the open tabs) — both come from the client's own store, as on Mac.
struct ShortcutRow {
  SidebarKey key;  // kCell or kFolder.
  std::string guid;  // The store row, so the context menu can remove it.
  std::string url;
  std::wstring title;
  bool folder = false;
  // Folders only: the catalog symbol drawn in the row's icon slot.
  std::string icon;
  // Split rows: the divider axis the pair reopens with.
  std::string layout;
  // Bookmark outline (the Mac's NSOutlineView): an expanded folder splices
  // its children in below itself, one indent step deeper. `parent_guid`
  // names the folder a row sits in (empty = the root).
  int depth = 0;
  bool expanded = false;
  std::string parent_guid;
  // A pinned tab bound to a LIVE tab (openOrFocusPinnedTab): the tab lives
  // in the pin's cell — never in the tab list — and the active one gets the
  // Mac's selected wash + 2pt accent ring.
  bool open = false;
  bool active = false;
  // A Peek or Reader View attached to a bound pane — the indicator (a
  // trailing slot on a bookmark row, a corner badge on a pinned cell).
  SidebarSurface attached_surface = SidebarSurface::kContent;
  RECT surface_box{};
  // The bound tab's session id while `open` — what the hover preview asks
  // the framework for a snapshot of. A pinned PAIR binds both: `tab_id` is
  // this row's own tab, `tab_id_b` its partner's.
  TabId tab_id;
  TabId tab_id_b;
  // DORMANT bound panes (the tab row's `unloaded`): the pane's live tab
  // holds history it has not loaded — restored with the session and never
  // visited, or discarded. Its favicon, and the cell's open dot, draw at
  // the Mac's 30% (TabFaviconPresentation.reclaimedOpacity) so an open
  // cell whose page is not really there reads apart from a loaded one. A
  // closed cell has no tab and so is never dormant.
  bool unloaded = false;
  bool unloaded_b = false;
  RECT bounds{};
  // The favicon stored with the row (a pinned tab has no live tab to ask).
  std::vector<unsigned char> favicon;
  int favicon_width = 0;
  int favicon_height = 0;
  // Split cell: one row whose second pane rides url_b/title_b/favicon_b
  // (a pinned split spans two grid slots; a bookmark split draws both
  // favicons and a "primary • secondary" title). Pane B's live tab is
  // tagged TagFor(guid, true) and lands in tab_id_b.
  std::string url_b;
  std::wstring title_b;
  std::vector<unsigned char> favicon_b;
  int favicon_b_width = 0;
  int favicon_b_height = 0;
};

// What the sidebar asks of its shell (docs/architecture-review.md §3.5).
// Implemented PRIVATELY by the shell, so none of this widens its public
// surface.
class PhiSidebarShell {
 public:
  virtual HWND SidebarShellWindow() const = 0;
  // The controller the split-drop overlay and the tear-off hit test are
  // addressed against, as with the native pages and the Spaces strip.
  virtual PhiShellWindow* SidebarOwner() = 0;
  virtual int SidebarScaled(int design_units) const = 0;
  virtual UINT SidebarDpi() const = 0;
  // The sidebar's width in DIP — the shell owns the resize band.
  virtual int SidebarWidthDip() const = 0;
  // The shell's per-turn Layout, sliced. Deriving it may run ComputeLayout,
  // so the callers hold the result rather than asking per row.
  virtual PhiSidebarRects SidebarRects() = 0;
  // The two band sizes the LAYOUT is derived from, counted from the tree
  // before any row is placed.
  virtual void SidebarSetRowCounts(int pinned_rows, int bookmark_rows) = 0;
  // A drag that could land in an EMPTY stored band is in flight (or has
  // ended): the layout reserves (releases) the band's drop zone. The
  // caller schedules the rebuild that re-places the rows under it.
  virtual void SidebarSetDropZones(bool pinned, bool bookmark) = 0;
  // The bookmark-bar and reader inputs the layout caches, re-read once per
  // rebuild after the active tab is known.
  virtual void SidebarRefreshLayoutInputs() = 0;
  // The ramped palette the whole chrome is painting with right now.
  virtual PhiPalette SidebarPalette() const = 0;
  // The browser this slot SHOWS; invalid between the last browser going and
  // teardown.
  virtual BrowserId SidebarActiveBrowser() const = 0;
  // The slot's active Space id — an agent Space hosted for a watch shows
  // no pinned tabs or bookmarks, like an agent window's own sidebar.
  virtual std::string SidebarActiveSpace() const = 0;
  // The framework's CURRENT state for `browser`, pulled typed — for the
  // gesture paths that just mutated the strip and act on the result inside
  // the same turn. Never for rows or paint: those read the window model.
  virtual bool SidebarPullSnapshot(
      BrowserId browser,
      phi_framework::WindowSnapshot* out) const = 0;
  // True when the split holding `tab_id` stacks its panes.
  virtual bool SidebarSplitIsStacked(TabId tab_id) const = 0;
  // The chrome fonts the rows draw with, all built and owned by the shell.
  virtual HFONT SidebarGlyphFont() const = 0;
  virtual HFONT SidebarSemiboldFont() const = 0;
  // The rows' text face (the DC's font while the band paints), for a
  // painter drawing a row off-screen.
  virtual HFONT SidebarTextFont() const = 0;
  // The hover cursor (see PhiChromeHover): which element, and where in its
  // own vector.
  virtual PhiChromeHover SidebarHoverKind() const = 0;
  virtual int SidebarHoverIndex() const = 0;
  // The Spaces strip, whose pips ride the same drag machinery as the rows.
  virtual PhiSpacesStrip* SidebarSpacesStrip() = 0;
  // THE refresh entry point: marks the rows dirty and posts one coalesced
  // rebuild for this turn.
  virtual void SidebarScheduleRefresh() = 0;
  // A rebuild noticed the active tab crossing a native-page route.
  virtual void SidebarSyncNativePages() = 0;
  // The chrome as it paints RIGHT NOW, into a bitmap — what the drag image
  // lifts the dragged element's pixels out of.
  virtual PhiDragBitmap SidebarCaptureChrome(
      const RECT& client_rect) = 0;
  // The reparented browser child, whose on-screen pixels are the drag
  // card's page.
  virtual HWND SidebarContentWindow() const = 0;
  // The New Tab entry point (the "+ New Tab" row honours the "New tab
  // behavior" pref, which the shell owns).
  virtual void SidebarStartNewTab() = 0;
  // The rows, as accessibility sees them (docs/architecture-review.md §3.6,
  // ranked finding 31): the shell publishes them on its window.
  virtual void SidebarPublishHits(std::vector<PhiHit> hits) = 0;

 protected:
  // Non-virtual and protected: the shell OWNS the sidebar, never the other
  // way round, so nothing ever deletes through this pointer.
  ~PhiSidebarShell() = default;
};

// THE sidebar (docs/architecture-review.md §3.5; lane 1 clusters H and J
// plus the sidebar half of Q): the row pipeline SidebarInputs ->
// BuildSidebar -> LayoutSidebar -> rows, the paint of that band, its hit
// tests, its scrolling, the expanded-bookmark set and the MRU order. Every
// mutation it resolves leaves as a SidebarCommand.
//
// INVARIANTS
//  * The row vectors are written in ONE function — Rebuild() — on the
//    posted rebuild turn. Paint, input and menus READ them; none of them
//    writes one (docs/sidebar-architecture.md §2.7, §7).
//  * A hit test answers a SidebarKey, never an index: rows rebuild under a
//    drag (a folder toggle, a notification arriving) and a key survives
//    that where a position does not.
//  * "Which window shows tab X" is answered from the TREE, owner_of
//    included — a tab hidden in a pinned or bookmark cell is still shown by
//    this window (lane 1 §2.1 item 4).
//  * The drag session exists only between ArmDrag and commit or cancel; the
//    view owns it, so a sidebar going away takes the drag image and the
//    split-drop overlay with it.
class PhiSidebarView {
 public:
  // The narrow services the view calls (docs/architecture-review.md §3.5):
  // its own store rows and the sidebar mutator, the palette the band paints
  // with, the client-wide layout mode, the agent mask badges — plus the
  // window host it does not call itself and hands to the drag session for
  // tear-off and cross-window drops. Any of them may be null.
  PhiSidebarView(PhiSidebarShell* shell,
                 SlotId slot,
                 phi_framework::PhiFramework* framework,
                 SidebarHost* sidebar_host,
                 SpacesHost* spaces_host,
                 ChromePrefsHost* prefs_host,
                 AgentHost* agent_host,
                 WindowHost* window_host,
                 const PhiWindowModel* model,
                 bool agent,
                 bool incognito);
  ~PhiSidebarView();

  PhiSidebarView(const PhiSidebarView&) = delete;
  PhiSidebarView& operator=(const PhiSidebarView&) = delete;

  using HoverKind = PhiChromeHover;
  using SlideVec = PhiSlideVec;

  // --- The row pipeline ---------------------------------------------------
  // THE writer of the row vectors. Runs on the shell's posted rebuild turn.
  void Rebuild();
  const SidebarTree& tree() const { return sidebar_tree_; }
  const std::vector<TabRow>& tabs() const { return tabs_; }
  const std::vector<ShortcutRow>& pinned() const { return pinned_; }
  const std::vector<ShortcutRow>& bookmarks() const { return bookmarks_; }
  // No stored bands at all: incognito, an agent Space. A drag reserves no
  // drop zone here.
  bool stored_bands_isolated() const { return stored_bands_isolated_; }
  const std::vector<GroupHeader>& group_headers() const {
    return group_headers_;
  }
  // Comfortable's "+" in the caption band, trailing the last tab.
  const RECT& add_tab_button() const { return add_tab_button_; }
  // The ACTIVE tab, after the tree's surface -> origin redirection, and its
  // URL — what the omnibox, the reader affordance and the native-page route
  // all read.
  TabId active_tab_id() const { return active_tab_id_; }
  const std::string& active_tab_url() const { return active_tab_url_; }
  // True when `tab_id` is one this window SHOWS: a content row, or a tab
  // hidden inside one of its stored cells (the tree's owner_of).
  bool ShowsTab(TabId tab_id) const;
  // Tab ids most-recently-used first — the Ctrl+Tab switcher's order.
  const std::vector<TabId>& mru() const { return tab_mru_.order(); }
  // The placed rows (keys + client rects) for the agent dialect's
  // window.layout, so a box probe can aim a real drag without pixels.
  base::ListValue LayoutRowsJson() const;
  // The rows as assistive technology sees them, rebuilt with them.
  const std::vector<PhiHit>& hits() const { return hits_; }

  // --- View state ---------------------------------------------------------
  // Folders the user has disclosed in the outline (session-local view
  // state, like the Mac outline's). Returns true when the folder ended
  // expanded.
  bool ToggleExpanded(const std::string& guid);
  void Expand(const std::string& guid) { expanded_bookmarks_.insert(guid); }
  bool IsExpanded(const std::string& guid) const {
    return expanded_bookmarks_.count(guid) > 0;
  }
  // The wheel over the sidebar; true when consumed (vertical layouts only).
  bool OnWheel(POINT client_pt, int wheel_delta);

  // --- Gestures -----------------------------------------------------------
  // The stored bands (the pinned grid, the bookmark outline) and the
  // content rows (group headers, tab rows). Two entry points because the
  // shell's own bands are hit-tested between them, in the order the pixels
  // stack. True when the point belonged to a row.
  bool OnStoredRowClick(POINT client_pt);
  bool OnContentRowClick(POINT client_pt);
  // Middle-click closes a tab row (the Mac tab list's otherMouseDown);
  // pins and bookmarks are persistent and ignore it.
  bool OnMiddleClick(POINT client_pt);
  // Sends a command to the host against the current tree; the host refreshes.
  bool Apply(const SidebarCommand& command);
  // Click on a pin or a sidebar bookmark row: focus the bound live tab if it
  // still exists, otherwise open the row's url as a new bound tab.
  void OpenOrFocusPinned(const std::string& guid, const std::string& url);
  // Splits `primary` with `secondary`, after dropping any pin or bookmark
  // binding on EITHER pane (a split never lives in a stored cell).
  bool SplitTabsNormalized(BrowserId browser, TabId primary, TabId secondary);
  // Drops a tab's pin/bookmark binding: the stored row stays put as an
  // unopened one, the tab joins the plain list.
  void UnbindStoredTab(BrowserId browser, TabId tab_id);
  // Pairs two live tabs into a split with `primary_id` leading. Returns the
  // split id.
  std::string FormSplitOrdered(BrowserId browser,
                               TabId primary_id,
                               TabId secondary_id,
                               const char* layout);

  // --- Drag (PhiSidebarDragSession) ---------------------------------------
  // Arms a drag on `key`. The session lives from here to the commit or the
  // cancel and no longer.
  void ArmDrag(const SidebarKey& key, POINT pt);
  // The tab row a press armed WITHOUT activating it: a plain click
  // activates on release, a drag never does.
  void NoteDragClickTab(TabId tab_id);
  void UpdateDrag(POINT pt);
  void CommitDrag();
  // An external cancel (the capture was taken away, a panel press).
  void AbortDrag();
  bool drag_armed() const { return drag_ != nullptr; }
  bool drag_active() const;
  PhiSidebarDragSession* drag() const { return drag_.get(); }
  // The reorder slide's clock tick; false once the leg has settled.
  bool OnSlideTimer();
  // The 2px accent insertion line, over the composed frame.
  void DrawDragIndicator(HDC dc);

  // --- Paint --------------------------------------------------------------
  // The band (pinned grid through tab list): once normally, or the two-pass
  // slide while a Space switch animates.
  void Paint(HDC dc, const PhiSidebarRects& layout, const PhiPalette& palette);
  // The wash rect of one of the sidebar's own hovered rows, empty for any
  // other kind — so a hover change invalidates the two elements, not the
  // whole shell.
  RECT RowHoverRect(HoverKind kind, int index) const;
  // Draws a favicon's BGRA pixels into `dest`, composited over `background`
  // (GDI has no alpha blit without msimg32, and we always know what is
  // behind the icon).
  // `opacity` < 1 draws the icon faded over `background` (a reclaimed tab).
  void DrawFavicon(HDC dc, const RECT& dest, const unsigned char* bgra,
                   int width, int height, COLORREF background,
                   float opacity = 1.0f);
  // A bookmark folder's icon slot: the catalog symbol it wears, or the
  // plain folder glyph for the catalog's "default".
  void DrawFolderIcon(HDC dc, const RECT& slot, const std::string& icon_id,
                      COLORREF ink, bool expanded);
  // The attached-surface indicator: a plate with the kind's glyph, or the
  // minus that closes it while its row is hovered.
  // An empty stored band's drop zone while a drag is in flight (the Mac's
  // PinnedTabViewController.emptyView): a dashed pill carrying a glyph and
  // a hint, washed with the accent while the drag hovers it. `stacked`
  // puts the glyph above the hint (the tall pinned zone); otherwise the
  // two sit in a row.
  void PaintDropZone(HDC dc, const RECT& zone, bool hot, const wchar_t* glyph,
                     int glyph_dip, const wchar_t* hint, bool stacked,
                     const PhiPalette& palette);
  void DrawSurfaceBadge(HDC dc, const RECT& box, SidebarSurface kind,
                        bool hovered, COLORREF ground);
  // leaving_band_ <-> the live row vectors. The Space slide paints both
  // models, so the strip exchanges them twice per frame.
  void SwapBandModel();
  void DropLeavingBand() { leaving_band_ = BandModel(); }

  // --- Geometry the drag session shares -----------------------------------
  // The pinned grid's placement walk over arbitrary cell widths (1 or 2
  // slots) — layout uses it for the real cells, the drag preview with a
  // phantom cell spliced in at the insertion slot.
  // `rows`, when given, receives the grid's row count for `units`.
  std::vector<RECT> PinnedGridWalk(const std::vector<int>& units,
                                   const PhiSidebarRects& layout,
                                   int* rows = nullptr) const;
  // The REAL grid's row count — what the layout's pinned band is sized to.
  int pinned_grid_rows() const { return pinned_grid_rows_; }
  // The bookmark cells an expanded chain reveals, in outline order.
  std::vector<const SidebarCell*> VisibleBookmarkCells() const;
  // The sections as the pure drop resolvers see them.
  std::vector<DropRow> TabDropRows() const;
  std::vector<DropRow> PinnedDropRows() const;
  std::vector<DropRow> BookmarkDropRows() const;
  // The shell, for the session that shares it.
  PhiSidebarShell* shell() const { return shell_; }
  phi_framework::PhiFramework* framework() const { return framework_; }
  SidebarHost* sidebar_host() const { return sidebar_host_; }
  // What the drag session it owns takes: the Space list for the pip
  // reorder and its palette, and the window set for tear-off and the
  // cross-window drop.
  SpacesHost* spaces_host() const { return spaces_host_; }
  WindowHost* window_host() const { return window_host_; }
  SlotId slot() const { return slot_; }
  bool incognito() const { return incognito_; }
  // The agent holds this (agent) window: the rows' close boxes are inert.
  void set_agent_holds_input(bool holds) { agent_holds_input_ = holds; }

 private:
  // Every constant in this class is a DESIGN UNIT — a pixel at 100%
  // scaling; nothing reaches the screen without going through here.
  int Scaled(int design_units) const {
    return shell_->SidebarScaled(design_units);
  }
  UINT dpi() const { return shell_->SidebarDpi(); }
  int sidebar_dip() const { return shell_->SidebarWidthDip(); }

  // Pinned cells and the bookmark outline as paint rows, from the tree's
  // cells (order, depth, bindings) and the store's views (titles, urls,
  // favicons), placed within `layout`. The tree must be built first.
  void RebuildShortcuts(const PhiSidebarRects& layout,
                        const phi_framework::WindowSnapshot& snapshot);
  // The pinned grid's metrics for LayoutPinnedGrid, from the sidebar width
  // and `layout.favorites`.
  PinnedGridMetrics GridMetrics(const PhiSidebarRects& layout) const;
  // Reopens or refocuses a STORED split (a pinned pair or a split
  // bookmark): missing panes are created, tags applied, and the pair
  // married.
  void OpenStoredSplit(const ShortcutRow& row);
  // The band, once, at one palette.
  void PaintBand(HDC dc, const PhiSidebarRects& layout,
                 const PhiPalette& palette);
  // The dragged element's slide offsets, zero with no drag in flight.
  PhiSlideVec RowSlide(const std::string& guid) const;
  double TabSlide(TabId tab_id) const;
  HWND shell_hwnd() const;
  // Publishes the rows into the shell's hit list — one hit per row, keyed
  // so a provider handed out before a rebuild finds its row again.
  void PublishHits(const PhiSidebarRects& layout);

  // Owns this view and outlives it.
  RAW_PTR_EXCLUSION PhiSidebarShell* const shell_;
  const SlotId slot_;
  // Owned by the framework (a singleton in chrome.dll); non-owning, and
  // across a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* const framework_;
  // The coordinator's services (§3.5). It outlives every shell, so these
  // are non-owning and never rebound.
  RAW_PTR_EXCLUSION SidebarHost* const sidebar_host_;
  RAW_PTR_EXCLUSION SpacesHost* const spaces_host_;
  RAW_PTR_EXCLUSION ChromePrefsHost* const prefs_host_;
  RAW_PTR_EXCLUSION AgentHost* const agent_host_;
  RAW_PTR_EXCLUSION WindowHost* const window_host_;
  // The coordinator's window model: the one source for rows, splits and
  // groups. It owns the instance and outlives every shell.
  RAW_PTR_EXCLUSION const PhiWindowModel* const model_;
  const bool incognito_;
  // An agent Space's sidebar: isolated like incognito's — no pinned tabs,
  // no bookmarks persist there (the Mac's BrowserState agent-Space rule).
  const bool agent_;
  bool agent_holds_input_ = false;

  std::vector<TabRow> tabs_;
  // The model — what every live tab IS (docs/sidebar-architecture.md §4.5).
  // tabs_, pinned_ and bookmarks_ are paint/hit-test caches derived from it
  // in Rebuild(); nothing decides bindings or split rows anywhere else.
  SidebarTree sidebar_tree_;
  RECT add_tab_button_{};
  std::vector<GroupHeader> group_headers_;
  std::vector<ShortcutRow> pinned_;
  std::vector<ShortcutRow> bookmarks_;
  // The active tab's URL (captured by Rebuild) and its session id.
  std::string active_tab_url_;
  TabId active_tab_id_;
  // Session-local view state: the disclosed folders, the list's scroll.
  std::set<std::string> expanded_bookmarks_;
  int scroll_offset_ = 0;
  int tabs_scroll_max_ = 0;
  // The pinned grid's ROW count, counted from the tree before the layout is
  // derived (the band it occupies pushes everything below it down) and read
  // back when the outline is placed under it.
  int pinned_grid_rows_ = 0;
  bool stored_bands_isolated_ = false;
  // Tab ids of the ACTIVE browser, most-recently-used first.
  PhiTabMru tab_mru_;
  // The rows as accessibility and tooltips read them.
  std::vector<PhiHit> hits_;

  // Space switch: the PAGE swaps instantly; the SIDEBAR BAND slides — and
  // natively, no macOS snapshot compositing: the leaving Space's laid-out
  // rows are RETAINED and painted live, shifted out at their frozen source
  // palette, while the entering rows paint shifted in at the ramping
  // palette.
  struct BandModel {
    std::vector<TabRow> tabs;
    std::vector<ShortcutRow> pinned;
    std::vector<ShortcutRow> bookmarks;
    std::vector<GroupHeader> headers;
  };
  BandModel leaving_band_;

  // Live only between ArmDrag and the commit or cancel (§3.5): its
  // destructor IS AbortDrag, and it owns the drag image and the split-drop
  // overlay, so nothing tears those down by hand.
  std::unique_ptr<PhiSidebarDragSession> drag_;
};

}  // namespace phi_client

#endif  // !defined(PHI_SIDEBAR_VIEW_MODEL_ONLY)

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_VIEW_H_
