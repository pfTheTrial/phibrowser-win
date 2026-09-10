// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_DRAG_SESSION_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_DRAG_SESSION_H_

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_sidebar_model.h"

namespace phi_client {

// ---------------------------------------------------------------------------
// THE PURE HALF — the drag's decisions, with no Win32 and no framework, so
// they are tested on the Linux host (test/model/sidebar_drag_test.cc). A
// host translation unit defines PHI_SIDEBAR_DRAG_MODEL_ONLY before
// including this and gets this half alone; the client build sees the whole
// file.

// WHAT is being dragged. A chip drag is the Spaces strip's pip, which rides
// the same press-move-release machinery as the rows and so is armed here
// too.
enum class PhiDragKind { kNone, kTab, kPin, kBookmark, kChip };

// Where the cursor currently sits during a tab/pin drag — same-band
// reorder, a conversion target (the Mac's moveNormalTab toPinned/
// toBookmark), the PAGE (a split drop, the Mac's SplitTabDropContainer —
// the one band bookmark drags also reach), or outside the window entirely
// (tear-off to a new window). Chip drags stay in their own band.
enum class PhiDropBand { kNone, kTabs, kPins, kBookmarks, kOutside, kContent };

// How far a sliding element has moved from its home, and how much it has
// grown. Pinned cells glide in BOTH axes (a grid gap wraps rows) AND
// resize — the grid stretches its cells to fill the row, so a phantom
// entering a row shrinks its neighbours and one leaving lets them widen;
// bookmark rows glide vertically.
struct PhiSlideVec {
  double dx = 0.0;
  double dy = 0.0;
  double dw = 0.0;
  double dh = 0.0;
  bool moved() const {
    return dx != 0.0 || dy != 0.0 || dw != 0.0 || dh != 0.0;
  }
};

// What arming a drag on `key` drags. A stored key (a cell or a folder) is a
// pinned-grid drag or a bookmark-outline one depending on which section
// holds it, which only the view knows — hence `key_is_pinned`. A group
// header is not draggable at all.
inline PhiDragKind PhiDragKindForKey(const SidebarKey& key,
                                     bool key_is_pinned) {
  switch (key.kind) {
    case SidebarKey::Kind::kTab:
    case SidebarKey::Kind::kSplit:
      return PhiDragKind::kTab;
    case SidebarKey::Kind::kCell:
    case SidebarKey::Kind::kFolder:
      return key_is_pinned ? PhiDragKind::kPin : PhiDragKind::kBookmark;
    case SidebarKey::Kind::kSpace:
      return PhiDragKind::kChip;
    case SidebarKey::Kind::kGroup:
    case SidebarKey::Kind::kNone:
      return PhiDragKind::kNone;
  }
  return PhiDragKind::kNone;
}

// The press has travelled far enough to BE a drag (the system's own
// SM_CXDRAG / SM_CYDRAG box, passed in so the rule is testable). Until it
// has, the press is still a click: the row activates on release and no drag
// image is lifted.
inline bool PhiDragPassedThreshold(int origin_x, int origin_y, int x, int y,
                                   int threshold_x, int threshold_y) {
  return std::abs(x - origin_x) >= threshold_x ||
         std::abs(y - origin_y) >= threshold_y;
}

// One leg of the reorder slide (Chrome's TabCollectionAnimatingLayoutManager:
// EASE_IN_OUT over kSlideMs), retargetable mid-flight — `from` is where the
// element was when the gap last moved, not where it started.
inline double PhiDragSlideEase(double from, double to,
                               unsigned long long elapsed_ms,
                               int duration_ms) {
  const double linear =
      duration_ms <= 0
          ? 1.0
          : std::min(1.0, static_cast<double>(elapsed_ms) / duration_ms);
  const double eased = linear * linear * (3.0 - 2.0 * linear);
  return from + (to - from) * eased;
}

}  // namespace phi_client

