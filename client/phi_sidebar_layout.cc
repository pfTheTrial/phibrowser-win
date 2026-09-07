// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phi_sidebar_layout.h"

#include <algorithm>

namespace phi_client {

int PinnedGridColumnLimit(int sidebar_dip) {
  const int decision = sidebar_dip - 8;
  return decision < 236 ? 3 : decision < 294 ? 4 : decision < 352 ? 5 : 6;
}

int PinnedGridColumns(const PinnedGridMetrics& m, int total_units) {
  const int avail = m.sidebar_dip - 2 * m.edge_inset_dip;
  int columns =
      std::max(1, std::min(PinnedGridColumnLimit(m.sidebar_dip), total_units));
  while (columns > 1 &&
         (avail - (columns - 1) * m.gap_dip) / columns < m.min_cell_width_dip) {
    --columns;
  }
  return columns;
}

PinnedGridPlacement LayoutPinnedGrid(const std::vector<int>& units,
                                     const PinnedGridMetrics& m) {
  PinnedGridPlacement out;
  int total_units = 0;
  for (const int u : units) total_units += std::max(1, u);
  out.columns = PinnedGridColumns(m, total_units);
  const int row_span =
      m.sidebar_px - 2 * m.edge_inset_px - (out.columns - 1) * m.gap_px;
  out.cells.reserve(units.size());
  int col = 0;
  int row_index = -1;
  for (const int u : units) {
    const int cu = std::min(std::max(1, u), out.columns);
    if (row_index < 0 || col + cu > out.columns) {
      ++row_index;
      col = 0;
    }
    const int y = m.origin_y + m.gap_px + row_index * (m.cell_height_px + m.gap_px);
    out.cells.push_back(
        {m.origin_x + col * m.gap_px + col * row_span / out.columns, y,
         m.origin_x + (col + cu - 1) * m.gap_px +
             (col + cu) * row_span / out.columns,
         y + m.cell_height_px});
    col += cu;
  }
  out.rows = units.empty() ? 0 : row_index + 1;
  return out;
}

ListPlacement LayoutList(const std::vector<ListItem>& items,
                         const ListMetrics& m) {
  ListPlacement out;
  out.rects.reserve(items.size());
  int y = m.top - m.scroll;
  for (const ListItem& item : items) {
    if (item.hidden) {
      out.rects.push_back({});
      out.unclipped.push_back({});
      continue;
    }
    const int h = item.kind == ListItem::Kind::kHeader ? m.header_height
                                                       : m.row_height;
    SidebarRect rect = {m.left, y, m.right, y + h};
    out.unclipped.push_back(rect);
    y += h + item.pad_below + m.row_gap;
    out.content_height += h + item.pad_below + m.row_gap;
    if (m.clip &&
        (rect.bottom <= m.viewport_top || rect.top >= m.viewport_bottom)) {
      rect = {};  // Scrolled out of the viewport.
    }
    out.rects.push_back(rect);
  }
  out.bottom = y;
  if (m.clip) {
    const int viewport = m.viewport_bottom - m.viewport_top;
    out.scroll_max = std::max(0, out.content_height - viewport);
  }
  return out;
}

StripWidths SolveStripWidths(int resting_count,
                             bool has_active,
                             int room,
                             int ideal_rest,
                             int ideal_active,
                             int floor) {
  StripWidths w{ideal_rest, has_active ? ideal_active : 0};
  if (resting_count * w.rest + w.active > room) {
    if (resting_count > 0) {
      w.rest = std::max(floor, (room - w.active) / resting_count);
    }
    if (has_active && resting_count * w.rest + w.active > room) {
      w.active = std::max(floor, room - resting_count * w.rest);
    }
  }
  return w;
}

}  // namespace phi_client
