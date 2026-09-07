// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phi_sidebar_drop.h"

namespace phi_client {

namespace {

int MidY(const SidebarRect& r) {
  return (r.top + r.bottom) / 2;
}
int MidX(const SidebarRect& r) {
  return (r.left + r.right) / 2;
}

}  // namespace

DropResolution ResolveBookmarkDrop(const std::vector<DropRow>& rows,
                                   int pointer_y,
                                   const SidebarKey& dragging) {
  DropResolution out;
  out.place.section = SidebarPlace::Section::kBookmarks;
  // The anchor among a row's siblings: the next visible row with the same
  // parent that is not the dragged one (its own descendants sit between and
  // are skipped by depth).
  const auto next_sibling = [&](size_t i) -> SidebarKey {
    for (size_t j = i + 1; j < rows.size(); ++j) {
      if (rows[j].depth < rows[i].depth) break;  // Left the parent.
      if (rows[j].parent_guid == rows[i].parent_guid &&
          rows[j].key != dragging) {
        return rows[j].key;
      }
    }
    return {};
  };
  for (size_t i = 0; i < rows.size(); ++i) {
    const DropRow& row = rows[i];
    if (row.bounds.empty() || pointer_y < row.bounds.top ||
        pointer_y >= row.bounds.bottom) {
      continue;
    }
    const bool upper = pointer_y < MidY(row.bounds);
    if (row.folder && row.expanded && row.key != dragging) {
      // Anywhere on an expanded folder: its first child.
      out.place.parent_guid = row.key.id;
      for (size_t j = i + 1; j < rows.size(); ++j) {
        if (rows[j].depth <= row.depth) break;
        if (rows[j].parent_guid == row.key.id && rows[j].key != dragging) {
          out.place.before = rows[j].key;
          break;
        }
      }
      out.gap_slot = static_cast<int>(i) + 1;
      return out;
    }
    if (row.folder && upper && row.key != dragging) {
      // Upper half of a collapsed folder: INTO it, at the end.
      out.place.parent_guid = row.key.id;
      out.folder_key = row.key;
      out.gap_slot = -1;
      return out;
    }
    out.place.parent_guid = row.parent_guid;
    if (upper) {
      out.place.before = row.key == dragging ? next_sibling(i) : row.key;
      out.gap_slot = static_cast<int>(i);
    } else {
      out.place.before = next_sibling(i);
      out.gap_slot = static_cast<int>(i) + 1;
    }
    return out;
  }
  // In a gap or past the end: the first row whose midpoint is below.
  for (size_t i = 0; i < rows.size(); ++i) {
    const DropRow& row = rows[i];
    if (row.bounds.empty() || MidY(row.bounds) <= pointer_y) continue;
    out.place.parent_guid = row.parent_guid;
    out.place.before = row.key == dragging ? next_sibling(i) : row.key;
    out.gap_slot = static_cast<int>(i);
    return out;
  }
  out.place.parent_guid.clear();  // The root's end.
  out.gap_slot = static_cast<int>(rows.size());
  return out;
}

DropResolution ResolvePinnedDrop(const std::vector<DropRow>& cells,
                                 int pointer_x,
                                 int pointer_y,
                                 const SidebarKey& dragging) {
  DropResolution out;
  out.place.section = SidebarPlace::Section::kPinned;
  int slot = 0;
  for (const DropRow& cell : cells) {
    if (cell.bounds.empty()) {
      continue;
    }
    const bool above = cell.bounds.bottom <= pointer_y;
    const bool same_row =
        pointer_y >= cell.bounds.top && pointer_y < cell.bounds.bottom;
    if (above || (same_row && MidX(cell.bounds) <= pointer_x)) {
      ++slot;
      continue;
    }
    break;
  }
  out.gap_slot = slot;
  // The anchor: the first cell at or after the slot that is not the dragged.
  int seen = 0;
  for (const DropRow& cell : cells) {
    if (seen >= slot && cell.key != dragging) {
      out.place.before = cell.key;
      break;
    }
    ++seen;
  }
  return out;
}

DropResolution ResolveTabDrop(const std::vector<DropRow>& rows,
                              int pointer_x,
                              int pointer_y,
                              bool horizontal,
                              const SidebarKey& dragging) {
  DropResolution out;
  out.place.section = SidebarPlace::Section::kTabs;
  out.place.group_explicit = true;
  // The first visible row whose midpoint is past the pointer is the anchor;
  // a header counts as its group's leading edge.
  // The gap slot counts VISIBLE TAB rows only (no headers, nothing hidden or
  // clipped): that is the index space the row-slide animation works in.
  int slot = 0;
  const DropRow* anchor = nullptr;
  size_t anchor_at = rows.size();
  const DropRow* previous = nullptr;  // Last visible row before the slot.
  for (size_t i = 0; i < rows.size(); ++i) {
    const DropRow& row = rows[i];
    if (row.bounds.empty()) {
      continue;
    }
    const int mid = horizontal ? MidX(row.bounds) : MidY(row.bounds);
    const int pointer = horizontal ? pointer_x : pointer_y;
    if (mid <= pointer) {
      previous = &row;
      if (!row.group_header) {
        ++slot;
      }
      continue;
    }
    anchor = &row;
    anchor_at = i;
    break;
  }
  out.gap_slot = slot;
  // Group: inside a run when the rows on both sides share a group; a header
  // as the anchor means "before the group", so ungrouped; after a group's
  // last member means "after the group", ungrouped.
  if (previous && !previous->group_header && !previous->group_id.empty() &&
      anchor && !anchor->group_header && anchor->group_id == previous->group_id) {
    out.place.group_id = previous->group_id;
  } else if (previous && previous->group_header) {
    out.place.group_id = previous->group_id;  // Right under the header.
  }
  if (anchor) {
    if (!anchor->group_header && anchor->key != dragging) {
      out.place.before = anchor->key;
    } else {
      // A header's first member is the tab anchor; the dragged row's own
      // successor when it was the anchor.
      for (size_t j = anchor_at + 1; j < rows.size(); ++j) {
        if (!rows[j].group_header && rows[j].key != dragging) {
          out.place.before = rows[j].key;
          break;
        }
      }
    }
  }
  return out;
}

}  // namespace phi_client