#if !defined(PHI_SIDEBAR_DRAG_MODEL_ONLY)

#include <windows.h>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_drag_image.h"
#include "phibrowser-win/client/phi_sidebar_drop.h"
#include "phibrowser-win/client/phi_split_drop.h"

namespace phi_client {

class PhiSidebarShell;
struct PhiSidebarRects;
class PhiSidebarView;
class PhiSpacesStrip;
class SpacesHost;
class WindowHost;

// ONE drag of a sidebar row, pinned cell, bookmark or Space pip
// (docs/architecture-review.md §3.5; lane 1 cluster I): the reorder and its
// slide animation, the pin and bookmark conversions, the tear-off, the drop
// on the page that forms or re-panes a split, and the drag image that
// carries the element between them.
//
// INVARIANTS
//  * It EXISTS only between ArmDrag and the commit or the cancel. Every
//    field of a drag lives in here, so a drag that ends cannot leave a flag
//    behind in the shell for the next one to trip over.
//  * Its DESTRUCTOR is AbortDrag: the drag image slides home (or goes), the
//    split-drop overlay comes down, the capture is released and the slide
//    timer is killed. The view owns the session, so a sidebar going away
//    takes both windows with it rather than the shell's destructor calling
//    two static Hide()s by hand.
//  * What is dragged is a SidebarKey, never an index: rows rebuild under a
//    drag (a folder toggle, a notification arriving) and a key survives
//    that. Index() re-resolves the key against the current rows for the
//    paint and slide code that walks vectors.
class PhiSidebarDragSession {
 public:
  // Arms the drag on `key` at `pt` (client coordinates). It is not LIVE
  // until the pointer passes the system drag threshold — see Update().
  PhiSidebarDragSession(PhiSidebarView* view, const SidebarKey& key, POINT pt);
  ~PhiSidebarDragSession();

  PhiSidebarDragSession(const PhiSidebarDragSession&) = delete;
  PhiSidebarDragSession& operator=(const PhiSidebarDragSession&) = delete;

  using DragKind = PhiDragKind;
  using DropBand = PhiDropBand;
  using SlideVec = PhiSlideVec;

  // Drag-reorder slide: Chrome's vertical-tab layout animation (200ms,
  // EASE_IN_OUT) — sibling rows glide to open the insertion gap.
  static constexpr int kSlideMs = 200;
  static constexpr UINT_PTR kSlideTimer = 0x518;
  // A reserved row-slide key: how far everything BELOW the bookmark
  // outline ("+ New Tab", the group headers, the tab rows) is pushed while
  // a drag opens a row-sized gap in the outline. Rides the same clock.
  static constexpr char kBandShiftKey[] = "\x01" "band";

  // The pointer moved with the button down.
  void Update(POINT pt);
  // The button came up: the drop lands (or the deferred click activates).
  void Commit();
  // The tab row a press armed WITHOUT activating it.
  void NoteClickTab(TabId tab_id) { click_tab_id_ = tab_id; }

  DragKind kind() const { return kind_; }
  DropBand band() const { return band_; }
  const SidebarKey& key() const { return key_; }
  bool active() const { return active_; }
  // PaintChrome renders the dragged element at home, in its lifted style,
  // so the image can lift its pixels straight out of the frame — the Mac's
  // createDraggingSnapshot. True only for that one capture pass.
  bool capture() const { return capture_; }
  // The dragged element's position in the kind's own vector (-1 when it is
  // gone), re-resolved from the key each time.
  int Index() const;
  // The visible bookmark row a drop INTO a collapsed folder highlights
  // (-1 = an ordinary gap insertion).
  int folder_row() const { return bookmark_drop_folder_row_; }
  // The slide offsets the painters glide siblings by.
  SlideVec RowSlideOffset(const std::string& guid) const;
  double DragSlideOffset(TabId tab_id) const;
  // True while the current leg is still in flight (the shell's timer).
  bool SlideRunning() const;
  // The 2px accent insertion line, horizontal for vertical lists and
  // vertical for horizontal ones.
  void DrawIndicator(HDC dc);

