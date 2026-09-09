// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_sidebar_view.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_bookmark_manager.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_mac_icons.h"
#include "phibrowser-win/client/phi_new_tab_page.h"
#include "phibrowser-win/client/phi_shell_layout.h"
#include "phibrowser-win/client/phi_sidebar_drag_session.h"
#include "phibrowser-win/client/phi_spaces_strip.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_window_model.h"

namespace phi_client {

namespace {

using phi_framework::SplitSnapshot;
using phi_framework::TabSnapshot;
using phi_framework::WindowSnapshot;

using shell_metrics::kEdgeInset;
using shell_metrics::kFaviconSlot;
using shell_metrics::kFavoritesSpacer;
using shell_metrics::kNavButton;
using shell_metrics::kPinnedCellHeight;
using shell_metrics::kPinnedGap;
using shell_metrics::kTabRowGap;
using shell_metrics::kTabRowHeight;
using shell_metrics::kTopBarSpacing;

using sidebar_metrics::kBookmarkIndent;
using sidebar_metrics::kCloseBoxSize;
using sidebar_metrics::kCornerRadius;
using sidebar_metrics::kFaviconSize;
using sidebar_metrics::kFolderIconSize;
using sidebar_metrics::kGroupHeaderHeight;
using sidebar_metrics::kGroupInnerInset;
using sidebar_metrics::kGroupMemberLeadInset;
using sidebar_metrics::kGroupMemberTrailInset;
using sidebar_metrics::kHTabActiveWidth;
using sidebar_metrics::kHTabGap;
using sidebar_metrics::kHTabHeight;
using sidebar_metrics::kHTabMinWidth;
using sidebar_metrics::kHTabWidth;
using sidebar_metrics::kPinnedFavicon;
using sidebar_metrics::kPinnedMinWidth;
using sidebar_metrics::kRowPad;
using sidebar_metrics::kStripPinFavicon;
using sidebar_metrics::kStripPinGap;
using sidebar_metrics::kStripPinSize;
using sidebar_metrics::kStripPinToTabs;

// A RECLAIMED tab — restored-but-never-loaded (unloaded) or discarded —
// draws its favicon at 30% (the Mac's TabFaviconPresentation.reclaimedOpacity,
// a700125c, which replaced the short-lived dashed ring); the dimmed TITLE on
// tab rows stays, as before.
constexpr float kReclaimedOpacity = 0.3f;

// The Mac's TabOpenIndicatorView (a700125c): a 2pt dot — black in light,
// white in dark — marking an inactive pinned tab or bookmark whose tab is
// open, behind "Show open tab indicators" (off by default). It hangs 3pt
// under a pinned favicon (2pt on the Comfortable strip, where the cell is
// shorter) and leads a bookmark's favicon by 2pt. A reclaimed tab dims the
// dot with its favicon.
constexpr int kOpenDotDiameter = 2;
constexpr int kOpenDotPinnedSpacing = 3;
constexpr int kOpenDotStripSpacing = 2;
constexpr int kOpenDotBookmarkSpacing = 2;

COLORREF OpenDotInk(const PhiPalette& palette) {
  return palette.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
}

// `ink` at `opacity` over `ground` — the dot has no alpha channel of its own.
COLORREF FadeInk(COLORREF ink, COLORREF ground, float opacity) {
  const auto mix = [&](int a, int b) {
    return static_cast<int>(b + (a - b) * opacity + 0.5f);
  };
  return RGB(mix(GetRValue(ink), GetRValue(ground)),
             mix(GetGValue(ink), GetGValue(ground)),
             mix(GetBValue(ink), GetBValue(ground)));
}

void DrawOpenDot(HDC dc, int left, int top, int diameter, COLORREF ink) {
  PhiFillEllipseAA(dc, {left, top, left + diameter, top + diameter}, ink);
}

}  // namespace


// A filled rounded rect, the Mac client's selection/field shape.
// A fill rounded on its TOP corners only, square along the bottom — the
// comfortable tab's shape, which has to sit flush on the toolbar it merges
// into. Drawing the round rect a radius taller and clipping to `r` puts the
// arcs where they belong and leaves the bottom edge straight.
// Every rounded shape here is antialiased and takes the TRUE corner radius
// (phi_dwrite.h). The GDI RoundRect these replaced took the corner
// ellipse's diameter, so a pill asked for kCornerRadius came out at half
// of it — and the active pin's ring, which passed 2 * kCornerRadius, was
// the one shape that drew the radius the constant names.
void PhiFillRoundedTop(HDC dc, const RECT& r, COLORREF fill, int radius) {
  if (r.right <= r.left || r.bottom <= r.top) {
    return;
  }
  const int save = ::SaveDC(dc);
  ::IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
  PhiFillRoundRectAA(dc, {r.left, r.top, r.right, r.bottom + radius}, radius,
                     fill);
  ::RestoreDC(dc, save);
}

void PhiFillRounded(HDC dc, const RECT& r, COLORREF fill, int radius) {
  PhiFillRoundRectAA(dc, r, radius, fill);
}

void PhiStrokeRounded(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
                   int radius) {
  PhiFillRoundRectAA(dc, r, radius, fill);
  PhiStrokeRoundRectAA(dc, r, radius, border, 1);
}

// Reader View's icon: SF Symbol `doc.plaintext`, which is what BOTH Mac
// address bars draw (SideAddressBar's HoverableButtonConfig and
// ReaderButtonView). Segoe MDL2's nearest neighbour is ReadingMode (E736), an
// OPEN BOOK — a different picture altogether — so the symbol is drawn instead
// of substituted: a rounded page outline with lines of text in it.
//
// Proportions are the symbol's own, in units of the glyph box: the page is
// 0.72 wide by 0.90 tall, corner radius 0.14, stroke 0.085, and the text runs
// as three lines inset 0.17 from each side, the last one short — four lines
// at this size close up into a smudge.
void PhiDrawReaderGlyph(HDC dc, const RECT& button, COLORREF ink, UINT dpi) {
  if (button.right <= button.left || button.bottom <= button.top) {
    return;
  }
  const auto scaled = [dpi](double units, double box) {
    return static_cast<int>(units * box * dpi / 96.0 + 0.5);
  };
  // The glyph box the symbol is laid out in: the Mac draws a 12pt image in a
  // 24pt button, so half the button, and never bigger than it fits.
  const double box = (std::min)(button.right - button.left,
                                button.bottom - button.top) * 96.0 / dpi / 2.0;
  const int page_w = scaled(0.72, box);
  const int page_h = scaled(0.90, box);
  if (page_w <= 2 || page_h <= 2) {
    return;
  }
  const int cx = (button.left + button.right) / 2;
  const int cy = (button.top + button.bottom) / 2;
  RECT page = {cx - page_w / 2, cy - page_h / 2, cx - page_w / 2 + page_w,
               cy - page_h / 2 + page_h};
  const int stroke = (std::max)(1, scaled(0.085, box));
  PhiStrokeRoundRectAA(dc, page, (std::max)(1, scaled(0.14, box)), ink, stroke);

  const int inset = scaled(0.17, box);
  const int line_left = page.left + inset;
  const int line_right = page.right - inset;
  if (line_right - line_left <= 1) {
    return;
  }
  // Evenly spaced across the page's inner height, so the block of text sits
  // optically centred rather than crowding either edge.
  const int rows = 3;
  const int top = page.top + inset;
  const int bottom = page.bottom - inset;
  // Solid fills, not the AA helper: a one-pixel bar antialiases to about half
  // coverage and the text would read lighter than the outline around it.
  HBRUSH text_ink = ::CreateSolidBrush(ink);
  for (int i = 0; i < rows; ++i) {
    const int y = top + (bottom - top - stroke) * i / (rows - 1);
    // The last line is short, as the symbol's is.
    const int right = i == rows - 1
                          ? line_left + (line_right - line_left) * 3 / 5
                          : line_right;
    RECT line = {line_left, y, right, y + stroke};
    ::FillRect(dc, &line, text_ink);
  }
  ::DeleteObject(text_ink);
}

void PhiDrawGlyph(HDC dc, const RECT& r, const wchar_t* glyph, COLORREF ink) {
  if (r.right <= r.left) {
    return;
  }
  ::SetTextColor(dc, ink);
  // An MDL2 icon under an MDL2 font goes through DirectWrite (grayscale
  // antialiasing both ways); text takes GDI as before.
  PhiDrawTextAA(dc, glyph, -1, const_cast<RECT*>(&r),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// The same, for an MDL2 ICON: rasterized by DirectWrite at `pixel_size`
// (the em the caller's GDI font was built at, so the glyph lands the same
// size in the same place) and alpha-blended over what is already painted.
// GDI renders these with ClearType — horizontal smoothing only, plus a
// grid-fit per stem — which is what stepped the magnifier's arc and fringed
// every diagonal in colour. Falls back to GDI whenever DirectWrite or the
// glyph is missing, so nothing depends on it being there.
void PhiDrawGlyphIcon(HDC dc, const RECT& r, const wchar_t* glyph,
                      int pixel_size, COLORREF ink) {
  if (r.right <= r.left) {
    return;
  }
  if (!PhiDrawIconGlyph(dc, r, glyph, pixel_size, ink)) {
    PhiDrawGlyph(dc, r, glyph, ink);
  }
}

// `over` at `alpha`/255 on `base` — the Mac's translucent overlays, opaque.
COLORREF PhiBlendOver(COLORREF over, COLORREF base, int alpha) {
  auto mix = [&](int a, int b) { return (a * alpha + b * (255 - alpha)) / 255; };
  return RGB(mix(GetRValue(over), GetRValue(base)),
             mix(GetGValue(over), GetGValue(base)),
             mix(GetBValue(over), GetBValue(base)));
}

// Readers over a WindowSnapshot — the model's copy or a freshly pulled one
// (PullSnapshot) alike. The split-drop paths map ids to tags and back
// several times inside a single drop, so they read the window ONCE and walk
// this instead.
const TabSnapshot* PhiFindTabIn(const WindowSnapshot& snapshot, TabId id) {
  if (!id.valid()) {
    return nullptr;
  }
  for (const TabSnapshot& tab : snapshot.tabs) {
    if (tab.id == id) {
      return &tab;
    }
  }
  return nullptr;
}

const TabSnapshot* PhiFindTagIn(const WindowSnapshot& snapshot,
                             const std::string& tag) {
  if (tag.empty()) {
    return nullptr;
  }
  for (const TabSnapshot& tab : snapshot.tabs) {
    if (tab.tag == tag) {
      return &tab;
    }
  }
  return nullptr;
}

const SplitSnapshot* PhiFindSplitIn(const WindowSnapshot& snapshot,
                                 const std::string& split_id) {
  if (split_id.empty()) {
    return nullptr;
  }
  for (const SplitSnapshot& split : snapshot.splits) {
    if (split.id == split_id) {
      return &split;
    }
  }
  return nullptr;
}

// The live tab bound to a stored row, or `fallback` when the row has none.
// A pinned tab or bookmark is OPENED by the very press that arms its drag, so
// the view row the drag was armed from can still say "closed" when the drop
// lands — and opening its url a second time would leave a duplicate tab
// beside the pane.
TabId PhiLiveTabForRow(const WindowSnapshot& snapshot,
                          const std::string& guid,
                          TabId fallback) {
  const TabSnapshot* tab = PhiFindTagIn(snapshot, guid);
  return tab ? tab->id : fallback;
}

// The framework names surfaces (a reader page recognised from what it
// shows, committed or not); the client parses no reader URLs.
SidebarSurface PhiSurfaceOf(const TabSnapshot& tab) {
  return tab.surface == phi_framework::TabSurface::kReader
             ? SidebarSurface::kReader
             : SidebarSurface::kContent;
}

RECT PhiToRect(const SidebarRect& r) {
  return RECT{r.left, r.top, r.right, r.bottom};
}
PhiSidebarView::PhiSidebarView(PhiSidebarShell* shell,
                               SlotId slot,
                               phi_framework::PhiFramework* framework,
                               SidebarHost* sidebar_host,
                               SpacesHost* spaces_host,
                               ChromePrefsHost* prefs_host,
                               AgentHost* agent_host,
                               WindowHost* window_host,
                               const PhiWindowModel* model,
                               bool agent,
                               bool incognito)
    : shell_(shell),
      slot_(slot),
      framework_(framework),
      sidebar_host_(sidebar_host),
      spaces_host_(spaces_host),
      prefs_host_(prefs_host),
      agent_host_(agent_host),
      window_host_(window_host),
      model_(model),
      incognito_(incognito),
      agent_(agent) {}

// A live drag goes with the view: the session's destructor IS AbortDrag, so
// the drag image and the split-drop overlay come down by ownership rather
// than by two ad-hoc calls in the shell's own destructor
// (docs/architecture-review.md §3.5).
PhiSidebarView::~PhiSidebarView() = default;

HWND PhiSidebarView::shell_hwnd() const {
  return shell_->SidebarShellWindow();
}

// The disclosed folders are view state of THIS shell alone: no push comes
// for them, so the caller schedules its own rebuild.
bool PhiSidebarView::ToggleExpanded(const std::string& guid) {
  if (expanded_bookmarks_.erase(guid)) {
    return false;
  }
  expanded_bookmarks_.insert(guid);
  return true;
}

bool PhiSidebarView::OnStoredRowClick(POINT pt) {
  for (const ShortcutRow& pin : pinned_) {
    if (::PtInRect(&pin.bounds, pt)) {
      if (pin.attached_surface != SidebarSurface::kContent &&
          ::PtInRect(&pin.surface_box, pt)) {
        Apply(SidebarCloseSurface{pin.key});
        return true;
      }
      ArmDrag(pin.key, pt);
      OpenOrFocusPinned(pin.guid, pin.url);
      return true;
    }
  }
  for (const ShortcutRow& bookmark : bookmarks_) {
    if (::PtInRect(&bookmark.bounds, pt)) {
      if (bookmark.attached_surface != SidebarSurface::kContent &&
          ::PtInRect(&bookmark.surface_box, pt)) {
        Apply(SidebarCloseSurface{bookmark.key});
        return true;
      }
      ArmDrag(bookmark.key, pt);
      if (bookmark.folder) {
        // A folder click discloses/collapses its children (the Mac's
        // toggleItemExpanded). The armed drag is keyed, so the rebuild
        // cannot invalidate it.
        ToggleExpanded(bookmark.guid);
        shell_->SidebarScheduleRefresh();
      } else if (!bookmark.url.empty()) {
        // Sidebar bookmark rows bind like pins (bookmarkGuidBacking); the
        // Bookmarks menu keeps opening plain tabs.
        OpenOrFocusPinned(bookmark.guid, bookmark.url);
      }
      return true;
    }
  }
  return false;
}

bool PhiSidebarView::OnContentRowClick(POINT pt) {
  for (const GroupHeader& gh : group_headers_) {
    if (::PtInRect(&gh.bounds, pt)) {
      const BrowserId browser = shell_->SidebarActiveBrowser();
      if (framework_ && browser.valid()) {
        framework_->SetTabGroupCollapsed(browser.value, gh.id.c_str(),
                                         !gh.collapsed);
      }
      return true;
    }
  }
  for (const TabRow& row : tabs_) {
    if (!::PtInRect(&row.bounds, pt)) {
      continue;
    }
    if (framework_) {
      if (row.attached_surface != SidebarSurface::kContent &&
          ::PtInRect(&row.surface_box, pt)) {
        Apply(SidebarCloseSurface{row.key});  // The indicator's minus.
      } else if (::PtInRect(&row.close_box, pt)) {
        if (!agent_holds_input_) {  // The Mac refuses ✕ under the agent.
          Apply(SidebarClose{row.key, -1});  // A merged row: both panes.
        }
      } else if (row.split_id.empty() && (::GetKeyState(VK_MENU) & 0x8000)) {
        // Alt-click, the Mac's option-click: split this tab with the active
        // one (performSplitAction). Either pane may be bound to a pinned tab
        // or a bookmark — the split takes both into the normal list.
        Apply(SidebarFormSplit{SidebarKey::Tab(sidebar_tree_.active_tab),
                               row.key, "vertical"});
      } else if (row.b_index >= 0 && ::PtInRect(&row.pane_b, pt)) {
        Apply(SidebarActivate{row.key, 1});
      } else {
        // The press only ARMS: a tab activates on the release of a plain
        // click (the session's commit), never because it was dragged — the
        // Mac's outline selects on click, not on mouse-down, so dragging a
        // background tab leaves the page the user was reading in place.
        ArmDrag(row.key, pt);
        NoteDragClickTab(row.id);
      }
    }
    return true;
  }
  return false;
}

// Middle-click closes a tab row anywhere on the row (the Mac tab list's
// otherMouseDown), merged split rows included. Pins and bookmarks are
// persistent — a middle-click must not delete them, so only tab rows react.
bool PhiSidebarView::OnMiddleClick(POINT pt) {
  if (!framework_) {
    return false;
  }
  for (const TabRow& row : tabs_) {
    if (!::PtInRect(&row.bounds, pt)) {
      continue;
    }
    Apply(SidebarClose{row.key, -1});
    return true;
  }
  return false;
}

// --- The drag session -------------------------------------------------------
//
// It exists between here and the commit or the cancel, and nowhere else
// (docs/architecture-review.md §3.5): every field of a drag lives inside
// it, so a drag that ends cannot leave a flag behind.

void PhiSidebarView::ArmDrag(const SidebarKey& key, POINT pt) {
  drag_ = std::make_unique<PhiSidebarDragSession>(this, key, pt);
}

void PhiSidebarView::NoteDragClickTab(TabId tab_id) {
  if (drag_) {
    drag_->NoteClickTab(tab_id);
  }
}

void PhiSidebarView::UpdateDrag(POINT pt) {
  if (drag_) {
    drag_->Update(pt);
  }
}

void PhiSidebarView::CommitDrag() {
  if (!drag_) {
    return;
  }
  // Held while it commits: the drop can rebuild the rows under it, and the
  // session must outlive its own last statement. Its destructor then finds
  // a finished drag and aborts nothing.
  const std::unique_ptr<PhiSidebarDragSession> session = std::move(drag_);
  session->Commit();
}

void PhiSidebarView::AbortDrag() {
  drag_.reset();  // ~PhiSidebarDragSession IS AbortDrag.
}

bool PhiSidebarView::drag_active() const {
  return drag_ && drag_->active();
}

bool PhiSidebarView::OnSlideTimer() {
  if (const HWND shell = shell_hwnd()) {
    ::InvalidateRect(shell, nullptr, FALSE);
  }
  return drag_ && drag_->SlideRunning();
}

void PhiSidebarView::DrawDragIndicator(HDC dc) {
  if (drag_) {
    drag_->DrawIndicator(dc);
  }
}

PhiSlideVec PhiSidebarView::RowSlide(const std::string& guid) const {
  return drag_ ? drag_->RowSlideOffset(guid) : PhiSlideVec{};
}

double PhiSidebarView::TabSlide(TabId tab_id) const {
  return drag_ ? drag_->DragSlideOffset(tab_id) : 0.0;
}

RECT PhiSidebarView::RowHoverRect(HoverKind kind, int index) const {
  const auto at = [&](const auto& rows) {
    return index >= 0 && index < static_cast<int>(rows.size())
               ? rows[static_cast<size_t>(index)].bounds
               : RECT{};
  };
  switch (kind) {
    case HoverKind::kTab:
      return at(tabs_);
    case HoverKind::kPin:
      return at(pinned_);
    case HoverKind::kBookmark:
      return at(bookmarks_);
    case HoverKind::kGroupHeader:
      return at(group_headers_);
    default:
      return RECT{};  // Not one of the sidebar's own rows.
  }
}

// The rows as assistive technology and tooltips read them (§3.6, ranked
// finding 31): ONE hit per row, in paint order, named by what the row
// shows and keyed by its position in the band so a provider handed out
// before a rebuild resolves back to the same row afterwards. Built HERE,
// with the rows — never from paint (PhiWindow's contract).
void PhiSidebarView::PublishHits(const PhiSidebarRects& layout) {
  hits_.clear();
  const auto add = [&](const RECT& bounds, int key, std::wstring name,
                       PhiHitRole role, bool selected) {
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
        name.empty()) {
      return;
    }
    PhiHit hit;
    hit.bounds = {bounds.left, bounds.top, bounds.right, bounds.bottom};
    hit.key = key;
    hit.name = std::move(name);
    hit.role = role;
    hit.selected = selected;
    hits_.push_back(std::move(hit));
  };
  // The keys are banded so a pin, a bookmark, a header and a tab row at the
  // same position never collide.
  constexpr int kPinKey = 0x1000;
  constexpr int kBookmarkKey = 0x2000;
  constexpr int kHeaderKey = 0x3000;
  constexpr int kTabKey = 0x4000;
  for (size_t i = 0; i < pinned_.size(); ++i) {
    const ShortcutRow& pin = pinned_[i];
    add(pin.bounds, kPinKey + static_cast<int>(i), pin.title,
        PhiHitRole::kListItem, pin.active);
  }
  for (size_t i = 0; i < bookmarks_.size(); ++i) {
    const ShortcutRow& row = bookmarks_[i];
    // A folder is a container the outline opens, not a page: it announces
    // as a group, and its expanded state rides the selection flag.
    add(row.bounds, kBookmarkKey + static_cast<int>(i), row.title,
        row.folder ? PhiHitRole::kGroup : PhiHitRole::kListItem,
        row.folder ? row.expanded : row.active);
  }
  for (size_t i = 0; i < group_headers_.size(); ++i) {
    const GroupHeader& header = group_headers_[i];
    add(header.bounds, kHeaderKey + static_cast<int>(i), header.title,
        PhiHitRole::kGroup, !header.collapsed);
  }
  for (size_t i = 0; i < tabs_.size(); ++i) {
    const TabRow& row = tabs_[i];
    add(row.bounds, kTabKey + static_cast<int>(i), row.title,
        PhiHitRole::kTab, row.active);
    // The close box is its own button: a screen reader has to be able to
    // close a tab without simulating a click on a coordinate.
    add(row.close_box, kTabKey + static_cast<int>(i) + 0x800,
        PhiStr("app.closeTab"),
        PhiHitRole::kButton, false);
  }
  if (layout.new_tab_row.right > layout.new_tab_row.left) {
    add(layout.new_tab_row, kTabKey - 1, PhiStr("app.newTab"),
        PhiHitRole::kButton, false);
  }
  shell_->SidebarPublishHits(hits_);
}

// THE writer of the row vectors (docs/sidebar-architecture.md §2.2, §2.7):
// SidebarInputs -> BuildSidebar -> LayoutSidebar -> rows, in one pass, on
// the shell's posted rebuild turn. Nothing else fills tabs_, pinned_,
// bookmarks_ or group_headers_ — not paint, not input, not a menu.
void PhiSidebarView::Rebuild() {
  tabs_.clear();
  group_headers_.clear();
  // Re-captured from the active row below; empty when the strip is
  // (placeholder mode), so no stale URL keeps a native page mounted.
  active_tab_url_.clear();
  active_tab_id_ = TabId();
  if (!framework_) {
    PublishHits(PhiSidebarRects{});
    return;
  }
  const BrowserId browser = shell_->SidebarActiveBrowser();
  if (!browser.valid()) {
    // Between the last browser going and teardown: no rows.
    PublishHits(PhiSidebarRects{});
    return;
  }
  // ONE window read per rebuild — the model's copy of the last committed
  // snapshot (docs/architecture-review.md §3.3); every row below derives
  // from it and the tree. A browser the model has not heard of yet (adopted
  // before its first push) has an empty strip for this turn: the stored
  // cells still render, the push that follows fills the rest.
  const WindowSnapshot empty;
  const WindowSnapshot* known = model_ ? model_->Get(browser) : nullptr;
  const WindowSnapshot& snapshot = known ? *known : empty;

  // Group visuals for the header rows (title/color/collapsed by id).
  std::map<std::string, GroupHeader> group_info;
  std::map<std::string, std::wstring> group_color_labels;
  for (const phi_framework::GroupSnapshot& group : snapshot.groups) {
    GroupHeader info;
    info.id = group.id;
    const PhiGroupColor& palette_entry = PhiGroupColorFor(group.color);
    info.color = palette_entry.color;
    info.name = base::UTF8ToWide(group.title);
    // An unnamed group is titled by its colour and member count
    // ("Grey · 2 tabs", displayTitle(memberCount:)); the count is filled
    // in once the strip has been walked.
    info.title = info.name;
    group_color_labels[info.id] =
        PhiStr((std::string("tabs.tabGroup.colorName.") +
                palette_entry.name).c_str());
    info.collapsed = group.collapsed;
    group_info[info.id] = std::move(info);
  }

  // The model: what every live tab IS — plain, pin-bound, bookmark-bound,
  // split pane, surface, grouped (docs/sidebar-architecture.md §4.5). A
  // reading surface still in the strip is never a row; an active one hands
  // the selection, and the address bar, to the tab it is covering.
  {
    SidebarInputs in;
    if (sidebar_host_ && prefs_host_) {
      in.stored = sidebar_host_->SidebarStoredItems(slot_.value);
      in.surfaces = sidebar_host_->AttachedSurfaces(browser.value);
      in.policy.horizontal_layout =
          prefs_host_->LayoutMode() == PhiLayoutMode::kComfortable;
    }
    in.policy.incognito = incognito_;
    for (const TabSnapshot& tab : snapshot.tabs) {
      in.strip.push_back({tab.id.value, tab.index, tab.tag, tab.split_id,
                          tab.group_id, tab.active, PhiSurfaceOf(tab),
                          tab.origin.value});
    }
    for (const auto& [id, info] : group_info) {
      in.groups.push_back({id, info.collapsed});
    }
    sidebar_tree_ = BuildSidebar(in);
  }
  active_tab_id_ = TabId(sidebar_tree_.active_tab);
  if (const TabSnapshot* active = PhiFindTabIn(snapshot, active_tab_id_)) {
    // The bookmark bar's "show on new tab" and the native new-tab page
    // read this.
    active_tab_url_ = active->url;
  }
  // The bookmark bar and the reader slot follow the active tab: re-read
  // them once here, before the layout the rows are placed in is derived.
  shell_->SidebarRefreshLayoutInputs();

  // The two stored bands' sizes feed the layout, and they come from the
  // tree — the pinned grid's row count and the outline's visible rows — so
  // ComputeLayout runs ONCE here, before any row is placed. (It used to be
  // called from inside the shortcut rebuild it depended on.)
  const bool comfortable =
      prefs_host_ && prefs_host_->LayoutMode() == PhiLayoutMode::kComfortable;
  {
    std::vector<int> units;
    units.reserve(sidebar_tree_.pins.size());
    for (const SidebarCell& pin : sidebar_tree_.pins) {
      units.push_back(pin.split ? 2 : 1);
    }
    pinned_grid_rows_ =
        comfortable || incognito_ || agent_
            ? 0
            : LayoutPinnedGrid(units, GridMetrics(PhiSidebarRects{})).rows;
    shell_->SidebarSetRowCounts(
        pinned_grid_rows_,
        incognito_ || agent_
            ? 0
            : static_cast<int>(VisibleBookmarkCells().size()));
  }
  const PhiSidebarRects layout = shell_->SidebarRects();
  RebuildShortcuts(layout, snapshot);

  // One paint row per content node; a split node is ONE merged row.
  const auto fill_pane = [&](const TabSnapshot& tab, std::wstring* title,
                             std::wstring* url,
                             std::vector<unsigned char>* favicon, int* fw,
                             int* fh) {
    std::string label = !tab.title.empty()
                            ? tab.title
                            : (!tab.url.empty() ? tab.url
                                                : std::string("(untitled)"));
    if (PhiBookmarkManager::IsManagerUrl(tab.url)) {
      // The Chromium view behind the native page has no WebUI to load, so
      // its title would be the failed URL; the tab shows the page's name.
      label = "Bookmarks";
    }
    *title = base::UTF8ToWide(label);
    if (incognito_ && PhiNewTabPage::IsNewTabUrl(tab.url)) {
      // The Mac's incognito new-tab title (Tab.incognitoNewTabTitle) in
      // place of Chromium's plain "New Tab".
      *title = PhiStr("common.newTabPage.incognitoTitle");
    }
    *url = base::UTF8ToWide(tab.url);
    int w = 0, h = 0;
    const unsigned char* pixels =
        framework_->TabFaviconBgraById(browser.value, tab.id.value, &w, &h);
    if (pixels && w > 0 && h > 0) {
      // The framework's buffer is scratch — valid only until its next call.
      const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
      // SAFETY: TabFaviconBgraById reports the dimensions of the buffer it
      // just returned.
      const auto copy =
          UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
      favicon->assign(copy.begin(), copy.end());
      *fw = w;
      *fh = h;
    }
  };
  const auto make_row = [&](const SidebarContentNode& node) {
    TabRow row;
    const TabSnapshot* a = PhiFindTabIn(snapshot, TabId(node.tab_a));
    if (!a) {
      return row;
    }
    row.key = node.key;
    row.attached_surface = node.attached_surface;
    row.index = a->index;
    row.id = a->id;
    row.active = node.active;
    row.split_id = a->split_id;
    row.group_id = node.group_id;
    row.tag = a->tag;
    row.internal_page = a->url.rfind("chrome://", 0) == 0;
    row.unloaded = a->unloaded;
    fill_pane(*a, &row.title, &row.url, &row.favicon, &row.favicon_width,
              &row.favicon_height);
    if (node.kind == SidebarNodeKind::kSplit) {
      if (const TabSnapshot* b = PhiFindTabIn(snapshot, TabId(node.tab_b))) {
        row.b_index = b->index;
        row.b_id = b->id;
        row.b_active = b->active;
        fill_pane(*b, &row.b_title, &row.b_url, &row.b_favicon,
                  &row.b_favicon_width, &row.b_favicon_height);
      }
    }
    return row;
  };

  // The native-page routes (bookmark manager, new-tab page) can only be
  // entered or left by a rebuild; mounting a page here would run inside
  // WM_PAINT, so post and do it after.
  shell_->SidebarSyncNativePages();

  // Geometry for the content nodes.
  int x = layout.tabs_left;
  int y = layout.tabs_top;
  if (layout.horizontal_tabs && !pinned_.empty()) {
    // The strip's pinned section leads; normal chips start after it.
    x = pinned_.back().bounds.right + Scaled(kStripPinToTabs);
  }
  // Chrome's strip shares its room out: tabs take their ideal width while
  // they fit and shrink toward the minimum as more open. Fixed widths meant
  // the tabs past the edge were simply dropped — and the ACTIVE tab was
  // among them as soon as a fourth opened, which is not a strip at all.
  // Resting tabs give up their width first; the active one keeps its wider
  // slot until they are all at the floor.
  StripWidths strip_widths{Scaled(kHTabWidth), Scaled(kHTabActiveWidth)};
  if (layout.horizontal_tabs) {
    int resting = 0;
    bool has_active = false;
    for (const SidebarContentNode& node : sidebar_tree_.content) {
      if (node.kind == SidebarNodeKind::kGroup) {
        continue;  // No headers on the strip.
      }
      if (node.active) {
        has_active = true;
      } else {
        ++resting;
      }
    }
    strip_widths = SolveStripWidths(resting, has_active, layout.tabs_limit - x,
                                    Scaled(kHTabWidth),
                                    Scaled(kHTabActiveWidth),
                                    Scaled(kHTabMinWidth));
  }
  const auto finish_row = [&](TabRow* row) {
    const int close_box = Scaled(kCloseBoxSize);
    const int box_mid = (row->bounds.top + row->bounds.bottom) / 2;
    row->close_box = {row->bounds.right - close_box - Scaled(4),
                      box_mid - close_box / 2, row->bounds.right - Scaled(4),
                      box_mid + close_box / 2};
    if (row->attached_surface != SidebarSurface::kContent) {
      // The indicator takes the trailing slot; the row's own close box is
      // withheld while a surface is attached (its minus closes the surface).
      // A row scrolled out of the viewport has no slot to take.
      row->surface_box = row->bounds.right > row->bounds.left ? row->close_box
                                                              : RECT{};
      row->close_box = RECT{};
    }
    if (row->b_index >= 0) {
      // Right half of the merged row activates pane B (clicks in the close
      // box still win — it is tested first).
      row->pane_b = {(row->bounds.left + row->bounds.right) / 2,
                     row->bounds.top, row->bounds.right, row->bounds.bottom};
    }
  };
  int list_scroll_max = 0;
  if (layout.horizontal_tabs) {
    // The strip: chips abut left to right; no headers, no collapse.
    for (const SidebarContentNode& node : sidebar_tree_.content) {
      if (node.kind == SidebarNodeKind::kGroup) {
        continue;
      }
      TabRow row = make_row(node);
      if (!row.id.valid()) {
        continue;
      }
      // The active tab takes the wider slot — it is the one carrying the
      // page, and the extra room is what separates it from its neighbours
      // now that the tabs abut instead of standing apart.
      const int tab_w = row.active ? strip_widths.active : strip_widths.rest;
      if (x + tab_w > layout.tabs_limit) {
        break;  // Strip is full; overflow handling is future work.
      }
      // The tab runs from the strip's top inset to the band's bottom edge,
      // where the active one merges into the toolbar.
      row.bounds = {x, y, x + tab_w, y + Scaled(kHTabHeight)};
      x += tab_w + Scaled(kHTabGap);
      finish_row(&row);
      tabs_.push_back(std::move(row));
    }
  } else {
    // The list: a header row before each group's members, members hidden
    // under a collapsed header, everything scrolled and clipped to the
    // tab-list viewport (LayoutList).
    std::vector<ListItem> items;
    std::vector<const SidebarContentNode*> placed;
    std::map<std::string, int> member_counts;
    for (const SidebarContentNode& node : sidebar_tree_.content) {
      if (node.kind == SidebarNodeKind::kGroup) {
        if (!group_info.count(node.group_id)) {
          continue;  // No visuals for it yet: members render flat.
        }
        items.push_back({ListItem::Kind::kHeader, false});
      } else {
        items.push_back({ListItem::Kind::kRow, node.hidden});
        if (!node.group_id.empty()) {
          ++member_counts[node.group_id];
        }
      }
      placed.push_back(&node);
    }
    // The card's inner insets: an expanded header opens the top inset
    // (less the row gap already there) below itself, the group's last
    // visible member the bottom one. A collapsed card is its header.
    for (size_t i = 0; i < placed.size(); ++i) {
      const SidebarContentNode& node = *placed[i];
      if (node.kind == SidebarNodeKind::kGroup) {
        if (!node.collapsed) {
          items[i].pad_below =
              std::max(0, Scaled(kGroupInnerInset) - Scaled(kTabRowGap));
        }
        continue;
      }
      if (node.group_id.empty() || node.hidden) {
        continue;
      }
      const bool last_member =
          i + 1 == placed.size() ||
          placed[i + 1]->kind == SidebarNodeKind::kGroup ||
          placed[i + 1]->group_id != node.group_id;
      if (last_member) {
        items[i].pad_below = Scaled(kGroupInnerInset);
      }
    }
    ListMetrics m;
    m.left = layout.tabs_left;
    m.right = Scaled(sidebar_dip() - kEdgeInset);
    m.top = layout.tabs_top;
    m.row_height = Scaled(kTabRowHeight);
    m.row_gap = Scaled(kTabRowGap);
    m.header_height = Scaled(kGroupHeaderHeight);
    m.scroll = scroll_offset_;
    m.clip = true;
    m.viewport_top = layout.tabs_top;
    m.viewport_bottom = layout.tabs_limit;
    const ListPlacement list = LayoutList(items, m);
    list_scroll_max = list.scroll_max;
    for (size_t i = 0; i < placed.size(); ++i) {
      const SidebarContentNode& node = *placed[i];
      if (node.kind == SidebarNodeKind::kGroup) {
        GroupHeader header = group_info.at(node.group_id);
        if (header.name.empty()) {
          header.title = PhiStrF(
              "tabs.tabGroup.summaryWithTabCount",
              {group_color_labels[node.group_id],
               std::to_wstring(member_counts[node.group_id])});
        }
        header.bounds = PhiToRect(list.rects[i]);
        // The tinted card starts at the header's placed edge even when the
        // header itself is scrolled out; member rows extend it below.
        header.container = PhiToRect(list.unclipped[i]);
        group_headers_.push_back(std::move(header));
        continue;
      }
      if (node.hidden) {
        continue;  // Hidden under a collapsed header.
      }
      TabRow row = make_row(node);
      if (!row.id.valid()) {
        continue;
      }
      row.bounds = PhiToRect(list.rects[i]);
      const bool in_card = !row.group_id.empty() && !group_headers_.empty() &&
                           group_headers_.back().id == row.group_id;
      if (in_card && row.bounds.right > row.bounds.left) {
        // A member row sits inside the card: its own edgesSpacing from the
        // card's leading edge, the inner table's inset from the trailing.
        row.bounds.left += Scaled(kGroupMemberLeadInset);
        row.bounds.right -= Scaled(kGroupMemberTrailInset);
      }
      finish_row(&row);
      if (in_card && row.bounds.right > row.bounds.left) {
        group_headers_.back().container.bottom =
            row.bounds.bottom + items[i].pad_below;
      }
      tabs_.push_back(std::move(row));
    }
  }
  if (layout.horizontal_tabs) {
    // The "+" follows the last tab, centred on the strip's line — the tabs
    // run from tabs_top to the band's bottom edge, and the band's other
    // controls are centred on that same line. It only exists once the
    // strip's width is known, which is here and not in the layout.
    const int btn = Scaled(kNavButton);
    const int gap = Scaled(kTopBarSpacing);
    const int left = x + gap;
    const int strip_mid = (layout.tabs_top + layout.title_bar.bottom) / 2;
    // tabs_limit already held this button's room back from the strip, so it
    // fits unless the tabs overran — clamp rather than draw under search.
    add_tab_button_ = left + btn + gap <= layout.search_tabs.left
                          ? RECT{left, strip_mid - btn / 2, left + btn,
                                 strip_mid + btn / 2}
                          : RECT{};
  } else {
    add_tab_button_ = RECT{};
    // Overflow bookkeeping for the wheel handler; the tinted group cards are
    // clamped to the viewport so they never bleed into the fixed bands.
    tabs_scroll_max_ = list_scroll_max;
    if (scroll_offset_ > tabs_scroll_max_) {
      scroll_offset_ = tabs_scroll_max_;
    }
    for (GroupHeader& gh : group_headers_) {
      gh.container.top =
          std::max(gh.container.top, static_cast<LONG>(layout.tabs_top));
      gh.container.bottom =
          std::min(gh.container.bottom, static_cast<LONG>(layout.tabs_limit));
      if (gh.container.bottom <= gh.container.top) {
        gh.container = RECT{};
      }
    }
  }
  // MRU bookkeeping for the Ctrl+Tab switcher (the rule itself is pure and
  // host-tested): the active tab moves to the front; ids that left the
  // strip fall out.
  {
    TabId active_id;
    std::vector<TabId> live;
    live.reserve(tabs_.size());
    for (const TabRow& row : tabs_) {
      if (row.active && !active_id.valid()) {
        active_id = row.id;
      }
      live.push_back(row.id);
      if (row.b_index >= 0) {
        live.push_back(row.b_id);
      }
    }
    tab_mru_.Sync(live, active_id);
  }
  // The rows the pointer, the keyboard and a screen reader all address.
  PublishHits(layout);
}

void PhiSidebarView::RebuildShortcuts(const PhiSidebarRects& layout,
                                      const WindowSnapshot& snapshot) {
  pinned_.clear();
  bookmarks_.clear();
  if (!sidebar_host_ || incognito_ || agent_) {
    return;  // Nothing persists from an incognito or agent window.
  }
  const auto consume = [](PhiStoredRowView view) {
    ShortcutRow row;
    row.title =
        base::UTF8ToWide(!view.title.empty() ? view.title : view.url);
    row.guid = std::move(view.guid);
    row.url = std::move(view.url);
    row.folder = view.folder;
    row.favicon = std::move(view.favicon_bgra);
    row.favicon_width = view.favicon_width;
    row.favicon_height = view.favicon_height;
    row.url_b = std::move(view.url_b);
    row.title_b = base::UTF8ToWide(view.title_b);
    row.favicon_b = std::move(view.favicon_b_bgra);
    row.favicon_b_width = view.favicon_b_width;
    row.favicon_b_height = view.favicon_b_height;
    row.icon = std::move(view.icon);
    row.layout = std::move(view.layout);
    return row;
  };
  // The tree decides what a cell IS (open, active, which live panes); the
  // store view only supplies what it looks like. Whether a bound pane is
  // dormant is the live tab's own state, read from the snapshot the tree
  // was built from.
  const auto dormant = [&](int tab_id) {
    const TabSnapshot* tab = PhiFindTabIn(snapshot, TabId(tab_id));
    return tab && tab->unloaded;
  };
  const auto bind = [&](const SidebarCell& cell, ShortcutRow* row) {
    row->open = cell.binding.open();
    row->active = cell.binding.active;
    row->tab_id = TabId(cell.binding.tab_a);
    row->tab_id_b = TabId(cell.binding.tab_b);
    row->unloaded = dormant(cell.binding.tab_a);
    row->unloaded_b = dormant(cell.binding.tab_b);
    row->attached_surface = cell.attached_surface;
  };
  std::map<std::string, PhiStoredRowView> pin_views;
  for (PhiStoredRowView& view : sidebar_host_->PinnedRows(slot_.value)) {
    pin_views.emplace(view.guid, std::move(view));
  }
  for (const SidebarCell& cell : sidebar_tree_.pins) {
    const auto view = pin_views.find(cell.guid);
    if (view == pin_views.end()) {
      continue;
    }
    ShortcutRow row = consume(std::move(view->second));
    row.key = cell.key;
    bind(cell, &row);
    pinned_.push_back(std::move(row));
  }
  // The outline (the Mac's NSOutlineView): the tree's depth-first order,
  // only the cells an expanded chain reveals. Views come per parent.
  std::map<std::string, std::map<std::string, PhiStoredRowView>> by_parent;
  for (const SidebarCell* cell : VisibleBookmarkCells()) {
    auto& siblings = by_parent[cell->parent_guid];
    if (siblings.empty()) {
      for (PhiStoredRowView& view :
           sidebar_host_->BookmarkRows(slot_.value, cell->parent_guid)) {
        siblings.emplace(view.guid, std::move(view));
      }
    }
    const auto view = siblings.find(cell->guid);
    if (view == siblings.end()) {
      continue;
    }
    ShortcutRow row = consume(std::move(view->second));
    row.key = cell->key;
    row.depth = cell->depth;
    row.parent_guid = cell->parent_guid;
    row.expanded = row.folder && expanded_bookmarks_.count(row.guid) > 0;
    bind(*cell, &row);
    bookmarks_.push_back(std::move(row));
  }

  // A split cell stores pane B's favicon since schema 2; rows folded from
  // older files may lack it — those fall back to the history cache (misses
  // schedule an async load that repaints on arrival).
  const int64_t icon_browser = shell_->SidebarActiveBrowser().value;
  if (framework_ && icon_browser) {
    const auto fill_favicon_b = [&](ShortcutRow& row) {
      if (row.url_b.empty() || !row.favicon_b.empty()) {
        return;
      }
      int width = 0, height = 0;
      const unsigned char* pixels = framework_->FaviconForUrlBgra(
          icon_browser, row.url_b.c_str(), &width, &height);
      if (pixels && width > 0 && height > 0) {
        const size_t bytes =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        // SAFETY: FaviconForUrlBgra reports its own buffer's dimensions.
        const auto view =
            UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
        row.favicon_b.assign(view.begin(), view.end());
        row.favicon_b_width = width;
        row.favicon_b_height = height;
      }
    };
    for (ShortcutRow& pin : pinned_) {
      fill_favicon_b(pin);
    }
    for (ShortcutRow& bookmark : bookmarks_) {
      fill_favicon_b(bookmark);
    }
  }

  if (layout.horizontal_tabs) {
    // No sidebar: pins become the strip's leading pinned section — fixed
    // 28pt cells, 2pt apart, centred on the strip row (the Mac's
    // TabStripMetrics.PinnedTab). Normal chips start after them.
    const int cell = Scaled(kStripPinSize);
    const int mid_y = layout.tabs_top + Scaled(kHTabHeight) / 2;
    int x = layout.tabs_left;
    for (ShortcutRow& pin : pinned_) {
      // A merged split cell takes two strip slots.
      const int w = pin.url_b.empty() ? cell : 2 * cell + Scaled(kStripPinGap);
      pin.bounds = {x, mid_y - cell / 2, x + w, mid_y + cell / 2};
      x += w + Scaled(kStripPinGap);
    }
    const int by = layout.favorites.bottom;
    for (ShortcutRow& row : bookmarks_) {
      row.bounds = {0, by, 0, by};  // Off-strip; bookmarks live in the menu.
    }
    return;
  }

  // The pinned grid (the Mac's PinnedTabLayout) — LayoutPinnedGrid, shared
  // with the drag previews. A split cell spans two units.
  {
    std::vector<int> units;
    units.reserve(pinned_.size());
    for (const ShortcutRow& pin : pinned_) {
      units.push_back(pin.url_b.empty() ? 1 : 2);
    }
    const PinnedGridPlacement grid = LayoutPinnedGrid(units, GridMetrics(layout));
    for (size_t i = 0; i < pinned_.size(); ++i) {
      ShortcutRow& pin = pinned_[i];
      pin.bounds = PhiToRect(grid.cells[i]);
      if (pin.attached_surface != SidebarSurface::kContent) {
        // The Mac's PinnedPeekBadgeView: a plate on the cell's top-right.
        const int badge = Scaled(16);
        pin.surface_box = {pin.bounds.right - badge - Scaled(2),
                           pin.bounds.top + Scaled(2), pin.bounds.right - Scaled(2),
                           pin.bounds.top + Scaled(2) + badge};
      }
    }
  }

  // Bookmarks are ordinary rows above the open tabs (36pt + the 2pt gap).
  {
    ListMetrics m;
    m.left = layout.tabs_left;
    m.right = Scaled(sidebar_dip() - kEdgeInset);
    m.top = layout.favorites.bottom +
            (pinned_grid_rows_ > 0 ? Scaled(kFavoritesSpacer) : 0);
    m.row_height = Scaled(kTabRowHeight);
    m.row_gap = Scaled(kTabRowGap);
    const std::vector<ListItem> items(bookmarks_.size(), ListItem{});
    const ListPlacement placed = LayoutList(items, m);
    for (size_t i = 0; i < bookmarks_.size(); ++i) {
      ShortcutRow& row = bookmarks_[i];
      row.bounds = PhiToRect(placed.rects[i]);
      if (row.attached_surface != SidebarSurface::kContent &&
          row.bounds.right > row.bounds.left) {
        const int box = Scaled(kCloseBoxSize);
        const int mid = (row.bounds.top + row.bounds.bottom) / 2;
        row.surface_box = {row.bounds.right - box - Scaled(4), mid - box / 2,
                           row.bounds.right - Scaled(4), mid + box / 2};
      }
    }
  }
}

PinnedGridMetrics PhiSidebarView::GridMetrics(
    const PhiSidebarRects& layout) const {
  PinnedGridMetrics m;
  m.sidebar_dip = sidebar_dip();
  m.edge_inset_dip = kEdgeInset;
  m.gap_dip = kPinnedGap;
  m.min_cell_width_dip = kPinnedMinWidth;
  m.origin_x = layout.favorites.left + Scaled(kEdgeInset);
  m.origin_y = layout.favorites.top;
  m.sidebar_px = Scaled(sidebar_dip());
  m.edge_inset_px = Scaled(kEdgeInset);
  m.gap_px = Scaled(kPinnedGap);
  m.cell_height_px = Scaled(kPinnedCellHeight);
  return m;
}

std::vector<RECT> PhiSidebarView::PinnedGridWalk(
    const std::vector<int>& units, const PhiSidebarRects& layout) const {
  // The pinned grid's placement (LayoutPinnedGrid), shared with the drag
  // previews so a phantom cell lands where the real one will.
  std::vector<RECT> rects;
  for (const SidebarRect& cell : LayoutPinnedGrid(units, GridMetrics(layout)).cells) {
    rects.push_back(PhiToRect(cell));
  }
  return rects;
}

// The outline rule itself is pure and host-tested; this hands back the
// cells it names.
std::vector<const SidebarCell*> PhiSidebarView::VisibleBookmarkCells()
    const {
  std::vector<const SidebarCell*> visible;
  for (const size_t row :
       PhiVisibleOutlineRows(sidebar_tree_.bookmarks, expanded_bookmarks_)) {
    visible.push_back(&sidebar_tree_.bookmarks[row]);
  }
  return visible;
}


bool PhiSidebarView::Apply(const SidebarCommand& command) {
  return sidebar_host_ &&
         sidebar_host_->Apply(slot_.value, command, sidebar_tree_);
}

std::vector<DropRow> PhiSidebarView::TabDropRows() const {
  // The tree's content order with each node's placed rect (empty when it is
  // hidden or scrolled out), headers included so group runs are visible.
  std::vector<DropRow> rows;
  for (const SidebarContentNode& node : sidebar_tree_.content) {
    DropRow row;
    row.key = node.key;
    row.group_id = node.group_id;
    row.group_header = node.kind == SidebarNodeKind::kGroup;
    if (row.group_header) {
      for (const GroupHeader& header : group_headers_) {
        if (header.id == node.group_id) {
          row.bounds = {header.bounds.left, header.bounds.top,
                        header.bounds.right, header.bounds.bottom};
        }
      }
    } else {
      for (const TabRow& tab : tabs_) {
        if (tab.key == node.key) {
          row.bounds = {tab.bounds.left, tab.bounds.top, tab.bounds.right,
                        tab.bounds.bottom};
        }
      }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<DropRow> PhiSidebarView::PinnedDropRows() const {
  std::vector<DropRow> rows;
  for (const ShortcutRow& pin : pinned_) {
    DropRow row;
    row.key = pin.key;
    row.bounds = {pin.bounds.left, pin.bounds.top, pin.bounds.right,
                  pin.bounds.bottom};
    rows.push_back(std::move(row));
  }
  return rows;
}

std::vector<DropRow> PhiSidebarView::BookmarkDropRows() const {
  std::vector<DropRow> rows;
  for (const ShortcutRow& bookmark : bookmarks_) {
    DropRow row;
    row.key = bookmark.key;
    row.bounds = {bookmark.bounds.left, bookmark.bounds.top,
                  bookmark.bounds.right, bookmark.bounds.bottom};
    row.depth = bookmark.depth;
    row.folder = bookmark.folder;
    row.expanded = bookmark.expanded;
    row.parent_guid = bookmark.parent_guid;
    rows.push_back(std::move(row));
  }
  return rows;
}

// "Which window shows tab X" — answered from the TREE, not from the paint
// rows (docs/architecture-review.md §3.5; lane 1 §2.1 item 4). Every live
// content tab of this window appears in the tree exactly once: as a content
// node, or in owner_of, hidden inside the pinned or bookmark cell that owns
// it. Walking tabs_ answered only the first kind, so a reader surface
// opened over a PINNED tab's page found no window at all.
bool PhiSidebarView::ShowsTab(TabId tab_id) const {
  if (!tab_id.valid()) {
    return false;
  }
  for (const TabRow& row : tabs_) {
    if (row.id == tab_id || (row.b_index >= 0 && row.b_id == tab_id)) {
      return true;
    }
  }
  return sidebar_tree_.owner_of.count(tab_id.value) > 0;
}

base::ListValue PhiSidebarView::LayoutRowsJson() const {
  base::ListValue rows;
  const auto surface_name = [](SidebarSurface kind) {
    return kind == SidebarSurface::kPeek     ? "peek"
           : kind == SidebarSurface::kReader ? "reader"
                                             : "";
  };
  const auto box = [](const RECT& r) {
    base::DictValue d;
    d.Set("left", static_cast<int>(r.left));
    d.Set("top", static_cast<int>(r.top));
    d.Set("right", static_cast<int>(r.right));
    d.Set("bottom", static_cast<int>(r.bottom));
    return d;
  };
  const auto emit = [&](const char* band, const SidebarKey& key,
                        const RECT& r, base::DictValue extra) {
    base::DictValue row = std::move(extra);
    row.Set("band", band);
    row.Set("key", key.ToString());
    row.Set("left", static_cast<int>(r.left));
    row.Set("top", static_cast<int>(r.top));
    row.Set("right", static_cast<int>(r.right));
    row.Set("bottom", static_cast<int>(r.bottom));
    rows.Append(std::move(row));
  };
  for (const ShortcutRow& pin : pinned_) {
    base::DictValue extra;
    extra.Set("guid", pin.guid);
    extra.Set("open", pin.open);
    extra.Set("tabId", pin.tab_id.value);
    extra.Set("unloaded", pin.unloaded);
    extra.Set("unloadedB", pin.unloaded_b);
    extra.Set("surface", surface_name(pin.attached_surface));
    extra.Set("surfaceBox", box(pin.surface_box));
    emit("pinned", pin.key, pin.bounds, std::move(extra));
  }
  for (const ShortcutRow& bookmark : bookmarks_) {
    base::DictValue extra;
    extra.Set("guid", bookmark.guid);
    extra.Set("folder", bookmark.folder);
    extra.Set("expanded", bookmark.expanded);
    extra.Set("depth", bookmark.depth);
    extra.Set("parentGuid", bookmark.parent_guid);
    extra.Set("open", bookmark.open);
    extra.Set("tabId", bookmark.tab_id.value);
    extra.Set("unloaded", bookmark.unloaded);
    extra.Set("unloadedB", bookmark.unloaded_b);
    extra.Set("surface", surface_name(bookmark.attached_surface));
    extra.Set("surfaceBox", box(bookmark.surface_box));
    emit("bookmarks", bookmark.key, bookmark.bounds, std::move(extra));
  }
  for (const GroupHeader& header : group_headers_) {
    base::DictValue extra;
    extra.Set("groupId", header.id);
    extra.Set("collapsed", header.collapsed);
    emit("tabs", SidebarKey::Group(header.id), header.bounds, std::move(extra));
  }
  for (const TabRow& tab : tabs_) {
    base::DictValue extra;
    extra.Set("tabId", tab.id.value);
    extra.Set("tabIdB", tab.b_id.value);
    extra.Set("groupId", tab.group_id);
    extra.Set("active", tab.active);
    extra.Set("unloaded", tab.unloaded);
    extra.Set("surface", surface_name(tab.attached_surface));
    extra.Set("surfaceBox", box(tab.surface_box));
    emit("tabs", tab.key, tab.bounds, std::move(extra));
  }
  return rows;
}

bool PhiSidebarView::OnWheel(POINT client_pt, int wheel_delta) {
  if (!prefs_host_ ||
      prefs_host_->LayoutMode() == PhiLayoutMode::kComfortable) {
    return false;
  }
  const PhiSidebarRects layout = shell_->SidebarRects();
  PhiSpacesStrip* const strip = shell_->SidebarSpacesStrip();
  if (::PtInRect(&layout.spaces_strip, client_pt)) {
    // Over the strip the wheel slides the pip viewport by whole pips, one
    // per notch (the Mac's SpacesStripWheelTracker): a notch toward the
    // user reveals later pips, the direction a vertical list would scroll.
    return strip->OnWheel(wheel_delta);
  }
  if (!::PtInRect(&layout.sidebar, client_pt) || tabs_scroll_max_ <= 0) {
    return false;
  }
  const int step = Scaled(kTabRowHeight + kTabRowGap);
  int next = scroll_offset_ - wheel_delta * step / WHEEL_DELTA;
  next = std::max(0, std::min(next, tabs_scroll_max_));
  if (next != scroll_offset_) {
    scroll_offset_ = next;
    shell_->SidebarScheduleRefresh();
  }
  return true;
}

void PhiSidebarView::OpenOrFocusPinned(const std::string& guid,
                                            const std::string& url) {
  if (!framework_ || guid.empty()) {
    return;
  }
  // Split rows route through their own open path (the Mac's
  // openOrFocusPinnedTab pair branch / openBookmark secondary branch).
  for (const ShortcutRow& pin : pinned_) {
    if (!pin.url_b.empty() && pin.guid == guid) {
      OpenStoredSplit(pin);
      return;
    }
  }
  for (const ShortcutRow& bookmark : bookmarks_) {
    if (!bookmark.url_b.empty() && bookmark.guid == guid) {
      OpenStoredSplit(bookmark);
      return;
    }
  }
  const BrowserId browser = shell_->SidebarActiveBrowser();
  if (!browser.valid()) {
    return;
  }
  // The strip as it IS, not as the model last heard it: the row may have
  // been opened a moment ago by this very gesture, and a second open would
  // leave a duplicate bound tab beside the first.
  WindowSnapshot strip;
  if (const TabSnapshot* bound =
          shell_->SidebarPullSnapshot(browser, &strip)
              ? PhiFindTagIn(strip, guid)
              : nullptr) {
    framework_->ActivateTabById(browser.value, bound->id.value);
    return;  // The framework's push schedules the rebuild.
  }
  if (url.empty()) {
    return;
  }
  const TabId created = framework_->OpenTab(browser, url.c_str());
  if (created.valid()) {
    // The Mac's createTab(customGuid:): the new tab carries the row's guid
    // as its identity, and session restore carries it across restarts.
    framework_->SetTabTagById(browser.value, created.value, guid.c_str());
  }
}

void PhiSidebarView::OpenStoredSplit(const ShortcutRow& row) {
  const BrowserId browser = shell_->SidebarActiveBrowser();
  WindowSnapshot strip;
  if (!framework_ || !browser.valid() ||
      !shell_->SidebarPullSnapshot(browser, &strip)) {
    return;
  }
  // Each pane binds by tag: TagFor(guid, false) for pane A, TagFor(guid,
  // true) for pane B — the same for a pinned cell and a split bookmark. The
  // strip is pulled fresh for the reason OpenOrFocusPinned pulls it.
  const auto lookup = [&](const std::string& tag, TabId* id,
                          std::string* split) {
    *id = TabId();
    split->clear();
    if (const TabSnapshot* tab = PhiFindTagIn(strip, tag)) {
      *id = tab->id;
      *split = tab->split_id;
    }
  };
  TabId id_a, id_b;
  std::string split_a, split_b;
  lookup(TagFor(row.guid, false), &id_a, &split_a);
  lookup(TagFor(row.guid, true), &id_b, &split_b);
  // Ensure BOTH panes live (a created pane binds by its own pane tag), then
  // marry them if Chromium lost the split — the Mac's openPinnedSplit /
  // openTwoURLsAsSplit in one path. A pane already in a DIFFERENT split is
  // left alone: the cell reads as partial and nothing is torn apart here.
  // OpenTab answers with the STABLE id, so nothing is re-read between the
  // two opens.
  if (!id_a.valid() && !row.url.empty()) {
    id_a = framework_->OpenTab(browser, row.url.c_str());
    if (id_a.valid()) {
      framework_->SetTabTagById(browser.value, id_a.value,
                                TagFor(row.guid, false).c_str());
    }
  }
  if (!id_b.valid() && !row.url_b.empty()) {
    id_b = framework_->OpenTab(browser, row.url_b.c_str());
    if (id_b.valid()) {
      framework_->SetTabTagById(browser.value, id_b.value,
                                TagFor(row.guid, true).c_str());
    }
  }
  if (!id_a.valid() || !id_b.valid()) {
    if (id_a.valid() || id_b.valid()) {
      framework_->ActivateTabById(browser.value,
                                  (id_a.valid() ? id_a : id_b).value);
    }
    return;
  }
  framework_->ActivateTabById(browser.value, id_a.value);
  if (split_a.empty() && split_b.empty()) {
    // Married in the axis the cell remembers; the framework's default is
    // side-by-side, and Chromium picks the leading pane itself.
    framework_->CreateSplitViewForTabs(
        browser.value, id_a.value, id_b.value,
        row.layout == "horizontal" ? "horizontal" : "vertical");
  }
}

std::string PhiSidebarView::FormSplitOrdered(BrowserId browser,
                                                  TabId primary_id,
                                                  TabId secondary_id,
                                                  const char* layout) {
  if (!framework_ || !primary_id.valid() || !secondary_id.valid() ||
      primary_id == secondary_id) {
    return std::string();
  }
  std::string split(framework_->CreateSplitViewForTabs(
      browser.value, primary_id.value, secondary_id.value, layout));
  if (split.empty()) {
    return split;
  }
  // Chromium normalizes the primary pane to the LOWER-indexed tab, so the
  // pair can land the other way round from what the drop asked for. Reversing
  // only reorders the strip — no tab is re-activated, so no pane blinks. The
  // split was created a line ago, so the model cannot know it yet: pull.
  WindowSnapshot fresh;
  if (!shell_->SidebarPullSnapshot(browser, &fresh)) {
    return split;
  }
  if (const SplitSnapshot* made = PhiFindSplitIn(fresh, split);
      made && made->primary != primary_id) {
    framework_->SwapSplitView(browser.value, split.c_str());
  }
  return split;
}

void PhiSidebarView::UnbindStoredTab(BrowserId browser, TabId tab_id) {
  if (!framework_ || !tab_id.valid()) {
    return;
  }
  // The stored row keeps its place and its url — it simply stops being
  // "open", which is the Mac's placeholder left behind by
  // demotePinnedTabLeavingPlaceholder. Unconditional: the tag may have been
  // set by this very gesture, before the model heard of it, so "is it
  // tagged?" cannot be answered from the model and clearing an empty tag is
  // harmless.
  framework_->SetTabTagById(browser.value, tab_id.value, "");
}

bool PhiSidebarView::SplitTabsNormalized(BrowserId browser,
                                              TabId primary,
                                              TabId secondary) {
  if (!framework_ || !primary.valid() || !secondary.valid() ||
      primary == secondary) {
    return false;
  }
  UnbindStoredTab(browser, primary);
  UnbindStoredTab(browser, secondary);
  // Ids carry across the unbinding (a cleared tag moves nothing); Chromium
  // picks the leading pane, as SplitActiveWith did.
  const char* split = framework_->CreateSplitViewForTabs(
      browser.value, primary.value, secondary.value, "vertical");
  return split && *split;
}

void PhiSidebarView::DrawFavicon(HDC dc,
                                     const RECT& dest,
                                     const unsigned char* bgra,
                                     int width,
                                     int height,
                                     COLORREF background,
                                     float opacity) {
  // A faded icon (a reclaimed tab): the pixels are PREMULTIPLIED, so
  // scaling all four channels is the whole blend — both paths below then
  // composite it over the DC exactly as they would a translucent icon.
  std::vector<unsigned char> faded;
  if (opacity < 1.0f && bgra && width > 0 && height > 0) {
    const size_t bytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    // SAFETY: the framework's contract is that `bgra` holds width*height*4
    // bytes (phi_framework_api.h).
    const auto source =
        UNSAFE_BUFFERS(base::span<const unsigned char>(bgra, bytes));
    faded.resize(bytes);
    const int scale = static_cast<int>(std::clamp(opacity, 0.0f, 1.0f) * 256);
    for (size_t i = 0; i < bytes; ++i) {
      faded[i] = static_cast<unsigned char>(source[i] * scale / 256);
    }
    bgra = faded.data();
  }
  // NEVER upscale: a 16px favicon inflated to a larger slot only blurs —
  // 1:1 native pixels centred in the slot are the sharpest a small source
  // can look (the driver rarely holds more than 16px). Downscaling larger
  // art is fine and goes through the high-quality path.
  RECT draw_rc = dest;
  const int dest_w = dest.right - dest.left;
  const int dest_h = dest.bottom - dest.top;
  if (width < dest_w && height < dest_h) {
    draw_rc.left = dest.left + (dest_w - width) / 2;
    draw_rc.top = dest.top + (dest_h - height) / 2;
    draw_rc.right = draw_rc.left + width;
    draw_rc.bottom = draw_rc.top + height;
  }
  // Premultiplied BGRA — exactly what the DirectWrite path draws with
  // high-quality interpolation. GDI's stretch below is the fallback.
  if (PhiDrawBgra(dc, draw_rc, bgra, width, height)) {
    return;
  }

  if (!bgra || width <= 0 || height <= 0) {
    return;
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // Top-down, the order Skia gave us.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;

  // Composite over the known background. The pixels are PREMULTIPLIED, so the
  // source contribution is already scaled: dst = src + bg * (1 - a).
  std::vector<unsigned char> composited(static_cast<size_t>(width) *
                                        static_cast<size_t>(height) * 4);
  const int br = GetRValue(background);
  const int bg_g = GetGValue(background);
  const int bb = GetBValue(background);
  // SAFETY: the framework's contract is that `bgra` holds width*height*4 bytes
  // (phi_framework_api.h). A raw pointer and a size is all a C++ ABI across the
  // module boundary can carry, so bounding it here is the first safe moment.
  const auto source = UNSAFE_BUFFERS(base::span<const unsigned char>(
      bgra, static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
  for (size_t i = 0; i + 3 < composited.size(); i += 4) {
    const int sb = source[i], sg = source[i + 1], sr = source[i + 2],
              sa = source[i + 3];
    const int inv = 255 - sa;
    composited[i] = static_cast<unsigned char>(sb + bb * inv / 255);
    composited[i + 1] = static_cast<unsigned char>(sg + bg_g * inv / 255);
    composited[i + 2] = static_cast<unsigned char>(sr + br * inv / 255);
    composited[i + 3] = 255;
  }
  const int prev_mode = ::SetStretchBltMode(dc, HALFTONE);
  ::StretchDIBits(dc, dest.left, dest.top, dest.right - dest.left,
                  dest.bottom - dest.top, 0, 0, width, height,
                  composited.data(), &info, DIB_RGB_COLORS, SRCCOPY);
  ::SetStretchBltMode(dc, prev_mode);
}

// A bookmark folder's icon slot: the catalog symbol it wears, or the plain
// folder glyph for the catalog's "default" (the Mac's BookmarkFolderIcon).
void PhiSidebarView::DrawFolderIcon(HDC dc,
                                    const RECT& slot,
                                    const std::string& icon_id,
                                    COLORREF ink,
                                    bool expanded) {
  const HFONT glyph_font = shell_->SidebarGlyphFont();
  const std::string resolved = PhiResolveFolderIcon(icon_id);
  const bool plain = resolved == PhiFolderIconDefaultId();
  const int px = std::min(slot.right - slot.left, slot.bottom - slot.top);
  if (px > 0) {
    RECT art = {slot.left + ((slot.right - slot.left) - px) / 2,
                slot.top + ((slot.bottom - slot.top) - px) / 2, 0, 0};
    art.right = art.left + px;
    art.bottom = art.top + px;
    // "default" wears the plain folder, open or closed, in the Mac's
    // two-tone accent-derived palette; anything else is its catalog symbol
    // in the row's ink.
    std::vector<unsigned char> bgra;
    if (plain) {
      const PhiPalette palette = shell_->SidebarPalette();
      bgra = PhiPlainFolderBgra(expanded, px, palette.accent, palette.dark,
                                palette.window_bg);
    }
    if (bgra.empty()) {
      const PhiMacIcon* mask = plain ? PhiPlainFolderMask(expanded)
                                     : PhiFolderIconMask(resolved);
      if (mask) {
        bgra = PhiTintedIconBgra(*mask, ink, px);
      }
    }
    if (!bgra.empty()) {
      PhiBlendBgra(dc, art, bgra.data(), px, px);
      return;
    }
  }
  HGDIOBJ previous = glyph_font ? ::SelectObject(dc, glyph_font)
                                   : nullptr;
  PhiDrawGlyph(dc, slot, L"", ink);  // Folder.
  if (previous) {
    ::SelectObject(dc, previous);
  }
}

// static
void PhiSidebarView::DrawSurfaceBadge(HDC dc,
                                      const RECT& box,
                                      SidebarSurface kind,
                                      bool hovered,
                                      COLORREF ground) {
  const HFONT glyph_font = shell_->SidebarGlyphFont();
  if (box.right <= box.left || kind == SidebarSurface::kContent ||
      !spaces_host_) {
    return;
  }
  const PhiPalette& palette = spaces_host_->PaletteForSlot(slot_.value);
  // A small plate the accent tints, so it reads as "something is attached"
  // whether or not the row is selected.
  PhiFillRounded(dc, box, PhiBlendOver(palette.accent, ground, 46),
              Scaled(kCornerRadius) / 2);
  HGDIOBJ previous_font = glyph_font ? ::SelectObject(dc, glyph_font)
                                        : nullptr;
  if (hovered) {
    PhiDrawGlyph(dc, box, L"\u2212", palette.accent);  // Minus: close it.
  } else if (kind == SidebarSurface::kReader) {
    PhiDrawReaderGlyph(dc, box, palette.accent, dpi());
  } else {
    // A window floating over the page.
    PhiDrawGlyph(dc, box, L"\uE8A7", palette.accent);
  }
  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

void PhiSidebarView::PaintBand(HDC dc,
                               const PhiSidebarRects& layout,
                               const PhiPalette& palette) {
  // Read ONCE for the whole band: the two chrome fonts the rows borrow, the
  // hover cursor the shell owns across every band, and the drag in flight.
  // Nothing below writes a row — paint reads a finished layout (§3.1
  // principle 7).
  const HFONT glyph_font = shell_->SidebarGlyphFont();
  const HFONT semibold_font = shell_->SidebarSemiboldFont();
  const std::string band_key = PhiSidebarDragSession::kBandShiftKey;
  const HoverKind hover_kind = shell_->SidebarHoverKind();
  const int hover_index = shell_->SidebarHoverIndex();
  const bool drag_active = drag_ && drag_->active();
  const bool drag_capture = drag_ && drag_->capture();
  const PhiDragKind drag_kind =
      drag_ ? drag_->kind() : PhiDragKind::kNone;
  const int drag_index = drag_ ? drag_->Index() : -1;
  const int folder_row = drag_ ? drag_->folder_row() : -1;
  // Palette-derived locals the band rows use (same derivations as
  // PaintChrome's own).
  const COLORREF kTintTop = palette.ground_top;
  const COLORREF kRowSelectedBg = palette.selected;
  const COLORREF kLabel = palette.label;
  const COLORREF kLabelDim = palette.label_dim;
  const COLORREF kBorder = palette.border;
  const COLORREF kActivePillBorder =
      palette.dark ? PhiBlendOver(RGB(0xFF, 0xFF, 0xFF), kRowSelectedBg, 51)
                   : kRowSelectedBg;

  // Pinned tabs: PinnedTabItem cells -- resting on the 4% wash, an 18pt
  // favicon centred, radius 8. The ACTIVE pin (its bound live tab is the
  // active tab) gets the selected wash and the Mac's 2pt accent ring; in the
  // horizontal strip the cells are the 28pt pinned section and the active one
  // reads like the neighbouring strip tabs (selected wash, no ring).
  const bool pin_sliding = drag_active && drag_kind == PhiDragKind::kPin &&
                           !layout.horizontal_tabs && drag_index >= 0 &&
                           drag_index < static_cast<int>(pinned_.size());
  // The open-tab dots (preference, off by default).
  const bool show_dots = prefs_host_ && prefs_host_->ShowOpenTabIndicators();
  const COLORREF dot_ink = OpenDotInk(palette);
  std::vector<size_t> pin_order;
  pin_order.reserve(pinned_.size());
  for (size_t i = 0; i < pinned_.size(); ++i) {
    if (!pin_sliding || static_cast<int>(i) != drag_index) {
      pin_order.push_back(i);
    }
  }
  if (pin_sliding) {
    pin_order.push_back(static_cast<size_t>(drag_index));
  }
  for (const size_t pin_order_index : pin_order) {
    const ShortcutRow& pin = pinned_[pin_order_index];
    const bool pin_dragged =
        pin_sliding && static_cast<int>(pin_order_index) == drag_index;
    if (pin_dragged && !drag_capture) {
      continue;  // It rides the drag image window; its slot is the gap.
    }
    // Siblings glide by their slide offsets (both axes — a grid gap wraps
    // rows); the dragged cell is only ever painted here at home, lifted,
    // for its snapshot.
    RECT cell = pin.bounds;
    if (!pin_dragged && drag_active && !layout.horizontal_tabs) {
      const SlideVec offset = RowSlide(pin.guid);
      ::OffsetRect(&cell, static_cast<int>(offset.dx),
                   static_cast<int>(offset.dy));
    }
    const bool pin_hovered =
        hover_kind == HoverKind::kPin &&
        hover_index == static_cast<int>(&pin - pinned_.data());
    const COLORREF pin_bg = pin_dragged ? palette.hover_strong
                            : pin.active
                                ? palette.selected
                                : (pin_hovered ? palette.hover_strong
                                               : palette.hover);
    PhiFillRounded(dc, cell, pin_bg, Scaled(kCornerRadius));
    if (pin_dragged) {
      PhiStrokeRounded(dc, cell, pin_bg, palette.border, Scaled(kCornerRadius));
    }
    if (pin.active && !layout.horizontal_tabs) {
      RECT rr = cell;
      ::InflateRect(&rr, -Scaled(1), -Scaled(1));
      PhiStrokeRoundRectAA(dc, rr, Scaled(kCornerRadius), palette.accent,
                           Scaled(2));
    }
    // A bound pane whose tab is dormant fades its favicon. The active cell
    // is never dormant (its page is showing).
    const bool dormant_a = pin.unloaded && !pin.active;
    const bool dormant_b = pin.unloaded_b && !pin.active;
    const int cx = (cell.left + cell.right) / 2;
    const int cy = (cell.top + cell.bottom) / 2;
    // The favicon's row: the open dot hangs under it.
    const int icon_size =
        Scaled(!pin.url_b.empty()        ? 16
               : layout.horizontal_tabs ? kStripPinFavicon
                                        : kPinnedFavicon);
    const int icon_bottom = cy + icon_size / 2;
    if (!pin.url_b.empty()) {
      // A merged split cell: two 16pt favicons side by side, the Mac's
      // PinnedSplitItem (16 + 4 + 16 centred in the double-width cell).
      const int size = icon_size;
      const RECT icon_a = {cx - Scaled(2) - size, cy - size / 2,
                           cx - Scaled(2), cy + size / 2};
      const RECT icon_b = {cx + Scaled(2), cy - size / 2,
                           cx + Scaled(2) + size, cy + size / 2};
      const auto pane_icon = [&](const RECT& slot,
                                 const std::vector<unsigned char>& bgra,
                                 int width, int height, bool dormant) {
        if (!bgra.empty()) {
          DrawFavicon(dc, slot, bgra.data(), width, height, pin_bg,
                      dormant ? kReclaimedOpacity : 1.0f);
        } else {
          HGDIOBJ previous_glyph_font =
              glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
          PhiDrawGlyph(dc, slot, L"", kLabelDim);
          if (previous_glyph_font) {
            ::SelectObject(dc, previous_glyph_font);
          }
        }
      };
      pane_icon(icon_a, pin.favicon, pin.favicon_width, pin.favicon_height,
                dormant_a);
      pane_icon(icon_b, pin.favicon_b, pin.favicon_b_width,
                pin.favicon_b_height, dormant_b);
    } else if (!pin.favicon.empty()) {
      const int size = icon_size;
      const RECT icon = {cx - size / 2, cy - size / 2, cx + size / 2,
                         cy + size / 2};
      DrawFavicon(dc, icon, pin.favicon.data(), pin.favicon_width,
                  pin.favicon_height, pin_bg,
                  dormant_a ? kReclaimedOpacity : 1.0f);
    } else {
      ::SetTextColor(dc, kLabel);
      const std::wstring initial =
          pin.title.empty() ? std::wstring(L"?") : pin.title.substr(0, 1);
      PhiDrawTextAA(dc, initial.c_str(), -1, &cell,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // The open dot: an inactive cell whose tab is open, centred under the
    // favicon (under the PAIR for a split cell), faded when a pane is
    // dormant — the Mac's PinnedTabItem/PinnedSplitItem openIndicatorHost.
    if (show_dots && pin.open && !pin.active) {
      const int dot = Scaled(kOpenDotDiameter);
      const int gap = Scaled(layout.horizontal_tabs ? kOpenDotStripSpacing
                                                    : kOpenDotPinnedSpacing);
      DrawOpenDot(dc, cx - dot / 2, icon_bottom + gap, dot,
                  dormant_a || dormant_b
                      ? FadeInk(dot_ink, pin_bg, kReclaimedOpacity)
                      : dot_ink);
    }
    if (pin.attached_surface != SidebarSurface::kContent &&
        pin.surface_box.right > pin.surface_box.left) {
      RECT badge = pin.surface_box;
      ::OffsetRect(&badge, cell.left - pin.bounds.left,
                   cell.top - pin.bounds.top);
      DrawSurfaceBadge(dc, badge, pin.attached_surface,
                       pin_hovered && !pin_dragged, pin_bg);
    }
  }

  // Bookmarks: rows shaped like tab rows (BookmarkCellView) -- primary-label
  // titles, folders marked with the disclosure glyph.
  const bool bookmark_sliding =
      drag_active && drag_kind == PhiDragKind::kBookmark &&
      !layout.horizontal_tabs && drag_index >= 0 &&
      drag_index < static_cast<int>(bookmarks_.size());
  std::vector<size_t> bookmark_order;
  bookmark_order.reserve(bookmarks_.size());
  for (size_t i = 0; i < bookmarks_.size(); ++i) {
    if (!bookmark_sliding || static_cast<int>(i) != drag_index) {
      bookmark_order.push_back(i);
    }
  }
  if (bookmark_sliding) {
    bookmark_order.push_back(static_cast<size_t>(drag_index));
  }
  for (const size_t bookmark_order_index : bookmark_order) {
    const ShortcutRow& bookmark = bookmarks_[bookmark_order_index];
    if (bookmark.bounds.right <= bookmark.bounds.left) {
      continue;  // Parked: comfortable keeps its bookmarks in the menu, and
                 // an unguarded row here painted its favicon at (0,0) — in
                 // the window's top-left corner, over the tab strip.
    }
    const bool bookmark_dragged =
        bookmark_sliding &&
        static_cast<int>(bookmark_order_index) == drag_index;
    if (bookmark_dragged && !drag_capture) {
      continue;  // It rides the drag image window; its slot is the gap.
    }
    RECT pill = bookmark.bounds;
    if (!bookmark_dragged && drag_active && !layout.horizontal_tabs) {
      const SlideVec offset = RowSlide(bookmark.guid);
      ::OffsetRect(&pill, static_cast<int>(offset.dx),
                   static_cast<int>(offset.dy));
    }
    pill.top += Scaled(kRowPad);
    pill.bottom -= Scaled(kRowPad);
    // Outline depth indents the pill itself, as the Mac outline rows do.
    pill.left += bookmark.depth * Scaled(kBookmarkIndent);
    const bool bookmark_hovered =
        hover_kind == HoverKind::kBookmark &&
        hover_index == static_cast<int>(&bookmark - bookmarks_.data());
    if (bookmark_dragged) {
      // Lifted, like the dragged tab row.
      PhiFillRounded(dc, pill, palette.hover_strong, Scaled(kCornerRadius));
      PhiStrokeRounded(dc, pill, palette.hover_strong, palette.border,
                    Scaled(kCornerRadius));
    } else if (drag_active &&
               (drag_kind == PhiDragKind::kBookmark ||
                drag_kind == PhiDragKind::kPin) &&
               folder_row ==
                   static_cast<int>(bookmark_order_index)) {
      // The Mac resolver's folder highlight: this drop lands INSIDE.
      PhiFillRounded(dc, pill, palette.hover_strong, Scaled(kCornerRadius));
      PhiStrokeRounded(dc, pill, palette.hover_strong, palette.accent,
                    Scaled(kCornerRadius));
    } else if (bookmark.active) {
      // Its bound live tab is the active tab: the row IS the tab, so it
      // reads exactly like the active tab row.
      PhiFillRounded(dc, pill, kRowSelectedBg, Scaled(kCornerRadius));
      if (palette.dark) {
        PhiStrokeRounded(dc, pill, kRowSelectedBg, kActivePillBorder,
                      Scaled(kCornerRadius));
      }
    } else if (bookmark_hovered) {
      PhiFillRounded(dc, pill, palette.hover, Scaled(kCornerRadius));
    }
    RECT text = pill;
    const int mid = (pill.top + pill.bottom) / 2;
    // A 14pt favicon centred in its 16pt slot 6pt in; a folder's 20pt art
    // 4pt in — both leave the title at 30pt (BookmarkCellView's leading
    // padding and spacing differ by exactly the slot difference).
    RECT icon = {pill.left + Scaled(6) +
                     (Scaled(kFaviconSlot) - Scaled(kFaviconSize)) / 2,
                 mid - Scaled(kFaviconSize) / 2, 0, 0};
    icon.right = icon.left + Scaled(kFaviconSize);
    icon.bottom = icon.top + Scaled(kFaviconSize);
    if (bookmark.folder) {
      icon = {pill.left + Scaled(4), mid - Scaled(kFolderIconSize) / 2,
              pill.left + Scaled(4) + Scaled(kFolderIconSize),
              mid - Scaled(kFolderIconSize) / 2 + Scaled(kFolderIconSize)};
    }
    // A bound pane whose tab is dormant fades its favicon and the title,
    // like a dormant tab row; the active row is never dormant.
    const bool dormant_a = bookmark.unloaded && !bookmark.active;
    const bool dormant_b = bookmark.unloaded_b && !bookmark.active;
    const COLORREF bookmark_bg = bookmark.active ? kRowSelectedBg : kTintTop;
    if (bookmark.folder) {
      // The Mac paints a Lottie folder carrying the chosen symbol on its
      // face; with no Lottie runtime here the symbol itself IS the row icon,
      // drawn from the same imageset artwork the Mac's picker uses. The
      // catalog's "default" keeps the plain folder glyph.
      DrawFolderIcon(dc, icon, bookmark.icon, kLabelDim, bookmark.expanded);
    } else if (!bookmark.favicon.empty()) {
      DrawFavicon(dc, icon, bookmark.favicon.data(), bookmark.favicon_width,
                  bookmark.favicon_height, bookmark_bg,
                  dormant_a ? kReclaimedOpacity : 1.0f);
    } else {
      HGDIOBJ previous_glyph_font =
          glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
      PhiDrawGlyph(dc, icon, L"\uE774", kLabelDim);
      if (previous_glyph_font) {
        ::SelectObject(dc, previous_glyph_font);
      }
    }
    text.left = pill.left + Scaled(6) + Scaled(kFaviconSlot) + Scaled(8);
    const bool split_row = !bookmark.url_b.empty();
    if (split_row) {
      // A split bookmark shows BOTH panes' favicons (the Mac's
      // BookmarkCellView secondary favicon, 24pt after the primary).
      RECT icon_b = icon;
      ::OffsetRect(&icon_b, Scaled(24), 0);
      if (!bookmark.favicon_b.empty()) {
        DrawFavicon(dc, icon_b, bookmark.favicon_b.data(),
                    bookmark.favicon_b_width, bookmark.favicon_b_height,
                    bookmark_bg, dormant_b ? kReclaimedOpacity : 1.0f);
      } else {
        HGDIOBJ previous_glyph_font =
            glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
        PhiDrawGlyph(dc, icon_b, L"\uE774", kLabelDim);
        if (previous_glyph_font) {
          ::SelectObject(dc, previous_glyph_font);
        }
      }
      text.left += Scaled(24);
    }
    // The open dot leads the favicon (the Mac's BookmarkCellView leading
    // overlay), decorative so the title never moves; faded when a bound
    // pane is dormant.
    if (show_dots && !bookmark.folder && bookmark.open && !bookmark.active) {
      const int dot = Scaled(kOpenDotDiameter);
      DrawOpenDot(dc, icon.left - Scaled(kOpenDotBookmarkSpacing) - dot,
                  mid - dot / 2, dot,
                  dormant_a || dormant_b
                      ? FadeInk(dot_ink, bookmark_bg, kReclaimedOpacity)
                      : dot_ink);
    }
    ::SetTextColor(dc, dormant_a || dormant_b ? kLabelDim : kLabel);
    // No disclosure glyph: the outline hides its triangles
    // (frameOfOutlineCell is zero) and the folder art alone says open or
    // closed. Folder titles are one weight up (.medium).
    std::wstring label = bookmark.title;
    if (split_row) {
      // "primary \u2022 secondary" \u2014 the secondary title, or its www.-stripped
      // host when the title duplicates the primary's (the Mac's fallback).
      std::wstring second = bookmark.title_b;
      if (second.empty()) {
        std::string host = bookmark.url_b;
        const size_t scheme = host.find("://");
        if (scheme != std::string::npos) {
          host = host.substr(scheme + 3);
        }
        const size_t slash = host.find('/');
        if (slash != std::string::npos) {
          host = host.substr(0, slash);
        }
        if (host.rfind("www.", 0) == 0) {
          host = host.substr(4);
        }
        second = base::UTF8ToWide(host);
      }
      if (!second.empty()) {
        label += L" \u2022 " + second;
      }
    }
    RECT bookmark_badge = bookmark.surface_box;
    const bool bookmark_attached =
        bookmark.attached_surface != SidebarSurface::kContent &&
        bookmark_badge.right > bookmark_badge.left;
    if (bookmark_attached) {
      const SlideVec offset = !bookmark_dragged && drag_active
                                  ? RowSlide(bookmark.guid)
                                  : SlideVec{};
      ::OffsetRect(&bookmark_badge, static_cast<int>(offset.dx),
                   static_cast<int>(offset.dy));
      text.right = bookmark_badge.left - Scaled(4);
    }
    HGDIOBJ folder_font_previous =
        bookmark.folder && semibold_font
            ? ::SelectObject(dc, semibold_font)
            : nullptr;
    PhiDrawTextAA(dc, label.c_str(), -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (folder_font_previous) {
      ::SelectObject(dc, folder_font_previous);
    }
    if (bookmark_attached) {
      DrawSurfaceBadge(dc, bookmark_badge, bookmark.attached_surface,
                       bookmark_hovered && !bookmark_dragged,
                       bookmark.active ? kRowSelectedBg : kTintTop);
    }
  }

  // "New Tab": the row heading the tab list -- a 16pt "+" 6pt in, an 8pt
  // gap, the title in textTertiary (NewTabButtonCellView). Hover raises the
  // Mac cell's radius-8 wash on the 2pt-inset pill.
  // Everything from here down to the tab rows rides the band shift while a
  // drag holds a gap open in the bookmark outline above it.
  const int band_shift = drag_active && !layout.horizontal_tabs
                             ? static_cast<int>(
                                   RowSlide(band_key).dy)
                             : 0;
  // The separator row: a 1pt `separator` hairline centred in its 16pt row,
  // spanning the rows' width (SeparatorCellView).
  if (layout.bookmark_separator.right > layout.bookmark_separator.left) {
    RECT sep = layout.bookmark_separator;
    ::OffsetRect(&sep, 0, band_shift);
    const int line_y = (sep.top + sep.bottom) / 2;
    const RECT line = {sep.left, line_y, sep.right, line_y + Scaled(1)};
    HBRUSH sep_brush = ::CreateSolidBrush(palette.separator);
    ::FillRect(dc, &line, sep_brush);
    ::DeleteObject(sep_brush);
  }
  if (layout.new_tab_row.right > layout.new_tab_row.left) {
    RECT row = layout.new_tab_row;
    ::OffsetRect(&row, 0, band_shift);
    if (hover_kind == HoverKind::kNewTabRow) {
      RECT pill = row;
      pill.top += Scaled(kRowPad);
      pill.bottom -= Scaled(kRowPad);
      PhiFillRounded(dc, pill, palette.hover, Scaled(kCornerRadius));
    }
    const int mid = (row.top + row.bottom) / 2;
    const RECT icon = {row.left + Scaled(6), mid - Scaled(8),
                       row.left + Scaled(6) + Scaled(16), mid + Scaled(8)};
    PhiDrawGlyph(dc, icon, L"+", palette.label_tertiary);
    RECT text = row;
    text.left = icon.right + Scaled(8);
    ::SetTextColor(dc, palette.label_tertiary);
    PhiDrawTextAA(dc, L"New Tab", -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }

  // The scrolling tab-list band (groups + tab rows) clips to its viewport:
  // a row straddling the bottom edge must not bleed into the bottom bar.
  const bool clip_rows = !layout.horizontal_tabs;
  int rows_clip_save = 0;
  if (clip_rows) {
    rows_clip_save = ::SaveDC(dc);
    ::IntersectClipRect(dc, 0, layout.tabs_top, layout.sidebar.right,
                        layout.tabs_limit);
  }

  // Tab groups: TabGroupCellView — the whole group cell rests on an 18%
  // tint of the group color; the 32pt header row carries the chevron, the
  // color dot (white ring) and the title. Clicking the header collapses.
  // The headers are drawn through a shifted viewport rather than by
  // offsetting each of their rects (the band shift, see above).
  POINT viewport_before = {};
  ::OffsetViewportOrgEx(dc, 0, band_shift, &viewport_before);
  for (const GroupHeader& gh : group_headers_) {
    if (gh.container.right > gh.container.left) {
      // The card: the hover wash under the 1pt `border` hairline.
      PhiStrokeRounded(dc, gh.container, palette.hover, palette.border,
                    Scaled(kCornerRadius));
    }
    if (gh.bounds.right <= gh.bounds.left) {
      continue;  // Header scrolled out of the viewport.
    }
    if (hover_kind == HoverKind::kGroupHeader &&
        hover_index == static_cast<int>(&gh - group_headers_.data())) {
      PhiFillRounded(dc, gh.bounds, palette.hover_strong,
                     Scaled(kCornerRadius));
    }
    // TabGroupHeaderView: 4pt leading, the 10pt chevron in a 20pt frame, the
    // 10pt dot, a 6pt gap, the semibold title.
    const int mid = (gh.bounds.top + gh.bounds.bottom) / 2;
    RECT chevron = {gh.bounds.left + Scaled(4), mid - Scaled(10),
                    gh.bounds.left + Scaled(4) + Scaled(20), mid + Scaled(10)};
    HGDIOBJ previous_glyph_font =
        glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
    PhiDrawGlyph(dc, chevron, gh.collapsed ? L"\uE76C" : L"\uE70D", kLabelDim);
    if (previous_glyph_font) {
      ::SelectObject(dc, previous_glyph_font);
    }
    // The dot: colored centre in a 1pt white ring, 10pt over all
    // (TabGroupHeaderColorIndicator).
    const int cx = chevron.right + Scaled(5);
    PhiFillEllipseAA(dc,
                     {cx - Scaled(5), mid - Scaled(5), cx + Scaled(5),
                      mid + Scaled(5)},
                     RGB(0xFF, 0xFF, 0xFF));
    PhiFillEllipseAA(dc,
                     {cx - Scaled(4), mid - Scaled(4), cx + Scaled(4),
                      mid + Scaled(4)},
                     gh.color);
    ::SetTextColor(dc, kLabel);
    RECT title_rc = {cx + Scaled(5) + Scaled(6), gh.bounds.top,
                     gh.bounds.right - Scaled(6), gh.bounds.bottom};
    HGDIOBJ previous_title_font =
        semibold_font ? ::SelectObject(dc, semibold_font) : nullptr;
    PhiDrawTextAA(dc, gh.title.c_str(), -1, &title_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (previous_title_font) {
      ::SelectObject(dc, previous_title_font);
    }
  }
  ::SetViewportOrgEx(dc, viewport_before.x, viewport_before.y, nullptr);

  // Tab rows: a 32pt pill inside the 36pt row (2pt vertical padding), radius
  // 8. The title is textPrimary whether active or not (SideTabView); only the
  // pill marks the active tab. The close box rides the ACTIVE row -- the Mac
  // shows it on hover, which GDI has no notion of yet.
  //
  // During a drag-reorder (Chrome's vertical-tab model) the DRAGGED row
  // floats with the cursor and paints LAST (on top); its siblings paint at
  // their slide offsets, gliding around the insertion gap.
  const bool sliding = drag_active && drag_kind == PhiDragKind::kTab &&
                       !layout.horizontal_tabs && drag_index >= 0 &&
                       drag_index < static_cast<int>(tabs_.size());
  std::vector<size_t> draw_order;
  draw_order.reserve(tabs_.size());
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (!sliding || static_cast<int>(i) != drag_index) {
      draw_order.push_back(i);
    }
  }
  if (sliding) {
    draw_order.push_back(static_cast<size_t>(drag_index));
  }
  bool rows_clip_active = clip_rows;
  for (const size_t order_index : draw_order) {
    const TabRow& row = tabs_[order_index];
    if (row.bounds.right <= row.bounds.left) {
      continue;  // Scrolled out of the viewport.
    }
    const bool is_dragged =
        sliding && static_cast<int>(order_index) == drag_index;
    if (is_dragged && !drag_capture) {
      continue;  // It rides the drag image window; its slot is the gap.
    }
    // Siblings glide by their slide offsets; the dragged row is only ever
    // painted here at home, lifted, for its snapshot.
    int dy = band_shift;
    if (!is_dragged && sliding) {
      dy += static_cast<int>(TabSlide(row.id));
    }
    RECT pill = row.bounds;
    ::OffsetRect(&pill, 0, dy);
    RECT close_box = row.close_box;
    ::OffsetRect(&close_box, 0, dy);
    if (!layout.horizontal_tabs) {
      pill.top += Scaled(kRowPad);
      pill.bottom -= Scaled(kRowPad);
    }
    const bool row_hovered =
        hover_kind == HoverKind::kTab &&
        hover_index == static_cast<int>(&row - tabs_.data());
    if (layout.horizontal_tabs) {
      // Chrome's strip. The active tab is the toolbar's own ground carried
      // up into the band, rounded on top and flush at the bottom so the two
      // read as one surface; a resting tab shows only the window tint it
      // sits on, divided from its neighbour by a hairline. Nothing here
      // rounds the bottom — that is the whole point of the merge.
      if (row.active) {
        PhiFillRoundedTop(dc, pill, palette.window_bg, Scaled(kCornerRadius));
      } else {
        if (row_hovered) {
          PhiFillRoundedTop(dc, pill, palette.hover, Scaled(kCornerRadius));
        }
        // The divider rides the tab's trailing edge, inset vertically so it
        // reads as a separator and not as a full-height rule.
        RECT divider = {pill.right - 1, pill.top + Scaled(6), pill.right,
                        pill.bottom - Scaled(6)};
        HBRUSH divider_brush = ::CreateSolidBrush(palette.separator);
        ::FillRect(dc, &divider, divider_brush);
        ::DeleteObject(divider_brush);
      }
    } else if (is_dragged) {
      // Lifted: the emphasized wash plus a hairline, Chrome's elevated
      // dragged view.
      PhiFillRounded(dc, pill, palette.hover_strong, Scaled(kCornerRadius));
      PhiStrokeRounded(dc, pill, palette.hover_strong, palette.border,
                    Scaled(kCornerRadius));
    } else if (row.active) {
      // Dark: the Mac's white-20% active border. Light: a hairline stands
      // in for the Mac's drop shadow — the same trade the Space chip makes,
      // or the white pill disappears on a white ground.
      PhiStrokeRounded(dc, pill, kRowSelectedBg,
                    palette.dark ? kActivePillBorder : kBorder,
                    Scaled(kCornerRadius));
    } else if (row_hovered) {
      PhiFillRounded(dc, pill, palette.hover, Scaled(kCornerRadius));
    }
    // A 14pt icon centred in the 16pt slot, 6pt from the pill's edge. A
    // merged split row draws the same shape once per pane around a hairline
    // divider (the Mac's single merged row with two panes).
    const int mid = (pill.top + pill.bottom) / 2;
    // Inside a group card the row rests on the card's wash, not the tint.
    const COLORREF ground = row.group_id.empty() ? kTintTop : palette.hover;
    const COLORREF row_bg =
        layout.horizontal_tabs
            ? (row.active ? palette.window_bg : ground)
            : (is_dragged ? palette.hover_strong
                          : row.active ? kRowSelectedBg : ground);
    const auto draw_pane = [&](int left, int right, const TabRow& r,
                               bool pane_b) {
      RECT slot = {left + Scaled(6), mid - Scaled(kFaviconSlot) / 2,
                   left + Scaled(6) + Scaled(kFaviconSlot),
                   mid + Scaled(kFaviconSlot) / 2};
      RECT icon = {
          slot.left + (Scaled(kFaviconSlot) - Scaled(kFaviconSize)) / 2,
          mid - Scaled(kFaviconSize) / 2, 0, 0};
      icon.right = icon.left + Scaled(kFaviconSize);
      icon.bottom = icon.top + Scaled(kFaviconSize);
      const auto& favicon = pane_b ? r.b_favicon : r.favicon;
      const std::vector<unsigned char>& logo = PhiLogoBgra256();
      // A DORMANT tab (restored but never loaded, or discarded) draws its
      // favicon at the Mac's 30% and reads dimmer than a live one. The
      // active row is never dormant, so this never fights the selected pill.
      const bool dormant = r.unloaded && !r.active;
      if (!favicon.empty()) {
        DrawFavicon(dc, icon, favicon.data(),
                    pane_b ? r.b_favicon_width : r.favicon_width,
                    pane_b ? r.b_favicon_height : r.favicon_height, row_bg,
                    dormant ? kReclaimedOpacity : 1.0f);
      } else if (r.internal_page && !logo.empty() &&
                 PhiDrawBgra(dc, icon, logo.data(), 256, 256)) {
        // Internal pages (chrome://newtab and friends) carry the Phi mark,
        // as the Mac shows its own logo there — not the generic globe.
      } else {
        // The Mac's no-favicon placeholder: the globe (FaviconProvider).
        HGDIOBJ previous_glyph_font =
            glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
        PhiDrawGlyph(dc, icon, L"\uE774", kLabelDim);
        if (previous_glyph_font) {
          ::SelectObject(dc, previous_glyph_font);
        }
      }
      ::SetTextColor(dc, dormant ? kLabelDim : kLabel);
      RECT title_rc = {slot.right + Scaled(8), pill.top, right - Scaled(4),
                       pill.bottom};
      PhiDrawTextAA(dc, (pane_b ? r.b_title : r.title).c_str(), -1, &title_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    };
    // The close box shows on HOVER only (SideTabView) \u2014 the active row
    // included; the title reclaims its slot the rest of the time. A row with
    // a surface attached has no close box: the indicator holds the slot.
    const bool attached = row.attached_surface != SidebarSurface::kContent;
    const bool show_close = row_hovered && !is_dragged && !attached;
    RECT surface_box = row.surface_box;
    ::OffsetRect(&surface_box, 0, dy);
    const int trailing = show_close ? close_box.left
                         : attached ? surface_box.left
                                    : pill.right;
    if (row.b_index >= 0) {
      const int div_x = (pill.left + pill.right) / 2;
      draw_pane(pill.left, div_x, row, /*pane_b=*/false);
      draw_pane(div_x, trailing, row, /*pane_b=*/true);
      RECT divider = {div_x, pill.top + Scaled(6), div_x + 1,
                      pill.bottom - Scaled(6)};
      HBRUSH sep = ::CreateSolidBrush(palette.separator);
      ::FillRect(dc, &divider, sep);
      ::DeleteObject(sep);
    } else {
      draw_pane(pill.left, trailing, row, /*pane_b=*/false);
    }
    if (show_close) {
      PhiDrawGlyph(dc, close_box, L"\u00D7", kLabelDim);
    }
    if (attached) {
      DrawSurfaceBadge(dc, surface_box, row.attached_surface,
                       row_hovered && !is_dragged, row_bg);
    }
    // The agent operating mask: a badge on rows an agent is holding. Accent
    // while the agent drives; dimmed once the user has taken the tab back
    // (the row keeps the badge so "Hand Back" stays discoverable).
    if (agent_host_ && row.id.valid() &&
        agent_host_->IsTabAgentDriven(row.id.value)) {
      const bool reclaimed = agent_host_->IsTabReclaimed(row.id.value);
      const int dot = Scaled(8);
      const int cx = trailing - Scaled(10);
      const int cy = (pill.top + pill.bottom) / 2;
      const COLORREF ink =
          reclaimed ? palette.label_tertiary : palette.accent;
      PhiFillEllipseAA(
          dc, {cx - dot / 2, cy - dot / 2, cx + dot / 2, cy + dot / 2}, ink);
    }
  }

  if (rows_clip_active) {
    ::RestoreDC(dc, rows_clip_save);
  }


}

void PhiSidebarView::Paint(HDC dc,
                           const PhiSidebarRects& layout,
                           const PhiPalette& palette) {
  PhiSpacesStrip* const strip = shell_->SidebarSpacesStrip();
  if (!strip->swap_running() || layout.horizontal_tabs) {
    PaintBand(dc, layout, palette);
    return;
  }
  // The Space switch slides the REAL rows (no snapshots): the leaving
  // Space's retained model paints shifted out at its frozen source palette,
  // the live (entering) model paints shifted in at the ramped palette, both
  // clipped to the band over the ramping ground.
  const double t = strip->SwapProgress();
  const bool forward = strip->swap_forward();
  const int band_w = Scaled(sidebar_dip());
  const int leave_dx = forward ? -static_cast<int>(t * band_w + 0.5)
                               : static_cast<int>(t * band_w + 0.5);
  const int enter_dx = forward ? band_w + leave_dx : leave_dx - band_w;
  const int save = ::SaveDC(dc);
  ::IntersectClipRect(dc, 0, layout.favorites.top, band_w,
                      layout.tabs_limit);
  POINT origin = {};
  ::GetViewportOrgEx(dc, &origin);
  ::SetViewportOrgEx(dc, origin.x + leave_dx, origin.y, nullptr);
  SwapBandModel();
  PaintBand(dc, layout, strip->swap_source_palette());
  SwapBandModel();
  ::SetViewportOrgEx(dc, origin.x + enter_dx, origin.y, nullptr);
  PaintBand(dc, layout, palette);
  ::SetViewportOrgEx(dc, origin.x, origin.y, nullptr);
  ::RestoreDC(dc, save);
  if (t >= 1.0) {
    // The page flips only now, the Mac's order.
    strip->NoteSlideLanded();
  }
}

void PhiSidebarView::SwapBandModel() {
  tabs_.swap(leaving_band_.tabs);
  pinned_.swap(leaving_band_.pinned);
  bookmarks_.swap(leaving_band_.bookmarks);
  group_headers_.swap(leaving_band_.headers);
}

}  // namespace phi_client
