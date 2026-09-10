// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_DROP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_DROP_H_

#include <string>
#include <vector>

#include "phi_sidebar_actions.h"
#include "phi_sidebar_layout.h"
#include "phi_sidebar_model.h"

// Where a drag lands — docs/sidebar-architecture.md §5. Pure functions over
// the placed rows of one section (key + rect + the little tree/group context
// a target needs) and the pointer; the controller feeds them its layout rows
// and gets back a SidebarPlace plus what to paint. Host-tested.

namespace phi_client {

struct DropRow {
  SidebarKey key;
  SidebarRect bounds;          // Empty = not on screen (hidden/clipped).
  // Bookmark outline context.
  int depth = 0;
  bool folder = false;
  bool expanded = false;
  std::string parent_guid;
  // Tab list context.
  std::string group_id;
  bool group_header = false;   // kGroup rows.
  // A member row: the group card's bottom edge (0 = none). A pointer past
  // the last member's midpoint but still above it drops INTO the group as
  // its last member; below it, after the group.
  int group_bottom = 0;
};

struct DropResolution {
  SidebarPlace place;
  // The visible slot the gap opens at, 0..n (n = end), counted over the
  // rows the slide animation moves: outline rows, grid cells, or visible
  // tab rows (headers never count). -1 = no gap: the drop lands INTO a
  // folder (highlight `folder_key` instead).
  int gap_slot = -1;
  SidebarKey folder_key;
};

// The bookmark outline (the Mac's SidebarBookmarkFolderDropResolver). Rows
// are the VISIBLE outline rows in order. Over an expanded folder: its first
// child. Over the upper half of a collapsed folder: INTO it (highlight, no
// gap). Otherwise before/after the row among its own siblings; a pointer
// below every row lands at the root's end. `dragging` (may be empty) is the
// row leaving its slot, never its own anchor.
DropResolution ResolveBookmarkDrop(const std::vector<DropRow>& rows,
                                   int pointer_y,
                                   const SidebarKey& dragging);

// The pinned grid: cells in order; the pointer's row of cells, then left or
// right of a cell's horizontal midpoint. Below every row = the end.
DropResolution ResolvePinnedDrop(const std::vector<DropRow>& cells,
                                 int pointer_x,
                                 int pointer_y,
                                 const SidebarKey& dragging);

// The tab list (vertical) or strip (horizontal): rows include group headers;
// the slot is before the first row whose midpoint is past the pointer. A
// drop inside a group's run (after its header, before its last member's
// bottom half) joins that group; between groups it is ungrouped. Header rows
// themselves anchor on the group's first member. `horizontal` compares x.
DropResolution ResolveTabDrop(const std::vector<DropRow>& rows,
                              int pointer_x,
                              int pointer_y,
                              bool horizontal,
                              const SidebarKey& dragging);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_DROP_H_
