// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SPLIT_DROP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SPLIT_DROP_H_

#include <windows.h>

#include <array>

#include <string>

namespace phi_client {

class PhiShellWindow;

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
// kNewWindow: the page's centre (Chrome's and Edge's rule — a tab dragged
// onto the page detaches) and the fifth snap tile (windows-native-ui.md
// §4.8).
enum class PhiSplitDropZone { kNone, kLeft, kRight, kTop, kBottom, kNewWindow };

// The snap-tile flyout at the page's top centre (§4.8): five 56 × 40 dip
// tiles — Left, Right, Top, Bottom split, then New window — in a radius-8
// flyout, 8 apart, a divider before the fifth. In replace mode the first
// two are the split's panes and the middle two are empty. `out[5]` is the
// flyout's own rect. Shell client coordinates, from `area` and `dpi`.
void PhiSnapTileRects(const RECT& area,
                      UINT dpi,
                      bool replace,
                      std::array<RECT, 6>* out);

// True when the zone lands in Chromium's PRIMARY slot (leading / top).
bool PhiSplitZoneIsPrimary(PhiSplitDropZone zone);
// The divider axis a create-mode drop on `zone` produces: "vertical" for the
// side cards (side-by-side), "horizontal" for the top/bottom ones (stacked).
const char* PhiSplitZoneLayout(PhiSplitDropZone zone);

// CREATE mode hit test: the four edge thirds of `area`, left/right winning the
// overlapping corners. `retained` is the zone the drag is already on — it
// keeps its grip until the pointer leaves it by more than a small tolerance,
// so tremble in a corner cannot flip the choice back and forth.
// The zone under `pt` while nothing is split: a snap tile first, then the
// edge thirds (with the 12 px exit tolerance for `retained`), then the
// centre = New window. `dpi` places the tiles; 96 lays them out in dip.
PhiSplitDropZone PhiSplitCreateZoneAt(const RECT& area,
                                      POINT pt,
                                      PhiSplitDropZone retained,
                                      UINT dpi = 96);

// What the hint overlay should be showing right now.
struct PhiSplitDropHints {
  RECT area{};        // The page area, in shell client coordinates.
  bool replace = false;
  bool stacked = false;  // Replace mode: the split's divider axis.
  double ratio = 0.5;    // Replace mode: the primary pane's share.
  // Replace mode: where Chromium actually laid the two panes out (the
  // framework's TabContentBoundsById, mapped into the shell), so the cards
  // trace the real cards rather than a ratio estimate — the Mac's
  // replacePaneRects mirrors its own SplitPaneHostView the same way. `panes`
  // is false until the layout has run; the ratio estimate stands in.
  bool panes = false;
  RECT primary{};
  RECT secondary{};
  PhiSplitDropZone active = PhiSplitDropZone::kNone;
};

// REPLACE mode hit test: the pane the point sits over, splitting at the
// divider's midline. Always resolves to kLeft (the primary pane — left for a
// side-by-side split, top for a stacked one) or kRight (the secondary).
PhiSplitDropZone PhiSplitReplaceZoneAt(const PhiSplitDropHints& hints,
                                       POINT pt,
                                       UINT dpi = 96);

// The live panes of the split `hints` describes: the real rects when it has
// them, else an estimate from the ratio over `hints.area` with the
// framework's flush panes and 4 DIP divider.
void PhiSplitReplacePaneRects(const PhiSplitDropHints& hints,
                              RECT* primary,
                              RECT* secondary);

// The hint cards themselves: one layered, click-through child window above the
// browser child, composed with GDI+ (per-pixel alpha, which is the only way to
// get the Mac card's translucency over live page content).
class PhiSplitDropOverlay {
 public:
  // Mounts or updates the cards over `controller`'s page area. Cheap to call
  // on every pointer move — a repaint only happens when something changed.
  static void Show(PhiShellWindow* controller,
                   const PhiSplitDropHints& hints);
  // Takes the cards down (the drag left the page, was dropped, or cancelled).
  static void Hide();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SPLIT_DROP_H_