 private:
  // Everything below is the body of the drag, in the order it runs.
  int Scaled(int design_units) const;
  HWND shell_hwnd() const;
  PhiSpacesStrip* strip() const;

  DropBand DropBandAt(POINT client_pt);
  // Inside the pinned band, the grid preview's extra row included.
  bool InPinnedBand(const PhiSidebarRects& layout, POINT client_pt) const;
  // A drag that could land in an EMPTY stored band reserves that band's
  // drop zone in the layout for its duration (the Mac's 100pt drag-time
  // pinned container and its "Drag tabs here" view): the pinned zone for
  // a tab or a plain bookmark, the bookmark zone for a tab or a pin. The
  // rows re-place under the zone on the posted rebuild; the release on
  // commit or cancel posts the one that closes it again.
  void ReserveDropZones();
  void ReleaseDropZones();
  int DragInsertSlot(POINT pt);
  void RetargetDragSlide();  // New targets for the current gap slot.
  bool CommitDragImpl();     // True when the drop changed something.
  void CancelDrag();         // Drops every field; the image is the caller's.
  void AbortDrag();

  // --- The drag image -----------------------------------------------------
  RECT DragSourceRect();
  // The Mac's drag-image boundary (containsTabDragBoundary): inside the
  // sidebar's list band, or over ANOTHER shell window, the row image shows;
  // elsewhere — the page, the header, off the window — the page card does.
  bool DragImageInside(POINT client_pt);
  void BeginDragImage(POINT client_pt);
  void SyncDragImage(POINT client_pt);
  // After the drop: slides home when nothing happened and the pointer was
  // still inside the sidebar; otherwise just takes the image down.
  void EndDragImage(bool acted, bool inside);
  // Over the pinned grid a tab or bookmark row is shown as the CELL it is
  // about to become: the phantom cell's size in the grid preview (capped
  // at a full row's third, so an only pin's sidebar-wide cell does not
  // bury the target), painted as a lifted cell — the wash, the favicon(s)
  // or the title's initial.
  bool DragCellSize(int* width, int* height);
  PhiDragBitmap BuildDragCell(int width, int height);
  // The mirror: a pinned cell over the outline or the tab list is shown as
  // the ROW it becomes — the lifted pill at the rows' width, favicon and
  // title (a split cell: pane A's, the row is merged anyway).
  bool DragRowSize(int* width, int* height);
  PhiDragBitmap BuildDragRow(int width, int height);
  // The page card's model for this drag: the dragged row's live page as
  // seen on screen, else its cached thumbnail(s), else favicon + title.
  PhiDragCard BuildDragCard();
  // The browser child's on-screen pixels, downscaled toward the card.
  PhiDragBitmap CaptureContentFromScreen();
  // A pinned row or bookmark released outside the window: its live tab(s)
  // move to the shell under the cursor, else tear off into a new window.
  bool TearOffStoredRow(DragKind kind, const SidebarKey& key, POINT release);

  // --- Drop on the page ---------------------------------------------------
  // The dragged row as the page drop sees it.
  bool SplitDragSource(DragKind kind,
                       const SidebarKey& key,
                       TabId* tab_id,
                       std::string* url,
                       std::string* guid) const;
  // Resolves what a drop at `pt` would do and shows (or hides) the hint
  // cards.
  bool ResolveSplitDrop(POINT pt, bool rebuild);
  // Performs the drop ResolveSplitDrop described. Takes its inputs by value
  // because the commit clears the drag state before acting.
  void CommitSplitDrop(DragKind kind,
                       const SidebarKey& source,
                       TabId focused_id,
                       PhiSplitDropZone zone,
                       bool replace,
                       const std::string& split_id);

