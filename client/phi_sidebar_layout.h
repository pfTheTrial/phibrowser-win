// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_LAYOUT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_LAYOUT_H_

#include <vector>

// Sidebar geometry as pure functions — docs/sidebar-architecture.md §6.
// Everything here is integer pixel math over already-scaled metrics: no
// HWND, no DPI lookups, no Win32 types, so it compiles and is tested on the
// Linux host (test/model/run.sh). The controller converts to RECT.

namespace phi_client {

struct SidebarRect {
  int left = 0, top = 0, right = 0, bottom = 0;
  bool empty() const { return right <= left || bottom <= top; }
  friend bool operator==(const SidebarRect& a, const SidebarRect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right &&
           a.bottom == b.bottom;
  }
};

// ---- The pinned grid (the Mac's PinnedTabLayout) ---------------------------
// Cells are 45pt high with 8pt gaps and insets. The width breakpoint
// (decisionWidth = sidebar − 8: under 236 → 3, under 294 → 4, under 352 → 5,
// else 6) is only the column LIMIT — the column count is min(limit, units),
// shrunk while a cell would dip under the 53pt minimum, and cells STRETCH to
// fill the row: a lone pin spans the sidebar, two pins split it in half. A
// split cell spans two units (clamped to the column count when narrow); a
// cell that would overflow its row wraps whole.
struct PinnedGridMetrics {
  // Column solving is in DIPs, as the Mac's breakpoints are.
  int sidebar_dip = 0;
  int edge_inset_dip = 0;
  int gap_dip = 0;
  int min_cell_width_dip = 0;
  // Placement is in pixels.
  int origin_x = 0;   // favorites.left + scaled edge inset.
  int origin_y = 0;   // favorites.top.
  int sidebar_px = 0;
  int edge_inset_px = 0;
  int gap_px = 0;
  int cell_height_px = 0;
};
struct PinnedGridPlacement {
  int columns = 1;
  int rows = 0;  // 0 when there are no cells.
  std::vector<SidebarRect> cells;  // One per input unit entry, same order.
};
int PinnedGridColumnLimit(int sidebar_dip);
int PinnedGridColumns(const PinnedGridMetrics& m, int total_units);
PinnedGridPlacement LayoutPinnedGrid(const std::vector<int>& units,
                                     const PinnedGridMetrics& m);

// ---- A vertical list (bookmark outline, tab list with group headers) -------
struct ListItem {
  enum class Kind { kHeader, kRow };
  Kind kind = Kind::kRow;
  bool hidden = false;  // Takes no room (collapsed group member).
  // Extra room below this item, before the gap: the group card's inner
  // insets (TabGroupCellView.innerTableTop/BottomInset) ride the header and
  // the group's last visible member.
  int pad_below = 0;
};
struct ListMetrics {
  int left = 0, right = 0;
  int top = 0;         // First item's top before scrolling.
  int row_height = 0;
  int row_gap = 0;
  int header_height = 0;  // Header pitch reuses row_gap.
  int scroll = 0;         // Subtracted from every top.
  // Clipping viewport; items entirely outside come back as empty rects.
  // clip=false places without clipping (the horizontal strip has none).
  bool clip = false;
  int viewport_top = 0, viewport_bottom = 0;
};
struct ListPlacement {
  std::vector<SidebarRect> rects;  // One per item; empty when hidden/clipped.
  // The same rects before clipping (hidden ones still empty) — containers
  // that span rows need the placed edge even when it is off the viewport.
  std::vector<SidebarRect> unclipped;
  int content_height = 0;  // Unscrolled height of every placed item.
  int scroll_max = 0;      // max(0, content − viewport) when clipping.
  int bottom = 0;          // y after the last placed item (scrolled).
};
ListPlacement LayoutList(const std::vector<ListItem>& items,
                         const ListMetrics& m);

// ---- The horizontal strip's shared width ------------------------------------
// Chrome's rule: tabs take their ideal width while they fit and shrink toward
// the floor as more open; resting tabs give up width first, the active one
// keeps its wider slot until they are all at the floor.
struct StripWidths {
  int rest = 0;
  int active = 0;  // 0 when there is no active tab.
};
StripWidths SolveStripWidths(int resting_count,
                             bool has_active,
                             int room,
                             int ideal_rest,
                             int ideal_active,
                             int floor);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_LAYOUT_H_
