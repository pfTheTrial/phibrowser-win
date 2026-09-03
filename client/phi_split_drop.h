// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SPLIT_DROP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SPLIT_DROP_H_

#include <windows.h>

#include <string>

namespace phi_client {

class PhiWindowController;

// Dropping a dragged sidebar row onto the PAGE to form (or re-pane) a split —
// the Windows twin of the Mac's SplitTabDropContainer.
//
// The Mac hangs this off an NSView that owns the web-content area and accepts
// the drag through NSDraggingDestination. Here the content area belongs to the
// reparented Chromium child, and the sidebar's drag is a raw mouse drag with
// the shell holding capture — so the shell resolves the zone itself (the
// controller's split-drop members) and this file owns the two things that are
// genuinely about the page area: the DROP GEOMETRY and the HINT CARDS drawn
// over it.
//
// The zones follow the Mac exactly:
//   create mode  (the focused tab is not in a split) — the four edge thirds
//                of the page form a new split; the dragged item lands in the
//                dropped pane, the focused tab takes the other. Left/right
//                make a side-by-side ("vertical") split, top/bottom a stacked
//                ("horizontal") one. The middle band is a no-drop area.
//   replace mode (the focused tab IS a split) — the page maps onto the split's
//                two live panes and the dragged item swaps into the one under
//                the cursor.
enum class PhiSplitDropZone { kNone, kLeft, kRight, kTop, kBottom };

// True when the zone lands in Chromium's PRIMARY slot (leading / top).
bool PhiSplitZoneIsPrimary(PhiSplitDropZone zone);
// The divider axis a create-mode drop on `zone` produces: "vertical" for the
// side cards (side-by-side), "horizontal" for the top/bottom ones (stacked).
const char* PhiSplitZoneLayout(PhiSplitDropZone zone);

// CREATE mode hit test: the four edge thirds of `area`, left/right winning the
// overlapping corners. `retained` is the zone the drag is already on — it
// keeps its grip until the pointer leaves it by more than a small tolerance,
// so tremble in a corner cannot flip the choice back and forth.
PhiSplitDropZone PhiSplitCreateZoneAt(const RECT& area,
                                      POINT pt,
                                      PhiSplitDropZone retained);

// REPLACE mode hit test: the pane the point sits over, splitting at the
// divider's midline. Always resolves to kLeft (the primary pane — left for a
// side-by-side split, top for a stacked one) or kRight (the secondary).
PhiSplitDropZone PhiSplitReplaceZoneAt(const RECT& area,
                                       POINT pt,
                                       bool stacked,
                                       double ratio);

// The live panes of a split filling `area`, mirroring Chromium's own layout so
// the replace-mode cards line up with what the user sees.
void PhiSplitReplacePaneRects(const RECT& area,
                              bool stacked,
                              double ratio,
                              RECT* primary,
                              RECT* secondary);

// What the hint overlay should be showing right now.
struct PhiSplitDropHints {
  RECT area{};        // The page area, in shell client coordinates.
  bool replace = false;
  bool stacked = false;  // Replace mode: the split's divider axis.
  double ratio = 0.5;    // Replace mode: the primary pane's share.
  PhiSplitDropZone active = PhiSplitDropZone::kNone;
};

// The hint cards themselves: one layered, click-through child window above the
// browser child, composed with GDI+ (per-pixel alpha, which is the only way to
// get the Mac card's translucency over live page content).
class PhiSplitDropOverlay {
 public:
  // Mounts or updates the cards over `controller`'s page area. Cheap to call
  // on every pointer move — a repaint only happens when something changed.
  static void Show(PhiWindowController* controller,
                   const PhiSplitDropHints& hints);
  // Takes the cards down (the drag left the page, was dropped, or cancelled).
  static void Hide();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SPLIT_DROP_H_