  // Owns this session and outlives it; the shell is its shell.
  RAW_PTR_EXCLUSION PhiSidebarView* const view_;
  RAW_PTR_EXCLUSION PhiSidebarShell* const shell_;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* const framework_;
  // The two narrow services a drag needs, taken from the view that owns
  // it: the Space list for a pip reorder (and the drop indicator's
  // palette), and the window set for the cross-window drop and the
  // tear-off (docs/architecture-review.md §3.5).
  RAW_PTR_EXCLUSION SpacesHost* const spaces_host_;
  RAW_PTR_EXCLUSION WindowHost* const window_host_;
  const SlotId slot_;
  const bool incognito_;

  // WHAT is dragged: a key, never an index.
  SidebarKey key_;
  DragKind kind_ = DragKind::kNone;
  DropBand band_ = DropBand::kNone;
  POINT origin_{};
  POINT point_{};  // Cursor, for the drop.
  bool active_ = false;
  bool finished_ = false;  // Committed or cancelled: the destructor is a no-op.
  int insert_ = -1;        // The gap slot for the slide animation, -1 = none.
  bool zones_reserved_ = false;  // ReserveDropZones took a zone.
  // The grid preview's rows beyond the real band, in pixels (a phantom
  // that wraps onto a new row): everything under the band slides down by
  // it, and the pinned band's hit region grows by it so the pointer can
  // follow the previewed row without leaving the band.
  int grid_extra_ = 0;
  // WHERE it lands (docs/sidebar-architecture.md §5), resolved by the pure
  // drop resolvers on every move; consumed by the commit as a SidebarMove.
  SidebarPlace place_;
  bool place_valid_ = false;
  SidebarKey folder_key_;  // A drop INTO this collapsed folder.
  int bookmark_drop_folder_row_ = -1;
  // The tab that was FOCUSED when this drag was armed — the split drop's
  // other pane. It has to be captured at arming time: the same mouse-down
  // that arms a drag also activates the tab it grabbed (and OPENS the
  // pinned tab or bookmark it grabbed).
  TabId focus_tab_id_;
  TabId click_tab_id_;

  // The slide legs: each eases current -> target over kSlideMs, retargeting
  // when the gap moves.
  std::map<TabId, double> drag_slide_from_;
  std::map<TabId, double> drag_slide_to_;
  std::map<std::string, SlideVec> row_slide_from_;
  std::map<std::string, SlideVec> row_slide_to_;
  ULONGLONG drag_slide_start_ = 0;  // Leg clock; 0 = idle.

  // The drag image (PhiDragImage): the dragged row's snapshot while the
  // cursor is inside the sidebar, the page card once it leaves.
  bool capture_ = false;
  bool image_inside_ = true;  // Last boundary verdict: row or card.
  // Inside, over ANOTHER band: the element morphs into that band's shape
  // (a row into a cell over the grid, a cell into a row over the outline
  // or the list). The width shown, to recompose on a change.
  enum class Morph { kNone, kCell, kRow };
  Morph image_morph_ = Morph::kNone;
  int image_morph_width_ = 0;
  bool card_built_ = false;   // The card is composed once per drag.
  PhiDragCard card_;
  RECT home_screen_{};  // The element's rect at lift-off, for the slide.

  // DROP ON THE PAGE — the Mac's SplitTabDropContainer.
  PhiSplitDropZone split_drop_zone_ = PhiSplitDropZone::kNone;
  // Whether this drag can form a split at all. Resolved ONCE when the
  // pointer enters the page (nothing it depends on can change mid-drag).
  bool split_drop_valid_ = false;
  bool split_drop_replace_ = false;
  std::string split_drop_split_id_;  // Replace mode: the split being re-paned.
  bool split_drop_stacked_ = false;  // Its divider axis, for the pane cards.
  double split_drop_ratio_ = 0.5;    // The primary pane's share.
};

}  // namespace phi_client

#endif  // !defined(PHI_SIDEBAR_DRAG_MODEL_ONLY)

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SIDEBAR_DRAG_SESSION_H_
