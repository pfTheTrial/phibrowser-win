// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_window_controller.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

// dwmapi.h reaches rpcndr.h, which #defines `small` — this file uses it as
// an identifier.
#undef small

#include <algorithm>
#include <functional>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/base64.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_agent_overlay.h"
#include "phibrowser-win/client/phi_downloads_popover.h"
#include "phibrowser-win/client/phi_extensions_popover.h"
#include "phibrowser-win/client/phi_group_overview.h"
#include "phibrowser-win/client/phi_search_tabs_panel.h"
#include "phibrowser-win/client/phi_space_theme_panel.h"
#include "phibrowser-win/client/phi_bookmark_manager.h"
#include "phibrowser-win/client/phi_create_space_panel.h"
#include "phibrowser-win/client/phi_folder_icon_picker.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_tab_preview.h"
#include "phibrowser-win/client/phi_tab_switcher.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_glyph_bitmap.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_mac_icons.h"
#include "phibrowser-win/client/phi_menu_bar.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_new_tab_page.h"
#include "phibrowser-win/client/phi_reader.h"
#include "phibrowser-win/client/phi_prompt.h"

namespace phi_client {

namespace {

constexpr wchar_t kShellClassName[] = L"PhiClientShell";

// --- Metrics -----------------------------------------------------------------
// Design units (points at 100%), taken from the Mac client so the two look the
// same: WebContentConstant, SidebarHeaderView, SpacesStripView,
// PinnedTabLayout, TabStripMetrics and SidebarBottomBar.
// The caption band: the OS title bar is removed (WM_NCCALCSIZE) and this row
// holds the MENU TITLES and the minimize / maximize / close buttons together
// — the menu bar lives on the caption row, not under it.
constexpr int kTitleBarHeight = 32;
// Comfortable's band is taller: it carries the tab strip as well as the
// window controls, and a tab is 34 tall over the strip's 6pt top inset.
constexpr int kTitleBarHeightTabs = 40;
constexpr int kCaptionButtonWidth = 46;  // The Win11 caption button metric.
constexpr int kMenuTitlePad = 10;        // Each title's horizontal padding.
// The Phi title's leading pad: its rect starts kPhiTitlePad short of the
// sidebar's edge inset, so the logo lands ON kEdgeInset — the left edge the
// Spaces pips, the pinned card and the tab rows below all share.
constexpr int kPhiTitlePad = 4;
// WebContentConstant.edgesSpacing — THE horizontal gutter. There is no other
// sidebar padding on Mac; every row applies 8 on each side itself.
constexpr int kEdgeInset = 8;
constexpr int kNavButton = 24;     // Every header button is 24x24.
constexpr int kNavGroupSpacing = 1;   // back/forward/reload stack spacing.
// The caption band's trailing control cluster. The vertical layouts moved
// their header controls up here: search-tabs sits one 16pt gap before
// minimize (close enough to reach, far enough that a miss does not minimize
// the window), and performance leads it with back / forward / reload. Both
// modes park search-tabs at the SAME x, so switching layout does not move
// the button under the pointer.
constexpr int kBandTrailingGap = 16;  // cluster → the caption buttons.
constexpr int kBandGroupGap = 8;      // the nav trio → search-tabs.
// The hairline standing in that gap: short enough to read as a seam between
// the app's controls and the window's, not as a border round either.
constexpr int kBandDividerHeight = 16;
// The sidebar's own top inset. It used to be navRow.bottom + 8; with the nav
// row gone the Spaces strip starts at the gutter, and every band under it
// rides up by the 32pt the row occupied.
constexpr int kSpacesStripTop = 8;
constexpr int kSpacesStripHeight = 32;   // SpacesStripView.sidebarHeight
// The strip's own gutter. The Mac's SpacesStripView uses 10, which leaves
// the pip row standing a hair inside the address bar and the tab rows below
// it; here the strip shares the sidebar's gutter instead, so the row of pip
// backgrounds spans EXACTLY the width of the address bar under it.
constexpr int kSpacesStripPad = kEdgeInset;
constexpr int kSpaceChip = 24;           // stripItemWidth/Height: a 24pt pip.
// The vertical layouts' cell: the painted card (the Mac's 24pt chip) plus a
// point of hit area on each side. Cell and gap together set what the eye
// reads as the space BETWEEN two Spaces — 2 of cell margin and 4 of gap, so
// six points of ground, where a cell the full height of the band left
// twelve and the row read as scattered. Comfortable keeps the plain 24pt
// pip: its single compact one shares the band with the tab strip.
constexpr int kSpaceChipVertical = 26;
constexpr int kSpaceChipGap = 4;         // stripSpacing.
// The selected card's corner RADIUS — and it is passed DOUBLED to the
// draws: GDI's RoundRect takes the corner's ellipse, not its radius, so
// every rounded rect in this file that hands its radius straight over is
// rounding half as hard as the Mac constant it is named for. The Mac draws
// 8 on its 24pt chip (a third of the side) as a `.continuous` squircle;
// 9 of real radius stands in for that on a circular arc. It is also about
// as far as the corner goes: the colour pass clears the glyph box whole,
// and a rounder card would have the box's own corners poking out of it and
// squaring it back off.
constexpr int kSpaceCardRadius = 9;
// ONE size per pip: what a Space's ICON measures, whichever medium it comes
// from — catalog artwork, colour emoji, or an MDL2 stand-in. Each medium
// needs a different number to land there (an imageset pads its canvas; a
// font's em is bigger than its ink, by a different amount per glyph), so
// the draws take the ink size and work the number out per glyph rather than
// the call site guessing one for all of them. The Mac carries three
// constants that all come out at ~15pt of ink in its 24pt strip item
// (phiIconSize 24, emojiFontSize 13 — "approximately 15pt visible" — and
// SF-symbol iconSize 14); this is that invariant, kept directly.
constexpr int kSpaceIcon = 13;
// The big pip keeps the sidebar's own 8pt gutter clear inside its 32pt
// pill, which is what stops a Space's icon from crowding its background.
constexpr int kSpaceIconVertical = 14;
// The strip's TEXT at the larger pip: the first letter of a Space with no
// icon, and the trailing slot's "+" / "…". A letter's ink is shorter than a
// glyph's em box, so it sits a size under the icons rather than matching
// them. (The 24pt pip draws both in the UI font, which is already this
// relation to its 13pt glyphs.)
constexpr int kSpaceTextVertical = 14;
constexpr int kAddressBarTop = 52;    // spacesStrip.bottom + 12
constexpr int kAddressBarHeight = 32;
constexpr int kHeaderHeight = 88;     // 48 base + 32 strip + 8 gap
constexpr int kHeaderSpacer = 5;      // header → pinned band spacer.
constexpr int kFavoritesSpacer = 3;   // pinned band → tab list spacer.
constexpr int kTabRowHeight = 36;     // Row pitch adds kTabRowGap below.
constexpr int kTabRowGap = 2;         // AppKit's default intercell height.
constexpr int kRowPad = 2;            // 36pt row → 32pt pill (vertical inset).
constexpr int kBottomBarHeight = 24;  // SidebarBottomBarState.singleRowHeight
constexpr int kBottomSpacer = 8;
constexpr int kCornerRadius = 8;      // The pill radius, everywhere.
// How far a shell with no placement to inherit sits off the work area's
// top-left corner (see the first-run placement in the constructor).
constexpr int kDefaultPlacementMargin = 24;
// The web content is an inset rounded card floating on the tint. Margins are
// the Mac's: LEFT FLUSH against the sidebar, 8 right/bottom, and a
// titleAwareArea breath on top — 8 in performance, 12 under a top bar.
constexpr int kContentMargin = 8;
constexpr int kContentRadius = 14;  // webContentContainerCornerRadius (macOS 26)
// The breath between the caption band and the content card. The card hugs
// the band now — 4, not the 8 performance had and the 12 balanced had — so
// the page starts higher in both vertical layouts.
constexpr int kContentTopGap = 4;
constexpr int kFaviconSlot = 16;    // The 16pt slot in the row…
constexpr int kFaviconSize = 14;    // …holding a 14pt icon (radius 3).
constexpr int kBookmarkIndent = 10;   // SideBarOutlineView.indentation.
constexpr int kFolderIconSize = 20;   // BookmarkFaviconView.folderSize.
// The Mac's SeparatorItem row between the bookmark outline and the tab list.
constexpr int kBookmarkSeparatorHeight = 16;
// Omnibox suggestion dropdown metrics (DIP). The popup class itself is
// registered further down (same anonymous namespace) — declared here because
// the field methods above it need the registration.
constexpr int kOmniboxRowHeight = 32;
constexpr int kOmniboxMaxRows = 8;
ATOM EnsureOmniboxPopupClass();  // Outline indent per depth (AppKit's).
constexpr int kNewTabRowHeight = 36;
// Pinned tabs: PinnedTabLayout — 45pt cells, 8pt gaps and insets, column
// limit by width breakpoint (3 at this sidebar width), 53pt minimum width.
constexpr int kPinnedCellHeight = 45;
constexpr int kPinnedGap = 8;
constexpr int kPinnedMinWidth = 53;
constexpr int kPinnedFavicon = 18;
// The horizontal strip's pinned section (TabStripMetrics.PinnedTab): fixed
// 28pt square cells, 2pt spacing, leading the normal tabs.
constexpr int kStripPinSize = 28;
constexpr int kStripPinGap = 2;
constexpr int kStripPinToTabs = 6;
constexpr int kStripPinFavicon = 16;
// The Mac's vertical-layout Space swap (SpaceManager.swapAnimationDuration).
constexpr int kSpaceSwapMs = 150;
// The theme ramp outlives the slide: a deliberate slow fade of the whole
// chrome to the target Space's colors.
constexpr int kThemeRampMs = 600;
constexpr UINT_PTR kSpaceSwapTimer = 0x515;
// How long a Space pip must stay hovered before its info card appears
// (SpacesStripView.hoverCardDelay), so a cursor just passing across the
// strip doesn't flash cards.
constexpr int kHoverCardDelayMs = 300;
constexpr int kHoverCardRadius = 12;  // Rounder than the pills' 8.
constexpr UINT_PTR kHoverCardTimer = 0x516;
// Drag-reorder slide: Chrome's vertical-tab layout animation (200ms,
// EASE_IN_OUT) — sibling rows glide to open the insertion gap.
constexpr int kDragSlideMs = 200;
constexpr UINT_PTR kDragSlideTimer = 0x518;
// A reserved row_slide key: how far everything BELOW the bookmark outline
// ("+ New Tab", group headers, the tab rows) is pushed while a drag opens a
// row-sized gap in the outline. Rides the same clock as the row slides.
constexpr char kBandShiftKey[] = "\x01" "band";
constexpr UINT_PTR kFloatingTickTimer = 0x522;
// Reverts the copy-link checkmark after its second (triggeredRevertDelay).
constexpr UINT_PTR kCopyLinkRevertTimer = 0x523;
// The Spaces strip's "+" fading in and out with the row hover
// (SpacesStripView.addButton: .animation(.easeInOut(duration: 0.15))).
constexpr int kAddSpaceFadeMs = 150;
constexpr UINT_PTR kAddSpaceFadeTimer = 0x524;
// Re-arms a kMsgPhiDeferredClose that found a nested loop still on the
// stack (see PhiWindowController::RequestClose).
constexpr UINT_PTR kDeferredCloseTimer = 0x525;
constexpr UINT kDeferredCloseRetryMs = 50;

// Tab groups: the Mac's 32pt TabGroupHeaderView on TabGroupCellView's card —
// the hover wash under a 1pt border hairline, radius 8; the group colour
// lives only in the header's dot. Members sit 4pt in from the header and
// the card's bottom (innerTableTop/BottomInset), 8pt in from its leading
// edge (the member cell's own edgesSpacing) and 4pt from its trailing one.
constexpr int kGroupHeaderHeight = 32;
constexpr int kGroupInnerInset = 4;
constexpr int kGroupMemberLeadInset = 8;
constexpr int kGroupMemberTrailInset = 4;
struct PhiGroupColor {
  const char* name;
  COLORREF color;
};
// Chromium's TabGroupColorId palette (the Mac's GroupColor set is the same
// nine); the name keys the localized colour label (tabs.tabGroup.colorName.*)
// that titles an unnamed group.
constexpr std::array<PhiGroupColor, 9> kGroupColors = {{
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
const PhiGroupColor& GroupColorByName(const std::string& name) {
  for (const PhiGroupColor& c : kGroupColors) {
    if (name == c.name) {
      return c;
    }
  }
  return kGroupColors[0];
}

// Bands used by the two layouts that put navigation above the web content.
constexpr int kTopBarHeight = 40;      // WebContentConstant.headerHeight
// Balanced's top bar is slimmer: 24pt controls with 4 above and below, the
// same 32pt band the Spaces strip runs at, so the sidebar's strip and the
// card's address row sit on ONE line across the window.
constexpr int kTopBarHeightNav = 32;
constexpr int kBookmarkBarHeight = 32;  // The Mac BookmarkBar's barHeight.
constexpr int kTopBarLeading = 16;     // header leading padding.
constexpr int kComfortableLeading = 12;  // …and comfortable's tighter one.
constexpr int kOmniboxHeight = 28;     // comfortable's omnibox capsule.
constexpr int kTopBarSpacing = 8;      // header button spacing.
constexpr int kTopBarField = 24;       // balanced's field, on the button line.
// The comfortable layout's tab strip shares the caption band with the
// window controls, Chrome-style, so the band is taller there and a tab runs
// from the strip's top inset all the way to the band's bottom edge — where
// the active one merges into the toolbar under it.
constexpr int kHTabStripTop = 6;       // Strip top inset (8 − 2).
constexpr int kHTabStripLeading = 8;   // The Space pip → the first tab.
constexpr int kHTabHeight = 34;        // kTitleBarHeightTabs − kHTabStripTop
constexpr int kHTabWidth = 200;        // Tab.idealWidth (a resting tab)
constexpr int kHTabActiveWidth = 240;  // The active tab takes more room.
constexpr int kHTabMinWidth = 56;      // Favicon, a breath, and the close box.
constexpr int kHTabGap = 0;            // Tabs abut; a hairline divides them.
constexpr int kCloseBoxSize = 24;      // UnifiedTabCloseButton frame.
// The bottom bar's two capsules (Feedback bordered, Chat on the accent).
// The sidebar bottom bar is the only place Feedback has a band now: the
// comfortable header that carried the labelled variant is gone, and its
// entry moved into the overflow menu. The paint keeps the label branch —
// ViewThatFits picks it as soon as a band is wide enough for it again.
constexpr int kFeedbackIconWidth = 32;  // Icon-only variant (bottom bar).
constexpr int kChatWidth = 58;
constexpr int kPillHeight = 22;

// --- Palette (light) ---------------------------------------------------------
// From Assets.xcassets/ColorSet: primary-label 85% black, common-border 8%
// black, sidebar-tab-selected white. The sidebar ground stands in for the Mac's
// material + per-Space gradient tint, which GDI cannot reproduce.
// Every colour now comes from the active theme (phi_theme.h), resolved by the
// coordinator and read at paint time.

// The stock EDIT window proc, saved when the first address field is
// subclassed. One per process — every EDIT shares the class proc.
WNDPROC g_edit_base_proc = nullptr;

// Re-assert the corner caps after the BROWSER child reorders itself: sent by
// the browser subclass below whenever its z-order changes (Chromium raises
// its window on activation, which would bury the caps).
constexpr UINT WM_PHI_RAISE_CAPS = WM_APP + 7;

// Original window procs of adopted browser children, by hwnd (a slot hosts
// one per Space). UI-thread only.
std::map<HWND, WNDPROC>& BrowserBaseProcs() {
  static base::NoDestructor<std::map<HWND, WNDPROC>> procs;
  return *procs;
}

LRESULT CALLBACK BrowserSubclassProc(HWND hwnd, UINT msg, WPARAM wparam,
                                     LPARAM lparam) {
  WNDPROC base = nullptr;
  auto it = BrowserBaseProcs().find(hwnd);
  if (it != BrowserBaseProcs().end()) {
    base = it->second;
  }
  if (msg == WM_WINDOWPOSCHANGED) {
    const auto* pos = reinterpret_cast<const WINDOWPOS*>(lparam);
    if (pos && !(pos->flags & SWP_NOZORDER)) {
      // Async: never touch sibling z-order from inside its own reorder.
      ::PostMessageW(::GetParent(hwnd), WM_PHI_RAISE_CAPS, 0, 0);
    }
  }
  if (msg == WM_NCDESTROY) {
    // Windows recycles HWND values: a stale map entry would hand a future
    // window this browser's old WNDPROC. Unhook on final destruction.
    const LRESULT result =
        base ? ::CallWindowProcW(base, hwnd, msg, wparam, lparam)
             : ::DefWindowProcW(hwnd, msg, wparam, lparam);
    BrowserBaseProcs().erase(hwnd);
    return result;
  }
  return base ? ::CallWindowProcW(base, hwnd, msg, wparam, lparam)
              : ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

// Every popup menu the controller tracks goes through here: the menu's
// nested loop is a PhiModalScope, so the coordinator defers pipe verbs and
// the controller defers its own close until the menu is down. Always
// TPM_RETURNCMD with no exclusion rect — the caller reads the chosen id.
UINT TrackPopupMenuModal(HMENU menu, UINT flags, int x, int y, HWND owner) {
  PhiModalScope modal;
  return static_cast<UINT>(
      ::TrackPopupMenu(menu, flags, x, y, 0, owner, nullptr));
}

ATOM EnsureShellClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    // CS_DBLCLKS: a group header's double-click opens the group overview.
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &PhiWindowController::ShellWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    // Phi's own icon in the taskbar and alt-tab — not Chromium's.
    wc.hIcon = PhiAppIcon(32);
    wc.hIconSm = PhiAppIcon(16);
    // No background brush: an erase flashes system-white under the chrome
    // before PaintChrome covers it. WM_ERASEBKGND answers "done" instead.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kShellClassName;
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

}  // namespace

// The Phi app icon, shipped beside the exe (resources\phi.ico — the Mac's
// own artwork). Sized copies for the taskbar/alt-tab and the caption band.
HICON PhiAppIcon(int size) {
  wchar_t exe_path[MAX_PATH] = {};
  ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  std::wstring path(exe_path);
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) {
    return nullptr;
  }
  path.resize(slash + 1);
  path += L"resources\\phi.ico";
  return reinterpret_cast<HICON>(::LoadImageW(
      nullptr, path.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
}

// The Phi logo as premultiplied BGRA, extracted ONCE from the ICO's 256px
// entry — shared by the caption band button and the internal-page favicon
// fallback (the D2D path downscales it crisply to any slot).
const std::vector<unsigned char>& PhiLogoBgra256() {
  static base::NoDestructor<std::vector<unsigned char>> store;
  static bool tried = false;
  std::vector<unsigned char>& bgra = *store;
  if (tried) {
    return bgra;
  }
  tried = true;
  HICON big = PhiAppIcon(256);
  if (!big) {
    return bgra;
  }
  ICONINFO info = {};
  if (::GetIconInfo(big, &info) && info.hbmColor) {
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = 256;
    bmi.bmiHeader.biHeight = -256;  // Top-down.
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    std::vector<unsigned char> straight(256 * 256 * 4);
    HDC screen = ::GetDC(nullptr);
    if (screen && ::GetDIBits(screen, info.hbmColor, 0, 256, straight.data(),
                              &bmi, DIB_RGB_COLORS) == 256) {
      // Straight alpha -> premultiplied, what the D2D path expects.
      for (size_t px = 0; px + 3 < straight.size(); px += 4) {
        const unsigned a = straight[px + 3];
        straight[px] = static_cast<unsigned char>(straight[px] * a / 255);
        straight[px + 1] =
            static_cast<unsigned char>(straight[px + 1] * a / 255);
        straight[px + 2] =
            static_cast<unsigned char>(straight[px + 2] * a / 255);
      }
      bgra = std::move(straight);
    }
    if (screen) {
      ::ReleaseDC(nullptr, screen);
    }
    ::DeleteObject(info.hbmColor);
    if (info.hbmMask) {
      ::DeleteObject(info.hbmMask);
    }
  }
  ::DestroyIcon(big);
  return bgra;
}

const char* PhiLayoutModeName(PhiLayoutMode mode) {
  switch (mode) {
    case PhiLayoutMode::kBalanced:
      return "balanced";
    case PhiLayoutMode::kComfortable:
      return "comfortable";
    case PhiLayoutMode::kPerformance:
      break;
  }
  return "performance";
}

bool PhiLayoutModeFromName(const std::string& name, PhiLayoutMode* out) {
  if (name == "performance") {
    *out = PhiLayoutMode::kPerformance;
  } else if (name == "balanced") {
    *out = PhiLayoutMode::kBalanced;
  } else if (name == "comfortable") {
    *out = PhiLayoutMode::kComfortable;
  } else {
    return false;
  }
  return true;
}

int PhiWindowController::Scaled(int design_units) const {
  return ::MulDiv(design_units, static_cast<int>(dpi_), 96);
}

RECT PhiWindowController::Scaled(const RECT& design_units) const {
  return {Scaled(design_units.left), Scaled(design_units.top),
          Scaled(design_units.right), Scaled(design_units.bottom)};
}

void PhiWindowController::EnsureFont() {
  if (ui_font_ && ui_font_dpi_ == dpi_) {
    return;
  }
  // SystemParametersInfoForDpi gives the message font already sized for this
  // monitor; the plain SPI_GETNONCLIENTMETRICS returns the PRIMARY monitor's,
  // which is wrong the moment a window moves to a display at another scale.
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  HFONT built = nullptr;
  if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                   &metrics, 0, dpi_)) {
    // The face tracks the system (Segoe UI), the SIZE is the Mac's: 13pt row
    // titles everywhere (TabContentView.titleFontSize).
    LOGFONTW ui = metrics.lfMessageFont;
    ui.lfHeight = -Scaled(13);
    built = ::CreateFontIndirectW(&ui);
  }
  if (!built) {
    return;  // Keep whatever we had rather than falling back to no font.
  }
  HFONT previous = ui_font_;
  ui_font_ = built;
  ui_font_dpi_ = dpi_;
  if (url_field_) {
    ::SendMessageW(url_field_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_),
                   TRUE);
  }
  if (previous) {
    ::DeleteObject(previous);  // Replaced only after nothing else refers to it.
  }
  // The same face one weight up, for folder titles and group headers. GDI
  // keeps Segoe UI's semibold as a family of its own: asking "Segoe UI" for
  // weight 600 rounds up to Bold, so name the face when that is the system
  // font. Other faces (the CJK UI fonts) synthesize it.
  {
    LOGFONTW semibold = metrics.lfMessageFont;
    semibold.lfHeight = -Scaled(13);
    semibold.lfWeight = FW_SEMIBOLD;
    if (std::wstring_view(semibold.lfFaceName) == L"Segoe UI") {
      const std::wstring face = L"Segoe UI Semibold";
      std::fill(std::begin(semibold.lfFaceName),
                std::end(semibold.lfFaceName), L'\0');
      std::copy_n(face.begin(),
                  std::min(face.size(), std::size(semibold.lfFaceName) - 1),
                  std::begin(semibold.lfFaceName));
    }
    if (HFONT semibold_built = ::CreateFontIndirectW(&semibold)) {
      if (ui_semibold_font_) {
        ::DeleteObject(ui_semibold_font_);
      }
      ui_semibold_font_ = semibold_built;
    }
  }

  if (emoji_font_ && emoji_font_dpi_ == dpi_) {
    return;
  }
  LOGFONTW small = metrics.lfMessageFont;
  small.lfHeight = -Scaled(11);
  if (HFONT small_built = ::CreateFontIndirectW(&small)) {
    if (small_font_) {
      ::DeleteObject(small_font_);
    }
    small_font_ = small_built;
  }
  LOGFONTW emoji = metrics.lfMessageFont;
  // The unsafe-libc-call gate rules out wcscpy_s; copy the face name directly.
  const std::wstring emoji_face = L"Segoe UI Emoji";
  std::fill(std::begin(emoji.lfFaceName), std::end(emoji.lfFaceName), L'\0');
  std::copy_n(emoji_face.begin(),
              std::min(emoji_face.size(), std::size(emoji.lfFaceName) - 1),
              std::begin(emoji.lfFaceName));
  emoji.lfHeight = -Scaled(kSpaceIcon);
  HFONT emoji_built = ::CreateFontIndirectW(&emoji);
  if (emoji_built) {
    HFONT previous_emoji = emoji_font_;
    emoji_font_ = emoji_built;
    emoji_font_dpi_ = dpi_;
    if (previous_emoji) {
      ::DeleteObject(previous_emoji);
    }
  }

  if (caption_font_ && caption_font_dpi_ == dpi_) {
    return;
  }
  // The caption glyph font — the OS buttons' own glyphs (E921/E922/E923/E8BB).
  LOGFONTW caption = metrics.lfMessageFont;
  const std::wstring caption_face = L"Segoe MDL2 Assets";
  std::fill(std::begin(caption.lfFaceName), std::end(caption.lfFaceName),
            L'\0');
  std::copy_n(caption_face.begin(),
              std::min(caption_face.size(),
                       std::size(caption.lfFaceName) - 1),
              std::begin(caption.lfFaceName));
  caption.lfHeight = -Scaled(10);
  HFONT caption_built = ::CreateFontIndirectW(&caption);
  if (caption_built) {
    HFONT previous_caption = caption_font_;
    caption_font_ = caption_built;
    caption_font_dpi_ = dpi_;
    if (previous_caption) {
      ::DeleteObject(previous_caption);
    }
  }
  // The same MDL2 face at the EMOJI size, for Space-pip catalog glyphs — one
  // optical size whether a pip wears an emoji or a Phi icon.
  caption.lfHeight = -Scaled(kSpaceIcon);
  if (HFONT chip_built = ::CreateFontIndirectW(&caption)) {
    if (chip_icon_font_) {
      ::DeleteObject(chip_icon_font_);
    }
    chip_icon_font_ = chip_built;
  }
  // Both pip faces again at the vertical layouts' larger pip size — the GDI
  // fallback there, for a private-use glyph with no catalog artwork and for
  // emoji when DirectWrite is unavailable.
  caption.lfHeight = -Scaled(kSpaceIconVertical);
  if (HFONT pip_icon_built = ::CreateFontIndirectW(&caption)) {
    if (pip_icon_font_) {
      ::DeleteObject(pip_icon_font_);
    }
    pip_icon_font_ = pip_icon_built;
  }
  emoji.lfHeight = -Scaled(kSpaceIconVertical);
  if (HFONT pip_emoji_built = ::CreateFontIndirectW(&emoji)) {
    if (pip_emoji_font_) {
      ::DeleteObject(pip_emoji_font_);
    }
    pip_emoji_font_ = pip_emoji_built;
  }
  LOGFONTW pip_text = metrics.lfMessageFont;
  pip_text.lfHeight = -Scaled(kSpaceTextVertical);
  if (HFONT pip_text_built = ::CreateFontIndirectW(&pip_text)) {
    if (pip_text_font_) {
      ::DeleteObject(pip_text_font_);
    }
    pip_text_font_ = pip_text_built;
  }
}

void PhiWindowController::RebuildMenuBar() {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return;
  }
  menu_bar_ = std::make_unique<PhiMenuBar>(this);  // The old bar's HMENUs go
  menu_bar_->Install(shell_hwnd_);                 // with its destructor.
  menu_titles_.clear();  // Re-measured on the next paint, in the new language.
  menu_titles_dpi_ = 0;
  ::InvalidateRect(shell_hwnd_, nullptr, TRUE);
}

void PhiWindowController::EnsureMenuTitleRects() {
  if (menu_titles_dpi_ == dpi_ && !menu_titles_.empty()) {
    return;
  }
  if (!menu_bar_) {
    return;
  }
  const std::vector<std::wstring> titles = menu_bar_->Titles();
  if (titles.empty()) {
    return;
  }
  EnsureFont();
  HDC dc = ::GetDC(shell_hwnd_);
  if (!dc) {
    return;
  }
  HGDIOBJ previous = ui_font_ ? ::SelectObject(dc, ui_font_) : nullptr;
  menu_titles_.clear();
  int x = Scaled(kEdgeInset) - Scaled(kPhiTitlePad);
  const int band = Scaled(kTitleBarHeight);
  bool first = true;
  for (const std::wstring& title : titles) {
    SIZE extent = {};
    ::GetTextExtentPoint32W(dc, title.c_str(),
                            static_cast<int>(title.size()), &extent);
    int width = extent.cx + 2 * Scaled(kMenuTitlePad);
    if (first) {
      // The logo and "Phi" are ONE button: the first title's rect carries
      // the icon (18) and its gap (6) ahead of the text, behind the shorter
      // leading pad that aligns the logo with the sidebar content.
      width += Scaled(kPhiTitlePad) - Scaled(kMenuTitlePad) + Scaled(18) +
               Scaled(6);
      first = false;
    }
    menu_titles_.emplace_back(title, RECT{x, 0, x + width, band});
    x += width;
  }
  menu_titles_dpi_ = dpi_;
  if (previous) {
    ::SelectObject(dc, previous);
  }
  ::ReleaseDC(shell_hwnd_, dc);
}

PhiWindowController::Layout PhiWindowController::ComputeLayout() {
  Layout out;
  out.mode = layout_mode_;
  out.chip_size = kSpaceChip;  // The vertical strip below raises it.
  if (shell_hwnd_) {
    ::GetClientRect(shell_hwnd_, &out.client);
    if (const UINT dpi = ::GetDpiForWindow(shell_hwnd_)) {
      dpi_ = dpi;
    }
  }
  EnsureFont();
  if (content_fullscreen_) {
    // Content fullscreen: the hosted child owns every pixel of the shell;
    // every chrome rect stays empty.
    out.content = out.client;
    return out;
  }
  const int w = out.client.right;
  const int h = out.client.bottom;
  if (!sidebar_resizing_ && host_) {
    sidebar_dip_ = std::max(kSidebarWidthMin,
                            std::min(host_->SidebarWidth(),
                                     kSidebarWidthMax));
  }
  const int sidebar = Scaled(sidebar_dip_);
  // Collapsed: the content card widens to the left margin (which becomes
  // the floating sidebar's hover trigger). The floating panel paints with
  // floating_layout_ set, which restores the expanded layout.
  const bool hide_sidebar = sidebar_collapsed_ && !floating_layout_;
  const int inset = Scaled(kEdgeInset);
  const int btn = Scaled(kNavButton);
  const int m = Scaled(kContentMargin);

  // The caption band tops every mode: menu titles leading, minimize /
  // maximize / close trailing. The OS caption is gone (WM_NCCALCSIZE), so
  // this row is both the title bar and the menu bar.
  // Comfortable's band carries the tab strip too, so it is taller there.
  const int tb = Scaled(layout_mode_ == PhiLayoutMode::kComfortable
                            ? kTitleBarHeightTabs
                            : kTitleBarHeight);
  out.title_bar = {0, 0, w, tb};
  const int cap = Scaled(kCaptionButtonWidth);
  out.caption_close = {w - cap, 0, w, tb};
  out.caption_max = {w - 2 * cap, 0, w - cap, tb};
  out.caption_min = {w - 3 * cap, 0, w - 2 * cap, tb};
  // A 24pt control centred in the caption band, its right edge at `right`.
  // The band's trailing cluster is built right-to-left from the caption
  // buttons with this.
  const auto band_control = [&](int right) {
    return RECT{right - btn, (tb - btn) / 2, right, (tb + btn) / 2};
  };
  // Where the menu titles have to stop: the trailing cluster's left edge.
  // Filled in by the modes that build one; the caption buttons otherwise.
  out.band_limit = out.caption_min.left;
  // The seam between the band's last control and the caption buttons, set
  // by each mode once it knows what ends its cluster (search-tabs, in all
  // three). It centres in kBandTrailingGap and rides the cluster's own
  // line, so comfortable's copy follows the strip down with the rest.
  const auto place_band_divider = [&](const RECT& last) {
    const int mid = (last.top + last.bottom) / 2;
    const int half = Scaled(kBandDividerHeight) / 2;
    // A 1pt rule, so it thickens with the controls it separates: the band's
    // own bottom edge can stay one physical pixel because it runs the whole
    // window, but a 16pt stub at 200% would read as a scratch.
    const int x = (last.right + out.caption_min.left) / 2;
    out.band_divider = {x, mid - half, x + Scaled(1), mid + half};
  };

  // The Spaces strip: 24pt pips from the left (10pt strip padding), one
  // trailing "+" chip. The Mac's second destination (new profile) moved to the
  // Spaces menu.
  auto place_spaces = [&](int left, int right, int top) {
    out.chip_size = kSpaceChipVertical;  // Only the vertical layouts call this.
    const int pip = Scaled(out.chip_size);
    out.spaces_strip = {left, top, right, top + Scaled(kSpacesStripHeight)};
    const int chip_top = top + (Scaled(kSpacesStripHeight) - pip) / 2;
    out.chips_left = left + Scaled(kSpacesStripPad);
    out.chips_top = chip_top;
    out.add_space = {right - Scaled(kSpacesStripPad) - pip, chip_top,
                     right - Scaled(kSpacesStripPad), chip_top + pip};
    out.chips_limit = out.add_space.left - Scaled(kSpaceChipGap);
  };

  // The top bar of the two top-nav layouts: the first 40pt band INSIDE the
  // content card (the Mac's WebContentHeader is the card's first stack item),
  // back / forward / reload leading, the address capsule filling the middle.
  auto place_top_bar = [&](int left, int right, int top, int leading,
                          int height) {
    out.top_bar = {left, top, right, top + Scaled(height)};
    const int mid = top + Scaled(height) / 2;
    int x = left + Scaled(leading);
    out.back = {x, mid - btn / 2, x + btn, mid + btn / 2};
    x += btn + Scaled(kTopBarSpacing);
    out.forward = {x, mid - btn / 2, x + btn, mid + btn / 2};
    x += btn + Scaled(kTopBarSpacing);
    out.reload = {x, mid - btn / 2, x + btn, mid + btn / 2};
    return mid;
  };

  // The bookmark bar takes the content's first band when visible: the strip
  // shows the ground tint and the paint pass lays pills over it.
  const auto carve_bookmark_bar = [&](Layout* l) {
    if (!BookmarkBarVisible()) {
      return;
    }
    const int bar_h = Scaled(kBookmarkBarHeight);
    l->bookmark_bar = {l->content.left, l->content.top, l->content.right,
                       l->content.top + bar_h};
    l->content.top += bar_h;
  };

  if (layout_mode_ == PhiLayoutMode::kComfortable) {
    // Chrome's stack. The tab strip shares the caption band with the window
    // controls — the active tab merges downward into the toolbar — the
    // toolbar runs the full width under it, and the page goes EDGE TO EDGE:
    // no floating card, no margins. The window tint survives only behind
    // the tabs, which is the one band the card never covered anyway.
    //
    // This band has no menu titles: comfortable's menus live behind the
    // overflow button at the toolbar's trailing edge, as Chrome's do.
    out.band_limit = 0;

    // This band's controls sit on the TAB STRIP's centre line, not the
    // band's: the strip starts kHTabStripTop below the top edge and runs to
    // the band's bottom (where the active tab merges into the toolbar), so
    // anything centred on the band reads half that inset high against the
    // tabs it flanks. band_control's band centre is right for the layouts
    // whose band carries nothing but these controls.
    const int strip_mid = (Scaled(kHTabStripTop) + tb) / 2;
    // The Phi button leads the band — the corner the Mac's menu bar gives
    // the Phi menu, and where the vertical layouts start their titles. The
    // active Space's compact pip follows it (the Mac clears the traffic
    // lights with 80pt; Windows has none, so the edge inset does).
    out.phi_menu = {inset, strip_mid - btn / 2, inset + btn,
                    strip_mid + btn / 2};
    out.chips_left = out.phi_menu.right + Scaled(kTopBarSpacing);
    out.chips_top = strip_mid - Scaled(kSpaceChip) / 2;
    // One compact pip.
    out.chips_limit = out.chips_left + Scaled(kSpaceChip);
    // search-tabs keeps the trailing spot the vertical layouts park it in,
    // so the button does not move when the layout does — on the strip's
    // line here, the band's there.
    const int search_right = out.caption_min.left - Scaled(kBandTrailingGap);
    out.search_tabs = {search_right - btn, strip_mid - btn / 2, search_right,
                       strip_mid + btn / 2};
    place_band_divider(out.search_tabs);
    // The "+" follows the last tab; the strip is full when the next tab
    // would need the room the button is holding.
    const int new_tab_gap = Scaled(kTopBarSpacing);
    out.tabs_left = out.chips_limit + Scaled(kHTabStripLeading);
    out.tabs_top = Scaled(kHTabStripTop);
    out.tabs_limit = out.search_tabs.left - new_tab_gap - btn - new_tab_gap;
    out.horizontal_tabs = true;

    // The toolbar: back / forward / reload leading, the omnibox filling the
    // middle, then extensions, downloads and the overflow menu.
    const int mid =
        place_top_bar(0, w, tb, kComfortableLeading, kTopBarHeight);
    out.content = {0, out.top_bar.bottom, w, h};
    carve_bookmark_bar(&out);
    int r = w - Scaled(kTopBarLeading);
    out.overflow = {r - btn, mid - btn / 2, r, mid + btn / 2};
    r = out.overflow.left - Scaled(kTopBarSpacing);
    out.downloads = {r - btn, mid - btn / 2, r, mid + btn / 2};
    r = out.downloads.left - Scaled(kTopBarSpacing);
    out.extensions = {r - btn, mid - btn / 2, r, mid + btn / 2};
    // Feedback, Chat and the memory chip have no band to sit in here; they
    // are items in the overflow menu instead.
    const int field_h = Scaled(kOmniboxHeight);
    out.url_field = {out.reload.right + Scaled(kTopBarSpacing),
                     mid - field_h / 2,
                     out.extensions.left - Scaled(kTopBarSpacing),
                     mid + field_h / 2};
    const int icon = Scaled(kFaviconSlot);
    out.page_icon = {out.url_field.left + Scaled(10), mid - icon / 2,
                     out.url_field.left + Scaled(10) + icon, mid + icon / 2};
    // Reader View trails INSIDE the pill, where the Mac's
    // WebContentAddressBar keeps it (and where Chrome keeps its own reading
    // icon) — and ONLY when the active tab has an article, the same rule the
    // sidebar's capsule follows.
    if (ActiveTabReaderOfferable()) {
      const int fbtn = field_h - Scaled(2);
      const int fr = out.url_field.right - Scaled(2);
      out.reader = {fr - fbtn, mid - fbtn / 2, fr, mid + fbtn / 2};
    }
    return out;
  }

  // Vertical layouts: the Mac sidebar stack, top to bottom.
  out.sidebar = {0, 0, sidebar, h};

  if (layout_mode_ == PhiLayoutMode::kBalanced) {
    // Balanced: navigation and the address bar live in the top bar, and
    // search-tabs has left the sidebar header for the caption band — the
    // SAME trailing spot performance parks it in. That empties the header
    // row, so the Spaces strip starts at the sidebar's own inset.
    out.search_tabs =
        band_control(out.caption_min.left - Scaled(kBandTrailingGap));
    place_band_divider(out.search_tabs);
    out.band_limit = out.search_tabs.left;
    const int gap = tb + Scaled(kContentTopGap);
    if (!hide_sidebar) {
      // The strip starts on the card's own top edge, so the sidebar's Spaces
      // row and the card's address row read as ONE 32pt band across the
      // window rather than two rows that nearly line up.
      place_spaces(0, sidebar, gap);
    }

    const int bar_left = hide_sidebar ? m : sidebar;
    const int mid =
        place_top_bar(bar_left, w - m, gap, kTopBarLeading, kTopBarHeightNav);
    out.content = {bar_left, gap + Scaled(kTopBarHeightNav), w - m, h - m};
    carve_bookmark_bar(&out);
    // Chat capsule trailing (balanced shows it in the header, as on Mac).
    // With the AI suite off it is not laid out at all and the address bar
    // takes the room back.
    const int r2 = w - m - Scaled(kTopBarLeading);
    if (kPhiAiEnabled) {
      out.chat = {r2 - Scaled(kChatWidth), mid - Scaled(kPillHeight) / 2, r2,
                  mid + Scaled(kPillHeight) / 2};
    }
    const int field_h = Scaled(kTopBarField);
    const int field_right =
        kPhiAiEnabled ? out.chat.left - Scaled(kTopBarSpacing) : r2;
    out.url_field = {out.reload.right + Scaled(6), mid - field_h / 2,
                     field_right, mid + field_h / 2};
    // WebContentAddressBar's trailing stack, INSIDE the capsule: the Mac
    // keeps the extensions menu there in balanced exactly as SideAddressBar
    // keeps it in performance — 2pt from the trailing edge, and a 22pt glyph
    // box (its menuButton is a 22pt Lottie in a 24pt hover circle inside the
    // 26pt bar). The Mac's full order is reader, copy link, extensions menu;
    // copy link is the one still to slot in, between the two below.
    const int fmid = (out.url_field.top + out.url_field.bottom) / 2;
    const int fbtn = field_h - Scaled(2);
    const int fr = out.url_field.right - Scaled(2);
    out.extensions = {fr - fbtn, fmid - fbtn / 2, fr, fmid + fbtn / 2};
    if (ActiveTabReaderOfferable()) {
      const int rr = out.extensions.left - Scaled(2);
      out.reader = {rr - fbtn, fmid - fbtn / 2, rr, fmid + fbtn / 2};
    }
    out.tabs_top = out.spaces_strip.bottom + Scaled(kHeaderSpacer);
  } else {
    // Performance: back / forward / reload and search-tabs trail the caption
    // band, before the caption buttons — the sidebar has no header row at
    // all now, so the address bar sits directly under the Spaces strip. The
    // trio keeps its 1pt stack spacing and search-tabs follows it, at the
    // same x balanced puts it.
    int r = out.caption_min.left - Scaled(kBandTrailingGap);
    out.search_tabs = band_control(r);
    place_band_divider(out.search_tabs);
    r = out.search_tabs.left - Scaled(kBandGroupGap);
    out.reload = band_control(r);
    r = out.reload.left - Scaled(kNavGroupSpacing);
    out.forward = band_control(r);
    r = out.forward.left - Scaled(kNavGroupSpacing);
    out.back = band_control(r);
    out.band_limit = out.back.left;
    if (!hide_sidebar) {
      place_spaces(0, sidebar, tb + Scaled(kSpacesStripTop));
      out.url_field = {inset, tb + Scaled(kAddressBarTop), sidebar - inset,
                       tb + Scaled(kAddressBarTop + kAddressBarHeight)};
      // SideAddressBar's trailing stack INSIDE the capsule: the copy-link
      // button then the extensions-menu button, 24pt each, spacing 2,
      // trailing inset 2 (rightStackView metrics).
      const int fmid = (out.url_field.top + out.url_field.bottom) / 2;
      int fr = out.url_field.right - Scaled(2);
      out.extensions = {fr - btn, fmid - btn / 2, fr, fmid + btn / 2};
      fr = out.extensions.left - Scaled(2);
      out.copy_link = {fr - btn, fmid - btn / 2, fr, fmid + btn / 2};
      // Reader View leads the stack, and ONLY when the active tab has an
      // article — a reader button on a page with nothing to read is the
      // thing the Mac's "show it only where it has something to offer"
      // change removed.
      if (ActiveTabReaderOfferable()) {
        const int rr = out.copy_link.left - Scaled(2);
        out.reader = {rr - btn, fmid - btn / 2, rr, fmid + btn / 2};
        fr = out.reader.left;
      } else {
        fr = out.copy_link.left;
      }
      if (field_ext_count_ > 0) {
        // Pinned-extension icons lead the stack — but only while the text
        // keeps the Mac's 84pt minimum (minimumAddressTextWidth); below
        // that they hide wholesale, as on Mac.
        const int strip_w = field_ext_count_ * btn +
                            (field_ext_count_ - 1) * Scaled(2);
        const int text_left = out.url_field.left + Scaled(12);
        const int stack_left =
            out.reader.right > out.reader.left ? out.reader.left
                                               : out.copy_link.left;
        const int remaining =
            stack_left - Scaled(2) - strip_w - Scaled(8) - text_left;
        if (remaining >= Scaled(84)) {
          out.field_ext_strip = {stack_left - Scaled(2) - strip_w,
                                 fmid - btn / 2, stack_left - Scaled(2),
                                 fmid + btn / 2};
        }
      }
    }
    out.content = {hide_sidebar ? m : sidebar, tb + Scaled(kContentTopGap),
                   w - m, h - m};
    carve_bookmark_bar(&out);
    out.tabs_top = tb + Scaled(kHeaderHeight + kHeaderSpacer);
  }

  // The pinned-tab band sits between the header and the tab list. Empty until
  // favourites exist, exactly as on Mac (which mounts a zero-height container).
  // The pinned band, then the bookmark rows, then "+ New Tab" and the open
  // tabs — the Mac sidebar's order. A non-empty band carries the grid's own
  // 8pt top/bottom insets (PinnedTabLayout.insets).
  const int pinned_rows = pinned_row_count_;
  const int pinned_height =
      pinned_rows > 0 ? 2 * Scaled(kPinnedGap) +
                            pinned_rows * Scaled(kPinnedCellHeight) +
                            (pinned_rows - 1) * Scaled(kPinnedGap)
                      : 0;
  out.favorites = {0, out.tabs_top, sidebar, out.tabs_top + pinned_height};
  const int bookmarks_top = out.favorites.bottom +
                            (pinned_height > 0 ? Scaled(kFavoritesSpacer) : 0);
  const int bookmarks_height =
      bookmark_row_count_ * Scaled(kTabRowHeight + kTabRowGap);
  // A non-empty outline is parted from the tab list by the Mac's 16pt
  // separator row (makeAllItems).
  const int separator_h =
      bookmark_row_count_ > 0 ? Scaled(kBookmarkSeparatorHeight) : 0;
  const int separator_top = bookmarks_top + bookmarks_height;
  out.bookmark_separator =
      separator_h > 0 ? RECT{inset, separator_top, sidebar - inset,
                             separator_top + separator_h}
                      : RECT{};
  out.new_tab_row = {inset, separator_top + separator_h, sidebar - inset,
                     separator_top + separator_h + Scaled(kNewTabRowHeight)};
  out.tabs_top = out.new_tab_row.bottom + Scaled(kTabRowGap);

  // The bottom bar: downloads and memory leading (spacing 4), the Feedback
  // and Chat capsules trailing — SidebarBottomBar's row, settings not in it.
  const int bar_h = Scaled(kBottomBarHeight);
  const int bar_top = h - Scaled(kBottomSpacer) - bar_h;
  out.bottom_bar = {0, bar_top, sidebar, bar_top + bar_h};
  int bx = inset;
  out.downloads = {bx, bar_top, bx + btn, bar_top + bar_h};
  // The browser-memory chip follows downloads only while the AI suite is on;
  // otherwise nothing takes its slot.
  if (kPhiAiEnabled) {
    bx += btn + Scaled(4);
    out.memory = {bx, bar_top, bx + btn, bar_top + bar_h};
  }
  // No extensions button down here in any layout: the Mac's bottom bar holds
  // downloads, memory and the notification card leading, feedback and chat
  // trailing — the extensions menu lives inside whichever address bar the
  // layout has (SideAddressBar or WebContentAddressBar).
  int br = sidebar - inset;
  const int pill_top = bar_top + (bar_h - Scaled(kPillHeight)) / 2;
  if (layout_mode_ != PhiLayoutMode::kBalanced) {
    // The Chat capsule fills the whole 24pt row (ChatButton contentHeight).
    // Balanced holds its top-bar chat rect instead: the Mac hides the
    // bottom-bar chat whenever navigation rides at the top
    // (updateChatButtonVisibility's navigationAtTop) — the capsule lives
    // in the header there, and this block must not overwrite it.
    if (kPhiAiEnabled) {
      out.chat = {br - Scaled(kChatWidth), bar_top, br, bar_top + bar_h};
      br = out.chat.left - Scaled(4);
    }
  }
  // The narrow sidebar takes ViewThatFits' icon-only feedback variant.
  out.feedback = {br - Scaled(kFeedbackIconWidth), pill_top, br,
                  pill_top + Scaled(kPillHeight)};

  out.tabs_left = inset;
  out.tabs_limit = out.bottom_bar.top - Scaled(kFavoritesSpacer);
  if (hide_sidebar) {
    const RECT zero = {};
    out.favorites = zero;
    out.bookmark_separator = zero;
    out.new_tab_row = zero;
    out.bottom_bar = zero;
    out.downloads = zero;
    out.memory = zero;
    out.extensions = zero;
    out.feedback = zero;
    out.chat = zero;
    out.tabs_limit = out.tabs_top;  // No tab rows either.
  }
  return out;
}

RECT PhiWindowController::content_rect() {
  return ComputeLayout().content;
}

RECT PhiWindowController::url_field_rect() {
  return ComputeLayout().url_field;
}

RECT PhiWindowController::reader_button_rect() {
  return ComputeLayout().reader;
}

bool PhiWindowController::ActiveTabExtensionOfferable() const {
  return host_ && active_tab_id_ != 0 &&
         host_->IsReaderOfferable(active_tab_id_, active_tab_url_);
}

int PhiWindowController::tabs_top() {
  return ComputeLayout().tabs_top;
}

base::ListValue PhiWindowController::LayoutRowsJson() const {
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
    extra.Set("tabId", pin.tab_id);
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
    extra.Set("tabId", bookmark.tab_id);
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
    extra.Set("tabId", tab.id);
    extra.Set("tabIdB", tab.b_id);
    extra.Set("groupId", tab.group_id);
    extra.Set("active", tab.active);
    extra.Set("surface", surface_name(tab.attached_surface));
    extra.Set("surfaceBox", box(tab.surface_box));
    emit("tabs", tab.key, tab.bounds, std::move(extra));
  }
  return rows;
}

int PhiWindowController::tab_row_height() const {
  return Scaled(kTabRowHeight);
}

int PhiWindowController::tab_row_gap() const {
  return Scaled(kTabRowGap);
}

int PhiWindowController::title_bar_height() const {
  // Comfortable's band carries the tab strip as well as the window controls,
  // so it is taller there — this is what the control socket reports, and a
  // fixed answer would put every caller's arithmetic 8pt out in that layout.
  return Scaled(layout_mode_ == PhiLayoutMode::kComfortable
                    ? kTitleBarHeightTabs
                    : kTitleBarHeight);
}

int PhiWindowController::sidebar_width() const {
  return Scaled(sidebar_dip_);
}

int PhiWindowController::content_margin() const {
  return Scaled(kContentMargin);
}

POINT PhiWindowController::frame_offset() const {
  POINT origin = {0, 0};
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return origin;
  }
  RECT window = {};
  ::GetWindowRect(shell_hwnd_, &window);
  ::ClientToScreen(shell_hwnd_, &origin);
  return {origin.x - window.left, origin.y - window.top};
}

void PhiWindowController::ApplyPalette() {
  RefreshTabs();  // Every colour is read from the host at paint time.
}

void PhiWindowController::ApplyLayoutMode(PhiLayoutMode mode) {
  if (layout_mode_ == mode) {
    return;
  }
  layout_mode_ = mode;
  LayoutContents();  // Also repositions the address field for the new mode.
  RefreshTabs();
}

PhiWindowController::PhiWindowController(SlotId slot,
                                        BrowserId browser,
                                        HWND browser_hwnd,
                                        phi_framework::PhiFramework* framework,
                                        PhiSlotHost* host,
                                        bool agent,
                                        bool incognito)
    : slot_(slot),
      browser_hwnd_(browser_hwnd),
      framework_(framework),
      host_(host),
      agent_(agent),
      incognito_(incognito) {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    LOG(ERROR) << "[PhiClient] adopt: invalid browser HWND for browser "
               << browser.value << " (slot " << slot_.value << ")";
    return;
  }
  EnsureShellClass();
  if (host_) {
    layout_mode_ = host_->LayoutMode();  // One layout for the whole client.
  }

  // Create the client shell so its CONTENT REGION lands exactly where the
  // browser window already is — the shell is one sidebar wider, plus its frame.
  //
  // Sizing the shell to the browser's own bounds instead would hand Chromium a
  // content area one sidebar NARROWER than the bounds it restored. Chromium
  // saves that shrunken rect as its restored bounds on close, so every restart
  // would lose another 240px until the window hit Chromium's minimum size.
  // Anchoring the content instead makes the round trip a fixed point.
  RECT wr = {};
  ::GetWindowRect(browser_hwnd_, &wr);
  if (const UINT dpi = ::GetDpiForWindow(browser_hwnd_)) {
    dpi_ = dpi;  // Lay the first frame out for the monitor we open on.
  }
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  int w = Scaled(1200 + sidebar_dip_), h = Scaled(800);
  // Distrust a restored rect that is mostly off-screen (a stale placement
  // saved from an off-screen window, or a monitor that is gone): anchoring
  // the shell to it would open as a sliver at the screen edge. Chromium's
  // own restore only guarantees ~30px stays visible, so require a usable
  // amount before honoring it.
  if (wr.right > wr.left && wr.bottom > wr.top) {
    HMONITOR monitor = ::MonitorFromRect(&wr, MONITOR_DEFAULTTONULL);
    MONITORINFO info = {sizeof(info)};
    RECT visible = {};
    if (!monitor || !::GetMonitorInfoW(monitor, &info) ||
        !::IntersectRect(&visible, &wr, &info.rcWork) ||
        visible.right - visible.left < 300 ||
        visible.bottom - visible.top < 200) {
      wr = {};  // Fall through to the CW_USEDEFAULT defaults above.
    }
  }
  if (wr.right > wr.left && wr.bottom > wr.top) {
    // The shell's frame is CUSTOM (WM_NCCALCSIZE): the client reaches the
    // window's top edge and only the resize borders remain non-client, so
    // AdjustWindowRectEx (which assumes an OS caption) would be wrong. The
    // content region sits at (sidebar, titleBand + topGap) inside the client;
    // place the window so that region lands exactly on the browser's bounds.
    const int border_x = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi_) +
                         ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
    const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi_) +
                         ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
    // Where the content region starts inside the client, PER LAYOUT — the
    // numbers ComputeLayout produces: comfortable has no sidebar and stacks
    // the taller band and the toolbar; balanced adds the navigation bar
    // below the band and the gap; performance is the band and the gap. The
    // trailing margin is the content card's everywhere but comfortable,
    // whose page runs edge to edge. A mode-blind conversion was placing
    // balanced and comfortable windows a bar (or a sidebar) off on their
    // first frame.
    int content_left = Scaled(sidebar_dip_);
    int content_top = Scaled(kTitleBarHeight) + Scaled(kContentTopGap);
    int trailing = Scaled(kContentMargin);
    switch (layout_mode_) {
      case PhiLayoutMode::kComfortable:
        content_left = 0;
        content_top = Scaled(kTitleBarHeightTabs) + Scaled(kTopBarHeight);
        trailing = 0;
        break;
      case PhiLayoutMode::kBalanced:
        content_top += Scaled(kTopBarHeightNav);
        break;
      default:
        break;
    }
    x = wr.left - content_left - border_x;
    y = wr.top - content_top;
    w = (wr.right + trailing + border_x) - x;
    h = (wr.bottom + trailing + border_y) - y;
  }

  // FIRST RUN — and any rect rejected above: there is no shell placement to
  // inherit, only Chromium's DEFAULT bounds, which sit a tile inside the work
  // area's top-left. Anchoring the CONTENT to those drags the sidebar off the
  // left edge and the caption band off the top: the window comes up with the
  // page pinned to the screen corner and its chrome out of reach. Open the
  // SHELL off that corner instead, by a margin of its own. Only the position
  // is chosen here — the size the anchor produced is Chromium's own default,
  // kept unless it would overflow the margin on the far edges. From the next
  // launch the saved rect reproduces this placement through the round trip
  // above, so this runs once per profile.
  MONITORINFO home = {sizeof(home)};
  if (::GetMonitorInfoW(
          ::MonitorFromWindow(browser_hwnd_, MONITOR_DEFAULTTOPRIMARY),
          &home) &&
      (x == CW_USEDEFAULT || x < home.rcWork.left || y < home.rcWork.top)) {
    const int work_w = home.rcWork.right - home.rcWork.left;
    const int work_h = home.rcWork.bottom - home.rcWork.top;
    // The margin gives way on a work area too small to hold it.
    const int margin = std::min(Scaled(kDefaultPlacementMargin),
                                std::min(work_w, work_h) / 8);
    x = home.rcWork.left + margin;
    y = home.rcWork.top + margin;
    w = std::min(w, work_w - 2 * margin);
    h = std::min(h, work_h - 2 * margin);
  }

  // A TEAR-OFF opens AROUND the point the tab was dropped at — the Mac's
  // resolvedTearOffWindowFrame: the drop's normalized position in the work
  // area becomes its position inside the new window, so a drop near the
  // top-left corner puts the window's top-left there, and one near the
  // bottom-right pulls the window up and left to stay on screen. The size
  // is whatever the placement above produced.
  POINT drop = {};
  if (!agent_ && host_ && host_->TakeTearOffDrop(&drop)) {
    MONITORINFO target = {sizeof(target)};
    if (::GetMonitorInfoW(::MonitorFromPoint(drop, MONITOR_DEFAULTTONEAREST),
                          &target)) {
      const RECT& work = target.rcWork;
      const int work_w = work.right - work.left;
      const int work_h = work.bottom - work.top;
      if (work_w > 1 && work_h > 1) {
        w = std::min(w, work_w);
        h = std::min(h, work_h);
        const double rx = std::min(
            1.0, std::max(0.0, static_cast<double>(drop.x - work.left) /
                                   work_w));
        const double ry = std::min(
            1.0, std::max(0.0, static_cast<double>(drop.y - work.top) /
                                   work_h));
        x = std::max<int>(work.left,
                          std::min<int>(drop.x - static_cast<int>(rx * w),
                                        work.right - w));
        y = std::max<int>(work.top,
                          std::min<int>(drop.y - static_cast<int>(ry * h),
                                        work.bottom - h));
      }
    }
  }

  if (agent_) {
    // A hidden agent surface: fixed size, parked far off-screen, kept off the
    // taskbar. It is still shown (below) so its content composites and CDP
    // screen capture works — the user simply never sees it.
    x = -32000;
    y = -32000;
    w = Scaled(1400 + sidebar_dip_);
    h = Scaled(900);
  }
  shell_hwnd_ = ::CreateWindowExW(
      agent_ ? (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE) : 0, kShellClassName,
      L"Phi", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr,
      nullptr, ::GetModuleHandleW(nullptr), this);
  if (!shell_hwnd_) {
    LOG(ERROR) << "[PhiClient] shell CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }

  // Force one frame recalculation now that the proc can see `this`: whatever
  // non-client layout the creation pass produced, this pass runs through OUR
  // WM_NCCALCSIZE and retires the OS caption for good.
  ::SetWindowPos(shell_hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);

  // The menus (never attached with SetMenu — the caption band draws the
  // titles itself).
  menu_bar_ = std::make_unique<PhiMenuBar>(this);
  menu_bar_->Install(shell_hwnd_);

  AdoptBrowserAsChild();
  EnsureUrlField();
  // The window the slot was created with backs the DEFAULT Space — whichever
  // Space holds that role now (it moves when its holder is deleted).
  const std::string default_space =
      host_ ? host_->DefaultSpaceId() : std::string("1");
  space_browsers_[default_space] = browser;
  browser_hwnds_[browser] = browser_hwnd_;
  space_order_.push_back(default_space);
  active_space_ = default_space;
  active_browser_ = browser;
  if (host_ && !incognito_ && !agent_) {
    // The default Space binds here rather than through AttachSpaceBrowser,
    // so it needs its own note or lazy restore would never learn its window.
    // NEVER for an agent/shadow window: it nominally binds the default Space
    // too, and noting it made the Space's remembered window THE AGENT'S —
    // an agent alive at quit then put the agent window (omitted from the
    // session) in the cold-start eager set and PARKED the user's real one.
    host_->NoteSpaceWindow(default_space, browser.value);
  }
  if (framework_ && !incognito_ && !agent_) {
    // An incognito shell stays OUT of the Space registry: routed URLs must
    // never resolve into an off-the-record window — and so does an agent
    // window, which the framework registers as "space.agent" itself.
    framework_->SetWindowSpace(browser.value, default_space.c_str());
  }
  LayoutContents();
  ::ShowWindow(shell_hwnd_, agent_ ? SW_SHOWNOACTIVATE : SW_SHOW);
  LOG(INFO) << "[PhiClient] " << (agent_ ? "agent " : "") << "shell window "
            << slot_.value << " browser=" << browser.value
            << " shell_hwnd=" << shell_hwnd_ << " browser_hwnd="
            << browser_hwnd_;
}

PhiWindowController::~PhiWindowController() {
  // From here on the shell's own messages (DestroyWindow below still
  // delivers WM_ACTIVATE / WM_SIZE / WM_CAPTURECHANGED) find a controller
  // that must do nothing but die.
  destroying_ = true;
  // A window closing under a live drag would leave the split-drop cards
  // holding this controller. Only one drag runs at a time (it owns the
  // mouse capture), so taking them down unconditionally is safe.
  PhiSplitDropOverlay::Hide();
  PhiDragImage::OwnerGone(shell_hwnd_);
  // The process-wide satellites bound to this window: each keeps a bare
  // PhiWindowController* and outlives its shell otherwise (the agent
  // overlay's Reposition runs from EVERY shell's LayoutContents, the
  // overview and the switcher answer keys, the search panel a click).
  PhiAgentOverlay::HideIfBoundTo(this);
  PhiGroupOverview::CloseIfBoundTo(this);
  PhiTabSwitcher::CancelIfBoundTo(this);
  PhiSearchTabsPanel::CloseIfBoundTo(this);
  // Owned components and popups go BEFORE the shell: their teardown calls
  // back into a controller that is still whole, rather than running as a
  // side effect of the shell's DestroyWindow on one that is half gone.
  tab_preview_.reset();
  bookmark_manager_.reset();
  new_tab_page_.reset();
  create_space_panel_.reset();
  menu_bar_.reset();
  for (HWND* popup : {&hover_card_popup_, &omnibox_popup_, &floating_sidebar_,
                      &status_bubble_}) {
    if (*popup && ::IsWindow(*popup)) {
      ::DestroyWindow(*popup);
    }
    *popup = nullptr;
  }
  for (HWND& cap : corner_caps_) {
    if (cap && ::IsWindow(cap)) {
      ::DestroyWindow(cap);
    }
    cap = nullptr;
  }
  // Detach the browser child before tearing down our window, so its teardown
  // is not entangled with ours. Full close plumbing is a later slice.
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetParent(browser_hwnd_, nullptr);
  }
  if (shell_hwnd_ && ::IsWindow(shell_hwnd_)) {
    ::KillTimer(shell_hwnd_, kDeferredCloseTimer);
    ::DestroyWindow(shell_hwnd_);  // Takes the address field child with it.
  }
  if (logo_icon_) {
    ::DestroyIcon(logo_icon_);
  }
  if (ui_font_) {
    ::DeleteObject(ui_font_);
  }
  if (emoji_font_) {
    ::DeleteObject(emoji_font_);
  }
  if (caption_font_) {
    ::DeleteObject(caption_font_);
  }
  if (chip_icon_font_) {
    ::DeleteObject(chip_icon_font_);
  }
  if (pip_icon_font_) {
    ::DeleteObject(pip_icon_font_);
  }
  if (pip_emoji_font_) {
    ::DeleteObject(pip_emoji_font_);
  }
  if (pip_text_font_) {
    ::DeleteObject(pip_text_font_);
  }
  if (small_font_) {
    ::DeleteObject(small_font_);
  }
  if (ui_semibold_font_) {
    ::DeleteObject(ui_semibold_font_);
  }
  if (back_buffer_) {
    ::DeleteObject(back_buffer_);
  }
  for (HBRUSH brush : band_brushes_) {
    ::DeleteObject(brush);
  }
  if (url_brush_) {
    ::DeleteObject(url_brush_);
  }
}

void PhiWindowController::EnsureUrlField() {
  if (url_field_ || !shell_hwnd_) {
    return;
  }
  url_field_ = ::CreateWindowExW(
      0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0,
      10, 10, shell_hwnd_, nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (!url_field_) {
    LOG(ERROR) << "[PhiClient] address field CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }
  EnsureFont();  // The shell's message font for THIS monitor's scaling.
  if (ui_font_) {
    ::SendMessageW(url_field_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_),
                   TRUE);
  }
  // The Mac's placeholder, shown by the EDIT itself while empty (and by the
  // capture path, which renders without children).
  ::SendMessageW(url_field_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(
                     PhiStr("addressBar.configuration.defaultPlaceholder")));
  AttachOmniboxField(url_field_);
  PositionUrlField(ComputeLayout());
}

void PhiWindowController::AttachOmniboxField(HWND edit) {
  if (!edit || !::IsWindow(edit)) {
    return;
  }
  ::SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  WNDPROC previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
      edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&UrlFieldProc)));
  if (!g_edit_base_proc) {
    g_edit_base_proc = previous;  // The EDIT class proc — one for all fields.
  }
}

HWND PhiWindowController::FocusedOmniboxField() const {
  const HWND focus = ::GetFocus();
  if (!focus) {
    return nullptr;
  }
  if (url_field_ && focus == url_field_) {
    return url_field_;
  }
  if (new_tab_page_ && new_tab_page_->field() &&
      focus == new_tab_page_->field()) {
    return new_tab_page_->field();
  }
  return nullptr;
}

void PhiWindowController::PositionUrlField(const Layout& layout) {
  if (!url_field_) {
    return;
  }
  // Sits INSIDE the painted field box, vertically centred: a single-line EDIT
  // draws its text at the top of its own rect, so filling the whole box would
  // leave the text hugging the upper edge.
  const int text_height = Scaled(20);
  const RECT& box = layout.url_field;
  // SideAddressBar: text starts 12 in from the pill's leading edge, 8 clear
  // before the in-field trailing stack (or the bare trailing edge without
  // one) — the EDIT must not extend under the buttons or it eats their
  // clicks.
  int right_stop = box.right;
  // Whatever the trailing stack holds in this layout — the pinned-icon strip
  // and copy link in performance, the extensions menu in either — the text
  // stops at the LEFTMOST of them. Rects that sit outside the capsule (the
  // comfortable layout keeps its extensions button in the top bar) are not
  // candidates.
  for (const RECT& accessory : {layout.field_ext_strip, layout.reader,
                                layout.copy_link, layout.extensions}) {
    if (accessory.right > accessory.left && accessory.left > box.left &&
        accessory.left < right_stop) {
      right_stop = accessory.left;
    }
  }
  const int text_left = layout.page_icon.right > layout.page_icon.left
                           ? layout.page_icon.right + Scaled(8)
                           : box.left + Scaled(12);
  const int width = right_stop - Scaled(8) - text_left;
  const RECT target = {text_left,
                       box.top + (box.bottom - box.top - text_height) / 2,
                       text_left + (width > 0 ? width : 1),
                       box.top + (box.bottom - box.top - text_height) / 2 +
                           text_height};
  // Idempotent, because the paint path calls this too: the in-field accessory
  // set changes with the page — the reader button comes and goes — and the
  // EDIT is a real child window, so a stale width paints straight over
  // whatever just appeared beside it. Moving only on a real change keeps that
  // call from feeding itself more paints.
  RECT current = {};
  if (::GetWindowRect(url_field_, &current) &&
      ::MapWindowPoints(HWND_DESKTOP, shell_hwnd_,
                        reinterpret_cast<POINT*>(&current), 2) &&
      ::EqualRect(&current, &target)) {
    return;
  }
  ::MoveWindow(url_field_, target.left, target.top,
               target.right - target.left, target.bottom - target.top, TRUE);
}

void PhiWindowController::SyncUrlField(bool force) {
  if (!url_field_) {
    return;
  }
  // Never overwrite a half-typed URL: nav state refreshes on every paint and
  // on every framework notification.
  if (!force && ::GetFocus() == url_field_) {
    return;
  }
  const int len = ::GetWindowTextLengthW(url_field_);
  std::vector<wchar_t> current(static_cast<size_t>(len) + 1, L'\0');
  if (len > 0) {
    ::GetWindowTextW(url_field_, current.data(), len + 1);
  }
  if (nav_url_ == current.data()) {
    return;  // Unchanged; setting it again would fight the caret.
  }
  url_field_programmatic_ = true;  // Not the user typing — no autocomplete.
  ::SetWindowTextW(url_field_, nav_url_.c_str());
  url_field_programmatic_ = false;
}

void PhiWindowController::CommitUrlField() {
  CommitOmniboxField(url_field_);
}

void PhiWindowController::CommitOmniboxField(HWND field) {
  if (!field || !::IsWindow(field) || !framework_) {
    return;
  }
  const int len = ::GetWindowTextLengthW(field);
  std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
  if (len > 0) {
    ::GetWindowTextW(field, buffer.data(), len + 1);
  }
  std::wstring typed(buffer.data());
  const size_t first = typed.find_first_not_of(L" \t");
  const size_t last = typed.find_last_not_of(L" \t");
  typed = (first == std::wstring::npos) ? std::wstring()
                                        : typed.substr(first, last - first + 1);
  if (typed.empty()) {
    return;
  }
  if (new_tab_page_ && new_tab_page_->visible()) {
    // omniBoxDidClear: the native page blanks its controls now, so nothing
    // of the new-tab page lingers over the arriving one; the rebuild that
    // sees the new URL unmounts it.
    ntp_commit_tab_id_ = active_tab_id_;
    new_tab_page_->HideControls();
  }
  // The framework resolves what was typed the way the omnibox would — a bare
  // host becomes a URL, free text becomes a search.
  const BrowserId browser = active_browser_id();
  if (!browser.valid()) {
    return;  // No browser attached: nowhere for the text to go.
  }
  if (omnibox_new_tab_pending_ || placeholder_browsers_.count(browser)) {
    // Omnibox new-tab mode — and a placeholder-mode browser, whose strip is
    // EMPTY (Navigate alone would no-op): the tab is born on commit. The
    // fresh tab is active, so Navigate applies the full typed-text
    // resolution to it (NewTab itself loads URLs literally).
    omnibox_new_tab_pending_ = false;
    framework_->NewTab(browser.value, "");
  }
  // The typed URL replaces the ORIGIN tab's page, which the reader is
  // covering — leave the reader, or what arrives loads unseen behind it
  // (the Mac's closeReaderOverlayForAddressBarNavigation).
  LeaveReaderViewForNavigation();
  framework_->Navigate(browser.value, base::WideToUTF8(typed).c_str());
  FocusActiveBrowser();  // Keys belong to the page again once we've navigated.
  RefreshTabs();
}

void PhiWindowController::OnContentFullscreenChanged(BrowserId browser_id,
                                                     bool fullscreen) {
  if (!shell_hwnd_) {
    return;
  }
  if (fullscreen) {
    fullscreen_browser_ = browser_id;
    if (content_fullscreen_) {
      return;
    }
    content_fullscreen_ = true;
    // The standard borderless-fullscreen recipe: remember placement and
    // style, strip the frame, cover the window's monitor.
    pre_fullscreen_placement_.length = sizeof(WINDOWPLACEMENT);
    ::GetWindowPlacement(shell_hwnd_, &pre_fullscreen_placement_);
    pre_fullscreen_style_ = ::GetWindowLongPtrW(shell_hwnd_, GWL_STYLE);
    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);
    ::GetMonitorInfoW(
        ::MonitorFromWindow(shell_hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
    ::SetWindowLongPtrW(
        shell_hwnd_, GWL_STYLE,
        pre_fullscreen_style_ &
            ~static_cast<LONG_PTR>(WS_CAPTION | WS_THICKFRAME));
    ::SetWindowPos(shell_hwnd_, HWND_TOP, monitor.rcMonitor.left,
                   monitor.rcMonitor.top,
                   monitor.rcMonitor.right - monitor.rcMonitor.left,
                   monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                   SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
  } else {
    // Exits arrive from whichever tab leaves fullscreen; only the one that
    // took the shell fullscreen releases it.
    if (!content_fullscreen_ ||
        (fullscreen_browser_.valid() && browser_id != fullscreen_browser_)) {
      return;
    }
    content_fullscreen_ = false;
    fullscreen_browser_ = BrowserId();
    ::SetWindowLongPtrW(shell_hwnd_, GWL_STYLE, pre_fullscreen_style_);
    ::SetWindowPlacement(shell_hwnd_, &pre_fullscreen_placement_);
    ::SetWindowPos(shell_hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
  LayoutContents();
  ::InvalidateRect(shell_hwnd_, nullptr, TRUE);
}

void PhiWindowController::OnPlaceholderModeChanged(BrowserId browser_id,
                                                   bool entered) {
  if (entered) {
    placeholder_browsers_.insert(browser_id);
  } else {
    placeholder_browsers_.erase(browser_id);
  }
  if (browser_id == active_browser_id()) {
    if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
      ::ShowWindow(browser_hwnd_, entered ? SW_HIDE : SW_SHOW);
    }
    if (entered && url_field_ && ::IsWindowVisible(url_field_)) {
      // The address bar stays the way in (the Mac keeps it usable).
      ::SetFocus(url_field_);
      ::SendMessageW(url_field_, EM_SETSEL, 0, -1);
    }
    RefreshTabs();
    if (shell_hwnd_) {
      ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    }
  }
}

void PhiWindowController::OpenFeedback() {
  if (!host_) {
    return;
  }
  // About the focused tab of the VISIBLE browser (the active Space's), whose
  // profile also supplies the report's system logs.
  const BrowserId browser = active_browser_id();
  if (!browser.valid()) {
    return;  // No browser, no focused tab to report about.
  }
  PhiFeedbackContext context;
  context.window_id = browser.value;
  for (const TabRow& row : tabs_) {
    if (row.active) {
      context.url = base::WideToUTF8(row.url);
      context.title = base::WideToUTF8(row.title);
      break;
    }
  }
  if (context.url.empty()) {
    context.url = active_tab_url_;
  }
  host_->RequestFeedback(slot_.value, context);
}

void PhiWindowController::StartNewTab() {
  // "New tab behavior = Omnibox": Ctrl+T / the New Tab row focus the
  // address field instead of opening a page; the tab is born on commit
  // (the Mac's toggleOmniBox new-tab mode). The page mode opens the NTP.
  if (host_ && host_->NewTabOpensOmnibox() && url_field_ &&
      ::IsWindowVisible(url_field_)) {
    omnibox_new_tab_pending_ = true;
    ::SetFocus(url_field_);
    ::SendMessageW(url_field_, EM_SETSEL, 0, -1);
    return;
  }
  const BrowserId browser = active_browser_id();
  if (framework_ && browser.valid()) {
    framework_->NewTab(browser.value, "");
    RefreshTabs();
  }
}

void PhiWindowController::OnOmniboxFieldChanged(HWND field) {
  // EN_CHANGE while the user types: run/refresh the autocomplete query.
  // Programmatic sets (SyncUrlField, arrowing onto a row) are suppressed.
  if (url_field_programmatic_ || !field || !framework_ ||
      ::GetFocus() != field) {
    return;
  }
  const int len = ::GetWindowTextLengthW(field);
  std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
  if (len > 0) {
    ::GetWindowTextW(field, buffer.data(), len + 1);
  }
  const std::wstring typed(buffer.data());
  if (typed.empty()) {
    CloseOmniboxPopup(/*stop_query=*/true);
    return;
  }
  const BrowserId browser = active_browser_id();
  if (!browser.valid()) {
    return;
  }
  framework_->StartOmniboxQuery(browser.value, base::WideToUTF8(typed).c_str());
}

void PhiWindowController::OnOmniboxResults(BrowserId browser_id,
                                           const std::string& json) {
  if (browser_id != active_browser_id() || !FocusedOmniboxField()) {
    return;  // A stale query, or the user is no longer typing.
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  omnibox_rows_.clear();
  omnibox_selected_ = -1;
  if (root) {
    if (const base::ListValue* matches = root->FindList("matches")) {
      for (const base::Value& v : *matches) {
        const base::DictValue* m = v.GetIfDict();
        if (!m ||
            omnibox_rows_.size() >= static_cast<size_t>(kOmniboxMaxRows)) {
          continue;
        }
        OmniboxRow row;
        row.line =
            m->FindInt("line").value_or(static_cast<int>(omnibox_rows_.size()));
        if (const std::string* s = m->FindString("type")) {
          row.type = *s;
        }
        if (const std::string* s = m->FindString("contents")) {
          row.contents = base::UTF8ToWide(*s);
        }
        if (const std::string* s = m->FindString("description")) {
          row.description = base::UTF8ToWide(*s);
        }
        if (const std::string* s = m->FindString("fillIntoEdit")) {
          row.fill = base::UTF8ToWide(*s);
        }
        row.has_tab_match = m->FindBool("hasTabMatch").value_or(false);
        omnibox_rows_.push_back(std::move(row));
      }
    }
  }
  if (omnibox_rows_.empty()) {
    CloseOmniboxPopup(/*stop_query=*/false);
  } else {
    ShowOmniboxPopup();
  }
}

void PhiWindowController::ShowOmniboxPopup() {
  const HWND field = FocusedOmniboxField();
  if (!shell_hwnd_ || !field) {
    return;
  }
  EnsureOmniboxPopupClass();
  if (!omnibox_popup_) {
    omnibox_popup_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PhiOmniboxPopup", L"",
        WS_POPUP, 0, 0, 0, 0, shell_hwnd_, nullptr,
        ::GetModuleHandleW(nullptr), this);
  }
  if (!omnibox_popup_) {
    return;
  }
  const int height =
      static_cast<int>(omnibox_rows_.size()) * Scaled(kOmniboxRowHeight) +
      Scaled(12);
  RECT anchor = {};
  int left = 0;
  int width = 0;
  if (new_tab_page_ && field == new_tab_page_->field()) {
    // Flush under the new-tab page's box and exactly as wide — the Mac's
    // suggestion list grows out of the box itself.
    anchor = new_tab_page_->BoxScreenRect();
    left = anchor.left;
    width = anchor.right - anchor.left;
  } else {
    ::GetWindowRect(field, &anchor);
    left = anchor.left - Scaled(8);
    width = std::max<int>(anchor.right - anchor.left + Scaled(16),
                          Scaled(320));
  }
  ::SetWindowPos(omnibox_popup_, HWND_TOP, left, anchor.bottom + Scaled(6),
                 width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::InvalidateRect(omnibox_popup_, nullptr, TRUE);
}

void PhiWindowController::CloseOmniboxPopup(bool stop_query) {
  omnibox_rows_.clear();
  omnibox_selected_ = -1;
  if (stop_query) {
    omnibox_new_tab_pending_ = false;  // The gesture was abandoned.
  }
  if (omnibox_popup_ && ::IsWindow(omnibox_popup_)) {
    ::ShowWindow(omnibox_popup_, SW_HIDE);
  }
  const BrowserId browser = active_browser_id();
  if (stop_query && framework_ && browser.valid()) {
    framework_->StopOmniboxQuery(browser.value);
  }
}

void PhiWindowController::MoveOmniboxSelection(int delta) {
  if (omnibox_rows_.empty()) {
    return;
  }
  const int count = static_cast<int>(omnibox_rows_.size());
  int next = omnibox_selected_ + delta;
  if (next < -1) {
    next = count - 1;  // Up from the field wraps to the last row.
  } else if (next >= count) {
    next = -1;  // Down past the last row returns to the field's own text.
  }
  omnibox_selected_ = next;
  const HWND field = FocusedOmniboxField();
  if (omnibox_selected_ >= 0 && field) {
    const OmniboxRow& row =
        omnibox_rows_[static_cast<size_t>(omnibox_selected_)];
    if (!row.fill.empty()) {
      // The row's fillIntoEdit lands in the field, caret at the end — the
      // omnibox arrow behavior.
      url_field_programmatic_ = true;
      ::SetWindowTextW(field, row.fill.c_str());
      ::SendMessageW(field, EM_SETSEL, row.fill.size(), row.fill.size());
      url_field_programmatic_ = false;
    }
  }
  if (omnibox_popup_) {
    ::InvalidateRect(omnibox_popup_, nullptr, TRUE);
  }
}

void PhiWindowController::CommitOmniboxSelection() {
  if (omnibox_selected_ < 0 ||
      omnibox_selected_ >= static_cast<int>(omnibox_rows_.size()) ||
      !framework_) {
    return;
  }
  const int line = omnibox_rows_[static_cast<size_t>(omnibox_selected_)].line;
  // The framework opens it with Chromium's own semantics (fixup,
  // switch-to-tab, shortcuts learning). Omnibox new-tab mode opens it in a
  // FRESH tab.
  const bool new_tab = omnibox_new_tab_pending_;
  omnibox_new_tab_pending_ = false;
  const BrowserId browser = active_browser_id();
  if (!browser.valid()) {
    return;
  }
  framework_->OpenOmniboxMatch(browser.value, line, new_tab);
  CloseOmniboxPopup(/*stop_query=*/false);
  FocusActiveBrowser();
  RefreshTabs();
}

int PhiWindowController::OmniboxRowAtY(int y) const {
  const int row = (y - Scaled(6)) / Scaled(kOmniboxRowHeight);
  return row >= 0 && row < static_cast<int>(omnibox_rows_.size()) ? row : -1;
}

void PhiWindowController::OmniboxHover(int y) {
  const int row = OmniboxRowAtY(y);
  if (row >= 0 && row != omnibox_selected_) {
    omnibox_selected_ = row;
    if (omnibox_popup_) {
      ::InvalidateRect(omnibox_popup_, nullptr, TRUE);
    }
  }
}

void PhiWindowController::OmniboxClick(int y) {
  const int row = OmniboxRowAtY(y);
  if (row >= 0) {
    omnibox_selected_ = row;
    CommitOmniboxSelection();
  }
}

void PhiWindowController::PaintOmniboxPopup(HWND popup, HDC dc) {
  RECT rc = {};
  ::GetClientRect(popup, &rc);
  const PhiPalette palette = CurrentPalette();
  HBRUSH bg = ::CreateSolidBrush(palette.card);
  ::FillRect(dc, &rc, bg);
  ::DeleteObject(bg);
  HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
  ::Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
  ::SelectObject(dc, old_brush);
  ::SelectObject(dc, old_pen);
  ::DeleteObject(pen);
  ::SetBkMode(dc, TRANSPARENT);
  for (size_t i = 0; i < omnibox_rows_.size(); ++i) {
    const OmniboxRow& row = omnibox_rows_[i];
    RECT r = {rc.left + Scaled(6),
              rc.top + Scaled(6) +
                  static_cast<int>(i) * Scaled(kOmniboxRowHeight),
              rc.right - Scaled(6), 0};
    r.bottom = r.top + Scaled(kOmniboxRowHeight);
    if (static_cast<int>(i) == omnibox_selected_) {
      PhiFillRoundRectAA(dc, r, Scaled(6), palette.hover_strong);
    }
    // Type glyph: search rows a magnifier, bookmarks a star, history a
    // clock, everything else a globe.
    const wchar_t glyph =
        row.type.find("search") != std::string::npos    ? L''
        : row.type.find("bookmark") != std::string::npos ? L''
        : row.type.find("history") != std::string::npos  ? L''
                                                         : L'';
    RECT glyph_rc = {r.left + Scaled(4), r.top, r.left + Scaled(28),
                     r.bottom};
    HGDIOBJ glyph_font =
        chip_icon_font_ ? ::SelectObject(dc, chip_icon_font_) : nullptr;
    ::SetTextColor(dc, palette.label_dim);
    const wchar_t glyph_text[2] = {glyph, L'\0'};
    PhiDrawTextAA(dc, glyph_text, 1, &glyph_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (glyph_font) {
      ::SelectObject(dc, glyph_font);
    }
    RECT text_rc = {glyph_rc.right + Scaled(6), r.top, r.right - Scaled(8),
                    r.bottom};
    HGDIOBJ prev_font = ui_font_ ? ::SelectObject(dc, ui_font_) : nullptr;
    ::SetTextColor(dc, palette.label);
    RECT measure = text_rc;
    PhiDrawTextAA(dc, row.contents.c_str(), -1, &measure,
                DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    RECT contents_rc = text_rc;
    contents_rc.right = std::min(
        text_rc.right,
        static_cast<LONG>(text_rc.left + (measure.right - measure.left)));
    PhiDrawTextAA(dc, row.contents.c_str(), -1, &contents_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    if (!row.description.empty() &&
        contents_rc.right + Scaled(16) < text_rc.right) {
      RECT desc_rc = {contents_rc.right + Scaled(8), r.top, text_rc.right,
                      r.bottom};
      ::SetTextColor(dc, palette.label_dim);
      const std::wstring desc = L"— " + row.description;
      PhiDrawTextAA(dc, desc.c_str(), -1, &desc_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    }
    if (prev_font) {
      ::SelectObject(dc, prev_font);
    }
  }
}

// static
LRESULT CALLBACK PhiWindowController::UrlFieldProc(HWND hwnd,
                                                   UINT msg,
                                                   WPARAM wparam,
                                                   LPARAM lparam) {
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiWindowController*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && self->destroying_) {
    // The field dies with the shell; its WM_KILLFOCUS would still reach the
    // framework for a window that is going.
    return ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
  }
  switch (msg) {
    case WM_KEYDOWN:
      if (wparam == VK_RETURN) {
        if (self) {
          if (self->omnibox_selected_ >= 0) {
            self->CommitOmniboxSelection();
          } else {
            // stop_query=false: the omnibox-new-tab pending flag must
            // survive into the commit (the query dies with focus anyway).
            self->CloseOmniboxPopup(/*stop_query=*/false);
            self->CommitOmniboxField(hwnd);
          }
        }
        return 0;
      }
      if (wparam == VK_ESCAPE) {
        if (self) {
          if (self->omnibox_popup_ &&
              ::IsWindowVisible(self->omnibox_popup_)) {
            // First Escape closes the list; the second reverts the field.
            self->CloseOmniboxPopup(/*stop_query=*/true);
            return 0;
          }
          if (hwnd != self->url_field_) {
            // The new-tab page's omnibox has no URL to revert to: Escape
            // clears what was typed and keeps the caret.
            ::SetWindowTextW(hwnd, L"");
            return 0;
          }
          self->SyncUrlField(/*force=*/true);
          self->FocusActiveBrowser();
        }
        return 0;
      }
      if ((wparam == VK_DOWN || wparam == VK_UP) && self &&
          !self->omnibox_rows_.empty()) {
        self->MoveOmniboxSelection(wparam == VK_DOWN ? 1 : -1);
        return 0;
      }
      break;
    case WM_CHAR:
      // Both keys are handled above; letting them reach the EDIT would only
      // produce the "unhandled character" beep.
      if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
        return 0;
      }
      break;
    case WM_KILLFOCUS:
      // The user left the field: the dropdown has nothing to complete.
      if (self) {
        self->CloseOmniboxPopup(/*stop_query=*/true);
      }
      break;
    case WM_MOUSEMOVE:
      // The capsule's hover brighten while the cursor rides the EDIT (the
      // child swallows the shell's mouse moves over the text area). The
      // new-tab page's field has no capsule.
      if (self && hwnd == self->url_field_ && !self->url_field_hover_) {
        self->SetUrlFieldHover(true);
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      break;
    case WM_MOUSELEAVE:
      if (self && hwnd == self->url_field_) {
        self->SetUrlFieldHover(false);
      }
      break;
    case WM_SETFOCUS: {
      const LRESULT result =
          ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
      ::SendMessageW(hwnd, EM_SETSEL, 0, -1);  // Typing replaces the whole URL.
      return result;
    }
  }
  return ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
}

void PhiWindowController::AdoptBrowserAsChild() {
  // Strip the top-level chrome/styles and make Chromium's window a clipped
  // child of our shell. SetParent on a WS_CHILD window is a true child reparent
  // (hwnd_message_handler.cc SetParentOrOwner), so it clips into our content
  // rect and stops being an independent taskbar/Alt-Tab entry.
  LONG_PTR style = ::GetWindowLongPtrW(browser_hwnd_, GWL_STYLE);
  style &= ~(WS_POPUP | WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME |
             WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
  style |= WS_CHILD | WS_CLIPSIBLINGS;
  ::SetWindowLongPtrW(browser_hwnd_, GWL_STYLE, style);
  // Drop the top-level extended frame edges too; without this + FRAMECHANGED,
  // Windows keeps the old non-client metrics and paints a dead band at the
  // top of the content region.
  LONG_PTR ex = ::GetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE);
  ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
          WS_EX_STATICEDGE | WS_EX_APPWINDOW);
  ::SetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE, ex);
  ::SetParent(browser_hwnd_, shell_hwnd_);
  // Watch its z-order: Chromium raises this window on activation, which
  // would bury the corner caps riding above it.
  if (BrowserBaseProcs().find(browser_hwnd_) == BrowserBaseProcs().end()) {
    WNDPROC previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        browser_hwnd_, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&BrowserSubclassProc)));
    BrowserBaseProcs()[browser_hwnd_] = previous;
  }
  ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
}

namespace {

// A corner cap: a tiny shell child ABOVE the browser child, shaped (window
// region) to the part of a radius-square OUTSIDE the rounded corner's arc.
// The browser renders via DirectComposition, which ignores the child's own
// window region — but hwnd AIRSPACE is honoured: a sibling above occludes.
// The cap paints the ground tint (plus the card's 1px arc), so the page
// reads as a rounded card exactly like the Mac's.
LRESULT CALLBACK CornerCapProc(HWND hwnd, UINT msg, WPARAM wparam,
                               LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_PAINT) {
    auto* self = reinterpret_cast<PhiWindowController*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    PAINTSTRUCT ps = {};
    HDC dc = ::BeginPaint(hwnd, &ps);
    if (self) {
      self->PaintCornerCap(hwnd, dc);
    }
    ::EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_NCHITTEST) {
    return HTTRANSPARENT;  // The tint under it takes the click.
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

ATOM EnsureCornerCapClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &CornerCapProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhiCornerCap";
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

// The status bubble rides the content card's bottom-left like the corner
// caps do: a shell-child sibling raised over the reparented browser (the
// only way client pixels appear over the page).
LRESULT CALLBACK StatusBubbleProc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_PAINT) {
    auto* self = reinterpret_cast<PhiWindowController*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    PAINTSTRUCT ps = {};
    HDC dc = ::BeginPaint(hwnd, &ps);
    if (self) {
      self->PaintStatusBubble(hwnd, dc);
    }
    ::EndPaint(hwnd, &ps);
    return 0;
  }
  if (msg == WM_NCHITTEST) {
    return HTTRANSPARENT;  // Hovers keep belonging to the page under it.
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

ATOM EnsureStatusBubbleClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &StatusBubbleProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhiStatusBubble";
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

// The omnibox suggestion dropdown: non-activating, so the address field
// keeps focus and caret while the list is up (clicks commit rows without a
// focus dance).
LRESULT CALLBACK OmniboxPopupProc(HWND hwnd, UINT msg, WPARAM wparam,
                                  LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiWindowController*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        self->PaintOmniboxPopup(hwnd, dc);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (self) {
        self->OmniboxHover(GET_Y_LPARAM(lparam));
      }
      return 0;
    case WM_LBUTTONDOWN:
      if (self) {
        self->OmniboxClick(GET_Y_LPARAM(lparam));
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

ATOM EnsureOmniboxPopupClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &OmniboxPopupProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhiOmniboxPopup";
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

// One row of the framework's tab list. The split-drop paths map ids to
// indices and back several times inside a single drop (a NewTab shifts every
// index after it), so they read the strip ONCE and walk this instead.
struct StripTab {
  int index = -1;
  int id = 0;
  bool active = false;
  std::string tag;
  std::string split_id;
  std::string group_id;
  std::string url;
  std::string title;
  bool unloaded = false;  // DORMANT: history not loaded yet.
  SidebarSurface surface = SidebarSurface::kContent;
  int origin_id = 0;
};

std::vector<StripTab> ReadStrip(phi_framework::PhiFramework* framework,
                                int64_t browser) {
  std::vector<StripTab> strip;
  if (!framework) {
    return strip;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework->ListTabsJson(browser), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return strip;
  }
  for (const base::Value& v : parsed->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    if (!d) {
      continue;
    }
    StripTab tab;
    tab.index = d->FindInt("index").value_or(-1);
    tab.id = d->FindInt("id").value_or(0);
    tab.active = d->FindBool("active").value_or(false);
    if (const std::string* tag = d->FindString("tag")) {
      tab.tag = *tag;
    }
    if (const std::string* split = d->FindString("splitId")) {
      tab.split_id = *split;
    }
    if (const std::string* group = d->FindString("groupId")) {
      tab.group_id = *group;
    }
    if (const std::string* title = d->FindString("title")) {
      tab.title = *title;
    }
    tab.unloaded = d->FindBool("unloaded").value_or(false);
    if (const std::string* url = d->FindString("url")) {
      tab.url = *url;
    }
    // The framework names surfaces (a reader page recognised from what it
    // shows, committed or not); the client parses no reader URLs.
    if (const std::string* surface = d->FindString("surface")) {
      if (*surface == "reader") {
        tab.surface = SidebarSurface::kReader;
        tab.origin_id = d->FindInt("originTabId").value_or(0);
      }
    }
    strip.push_back(std::move(tab));
  }
  return strip;
}

const StripTab* StripTabById(const std::vector<StripTab>& strip, int id) {
  if (id == 0) {
    return nullptr;
  }
  for (const StripTab& tab : strip) {
    if (tab.id == id) {
      return &tab;
    }
  }
  return nullptr;
}

// The strip index of `id` right now — resolved fresh against a strip read
// after the last mutation, never carried across one.
int StripIndexOfId(const std::vector<StripTab>& strip, int id) {
  const StripTab* tab = StripTabById(strip, id);
  return tab ? tab->index : -1;
}

// The live tab bound to a stored row, or `fallback` when the row has none.
// A pinned tab or bookmark is OPENED by the very press that arms its drag, so
// the view row the drag was armed from can still say "closed" when the drop
// lands — and opening its url a second time would leave a duplicate tab
// beside the pane.
int LiveTabForStoredRow(const std::vector<StripTab>& strip,
                        const std::string& guid,
                        int fallback) {
  if (guid.empty()) {
    return fallback;
  }
  for (const StripTab& tab : strip) {
    if (tab.tag == guid) {
      return tab.id;
    }
  }
  return fallback;
}

RECT ToRect(const SidebarRect& r) {
  return RECT{r.left, r.top, r.right, r.bottom};
}

int StripIdAtIndex(const std::vector<StripTab>& strip, int index) {
  for (const StripTab& tab : strip) {
    if (tab.index == index) {
      return tab.id;
    }
  }
  return 0;
}

}  // namespace

namespace {

// The ground tint at `y` of a `height`-tall window: the band gradient's
// colour there, which is what a corner cap shows outside the card's arc.
COLORREF GroundTintAt(const PhiPalette& palette, int y, int height) {
  height = height > 0 ? height : 1;
  auto channel = [&](int top, int bottom) {
    return top + (bottom - top) * y / height;
  };
  return RGB(
      channel(GetRValue(palette.ground_top), GetRValue(palette.ground_bottom)),
      channel(GetGValue(palette.ground_top), GetGValue(palette.ground_bottom)),
      channel(GetBValue(palette.ground_top),
              GetBValue(palette.ground_bottom)));
}

}  // namespace

bool PhiWindowController::CommitCornerCap(int index) {
  if (index < 0 || index > 3 || !corner_caps_[index] ||
      !::IsWindow(corner_caps_[index])) {
    return false;
  }
  RECT rc = {};
  ::GetClientRect(corner_caps_[index], &rc);
  const int radius = rc.right;
  if (radius <= 0 || rc.bottom != radius) {
    return false;
  }
  const Layout layout = ComputeLayout();
  const PhiPalette palette = CurrentPalette();
  const int y = index < 2 ? layout.content.top
                          : layout.content.bottom - radius;
  // The card's 1px hairline rides the ring just outside the arc — the same
  // circle the frame's PhiStrokeRoundRectAA of kContentRadius + 1 follows,
  // so the two meet at the cap's edge.
  return PhiCommitCornerCap(corner_caps_[index], index, radius,
                            GroundTintAt(palette, y, layout.client.bottom),
                            palette.border);
}

void PhiWindowController::RefreshCornerCaps() {
  for (int i = 0; i < 4; ++i) {
    HWND cap = corner_caps_[i];
    if (!cap || !::IsWindow(cap)) {
      continue;
    }
    const bool layered =
        (::GetWindowLongPtrW(cap, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
    if (!layered || !CommitCornerCap(i)) {
      ::InvalidateRect(cap, nullptr, TRUE);
    }
  }
}

// The GDI path, for a cap whose layer could not be committed and so keeps
// the region cut: the tint over the bite, and the hairline around it.
void PhiWindowController::PaintCornerCap(HWND cap, HDC dc) {
  int corner = -1;
  for (int i = 0; i < 4; ++i) {
    if (corner_caps_[i] == cap) {
      corner = i;
    }
  }
  if (corner < 0) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(cap, &rc);
  const Layout layout = ComputeLayout();
  const PhiPalette palette = CurrentPalette();
  // The ground tint at this cap's height, matching the band gradient.
  const int y = corner < 2 ? layout.content.top
                           : layout.content.bottom - rc.bottom;
  HBRUSH tint =
      ::CreateSolidBrush(GroundTintAt(palette, y, layout.client.bottom));
  ::FillRect(dc, &rc, tint);
  ::DeleteObject(tint);
  // The card's 1px border, continued around the arc: the ring just OUTSIDE
  // the region's circle (same centre, radius + 1), which is also the circle
  // the shell's frame stroke follows, so the two meet at the cap's edge.
  // A ring on the region's own circle would fall inside the bite and be
  // clipped away — the corners then showed a bare, unstroked arc.
  const int r = rc.right;
  const int cx = (corner == 0 || corner == 2) ? r : 0;
  const int cy = corner < 2 ? r : 0;
  PhiStrokeEllipseAA(dc, {cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1},
                     palette.border, 1);
}

void PhiWindowController::OnTargetURLChanged(BrowserId browser_id,
                                             const std::wstring& url) {
  if (agent_) {
    return;  // Off-screen agent surfaces show no chrome.
  }
  if (browser_id != active_browser_id()) {
    return;  // A hidden Space's page; its bubble would lie about this one.
  }
  SetStatusUrl(url);
}

void PhiWindowController::SetStatusUrl(const std::wstring& url) {
  if (url == status_url_) {
    return;
  }
  status_url_ = url;
  if (url.empty()) {
    if (status_bubble_) {
      ::ShowWindow(status_bubble_, SW_HIDE);
    }
    return;
  }
  if (!shell_hwnd_) {
    return;
  }
  EnsureStatusBubbleClass();
  if (!status_bubble_) {
    // Layered: its rounded corner is alpha, not a region's stair-step.
    status_bubble_ = ::CreateWindowExW(
        WS_EX_LAYERED, L"PhiStatusBubble", L"", WS_CHILD, 0, 0, 0, 0,
        shell_hwnd_, nullptr, ::GetModuleHandleW(nullptr), this);
  }
  if (!status_bubble_) {
    return;
  }
  const Layout layout = ComputeLayout();
  const int height = Scaled(24);
  SIZE text = {};
  if (HDC dc = ::GetDC(shell_hwnd_)) {
    HGDIOBJ old_font = small_font_ ? ::SelectObject(dc, small_font_)
                                   : nullptr;
    ::GetTextExtentPoint32W(dc, status_url_.c_str(),
                            static_cast<int>(status_url_.size()), &text);
    if (old_font) {
      ::SelectObject(dc, old_font);
    }
    ::ReleaseDC(shell_hwnd_, dc);
  }
  // At most half the content width (the Mac StatusURLView's cap).
  const int max_width = std::max<int>(
      Scaled(60), (layout.content.right - layout.content.left) / 2);
  const int width = std::min<int>(text.cx + Scaled(24), max_width);
  ::SetWindowPos(status_bubble_, HWND_TOP, layout.content.left,
                 layout.content.bottom - height, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
  // Only the top-right corner rounds. The bubble's surface is committed
  // with that corner as alpha coverage; should the layer fail, the same
  // shape goes on as a window region (oversized so the other three corners
  // fall outside the window) and the bubble paints through WM_PAINT.
  const int r = Scaled(8);
  const bool layered =
      (::GetWindowLongPtrW(status_bubble_, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
  if (layered &&
      PhiCommitShapedLayer(
          status_bubble_, width, height,
          [&](HDC dc) { PaintStatusBubble(status_bubble_, dc); },
          [&](double px, double py) {
            return PhiRoundRectCoverage(px, py, width, height, r,
                                        /*corners=*/0x2);
          })) {
    return;
  }
  if (layered) {
    ::SetWindowLongPtrW(
        status_bubble_, GWL_EXSTYLE,
        ::GetWindowLongPtrW(status_bubble_, GWL_EXSTYLE) & ~WS_EX_LAYERED);
  }
  HRGN shape = ::CreateRoundRectRgn(-2 * r, 0, width + 1, height + 2 * r + 1,
                                    2 * r, 2 * r);
  ::SetWindowRgn(status_bubble_, shape, TRUE);  // Window owns it now.
  ::InvalidateRect(status_bubble_, nullptr, TRUE);
}

void PhiWindowController::PaintStatusBubble(HWND bubble, HDC dc) {
  RECT rc = {};
  ::GetClientRect(bubble, &rc);
  const PhiPalette palette = CurrentPalette();
  HBRUSH fill = ::CreateSolidBrush(palette.card);
  ::FillRect(dc, &rc, fill);
  ::DeleteObject(fill);
  // The card border along the exposed top and right edges: a rounded rect
  // oversized past the left and bottom, so only its top-right arc shows.
  const int r = Scaled(8);
  PhiStrokeRoundRectAA(dc, {-2 * r, 0, rc.right, rc.bottom + 2 * r}, r,
                       palette.border, 1);
  RECT text = rc;
  text.left += Scaled(10);
  text.right -= Scaled(12);
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, palette.label);
  HGDIOBJ old_font = small_font_ ? ::SelectObject(dc, small_font_) : nullptr;
  // Middle truncation, the way a URL keeps its host and tail readable.
  PhiDrawTextAA(dc, status_url_.c_str(), -1, &text,
              DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
  if (old_font) {
    ::SelectObject(dc, old_font);
  }
}

RECT PhiWindowController::ContentRect() {
  return ComputeLayout().content;
}

void PhiWindowController::ToggleSidebar() {
  if (ComputeLayout().horizontal_tabs) {
    return;  // Comfortable has no sidebar to collapse.
  }
  sidebar_collapsed_ = !sidebar_collapsed_;
  HideFloatingSidebar(/*animated=*/false);
  LayoutContents();
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

void PhiWindowController::ToggleDownloadsPopover() {
  // The menu / Ctrl+J entry to the same popover the sidebar button toggles.
  // The popover's own opening grace period absorbs the menu teardown's
  // activation churn, so this can run inline.
  const BrowserId browser = active_browser_id();
  if (!host_ || !framework_ || !shell_hwnd_ || !browser.valid()) {
    return;
  }
  RECT anchor = ComputeLayout().downloads;
  ::MapWindowPoints(shell_hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor),
                    2);
  PhiDownloadsPopover::Toggle(shell_hwnd_, anchor,
                              host_->PaletteForSlot(slot_.value), framework_,
                              browser);
}

void PhiWindowController::LayoutContents() {
  if (destroying_) {
    return;  // The shell's own DestroyWindow sends WM_SIZE on the way down.
  }
  if (bookmark_manager_ && bookmark_manager_->visible()) {
    bookmark_manager_->Reposition();
  }
  if (new_tab_page_ && new_tab_page_->visible()) {
    new_tab_page_->Reposition();
  }
  // Reader and peek panels are hosted OVER the content card, in windows the
  // coordinator owns rather than this controller — they have to follow it.
  if (host_) {
    host_->NoteContentLayoutChanged(slot_.value);
  }
  if (!shell_hwnd_) {
    return;
  }
  ScheduleRefresh();  // Row bounds derive from the client size.
  SetStatusUrl(std::wstring());  // Anchored to the OLD content rect.
  CloseOmniboxPopup(/*stop_query=*/false);  // Anchored to the OLD field rect.
  const Layout layout = ComputeLayout();
  PositionUrlField(layout);
  if (creating_space_) {
    // The form lives only in a docked vertical sidebar: a layout switch or
    // a collapse ends it (the Mac re-routes to its standalone window).
    if (layout_mode_ == PhiLayoutMode::kComfortable || sidebar_collapsed_) {
      CancelCreateSpaceForm();
    } else if (create_space_panel_) {
      create_space_panel_->Reposition(CreateSpaceFormRect());
    }
  }
  if (url_field_) {
    const bool field_hidden =
        content_fullscreen_ ||
        (sidebar_collapsed_ && layout.mode == PhiLayoutMode::kPerformance);
    ::ShowWindow(url_field_, field_hidden ? SW_HIDE : SW_SHOW);
  }
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    const int w = layout.content.right - layout.content.left;
    const int h = layout.content.bottom - layout.content.top;
    // A child awaiting the slide-end reveal is PARKED outside the client
    // area — the parent clips children, so it stays invisible even if
    // Chromium shows it on its own (fresh browsers do, post-creation).
    const int x = (swap_start_ && swap_reveal_ == browser_hwnd_)
                      ? layout.client.right
                      : layout.content.left;
    // A placeholder-mode browser stays HIDDEN: its strip is empty and the
    // dead tab's final frame would linger in the content card.
    const bool placeholding =
        placeholder_browsers_.count(active_browser_id()) > 0;
    ::SetWindowPos(browser_hwnd_, nullptr, x, layout.content.top,
                   w > 0 ? w : 0, h > 0 ? h : 0,
                   SWP_NOZORDER | SWP_NOACTIVATE |
                       (placeholding ? SWP_HIDEWINDOW : SWP_SHOWWINDOW));
  }

  // Watch-mode overlay (fog + control pill) tracks the content rect.
  PhiAgentOverlay::Reposition();

  // The rounded card corners. DComp ignores a region ON the browser child
  // (verified by pixel probe: corners stayed page-white), so the corners are
  // capped by sibling windows above it instead.
  EnsureCornerCapClass();
  const int radius = Scaled(kContentRadius);
  // Top caps only where the content's top edge meets the tint (performance);
  // under a top bar the card's top corners belong to the bar band.
  const bool top_caps = layout.mode == PhiLayoutMode::kPerformance;
  const std::array<POINT, 4> positions = {{
      {layout.content.left, layout.content.top},
      {layout.content.right - radius, layout.content.top},
      {layout.content.left, layout.content.bottom - radius},
      {layout.content.right - radius, layout.content.bottom - radius},
  }};
  for (int i = 0; i < 4; ++i) {
    if (!corner_caps_[i]) {
      // Layered (per-pixel alpha, a child since Windows 8): the arc is a
      // coverage ramp, not a region's pixel-grid cut.
      corner_caps_[i] = ::CreateWindowExW(
          WS_EX_LAYERED, L"PhiCornerCap", L"", WS_CHILD, 0, 0, radius,
          radius, shell_hwnd_, nullptr, ::GetModuleHandleW(nullptr), this);
    }
    if (!corner_caps_[i]) {
      continue;
    }
    const bool shown = (i >= 2 || top_caps) && !content_fullscreen_;
    ::SetWindowPos(corner_caps_[i], HWND_TOP, positions[i].x, positions[i].y,
                   radius, radius,
                   SWP_NOACTIVATE | (shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    // Shape: the radius-square minus the quarter circle — only the bite
    // outside the card's curve shows. The circle is centred on the cap's
    // inner corner with radius `radius` exactly; the cap strokes the ring
    // one pixel outside it, and the shell's frame uses the same circle, so
    // the hairline runs unbroken around the arc. The layer carries the
    // shape as alpha; only if it cannot be committed does the cap fall
    // back to a window region (the same shape, on the pixel grid).
    const bool layered =
        (::GetWindowLongPtrW(corner_caps_[i], GWL_EXSTYLE) & WS_EX_LAYERED) !=
        0;
    if (layered && CommitCornerCap(i)) {
      continue;
    }
    if (layered) {
      ::SetWindowLongPtrW(
          corner_caps_[i], GWL_EXSTYLE,
          ::GetWindowLongPtrW(corner_caps_[i], GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }
    HRGN square = ::CreateRectRgn(0, 0, radius + 1, radius + 1);
    const int cx = (i == 0 || i == 2) ? radius : 0;
    const int cy = i < 2 ? radius : 0;
    HRGN circle = ::CreateEllipticRgn(cx - radius, cy - radius, cx + radius,
                                      cy + radius);
    ::CombineRgn(square, square, circle, RGN_DIFF);
    ::DeleteObject(circle);
    ::SetWindowRgn(corner_caps_[i], square, TRUE);  // Window owns it now.
    ::InvalidateRect(corner_caps_[i], nullptr, TRUE);
  }
}

bool PhiWindowController::InSidebarResizeBand(POINT client_pt) {
  if (!host_ || host_->LayoutMode() == PhiLayoutMode::kComfortable) {
    return false;
  }
  const Layout layout = ComputeLayout();
  return client_pt.y >= layout.title_bar.bottom &&
         client_pt.x >= Scaled(sidebar_dip_) - Scaled(4) &&
         client_pt.x <= Scaled(sidebar_dip_);
}

void PhiWindowController::ArmDrag(const SidebarKey& key, POINT pt) {
  drag_key_ = key;
  switch (key.kind) {
    case SidebarKey::Kind::kTab:
    case SidebarKey::Kind::kSplit:
      drag_kind_ = DragKind::kTab;
      break;
    case SidebarKey::Kind::kCell:
    case SidebarKey::Kind::kFolder:
      drag_kind_ = std::any_of(pinned_.begin(), pinned_.end(),
                               [&](const ShortcutRow& p) { return p.key == key; })
                       ? DragKind::kPin
                       : DragKind::kBookmark;
      break;
    case SidebarKey::Kind::kSpace:
      drag_kind_ = DragKind::kChip;
      break;
    case SidebarKey::Kind::kGroup:
    case SidebarKey::Kind::kNone:
      drag_kind_ = DragKind::kNone;
      break;
  }
  // Read BEFORE the caller acts on the press: every arming site activates the
  // row it grabbed (a tab) or opens it (a pinned tab, a bookmark), so this is
  // the last moment the page the user is actually looking at is still the
  // active one. A split drop pairs the dragged row with THAT page.
  drag_focus_tab_id_ = active_tab_id_;
  drag_origin_ = pt;
  drag_point_ = pt;
  drag_active_ = false;
  drag_insert_ = -1;
  drag_place_valid_ = false;
  drag_folder_key_ = {};
}

int PhiWindowController::DragIndex() const {
  switch (drag_kind_) {
    case DragKind::kTab:
      for (size_t i = 0; i < tabs_.size(); ++i) {
        if (tabs_[i].key == drag_key_) return static_cast<int>(i);
      }
      return -1;
    case DragKind::kPin:
      for (size_t i = 0; i < pinned_.size(); ++i) {
        if (pinned_[i].key == drag_key_) return static_cast<int>(i);
      }
      return -1;
    case DragKind::kBookmark:
      for (size_t i = 0; i < bookmarks_.size(); ++i) {
        if (bookmarks_[i].key == drag_key_) return static_cast<int>(i);
      }
      return -1;
    case DragKind::kChip:
      for (size_t i = 0; i < chips_.size(); ++i) {
        if (chips_[i].id == drag_key_.id) return static_cast<int>(i);
      }
      return -1;
    case DragKind::kNone:
      break;
  }
  return -1;
}

bool PhiWindowController::Apply(const SidebarCommand& command) {
  return host_ && host_->Apply(slot_.value, command, sidebar_tree_);
}

std::vector<DropRow> PhiWindowController::TabDropRows() const {
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

std::vector<DropRow> PhiWindowController::PinnedDropRows() const {
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

std::vector<DropRow> PhiWindowController::BookmarkDropRows() const {
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

PhiWindowController::SlideVec PhiWindowController::RowSlideOffset(
    const std::string& guid) const {
  const auto to = row_slide_to_.find(guid);
  if (to == row_slide_to_.end()) {
    return SlideVec{};
  }
  SlideVec from;
  if (const auto f = row_slide_from_.find(guid);
      f != row_slide_from_.end()) {
    from = f->second;
  }
  if (!drag_slide_start_) {
    return to->second;
  }
  const double linear = std::min(
      1.0, static_cast<double>(::GetTickCount64() - drag_slide_start_) /
               kDragSlideMs);
  const double eased = linear * linear * (3.0 - 2.0 * linear);
  return SlideVec{from.dx + (to->second.dx - from.dx) * eased,
                  from.dy + (to->second.dy - from.dy) * eased};
}

double PhiWindowController::DragSlideOffset(int tab_id) const {
  const auto to = drag_slide_to_.find(tab_id);
  if (to == drag_slide_to_.end()) {
    return 0.0;
  }
  double from = 0.0;
  if (const auto f = drag_slide_from_.find(tab_id);
      f != drag_slide_from_.end()) {
    from = f->second;
  }
  if (!drag_slide_start_) {
    return to->second;
  }
  const double linear = std::min(
      1.0, static_cast<double>(::GetTickCount64() - drag_slide_start_) /
               kDragSlideMs);
  const double eased = linear * linear * (3.0 - 2.0 * linear);
  return from + (to->second - from) * eased;
}

void PhiWindowController::RetargetDragSlide() {
  // The new leg starts wherever every element currently is (mid-flight
  // retargeting, as Chromium's animating layout manager does).
  std::map<int, double> from;
  for (const auto& [id, target] : drag_slide_to_) {
    from[id] = DragSlideOffset(id);
  }
  std::map<std::string, SlideVec> row_from;
  for (const auto& [guid, target] : row_slide_to_) {
    row_from[guid] = RowSlideOffset(guid);
  }
  std::map<int, double> to;
  std::map<std::string, SlideVec> row_to;
  const Layout layout = ComputeLayout();
  const int pitch = Scaled(kTabRowHeight + kTabRowGap);

  if (drag_active_ && !layout.horizontal_tabs && drag_insert_ >= 0) {
    if (drag_kind_ == DragKind::kTab && DragIndex() >= 0 &&
        DragIndex() < static_cast<int>(tabs_.size())) {
      // The dragged row vacates its list slot in EVERY band; where the gap
      // reopens depends on the target under the cursor.
      std::vector<const TabRow*> visible;
      int dragged_pos = -1;
      for (const TabRow& row : tabs_) {
        if (row.bounds.right <= row.bounds.left) {
          continue;
        }
        if (static_cast<int>(&row - tabs_.data()) == DragIndex()) {
          dragged_pos = static_cast<int>(visible.size());
        }
        visible.push_back(&row);
      }
      if (dragged_pos >= 0) {
        for (int i = 0; i < static_cast<int>(visible.size()); ++i) {
          if (i == dragged_pos) {
            continue;
          }
          double dy = 0.0;
          if (drag_band_ == DropBand::kTabs) {
            const int to_pos = drag_insert_ > dragged_pos ? drag_insert_ - 1
                                                          : drag_insert_;
            if (i > dragged_pos && i <= to_pos) {
              dy = -pitch;
            } else if (i >= to_pos && i < dragged_pos) {
              dy = pitch;
            }
          } else {
            // Leaving the list: later rows close the vacated slot. (The
            // outline gap a bookmark conversion opens pushes the whole band
            // below it — the band shift, below.)
            if (i > dragged_pos) {
              dy = -pitch;
            }
          }
          if (dy != 0.0) {
            to[visible[static_cast<size_t>(i)]->id] = dy;
          }
        }
      }
      if (drag_band_ == DropBand::kPins && !pinned_.empty()) {
        // A phantom one-slot cell opens the grid gap at the insertion
        // slot; every displaced cell glides to its previewed position.
        std::vector<int> units;
        for (const ShortcutRow& pin : pinned_) {
          units.push_back(pin.url_b.empty() ? 1 : 2);
        }
        const int at =
            std::min(drag_insert_, static_cast<int>(units.size()));
        std::vector<int> preview = units;
        preview.insert(preview.begin() + at, 1);
        const std::vector<RECT> target = PinnedGridWalk(preview, layout);
        for (int i = 0; i < static_cast<int>(pinned_.size()); ++i) {
          const size_t j = static_cast<size_t>(i < at ? i : i + 1);
          const SlideVec v = {
              static_cast<double>(target[j].left - pinned_[i].bounds.left),
              static_cast<double>(target[j].top - pinned_[i].bounds.top)};
          if (v.dx != 0.0 || v.dy != 0.0) {
            row_to[pinned_[static_cast<size_t>(i)].guid] = v;
          }
        }
      } else if (drag_band_ == DropBand::kBookmarks) {
        for (int i = 0; i < static_cast<int>(bookmarks_.size()); ++i) {
          if (i >= drag_insert_) {
            row_to[bookmarks_[static_cast<size_t>(i)].guid] =
                SlideVec{0.0, static_cast<double>(pitch)};
          }
        }
      }
    } else if (drag_kind_ == DragKind::kPin &&
               drag_band_ != DropBand::kPins && DragIndex() >= 0 &&
               DragIndex() < static_cast<int>(pinned_.size())) {
      // The cell has left the grid (unpinning into the list, or becoming a
      // bookmark): the grid closes its slot, and over the outline the
      // bookmark rows part at the target — unless the drop goes INTO a
      // collapsed folder, which highlights instead of opening a gap.
      std::vector<int> units;
      for (int i = 0; i < static_cast<int>(pinned_.size()); ++i) {
        if (i != DragIndex()) {
          units.push_back(
              pinned_[static_cast<size_t>(i)].url_b.empty() ? 1 : 2);
        }
      }
      const std::vector<RECT> target = PinnedGridWalk(units, layout);
      for (int i = 0; i < static_cast<int>(pinned_.size()); ++i) {
        if (i == DragIndex()) {
          continue;
        }
        const size_t j = static_cast<size_t>(i > DragIndex() ? i - 1 : i);
        if (j >= target.size()) {
          continue;
        }
        const SlideVec v = {
            static_cast<double>(target[j].left - pinned_[i].bounds.left),
            static_cast<double>(target[j].top - pinned_[i].bounds.top)};
        if (v.dx != 0.0 || v.dy != 0.0) {
          row_to[pinned_[static_cast<size_t>(i)].guid] = v;
        }
      }
      if (drag_band_ == DropBand::kBookmarks &&
          bookmark_drop_folder_row_ < 0) {
        for (int i = 0; i < static_cast<int>(bookmarks_.size()); ++i) {
          if (i >= drag_insert_) {
            row_to[bookmarks_[static_cast<size_t>(i)].guid] =
                SlideVec{0.0, static_cast<double>(pitch)};
          }
        }
      }
    } else if (drag_kind_ == DragKind::kBookmark &&
               drag_band_ == DropBand::kPins && DragIndex() >= 0 &&
               DragIndex() < static_cast<int>(bookmarks_.size())) {
      // A bookmark over the grid (the Mac's moveBookmarkOut(toPinnedTabs:)):
      // a phantom cell of the width it will take opens the grid slot, and
      // the outline closes the row it is leaving.
      const ShortcutRow& dragged = bookmarks_[static_cast<size_t>(DragIndex())];
      std::vector<int> units;
      for (const ShortcutRow& pin : pinned_) {
        units.push_back(pin.url_b.empty() ? 1 : 2);
      }
      const int at = std::max(
          0, std::min(drag_insert_, static_cast<int>(units.size())));
      std::vector<int> preview = units;
      preview.insert(preview.begin() + at, dragged.url_b.empty() ? 1 : 2);
      const std::vector<RECT> target = PinnedGridWalk(preview, layout);
      for (int i = 0; i < static_cast<int>(pinned_.size()); ++i) {
        const size_t j = static_cast<size_t>(i < at ? i : i + 1);
        if (j >= target.size()) {
          continue;
        }
        const SlideVec v = {
            static_cast<double>(target[j].left - pinned_[i].bounds.left),
            static_cast<double>(target[j].top - pinned_[i].bounds.top)};
        if (v.dx != 0.0 || v.dy != 0.0) {
          row_to[pinned_[static_cast<size_t>(i)].guid] = v;
        }
      }
      for (int i = DragIndex() + 1; i < static_cast<int>(bookmarks_.size());
           ++i) {
        row_to[bookmarks_[static_cast<size_t>(i)].guid] =
            SlideVec{0.0, static_cast<double>(-pitch)};
      }
    } else if (drag_kind_ == DragKind::kPin &&
               drag_band_ == DropBand::kPins && DragIndex() >= 0 &&
               DragIndex() < static_cast<int>(pinned_.size())) {
      // Grid reorder: the dragged cell floats; a phantom of its own width
      // holds the target slot open.
      std::vector<int> units;
      for (int i = 0; i < static_cast<int>(pinned_.size()); ++i) {
        if (i == DragIndex()) {
          continue;
        }
        units.push_back(
            pinned_[static_cast<size_t>(i)].url_b.empty() ? 1 : 2);
      }
      const int dragged_units =
          pinned_[static_cast<size_t>(DragIndex())].url_b.empty() ? 1 : 2;
      int to_pos = drag_insert_ > DragIndex() ? drag_insert_ - 1
                                              : drag_insert_;
      to_pos = std::max(0, std::min(to_pos, static_cast<int>(units.size())));
      std::vector<int> preview = units;
      preview.insert(preview.begin() + to_pos, dragged_units);
      const std::vector<RECT> target = PinnedGridWalk(preview, layout);
      for (int i = 0; i < static_cast<int>(pinned_.size()); ++i) {
        if (i == DragIndex()) {
          continue;
        }
        const int j = i > DragIndex() ? i - 1 : i;
        const size_t jj = static_cast<size_t>(j >= to_pos ? j + 1 : j);
        const SlideVec v = {
            static_cast<double>(target[jj].left - pinned_[i].bounds.left),
            static_cast<double>(target[jj].top - pinned_[i].bounds.top)};
        if (v.dx != 0.0 || v.dy != 0.0) {
          row_to[pinned_[static_cast<size_t>(i)].guid] = v;
        }
      }
    } else if (drag_kind_ == DragKind::kBookmark && DragIndex() >= 0 &&
               DragIndex() < static_cast<int>(bookmarks_.size()) &&
               bookmark_drop_folder_row_ < 0) {
      // A drop INTO a collapsed folder opens no gap — the folder row
      // highlights instead and every displaced row settles home.
      const int to_pos = drag_insert_ > DragIndex() ? drag_insert_ - 1
                                                    : drag_insert_;
      for (int i = 0; i < static_cast<int>(bookmarks_.size()); ++i) {
        if (i == DragIndex()) {
          continue;
        }
        double dy = 0.0;
        if (i > DragIndex() && i <= to_pos) {
          dy = -pitch;
        } else if (i >= to_pos && i < DragIndex()) {
          dy = pitch;
        }
        if (dy != 0.0) {
          row_to[bookmarks_[static_cast<size_t>(i)].guid] =
              SlideVec{0.0, dy};
        }
      }
    }
  }
  // A gap opening in the outline pushes everything under it down by a row:
  // "+ New Tab", the group headers and the tab rows all ride the band shift.
  if (drag_active_ && !layout.horizontal_tabs && drag_insert_ >= 0 &&
      drag_band_ == DropBand::kBookmarks && bookmark_drop_folder_row_ < 0 &&
      (drag_kind_ == DragKind::kTab || drag_kind_ == DragKind::kPin)) {
    row_to[kBandShiftKey] = SlideVec{0.0, static_cast<double>(pitch)};
  }
  // Displaced elements whose displacement ended slide home; settled ones
  // drop out of the books entirely.
  for (const auto& [id, start] : from) {
    if (!to.count(id) && start != 0.0) {
      to[id] = 0.0;
    }
  }
  for (const auto& [id, target] : to) {
    if (!from.count(id)) {
      from[id] = 0.0;
    }
  }
  for (const auto& [guid, start] : row_from) {
    if (!row_to.count(guid) && (start.dx != 0.0 || start.dy != 0.0)) {
      row_to[guid] = SlideVec{};
    }
  }
  for (const auto& [guid, target] : row_to) {
    if (!row_from.count(guid)) {
      row_from[guid] = SlideVec{};
    }
  }
  drag_slide_from_ = std::move(from);
  drag_slide_to_ = std::move(to);
  row_slide_from_ = std::move(row_from);
  row_slide_to_ = std::move(row_to);
  drag_slide_start_ = ::GetTickCount64();
  if (shell_hwnd_ &&
      (!drag_slide_to_.empty() || !row_slide_to_.empty())) {
    ::SetTimer(shell_hwnd_, kDragSlideTimer, 16, nullptr);
  }
}

PhiWindowController::DropBand PhiWindowController::DropBandAt(
    POINT client_pt) {
  // Outside the shell window entirely = tear-off (the Mac's pure
  // boundary hit-test — no distance threshold).
  POINT screen = client_pt;
  ::ClientToScreen(shell_hwnd_, &screen);
  RECT window_rect = {};
  ::GetWindowRect(shell_hwnd_, &window_rect);
  if (!::PtInRect(&window_rect, screen)) {
    return DropBand::kOutside;
  }
  const Layout layout = ComputeLayout();
  // The PAGE first, and before the incognito and horizontal-layout exits
  // below: a split drop is offered in every layout and off the record too
  // (it forms no stored row, so there is nothing to keep).
  if (::PtInRect(&layout.content, client_pt)) {
    return DropBand::kContent;
  }
  if (incognito_) {
    return DropBand::kTabs;  // No pin/bookmark conversions off the record.
  }
  if (layout.horizontal_tabs) {
    return DropBand::kTabs;  // The strip has no sidebar bands.
  }
  if (::PtInRect(&layout.favorites, client_pt)) {
    return DropBand::kPins;
  }
  if (!bookmarks_.empty() && client_pt.x < layout.sidebar.right &&
      client_pt.y >= bookmarks_.front().bounds.top &&
      client_pt.y <= bookmarks_.back().bounds.bottom) {
    return DropBand::kBookmarks;
  }
  return DropBand::kTabs;
}

void PhiWindowController::UpdateDrag(POINT pt) {
  if (drag_kind_ == DragKind::kNone) {
    return;
  }
  if (!drag_active_) {
    if (std::abs(pt.x - drag_origin_.x) < ::GetSystemMetrics(SM_CXDRAG) &&
        std::abs(pt.y - drag_origin_.y) < ::GetSystemMetrics(SM_CYDRAG)) {
      return;
    }
    drag_active_ = true;
    ::SetCapture(shell_hwnd_);
    BeginDragImage(pt);  // The element lifts off into its own window.
  }
  // Tab and pin drags can cross bands (conversions, tear-off); a bookmark
  // drag stays in its own outline EXCEPT over the page, where it can form a
  // split like the other two (the Mac's bookmark DragSource), and outside
  // the window, where its live tab tears off like theirs.
  drag_point_ = pt;
  DropBand band = DropBand::kTabs;
  if (drag_kind_ == DragKind::kTab || drag_kind_ == DragKind::kPin) {
    band = DropBandAt(pt);
  } else if (drag_kind_ == DragKind::kBookmark) {
    band = DropBand::kBookmarks;
    POINT screen = pt;
    ::ClientToScreen(shell_hwnd_, &screen);
    RECT window_rect = {};
    ::GetWindowRect(shell_hwnd_, &window_rect);
    const Layout bookmark_layout = ComputeLayout();
    const bool folder =
        DragIndex() >= 0 && DragIndex() < static_cast<int>(bookmarks_.size()) &&
        bookmarks_[static_cast<size_t>(DragIndex())].folder;
    if (!::PtInRect(&window_rect, screen)) {
      band = DropBand::kOutside;
    } else if (::PtInRect(&bookmark_layout.content, pt)) {
      band = DropBand::kContent;
    } else if (!folder && !incognito_ && !bookmark_layout.horizontal_tabs &&
               ::PtInRect(&bookmark_layout.favorites, pt)) {
      band = DropBand::kPins;  // The Mac's moveBookmarkOut(toPinnedTabs:).
    }
  }
  const bool band_changed = band != drag_band_;
  drag_band_ = band;
  // The image first: crossing out of the sidebar swaps the row for the page
  // card, which captures the page BEFORE the hint cards go up over it.
  SyncDragImage(pt);
  if (band == DropBand::kContent) {
    // The page has no insertion slot: the drop is described by the split
    // zone under the cursor, and the hint cards are the whole feedback.
    ResolveSplitDrop(pt, /*rebuild=*/band_changed);
    if (drag_insert_ != -1 || band_changed) {
      drag_insert_ = -1;
      RetargetDragSlide();  // The row leaves the list while it hovers here.
      if (shell_hwnd_) {
        ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
      }
    }
    return;
  }
  PhiSplitDropOverlay::Hide();
  split_drop_valid_ = false;
  split_drop_zone_ = PhiSplitDropZone::kNone;
  // The pure resolvers describe the drop as a SidebarPlace and the gap slot
  // the animation opens; a change between INTO-a-folder and a gap repaints
  // even at the same slot.
  const int folder_before = bookmark_drop_folder_row_;
  const int slot = DragInsertSlot(pt);
  const bool folder_target_changed = folder_before != bookmark_drop_folder_row_;
  const bool animated_kind = drag_kind_ == DragKind::kTab ||
                             drag_kind_ == DragKind::kPin ||
                             drag_kind_ == DragKind::kBookmark;
  if (slot != drag_insert_ || band_changed || folder_target_changed) {
    drag_insert_ = slot;
    if (animated_kind) {
      RetargetDragSlide();  // The gap moved: siblings glide to the new slot.
    }
    if (shell_hwnd_) {
      ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    }
  }
  // A plain move repaints nothing: the dragged element rides the drag image
  // window, and the chrome only changes when the gap does.
}

void PhiWindowController::CommitDrag() {
  // Read before the drop clears them: whether the image slides home or just
  // goes depends on where the pointer was and whether anything happened.
  const bool active = drag_active_;
  const bool inside = drag_image_inside_;
  const bool acted = CommitDragImpl();
  if (active) {
    EndDragImage(acted, inside);
  }
}

bool PhiWindowController::CommitDragImpl() {
  const DragKind kind = drag_kind_;
  const DropBand band = drag_band_;
  const SidebarKey key = drag_key_;
  const int slot = drag_insert_;
  const bool active = drag_active_;
  const POINT release = drag_point_;
  const SidebarPlace place = drag_place_;
  const bool place_valid = drag_place_valid_;
  // What a drop on the page would do (CancelDrag clears these too).
  const PhiSplitDropZone split_zone = split_drop_zone_;
  const bool split_replace = split_drop_replace_;
  const std::string split_id = split_drop_split_id_;
  const int split_focus = drag_focus_tab_id_;
  const int click_tab = drag_click_tab_id_;
  const int chip_pos = DragIndex();
  CancelDrag();
  if (!active && kind == DragKind::kTab && click_tab != 0) {
    // A plain click on a tab row: the deferred activation.
    Apply(SidebarActivate{SidebarKey::Tab(click_tab), 0});
    return false;
  }
  // The page band lands before the slot paths below: a split drop has no
  // insertion slot at all, and the dead middle band is a no-op.
  if (band == DropBand::kContent) {
    if (active && split_zone != PhiSplitDropZone::kNone) {
      CommitSplitDrop(kind, key, split_focus, split_zone, split_replace,
                      split_id);
      return true;
    }
    return false;
  }
  if (!active) {
    return false;
  }
  if (band == DropBand::kOutside) {
    // Released outside the window: the live tab(s) join the shell under the
    // cursor or tear off into a new window (the Mac's live-wrapper
    // tear-off). A stored row stays behind as a closed cell.
    if (kind == DragKind::kPin || kind == DragKind::kBookmark) {
      return TearOffStoredRow(kind, key, release);
    }
    if (kind != DragKind::kTab || !framework_) {
      return false;
    }
    const int tab_a = TabOfKey(sidebar_tree_, key, 0);
    const int tab_b = TabOfKey(sidebar_tree_, key, 1);
    if (tab_a == 0) {
      return false;
    }
    // The drag image comes down first so the hit test cannot land on it.
    PhiDragImage::Hide();
    POINT screen = release;
    ::ClientToScreen(shell_hwnd_, &screen);
    const int64_t target =
        host_ ? host_->SlotAtScreenPoint(screen, slot_.value) : 0;
    if (!target || !host_ ||
        !host_->RequestDropTabOnWindow(slot_.value, tab_a, tab_b, target)) {
      // No browser attached: an empty strip, and nothing tears off.
      const int64_t browser = active_browser_id().value;
      const std::vector<StripTab> strip =
          browser ? ReadStrip(framework_, browser) : std::vector<StripTab>();
      const int index_a = StripIndexOfId(strip, tab_a);
      const int index_b = tab_b ? StripIndexOfId(strip, tab_b) : -1;
      if (index_a >= 0) {
        if (host_) {
          host_->NoteTearOffDrop(screen);
        }
        framework_->MoveTabsToNewWindow(browser, index_a, index_b);
      }
    }
    RefreshTabs();
    return true;
  }
  if (kind == DragKind::kChip) {
    if (slot < 0 || chip_pos < 0 || chip_pos >= static_cast<int>(chips_.size()) ||
        !host_) {
      return false;
    }
    // Insert-before `slot` becomes the final position by removing the
    // dragged pip first; the slot counts the WINDOWED pips, the row index
    // adds those scrolled out of the viewport ahead of it.
    const int to_pos = slot > chip_pos ? slot - 1 : slot;
    if (to_pos == chip_pos) {
      return false;
    }
    host_->ReorderSpace(chips_[static_cast<size_t>(chip_pos)].id,
                        to_pos + chips_first_index_);
    return true;
  }
  // Everything else is ONE verb: the item goes to the resolved place — a
  // reorder, a tree move, a pin, an unpin, a bookmark, a conversion between
  // the two stored sections (docs/sidebar-architecture.md §5.2).
  if (!place_valid) {
    return false;
  }
  return Apply(SidebarMove{key, place});
}

void PhiWindowController::CancelDrag() {
  const bool was_active = drag_active_;
  drag_kind_ = DragKind::kNone;
  drag_band_ = DropBand::kNone;
  drag_key_ = {};
  drag_active_ = false;
  drag_insert_ = -1;
  drag_place_valid_ = false;
  drag_folder_key_ = {};
  bookmark_drop_folder_row_ = -1;
  drag_focus_tab_id_ = 0;
  drag_click_tab_id_ = 0;
  split_drop_valid_ = false;
  split_drop_zone_ = PhiSplitDropZone::kNone;
  split_drop_replace_ = false;
  split_drop_split_id_.clear();
  PhiSplitDropOverlay::Hide();
  drag_slide_from_.clear();
  drag_slide_to_.clear();
  row_slide_from_.clear();
  row_slide_to_.clear();
  drag_slide_start_ = 0;
  if (shell_hwnd_) {
    ::KillTimer(shell_hwnd_, kDragSlideTimer);
  }
  if (was_active) {
    if (::GetCapture() == shell_hwnd_) {
      ::ReleaseCapture();
    }
    if (shell_hwnd_) {
      ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    }
  }
}

void PhiWindowController::AbortDrag() {
  const bool active = drag_active_;
  const bool inside = drag_image_inside_;
  CancelDrag();
  if (active) {
    EndDragImage(/*acted=*/false, inside);
  }
}

// --- The drag image ----------------------------------------------------------
//
// The Mac's NSDraggingSession image with TabDraggingSession's switching on
// top of it. See PhiDragImage for the window; this is the shell's side —
// what to lift, where the boundary runs, and what the page card shows.

RECT PhiWindowController::DragSourceRect() {
  const Layout layout = ComputeLayout();
  RECT r = {};
  switch (drag_kind_) {
    case DragKind::kTab:
      if (DragIndex() >= 0 && DragIndex() < static_cast<int>(tabs_.size())) {
        r = tabs_[static_cast<size_t>(DragIndex())].bounds;
        if (!layout.horizontal_tabs) {
          r.top += Scaled(kRowPad);  // The pill inside the row.
          r.bottom -= Scaled(kRowPad);
        }
      }
      break;
    case DragKind::kPin:
      if (DragIndex() >= 0 &&
          DragIndex() < static_cast<int>(pinned_.size())) {
        r = pinned_[static_cast<size_t>(DragIndex())].bounds;
      }
      break;
    case DragKind::kBookmark:
      if (DragIndex() >= 0 &&
          DragIndex() < static_cast<int>(bookmarks_.size())) {
        const ShortcutRow& row = bookmarks_[static_cast<size_t>(DragIndex())];
        r = row.bounds;
        r.top += Scaled(kRowPad);
        r.bottom -= Scaled(kRowPad);
        r.left += row.depth * Scaled(kBookmarkIndent);
      }
      break;
    case DragKind::kChip:
    case DragKind::kNone:
      break;
  }
  return r;
}

bool PhiWindowController::DragImageInside(POINT pt) {
  if (drag_kind_ == DragKind::kBookmark && DragIndex() >= 0 &&
      DragIndex() < static_cast<int>(bookmarks_.size()) &&
      bookmarks_[static_cast<size_t>(DragIndex())].folder) {
    return true;  // A folder has no page to show (shouldTrackDraggingImage).
  }
  const Layout layout = ComputeLayout();
  RECT band = layout.sidebar;
  if (layout.horizontal_tabs) {
    band = layout.title_bar;  // The strip lives in the caption band.
  } else {
    // The Mac's boundary is the vertical tab list's view: pins, bookmarks,
    // "+ New Tab" and the tabs — not the header above them.
    band.top = layout.favorites.top;
    if (layout.bottom_bar.bottom > layout.bottom_bar.top) {
      band.bottom = layout.bottom_bar.top;
    }
  }
  if (::PtInRect(&band, pt)) {
    return true;
  }
  POINT screen = pt;
  ::ClientToScreen(shell_hwnd_, &screen);
  RECT window_rect = {};
  ::GetWindowRect(shell_hwnd_, &window_rect);
  if (::PtInRect(&window_rect, screen)) {
    return false;  // The page, the header, the caption band.
  }
  // Another shell window under the cursor takes the row into its strip, so
  // the row is what shows there (isInsideAnyOtherBrowserTabDragBoundary).
  return host_ && host_->SlotAtScreenPoint(screen, slot_.value) != 0;
}

void PhiWindowController::BeginDragImage(POINT pt) {
  if (drag_kind_ == DragKind::kChip || drag_kind_ == DragKind::kNone ||
      !shell_hwnd_) {
    return;  // A chip drag paints its own pill in the strip.
  }
  const RECT source = DragSourceRect();
  if (source.right <= source.left || source.bottom <= source.top) {
    return;
  }
  // Lift the element's pixels straight out of the frame, painted at home in
  // its lifted style — the Mac's createDraggingSnapshot of the cell.
  drag_capture_ = true;
  const PhiDragBitmap row = CaptureChromeRect(source);
  drag_capture_ = false;
  if (row.empty()) {
    return;
  }
  POINT corners[2] = {{source.left, source.top}, {source.right, source.bottom}};
  ::MapWindowPoints(shell_hwnd_, nullptr, corners, 2);
  drag_home_screen_ = {corners[0].x, corners[0].y, corners[1].x, corners[1].y};
  drag_image_inside_ = true;
  drag_card_built_ = false;
  drag_card_ = PhiDragCard();
  POINT screen = pt;
  ::ClientToScreen(shell_hwnd_, &screen);
  PhiDragImage::Begin(shell_hwnd_, dpi_, row, Scaled(kCornerRadius),
                      POINT{pt.x - source.left, pt.y - source.top}, screen);
  // The element leaves the chrome: its slot is the gap from here on.
  ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
}

void PhiWindowController::SyncDragImage(POINT pt) {
  if (!drag_active_ || !shell_hwnd_ || !PhiDragImage::active()) {
    return;
  }
  POINT screen = pt;
  ::ClientToScreen(shell_hwnd_, &screen);
  const bool inside = DragImageInside(pt);
  if (inside == drag_image_inside_) {
    PhiDragImage::MoveTo(screen);
    return;
  }
  drag_image_inside_ = inside;
  if (inside) {
    PhiDragImage::ShowRow(screen);  // Back in: the row again.
    return;
  }
  if (!drag_card_built_) {
    // Composed once per drag, the first time the cursor leaves — the Mac's
    // cachedTabSnapshotImage. Out of sight first: the card may read the
    // page off the screen, and the row image is hanging over its edge.
    drag_card_built_ = true;
    PhiDragImage::Conceal();
    drag_card_ = BuildDragCard();
  }
  PhiDragImage::ShowCard(drag_card_, CurrentPalette(), screen);
}

void PhiWindowController::EndDragImage(bool acted, bool inside) {
  if (!PhiDragImage::active()) {
    return;
  }
  // AppKit's animatesToStartingPositionsOnCancelOrFail: true inside the
  // sidebar, false once the drag has left it (a tear-off must not bounce).
  if (!acted && inside) {
    PhiDragImage::SlideBack(drag_home_screen_);
  } else {
    PhiDragImage::Hide();
  }
}

PhiDragBitmap PhiWindowController::CaptureChromeRect(const RECT& client_rect) {
  PhiDragBitmap out;
  if (!shell_hwnd_) {
    return out;
  }
  RECT client = {};
  ::GetClientRect(shell_hwnd_, &client);
  RECT clipped = {};
  if (!::IntersectRect(&clipped, &client_rect, &client)) {
    return out;
  }
  const int frame_w = client.right;
  const int frame_h = client.bottom;
  if (frame_w <= 0 || frame_h <= 0) {
    return out;
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = frame_w;
  info.bmiHeader.biHeight = -frame_h;  // Top-down.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP dib =
      ::CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  ::ReleaseDC(nullptr, screen);
  if (!dib || !bits) {
    if (dib) {
      ::DeleteObject(dib);
    }
    ::DeleteDC(mem);
    return out;
  }
  HGDIOBJ old = ::SelectObject(mem, dib);
  HBRUSH fill = ::CreateSolidBrush(BackgroundFill());
  ::FillRect(mem, &client, fill);
  ::DeleteObject(fill);
  // The same frame WM_PAINT composes, into our own bits.
  PaintChrome(mem, ComputeLayout(), /*include_child_controls=*/false);
  ::GdiFlush();
  const int w = clipped.right - clipped.left;
  const int h = clipped.bottom - clipped.top;
  const size_t row_bytes = static_cast<size_t>(w) * 4;
  // SAFETY: the DIB was created frame_w x frame_h at 32bpp, top-down, so it
  // holds exactly frame_w * frame_h * 4 bytes with no stride padding.
  const auto frame = UNSAFE_BUFFERS(base::span<const unsigned char>(
      static_cast<const unsigned char*>(bits),
      static_cast<size_t>(frame_w) * static_cast<size_t>(frame_h) * 4));
  out.bgra.resize(row_bytes * static_cast<size_t>(h));
  base::span<unsigned char> dest(out.bgra);
  for (int y = 0; y < h; ++y) {
    const size_t from =
        (static_cast<size_t>(clipped.top + y) * static_cast<size_t>(frame_w) +
         static_cast<size_t>(clipped.left)) *
        4;
    dest.subspan(static_cast<size_t>(y) * row_bytes, row_bytes)
        .copy_from(frame.subspan(from, row_bytes));
  }
  out.width = w;
  out.height = h;
  ::SelectObject(mem, old);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
  return out;
}

PhiDragBitmap PhiWindowController::CaptureContentFromScreen() {
  PhiDragBitmap out;
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_) ||
      !::IsWindowVisible(browser_hwnd_) || !shell_hwnd_ ||
      ::IsIconic(shell_hwnd_)) {
    return out;
  }
  // The child paints through DirectComposition, which PrintWindow cannot
  // read (the same blindness captureWindow works around over CDP) — but
  // the screen itself can be read back, and the page is on it right now.
  RECT page = {};
  ::GetWindowRect(browser_hwnd_, &page);
  MONITORINFO monitor = {sizeof(monitor)};
  if (!::GetMonitorInfoW(
          ::MonitorFromWindow(browser_hwnd_, MONITOR_DEFAULTTONULL),
          &monitor)) {
    return out;
  }
  RECT visible = {};
  if (!::IntersectRect(&visible, &page, &monitor.rcMonitor)) {
    return out;
  }
  const int w = visible.right - visible.left;
  const int h = visible.bottom - visible.top;
  if (w < 32 || h < 32) {
    return out;
  }
  // Downscale on the way in: the card is 220 DIPs wide, and four times that
  // keeps the bicubic pass sharp without dragging a 4K frame around.
  const int limit = Scaled(PhiDragImage::kCardWidth * 4);
  const double scale =
      std::min(1.0, static_cast<double>(limit) / std::max(w, h));
  const int tw = std::max(1, static_cast<int>(w * scale));
  const int th = std::max(1, static_cast<int>(h * scale));
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = tw;
  info.bmiHeader.biHeight = -th;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP dib =
      ::CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      ::DeleteObject(dib);
    }
    ::DeleteDC(mem);
    ::ReleaseDC(nullptr, screen);
    return out;
  }
  HGDIOBJ old = ::SelectObject(mem, dib);
  ::SetStretchBltMode(mem, HALFTONE);
  ::SetBrushOrgEx(mem, 0, 0, nullptr);
  const bool copied = ::StretchBlt(mem, 0, 0, tw, th, screen, visible.left,
                                   visible.top, w, h, SRCCOPY) != FALSE;
  ::GdiFlush();
  if (copied) {
    const size_t bytes = static_cast<size_t>(tw) * static_cast<size_t>(th) * 4;
    // SAFETY: the DIB is tw x th at 32bpp, top-down — exactly `bytes`.
    const auto pixels = UNSAFE_BUFFERS(base::span<const unsigned char>(
        static_cast<const unsigned char*>(bits), bytes));
    out.bgra.assign(pixels.begin(), pixels.end());
    // Screen pixels carry no alpha; the card composes them premultiplied.
    for (size_t i = 3; i < out.bgra.size(); i += 4) {
      out.bgra[i] = 0xFF;
    }
    out.width = tw;
    out.height = th;
  }
  ::SelectObject(mem, old);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  if (PhiDragBitmapIsUniform(out)) {
    out = PhiDragBitmap();  // A page that has not painted yet is no page.
  }
  return out;
}

PhiDragCard PhiWindowController::BuildDragCard() {
  PhiDragCard card;
  const int64_t browser = active_browser_id().value;
  const std::vector<StripTab> strip = framework_ && browser
                                          ? ReadStrip(framework_, browser)
                                          : std::vector<StripTab>();
  const auto copy_favicon = [](const std::vector<unsigned char>& bgra,
                               int width, int height, PhiDragBitmap* out) {
    if (!bgra.empty() && width > 0 && height > 0) {
      out->bgra = bgra;
      out->width = width;
      out->height = height;
    }
  };
  int tab_a = 0;
  int tab_b = 0;
  switch (drag_kind_) {
    case DragKind::kTab: {
      if (DragIndex() < 0 || DragIndex() >= static_cast<int>(tabs_.size())) {
        return card;
      }
      const TabRow& row = tabs_[static_cast<size_t>(DragIndex())];
      tab_a = row.id;
      card.a.title = row.title;
      copy_favicon(row.favicon, row.favicon_width, row.favicon_height,
                   &card.a.favicon);
      if (row.b_index >= 0) {
        card.split = true;
        card.stacked = SplitIsStacked(row.id);
        tab_b = row.b_id;
        card.b.title = row.b_title;
        copy_favicon(row.b_favicon, row.b_favicon_width, row.b_favicon_height,
                     &card.b.favicon);
      }
      break;
    }
    case DragKind::kPin:
    case DragKind::kBookmark: {
      const std::vector<ShortcutRow>& rows =
          drag_kind_ == DragKind::kPin ? pinned_ : bookmarks_;
      if (DragIndex() < 0 || DragIndex() >= static_cast<int>(rows.size())) {
        return card;
      }
      const ShortcutRow& row = rows[static_cast<size_t>(DragIndex())];
      card.a.title = row.title;
      copy_favicon(row.favicon, row.favicon_width, row.favicon_height,
                   &card.a.favicon);
      // The press that armed the drag opened the row: its live tab is found
      // by tag, which the cached view row may not know yet.
      tab_a = LiveTabForStoredRow(strip, row.guid, row.open ? row.tab_id : 0);
      if (!row.url_b.empty()) {
        card.split = true;
        card.stacked = row.layout == "horizontal";
        card.b.title = row.title_b;
        copy_favicon(row.favicon_b, row.favicon_b_width, row.favicon_b_height,
                     &card.b.favicon);
        tab_b = LiveTabForStoredRow(strip, TagFor(row.guid, true),
                                    row.tab_id_b);
      }
      break;
    }
    case DragKind::kChip:
    case DragKind::kNone:
      return card;
  }
  // What is on screen beats the cache (the Mac's live capture for the
  // active tab): after the press, the dragged row's page is the one showing
  // — and a split shows both panes at once, so the capture IS the composite.
  bool showing = false;
  for (const StripTab& tab : strip) {
    if (tab.active && (tab.id == tab_a || (tab_b != 0 && tab.id == tab_b))) {
      showing = true;
      break;
    }
  }
  if (showing) {
    card.whole = CaptureContentFromScreen();
  }
  if (card.whole.empty() && framework_) {
    const auto thumbnail = [&](int tab_id, PhiDragBitmap* out) {
      if (tab_id == 0) {
        return;
      }
      int width = 0;
      int height = 0;
      const unsigned char* pixels =
          framework_->TabThumbnailBgra(browser, tab_id, &width, &height);
      if (!pixels || width <= 0 || height <= 0) {
        return;
      }
      // SAFETY: TabThumbnailBgra reports the dimensions of the buffer it
      // just returned, the same contract TabFaviconBgra carries.
      const auto copy = UNSAFE_BUFFERS(base::span<const unsigned char>(
          pixels, static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
      out->bgra.assign(copy.begin(), copy.end());
      out->width = width;
      out->height = height;
    };
    thumbnail(tab_a, &card.a.snapshot);
    if (card.split) {
      thumbnail(tab_b, &card.b.snapshot);
    }
  }
  return card;
}

bool PhiWindowController::TearOffStoredRow(DragKind kind,
                                           const SidebarKey& key,
                                           POINT release) {
  const std::vector<ShortcutRow>& rows =
      kind == DragKind::kPin ? pinned_ : bookmarks_;
  const auto found = std::find_if(rows.begin(), rows.end(),
                                  [&](const ShortcutRow& r) { return r.key == key; });
  if (!framework_ || found == rows.end()) {
    return false;
  }
  const ShortcutRow row = *found;
  if (row.folder) {
    return false;  // Nothing to open.
  }
  const int64_t browser = active_browser_id().value;
  if (!browser) {
    return false;  // No browser attached: nothing is open to tear off.
  }
  std::vector<StripTab> strip = ReadStrip(framework_, browser);
  // The press that armed the drag opened the row, so its live tab(s) are
  // found by tag — the Mac's live-wrapper path. A row that failed to open
  // has nothing to tear off.
  const int id_a = LiveTabForStoredRow(strip, row.guid,
                                       row.open ? row.tab_id : 0);
  int id_b = 0;
  if (!row.url_b.empty()) {
    id_b = LiveTabForStoredRow(strip, TagFor(row.guid, true), row.tab_id_b);
  }
  if (id_a == 0) {
    return false;
  }
  // The stored row stays behind as an unopened placeholder: the Mac clears
  // custom_value on every pane before the move, or the panes would snap
  // back into the cell in the new window.
  UnbindStoredTab(browser, id_a);
  if (id_b != 0) {
    UnbindStoredTab(browser, id_b);
  }
  // The drag image comes down first so the hit test cannot land on it.
  PhiDragImage::Hide();
  POINT screen = release;
  ::ClientToScreen(shell_hwnd_, &screen);
  const int64_t target =
      host_ ? host_->SlotAtScreenPoint(screen, slot_.value) : 0;
  if (!target || !host_ ||
      !host_->RequestDropTabOnWindow(slot_.value, id_a, id_b, target)) {
    strip = ReadStrip(framework_, browser);
    const int index_a = StripIndexOfId(strip, id_a);
    const int index_b = id_b != 0 ? StripIndexOfId(strip, id_b) : -1;
    if (index_a < 0) {
      return false;
    }
    if (host_) {
      host_->NoteTearOffDrop(screen);
    }
    framework_->MoveTabsToNewWindow(browser, index_a, index_b);
  }
  RefreshTabs();
  return true;
}

// --- Drop on the page: form or re-pane a split -----------------------------
//
// The Mac's SplitTabDropContainer, whose three drag sources (a tab, a pinned
// tab, a bookmark) are this client's kTab / kPin / kBookmark drags.

bool PhiWindowController::SplitDragSource(DragKind kind,
                                          const SidebarKey& key,
                                          int* tab_id,
                                          std::string* url,
                                          std::string* guid) const {
  *tab_id = 0;
  url->clear();
  guid->clear();
  if (key.empty()) {
    return false;
  }
  if (kind == DragKind::kTab) {
    const auto found = std::find_if(tabs_.begin(), tabs_.end(),
                                    [&](const TabRow& r) { return r.key == key; });
    if (found == tabs_.end()) {
      return false;
    }
    const TabRow& row = *found;
    // A split row is never a source — the page cannot nest a split inside a
    // split (the Mac's isSourceASplit).
    if (!row.split_id.empty() || row.b_index >= 0) {
      return false;
    }
    *tab_id = row.id;
    return row.id != 0;
  }
  if (kind != DragKind::kPin && kind != DragKind::kBookmark) {
    return false;
  }
  const std::vector<ShortcutRow>& rows =
      kind == DragKind::kPin ? pinned_ : bookmarks_;
  const auto found = std::find_if(rows.begin(), rows.end(),
                                  [&](const ShortcutRow& r) { return r.key == key; });
  if (found == rows.end()) {
    return false;
  }
  const ShortcutRow& row = *found;
  // A folder holds no page, and a STORED split cell cannot become one pane
  // of another split.
  if (row.folder || !row.url_b.empty()) {
    return false;
  }
  // Both are answered: an OPEN row can hand over its live tab, and the url is
  // what a closed one (or a self-drop) opens instead.
  *tab_id = row.open ? row.tab_id : 0;
  *url = row.url;
  *guid = row.guid;
  return *tab_id != 0 || !url->empty();
}

bool PhiWindowController::ResolveSplitDrop(POINT pt, bool rebuild) {
  const auto no_drop = [&] {
    split_drop_valid_ = false;
    split_drop_zone_ = PhiSplitDropZone::kNone;
    split_drop_replace_ = false;
    split_drop_split_id_.clear();
    PhiSplitDropOverlay::Hide();
    return false;
  };
  const RECT area = ComputeLayout().content;
  if (!::PtInRect(&area, pt)) {
    return no_drop();
  }
  if (!rebuild) {
    // Mid-hover: nothing the mode depends on can change while the button is
    // down, so only the zone under the pointer is recomputed.
    if (!split_drop_valid_) {
      return false;
    }
    split_drop_zone_ =
        split_drop_replace_
            ? PhiSplitReplaceZoneAt(area, pt, split_drop_stacked_,
                                    split_drop_ratio_)
            : PhiSplitCreateZoneAt(area, pt, split_drop_zone_);
    PhiSplitDropHints hints;
    hints.area = area;
    hints.replace = split_drop_replace_;
    hints.stacked = split_drop_stacked_;
    hints.ratio = split_drop_ratio_;
    hints.active = split_drop_zone_;
    PhiSplitDropOverlay::Show(this, hints);
    return true;
  }
  const int focused_id =
      drag_focus_tab_id_ != 0 ? drag_focus_tab_id_ : active_tab_id_;
  if (!framework_ || focused_id == 0) {
    return no_drop();
  }
  int source_id = 0;
  std::string source_url;
  std::string source_guid;
  if (!SplitDragSource(drag_kind_, drag_key_, &source_id, &source_url,
                       &source_guid)) {
    return no_drop();
  }
  const int64_t browser = active_browser_id().value;
  if (!browser) {
    return no_drop();
  }
  const std::vector<StripTab> strip = ReadStrip(framework_, browser);
  source_id = LiveTabForStoredRow(strip, source_guid, source_id);
  const StripTab* focused = StripTabById(strip, focused_id);
  if (!focused) {
    return no_drop();  // The page the drag started from has closed.
  }
  // A live pinned tab or bookmark tab that is already a pane cannot be
  // dragged into another split either.
  if (const StripTab* source = StripTabById(strip, source_id);
      source && !source->split_id.empty()) {
    return no_drop();
  }

  PhiSplitDropHints hints;
  hints.area = area;
  if (focused->split_id.empty()) {
    // CREATE: the four edge thirds, the middle a no-drop band.
    split_drop_replace_ = false;
    split_drop_split_id_.clear();
    split_drop_zone_ = PhiSplitCreateZoneAt(area, pt, split_drop_zone_);
  } else {
    // REPLACE: the page maps onto the focused split's two panes.
    const auto stored_is_pinned = [&](const std::string& tag) {
      SidebarTag parsed;
      if (!ParseTag(tag, &parsed)) {
        return false;
      }
      for (const ShortcutRow& row : pinned_) {
        if (row.guid == parsed.guid) {
          return true;
        }
      }
      return false;
    };
    for (const StripTab& tab : strip) {
      if (tab.split_id != focused->split_id) {
        continue;
      }
      // Replacing a pane with itself (or with its own sibling) is a no-op,
      // and a PINNED split persists as a guid pair the swap would strand —
      // the evicted pane would stay flagged pinned while the incoming one
      // is not. Neither offers a drop.
      if (tab.id == source_id || stored_is_pinned(tab.tag)) {
        return no_drop();
      }
    }
    bool stacked = false;
    double ratio = 0.5;
    if (std::optional<base::Value> parsed = base::JSONReader::Read(
            framework_->SplitViewsJson(browser), base::JSON_PARSE_RFC);
        parsed && parsed->is_list()) {
      for (const base::Value& v : parsed->GetList()) {
        const base::DictValue* d = v.GetIfDict();
        const std::string* id = d ? d->FindString("id") : nullptr;
        if (!id || *id != focused->split_id) {
          continue;
        }
        const std::string* layout = d->FindString("layout");
        stacked = layout && *layout == "horizontal";
        ratio = d->FindDouble("ratio").value_or(0.5);
        break;
      }
    }
    split_drop_replace_ = true;
    split_drop_split_id_ = focused->split_id;
    split_drop_stacked_ = stacked;
    split_drop_ratio_ = ratio;
    split_drop_zone_ = PhiSplitReplaceZoneAt(area, pt, stacked, ratio);
    hints.replace = true;
    hints.stacked = stacked;
    hints.ratio = ratio;
  }
  hints.active = split_drop_zone_;
  split_drop_valid_ = true;
  PhiSplitDropOverlay::Show(this, hints);
  return true;
}

std::string PhiWindowController::FormSplitOrdered(int64_t browser,
                                                  int primary_id,
                                                  int secondary_id,
                                                  const char* layout) {
  if (!framework_ || primary_id == 0 || secondary_id == 0 ||
      primary_id == secondary_id) {
    return std::string();
  }
  std::string split(framework_->CreateSplitViewForTabs(browser, primary_id,
                                                       secondary_id, layout));
  if (split.empty()) {
    return split;
  }
  // Chromium normalizes the primary pane to the LOWER-indexed tab, so the
  // pair can land the other way round from what the drop asked for. Reversing
  // only reorders the strip — no tab is re-activated, so no pane blinks.
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework_->SplitViewsJson(browser), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return split;
  }
  for (const base::Value& v : parsed->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    const std::string* id = d ? d->FindString("id") : nullptr;
    if (!id || *id != split) {
      continue;
    }
    if (d->FindInt("primaryTabId").value_or(0) != primary_id) {
      framework_->SwapSplitView(browser, split.c_str());
    }
    break;
  }
  return split;
}

void PhiWindowController::UnbindStoredTab(int64_t browser, int tab_id) {
  if (!framework_ || tab_id == 0) {
    return;
  }
  const std::vector<StripTab> strip = ReadStrip(framework_, browser);
  const StripTab* tab = StripTabById(strip, tab_id);
  if (!tab || tab->tag.empty() || tab->index < 0) {
    return;  // Already a plain tab.
  }
  // The stored row keeps its place and its url — it simply stops being
  // "open", which is the Mac's placeholder left behind by
  // demotePinnedTabLeavingPlaceholder.
  framework_->SetTabTag(browser, tab->index, "");
}

bool PhiWindowController::SplitActiveWithNormalized(int64_t browser,
                                                    int tab_id) {
  if (!framework_ || tab_id == 0) {
    return false;
  }
  // The ACTIVE tab is read from the strip, not from active_tab_id_: callers
  // routinely activate the pane they are about to split with, and the cached
  // id is only refreshed by a rebuild.
  for (const StripTab& tab : ReadStrip(framework_, browser)) {
    if (tab.active) {
      UnbindStoredTab(browser, tab.id);
      break;
    }
  }
  UnbindStoredTab(browser, tab_id);
  // Clearing a tag never moves a tab, but the index is resolved after it
  // anyway — the id is the only thing worth carrying across a mutation.
  const int index = StripIndexOfId(ReadStrip(framework_, browser), tab_id);
  return index >= 0 && framework_->SplitActiveWith(browser, index);
}

void PhiWindowController::CommitSplitDrop(DragKind kind,
                                          const SidebarKey& source,
                                          int focused_id,
                                          PhiSplitDropZone zone,
                                          bool replace,
                                          const std::string& split_id) {
  if (!framework_ || zone == PhiSplitDropZone::kNone || focused_id == 0) {
    return;
  }
  int source_id = 0;
  std::string source_url;
  std::string source_guid;
  if (!SplitDragSource(kind, source, &source_id, &source_url,
                       &source_guid)) {
    return;
  }
  const int64_t browser = active_browser_id().value;
  if (!browser) {
    return;
  }
  source_id =
      LiveTabForStoredRow(ReadStrip(framework_, browser), source_guid,
                          source_id);
  // The dragged item lands in the pane the cursor was over; the layout names
  // the divider, not the arrangement.
  const bool dropped_primary = PhiSplitZoneIsPrimary(zone);
  const char* layout = PhiSplitZoneLayout(zone);

  // A fresh tab on `url`, as a stable id (NewTab answers with an index, and
  // every later mutation moves it).
  const auto open_tab = [&](const std::string& url) {
    const int index = framework_->NewTab(browser, url.c_str());
    if (index < 0) {
      return 0;
    }
    return StripIdAtIndex(ReadStrip(framework_, browser), index);
  };

  if (replace) {
    if (split_id.empty()) {
      return;
    }
    // Which pane the drop targets, and which tab it evicts.
    const int slot = dropped_primary ? 0 : 1;
    int evicted_id = 0;
    if (std::optional<base::Value> parsed = base::JSONReader::Read(
            framework_->SplitViewsJson(browser), base::JSON_PARSE_RFC);
        parsed && parsed->is_list()) {
      for (const base::Value& v : parsed->GetList()) {
        const base::DictValue* d = v.GetIfDict();
        const std::string* id = d ? d->FindString("id") : nullptr;
        if (!id || *id != split_id) {
          continue;
        }
        evicted_id = d->FindInt(slot == 0 ? "primaryTabId" : "secondaryTabId")
                         .value_or(0);
        break;
      }
    }
    if (evicted_id == 0) {
      return;  // The split went away mid-drag.
    }
    // The evicted pane is kept as a standalone tab beside the split, unless
    // it is an empty new-tab page — closing that beats littering the strip
    // (the Mac's splitPaneReplacementKeepsEvicted).
    const std::vector<StripTab> before = ReadStrip(framework_, browser);
    const StripTab* evicted = StripTabById(before, evicted_id);
    const bool keep_evicted =
        !evicted || !PhiNewTabPage::IsNewTabUrl(evicted->url);
    int incoming_id = source_id;
    if (incoming_id != 0) {
      // A live pinned tab or bookmark tab keeps its page: drop the binding
      // and let the tab itself take the pane. Unconditional, because the
      // COMFORTABLE layout leaves bookmark-bound tabs on the strip, so even
      // a plain tab drag can be carrying a binding.
      UnbindStoredTab(browser, incoming_id);
    } else {
      if (source_url.empty()) {
        return;
      }
      incoming_id = open_tab(source_url);
    }
    if (incoming_id == 0 || incoming_id == evicted_id) {
      return;
    }
    const int index = StripIndexOfId(ReadStrip(framework_, browser),
                                     incoming_id);
    if (index >= 0) {
      framework_->SwapTabIntoSplit(browser, split_id.c_str(), slot, index,
                                   keep_evicted);
    }
    RefreshTabs();
    return;
  }

  // CREATE. Each source keeps the Mac's own rule for what happens when it
  // resolves to the focused tab itself.
  int dragged_id = 0;
  if (kind == DragKind::kTab) {
    if (source_id == focused_id) {
      // Dragged = focused: a fresh new-tab page becomes the partner and takes
      // the slot OPPOSITE the drop, since the dragged tab visually lands
      // where the cursor is.
      UnbindStoredTab(browser, focused_id);
      const int fresh = open_tab(std::string());
      if (fresh == 0) {
        return;
      }
      FormSplitOrdered(browser, dropped_primary ? focused_id : fresh,
                       dropped_primary ? fresh : focused_id, layout);
      RefreshTabs();
      return;
    }
    dragged_id = source_id;
  } else if (source_id != 0 && source_id != focused_id) {
    // A live stored row: the tab joins the split and its row stays behind as
    // an unopened one.
    dragged_id = source_id;
  } else if (kind == DragKind::kBookmark && source_id == focused_id) {
    // The bookmark's live tab IS the focused pane. Opening its url again
    // would just duplicate what the user is looking at, so the partner is a
    // fresh new-tab page in the opposite slot.
    UnbindStoredTab(browser, focused_id);
    const int fresh = open_tab(std::string());
    if (fresh == 0) {
      return;
    }
    FormSplitOrdered(browser, dropped_primary ? focused_id : fresh,
                     dropped_primary ? fresh : focused_id, layout);
    RefreshTabs();
    return;
  } else {
    // A closed row — or a pinned one whose live tab is the focused pane:
    // its url opens as the new pane and the stored row is left intact, so
    // the slot still exists for next time.
    if (source_url.empty()) {
      return;
    }
    dragged_id = open_tab(source_url);
  }
  if (dragged_id == 0 || dragged_id == focused_id) {
    return;
  }
  // NEITHER pane may carry a stored binding: a split never lives inside a
  // pinned cell or a bookmark cell, it shows as one merged row in the normal
  // tab list. Each stored row stays where it is, as an unopened placeholder
  // (the Mac's makeTabNormalOpened /
  // demotePinnedTabLeavingPlaceholder, which every one of its
  // split-formation paths runs over both panes).
  UnbindStoredTab(browser, focused_id);
  UnbindStoredTab(browser, dragged_id);
  FormSplitOrdered(browser, dropped_primary ? dragged_id : focused_id,
                   dropped_primary ? focused_id : dragged_id, layout);
  RefreshTabs();
}

int PhiWindowController::DragInsertSlot(POINT pt) {
  // Resolves WHERE the drag lands (drag_place_) with the pure resolvers and
  // answers the gap slot the slide animation opens (-1 = no gap).
  drag_place_valid_ = false;
  drag_folder_key_ = {};
  bookmark_drop_folder_row_ = -1;
  const auto take = [&](const DropResolution& r) {
    drag_place_ = r.place;
    drag_place_valid_ = true;
    drag_folder_key_ = r.folder_key;
    if (!r.folder_key.empty()) {
      for (size_t i = 0; i < bookmarks_.size(); ++i) {
        if (bookmarks_[i].key == r.folder_key) {
          bookmark_drop_folder_row_ = static_cast<int>(i);
        }
      }
    }
    return r.gap_slot;
  };
  const Layout layout = ComputeLayout();
  switch (drag_kind_) {
    case DragKind::kTab:
      switch (drag_band_) {
        case DropBand::kOutside:
          return 0;  // Tear-off: any non-negative slot commits.
        case DropBand::kPins:
          return take(ResolvePinnedDrop(PinnedDropRows(), pt.x, pt.y, {}));
        case DropBand::kBookmarks:
          return take(ResolveBookmarkDrop(BookmarkDropRows(), pt.y, {}));
        default:
          return take(ResolveTabDrop(TabDropRows(), pt.x, pt.y,
                                     layout.horizontal_tabs, drag_key_));
      }
    case DragKind::kPin:
      switch (drag_band_) {
        case DropBand::kTabs:
          // Unpin into the list: the anchor says where; the list itself
          // opens no gap for a cell.
          take(ResolveTabDrop(TabDropRows(), pt.x, pt.y,
                              layout.horizontal_tabs, {}));
          return 0;
        case DropBand::kOutside:
          return -1;  // The release tears off; there is no slot.
        case DropBand::kBookmarks:
          take(ResolveBookmarkDrop(BookmarkDropRows(), pt.y, {}));
          return -1;  // The outline gap rides bookmark_drop_folder_row_.
        default:
          return take(ResolvePinnedDrop(PinnedDropRows(), pt.x, pt.y, drag_key_));
      }
    case DragKind::kBookmark:
      switch (drag_band_) {
        case DropBand::kOutside:
          return -1;  // The release tears off; there is no outline slot.
        case DropBand::kPins:
          return take(ResolvePinnedDrop(PinnedDropRows(), pt.x, pt.y, {}));
        case DropBand::kTabs:
          take(ResolveTabDrop(TabDropRows(), pt.x, pt.y,
                              layout.horizontal_tabs, {}));
          return -1;
        default:
          return take(ResolveBookmarkDrop(BookmarkDropRows(), pt.y, drag_key_));
      }
    case DragKind::kChip: {
      int slot = 0;
      for (const SpaceChip& chip : chips_) {
        if (pt.x > (chip.bounds.left + chip.bounds.right) / 2) {
          ++slot;
        }
      }
      return slot;
    }
    case DragKind::kNone:
      break;
  }
  return -1;
}

void PhiWindowController::DrawDragIndicator(HDC dc) {
  if (!drag_active_ || drag_insert_ < 0 || !host_) {
    return;
  }
  const PhiPalette& palette = host_->PaletteForSlot(slot_.value);
  // The 2px accent insertion line, horizontal for vertical lists and
  // vertical for horizontal ones.
  RECT line{};
  const auto before_edge = [&](const RECT& r, bool horizontal_list) {
    if (horizontal_list) {
      line = {r.left - Scaled(2), r.top, r.left, r.bottom};
    } else {
      line = {r.left, r.top - Scaled(2), r.right, r.top};
    }
  };
  const auto after_edge = [&](const RECT& r, bool horizontal_list) {
    if (horizontal_list) {
      line = {r.right, r.top, r.right + Scaled(2), r.bottom};
    } else {
      line = {r.left, r.bottom, r.right, r.bottom + Scaled(2)};
    }
  };
  const auto pick = [&](const auto& rows, bool horizontal_list) {
    if (rows.empty()) {
      return false;
    }
    if (drag_insert_ < static_cast<int>(rows.size())) {
      before_edge(rows[static_cast<size_t>(drag_insert_)].bounds,
                  horizontal_list);
    } else {
      after_edge(rows.back().bounds, horizontal_list);
    }
    return true;
  };
  bool have = false;
  const Layout layout = ComputeLayout();
  switch (drag_kind_) {
    case DragKind::kTab:
      // Every vertical target animates its own gap now (Chrome's vertical
      // tabs draw no line either); only the horizontal strip keeps one.
      have = layout.horizontal_tabs && drag_band_ == DropBand::kTabs
                 ? pick(tabs_, /*horizontal_list=*/true)
                 : false;
      break;
    case DragKind::kPin:
      // The grid gap is the indicator in the sidebar; the horizontal
      // strip's pinned section keeps the line.
      have = layout.horizontal_tabs && drag_band_ == DropBand::kPins
                 ? pick(pinned_, /*horizontal_list=*/true)
                 : false;
      break;
    case DragKind::kBookmark:
      have = false;  // The row gap is the indicator.
      break;
    case DragKind::kChip:
      have = pick(chips_, /*horizontal_list=*/true);
      break;
    case DragKind::kNone:
      break;
  }
  if (!have) {
    return;
  }
  HBRUSH accent = ::CreateSolidBrush(palette.accent);
  ::FillRect(dc, &line, accent);
  ::DeleteObject(accent);
}

void PhiWindowController::OnChromeMouseMove(POINT client_pt) {
  HoverKind kind = HoverKind::kNone;
  int index = -1;
  {
    // The nav buttons' hover washes (back/forward/reload/search).
    const Layout layout = ComputeLayout();
    // The first six MUST stay in the paint table's order — PaintChrome keys
    // a button's hover wash on this index.
    // APPEND ONLY: PaintChrome keys a button's hover wash on its index here,
    // so inserting into the middle silently moves someone else's wash.
    const std::array<RECT, 14> nav = {layout.back, layout.forward,
                                      layout.reload, layout.search_tabs,
                                      add_tab_button_, layout.overflow,
                                      layout.downloads, layout.memory,
                                      layout.feedback, layout.chat,
                                      layout.extensions, layout.copy_link,
                                      layout.phi_menu, layout.reader};
    for (size_t i = 0; i < nav.size() && kind == HoverKind::kNone; ++i) {
      if (::PtInRect(&nav[i], client_pt)) {
        kind = HoverKind::kNav;
        index = static_cast<int>(i);
      }
    }
  }
  for (size_t i = 0; i < chips_.size() && kind == HoverKind::kNone; ++i) {
    if (ChipHit(chips_[i], client_pt)) {
      kind = HoverKind::kChip;
      index = static_cast<int>(i);
    }
  }
  {
    // The strip's trailing slot ("+" / "…"), and — tracked apart from the
    // hover kind, since resting on a pip is both a pip hover and a row
    // hover — the strip's ROW: inside its 10pt padding, pips and the empty
    // run to the trailing slot included. The row is the "+" button's
    // reveal region (the Mac's onHover on the whole row). Empty slot =
    // no strip in this layout (comfortable, incognito): no row to hover.
    bool row_hovered = false;
    if (add_space_button_.right > add_space_button_.left) {
      if (kind == HoverKind::kNone &&
          ::PtInRect(&add_space_button_, client_pt)) {
        kind = HoverKind::kAddSpace;
        index = 0;
      }
      RECT row = ComputeLayout().spaces_strip;
      ::InflateRect(&row, -Scaled(kSpacesStripPad), 0);
      row_hovered = ::PtInRect(&row, client_pt) != FALSE;
    }
    SetStripRowHovered(row_hovered);
  }
  for (size_t i = 0; i < pinned_.size() && kind == HoverKind::kNone; ++i) {
    if (::PtInRect(&pinned_[i].bounds, client_pt)) {
      kind = HoverKind::kPin;
      index = static_cast<int>(i);
    }
  }
  for (size_t i = 0; i < bookmarks_.size() && kind == HoverKind::kNone; ++i) {
    if (::PtInRect(&bookmarks_[i].bounds, client_pt)) {
      kind = HoverKind::kBookmark;
      index = static_cast<int>(i);
    }
  }
  for (size_t i = 0; i < group_headers_.size() && kind == HoverKind::kNone;
       ++i) {
    if (::PtInRect(&group_headers_[i].bounds, client_pt)) {
      kind = HoverKind::kGroupHeader;
      index = static_cast<int>(i);
    }
  }
  if (kind == HoverKind::kNone) {
    const RECT new_tab = ComputeLayout().new_tab_row;
    if (::PtInRect(&new_tab, client_pt)) {
      kind = HoverKind::kNewTabRow;
      index = 0;
    }
  }
  {
    // The address capsule's rim (the EDIT subclass covers the text area).
    const Layout layout = ComputeLayout();
    SetUrlFieldHover(::PtInRect(&layout.url_field, client_pt) != FALSE);
    for (size_t i = 0;
         i < field_extensions_.size() && kind == HoverKind::kNone; ++i) {
      const RECT slot = FieldExtRect(layout, i);
      if (::PtInRect(&slot, client_pt)) {
        kind = HoverKind::kFieldExt;
        index = static_cast<int>(i);
      }
    }
  }
  for (size_t i = 0; i < tabs_.size() && kind == HoverKind::kNone; ++i) {
    if (::PtInRect(&tabs_[i].bounds, client_pt)) {
      kind = HoverKind::kTab;
      index = static_cast<int>(i);
    }
  }
  if (kind != hover_kind_ || index != hover_index_) {
    hover_kind_ = kind;
    hover_index_ = index;
    UpdateHoverCard();
    UpdateTabPreview();
    // The model is unchanged — only recompose (no dirty flag).
    if (shell_hwnd_) {
      ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    }
  }
}

void PhiWindowController::SetUrlFieldHover(bool hovered) {
  if (!hovered) {
    // A leave from one side (EDIT vs shell rim) can land after the enter on
    // the other; trust the cursor, not the message order.
    POINT pt = {};
    if (::GetCursorPos(&pt) && shell_hwnd_ &&
        ::ScreenToClient(shell_hwnd_, &pt)) {
      const RECT field = ComputeLayout().url_field;
      hovered = ::PtInRect(&field, pt) != FALSE;
    }
  }
  if (url_field_hover_ == hovered) {
    return;
  }
  url_field_hover_ = hovered;
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
  if (url_field_) {
    // The EDIT repaints through WM_CTLCOLOREDIT to pick up the new wash.
    ::InvalidateRect(url_field_, nullptr, TRUE);
  }
}

void PhiWindowController::ClearHover() {
  DismissHoverCard(std::string());
  SetStripRowHovered(false);
  if (tab_preview_) {
    tab_preview_->PointerExited();
  }
  if (hover_kind_ != HoverKind::kNone) {
    hover_kind_ = HoverKind::kNone;
    hover_index_ = -1;
    if (shell_hwnd_) {
      ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    }
  }
}

void PhiWindowController::SetStripRowHovered(bool hovered) {
  if (strip_row_hovered_ == hovered) {
    return;
  }
  strip_row_hovered_ = hovered;
  const ULONGLONG now = ::GetTickCount64();
  const ULONGLONG elapsed =
      add_space_fade_start_ ? now - add_space_fade_start_ : 0;
  // A flip mid-fade reverses from where the fade stands: the curve is
  // symmetric, so backdating the clock by the leg still to run lands the
  // reversed fade exactly on the current opacity — a quick in-and-out
  // never snaps.
  add_space_fade_start_ =
      add_space_fade_start_ &&
              elapsed < static_cast<ULONGLONG>(kAddSpaceFadeMs)
          ? now - (static_cast<ULONGLONG>(kAddSpaceFadeMs) - elapsed)
          : now;
  if (shell_hwnd_) {
    ::SetTimer(shell_hwnd_, kAddSpaceFadeTimer, 16, nullptr);
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

double PhiWindowController::AddSpaceRevealProgress() const {
  double eased = 1.0;
  if (add_space_fade_start_) {
    const double linear = std::min(
        1.0, static_cast<double>(::GetTickCount64() - add_space_fade_start_) /
                 kAddSpaceFadeMs);
    eased = linear * linear * (3.0 - 2.0 * linear);
  }
  return strip_row_hovered_ ? eased : 1.0 - eased;
}

RECT PhiWindowController::CreateSpaceFormRect() {
  const Layout layout = ComputeLayout();
  return {layout.sidebar.left, layout.spaces_strip.bottom,
          layout.sidebar.right, layout.sidebar.bottom};
}

void PhiWindowController::ShowCreateSpaceForm() {
  if (!host_) {
    return;
  }
  // CreateSpacePanel.requestCreation: the vertical layouts host the form
  // inline in the docked sidebar. The horizontal layout, a collapsed
  // sidebar (the floating panel does not host it here) and windows without
  // a Spaces strip create at once instead — the Mac's standalone window has
  // no Windows counterpart yet.
  const RECT form = CreateSpaceFormRect();
  const bool can_host = !incognito_ && !agent_ && !floating_layout_ &&
                        !sidebar_collapsed_ &&
                        layout_mode_ != PhiLayoutMode::kComfortable &&
                        form.right - form.left > Scaled(120) &&
                        form.bottom - form.top > Scaled(200);
  if (!can_host) {
    host_->RequestCreateSpace(slot_.value, /*new_profile=*/false);
    RefreshTabs();
    return;
  }
  if (creating_space_) {
    if (create_space_panel_) {
      create_space_panel_->FocusField();
    }
    return;
  }
  if (!create_space_panel_) {
    create_space_panel_ = std::make_unique<PhiCreateSpacePanel>(this);
  }
  // Seeded from the Space the "+" was pressed in: its profile opens the
  // picker, its theme is what the random pick must differ from.
  std::string profile_id = "Default";
  for (const PhiSpaceView& space : host_->Spaces()) {
    if (space.id == active_space_) {
      profile_id = space.profile_id;
      break;
    }
  }
  std::string theme_id = host_->SpaceThemeId(active_space_);
  if (theme_id.empty()) {
    theme_id = host_->ThemeId();
  }
  creating_space_ = true;
  preview_until_switch_ = false;
  DismissHoverCard(std::string());
  create_space_panel_->Show(form, profile_id, theme_id, host_->Saturation());
  RefreshTabs();  // The strip reserves the placeholder pip's slot.
  create_space_panel_->FocusField();
}

void PhiWindowController::CommitCreateSpaceForm(const PhiNewSpaceSpec& spec) {
  if (!creating_space_) {
    return;
  }
  creating_space_ = false;
  creating_space_icon_text_.clear();
  if (create_space_panel_) {
    // Instant: the new Space presents over this window (the Mac's
    // `animated: false` activation after the sidebar flow).
    create_space_panel_->Hide(/*animated=*/false);
  }
  // The previewed theme stays up until the created Space presents, so the
  // chrome never flashes back to the source theme in between.
  preview_until_switch_ = true;
  const std::string id =
      host_ ? host_->RequestCreateSpaceWithOptions(slot_.value, spec)
            : std::string();
  if (id.empty()) {
    EndCreateSpacePreview();  // Creation failed: back to the source theme.
  }
  FocusActiveBrowser();
  RefreshTabs();
}

void PhiWindowController::CancelCreateSpaceForm() {
  if (!creating_space_) {
    return;
  }
  creating_space_ = false;
  creating_space_icon_text_.clear();
  if (create_space_panel_) {
    create_space_panel_->Hide(/*animated=*/true);
  }
  EndCreateSpacePreview();
  FocusActiveBrowser();
  RefreshTabs();
}

void PhiWindowController::PreviewCreateSpaceTheme(const std::string& theme_id,
                                                  int saturation) {
  if (!host_ || !creating_space_) {
    return;
  }
  preview_palette_ = PhiResolvePalette(PhiThemeById(theme_id),
                                       host_->Appearance(), saturation);
  ApplyPalette();
  if (create_space_panel_) {
    create_space_panel_->Refresh();
  }
  if (new_tab_page_ && new_tab_page_->visible()) {
    new_tab_page_->Refresh();
  }
  RefreshCornerCaps();
}

void PhiWindowController::EndCreateSpacePreview() {
  if (!preview_palette_ && !preview_until_switch_) {
    return;
  }
  preview_palette_.reset();
  preview_until_switch_ = false;
  ApplyPalette();
  if (new_tab_page_ && new_tab_page_->visible()) {
    new_tab_page_->Refresh();
  }
  RefreshCornerCaps();
}

void PhiWindowController::SetCreateSpaceIcon(const std::string& storage_value) {
  creating_space_icon_text_ =
      base::UTF8ToWide(PhiIconDisplayText(storage_value));
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

void PhiWindowController::OnSpaceFormProfileCreated(
    const std::string& profile_id) {
  if (creating_space_ && create_space_panel_) {
    create_space_panel_->OnProfileCreated(profile_id);
  }
}

// True when `tab_id`'s split stacks its panes. Asked only while a split row
// is actually hovered, so the framework round-trip stays off the paint path.
bool PhiWindowController::SplitIsStacked(int tab_id) const {
  if (!framework_ || tab_id == 0) {
    return false;
  }
  const int64_t browser = active_browser_id().value;
  if (!browser) {
    return false;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework_->SplitViewsJson(browser), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return false;
  }
  for (const base::Value& v : parsed->GetList()) {
    const base::DictValue* split = v.GetIfDict();
    if (!split) {
      continue;
    }
    if (split->FindInt("primaryTabId").value_or(-1) != tab_id &&
        split->FindInt("secondaryTabId").value_or(-1) != tab_id) {
      continue;
    }
    const std::string* layout = split->FindString("layout");
    return layout && *layout == "horizontal";
  }
  return false;
}

std::string PhiWindowController::SplitIdForTab(int tab_id) const {
  if (!framework_ || tab_id == 0) {
    return std::string();
  }
  const int64_t browser = active_browser_id().value;
  if (!browser) {
    return std::string();
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return std::string();
  }
  for (const base::Value& v : parsed->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    if (!d || d->FindInt("id").value_or(0) != tab_id) {
      continue;
    }
    if (const std::string* split = d->FindString("splitId")) {
      return *split;
    }
    return std::string();
  }
  return std::string();
}

// Re-orients a freshly recreated split to the axis its row remembers. The
// framework creates every split side-by-side, so only "horizontal" is work.
void PhiWindowController::ApplySplitLayout(int index,
                                           const std::string& layout) {
  if (!framework_ || layout.empty() || layout == "vertical" || index < 0) {
    return;
  }
  const int64_t browser = active_browser_id().value;
  if (!browser) {
    return;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return;
  }
  for (const base::Value& v : parsed->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    if (!d || d->FindInt("index").value_or(-1) != index) {
      continue;
    }
    const std::string* split = d->FindString("splitId");
    if (split && !split->empty()) {
      // A ratio outside [0,1] keeps whatever the split already has.
      framework_->UpdateSplitView(browser, split->c_str(), layout.c_str(),
                                  -1.0);
    }
    return;
  }
}

// The hovered row's page-snapshot card (the Mac's TabPreviewRegistration,
// which every tab surface installs on its cells). Eligibility is the Mac's:
// the setting on, no drag in flight, and a LIVE tab behind the row. A split
// row previews BOTH its panes in one card, the Mac's SplitTabPreview.
bool PhiWindowController::WantsBookmarkPage() const {
  return !is_incognito() && PhiBookmarkManager::IsManagerUrl(active_tab_url_);
}

// Mounts or unmounts the native bookmark page for the ACTIVE tab's URL —
// the Mac's URL-driven rule, so the tab stays an ordinary browser tab and
// navigating it away restores the Chromium view.
void PhiWindowController::SyncBookmarkManager() {
  const bool wanted = WantsBookmarkPage();
  if (!wanted) {
    if (bookmark_manager_) {
      bookmark_manager_->Hide();
    }
    return;
  }
  if (!bookmark_manager_) {
    bookmark_manager_ = std::make_unique<PhiBookmarkManager>(this);
  }
  if (bookmark_manager_->visible()) {
    bookmark_manager_->Reposition();
    bookmark_manager_->Refresh();
  } else {
    bookmark_manager_->Show();
  }
}

// The native new-tab page (phi_new_tab_page.cc) mounts by the same rule:
// the ACTIVE tab sits on a new-tab URL — or the browser has no tabs at all
// (placeholder mode), where the page's omnibox is the way back in. Agent
// windows keep Chromium's web NTP: the agent drives it over CDP and no one
// looks at a native one there.
bool PhiWindowController::WantsNativeNtp() const {
  if (agent_ || content_fullscreen_) {
    return false;
  }
  if (placeholder_browsers_.count(active_browser_id())) {
    return true;
  }
  return PhiNewTabPage::IsNewTabUrl(active_tab_url_);
}

void PhiWindowController::SyncNativeNtp() {
  if (swap_reveal_) {
    // A Space switch is mid-slide: the page arrives (or leaves) with the
    // content, when RevealSwappedContent re-runs this — the Mac reveals the
    // target Space's content, native page included, on completion.
    return;
  }
  if (!WantsNativeNtp()) {
    if (new_tab_page_ && new_tab_page_->visible()) {
      const HWND focus = ::GetFocus();
      const bool had_focus =
          focus && (focus == new_tab_page_->hwnd() ||
                    focus == new_tab_page_->field());
      new_tab_page_->Hide();
      if (had_focus) {
        FocusActiveBrowser();  // Never leave the keyboard on a hidden field.
      }
    }
    return;
  }
  if (!new_tab_page_) {
    new_tab_page_ = std::make_unique<PhiNewTabPage>(this);
  }
  if (new_tab_page_->visible()) {
    // Controls blanked by a commit come back once that navigation is no
    // longer the active tab's business: another tab is active, or the load
    // ended and this tab is still a new tab (a new-tab URL typed in, a
    // navigation that never left).
    if (new_tab_page_->controls_hidden() &&
        (active_tab_id_ != ntp_commit_tab_id_ || !nav_loading_)) {
      new_tab_page_->Reset();
      FocusNativeNtpIfIdle();
    } else {
      new_tab_page_->Refresh();  // A palette ramp repaints it with the chrome.
    }
    return;
  }
  new_tab_page_->Show();
  if (shell_hwnd_) {
    // The corner caps go back on top of the page, as they ride the browser.
    ::PostMessageW(shell_hwnd_, WM_PHI_RAISE_CAPS, 0, 0);
    // viewDidAppear focuses the omnibox — in the window the user is in; a
    // background window mounting its page must not pull the focus over.
    if (::GetForegroundWindow() == shell_hwnd_) {
      new_tab_page_->FocusField();
    }
  }
}

void PhiWindowController::FocusNativeNtpIfIdle() {
  if (!new_tab_page_ || !shell_hwnd_ ||
      ::GetForegroundWindow() != shell_hwnd_) {
    return;
  }
  const HWND focus = ::GetFocus();
  if (focus && focus != shell_hwnd_ && focus != browser_hwnd_ &&
      focus != new_tab_page_->hwnd() && focus != new_tab_page_->field()) {
    return;  // The user is in another control (the address field, a panel).
  }
  new_tab_page_->FocusField();
}

void PhiWindowController::ResetNativeNtp() {
  if (new_tab_page_ && new_tab_page_->visible()) {
    new_tab_page_->Reset();
    FocusNativeNtpIfIdle();
    return;
  }
  SyncNativeNtp();
}

void PhiWindowController::NoteTabDistillable(int tab_id, bool distillable) {
  const bool changed = distillable ? distillable_tabs_.insert(tab_id).second
                                   : distillable_tabs_.erase(tab_id) > 0;
  if (changed && shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

bool PhiWindowController::ActiveTabDistillable() const {
  for (const TabRow& row : tabs_) {
    if (row.active) {
      return distillable_tabs_.count(row.id) > 0;
    }
  }
  return false;
}

bool PhiWindowController::ActiveTabReaderOfferable() const {
  if (active_tab_id_ == 0 || !CanOfferReaderForUrl(active_tab_url_)) {
    return false;
  }
  if (ActiveTabDistillable()) {
    return true;
  }
  return host_ && (host_->IsReaderOfferable(active_tab_id_, active_tab_url_) ||
                   host_->IsReaderActive(active_tab_id_, active_tab_url_));
}

bool PhiWindowController::ActiveTabShowsReader() const {
  return host_ && active_tab_id_ != 0 &&
         host_->IsReaderActive(active_tab_id_, active_tab_url_);
}

bool PhiWindowController::ActiveTabCanUseReader() const {
  return active_tab_id_ != 0 && CanOfferReaderForUrl(active_tab_url_);
}

// static
bool PhiWindowController::CanOfferReaderForUrl(const std::string& url) {
  // The Mac's ReaderExtractionService.canOfferReader: a real document, not
  // one of Phi's own pages, and never a PDF — the reader's extraction ladder
  // is a DOM one and a PDF has no DOM to walk (the migration plan lists PDFs
  // and file:// as out of scope on purpose). This gate sits AHEAD of both
  // verdicts, so no signal can light the button on a page the reader could
  // not open anyway.
  if (url.empty() || PhiNewTabPage::IsNewTabUrl(url)) {
    return false;
  }
  for (const char* local : {"chrome://conversation", "phi://skills",
                            "chrome://skills"}) {
    if (url.rfind(local, 0) == 0) {
      return false;
    }
  }
  // The path's extension, ignoring any query or fragment.
  const size_t cut = url.find_first_of("?#");
  std::string path = cut == std::string::npos ? url : url.substr(0, cut);
  const size_t dot = path.rfind('.');
  if (dot == std::string::npos) {
    return true;
  }
  std::string ext = path.substr(dot + 1);
  for (char& c : ext) {
    c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  }
  return ext != "pdf";
}

RECT PhiWindowController::card_rect() {
  const Layout layout = ComputeLayout();
  RECT card = layout.content;
  // The top bar is the card's first band, so the card starts at ITS top —
  // the same rule the card's outline follows (see the frame in PaintChrome).
  // Comfortable is excluded there and here alike: no card, no wrapping.
  if (layout.mode != PhiLayoutMode::kComfortable &&
      layout.top_bar.right > layout.top_bar.left) {
    card.top = layout.top_bar.top;
  }
  return card;
}

void PhiWindowController::CardRoundedCorners(bool* top, bool* bottom) const {
  const bool any = !content_fullscreen_;
  if (bottom) {
    *bottom = any;
  }
  // The card's top corners are rounded wherever there IS a card — in the
  // layouts with a top bar they are the BAR's corners (painted by its own
  // RoundRect), which is why a surface covering the card must round them and
  // one covering only the page below the bar must not. Comfortable has no
  // card: its page is edge to edge and the band above it is the window's.
  if (top) {
    *top = any && layout_mode_ != PhiLayoutMode::kComfortable;
  }
}

void PhiWindowController::RaiseCornerCaps() {
  if (shell_hwnd_) {
    ::PostMessageW(shell_hwnd_, WM_PHI_RAISE_CAPS, 0, 0);
  }
}

bool PhiWindowController::ShowsTab(int tab_id) const {
  for (const TabRow& row : tabs_) {
    if (row.id == tab_id) {
      return true;
    }
  }
  return false;
}

void PhiWindowController::ToggleReaderView() {
  if (!host_ || active_tab_id_ == 0) {
    return;
  }
  host_->RequestToggleReaderView(active_tab_id_, active_tab_url_);
}

bool PhiWindowController::LeaveReaderViewForNavigation() {
  // Every surface attached to the active tab — the reader covering it, a
  // peek floating over it — closes before the page underneath changes.
  if (!host_ || active_tab_id_ == 0) {
    return false;
  }
  const bool reader = host_->IsReaderActive(active_tab_id_, active_tab_url_);
  bool peek = false;
  for (const SidebarContentNode& node : sidebar_tree_.content) {
    if (node.active && node.attached_surface != SidebarSurface::kContent) {
      peek = true;
    }
  }
  for (const SidebarCell& cell : sidebar_tree_.pins) {
    if (cell.binding.active && cell.attached_surface != SidebarSurface::kContent) {
      peek = true;
    }
  }
  for (const SidebarCell& cell : sidebar_tree_.bookmarks) {
    if (cell.binding.active && cell.attached_surface != SidebarSurface::kContent) {
      peek = true;
    }
  }
  if (!reader && !peek) {
    return false;
  }
  host_->RequestCloseSurfaces(active_tab_id_);
  return true;
}

void PhiWindowController::UpdateTabPreview() {
  if (!host_ || !host_->ShowTabPreviews() || drag_active_) {
    if (tab_preview_) {
      tab_preview_->PointerExited();
    }
    return;
  }

  PhiTabPreview::Target target;
  bool eligible = false;
  switch (hover_kind_) {
    case HoverKind::kTab:
      if (hover_index_ >= 0 && hover_index_ < static_cast<int>(tabs_.size())) {
        const TabRow& row = tabs_[static_cast<size_t>(hover_index_)];
        if (row.id != 0) {
          target.key = "tab:" + base::NumberToString(row.id);
          target.tab_id = row.id;
          target.title = row.title;
          target.url = row.url;
          target.foreground = row.active && row.b_index < 0;
          target.anchor = row.bounds;
          if (row.b_index >= 0 && row.b_id != 0) {
            // A merged split row: pane B rides along.
            target.tab_id_b = row.b_id;
            target.title_b = row.b_title;
            target.url_b = row.b_url;
            target.foreground = row.active && !row.b_active;
            target.foreground_b = row.b_active;
          }
          eligible = true;
        }
      }
      break;
    case HoverKind::kPin:
    case HoverKind::kBookmark: {
      const std::vector<ShortcutRow>& rows =
          hover_kind_ == HoverKind::kPin ? pinned_ : bookmarks_;
      if (hover_index_ >= 0 && hover_index_ < static_cast<int>(rows.size())) {
        const ShortcutRow& row = rows[static_cast<size_t>(hover_index_)];
        // Folders have no page, and an unopened row has no live tab to
        // snapshot. A BOOKMARK split carries both panes under one guid, so
        // there is no second tab id to ask for — it keeps the single card.
        if (!row.folder && row.open && row.tab_id != 0) {
          target.key = "row:" + row.guid;
          target.tab_id = row.tab_id;
          target.title = row.title;
          target.url = base::UTF8ToWide(row.url);
          target.foreground = row.active;
          target.anchor = row.bounds;
          if (!row.url_b.empty() && row.tab_id_b != 0) {
            target.tab_id_b = row.tab_id_b;
            target.title_b = row.title_b;
            target.url_b = base::UTF8ToWide(row.url_b);
          }
          eligible = true;
        }
      }
      break;
    }
    default:
      break;
  }

  if (!eligible) {
    if (tab_preview_) {
      tab_preview_->PointerExited();
    }
    return;
  }

  // TabPreviewURLPolicy: the new-tab page shows no address, and the
  // bookmark manager opts out of snapshots entirely.
  const auto apply_url_policy = [&](std::wstring* url, bool* allow) {
    const std::string utf8 = base::WideToUTF8(*url);
    if (utf8 == "chrome://newtab/" || utf8 == "chrome://new-tab-page/" ||
        utf8 == "about:blank") {
      url->clear();
    }
    if (utf8.rfind("chrome://bookmarks", 0) == 0 ||
        utf8.rfind("phi://bookmarks", 0) == 0) {
      *allow = false;
    }
  };
  apply_url_policy(&target.url, &target.allow_snapshot);
  if (target.tab_id_b != 0) {
    apply_url_policy(&target.url_b, &target.allow_snapshot);
    target.stacked = SplitIsStacked(target.tab_id);
  }

  // The card hangs off the row in SCREEN space.
  POINT top_left = {target.anchor.left, target.anchor.top};
  POINT bottom_right = {target.anchor.right, target.anchor.bottom};
  ::ClientToScreen(shell_hwnd_, &top_left);
  ::ClientToScreen(shell_hwnd_, &bottom_right);
  target.anchor = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};

  // Sidebar rows hand the card off their right edge; the pinned grid and the
  // comfortable strip drop it underneath, as the Mac call sites do.
  const bool comfortable = host_->LayoutMode() == PhiLayoutMode::kComfortable;
  target.placement =
      (hover_kind_ == HoverKind::kPin || comfortable)
          ? PhiTabPreview::Placement::kBelowAttached
          : PhiTabPreview::Placement::kRightTopAttached;

  if (!tab_preview_) {
    tab_preview_ = std::make_unique<PhiTabPreview>(this);
  }
  tab_preview_->PointerEntered(target);
}

void PhiWindowController::UpdateHoverCard() {
  std::string chip_id;
  if (hover_kind_ == HoverKind::kChip && hover_index_ >= 0 &&
      hover_index_ < static_cast<int>(chips_.size()) &&
      // Agent pips carry a taskId, not a Space id — they have no Space card.
      !chips_[static_cast<size_t>(hover_index_)].is_agent) {
    chip_id = chips_[static_cast<size_t>(hover_index_)].id;
  }
  // Click suppression means "while the cursor stays on the clicked pip":
  // genuinely moving off it re-arms the card (the Mac's hoverBegan).
  if (!card_suppressed_space_.empty() && chip_id != card_suppressed_space_) {
    card_suppressed_space_.clear();
  }
  if (chip_id.empty() || chip_id == card_suppressed_space_) {
    DismissHoverCard(chip_id);
    return;
  }
  if (chip_id == hover_card_space_) {
    return;  // Already up for this pip.
  }
  if (!hover_card_space_.empty()) {
    // Warm hand-off pip → pip: swap the card's content instantly, the
    // Mac's already-warm behavior (system tooltips do the same).
    ResolveHoverCard(chip_id);
    hover_card_space_ = chip_id;
    pending_card_space_.clear();
    return;
  }
  // Cold: show only once the cursor settles. Re-setting the same timer id
  // restarts the countdown as the cursor walks across pips.
  pending_card_space_ = chip_id;
  if (shell_hwnd_) {
    ::SetTimer(shell_hwnd_, kHoverCardTimer, kHoverCardDelayMs, nullptr);
  }
}

void PhiWindowController::PromoteHoverCard() {
  if (shell_hwnd_) {
    ::KillTimer(shell_hwnd_, kHoverCardTimer);
  }
  if (pending_card_space_.empty()) {
    return;
  }
  const std::string pending = pending_card_space_;
  pending_card_space_.clear();
  // Show only if the cursor is still on the scheduled pip — the Mac gates
  // presentation on the real cursor position for the same reason.
  if (hover_kind_ != HoverKind::kChip || hover_index_ < 0 ||
      hover_index_ >= static_cast<int>(chips_.size()) ||
      chips_[static_cast<size_t>(hover_index_)].id != pending) {
    return;
  }
  ResolveHoverCard(pending);
  hover_card_space_ = pending;
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

void PhiWindowController::DismissHoverCard(
    const std::string& suppress_space_id) {
  card_suppressed_space_ = suppress_space_id;
  if (pending_card_space_.empty() && hover_card_space_.empty()) {
    return;
  }
  pending_card_space_.clear();
  if (shell_hwnd_) {
    ::KillTimer(shell_hwnd_, kHoverCardTimer);
  }
  if (!hover_card_space_.empty()) {
    hover_card_space_.clear();
    if (shell_hwnd_) {
      ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    }
  }
}

void PhiWindowController::ResolveHoverCard(const std::string& space_id) {
  card_profile_.clear();
  card_accent_ = host_ ? host_->PaletteForSlot(slot_.value).accent
                       : RGB(0x3A, 0xA4, 0xD5);
  card_shortcut_ = 0;
  if (!host_) {
    return;
  }
  std::string profile_id;
  int position = 0;
  for (const PhiSpaceView& space : host_->Spaces()) {
    ++position;
    if (space.id == space_id) {
      profile_id = space.profile_id;
      if (position <= 9) {
        card_shortcut_ = position;  // The Spaces menu's Ctrl+1..9 binding.
      }
      break;
    }
  }
  // The pill and keycaps tint with the SPACE's own theme where it pins one
  // (the Mac card resolves the pip's theme color the same way).
  const std::string theme_id = host_->SpaceThemeId(space_id);
  if (!theme_id.empty()) {
    const bool dark = host_->PaletteForSlot(slot_.value).dark;
    card_accent_ =
        PhiResolvePalette(PhiThemeById(theme_id),
                          dark ? PhiAppearance::kDark : PhiAppearance::kLight)
            .accent;
  }
  // The bound profile's display name, from the same list the menu bar's
  // Change Profile submenu reads; the raw id stands in when unnamed.
  card_profile_ = base::UTF8ToWide(profile_id);
  if (!framework_ || profile_id.empty()) {
    return;
  }
  std::optional<base::Value> list = base::JSONReader::Read(
      framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!list || !list->is_list()) {
    return;
  }
  for (const base::Value& v : list->GetList()) {
    if (!v.is_dict()) {
      continue;
    }
    const std::string* id = v.GetDict().FindString("id");
    if (!id || *id != profile_id) {
      continue;
    }
    const std::string* name = v.GetDict().FindString("name");
    if (name && !name->empty()) {
      card_profile_ = base::UTF8ToWide(*name);
    }
    break;
  }
}

void PhiWindowController::SetAgentVisible(bool visible) {
  if (!agent_ || !shell_hwnd_ || visible == agent_visible_) {
    return;
  }
  agent_visible_ = visible;
  if (visible) {
    // Surface centred on the work area so the user can watch the agent live.
    RECT work = {};
    RECT wr = {};
    ::GetWindowRect(shell_hwnd_, &wr);
    const int w = wr.right - wr.left;
    const int h = wr.bottom - wr.top;
    int x = 120, y = 80;
    if (::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
      x = work.left + std::max<LONG>(0, (work.right - work.left - w) / 2);
      y = work.top + std::max<LONG>(0, (work.bottom - work.top - h) / 3);
    }
    ::SetWindowPos(shell_hwnd_, HWND_TOP, x, y, 0, 0,
                   SWP_NOSIZE | SWP_SHOWWINDOW);
    ::SetForegroundWindow(shell_hwnd_);
  } else {
    // Park off-screen again — still shown so its content keeps rendering for
    // the agent's CDP capture, just invisible to the user.
    ::SetWindowPos(shell_hwnd_, HWND_BOTTOM, -32000, -32000, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE);
  }
}

bool PhiWindowController::OnSidebarWheel(POINT client_pt, int wheel_delta) {
  if (!host_ || host_->LayoutMode() == PhiLayoutMode::kComfortable) {
    return false;
  }
  const Layout layout = ComputeLayout();
  if (::PtInRect(&layout.spaces_strip, client_pt)) {
    // Over the strip the wheel slides the pip viewport by whole pips, one
    // per notch (the Mac's SpacesStripWheelTracker): a notch toward the
    // user reveals later pips, the direction a vertical list would scroll.
    StepChipsViewport(wheel_delta < 0 ? 1 : -1);
    return true;
  }
  if (!::PtInRect(&layout.sidebar, client_pt) || tabs_scroll_max_ <= 0) {
    return false;
  }
  const int step = Scaled(kTabRowHeight + kTabRowGap);
  int next = scroll_offset_ - wheel_delta * step / WHEEL_DELTA;
  next = std::max(0, std::min(next, tabs_scroll_max_));
  if (next != scroll_offset_) {
    scroll_offset_ = next;
    ScheduleRefresh();
  }
  return true;
}

COLORREF PhiWindowController::BackgroundFill() const {
  return host_ ? host_->PaletteForSlot(slot_.value).window_bg
               : RGB(0xFF, 0xFF, 0xFF);
}

void PhiWindowController::RefreshTabs() {
  RebuildTabs();
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    ::UpdateWindow(shell_hwnd_);  // Paint now, not at the next idle.
  }
}

void PhiWindowController::ScheduleRefresh() {
  // Coalesced: one posted rebuild per burst of notifications, then one
  // paint. The rebuild never runs inside WM_PAINT.
  rows_dirty_ = true;
  if (shell_hwnd_) {
    if (!rebuild_posted_) {
      rebuild_posted_ =
          ::PostMessageW(shell_hwnd_, kMsgPhiRebuildRows, 0, 0) != 0;
    }
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

void PhiWindowController::AttachSpaceBrowser(const std::string& space_id,
                                             BrowserId browser,
                                             HWND browser_hwnd) {
  if (!browser_hwnd || !::IsWindow(browser_hwnd)) {
    return;
  }
  // Reparent it into this shell exactly like the primary browser, then show it
  // in place of the current Space's.
  HWND previous = browser_hwnd_;
  BeginSpaceSwap(space_id);
  browser_hwnd_ = browser_hwnd;
  AdoptBrowserAsChild();
  if (swap_start_) {
    swap_reveal_ = browser_hwnd_;
    swap_conceal_ =
        previous && previous != browser_hwnd ? previous : nullptr;
    // Parked off the client rect until the band lands (LayoutContents keeps
    // it there; hiding would lose to Chromium's own post-creation Show).
    RECT rc = {};
    ::GetClientRect(shell_hwnd_, &rc);
    ::SetWindowPos(browser_hwnd_, nullptr, rc.right, 0, 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  if (space_browsers_.find(space_id) == space_browsers_.end()) {
    space_order_.push_back(space_id);
  }
  space_browsers_[space_id] = browser;
  if (host_) {
    host_->NoteSpaceWindow(space_id, browser.value);
  }
  browser_hwnds_[browser] = browser_hwnd;
  active_space_ = space_id;
  active_browser_ = browser;
  if (preview_until_switch_) {
    EndCreateSpacePreview();
  }
  if (framework_) {
    framework_->SetWindowSpace(browser.value, space_id.c_str());
  }
  LayoutContents();
  if (!swap_start_ && previous && previous != browser_hwnd &&
      ::IsWindow(previous)) {
    // Outgoing goes away only after the incoming one is shown in place, so
    // the content rect is never uncovered.
    ::ShowWindow(previous, SW_HIDE);
  }
  RefreshTabs();
}

void PhiWindowController::FocusActiveBrowser() {
  if (new_tab_page_ && new_tab_page_->WantsFocus()) {
    // The native new-tab page owns the keyboard while it is up: its omnibox
    // IS the page (the Mac focuses it in viewDidAppear).
    new_tab_page_->FocusField();
    return;
  }
  // Showing a different browser child is not enough: as a WS_CHILD it never
  // gets WM_ACTIVATE, so without this the keyboard still goes to the browser
  // that was visible before the Space switch (input appears dead until the
  // user clicks). Give the child Win32 focus AND drive Chromium's own
  // activation across the boundary.
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetFocus(browser_hwnd_);
  }
  if (framework_ && active_browser_.valid()) {
    framework_->FocusBrowserWindow(active_browser_.value);
  }
}

bool PhiWindowController::HasSpace(const std::string& space_id) const {
  return space_browsers_.find(space_id) != space_browsers_.end();
}

namespace {

COLORREF LerpColor(COLORREF a, COLORREF b, double t) {
  const auto mix = [t](int from, int to) {
    return static_cast<int>(from + (to - from) * t + 0.5);
  };
  return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)),
             mix(GetBValue(a), GetBValue(b)));
}

PhiPalette LerpPalette(const PhiPalette& a, const PhiPalette& b, double t) {
  PhiPalette out = b;
  out.ground_top = LerpColor(a.ground_top, b.ground_top, t);
  out.ground_bottom = LerpColor(a.ground_bottom, b.ground_bottom, t);
  out.card = LerpColor(a.card, b.card, t);
  out.selected = LerpColor(a.selected, b.selected, t);
  out.hover = LerpColor(a.hover, b.hover, t);
  out.hover_strong = LerpColor(a.hover_strong, b.hover_strong, t);
  out.label = LerpColor(a.label, b.label, t);
  out.label_dim = LerpColor(a.label_dim, b.label_dim, t);
  out.label_tertiary = LerpColor(a.label_tertiary, b.label_tertiary, t);
  out.border = LerpColor(a.border, b.border, t);
  out.separator = LerpColor(a.separator, b.separator, t);
  out.window_bg = LerpColor(a.window_bg, b.window_bg, t);
  out.accent = LerpColor(a.accent, b.accent, t);
  out.dark = t < 0.5 ? a.dark : b.dark;
  return out;
}

}  // namespace

double PhiWindowController::SwapEasedProgress() const {
  if (!swap_start_) {
    return 1.0;
  }
  const double linear = std::min(
      1.0, static_cast<double>(::GetTickCount64() - swap_start_) /
               kSpaceSwapMs);
  return linear * linear * (3.0 - 2.0 * linear);
}

double PhiWindowController::RampEasedProgress() const {
  if (!ramp_start_) {
    return 1.0;
  }
  const double linear = std::min(
      1.0, static_cast<double>(::GetTickCount64() - ramp_start_) /
               kThemeRampMs);
  return linear * linear * (3.0 - 2.0 * linear);
}

PhiPalette PhiWindowController::CurrentPalette() const {
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  if (incognito_) {
    // Incognito is FIXED-DARK regardless of theme/appearance (the Mac's
    // incognito window contract).
    static const PhiPalette kIncognito =
        PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kDark);
    return kIncognito;
  }
  if (preview_palette_) {
    return *preview_palette_;  // The create form's live theme preview.
  }
  PhiPalette target = host_ ? host_->PaletteForSlot(slot_.value) : kFallback;
  const double t = RampEasedProgress();
  if (t < 1.0) {
    target = LerpPalette(swap_source_palette_, target, t);
  }
  return target;
}

void PhiWindowController::ComposeFrame(HDC mem) {
  PaintChrome(mem, ComputeLayout(), /*include_child_controls=*/false);
  DrawDragIndicator(mem);
}

void PhiWindowController::BeginSpaceSwap(
    const std::string& entering_space_id) {
  if (!host_ || host_->LayoutMode() == PhiLayoutMode::kComfortable ||
      entering_space_id == active_space_) {
    return;
  }
  CancelSpaceSwap();  // Settle any in-flight switch first.
  // Direction from strip order, the Mac's forward/backward semantics.
  int from = -1, to = -1, at = 0;
  for (const PhiSpaceView& space : host_->Spaces()) {
    if (space.id == active_space_) {
      from = at;
    }
    if (space.id == entering_space_id) {
      to = at;
    }
    ++at;
  }
  swap_forward_ = to >= from;
  swap_source_palette_ = CurrentPalette();
  // The strip's selected pill slides from the leaving pip to the entering
  // one on the same clock; capture where it starts (active_space_ is still
  // the leaving Space here). An off-strip leaving pip just skips the slide.
  swap_chip_from_ = RECT{};
  for (const SpaceChip& chip : chips_) {
    if (chip.id == active_space_) {
      swap_chip_from_ = chip.bounds;
      break;
    }
  }
  // Retain the leaving Space's laid-out rows: the slide paints them LIVE
  // (RebuildTabs refills the working vectors with the entering Space's).
  leaving_band_ = BandModel();
  SwapBandModel();
  swap_start_ = ramp_start_ = ::GetTickCount64();
  ::SetTimer(shell_hwnd_, kSpaceSwapTimer, 16, nullptr);
}

void PhiWindowController::RevealSwappedContent() {
  if (swap_reveal_ && ::IsWindow(swap_reveal_)) {
    const Layout layout = ComputeLayout();
    ::SetWindowPos(swap_reveal_, nullptr, layout.content.left,
                   layout.content.top,
                   layout.content.right - layout.content.left,
                   layout.content.bottom - layout.content.top,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  if (swap_conceal_ && ::IsWindow(swap_conceal_) &&
      swap_conceal_ != browser_hwnd_) {
    ::ShowWindow(swap_conceal_, SW_HIDE);
  }
  const bool had_reveal = swap_reveal_ != nullptr;
  swap_reveal_ = nullptr;
  swap_conceal_ = nullptr;
  if (had_reveal) {
    SyncNativeNtp();  // Deferred past the slide, like the content itself.
    FocusActiveBrowser();
  }
}

void PhiWindowController::CancelSpaceSwap() {
  RevealSwappedContent();  // A superseded/aborted switch settles instantly.
  leaving_band_ = BandModel();
  swap_chip_from_ = RECT{};
  if ((swap_start_ || ramp_start_) && shell_hwnd_) {
    ::KillTimer(shell_hwnd_, kSpaceSwapTimer);
  }
  swap_start_ = 0;
  ramp_start_ = 0;
  RepaintUrlField();  // The palette jumped to its target.
}

void PhiWindowController::RepaintUrlField() {
  if (url_field_ && ::IsWindow(url_field_)) {
    // Erase so WM_CTLCOLOREDIT hands the EDIT the current palette's brush.
    ::InvalidateRect(url_field_, nullptr, TRUE);
  }
}

bool PhiWindowController::ShowSpace(const std::string& space_id) {
  auto it = space_browsers_.find(space_id);
  if (it == space_browsers_.end()) {
    return false;
  }
  auto hwnd_it = browser_hwnds_.find(it->second);
  if (hwnd_it == browser_hwnds_.end() || !::IsWindow(hwnd_it->second)) {
    return false;
  }
  HWND previous = (browser_hwnd_ && browser_hwnd_ != hwnd_it->second &&
                   ::IsWindow(browser_hwnd_))
                      ? browser_hwnd_
                      : nullptr;
  SetStatusUrl(std::wstring());  // The leaving page's link target is stale.
  CloseOmniboxPopup(/*stop_query=*/true);  // So is a half-typed query.
  BeginSpaceSwap(space_id);
  browser_hwnd_ = hwnd_it->second;
  active_space_ = space_id;
  active_browser_ = it->second;
  if (preview_until_switch_) {
    EndCreateSpacePreview();
  }
  if (swap_start_) {
    // Deferred: the entering page is positioned hidden and revealed when
    // the band slide completes; the leaving page stays up until then.
    swap_reveal_ = browser_hwnd_;
    swap_conceal_ = previous;
    LayoutContents();
  } else {
    LayoutContents();  // Shows + positions the incoming child...
    // ...and only then drop the outgoing one (never uncover the rect).
    if (previous) {
      ::ShowWindow(previous, SW_HIDE);
    }
    FocusActiveBrowser();
  }
  RefreshTabs();
  RepaintUrlField();  // Comfortable switches with no ramp: retint now.
  if (host_ && !agent_) {
    host_->NoteSlotActiveSpace(space_id);
  }
  return true;
}

void PhiWindowController::CloseSpaceBrowser(const std::string& space_id) {
  auto it = space_browsers_.find(space_id);
  if (it == space_browsers_.end()) {
    return;
  }
  if (PhiModalDepth() > 0) {
    // Closing the slot's last browser destroys `this`; never from under a
    // nested loop of our own (the RequestClose invariant).
    deferred_space_closes_.push_back(space_id);
    PostDeferredClose();
    return;
  }
  if (active_space_ == space_id) {
    for (const std::string& other : space_order_) {
      if (other != space_id && HasSpace(other)) {
        ShowSpace(other);
        break;
      }
    }
  }
  if (framework_) {
    // Chromium's close path; OnBrowserWindowClosing then reaches
    // ForgetBrowser for the bookkeeping. Closing the slot's last browser
    // tears the whole slot down — intended for a window that only ever
    // showed the deleted Space.
    framework_->CloseBrowserWindow(it->second.value);
  }
}

bool PhiWindowController::ForgetBrowser(BrowserId browser) {
  browser_hwnds_.erase(browser);
  for (auto it = space_browsers_.begin(); it != space_browsers_.end();) {
    if (it->second == browser) {
      std::erase(space_order_, it->first);
      it = space_browsers_.erase(it);
    } else {
      ++it;
    }
  }
  if (active_browser_ == browser) {
    active_browser_ = BrowserId();
    browser_hwnd_ = nullptr;
    // Fall back to any remaining Space so the slot stays usable.
    if (!space_order_.empty()) {
      ShowSpace(space_order_.front());
    }
  }
  return !space_browsers_.empty();
}

void PhiWindowController::RequestClose() {
  // A slot holds ONE BROWSER PER SPACE VISITED, not one per window: closing
  // only the slot's original browser (the one its window id names) leaves the
  // other Spaces' browsers alive, the slot survives ForgetBrowser, and the
  // window refuses to close after a Space switch.
  //
  // Snapshot before closing: each close can fire OnBrowserWindowClosing
  // synchronously, which mutates space_browsers_ — and destroys `this` when
  // the last browser goes. Everything below is local for that reason.
  if (PhiModalDepth() > 0) {
    // A context menu, prompt or picker of this window is pumping messages
    // (a pipe verb or a WM_CLOSE reached us through its loop): destroying
    // the controller now would return into a freed frame. Park it.
    deferred_slot_close_ = true;
    PostDeferredClose();
    return;
  }
  phi_framework::PhiFramework* framework = framework_;
  HWND shell = shell_hwnd_;
  std::vector<BrowserId> browsers;
  for (const auto& [space_id, browser_id] : space_browsers_) {
    browsers.push_back(browser_id);
  }
  if (!framework || browsers.empty()) {
    if (shell && ::IsWindow(shell)) {
      ::DestroyWindow(shell);
    }
    return;
  }
  for (BrowserId browser_id : browsers) {
    framework->CloseBrowserWindow(browser_id.value);
  }
}

void PhiWindowController::PostDeferredClose() {
  if (shell_hwnd_ && ::IsWindow(shell_hwnd_)) {
    ::PostMessageW(shell_hwnd_, kMsgPhiDeferredClose, 0, 0);
  }
}

void PhiWindowController::RunDeferredClose() {
  if (PhiModalDepth() > 0) {
    // Still inside the loop (a posted message is dispatched by it, and so
    // is this timer): try again once it has had a chance to unwind.
    ::SetTimer(shell_hwnd_, kDeferredCloseTimer, kDeferredCloseRetryMs,
               nullptr);
    return;
  }
  // ONE parked close per turn: any of them can destroy `this`, so the next
  // is re-posted BEFORE it runs — to a window that is then simply gone.
  if (!deferred_space_closes_.empty()) {
    const std::string space_id = deferred_space_closes_.front();
    deferred_space_closes_.erase(deferred_space_closes_.begin());
    if (!deferred_space_closes_.empty() || deferred_slot_close_) {
      PostDeferredClose();
    }
    CloseSpaceBrowser(space_id);
    return;
  }
  if (deferred_slot_close_) {
    deferred_slot_close_ = false;
    RequestClose();  // May destroy `this`; nothing follows.
  }
}

bool PhiWindowController::GetBounds(int* x, int* y, int* w, int* h) const {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return false;
  }
  RECT r = {};
  if (!::GetWindowRect(shell_hwnd_, &r)) {
    return false;
  }
  *x = r.left;
  *y = r.top;
  *w = r.right - r.left;
  *h = r.bottom - r.top;
  return true;
}

bool PhiWindowController::SetBounds(int x, int y, int w, int h) {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return false;
  }
  return ::SetWindowPos(shell_hwnd_, nullptr, x, y, w, h,
                        SWP_NOZORDER | SWP_NOACTIVATE) != 0;
}

void PhiWindowController::RebuildNavState() {
  nav_url_.clear();
  can_go_back_ = can_go_forward_ = nav_loading_ = false;
  const BrowserId browser = active_browser_id();
  if (!framework_ || !browser.valid()) {
    return;
  }
  std::optional<base::DictValue> st = base::JSONReader::ReadDict(
      framework_->NavStateJson(browser.value), base::JSON_PARSE_RFC);
  if (!st) {
    return;
  }
  if (const std::string* u = st->FindString("url")) {
    nav_url_ = base::UTF8ToWide(*u);
  }
  // The NTP shows the PLACEHOLDER, not its chrome:// url (the Mac's
  // updateDisplayedURL isNTP blank).
  if (nav_url_.rfind(L"chrome://newtab", 0) == 0) {
    nav_url_.clear();
  }
  can_go_back_ = st->FindBool("canGoBack").value_or(false);
  can_go_forward_ = st->FindBool("canGoForward").value_or(false);
  nav_loading_ = st->FindBool("loading").value_or(false);
  SyncUrlField(/*force=*/false);
}

void PhiWindowController::RefreshFieldExtensions() {
  std::vector<FieldExtension> next;
  const BrowserId browser = active_browser_id();
  if (framework_ && browser.valid()) {
    std::optional<base::Value> parsed = base::JSONReader::Read(
        framework_->ListExtensionsJson(browser.value), base::JSON_PARSE_RFC);
    if (parsed && parsed->is_list()) {
      // The pinned set in toolbar order (pinnedIndex), the Mac's presented
      // pinned order.
      std::vector<std::pair<int, std::string>> pinned;
      for (const base::Value& value : parsed->GetList()) {
        const base::DictValue* dict = value.GetIfDict();
        if (!dict || !dict->FindBool("pinned").value_or(false)) {
          continue;
        }
        if (const std::string* id = dict->FindString("id")) {
          pinned.emplace_back(dict->FindInt("pinnedIndex").value_or(0), *id);
        }
      }
      std::sort(pinned.begin(), pinned.end());
      for (const auto& [index, id] : pinned) {
        FieldExtension ext;
        ext.id = id;
        int w = 0;
        int h = 0;
        const unsigned char* bgra =
            framework_->ExtensionIconBgra(id.c_str(), &w, &h);
        if (bgra && w > 0 && h > 0) {
          const auto source = UNSAFE_BUFFERS(base::span<const unsigned char>(
              bgra, static_cast<size_t>(w) * h * 4));
          ext.icon.assign(source.begin(), source.end());
          ext.icon_w = w;
          ext.icon_h = h;
        }
        next.push_back(std::move(ext));
      }
    }
  }
  // Repaint only on a REAL change: this runs from paint-triggered rebuilds,
  // where an unconditional invalidate would loop.
  const bool changed =
      next.size() != field_extensions_.size() ||
      !std::equal(next.begin(), next.end(), field_extensions_.begin(),
                  [](const FieldExtension& a, const FieldExtension& b) {
                    return a.id == b.id && a.icon_w == b.icon_w &&
                           a.icon_h == b.icon_h;
                  });
  field_extensions_ = std::move(next);
  field_ext_count_ = static_cast<int>(field_extensions_.size());
  if (changed && shell_hwnd_) {
    PositionUrlField(ComputeLayout());  // The EDIT yields the strip's width.
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

RECT PhiWindowController::FieldExtRect(const Layout& layout, size_t i) const {
  if (layout.field_ext_strip.right <= layout.field_ext_strip.left) {
    return RECT{};
  }
  const int btn = Scaled(kNavButton);
  const LONG left = layout.field_ext_strip.left +
                    static_cast<LONG>(i) * (btn + Scaled(2));
  return {left, layout.field_ext_strip.top, left + btn,
          layout.field_ext_strip.bottom};
}

void PhiWindowController::RebuildChips() {
  chips_.clear();
  chips_overflow_ = false;
  chips_clip_ = RECT{};
  chips_first_index_ = 0;
  creating_pip_bounds_ = RECT{};
  const Layout layout = ComputeLayout();
  add_space_button_ = layout.add_space;
  if (!host_ || incognito_) {
    add_space_button_ = RECT{};  // Incognito has no Spaces strip.
    return;
  }
  // The full pip row in strip order (the Mac's stripOrderedSpaces): the
  // user Spaces, then — only in a real user window; the agent's own hidden
  // window never lists them — the live agent Spaces as robot pips. Clicking
  // one surfaces its window so the user can watch the agent work live.
  std::vector<SpaceChip> row;
  for (const PhiSpaceView& space : host_->Spaces()) {
    // Comfortable shows a single compact pip — the ACTIVE Space; the rest
    // live in the popup it opens.
    if (layout.horizontal_tabs && space.id != active_space_) {
      continue;
    }
    SpaceChip chip;
    chip.id = space.id;
    chip.label = !space.name.empty()
                     ? base::UTF8ToWide(space.name)
                     : L"Space " + base::UTF8ToWide(space.id);
    chip.icon = base::UTF8ToWide(space.icon_text);
    chip.open = HasSpace(space.id);
    row.push_back(std::move(chip));
  }
  if (!agent_ && !layout.horizontal_tabs) {
    for (const PhiAgentPipView& pip : host_->AgentSpaces()) {
      SpaceChip chip;
      chip.id = pip.task_id;
      chip.is_agent = true;
      chip.open = pip.watched;
      chip.label = pip.caption.empty()
                       ? L"Agent"
                       : base::UTF8ToWide(pip.caption);
      chip.icon = L"\U0001F916";  // 🤖
      row.push_back(std::move(chip));
    }
  }
  const int item = Scaled(layout.chip_size);
  const int gap = Scaled(kSpaceChipGap);
  const auto width = [&](int pips) {
    return pips <= 0 ? 0 : pips * item + (pips - 1) * gap;
  };
  const int total = static_cast<int>(row.size());
  const bool reserve_placeholder = creating_space_ && !layout.horizontal_tabs;
  // visiblePipCount: the room left of the trailing slot (`chips_limit`
  // already keeps the gap before it). While the create form is up the row's
  // end holds the dashed placeholder pip (the Mac's
  // creatingSpacePlaceholderPip): reserve its slot so the real pips can
  // never collide with it.
  int budget = layout.chips_limit - layout.chips_left;
  if (reserve_placeholder) {
    budget -= item + gap;
  }
  // 65% of a pip (plus its gap) of a hidden neighbour shows past an edge
  // that hides pips — enough of the icon to read; the peeked pip is laid
  // out whole and clipped.
  const int peek = item * 13 / 20 + gap;
  // Whole pips that fit beside ONE peek (the window at either end of the
  // row) and beside both (anywhere between). The Mac reserves both at every
  // position so its animated slide never reflows; this slide is instant, so
  // the window keeps only the peeks it shows and the pips fill the room up
  // to the "…" instead of leaving a blank slot before it.
  int count_edge = total;
  int count_mid = total;
  if (total > 0 && width(total) > budget) {
    chips_overflow_ = true;
    count_edge = total - 1;
    while (count_edge > 1 && width(count_edge) + peek > budget) {
      --count_edge;
    }
    count_mid = count_edge;
    while (count_mid > 1 && width(count_mid) + 2 * peek > budget) {
      --count_mid;
    }
  }
  const int max_start = std::max(0, total - count_edge);
  if (reserve_placeholder) {
    // slideViewportToEnd: the placeholder stands after the last pip, so an
    // overflowing row shows its tail for the placeholder to read as
    // continuing it.
    chips_start_ = max_start;
  } else if (chips_anchor_space_ != active_space_ ||
             chips_anchor_visible_ != count_mid) {
    // ensureActivePipVisible: centre the active pip in the window, clamped
    // at the row's ends so the first/last few pips still fill it. Wheel
    // steps between switches are a manual peek and are left alone.
    chips_anchor_space_ = active_space_;
    chips_anchor_visible_ = count_mid;
    int index = 0;
    for (int i = 0; i < total; ++i) {
      if (!row[static_cast<size_t>(i)].is_agent &&
          row[static_cast<size_t>(i)].id == active_space_) {
        index = i;
        break;
      }
    }
    chips_start_ = std::max(0, std::min(index - count_mid / 2, max_start));
  }
  chips_start_ = std::max(0, std::min(chips_start_, max_start));
  const bool at_edge = chips_start_ == 0 || chips_start_ == max_start;
  const int visible =
      std::min(at_edge ? count_edge : count_mid, total - chips_start_);
  const int leading_peek = chips_start_ > 0 ? peek : 0;
  const int trailing_peek = chips_start_ + visible < total ? peek : 0;
  const int first = chips_start_ - (leading_peek ? 1 : 0);
  const int end = std::min(total, chips_start_ + visible + (trailing_peek ? 1 : 0));
  chips_first_index_ = first;
  const int origin =
      layout.chips_left + leading_peek - chips_start_ * (item + gap);
  for (int i = first; i < end; ++i) {
    SpaceChip chip = std::move(row[static_cast<size_t>(i)]);
    const int x = origin + i * (item + gap);
    chip.bounds = {x, layout.chips_top, x + item, layout.chips_top + item};
    chips_.push_back(std::move(chip));
  }
  const int viewport_right =
      layout.chips_left + leading_peek + width(visible) + trailing_peek;
  if (leading_peek || trailing_peek) {
    chips_clip_ = {layout.chips_left, layout.chips_top, viewport_right,
                   layout.chips_top + item};
  }
  if (reserve_placeholder) {
    const int x = total > 0 ? viewport_right + gap : layout.chips_left;
    // The same CELL a real pip gets; the dashed ring is painted inside it at
    // the chip size, exactly as a pip's card is.
    creating_pip_bounds_ = {x, layout.chips_top, x + item,
                            layout.chips_top + item};
  }
}

bool PhiWindowController::ChipHit(const SpaceChip& chip,
                                  POINT client_pt) const {
  // A peeked pip is hit only on the part that shows (the Mac's
  // peekHitTarget): its full rect would leak past the clip into the padding.
  if (!::PtInRect(&chip.bounds, client_pt)) {
    return false;
  }
  return chips_clip_.right <= chips_clip_.left ||
         ::PtInRect(&chips_clip_, client_pt);
}

void PhiWindowController::StepChipsViewport(int step) {
  // Purely a manual peek at off-screen pips, clamped at the row's ends: the
  // next Space switch re-anchors the window on the active pip. Ignored
  // mid-drag, where sliding the row under the drop targets would scramble
  // the reorder, and while the create form pins the window to the end.
  if (step == 0 || !chips_overflow_ || drag_active_ || creating_space_) {
    return;
  }
  // RebuildChips clamps the window to the row's true end; only a step that
  // can never move it is refused here, so the repaint is not wasted.
  if (step < 0 && chips_start_ == 0) {
    return;
  }
  chips_start_ = std::max(0, chips_start_ + step);
  // The pips move under a RESTING pointer: re-hover against the new row now,
  // not at the next paint, so the wash and the hover card follow the pip
  // that is under the cursor rather than the index that used to be.
  RebuildChips();
  // A one-pip step often leaves the SAME chips_ index under the cursor with
  // a different pip in it; forget the old hover so the re-hover registers
  // as a change and hands the card over.
  hover_kind_ = HoverKind::kNone;
  hover_index_ = -1;
  POINT cursor = {};
  if (shell_hwnd_ && ::GetCursorPos(&cursor)) {
    ::ScreenToClient(shell_hwnd_, &cursor);
    OnChromeMouseMove(cursor);
  }
  ScheduleRefresh();
}

namespace {

// The Mac's TabDiscardedFaviconOutline: a 1px dashed ring around the favicon
// of a tab whose renderer is gone — restored-but-never-loaded (unloaded) or
// discarded. The Mac aligned both states on this ring (dda300c6) after a
// short-lived dimmed-favicon treatment; the dimmed TITLE stays, as before.
void DrawDormantRing(HDC dc, const RECT& icon, COLORREF color) {
  // 2px of air around a 16px icon; scales with the icon at higher DPI. The
  // dashes scale the same way (2 on, 2 off at 16px).
  const int inset = std::max(2, static_cast<int>(icon.right - icon.left) / 8);
  PhiStrokeEllipseDashedAA(dc,
                           {icon.left - inset, icon.top - inset,
                            icon.right + inset, icon.bottom + inset},
                           color, 1, inset, inset);
}

}  // namespace

void PhiWindowController::DrawFavicon(HDC dc,
                                     const RECT& dest,
                                     const unsigned char* bgra,
                                     int width,
                                     int height,
                                     COLORREF background) {
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

PinnedGridMetrics PhiWindowController::GridMetrics(
    const Layout& layout) const {
  PinnedGridMetrics m;
  m.sidebar_dip = sidebar_dip_;
  m.edge_inset_dip = kEdgeInset;
  m.gap_dip = kPinnedGap;
  m.min_cell_width_dip = kPinnedMinWidth;
  m.origin_x = layout.favorites.left + Scaled(kEdgeInset);
  m.origin_y = layout.favorites.top;
  m.sidebar_px = Scaled(sidebar_dip_);
  m.edge_inset_px = Scaled(kEdgeInset);
  m.gap_px = Scaled(kPinnedGap);
  m.cell_height_px = Scaled(kPinnedCellHeight);
  return m;
}

std::vector<RECT> PhiWindowController::PinnedGridWalk(
    const std::vector<int>& units, const Layout& layout) {
  // The pinned grid's placement (LayoutPinnedGrid), shared with the drag
  // previews so a phantom cell lands where the real one will.
  std::vector<RECT> rects;
  for (const SidebarRect& cell : LayoutPinnedGrid(units, GridMetrics(layout)).cells) {
    rects.push_back(ToRect(cell));
  }
  return rects;
}

std::vector<const SidebarCell*> PhiWindowController::VisibleBookmarkCells()
    const {
  // The outline: a collapsed folder hides its whole subtree. The tree is
  // depth-first, so a collapsed folder's descendants are the cells that
  // follow it while their depth stays greater than its own.
  std::vector<const SidebarCell*> visible;
  int collapsed_at = -1;
  for (const SidebarCell& cell : sidebar_tree_.bookmarks) {
    if (collapsed_at >= 0 && cell.depth > collapsed_at) {
      continue;
    }
    collapsed_at = -1;
    visible.push_back(&cell);
    if (cell.kind == SidebarStoredKind::kFolder &&
        !expanded_bookmarks_.count(cell.guid)) {
      collapsed_at = cell.depth;
    }
  }
  return visible;
}

void PhiWindowController::RebuildShortcuts(const Layout& layout) {
  pinned_.clear();
  bookmarks_.clear();
  if (!host_ || incognito_) {
    return;  // Nothing persists from an incognito window.
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
  // store view only supplies what it looks like.
  const auto bind = [](const SidebarCell& cell, ShortcutRow* row) {
    row->open = cell.binding.open();
    row->active = cell.binding.active;
    row->tab_id = cell.binding.tab_a;
    row->tab_id_b = cell.binding.tab_b;
    row->attached_surface = cell.attached_surface;
  };
  std::map<std::string, PhiStoredRowView> pin_views;
  for (PhiStoredRowView& view : host_->PinnedRows(slot_.value)) {
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
           host_->BookmarkRows(slot_.value, cell->parent_guid)) {
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
  const int64_t icon_browser = active_browser_id().value;
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
      pin.bounds = ToRect(grid.cells[i]);
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
    m.right = Scaled(sidebar_dip_ - kEdgeInset);
    m.top = layout.favorites.bottom +
            (pinned_row_count_ > 0 ? Scaled(kFavoritesSpacer) : 0);
    m.row_height = Scaled(kTabRowHeight);
    m.row_gap = Scaled(kTabRowGap);
    const std::vector<ListItem> items(bookmarks_.size(), ListItem{});
    const ListPlacement placed = LayoutList(items, m);
    for (size_t i = 0; i < bookmarks_.size(); ++i) {
      ShortcutRow& row = bookmarks_[i];
      row.bounds = ToRect(placed.rects[i]);
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

void PhiWindowController::RebuildTabs() {
  RebuildNavState();
  RefreshFieldExtensions();
  RebuildChips();
  tabs_.clear();
  group_headers_.clear();
  rows_dirty_ = false;
  // Re-captured from the active row below; empty when the strip is
  // (placeholder mode), so no stale URL keeps a native page mounted.
  active_tab_url_.clear();
  active_tab_id_ = 0;
  if (!framework_) {
    return;
  }
  const int64_t browser = active_browser_id().value;
  if (!browser) {
    return;  // Between the last browser going and teardown: no rows.
  }
  // ONE strip read per rebuild; every row below derives from it and the tree.
  const std::vector<StripTab> strip = ReadStrip(framework_, browser);

  // Group visuals for the header rows (title/color/collapsed by id).
  std::map<std::string, GroupHeader> group_info;
  std::map<std::string, std::wstring> group_color_labels;
  if (std::optional<base::Value> gparsed = base::JSONReader::Read(
          framework_->TabGroupsJson(browser), base::JSON_PARSE_RFC);
      gparsed && gparsed->is_list()) {
    for (const base::Value& v : gparsed->GetList()) {
      const base::DictValue* d = v.GetIfDict();
      if (!d) {
        continue;
      }
      GroupHeader info;
      if (const std::string* id = d->FindString("id")) {
        info.id = *id;
      }
      const std::string* color = d->FindString("color");
      const PhiGroupColor& palette_entry =
          GroupColorByName(color ? *color : "grey");
      info.color = palette_entry.color;
      const std::string* title = d->FindString("title");
      info.name = title ? base::UTF8ToWide(*title) : std::wstring();
      // An unnamed group is titled by its colour and member count
      // ("Grey · 2 tabs", displayTitle(memberCount:)); the count is filled
      // in once the strip has been walked.
      info.title = info.name;
      group_color_labels[info.id] =
          PhiStr((std::string("tabs.tabGroup.colorName.") +
                  palette_entry.name).c_str());
      info.collapsed = d->FindBool("collapsed").value_or(false);
      group_info[info.id] = std::move(info);
    }
  }

  // The model: what every live tab IS — plain, pin-bound, bookmark-bound,
  // split pane, surface, grouped (docs/sidebar-architecture.md §4.5). A
  // reading surface still in the strip is never a row; an active one hands
  // the selection, and the address bar, to the tab it is covering.
  {
    SidebarInputs in;
    if (host_) {
      in.stored = host_->SidebarStoredItems(slot_.value);
      in.surfaces = host_->AttachedSurfaces(browser);
      in.policy.horizontal_layout =
          host_->LayoutMode() == PhiLayoutMode::kComfortable;
    }
    in.policy.incognito = incognito_;
    for (const StripTab& tab : strip) {
      in.strip.push_back({tab.id, tab.index, tab.tag, tab.split_id,
                          tab.group_id, tab.active, tab.surface,
                          tab.origin_id});
    }
    for (const auto& [id, info] : group_info) {
      in.groups.push_back({id, info.collapsed});
    }
    sidebar_tree_ = BuildSidebar(in);
  }
  active_tab_id_ = sidebar_tree_.active_tab;
  if (const StripTab* active = StripTabById(strip, active_tab_id_)) {
    // The bookmark bar's "show on new tab" and the native new-tab page
    // read this.
    active_tab_url_ = active->url;
  }

  // The two stored bands' sizes feed the layout, and they come from the
  // tree — the pinned grid's row count and the outline's visible rows — so
  // ComputeLayout runs ONCE here, before any row is placed. (It used to be
  // called from inside the shortcut rebuild it depended on.)
  const bool comfortable =
      host_ && host_->LayoutMode() == PhiLayoutMode::kComfortable;
  {
    std::vector<int> units;
    units.reserve(sidebar_tree_.pins.size());
    for (const SidebarCell& pin : sidebar_tree_.pins) {
      units.push_back(pin.split ? 2 : 1);
    }
    pinned_row_count_ =
        comfortable || incognito_
            ? 0
            : LayoutPinnedGrid(units, GridMetrics(Layout{})).rows;
    bookmark_row_count_ =
        incognito_ ? 0 : static_cast<int>(VisibleBookmarkCells().size());
  }
  const Layout layout = ComputeLayout();
  RebuildShortcuts(layout);

  // One paint row per content node; a split node is ONE merged row.
  const auto fill_pane = [&](const StripTab& tab, std::wstring* title,
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
        framework_->TabFaviconBgra(browser, tab.index, &w, &h);
    if (pixels && w > 0 && h > 0) {
      // The framework's buffer is scratch — valid only until its next call.
      const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
      // SAFETY: TabFaviconBgra reports the dimensions of the buffer it just
      // returned.
      const auto copy =
          UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
      favicon->assign(copy.begin(), copy.end());
      *fw = w;
      *fh = h;
    }
  };
  const auto make_row = [&](const SidebarContentNode& node) {
    TabRow row;
    const StripTab* a = StripTabById(strip, node.tab_a);
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
      if (const StripTab* b = StripTabById(strip, node.tab_b)) {
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
  if (shell_hwnd_) {
    const bool mounted = bookmark_manager_ && bookmark_manager_->visible();
    const bool ntp_mounted = new_tab_page_ && new_tab_page_->visible();
    // A page blanked by a commit whose navigation is over (or belongs to
    // another tab now) needs the sync too, to bring its controls back.
    const bool ntp_stale =
        ntp_mounted && new_tab_page_->controls_hidden() &&
        (active_tab_id_ != ntp_commit_tab_id_ || !nav_loading_);
    if (ntp_mounted) {
      new_tab_page_->Refresh();  // Palette ramps repaint it with the chrome.
    }
    if (mounted != WantsBookmarkPage() || ntp_mounted != WantsNativeNtp() ||
        ntp_stale) {
      ::PostMessageW(shell_hwnd_, kMsgPhiSyncNativePages, 0, 0);
    }
  }

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
      if (row.id == 0) {
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
    m.right = Scaled(sidebar_dip_ - kEdgeInset);
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
        header.bounds = ToRect(list.rects[i]);
        // The tinted card starts at the header's placed edge even when the
        // header itself is scrolled out; member rows extend it below.
        header.container = ToRect(list.unclipped[i]);
        group_headers_.push_back(std::move(header));
        continue;
      }
      if (node.hidden) {
        continue;  // Hidden under a collapsed header.
      }
      TabRow row = make_row(node);
      if (row.id == 0) {
        continue;
      }
      row.bounds = ToRect(list.rects[i]);
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
  // MRU bookkeeping for the Ctrl+Tab switcher: the active tab moves to the
  // front; ids that left the strip fall out.
  {
    int active_id = 0;
    for (const TabRow& row : tabs_) {
      if (row.active) {
        active_id = row.id;
        break;
      }
    }
    std::erase_if(tab_mru_, [&](int id) {
      return std::none_of(tabs_.begin(), tabs_.end(), [&](const TabRow& row) {
        return row.id == id || (row.b_index >= 0 && row.b_id == id);
      });
    });
    if (active_id) {
      std::erase(tab_mru_, active_id);
      tab_mru_.insert(tab_mru_.begin(), active_id);
    }
  }
  rows_dirty_ = false;
}

void PhiWindowController::OpenOrFocusPinned(const std::string& guid,
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
  const int64_t browser = active_browser_id().value;
  if (!browser) {
    return;
  }
  const int index = IndexOfTagged(browser, guid);
  if (index >= 0) {
    framework_->ActivateTab(browser, index);
    RefreshTabs();
    return;
  }
  if (url.empty()) {
    return;
  }
  const int created = framework_->NewTab(browser, url.c_str());
  if (created >= 0) {
    // The Mac's createTab(customGuid:): the new tab carries the row's guid
    // as its identity, and session restore carries it across restarts.
    framework_->SetTabTag(browser, created, guid.c_str());
  }
  RefreshTabs();
}

void PhiWindowController::OpenStoredSplit(const ShortcutRow& row) {
  const int64_t browser = active_browser_id().value;
  if (!framework_ || !browser) {
    return;
  }
  // Each pane binds by tag: TagFor(guid, false) for pane A, TagFor(guid,
  // true) for pane B — the same for a pinned cell and a split bookmark.
  const auto lookup = [&](const std::string& tag, int* index,
                          std::string* split) {
    *index = -1;
    split->clear();
    if (tag.empty()) {
      return;
    }
    std::optional<base::Value> parsed = base::JSONReader::Read(
        framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
    if (!parsed || !parsed->is_list()) {
      return;
    }
    for (const base::Value& v : parsed->GetList()) {
      const base::DictValue* d = v.GetIfDict();
      const std::string* row_tag = d ? d->FindString("tag") : nullptr;
      if (row_tag && *row_tag == tag) {
        *index = d->FindInt("index").value_or(-1);
        if (const std::string* s = d->FindString("splitId")) {
          *split = *s;
        }
        return;
      }
    }
  };
  int index_a = -1, index_b = -1;
  std::string split_a, split_b;
  lookup(TagFor(row.guid, false), &index_a, &split_a);
  lookup(TagFor(row.guid, true), &index_b, &split_b);
  // Ensure BOTH panes live (a created pane binds by its own pane tag), then
  // marry them if Chromium lost the split — the Mac's openPinnedSplit /
  // openTwoURLsAsSplit in one path. A pane already in a DIFFERENT split is
  // left alone: the cell reads as partial and nothing is torn apart here.
  if (index_a < 0 && !row.url.empty()) {
    index_a = framework_->NewTab(browser, row.url.c_str());
    if (index_a >= 0) {
      framework_->SetTabTag(browser, index_a, TagFor(row.guid, false).c_str());
    }
    // Indices shift under the new tab; re-resolve pane B.
    lookup(TagFor(row.guid, true), &index_b, &split_b);
  }
  if (index_b < 0 && !row.url_b.empty()) {
    index_b = framework_->NewTab(browser, row.url_b.c_str());
    if (index_b >= 0) {
      framework_->SetTabTag(browser, index_b, TagFor(row.guid, true).c_str());
    }
    lookup(TagFor(row.guid, false), &index_a, &split_a);
  }
  if (index_a < 0 || index_b < 0) {
    if (index_a >= 0 || index_b >= 0) {
      framework_->ActivateTab(browser, index_a >= 0 ? index_a : index_b);
    }
    RefreshTabs();
    return;
  }
  framework_->ActivateTab(browser, index_a);
  if (split_a.empty() && split_b.empty()) {
    framework_->SplitActiveWith(browser, index_b);
    ApplySplitLayout(index_a, row.layout);
  }
  RefreshTabs();
}

int PhiWindowController::IndexOfTagged(int64_t browser,
                                       const std::string& tag) const {
  if (!framework_ || tag.empty()) {
    return -1;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return -1;
  }
  for (const base::Value& v : parsed->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    const std::string* row_tag = d ? d->FindString("tag") : nullptr;
    if (row_tag && *row_tag == tag) {
      return d->FindInt("index").value_or(-1);
    }
  }
  return -1;
}

int PhiWindowController::CaptionHitTest(POINT client_pt) {
  const Layout layout = ComputeLayout();
  if (!::PtInRect(&layout.title_bar, client_pt)) {
    return 0;
  }
  if (::PtInRect(&layout.caption_close, client_pt)) {
    return HTCLOSE;
  }
  if (::PtInRect(&layout.caption_max, client_pt)) {
    return HTMAXBUTTON;
  }
  if (::PtInRect(&layout.caption_min, client_pt)) {
    return HTMINBUTTON;
  }
  // The band's own controls take their own clicks — the trailing cluster,
  // and comfortable's leading Phi button — without this the band would drag
  // the window out from under the pointer instead.
  if (::PtInRect(&layout.back, client_pt) ||
      ::PtInRect(&layout.forward, client_pt) ||
      ::PtInRect(&layout.reload, client_pt) ||
      ::PtInRect(&layout.search_tabs, client_pt) ||
      ::PtInRect(&layout.phi_menu, client_pt)) {
    return HTCLIENT;
  }
  if (layout.horizontal_tabs) {
    // Comfortable's band IS the tab strip. Everything drawn in it takes its
    // own clicks; the bare run past the last tab still drags the window,
    // which is how Chrome's strip behaves.
    if (::PtInRect(&add_tab_button_, client_pt)) {
      return HTCLIENT;
    }
    for (const TabRow& row : tabs_) {
      if (::PtInRect(&row.bounds, client_pt)) {
        return HTCLIENT;
      }
    }
    for (const SpaceChip& chip : chips_) {
      if (ChipHit(chip, client_pt)) {
        return HTCLIENT;
      }
    }
    for (const ShortcutRow& pin : pinned_) {
      if (::PtInRect(&pin.bounds, client_pt)) {
        return HTCLIENT;
      }
    }
  }
  const size_t titles = VisibleMenuTitles(layout);
  for (size_t i = 0; i < titles; ++i) {
    if (::PtInRect(&menu_titles_[i].second, client_pt)) {
      return HTCLIENT;  // A menu title takes the click itself.
    }
  }
  return HTCAPTION;  // Everything else in the band drags the window.
}

void PhiWindowController::SetCaptionHot(int code) {
  if (hot_caption_ == code) {
    return;
  }
  hot_caption_ = code;
  if (shell_hwnd_) {
    const RECT band = ComputeLayout().title_bar;
    ::InvalidateRect(shell_hwnd_, &band, FALSE);
  }
}

size_t PhiWindowController::VisibleMenuTitles(const Layout& layout) {
  EnsureMenuTitleRects();
  if (menu_titles_.empty()) {
    return 0;
  }
  // Comfortable gives the whole band to the tab strip and has no titles at
  // all — its band_limit is 0, so the clip below drops every one of them.
  // The vertical layouts show the Phi button alone until the band reveals.
  const bool all = menu_revealed_ || menu_alt_revealed_;
  const size_t shown = all ? menu_titles_.size() : 1;
  size_t fits = 0;
  while (fits < shown &&
         menu_titles_[fits].second.right <= layout.band_limit) {
    ++fits;
  }
  return fits;
}

void PhiWindowController::UpdateMenuRevealFromCursor() {
  POINT pt = {};
  if (!shell_hwnd_ || !::GetCursorPos(&pt)) {
    SetMenuRevealed(false);
    return;
  }
  ::ScreenToClient(shell_hwnd_, &pt);
  const RECT band = ComputeLayout().title_bar;
  SetMenuRevealed(::PtInRect(&band, pt) != 0);
}

void PhiWindowController::SetMenuRevealed(bool revealed) {
  if (!revealed && menu_open_) {
    return;  // A dropped menu holds the bar open until it closes.
  }
  if (menu_revealed_ == revealed) {
    return;
  }
  menu_revealed_ = revealed;
  if (!revealed) {
    hot_menu_ = -1;  // Nothing under the pointer once the titles are gone.
  }
  if (shell_hwnd_) {
    const RECT band = ComputeLayout().title_bar;
    ::InvalidateRect(shell_hwnd_, &band, FALSE);
  }
}

void PhiWindowController::UpdateMenuHotFromCursor() {
  POINT pt = {};
  if (!shell_hwnd_ || !::GetCursorPos(&pt)) {
    SetMenuHot(-1);
    return;
  }
  ::ScreenToClient(shell_hwnd_, &pt);
  const Layout layout = ComputeLayout();
  int hot = -1;
  if (::PtInRect(&layout.title_bar, pt)) {
    const size_t titles = VisibleMenuTitles(layout);
    for (size_t i = 0; i < titles; ++i) {
      if (::PtInRect(&menu_titles_[i].second, pt)) {
        hot = static_cast<int>(i);
        break;
      }
    }
  }
  SetMenuHot(hot);
}

void PhiWindowController::SetMenuHot(int index) {
  if (menu_open_ && index != hot_menu_) {
    // A dropped menu owns the highlight until it closes. Its popup is modal,
    // so the shell sees no pointer motion meanwhile — anything trying to move
    // the highlight now is stale by definition.
    return;
  }
  if (hot_menu_ == index) {
    return;
  }
  hot_menu_ = index;
  if (shell_hwnd_) {
    const RECT band = ComputeLayout().title_bar;
    ::InvalidateRect(shell_hwnd_, &band, FALSE);
  }
}

void PhiWindowController::OnChromeClick(int x, int y) {
  // Any press on the chrome is an interaction: the hover card goes down and
  // stays down until the pointer leaves the row it was pressed on.
  if (tab_preview_) {
    tab_preview_->Cancel();
  }
  // Clicking anywhere else in the chrome means the user is done editing the
  // address; hand the keyboard back to the page (the field is a child window,
  // so clicks on it never reach here).
  if (url_field_ && ::GetFocus() == url_field_) {
    FocusActiveBrowser();
  }
  const Layout layout = ComputeLayout();
  POINT pt = {x, y};
  // Comfortable's Phi button. That band carries no titles, so the leading
  // logo drops the bar's first menu itself — the rest of the bar hangs off
  // the toolbar's overflow.
  if (menu_bar_ && ::PtInRect(&layout.phi_menu, pt)) {
    POINT anchor = {layout.phi_menu.left, layout.title_bar.bottom};
    ::ClientToScreen(shell_hwnd_, &anchor);
    menu_open_ = true;
    menu_bar_->ShowAt(0, anchor.x, anchor.y);
    menu_open_ = false;
    return;
  }
  // A menu title in the caption band drops its menu, aligned under the title.
  // The band's trailing controls are checked BELOW this block, so the
  // drag-space early-out must not swallow them.
  if (::PtInRect(&layout.title_bar, pt) && menu_bar_ &&
      !layout.horizontal_tabs &&
      !::PtInRect(&layout.back, pt) && !::PtInRect(&layout.forward, pt) &&
      !::PtInRect(&layout.reload, pt) && !::PtInRect(&layout.search_tabs, pt)) {
    const size_t titles = VisibleMenuTitles(layout);
    for (size_t i = 0; i < titles; ++i) {
      if (::PtInRect(&menu_titles_[i].second, pt)) {
        POINT anchor = {menu_titles_[i].second.left, layout.title_bar.bottom};
        ::ClientToScreen(shell_hwnd_, &anchor);
        // The highlight belongs to the menu that is open, not to wherever
        // the pointer was last SEEN: clicking a second title while the first
        // menu is up dismisses that popup and opens this one WITHOUT the
        // shell ever receiving a move over the new title, so the old title
        // kept the wash while the new title's menu hung under it.
        SetMenuHot(static_cast<int>(i));
        // The popup takes the pointer, so the leave messages it triggers
        // must not collapse the bar behind the menu being read.
        menu_open_ = true;
        menu_bar_->ShowAt(i, anchor.x, anchor.y);
        menu_open_ = false;
        // Alt's sticky reveal has done its job by the time the menu closes;
        // the pointer decides from here.
        menu_alt_revealed_ = false;
        UpdateMenuRevealFromCursor();
        UpdateMenuHotFromCursor();
        return;
      }
    }
    return;  // The rest of the band is drag space; no chrome lives here.
  }
  if (framework_ && ::PtInRect(&layout.back, pt)) {
    // Back while the reader covers this tab reads as "leave the reader" —
    // the Mac's closeReaderOverlayForBackForwardNavigation, and what the
    // in-place reader did before there was an overlay.
    if (LeaveReaderViewForNavigation()) {
      return;
    }
    if (const BrowserId browser = active_browser_id(); browser.valid()) {
      framework_->GoBack(browser.value);
    }
    RefreshTabs();
    return;
  }
  if (framework_ && ::PtInRect(&layout.forward, pt)) {
    if (LeaveReaderViewForNavigation()) {
      return;
    }
    if (const BrowserId browser = active_browser_id(); browser.valid()) {
      framework_->GoForward(browser.value);
    }
    RefreshTabs();
    return;
  }
  if (framework_ && ::PtInRect(&layout.reload, pt)) {
    if (const BrowserId browser = active_browser_id(); browser.valid()) {
      framework_->Reload(browser.value, /*bypass_cache=*/false);
    }
    RefreshTabs();
    return;
  }
  if (::PtInRect(&layout.search_tabs, pt)) {
    PhiSearchTabsPanel::Toggle(this);  // The header's magnifier.
    return;
  }
  if (::PtInRect(&layout.downloads, pt)) {
    // The Mac DownloadButtonView's popover toggle.
    const BrowserId browser = active_browser_id();
    if (host_ && framework_ && browser.valid()) {
      RECT anchor = layout.downloads;
      ::MapWindowPoints(shell_hwnd_, nullptr,
                        reinterpret_cast<POINT*>(&anchor), 2);
      PhiDownloadsPopover::Toggle(shell_hwnd_, anchor,
                                  host_->PaletteForSlot(slot_.value),
                                  framework_, browser);
    }
    return;
  }
  if (::PtInRect(&layout.feedback, pt)) {
    OpenFeedback();  // The sidebar's Feedback capsule (SidebarBottomBar).
    return;
  }
  if (::PtInRect(&layout.extensions, pt)) {
    // The extensions popover, off the puzzle button.
    const BrowserId browser = active_browser_id();
    if (host_ && framework_ && browser.valid()) {
      RECT anchor = layout.extensions;
      ::MapWindowPoints(shell_hwnd_, nullptr,
                        reinterpret_cast<POINT*>(&anchor), 2);
      PhiExtensionsPopover::Toggle(shell_hwnd_, anchor,
                                   host_->PaletteForSlot(slot_.value),
                                   framework_, browser);
    }
    return;
  }
  for (size_t i = 0; i < field_extensions_.size(); ++i) {
    const RECT slot = FieldExtRect(layout, i);
    if (slot.right > slot.left && ::PtInRect(&slot, pt)) {
      // A pinned extension's in-field icon: run its action anchored at the
      // icon (a popup-bearing action opens Chromium's popup there).
      const BrowserId browser = active_browser_id();
      if (framework_ && browser.valid()) {
        RECT anchor = slot;
        ::MapWindowPoints(shell_hwnd_, nullptr,
                          reinterpret_cast<POINT*>(&anchor), 2);
        framework_->TriggerExtensionAction(
            browser.value,
            field_extensions_[i].id.c_str(), anchor.left, anchor.top,
            anchor.right - anchor.left, anchor.bottom - anchor.top);
      }
      return;
    }
  }
  if (::PtInRect(&layout.reader, pt)) {
    ToggleReaderView();
    return;
  }
  if (::PtInRect(&layout.copy_link, pt)) {
    // The in-field copy-link button: current URL to the clipboard, glyph
    // swaps to a checkmark for a second (the Mac copyURLButton).
    if (!nav_url_.empty() && ::OpenClipboard(shell_hwnd_)) {
      ::EmptyClipboard();
      const size_t bytes = (nav_url_.size() + 1) * sizeof(wchar_t);
      if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (wchar_t* dest = static_cast<wchar_t*>(::GlobalLock(mem))) {
          std::copy_n(nav_url_.c_str(), nav_url_.size() + 1, dest);
          ::GlobalUnlock(mem);
          if (!::SetClipboardData(CF_UNICODETEXT, mem)) {
            ::GlobalFree(mem);
          }
        } else {
          ::GlobalFree(mem);
        }
      }
      ::CloseClipboard();
      copy_link_flash_ = true;
      ::SetTimer(shell_hwnd_, kCopyLinkRevertTimer, 1000, nullptr);
      ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    }
    return;
  }
  for (const BarItem& item : bar_items_) {
    if (!::PtInRect(&item.bounds, pt)) {
      continue;
    }
    if (item.overflow) {
      // Skip the pills already on the bar; the menu takes the rest.
      ShowBookmarkBarMenu(item.bounds, std::string(),
                          static_cast<size_t>(item.bookmark));
    } else if (item.bookmark >= 0 &&
               item.bookmark < static_cast<int>(bookmarks_.size())) {
      const ShortcutRow& row = bookmarks_[static_cast<size_t>(item.bookmark)];
      if (row.folder) {
        ShowBookmarkBarMenu(item.bounds, row.guid, 0);
      } else if (!row.url.empty()) {
        OpenOrFocusPinned(row.guid, row.url);
      }
    }
    return;
  }
  for (const ShortcutRow& pin : pinned_) {
    if (::PtInRect(&pin.bounds, pt)) {
      if (pin.attached_surface != SidebarSurface::kContent &&
          ::PtInRect(&pin.surface_box, pt)) {
        Apply(SidebarCloseSurface{pin.key});
        return;
      }
      ArmDrag(pin.key, pt);
      OpenOrFocusPinned(pin.guid, pin.url);
      return;
    }
  }
  for (const ShortcutRow& bookmark : bookmarks_) {
    if (::PtInRect(&bookmark.bounds, pt)) {
      if (bookmark.attached_surface != SidebarSurface::kContent &&
          ::PtInRect(&bookmark.surface_box, pt)) {
        Apply(SidebarCloseSurface{bookmark.key});
        return;
      }
      ArmDrag(bookmark.key, pt);
      if (bookmark.folder) {
        // A folder click discloses/collapses its children (the Mac's
        // toggleItemExpanded). The armed drag is keyed, so the rebuild
        // cannot invalidate it.
        const std::string guid = bookmark.guid;
        if (!expanded_bookmarks_.erase(guid)) {
          expanded_bookmarks_.insert(guid);
        }
        RefreshTabs();
      } else if (!bookmark.url.empty()) {
        // Sidebar bookmark rows bind like pins (bookmarkGuidBacking); the
        // Bookmarks menu keeps opening plain tabs.
        OpenOrFocusPinned(bookmark.guid, bookmark.url);
      }
      return;
    }
  }
  if (framework_ && (::PtInRect(&layout.new_tab_row, pt) ||
                     ::PtInRect(&add_tab_button_, pt))) {
    StartNewTab();
    return;
  }
  if (menu_bar_ && ::PtInRect(&layout.overflow, pt)) {
    // Comfortable's only way to the menu bar. Anchored to the button's
    // trailing edge, under the toolbar, and right-aligned there.
    POINT anchor = {layout.overflow.right, layout.top_bar.bottom};
    ::ClientToScreen(shell_hwnd_, &anchor);
    menu_open_ = true;
    {
      PhiModalScope modal;  // The bar's TrackPopupMenu loop.
      menu_bar_->ShowOverflow(anchor.x, anchor.y);
    }
    menu_open_ = false;
    return;
  }
  RebuildTabs();  // Chips and rows may be stale if no paint has run yet.
  for (const SpaceChip& chip : chips_) {
    if (!ChipHit(chip, pt)) {
      continue;
    }
    // An agent pip toggles watching that agent Space (surface / park its
    // hidden window). It never reorders and has no Space card.
    if (chip.is_agent) {
      if (host_) {
        host_->RequestWatchAgentSpace(chip.id);
      }
      RefreshTabs();
      return;
    }
    // A click means "switch", not "hover": drop the pip's card and keep it
    // down until the pointer leaves this pip — without this the switched-to
    // strip's fresh hover re-presents it right after the swap (the Mac's
    // suppressHoverCard). The modal popup below would freeze it, too.
    DismissHoverCard(chip.id);
    if (!layout.horizontal_tabs) {
      // Vertical strips show every pip: reorderable by drag.
      ArmDrag(SidebarKey::Space(chip.id), pt);
    }
    if (layout.horizontal_tabs) {
      // Comfortable draws ONE compact pip — the active Space. Clicking it
      // opens the switcher popup over the FULL Space list, the Mac's
      // SpacesStripView compact-chip behaviour.
      std::vector<std::pair<std::string, std::wstring>> spaces;
      std::vector<std::wstring> glyphs;
      if (host_) {
        for (const PhiSpaceView& space : host_->Spaces()) {
          spaces.emplace_back(
              space.id, base::UTF8ToWide(!space.name.empty()
                                             ? space.name
                                             : "Space " + space.id));
          glyphs.push_back(base::UTF8ToWide(space.icon_text));
        }
      }
      HMENU menu = ::CreatePopupMenu();
      if (!menu || spaces.empty()) {
        if (menu) {
          ::DestroyMenu(menu);
        }
        return;
      }
      // Each row wears its Space's icon as a bitmap — the strip's artwork
      // or colour emoji, which menu text cannot render.
      std::vector<HBITMAP> bitmaps;
      for (size_t i = 0; i < spaces.size(); ++i) {
        // Bold (the default item) marks the active Space: a row with a
        // bitmap shows no check mark.
        ::AppendMenuW(menu,
                      MF_STRING | (spaces[i].first == active_space_
                                       ? MF_CHECKED | MF_DEFAULT
                                       : 0),
                      static_cast<UINT>(i) + 1, spaces[i].second.c_str());
        if (HBITMAP bitmap = PhiSpaceGlyphBitmap(
                glyphs[i], Scaled(16), ::GetSysColor(COLOR_MENUTEXT))) {
          bitmaps.push_back(bitmap);
          PhiSetMenuItemBitmap(menu, static_cast<UINT>(i) + 1, bitmap);
        }
      }
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      const UINT chosen = TrackPopupMenuModal(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN,
          screen.x, screen.y, shell_hwnd_);
      ::DestroyMenu(menu);
      for (HBITMAP bitmap : bitmaps) {
        ::DeleteObject(bitmap);
      }
      if (chosen >= 1 && chosen <= spaces.size()) {
        const std::string space_id = spaces[chosen - 1].first;
        if (HasSpace(space_id)) {
          ShowSpace(space_id);
        } else if (host_) {
          host_->RequestSwitchSpace(slot_.value, space_id);
        }
      }
      RefreshTabs();
      return;
    }
    if (creating_space_) {
      return;  // Read-only while the form is up (the Mac's isCreatingSpace).
    }
    if (chip.open) {
      ShowSpace(chip.id);
    } else if (host_) {
      // Not yet in this slot: the owner creates its browser in the Space's
      // profile and attaches it here.
      host_->RequestSwitchSpace(slot_.value, chip.id);
    }
    RefreshTabs();
    return;
  }
  if (host_ && ::PtInRect(&layout.add_space, pt)) {
    if (creating_space_) {
      return;  // The form is already up: requestCreation is a no-op then.
    }
    if (!chips_overflow_) {
      // The pointer is on the row, so the "+" is up — or would be, had the
      // hover-in reached us: a click under a pointer that never moved since
      // a menu dismissed over it reveals the button rather than being lost.
      // (The Mac's flag is watchdog-verified against the real pointer, so
      // its hidden-and-disabled "+" never sits under a pointer on the row.)
      SetStripRowHovered(true);
      ShowCreateSpaceForm();
      return;
    }
    // Overflow: the "…" opens the full switcher — every Space plus a
    // "New Space" row (the Mac moreButton's menu).
    std::vector<std::pair<std::string, std::wstring>> spaces;
    std::vector<std::wstring> glyphs;
    for (const PhiSpaceView& space : host_->Spaces()) {
      spaces.emplace_back(
          space.id, base::UTF8ToWide(!space.name.empty()
                                         ? space.name
                                         : "Space " + space.id));
      glyphs.push_back(base::UTF8ToWide(space.icon_text));
    }
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    // Each row wears its Space's icon as a bitmap (see the compact pip's
    // switcher above).
    std::vector<HBITMAP> bitmaps;
    for (size_t i = 0; i < spaces.size(); ++i) {
      ::AppendMenuW(menu,
                    MF_STRING | (spaces[i].first == active_space_
                                     ? MF_CHECKED | MF_DEFAULT
                                     : 0),
                    static_cast<UINT>(i) + 1, spaces[i].second.c_str());
      if (HBITMAP bitmap = PhiSpaceGlyphBitmap(
              glyphs[i], Scaled(16), ::GetSysColor(COLOR_MENUTEXT))) {
        bitmaps.push_back(bitmap);
        PhiSetMenuItemBitmap(menu, static_cast<UINT>(i) + 1, bitmap);
      }
    }
    const UINT kNewSpace = static_cast<UINT>(spaces.size()) + 1;
    const UINT kNewSpaceProfile = kNewSpace + 1;
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kNewSpace, L"New Space");
    // The Mac's create panel offers a fresh Profile for the new Space; here
    // it is the second row (profile creation is asynchronous — the Space
    // follows once the profile exists).
    ::AppendMenuW(menu, MF_STRING, kNewSpaceProfile,
                  L"New Space with New Profile...");
    POINT screen = {layout.add_space.left, layout.add_space.bottom};
    ::ClientToScreen(shell_hwnd_, &screen);
    const UINT chosen = TrackPopupMenuModal(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x,
        screen.y, shell_hwnd_);
    ::DestroyMenu(menu);
    for (HBITMAP bitmap : bitmaps) {
      ::DeleteObject(bitmap);
    }
    if (chosen == kNewSpace) {
      ShowCreateSpaceForm();
    } else if (chosen == kNewSpaceProfile) {
      host_->RequestCreateSpace(slot_.value, /*new_profile=*/true);
    } else if (chosen >= 1 && chosen <= spaces.size()) {
      const std::string space_id = spaces[chosen - 1].first;
      if (HasSpace(space_id)) {
        ShowSpace(space_id);
      } else {
        host_->RequestSwitchSpace(slot_.value, space_id);
      }
    }
    RefreshTabs();
    return;
  }
  for (const GroupHeader& gh : group_headers_) {
    if (::PtInRect(&gh.bounds, pt)) {
      const BrowserId browser = active_browser_id();
      if (framework_ && browser.valid()) {
        framework_->SetTabGroupCollapsed(browser.value, gh.id.c_str(),
                                         !gh.collapsed);
      }
      RefreshTabs();
      return;
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
        Apply(SidebarClose{row.key, -1});  // A merged row: both panes.
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
        // click (CommitDragImpl), never because it was dragged — the Mac's
        // outline selects on click, not on mouse-down, so dragging a
        // background tab leaves the page the user was reading in place.
        ArmDrag(row.key, pt);
        drag_click_tab_id_ = row.id;
      }
    }
    RefreshTabs();
    return;
  }
}

void PhiWindowController::OnChromeMiddleClick(int x, int y) {
  // Middle-click closes a tab row anywhere on the row (the Mac tab list's
  // otherMouseDown), merged split rows included. Pins and bookmarks are
  // persistent — a middle-click must not delete them, so only tab rows react.
  const POINT pt = {x, y};
  if (!framework_) {
    return;
  }
  for (const TabRow& row : tabs_) {
    if (!::PtInRect(&row.bounds, pt)) {
      continue;
    }
    Apply(SidebarClose{row.key, -1});
    return;
  }
}

namespace {

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
void FillRoundedTop(HDC dc, const RECT& r, COLORREF fill, int radius) {
  if (r.right <= r.left || r.bottom <= r.top) {
    return;
  }
  const int save = ::SaveDC(dc);
  ::IntersectClipRect(dc, r.left, r.top, r.right, r.bottom);
  PhiFillRoundRectAA(dc, {r.left, r.top, r.right, r.bottom + radius}, radius,
                     fill);
  ::RestoreDC(dc, save);
}

void FillRounded(HDC dc, const RECT& r, COLORREF fill, int radius) {
  PhiFillRoundRectAA(dc, r, radius, fill);
}

// A rounded rect with a hairline border — the address field.
// The chip the paint pass draws: `painted` square, centred in whatever cell
// the layout hands it. The vertical layouts' cell is the whole 32pt strip
// band — that size is hit area and spacing, and a selection card filling it
// would swallow the icon it exists to frame, so the card stays the Mac's
// 24pt chip (radius 8 on it, the third-of-a-side the Mac draws).
RECT PaintedChip(const RECT& cell, int painted) {
  RECT chip = cell;
  ::InflateRect(
      &chip,
      -std::max(0, static_cast<int>(cell.right - cell.left) - painted) / 2,
      -std::max(0, static_cast<int>(cell.bottom - cell.top) - painted) / 2);
  return chip;
}

void StrokeRounded(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
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
void DrawReaderGlyph(HDC dc, const RECT& button, COLORREF ink, UINT dpi) {
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

void DrawGlyph(HDC dc, const RECT& r, const wchar_t* glyph, COLORREF ink) {
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
void DrawIconGlyph(HDC dc, const RECT& r, const wchar_t* glyph,
                   int pixel_size, COLORREF ink) {
  if (r.right <= r.left) {
    return;
  }
  if (!PhiDrawIconGlyph(dc, r, glyph, pixel_size, ink)) {
    DrawGlyph(dc, r, glyph, ink);
  }
}

// `over` at `alpha`/255 on `base` — the Mac's translucent overlays, opaque.
COLORREF BlendOver(COLORREF over, COLORREF base, int alpha) {
  auto mix = [&](int a, int b) { return (a * alpha + b * (255 - alpha)) / 255; };
  return RGB(mix(GetRValue(over), GetRValue(base)),
             mix(GetGValue(over), GetGValue(base)),
             mix(GetBValue(over), GetBValue(base)));
}

// Puts `text` on the clipboard — the context menus' Copy Link.
void CopyTextToClipboard(HWND owner, const std::wstring& text) {
  if (!::OpenClipboard(owner)) {
    return;
  }
  ::EmptyClipboard();
  const size_t count = text.size() + 1;
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, count * sizeof(wchar_t))) {
    if (void* dest = ::GlobalLock(mem)) {
      // SAFETY: the allocation above is exactly `count` wchar_t.
      auto span = UNSAFE_BUFFERS(
          base::span<wchar_t>(static_cast<wchar_t*>(dest), count));
      std::copy(text.begin(), text.end(), span.begin());
      span[text.size()] = L'\0';
      ::GlobalUnlock(mem);
      if (::SetClipboardData(CF_UNICODETEXT, mem)) {
        mem = nullptr;  // The system owns it now.
      }
    }
    if (mem) {
      ::GlobalFree(mem);
    }
  }
  ::CloseClipboard();
}

// The url of the tab at `index` (TabRow keeps no url of its own).
std::string TabUrlAtIndex(phi_framework::PhiFramework* framework,
                          int64_t browser,
                          int index) {
  if (!framework) {
    return std::string();
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework->ListTabsJson(browser), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return std::string();
  }
  for (const base::Value& v : parsed->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    if (d && d->FindInt("index").value_or(-1) == index) {
      const std::string* url = d->FindString("url");
      return url ? *url : std::string();
    }
  }
  return std::string();
}

}  // namespace

// A bookmark folder's icon slot: the catalog symbol it wears, or the plain
// folder glyph for the catalog's "default" (the Mac's BookmarkFolderIcon).
void PhiWindowController::DrawFolderIcon(HDC dc,
                                         const RECT& slot,
                                         const std::string& icon_id,
                                         COLORREF ink,
                                         bool expanded) {
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
      const PhiPalette palette = CurrentPalette();
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
  HGDIOBJ previous = caption_font_ ? ::SelectObject(dc, caption_font_)
                                   : nullptr;
  DrawGlyph(dc, slot, L"", ink);  // Folder.
  if (previous) {
    ::SelectObject(dc, previous);
  }
}

void PhiWindowController::OnChromeContextMenu(int x, int y) {
  if (!host_ || !shell_hwnd_) {
    return;
  }
  RebuildTabs();  // Chips may be stale if nothing has painted yet.
  POINT pt = {x, y};
  const SpaceChip* target = nullptr;
  for (const SpaceChip& chip : chips_) {
    if (ChipHit(chip, pt)) {
      target = &chip;
      break;
    }
  }
  if (!target) {
    // Not a chip: the row context menus. Tab rows work in both orientations
    // (the strip's horizontal tabs share tabs_).
    for (const GroupHeader& gh : group_headers_) {
      if (!::PtInRect(&gh.bounds, pt)) {
        continue;
      }
      enum : UINT { kRename = 1, kUngroup, kCloseGroup, kColorFirst = 100 };
      HMENU colors = ::CreatePopupMenu();
      HMENU menu = ::CreatePopupMenu();
      if (!menu || !colors) {
        if (menu) {
          ::DestroyMenu(menu);
        }
        if (colors) {
          ::DestroyMenu(colors);
        }
        return;
      }
      for (size_t i = 0; i < kGroupColors.size(); ++i) {
        ::AppendMenuW(colors, MF_STRING | (kGroupColors[i].color == gh.color
                                               ? MF_CHECKED
                                               : 0),
                      kColorFirst + i,
                      PhiStr((std::string("tabs.tabGroup.colorName.") +
                              kGroupColors[i].name).c_str()));
      }
      ::AppendMenuW(menu, MF_STRING, kRename, L"Rename Group...");
      ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(colors),
                    L"Change Color");
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      ::AppendMenuW(menu, MF_STRING, kUngroup, L"Ungroup");
      ::AppendMenuW(menu, MF_STRING, kCloseGroup, L"Close Group");
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      // Everything the handlers need is copied out BEFORE the menu's loop:
      // a kMsgPhiRebuildRows dispatched inside it clears group_headers_,
      // and `gh` dangles from then on. Same rule for every branch below.
      const std::string group_id = gh.id;
      const std::wstring title = gh.name;
      const UINT chosen = TrackPopupMenuModal(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
          screen.x, screen.y, shell_hwnd_);
      ::DestroyMenu(menu);  // Owns the submenu.
      const int64_t browser = active_browser_id().value;
      if (!framework_ || !browser) {
        return;
      }
      if (chosen == kRename) {
        std::wstring value = title;
        if (PhiPromptText(shell_hwnd_, L"Rename Group", L"Group name:",
                          &value)) {
          framework_->RenameTabGroup(browser, group_id.c_str(),
                                     base::WideToUTF8(value).c_str());
        }
      } else if (chosen == kUngroup) {
        framework_->UngroupTabGroup(browser, group_id.c_str());
      } else if (chosen == kCloseGroup) {
        framework_->CloseTabGroup(browser, group_id.c_str());
      } else if (chosen >= kColorFirst &&
                 chosen < kColorFirst + kGroupColors.size()) {
        framework_->SetTabGroupColor(
            browser, group_id.c_str(),
            kGroupColors[chosen - kColorFirst].name);
      }
      RefreshTabs();
      return;
    }
    for (const TabRow& row : tabs_) {
      if (!::PtInRect(&row.bounds, pt)) {
        continue;
      }
      // The Mac tab menu (TabModel+Sidebar), in its order: pin, duplicate,
      // copy, the split block, bookmarks, groups, then close.
      enum : UINT { kPin = 1, kBookmark, kClose, kCloseOther, kSplitCurrent,
                    kOpenAsSplit, kRemoveSplit, kGroup, kUngroupTab,
                    kPinSplit, kBookmarkSplit, kDuplicate, kCopyUrl,
                    kCopyLeft, kCopyRight, kConvertSplit, kTakeControl,
                    kHandBack, kReversePanes, kMoveNewGroup, kFolderNew };
      // Dynamic rows ride these ranges (99 each): Move/Clone-to-Space,
      // the bookmark folders of "Add to Folder", the groups of "Add/Move
      // to Group".
      constexpr UINT kMoveSpaceFirst = 100;
      constexpr UINT kCloneSpaceFirst = 200;
      constexpr UINT kFolderFirst = 300;
      constexpr UINT kGroupFirst = 400;
      std::vector<std::string> space_targets;
      std::vector<std::string> folder_targets;
      std::vector<std::string> group_targets;
      // A group's menu title: its name (or "Untitled") with its member
      // count, the Mac's displayTitle(memberCount:).
      const auto group_label = [&](const GroupHeader& gh) {
        int members = 0;
        for (const TabRow& t : tabs_) {
          if (t.group_id == gh.id) {
            ++members;
          }
        }
        return (gh.name.empty() ? std::wstring(L"Untitled") : gh.name) +
               L" (" + std::to_wstring(members) + L")";
      };
      const int64_t browser = active_browser_id().value;
      if (!browser) {
        return;  // A row of a browser that is already gone.
      }
      HMENU menu = ::CreatePopupMenu();
      if (!menu) {
        return;
      }
      const bool merged = row.b_index >= 0;
      const std::string url_a = TabUrlAtIndex(framework_, browser, row.index);
      const std::string url_b =
          merged ? TabUrlAtIndex(framework_, browser, row.b_index)
                 : std::string();
      // Pinning/bookmarking persists to the Space store — grayed off the
      // record.
      const UINT persist_flags =
          MF_STRING | (incognito_ ? MF_DISABLED | MF_GRAYED : 0u);
      // A tab an agent is holding leads with the takeover: it is the action
      // the badge is asking about.
      if (host_ && row.id != 0 && host_->IsTabAgentDriven(row.id)) {
        const bool reclaimed = host_->IsTabReclaimed(row.id);
        ::AppendMenuW(menu, MF_STRING, reclaimed ? kHandBack : kTakeControl,
                      reclaimed ? L"Hand Back to Agent" : L"Take Control");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      }
      if (!merged) {
        ::AppendMenuW(menu, persist_flags, kPin, L"Pin");
        ::AppendMenuW(menu, MF_STRING, kDuplicate, L"Duplicate");
        ::AppendMenuW(menu, MF_STRING, kCopyUrl, L"Copy Link");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (row.split_id.empty()) {
          // "Split with Current" pairs this row with the FOCUSED tab; on
          // the focused row itself — or when the focused tab is already
          // split — it becomes "Open as Split", a fresh pane (the Mac's
          // two labels).
          bool current_can_pair = false;
          for (const TabRow& other : tabs_) {
            if (&other != &row && other.active && other.b_index < 0 &&
                other.split_id.empty()) {
              current_can_pair = true;
              break;
            }
          }
          ::AppendMenuW(menu, MF_STRING,
                        current_can_pair ? kSplitCurrent : kOpenAsSplit,
                        current_can_pair ? L"Split with Current"
                                         : L"Open as Split");
          ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        }
        ::AppendMenuW(menu, persist_flags, kBookmark, L"Add to Bookmark");
        // "Add to Folder": this Space's bookmark folders in outline order,
        // nesting indented, then a fresh folder — the Mac's addToFolder
        // submenu. Nothing persists from incognito.
        if (!incognito_ && host_) {
          HMENU folder_menu = ::CreatePopupMenu();
          std::function<void(const std::string&, int)> append_folders =
              [&](const std::string& parent, int depth) {
                for (const PhiStoredRowView& stored :
                     host_->BookmarkRows(slot_.value, parent)) {
                  if (!stored.folder || folder_targets.size() >= 99u) {
                    continue;
                  }
                  folder_targets.push_back(stored.guid);
                  std::wstring label(static_cast<size_t>(depth) * 2, L' ');
                  label += base::UTF8ToWide(
                      stored.title.empty() ? "Untitled" : stored.title);
                  ::AppendMenuW(
                      folder_menu, MF_STRING,
                      kFolderFirst +
                          static_cast<UINT>(folder_targets.size() - 1),
                      label.c_str());
                  append_folders(stored.guid, depth + 1);
                }
              };
          append_folders(std::string(), 0);
          if (!folder_targets.empty()) {
            ::AppendMenuW(folder_menu, MF_SEPARATOR, 0, nullptr);
          }
          ::AppendMenuW(folder_menu, MF_STRING, kFolderNew, L"New Folder...");
          ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                        reinterpret_cast<UINT_PTR>(folder_menu),
                        L"Add to Folder");
        }
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        // The Mac's group block: an ungrouped tab may start a group or join
        // one that exists; a grouped tab may move to another, to a new one,
        // or leave.
        if (row.group_id.empty()) {
          ::AppendMenuW(menu, MF_STRING, kGroup, L"New Tab Group");
          HMENU add_menu = ::CreatePopupMenu();
          for (const GroupHeader& gh : group_headers_) {
            if (group_targets.size() >= 99u) {
              break;
            }
            group_targets.push_back(gh.id);
            ::AppendMenuW(add_menu, MF_STRING,
                          kGroupFirst +
                              static_cast<UINT>(group_targets.size() - 1),
                          group_label(gh).c_str());
          }
          if (!group_targets.empty()) {
            ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                          reinterpret_cast<UINT_PTR>(add_menu),
                          L"Add to Group");
          } else {
            ::DestroyMenu(add_menu);
          }
        } else {
          HMENU move_group_menu = ::CreatePopupMenu();
          for (const GroupHeader& gh : group_headers_) {
            if (gh.id == row.group_id || group_targets.size() >= 99u) {
              continue;
            }
            group_targets.push_back(gh.id);
            ::AppendMenuW(move_group_menu, MF_STRING,
                          kGroupFirst +
                              static_cast<UINT>(group_targets.size() - 1),
                          group_label(gh).c_str());
          }
          if (!group_targets.empty()) {
            ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                          reinterpret_cast<UINT_PTR>(move_group_menu),
                          L"Move to Group");
          } else {
            ::DestroyMenu(move_group_menu);
          }
          ::AppendMenuW(menu, MF_STRING, kMoveNewGroup, L"Move to New Group");
          ::AppendMenuW(menu, MF_STRING, kUngroupTab, L"Remove from Group");
        }
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        // Move/Clone to Space — the Mac's cross-Space submenus (the tab's
        // own Space skipped; nothing persists from incognito).
        if (!incognito_ && host_) {
          HMENU move_menu = ::CreatePopupMenu();
          HMENU clone_menu = ::CreatePopupMenu();
          for (const PhiSpaceView& space : host_->Spaces()) {
            if (space.id == active_space_ ||
                space_targets.size() >= 99u) {
              continue;
            }
            const std::wstring name = base::UTF8ToWide(
                !space.name.empty() ? space.name : "Space " + space.id);
            ::AppendMenuW(move_menu, MF_STRING,
                          kMoveSpaceFirst +
                              static_cast<UINT>(space_targets.size()),
                          name.c_str());
            ::AppendMenuW(clone_menu, MF_STRING,
                          kCloneSpaceFirst +
                              static_cast<UINT>(space_targets.size()),
                          name.c_str());
            space_targets.push_back(space.id);
          }
          if (!space_targets.empty()) {
            ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                          reinterpret_cast<UINT_PTR>(move_menu),
                          L"Move to Space");
            ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                          reinterpret_cast<UINT_PTR>(clone_menu),
                          L"Clone to Space");
            ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
          } else {
            ::DestroyMenu(move_menu);
            ::DestroyMenu(clone_menu);
          }
        }
      } else {
        // The merged split row: every action operates on the pair.
        ::AppendMenuW(menu, persist_flags, kPinSplit, L"Pin Split");
        ::AppendMenuW(menu, MF_STRING, kDuplicate, L"Duplicate Split");
        ::AppendMenuW(menu, MF_STRING, kCopyLeft, L"Copy Left URL");
        ::AppendMenuW(menu, MF_STRING, kCopyRight, L"Copy Right URL");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        // The Mac's "Reverse Panes": swap the two sides of this split.
        ::AppendMenuW(menu, MF_STRING, kReversePanes, L"Reverse Panes");
        // The Mac's convert item, labelled by where it takes you: a
        // side-by-side pair offers the stacked arrangement and vice versa.
        ::AppendMenuW(menu, MF_STRING, kConvertSplit,
                      SplitIsStacked(row.id)
                          ? L"Convert to Horizontal Split"
                          : L"Convert to Vertical Split");
        ::AppendMenuW(menu, MF_STRING, kRemoveSplit, L"Remove from Split");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, persist_flags, kBookmarkSplit,
                      L"Add Split to Bookmark");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      }
      ::AppendMenuW(menu, MF_STRING, kClose, L"Close");
      ::AppendMenuW(menu, MF_STRING, kCloseOther, L"Close Other Tabs");
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      // The row's identity, copied out before the menu loop (see the group
      // header above): `row` is a reference into tabs_, which the rebuild
      // a title/favicon notification posts can clear under the menu.
      const SidebarKey row_key = row.key;
      const int row_id = row.id;
      const int row_index = row.index;
      const int row_b_index = row.b_index;
      const std::wstring row_title = row.title;
      const UINT chosen = TrackPopupMenuModal(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
          screen.x, screen.y, shell_hwnd_);
      ::DestroyMenu(menu);
      if (chosen >= kMoveSpaceFirst &&
          chosen < kMoveSpaceFirst + space_targets.size()) {
        Apply(SidebarMoveToSpace{row_key,
                                 space_targets[chosen - kMoveSpaceFirst],
                                 /*clone=*/false});
      } else if (chosen >= kCloneSpaceFirst &&
                 chosen < kCloneSpaceFirst + space_targets.size()) {
        Apply(SidebarMoveToSpace{row_key,
                                 space_targets[chosen - kCloneSpaceFirst],
                                 /*clone=*/true});
      } else if (chosen >= kFolderFirst &&
                 chosen < kFolderFirst + folder_targets.size()) {
        host_->AddBookmarkRow(slot_.value,
                              folder_targets[chosen - kFolderFirst],
                              base::WideToUTF8(row_title), url_a);
      } else if (chosen == kFolderNew && host_) {
        std::wstring name = L"Untitled";
        if (PhiPromptText(shell_hwnd_, L"New Folder", L"Folder name:",
                          &name)) {
          const std::string folder = host_->AddBookmarkFolder(
              slot_.value, std::string(), base::WideToUTF8(name));
          if (!folder.empty()) {
            host_->AddBookmarkRow(slot_.value, folder,
                                  base::WideToUTF8(row_title), url_a);
          }
        }
      } else if (chosen >= kGroupFirst &&
                 chosen < kGroupFirst + group_targets.size() && framework_) {
        // Joining (or moving to) an existing group: the model takes the tab
        // out of its current group atomically, so one call covers both.
        Apply(SidebarJoinGroup{row_key, group_targets[chosen - kGroupFirst]});
      } else if (chosen == kMoveNewGroup) {
        Apply(SidebarJoinGroup{row_key, std::string()});
      } else if (chosen == kReversePanes) {
        Apply(SidebarSwapPanes{row_key});
      } else if (chosen == kPin || chosen == kPinSplit) {
        // Pinning MOVES the tab (or the pair) into a cell at the grid's end.
        SidebarPlace grid;
        grid.section = SidebarPlace::Section::kPinned;
        Apply(SidebarMove{row_key, grid});
      } else if (chosen == kBookmark || chosen == kBookmarkSplit) {
        // The menu's bookmark is an unbound copy (the drag path binds).
        Apply(SidebarBookmarkCopy{row_key});
      } else if (chosen == kDuplicate && framework_) {
        // A split duplicates as a fresh split (both panes), as on Mac —
        // in the same arrangement as the one it came from.
        const bool stacked = merged && SplitIsStacked(row_id);
        const int fresh_a =
            url_a.empty() ? -1 : framework_->NewTab(browser, url_a.c_str());
        if (merged && fresh_a >= 0 && !url_b.empty()) {
          const int fresh_b = framework_->NewTab(browser, url_b.c_str());
          framework_->ActivateTab(browser, fresh_a);
          if (fresh_b >= 0) {
            framework_->SplitActiveWith(browser, fresh_b);
            if (stacked) {
              ApplySplitLayout(fresh_a, "horizontal");
            }
          }
        }
        RefreshTabs();
      } else if (chosen == kCopyUrl) {
        CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(url_a));
      } else if (chosen == kCopyLeft) {
        CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(url_a));
      } else if (chosen == kCopyRight) {
        CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(url_b));
      } else if (chosen == kSplitCurrent) {
        Apply(SidebarFormSplit{SidebarKey::Tab(sidebar_tree_.active_tab),
                               row_key, "vertical"});
      } else if (chosen == kOpenAsSplit && framework_) {
        // A fresh blank pane beside this tab, the Mac's openAsSplit. The row
        // becomes the active pane first; ids carry across the new tab, whose
        // insertion can shift every index after it.
        const int fresh = framework_->NewTab(browser, "");
        if (fresh >= 0) {
          const std::vector<StripTab> strip = ReadStrip(framework_, browser);
          const int fresh_id = StripIdAtIndex(strip, fresh);
          const int strip_index = StripIndexOfId(strip, row_id);
          if (strip_index >= 0) {
            framework_->ActivateTab(browser, strip_index);
            SplitActiveWithNormalized(browser, fresh_id);
          }
        }
        RefreshTabs();
      } else if (chosen == kTakeControl && host_) {
        host_->RequestReclaimTab(row_id);
      } else if (chosen == kHandBack && host_) {
        host_->RequestReleaseTab(row_id);
      } else if (chosen == kConvertSplit) {
        // Toggle the divider axis; the stored cell a pane is bound to
        // remembers it so the split reopens the same way.
        Apply(SidebarSetLayout{
            row_key, SplitIsStacked(row_id) ? "vertical" : "horizontal"});
      } else if (chosen == kRemoveSplit) {
        Apply(SidebarBreakSplit{row_key});
      } else if (chosen == kGroup) {
        Apply(SidebarJoinGroup{row_key, std::string()});
      } else if (chosen == kUngroupTab) {
        Apply(SidebarLeaveGroup{row_key});
      } else if (chosen == kClose) {
        Apply(SidebarClose{row_key, -1});
      } else if (chosen == kCloseOther && framework_) {
        // Every UNBOUND tab except this row (both panes of a merged one);
        // pin/bookmark-bound tabs live in their cells and stay.
        std::vector<int> doomed;
        std::optional<base::Value> parsed = base::JSONReader::Read(
            framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
        if (parsed && parsed->is_list()) {
          for (const base::Value& v : parsed->GetList()) {
            const base::DictValue* d = v.GetIfDict();
            if (!d) {
              continue;
            }
            const int index = d->FindInt("index").value_or(-1);
            const std::string* tag = d->FindString("tag");
            if (index < 0 || index == row_index ||
                (merged && index == row_b_index) ||
                (tag && !tag->empty())) {
              continue;
            }
            doomed.push_back(index);
          }
        }
        std::sort(doomed.rbegin(), doomed.rend());
        for (int index : doomed) {
          framework_->CloseTab(browser, index);
        }
        RefreshTabs();
      }
      return;
    }
    for (const ShortcutRow& pin : pinned_) {
      if (!::PtInRect(&pin.bounds, pt)) {
        continue;
      }
      // The Mac pinned-cell menu: open, duplicate, copy, rename, unpin,
      // and close (the bound live tab or pair) when open.
      enum : UINT { kOpen = 1, kUnpin, kDuplicate, kCopyUrl, kCopyLeft,
                    kCopyRight, kRename, kClose, kConvertSplit, kEditUrl };
      HMENU menu = ::CreatePopupMenu();
      if (!menu) {
        return;
      }
      const bool split_cell = !pin.url_b.empty();
      ::AppendMenuW(menu, MF_STRING, kOpen, L"Open");
      ::AppendMenuW(menu, MF_STRING, kDuplicate,
                    split_cell ? L"Duplicate Split" : L"Duplicate");
      if (split_cell) {
        ::AppendMenuW(menu, MF_STRING, kCopyLeft, L"Copy Left URL");
        ::AppendMenuW(menu, MF_STRING, kCopyRight, L"Copy Right URL");
      } else {
        ::AppendMenuW(menu, MF_STRING, kCopyUrl, L"Copy Link");
      }
      ::AppendMenuW(menu, MF_STRING, kRename, L"Rename...");
      // The Mac's Edit…: the pin's address (a split pin keeps its panes).
      if (!split_cell) {
        ::AppendMenuW(menu, MF_STRING, kEditUrl, L"Edit URL...");
      }
      if (split_cell) {
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kConvertSplit,
                      pin.layout == "horizontal"
                          ? L"Convert to Horizontal Split"
                          : L"Convert to Vertical Split");
      }
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      ::AppendMenuW(menu, MF_STRING, kUnpin,
                    split_cell ? L"Unpin Split" : L"Unpin");
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      ::AppendMenuW(menu, MF_STRING | (pin.open ? 0u : MF_DISABLED |
                                                           MF_GRAYED),
                    kClose, L"Close");
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      // Copied before the loop: pinned_ can be cleared under the menu.
      const std::string guid = pin.guid;
      const std::string url = pin.url;
      const std::string url_b = pin.url_b;
      const std::wstring pin_title = pin.title;
      const std::string pin_layout = pin.layout;
      const UINT chosen = TrackPopupMenuModal(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
          screen.x, screen.y, shell_hwnd_);
      ::DestroyMenu(menu);
      const int64_t browser = active_browser_id().value;
      if (!browser) {
        return;  // The slot lost its last browser under the menu.
      }
      if (chosen == kOpen && !url.empty()) {
        OpenOrFocusPinned(guid, url);
      } else if (chosen == kDuplicate && framework_ && !url.empty()) {
        // A fresh unbound copy — for a split cell, a fresh split.
        const int fresh_a = framework_->NewTab(browser, url.c_str());
        if (split_cell && fresh_a >= 0 && !url_b.empty()) {
          const int fresh_b = framework_->NewTab(browser, url_b.c_str());
          framework_->ActivateTab(browser, fresh_a);
          if (fresh_b >= 0) {
            framework_->SplitActiveWith(browser, fresh_b);
          }
        }
        RefreshTabs();
      } else if (chosen == kCopyUrl || chosen == kCopyLeft) {
        CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(url));
      } else if (chosen == kCopyRight) {
        CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(url_b));
      } else if (chosen == kRename) {
        std::wstring name = pin_title;
        if (PhiPromptText(shell_hwnd_, L"Rename Pinned Tab",
                          L"Enter a new name for this pinned tab.", &name)) {
          host_->RenameStoredRow(guid, base::WideToUTF8(name));
        }
      } else if (chosen == kConvertSplit) {
        // Flip what the cell REMEMBERS, and the live split too when it is
        // currently open — the Mac's convertClosedPinnedSplitLayout.
        Apply(SidebarSetLayout{
            SidebarKey::Cell(guid),
            pin_layout == "horizontal" ? "vertical" : "horizontal"});
      } else if (chosen == kEditUrl && host_ && !guid.empty()) {
        std::wstring edited = base::UTF8ToWide(url);
        if (PhiPromptText(shell_hwnd_, L"Edit Pinned Tab", L"Address:",
                          &edited) &&
            !edited.empty()) {
          host_->SetStoredRowUrl(guid, base::WideToUTF8(edited));
        }
      } else if (chosen == kUnpin && !guid.empty()) {
        // The cell goes; a live bound tab returns to the tab list as a
        // normal tab (the tags are cleared).
        Apply(SidebarRemoveCell{SidebarKey::Cell(guid)});
      } else if (chosen == kClose) {
        // Close the bound live tab(s); the pinned cell stays, as on Mac.
        Apply(SidebarClose{SidebarKey::Cell(guid), -1});
      }
      return;
    }
    for (const ShortcutRow& bookmark : bookmarks_) {
      if (!::PtInRect(&bookmark.bounds, pt)) {
        continue;
      }
      // The Mac bookmark-row menu: open, open as split, copy, rename,
      // delete (folders keep rename/new-nested-folder/delete).
      enum : UINT { kOpen = 1, kOpenSplit, kCopyUrl, kCopyLeft, kCopyRight,
                    kRename, kNewFolder, kChangeIcon, kRemove,
                    kConvertSplit, kEditUrl };
      // Move/Clone-to-Space rows ride these ranges (99 each).
      constexpr UINT kMoveSpaceFirst = 100;
      constexpr UINT kCloneSpaceFirst = 200;
      std::vector<std::string> space_targets;
      HMENU menu = ::CreatePopupMenu();
      if (!menu) {
        return;
      }
      const bool split_row = !bookmark.url_b.empty();
      if (!bookmark.folder) {
        ::AppendMenuW(menu, MF_STRING, kOpen, L"Open in New Tab");
        ::AppendMenuW(menu, MF_STRING, kOpenSplit, L"Open as Split");
        if (split_row) {
          ::AppendMenuW(menu, MF_STRING, kCopyLeft, L"Copy Left URL");
          ::AppendMenuW(menu, MF_STRING, kCopyRight, L"Copy Right URL");
        } else {
          ::AppendMenuW(menu, MF_STRING, kCopyUrl, L"Copy Link");
        }
      }
      ::AppendMenuW(menu, MF_STRING, kRename, L"Rename...");
      if (!bookmark.folder && !split_row) {
        ::AppendMenuW(menu, MF_STRING, kEditUrl, L"Edit URL...");  // Mac: Edit…
      }
      if (split_row) {
        ::AppendMenuW(menu, MF_STRING, kConvertSplit,
                      bookmark.layout == "horizontal"
                          ? L"Convert to Horizontal Split"
                          : L"Convert to Vertical Split");
      }
      if (bookmark.folder) {
        // The Mac's folder menu: Rename, then the icon picker.
        ::AppendMenuW(menu, MF_STRING, kChangeIcon, L"Change Icon...");
        ::AppendMenuW(menu, MF_STRING, kNewFolder,
                      L"New Nested Folder...");
      }
      // Move/Clone to Space — the Mac's appendSpaceTransferMenuItems, leaf
      // rows only (a folder's subtree stays where it is).
      if (!bookmark.folder && !incognito_ && host_) {
        HMENU move_menu = ::CreatePopupMenu();
        HMENU clone_menu = ::CreatePopupMenu();
        for (const PhiSpaceView& space : host_->Spaces()) {
          if (space.id == active_space_ || space_targets.size() >= 99u) {
            continue;
          }
          const std::wstring name = base::UTF8ToWide(
              !space.name.empty() ? space.name : "Space " + space.id);
          ::AppendMenuW(move_menu, MF_STRING,
                        kMoveSpaceFirst +
                            static_cast<UINT>(space_targets.size()),
                        name.c_str());
          ::AppendMenuW(clone_menu, MF_STRING,
                        kCloneSpaceFirst +
                            static_cast<UINT>(space_targets.size()),
                        name.c_str());
          space_targets.push_back(space.id);
        }
        if (!space_targets.empty()) {
          ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
          ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                        reinterpret_cast<UINT_PTR>(move_menu),
                        L"Move to Space");
          ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                        reinterpret_cast<UINT_PTR>(clone_menu),
                        L"Clone to Space");
        } else {
          ::DestroyMenu(move_menu);
          ::DestroyMenu(clone_menu);
        }
      }
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      ::AppendMenuW(menu, MF_STRING, kRemove,
                    bookmark.folder ? L"Delete Folder" : L"Delete");
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      // Copied before the loop: bookmarks_ can be cleared under the menu.
      const std::string guid = bookmark.guid;
      const std::string url = bookmark.url;
      const std::string url_b = bookmark.url_b;
      const std::wstring bookmark_title = bookmark.title;
      const bool folder = bookmark.folder;
      const std::string row_icon = bookmark.icon;
      const std::string row_layout = bookmark.layout;
      const RECT row_bounds = bookmark.bounds;
      const UINT chosen = TrackPopupMenuModal(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
          screen.x, screen.y, shell_hwnd_);
      ::DestroyMenu(menu);
      const int64_t browser = active_browser_id().value;
      if (!browser) {
        return;  // The slot lost its last browser under the menu.
      }
      if (chosen >= kMoveSpaceFirst &&
          chosen < kMoveSpaceFirst + space_targets.size()) {
        Apply(SidebarMoveToSpace{SidebarKey::Cell(guid),
                                 space_targets[chosen - kMoveSpaceFirst],
                                 /*clone=*/false});
      } else if (chosen >= kCloneSpaceFirst &&
                 chosen < kCloneSpaceFirst + space_targets.size()) {
        Apply(SidebarMoveToSpace{SidebarKey::Cell(guid),
                                 space_targets[chosen - kCloneSpaceFirst],
                                 /*clone=*/true});
      } else if (chosen == kEditUrl && !folder && host_) {
        std::wstring edited = base::UTF8ToWide(url);
        if (PhiPromptText(shell_hwnd_, L"Edit Bookmark", L"Address:",
                          &edited) &&
            !edited.empty()) {
          host_->SetStoredRowUrl(guid, base::WideToUTF8(edited));
        }
      } else if (chosen == kOpen && !folder && !url.empty()) {
        OpenOrFocusPinned(guid, url);
      } else if (chosen == kOpenSplit && !folder && framework_ &&
                 !url.empty()) {
        if (split_row) {
          // A split bookmark opens as its own pair either way.
          OpenOrFocusPinned(guid, url);
        } else {
          // Pair the bookmark with the currently focused tab (the Mac's
          // openAsSplit on a bookmark row → formSplitFromBookmark). The
          // partner is read from the STRIP, not from tabs_: a focused tab
          // bound to a pinned cell or another bookmark is not a row there,
          // and skipping it would silently do nothing. It is unbound with
          // the new pane below, so the pair lands in the normal tab list.
          int partner_id = 0;
          for (const StripTab& tab : ReadStrip(framework_, browser)) {
            if (tab.active && tab.split_id.empty()) {
              partner_id = tab.id;
              break;
            }
          }
          const int fresh = framework_->NewTab(browser, url.c_str());
          if (partner_id != 0 && fresh >= 0) {
            const std::vector<StripTab> strip = ReadStrip(framework_, browser);
            const int fresh_id = StripIdAtIndex(strip, fresh);
            const int partner_index = StripIndexOfId(strip, partner_id);
            if (partner_index >= 0) {
              framework_->ActivateTab(browser, partner_index);
              SplitActiveWithNormalized(browser, fresh_id);
            }
          }
          RefreshTabs();
        }
      } else if (chosen == kCopyUrl || chosen == kCopyLeft) {
        CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(url));
      } else if (chosen == kCopyRight) {
        CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(url_b));
      } else if (chosen == kRename) {
        std::wstring name = bookmark_title;
        if (PhiPromptText(shell_hwnd_,
                          folder ? L"Rename Folder" : L"Rename Bookmark",
                          folder ? L"Enter a new name for this folder."
                                 : L"Enter a new name for this bookmark.",
                          &name)) {
          host_->RenameStoredRow(guid, base::WideToUTF8(name));
        }
      } else if (chosen == kConvertSplit) {
        Apply(SidebarSetLayout{
            SidebarKey::Cell(guid),
            row_layout == "horizontal" ? "vertical" : "horizontal"});
      } else if (chosen == kChangeIcon && folder) {
        // The picker hangs off the row, as the Mac's popover does.
        POINT top_left = {row_bounds.left, row_bounds.top};
        POINT bottom_right = {row_bounds.right, row_bounds.bottom};
        ::ClientToScreen(shell_hwnd_, &top_left);
        ::ClientToScreen(shell_hwnd_, &bottom_right);
        const RECT anchor = {top_left.x, top_left.y, bottom_right.x,
                             bottom_right.y};
        std::string picked;
        {
          PhiModalScope modal;  // The picker pumps its own loop.
          picked = PhiPickFolderIcon(this, anchor, row_icon);
        }
        if (!picked.empty()) {
          host_->SetStoredRowIcon(guid, picked);
        }
      } else if (chosen == kNewFolder && folder) {
        std::wstring name = L"Untitled";
        if (PhiPromptText(shell_hwnd_, L"New Folder", L"Folder name:",
                          &name)) {
          // Disclose the parent so the new child is visible where it lands.
          expanded_bookmarks_.insert(guid);
          host_->AddBookmarkFolder(slot_.value, guid, base::WideToUTF8(name));
        }
      } else if (chosen == kRemove && !guid.empty()) {
        // A folder takes its subtree.
        Apply(SidebarRemoveCell{folder ? SidebarKey::Folder(guid)
                                       : SidebarKey::Cell(guid)});
      }
      return;
    }
    // The tab area's background — the Mac tab-area menu's bookmark block
    // (TabAreaContextMenuHelper): root folder creation lives here. Both
    // orientations share it, as the Mac's two hosts share one
    // TabAreaContextMenuHelper: the sidebar below the band in the vertical
    // layouts, and in comfortable the caption band itself, whose bare run
    // past the last tab IS that layout's empty tab-strip space
    // (TabStripBarController hangs the same stripContextMenu off the bar).
    const Layout layout = ComputeLayout();
    const bool in_tab_area =
        layout.horizontal_tabs
            ? ::PtInRect(&layout.title_bar, pt) != 0
            : (pt.x < layout.sidebar.right && pt.y > layout.title_bar.bottom);
    if (in_tab_area) {
      // The Mac tab-area menu (TabAreaContextMenuHelper): new tab, reopen,
      // the bookmark block, the layout switch, then the active Space's own
      // controls (the Spaces menu's rows, for the Space under the pointer).
      enum : UINT { kNewTab = 1, kReopen, kNewRootFolder, kBookmarkAll,
                    kLayoutPerformance, kLayoutBalanced, kLayoutComfortable,
                    kNewSpace, kRenameSpace, kChangeSpaceIcon, kSetTheme,
                    kDeleteSpace,
                    // The Change Profile submenu's rows ride this range.
                    kProfileFirst = 600 };
      HMENU menu = ::CreatePopupMenu();
      HMENU layout_menu = ::CreatePopupMenu();
      HMENU profile_menu = ::CreatePopupMenu();
      if (!menu || !layout_menu || !profile_menu) {
        for (HMENU created : {menu, layout_menu, profile_menu}) {
          if (created) {
            ::DestroyMenu(created);
          }
        }
        return;
      }
      const UINT persist_flags =
          MF_STRING | (incognito_ ? MF_DISABLED | MF_GRAYED : 0u);
      ::AppendMenuW(menu, MF_STRING, kNewTab, L"New Tab");
      ::AppendMenuW(menu, MF_STRING, kReopen, L"Reopen Closed Tab");
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      ::AppendMenuW(menu, persist_flags, kNewRootFolder, L"New Folder...");
      ::AppendMenuW(menu, persist_flags | (tabs_.empty() ? MF_DISABLED : 0u),
                    kBookmarkAll, L"Bookmark All Tabs");
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      const struct {
        UINT id;
        PhiLayoutMode mode;
        const wchar_t* label;
      } kLayouts[] = {
          {kLayoutPerformance, PhiLayoutMode::kPerformance, L"Performance"},
          {kLayoutBalanced, PhiLayoutMode::kBalanced, L"Balanced"},
          {kLayoutComfortable, PhiLayoutMode::kComfortable, L"Comfortable"},
      };
      for (const auto& entry : kLayouts) {
        ::AppendMenuW(layout_menu,
                      MF_STRING | (layout_mode_ == entry.mode ? MF_CHECKED : 0u),
                      entry.id, entry.label);
      }
      ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                    reinterpret_cast<UINT_PTR>(layout_menu), L"Layout Mode");
      // The active Space's own theme and profile, in the Mac's order
      // (appendActiveSpaceMenuItems): rename, icon, theme, profile, delete.
      std::vector<std::string> profile_ids;
      std::vector<std::wstring> profile_names;
      if (!incognito_) {
        // Change Profile — the Mac's radio list. Re-binding respawns the
        // Space's window, which the default Space's cannot be (a fresh
        // shell's first browser lands there), so its rows read the binding
        // out but stay disabled.
        std::string space_profile;
        for (const PhiSpaceView& space : host_->Spaces()) {
          if (space.id == active_space_) {
            space_profile = space.profile_id;
            break;
          }
        }
        const bool can_reprofile = active_space_ != host_->DefaultSpaceId();
        for (const std::string& summary : host_->ProfileSummaries()) {
          const size_t tab = summary.find('\t');
          const std::string id = summary.substr(0, tab);
          const std::string name =
              tab == std::string::npos ? id : summary.substr(tab + 1);
          if (id.empty()) {
            continue;
          }
          ::AppendMenuW(profile_menu,
                        MF_STRING | (id == space_profile ? MF_CHECKED : 0u) |
                            (can_reprofile ? 0u : MF_DISABLED | MF_GRAYED),
                        kProfileFirst + static_cast<UINT>(profile_ids.size()),
                        base::UTF8ToWide(name).c_str());
          profile_ids.push_back(id);
          profile_names.push_back(base::UTF8ToWide(name));
        }
        if (profile_ids.empty()) {
          ::AppendMenuW(profile_menu, MF_STRING | MF_DISABLED | MF_GRAYED, 0,
                        PhiStr("menuBar.noProfiles"));
        }
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(menu, MF_STRING, kNewSpace, L"New Space...");
        ::AppendMenuW(menu, MF_STRING, kRenameSpace, L"Rename Space...");
        ::AppendMenuW(menu, MF_STRING, kChangeSpaceIcon, L"Change Icon...");
        // Set Theme opens the Mac's editor panel (swatch row + saturation
        // slider) rather than a list of theme names: color and saturation
        // are one control there, and a Space always wears a concrete theme,
        // so there is no "follow the app" row to offer.
        ::AppendMenuW(menu, MF_STRING, kSetTheme, PhiStr("sidebar.setTheme"));
        ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                      reinterpret_cast<UINT_PTR>(profile_menu),
                      PhiStr("app.activeSpaceContextMenu.profileSubmenuTitle"));
        ::AppendMenuW(menu,
                      MF_STRING | (host_->CanDeleteSpace(active_space_)
                                       ? 0u
                                       : MF_DISABLED | MF_GRAYED),
                      kDeleteSpace, L"Delete Space...");
      } else {
        // Incognito shells have no Spaces, so the profile submenu never
        // gets attached — free it here rather than leaking its handle.
        ::DestroyMenu(profile_menu);
      }
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      const UINT chosen = TrackPopupMenuModal(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
          screen.x, screen.y, shell_hwnd_);
      ::DestroyMenu(menu);  // Owns every submenu it was given.
      if (chosen == kSetTheme) {
        // Anchored on the click, like the Mac's .nearTrigger placement.
        const RECT anchor = {screen.x, screen.y, screen.x, screen.y};
        PhiShowSpaceThemePanel(this, anchor, active_space_);
        return;
      }
      // The profile rows' dynamic range — a switch cannot express it.
      if (chosen >= kProfileFirst &&
          chosen - kProfileFirst < profile_ids.size()) {
        const size_t i = chosen - kProfileFirst;
        // Confirmed like the Mac's: the Space's window is replaced and its
        // tabs reload in the new profile, where the old profile's logins do
        // not exist.
        if (PhiConfirm(shell_hwnd_,
                       PhiStrF("app.changeSpaceProfileConfirmation.title",
                               {profile_names[i]})
                           .c_str(),
                       PhiStr("app.changeSpaceProfileConfirmation."
                              "profileScopedMessage"))) {
          host_->SetSpaceProfile(active_space_, profile_ids[i]);
          RefreshTabs();
        }
        return;
      }
      switch (chosen) {
        case kNewTab:
          StartNewTab();
          break;
        case kNewRootFolder: {
          std::wstring name = L"Untitled";
          if (PhiPromptText(shell_hwnd_, L"New Folder", L"Folder name:",
                            &name)) {
            host_->AddBookmarkFolder(slot_.value, std::string(),
                                     base::WideToUTF8(name));
          }
          break;
        }
        case kLayoutPerformance:
          host_->RequestLayoutMode(PhiLayoutMode::kPerformance);
          break;
        case kLayoutBalanced:
          host_->RequestLayoutMode(PhiLayoutMode::kBalanced);
          break;
        case kLayoutComfortable:
          host_->RequestLayoutMode(PhiLayoutMode::kComfortable);
          break;
        // The rest are the menu bar's own commands, run through it so the
        // sidebar and the bar cannot drift apart.
        case kReopen:
        case kBookmarkAll:
        case kNewSpace:
        case kRenameSpace:
        case kChangeSpaceIcon:
        case kDeleteSpace:
          if (menu_bar_) {
            menu_bar_->OnCommand(chosen == kReopen        ? kCmdReopenClosedTab
                                 : chosen == kBookmarkAll ? kCmdBookmarkAllTabs
                                 : chosen == kNewSpace    ? kCmdNewSpace
                                 : chosen == kRenameSpace ? kCmdRenameSpace
                                 : chosen == kChangeSpaceIcon
                                     ? kCmdChangeIcon
                                     : kCmdDeleteSpace);
          }
          break;
        default:
          break;
      }
    }
    return;
  }

  if (target->is_agent) {
    // The agent pip's own menu (the Mac agentContextMenu): the transcript
    // console and finishing the task. The Space menu below would misapply
    // rename/theme/delete to a task id.
    enum : UINT { kTranscript = 1, kFinish };
    HMENU agent_menu = ::CreatePopupMenu();
    if (!agent_menu) {
      return;
    }
    ::AppendMenuW(agent_menu, MF_STRING, kTranscript, L"Show Transcript");
    ::AppendMenuW(agent_menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(agent_menu, MF_STRING, kFinish, L"Finish Task");
    POINT screen = pt;
    ::ClientToScreen(shell_hwnd_, &screen);
    // Copied BEFORE the menu: a rebuild dispatched inside its loop clears
    // chips_, and the handlers below can rebuild them too.
    const std::string task = target->id;
    const UINT chosen = TrackPopupMenuModal(
        agent_menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x,
        screen.y, shell_hwnd_);
    ::DestroyMenu(agent_menu);
    if (chosen == kTranscript) {
      host_->RequestShowTranscript(task);
    } else if (chosen == kFinish) {
      host_->RequestAgentFinish(task);
      RefreshTabs();
    }
    return;
  }

  // A user Space pip has no context menu: switching is a click, and
  // rename / icon / theme / profile / delete live in the sidebar's own
  // menu (right-click its empty space), Settings > Spaces and the Spaces
  // menu.
}

// --- The floating sidebar -------------------------------------------------
// The Mac panel (WebContentContainerViewController+FloatingSidebar): with
// the sidebar collapsed, hovering the window's left margin slides in a
// popup showing the real sidebar. It renders by running PaintChrome
// against the EXPANDED layout (floating_layout_) into a back buffer and
// blitting the sidebar band; its mouse events forward through the normal
// handlers with the band's y offset, so chips, tabs, pins, bookmarks and
// menus all behave as in the docked sidebar.

void PhiWindowController::ShowFloatingSidebar() {
  if (floating_sidebar_ || !sidebar_collapsed_ || !shell_hwnd_) {
    return;
  }
  floating_layout_ = true;
  const Layout layout = ComputeLayout();
  floating_layout_ = false;
  if (layout.horizontal_tabs) {
    return;
  }
  // The Mac's 5pt inset from the window edges; the panel band starts below
  // the caption so the menu row never appears inside it.
  const int panel_inset = Scaled(5);
  floating_offset_y_ = layout.title_bar.bottom + panel_inset;
  const int panel_w = Scaled(sidebar_dip_);
  const int panel_h = layout.client.bottom - floating_offset_y_ - panel_inset;
  if (panel_h <= 0 || panel_w <= 0) {
    return;
  }
  static bool registered = false;
  if (!registered) {
    registered = true;
    WNDCLASSW cls = {};
    cls.lpfnWndProc = &PhiWindowController::FloatingSidebarProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"PhiFloatingSidebar";
    ::RegisterClassW(&cls);
  }
  POINT origin = {0, 0};
  ::ClientToScreen(shell_hwnd_, &origin);
  floating_sidebar_ = ::CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"PhiFloatingSidebar", L"",
      WS_POPUP, origin.x + panel_inset, origin.y + floating_offset_y_,
      panel_w, panel_h, shell_hwnd_, nullptr, ::GetModuleHandleW(nullptr),
      nullptr);
  if (!floating_sidebar_) {
    return;
  }
  ::SetWindowLongPtrW(floating_sidebar_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(floating_sidebar_, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));
  // The Mac's 0.15s ease-out slide-in.
  ::AnimateWindow(floating_sidebar_, 140, AW_SLIDE | AW_HOR_POSITIVE);
  ::SetTimer(floating_sidebar_, kFloatingTickTimer, 100, nullptr);
  floating_shown_tick_ = ::GetTickCount64();
  floating_outside_tick_ = 0;
}

void PhiWindowController::HideFloatingSidebar(bool animated) {
  if (!floating_sidebar_) {
    return;
  }
  HWND panel = floating_sidebar_;
  floating_sidebar_ = nullptr;
  ::KillTimer(panel, kFloatingTickTimer);
  if (animated) {
    ::AnimateWindow(panel, 140, AW_SLIDE | AW_HOR_NEGATIVE | AW_HIDE);
  }
  ::DestroyWindow(panel);
}

void PhiWindowController::FloatingSidebarTick() {
  if (!floating_sidebar_) {
    return;
  }
  // Hover states, hover cards and tab changes land on the next tick.
  ::InvalidateRect(floating_sidebar_, nullptr, FALSE);
  POINT cursor = {};
  ::GetCursorPos(&cursor);
  RECT panel_rc = {};
  ::GetWindowRect(floating_sidebar_, &panel_rc);
  bool inside = ::PtInRect(&panel_rc, cursor) != FALSE;
  if (!inside && shell_hwnd_) {
    // The trigger strip pins it open, like the Mac's trigger tracking.
    POINT origin = {0, 0};
    ::ClientToScreen(shell_hwnd_, &origin);
    const Layout layout = ComputeLayout();
    RECT strip = {origin.x, origin.y + layout.title_bar.bottom,
                  origin.x + layout.content.left,
                  origin.y + layout.client.bottom};
    inside = ::PtInRect(&strip, cursor) != FALSE;
  }
  const ULONGLONG now = ::GetTickCount64();
  if (inside) {
    floating_outside_tick_ = 0;
    return;
  }
  if (!floating_outside_tick_) {
    floating_outside_tick_ = now;
    return;
  }
  // The Mac's 0.12s leave delay and 0.5s minimum visible time.
  if (now - floating_outside_tick_ >= 120 &&
      now - floating_shown_tick_ >= 500) {
    HideFloatingSidebar(/*animated=*/true);
  }
}

void PhiWindowController::PaintFloatingSidebar(HDC dc) {
  if (!floating_sidebar_) {
    return;
  }
  RECT panel_rc = {};
  ::GetClientRect(floating_sidebar_, &panel_rc);
  floating_layout_ = true;
  RebuildTabs();
  const Layout layout = ComputeLayout();
  HDC mem = ::CreateCompatibleDC(dc);
  HBITMAP bitmap = ::CreateCompatibleBitmap(dc, layout.client.right,
                                            layout.client.bottom);
  HGDIOBJ old = ::SelectObject(mem, bitmap);
  // include_child_controls: the panel has no live EDIT child, so the
  // address text is painted by hand (the capture path's contract).
  PaintChrome(mem, layout, /*include_child_controls=*/true);
  ::BitBlt(dc, 0, 0, panel_rc.right, panel_rc.bottom, mem, 0,
           floating_offset_y_, SRCCOPY);
  ::SelectObject(mem, old);
  ::DeleteObject(bitmap);
  ::DeleteDC(mem);
  floating_layout_ = false;
  ScheduleRefresh();  // The shell recomputes its collapsed bounds next turn.
  // The panel's edge hairline (the Mac panel's 1pt border).
  HPEN pen = ::CreatePen(PS_SOLID, 1, CurrentPalette().border);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
  ::Rectangle(dc, panel_rc.left, panel_rc.top, panel_rc.right,
              panel_rc.bottom);
  ::SelectObject(dc, old_brush);
  ::SelectObject(dc, old_pen);
  ::DeleteObject(pen);
}

// static
LRESULT CALLBACK PhiWindowController::FloatingSidebarProc(HWND hwnd,
                                                          UINT msg,
                                                          WPARAM wparam,
                                                          LPARAM lparam) {
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiWindowController*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self || self->destroying_) {
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  // Events forward through the docked sidebar's handlers under the
  // expanded-layout override, shifted by the panel's band offset.
  const auto forward = [&](auto&& call) {
    self->floating_layout_ = true;
    self->RebuildTabs();
    call();
    self->floating_layout_ = false;
    self->ScheduleRefresh();
    ::InvalidateRect(hwnd, nullptr, FALSE);
  };
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;  // Focus stays with the page.
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        self->PaintFloatingSidebar(dc);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_PRINTCLIENT:  // AnimateWindow renders the slide through this.
      if (self) {
        self->PaintFloatingSidebar(reinterpret_cast<HDC>(wparam));
      }
      return 0;
    case WM_MOUSEMOVE:
      if (self) {
        forward([&] {
          POINT pt = {GET_X_LPARAM(lparam),
                      GET_Y_LPARAM(lparam) + self->floating_offset_y_};
          self->OnChromeMouseMove(pt);
        });
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
      if (self) {
        forward([&] {
          self->OnChromeClick(GET_X_LPARAM(lparam),
                              GET_Y_LPARAM(lparam) +
                                  self->floating_offset_y_);
          self->AbortDrag();  // Drags never start from the panel.
        });
        return 0;
      }
      break;
    case WM_RBUTTONUP:
      if (self) {
        forward([&] {
          self->OnChromeContextMenu(GET_X_LPARAM(lparam),
                                    GET_Y_LPARAM(lparam) +
                                        self->floating_offset_y_);
        });
        return 0;
      }
      break;
    case WM_MOUSEWHEEL:
      if (self) {
        forward([&] {
          POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
          ::ScreenToClient(hwnd, &pt);
          pt.y += self->floating_offset_y_;
          self->OnSidebarWheel(pt, GET_WHEEL_DELTA_WPARAM(wparam));
        });
        return 0;
      }
      break;
    case WM_TIMER:
      if (self && wparam == kFloatingTickTimer) {
        self->FloatingSidebarTick();
        return 0;
      }
      break;
    case WM_DESTROY:
      ::KillTimer(hwnd, kFloatingTickTimer);
      if (self && self->floating_sidebar_ == hwnd) {
        self->floating_sidebar_ = nullptr;
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

bool PhiWindowController::BookmarkBarVisible() {
  if (!host_ || incognito_ || content_fullscreen_) {
    return false;
  }
  if (host_->AlwaysShowBookmarkBar()) {
    return true;
  }
  if (!host_->BookmarkBarOnNewTab()) {
    return false;
  }
  // "On new tab": Chromium's NTP, a blank tab, or the placeholder shell.
  if (placeholder_browsers_.count(active_browser_id())) {
    return true;
  }
  return active_tab_url_.empty() || active_tab_url_ == "about:blank" ||
         active_tab_url_.rfind("chrome://newtab", 0) == 0 ||
         active_tab_url_.rfind("chrome://new-tab-page", 0) == 0;
}

void PhiWindowController::PaintBookmarkBar(HDC dc,
                                           const Layout& layout,
                                           const PhiPalette& palette) {
  bar_items_.clear();
  const RECT& band = layout.bookmark_bar;
  if (band.right <= band.left || band.bottom <= band.top) {
    return;
  }
  // Root rows only — the sidebar outline's spliced children (depth > 0)
  // belong to the sidebar's disclosure state, not the bar.
  std::vector<int> roots;
  for (size_t i = 0; i < bookmarks_.size(); ++i) {
    if (bookmarks_[i].depth == 0) {
      roots.push_back(static_cast<int>(i));
    }
  }
  ::SetBkMode(dc, TRANSPARENT);
  if (ui_font_) {
    ::SelectObject(dc, ui_font_);
  }
  if (roots.empty()) {
    // The Mac bar's empty hint, dimmed and leading.
    RECT hint = band;
    hint.left += Scaled(12);
    ::SetTextColor(dc, palette.label_dim);
    PhiDrawTextAA(dc, L"Bookmarks you add appear here", -1, &hint,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    return;
  }
  const int pill_h = Scaled(24);
  const int pad = Scaled(8);
  const int icon_px = Scaled(kFaviconSize);
  const int more_w = Scaled(24);
  const int mid = (band.top + band.bottom) / 2;
  const int limit = band.right - pad - more_w;
  int x = band.left + pad;
  size_t shown = 0;
  for (const int index : roots) {
    const ShortcutRow& row = bookmarks_[static_cast<size_t>(index)];
    SIZE extent = {0, 0};
    ::GetTextExtentPoint32W(dc, row.title.c_str(),
                            static_cast<int>(row.title.size()), &extent);
    const int label_w = std::min<int>(extent.cx, Scaled(160));
    const int pill_w = pad + icon_px + Scaled(6) + label_w + pad;
    if (x + pill_w > limit) {
      break;
    }
    const RECT pill = {x, mid - pill_h / 2, x + pill_w, mid + pill_h / 2};
    RECT icon = {pill.left + pad, mid - icon_px / 2, pill.left + pad + icon_px,
                 mid + icon_px / 2};
    if (row.folder) {
      DrawFolderIcon(dc, icon, row.icon, palette.label_dim, false);
    } else if (!row.favicon.empty()) {
      DrawFavicon(dc, icon, row.favicon.data(), row.favicon_width,
                  row.favicon_height, palette.ground_top);
    } else {
      HGDIOBJ previous = caption_font_ ? ::SelectObject(dc, caption_font_)
                                       : nullptr;
      DrawGlyph(dc, icon, L"", palette.label_dim);  // Globe.
      if (previous) {
        ::SelectObject(dc, previous);
      }
    }
    RECT text = {icon.right + Scaled(6), pill.top, pill.right - pad,
                 pill.bottom};
    if (ui_font_) {
      ::SelectObject(dc, ui_font_);
    }
    ::SetTextColor(dc, palette.label);
    PhiDrawTextAA(dc, row.title.c_str(), -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    bar_items_.push_back({pill, index, false});
    x = pill.right + Scaled(8);
    ++shown;
  }
  if (shown < roots.size()) {
    // The Mac's "More Bookmarks" chevron.
    const RECT more = {limit, mid - pill_h / 2, limit + more_w,
                       mid + pill_h / 2};
    HGDIOBJ previous = caption_font_ ? ::SelectObject(dc, caption_font_)
                                     : nullptr;
    DrawGlyph(dc, more, L"", palette.label_dim);  // Chevron right.
    if (previous) {
      ::SelectObject(dc, previous);
    }
    BarItem overflow;
    overflow.bounds = more;
    overflow.bookmark = static_cast<int>(shown);  // First hidden root.
    overflow.overflow = true;
    bar_items_.push_back(overflow);
  }
}

void PhiWindowController::ShowBookmarkBarMenu(const RECT& anchor,
                                              const std::string& parent_guid,
                                              size_t skip_first) {
  if (!host_) {
    return;
  }
  HMENU menu = ::CreatePopupMenu();
  if (!menu) {
    return;
  }
  std::vector<std::pair<std::string, std::string>> targets;  // guid, url.
  const auto build = [&](const auto& self, HMENU into,
                         const std::string& parent, size_t skip,
                         int depth) -> void {
    size_t at = 0;
    for (const PhiStoredRowView& row :
         host_->BookmarkRows(slot_.value, parent)) {
      if (at++ < skip || targets.size() >= 200) {
        continue;
      }
      const std::wstring title =
          base::UTF8ToWide(row.title.empty() ? row.url : row.title);
      if (row.folder) {
        if (depth >= 4) {
          continue;
        }
        HMENU sub = ::CreatePopupMenu();
        if (!sub) {
          continue;
        }
        self(self, sub, row.guid, 0, depth + 1);
        ::AppendMenuW(into, MF_POPUP | MF_STRING,
                      reinterpret_cast<UINT_PTR>(sub), title.c_str());
      } else if (!row.url.empty()) {
        targets.emplace_back(row.guid, row.url);
        ::AppendMenuW(into, MF_STRING, targets.size(), title.c_str());
      }
    }
  };
  build(build, menu, parent_guid, skip_first, 0);
  POINT at = {anchor.left, anchor.bottom};
  ::ClientToScreen(shell_hwnd_, &at);
  const UINT chosen = TrackPopupMenuModal(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y,
      shell_hwnd_);
  ::DestroyMenu(menu);
  if (chosen >= 1 && chosen <= targets.size()) {
    OpenOrFocusPinned(targets[chosen - 1].first, targets[chosen - 1].second);
  }
}

void PhiWindowController::PaintChrome(HDC dc,
                                     const Layout& layout,
                                     bool include_child_controls) {
  // The SLOT's palette (a Space may pin its own theme), ramped source →
  // target while a Space swap slide is in flight.
  const PhiPalette palette = CurrentPalette();
  const COLORREF kTintTop = palette.ground_top;
  const COLORREF kTintBottom = palette.ground_bottom;
  const COLORREF kRowSelectedBg = palette.selected;
  const COLORREF kLabel = palette.label;
  const COLORREF kLabelDim = palette.label_dim;
  const COLORREF kBorder = palette.border;
  // The address field's two fills. In performance the field is a pill in the
  // SIDEBAR, so it rests on the 4% wash over the Space tint (sidebarTabHovered
  // — SideAddressBar's showBackgroundWhenInactive), brightening to 8%. In
  // balanced and comfortable it sits on the TOP BAR, whose ground is
  // windowBackground: the Mac composites the very same translucent overlays
  // there (WebContentAddressBar's .sidebarTabHovered over the header), so the
  // field carries no Space tint in those two. Blending them over the tinted
  // ground instead is what painted a themed pill on the untinted bar.
  const bool kFieldOnBar = layout.mode != PhiLayoutMode::kPerformance;
  const COLORREF kFieldBg = kFieldOnBar ? palette.bar_hover : palette.hover;
  const COLORREF kFieldBgHot =
      kFieldOnBar ? palette.bar_hover_strong : palette.hover_strong;
  // The active pill's 1pt outline exists only in dark mode (white 20%).
  const COLORREF kActivePillBorder =
      palette.dark ? BlendOver(RGB(0xFF, 0xFF, 0xFF), kRowSelectedBg, 51)
                   : kRowSelectedBg;
  // Paint only the bands we own. Whatever is left is the content region, and
  // the reparented browser child covers it. No seam along the content edge:
  // the chrome runs straight into the page.
  // The Space tint, painted over the WHOLE client area: the content card is
  // inset, so the ground shows around it on every side. Banded by hand rather
  // than GradientFill, which lives in msimg32 and is not linked here.
  ::ExcludeClipRect(dc, layout.content.left, layout.content.top,
                    layout.content.right, layout.content.bottom);
  const int height = layout.client.bottom > 0 ? layout.client.bottom : 1;
  const int bands = 48;
  // The band brushes only depend on the palette, not the height — cache them
  // instead of creating and destroying 48 GDI brushes every paint.
  if (band_brushes_.empty() || band_top_ != kTintTop ||
      band_bottom_ != kTintBottom) {
    for (HBRUSH brush : band_brushes_) {
      ::DeleteObject(brush);
    }
    band_brushes_.clear();
    band_brushes_.reserve(bands);
    for (int i = 0; i < bands; ++i) {
      const int r = GetRValue(kTintTop) +
                    (GetRValue(kTintBottom) - GetRValue(kTintTop)) * i / bands;
      const int g = GetGValue(kTintTop) +
                    (GetGValue(kTintBottom) - GetGValue(kTintTop)) * i / bands;
      const int b = GetBValue(kTintTop) +
                    (GetBValue(kTintBottom) - GetBValue(kTintTop)) * i / bands;
      band_brushes_.push_back(::CreateSolidBrush(RGB(r, g, b)));
    }
    band_top_ = kTintTop;
    band_bottom_ = kTintBottom;
  }
  for (int i = 0; i < bands; ++i) {
    RECT band = {0, height * i / bands, layout.client.right,
                 height * (i + 1) / bands};
    ::FillRect(dc, &band, band_brushes_[i]);
  }

  ::SelectClipRgn(dc, nullptr);

  // The card's hairline: 1px of `border` just outside the content region
  // (the Mac's contentOuterBorder), ROUNDED to follow the card's corners —
  // a square frame would cut straight across the corner arcs. Under a top
  // bar the card begins at the bar's top edge, so the outline wraps both.
  if (layout.content.right > layout.content.left &&
      layout.mode != PhiLayoutMode::kComfortable) {
    // Comfortable has no card to outline: its page runs edge to edge and
    // the toolbar's own separator is the only rule above it.
    RECT frame_rc = layout.content;
    if (layout.top_bar.right > layout.top_bar.left) {
      frame_rc.top = layout.top_bar.top;
    }
    ::InflateRect(&frame_rc, 1, 1);
    // The frame sits 1px outside the card, so its corner circle is the
    // card's (the caps' circle) grown by 1 — concentric, and the very
    // circle each cap strokes inside its bite.
    PhiStrokeRoundRectAA(dc, frame_rc, Scaled(kContentRadius) + 1,
                         palette.border, 1);
  }

  PaintBookmarkBar(dc, layout, palette);

  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = ui_font_ ? ::SelectObject(dc, ui_font_) : nullptr;

  // The caption band: menu titles leading, minimize / maximize / close
  // trailing — the menu bar ON the title row. The ground already painted it;
  // only the titles, glyphs and hot states go on top.
  if (layout.title_bar.right > layout.title_bar.left) {
    // The logo is loaded AT the size it draws (the multi-size ico has exact
    // entries per DPI), so no GDI rescale blurs it.
    const int logo_size = Scaled(18);
    const std::vector<unsigned char>& logo_bgra = PhiLogoBgra256();
    const int logo_native = 256;
    if (logo_bgra.empty()) {
      // Fallback: the nearest ICO entry, held per window at the size this
      // DPI draws — a process static loaded at the first window's DPI drew
      // blurred (and leaked) on every other display.
      if (logo_icon_ && logo_icon_size_ != logo_size) {
        ::DestroyIcon(logo_icon_);
        logo_icon_ = nullptr;
      }
      if (!logo_icon_) {
        logo_icon_ = PhiAppIcon(logo_size);
        logo_icon_size_ = logo_size;
      }
    }
    const HICON logo = logo_icon_;
    // One draw for both callers: the Phi title's icon, and comfortable's
    // standalone Phi button.
    const auto draw_logo = [&](int logo_x, int logo_y) {
      RECT logo_rc = {logo_x, logo_y, logo_x + logo_size,
                      logo_y + logo_size};
      const bool drew =
          !logo_bgra.empty() &&
          PhiDrawBgra(dc, logo_rc, logo_bgra.data(), logo_native,
                      logo_native);
      if (!drew && logo) {
        ::DrawIconEx(dc, logo_x, logo_y, logo, logo_size, logo_size, 0,
                     nullptr, DI_NORMAL);
      }
    };
    // Comfortable draws no titles — the tab strip owns the band — so the Phi
    // menu shows as the logo alone in the leading slot, with the same hover
    // wash the band's other buttons take. (Its index in the hover table.)
    if (layout.phi_menu.right > layout.phi_menu.left) {
      if (hover_kind_ == HoverKind::kNav && hover_index_ == 12) {
        FillRounded(dc, layout.phi_menu, palette.hover, Scaled(4));
      }
      draw_logo(
          (layout.phi_menu.left + layout.phi_menu.right - logo_size) / 2,
          (layout.phi_menu.top + layout.phi_menu.bottom - logo_size) / 2);
    }
    const size_t titles = VisibleMenuTitles(layout);
    for (size_t i = 0; i < titles; ++i) {
      const RECT& title_rc = menu_titles_[i].second;
      if (static_cast<int>(i) == hot_menu_) {
        RECT hot_rc = title_rc;
        ::InflateRect(&hot_rc, 0, -Scaled(4));
        FillRounded(dc, hot_rc, palette.hover, Scaled(4));
      }
      ::SetTextColor(dc, kLabel);
      RECT text_rc = title_rc;
      if (i == 0) {
        // The Phi button: icon leading, label after — one hit target.
        draw_logo(title_rc.left + Scaled(kPhiTitlePad),
                  (title_rc.bottom + title_rc.top - logo_size) / 2);
        text_rc.left += Scaled(kPhiTitlePad) + logo_size + Scaled(6);
        PhiDrawTextAA(dc, menu_titles_[i].first.c_str(), -1, &text_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        continue;
      }
      PhiDrawTextAA(dc, menu_titles_[i].first.c_str(), -1, &text_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // The seam between the app's band controls and the window's. It draws in
    // the TERTIARY ink (30%), not the 6% separator the band's own bottom
    // edge uses: a hairline that short, standing alone in a gap rather than
    // running the window's width, disappears at 6%.
    if (layout.band_divider.right > layout.band_divider.left) {
      HBRUSH divider_brush = ::CreateSolidBrush(palette.label_tertiary);
      ::FillRect(dc, &layout.band_divider, divider_brush);
      ::DeleteObject(divider_brush);
    }
    // Hot fills first (close turns the Win11 red), then the MDL2 glyphs.
    if (hot_caption_ == HTMINBUTTON || hot_caption_ == HTMAXBUTTON) {
      const RECT& hot_rc = hot_caption_ == HTMINBUTTON ? layout.caption_min
                                                       : layout.caption_max;
      HBRUSH hover_brush = ::CreateSolidBrush(palette.hover);
      ::FillRect(dc, &hot_rc, hover_brush);
      ::DeleteObject(hover_brush);
    } else if (hot_caption_ == HTCLOSE) {
      HBRUSH close_brush = ::CreateSolidBrush(RGB(0xC4, 0x2B, 0x1C));
      ::FillRect(dc, &layout.caption_close, close_brush);
      ::DeleteObject(close_brush);
    }
    HGDIOBJ previous_caption =
        caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
    const bool zoomed = shell_hwnd_ && ::IsZoomed(shell_hwnd_);
    // Comfortable's band carries the tab strip, and everything on it sits on
    // the STRIP's line rather than the band's — the caption glyphs follow, so
    // the trio reads level with search-tabs next to it. Only the glyphs move:
    // the buttons keep the full band height, which is the Win11 hit target
    // (and the hover wash) whatever the glyph inside is doing.
    const int glyph_drop =
        layout.mode == PhiLayoutMode::kComfortable
            ? (layout.tabs_top + layout.title_bar.bottom) / 2 -
                  (layout.title_bar.top + layout.title_bar.bottom) / 2
            : 0;
    const auto glyph_rc = [&](const RECT& rc) {
      return RECT{rc.left, rc.top + glyph_drop, rc.right,
                  rc.bottom + glyph_drop};
    };
    DrawGlyph(dc, glyph_rc(layout.caption_min), L"\uE921", kLabel);
    DrawGlyph(dc, glyph_rc(layout.caption_max),
              zoomed ? L"\uE923" : L"\uE922", kLabel);
    DrawGlyph(dc, glyph_rc(layout.caption_close), L"\uE8BB",
              hot_caption_ == HTCLOSE ? RGB(0xFF, 0xFF, 0xFF) : kLabel);
    if (previous_caption) {
      ::SelectObject(dc, previous_caption);
    }
  }

  // The top bar (balanced / comfortable): the content card's FIRST band -- the
  // Mac's WebContentHeader is the card's first stack item -- so it paints on
  // the card's own ground (windowBackground) with the card's rounded top
  // corners, and a 1pt separator under it running into the page below.
  if (layout.top_bar.right > layout.top_bar.left) {
    const RECT& band = layout.top_bar;
    const int radius = Scaled(kContentRadius);
    HBRUSH bg = ::CreateSolidBrush(palette.window_bg);
    if (layout.mode == PhiLayoutMode::kComfortable) {
      // Edge to edge and square: there is no card here, and the band's top
      // corners belong to the tabs merging into it, not to a rounded card.
      ::FillRect(dc, &band, bg);
    } else {
      // The card's top corners at kContentRadius — the same curve its
      // hairline and its bottom corner caps use, so the arcs meet.
      PhiFillRoundRectAA(dc, band, radius, palette.window_bg);
      RECT lower = {band.left, band.top + radius, band.right, band.bottom};
      ::FillRect(dc, &lower, bg);
    }
    ::DeleteObject(bg);
    RECT sep = {band.left, band.bottom - 1, band.right, band.bottom};
    HBRUSH sep_brush = ::CreateSolidBrush(palette.separator);
    ::FillRect(dc, &sep, sep_brush);
    ::DeleteObject(sep_brush);
  }

  // Nav row: MDL2 icons at the 13pt icon size \u2014 the UI font's \u2039 \u203A \u21BB were
  // punctuation-sized. Hovering an actionable button raises a rounded
  // wash, the Mac header buttons' treatment. Each mode empties the rects
  // it hides, so nothing draws where a control does not exist. (The
  // sidebar-toggle stand-in is gone: it only opened stray windows until
  // the real toggle lands.)
  {
    HGDIOBJ previous_nav_font =
        chip_icon_font_ ? ::SelectObject(dc, chip_icon_font_) : nullptr;
    struct NavButton {
      RECT rect;
      const wchar_t* glyph;
      bool enabled;
    };
    const std::array<NavButton, 6> nav_buttons = {{
        {layout.back, L"\uE72B", can_go_back_},
        {layout.forward, L"\uE72A", can_go_forward_},
        {layout.reload, nav_loading_ ? L"\uE711" : L"\uE72C", true},
        {layout.search_tabs, L"\uE721", true},
        // Comfortable only: the strip's "+" and the toolbar's overflow.
        {add_tab_button_, L"\uE710", true},
        {layout.overflow, L"\uE712", true},
    }};
    for (size_t i = 0; i < nav_buttons.size(); ++i) {
      const NavButton& button = nav_buttons[i];
      if (button.rect.right <= button.rect.left) {
        continue;  // Hidden in this mode.
      }
      const bool hot = hover_kind_ == HoverKind::kNav &&
                       hover_index_ == static_cast<int>(i);
      if (hot && button.enabled) {
        // The Mac header buttons: cornerRadius 4 on the plain hover wash.
        FillRounded(dc, button.rect, palette.hover, Scaled(4));
      }
      DrawIconGlyph(dc, button.rect, button.glyph, Scaled(kSpaceIcon),
                    button.enabled ? kLabel : kLabelDim);
    }
    if (previous_nav_font) {
      ::SelectObject(dc, previous_nav_font);
    }
  }

  // Address field. In the sidebar it is a pill on the 4% wash with no border
  // (SideAddressBar); in the top bar it is bare -- transparent until hovered on
  // Mac -- so only the EDIT's text shows there. The capture path (no children
  // rendered) draws the text itself so a screenshot still shows the address.
  // The in-field trailing stack (reader, copy link, extensions menu) sits ON
  // the address field, which already wears the resting 4% wash — so a button
  // there takes the EMPHASIZED one, or it paints itself invisible. And while
  // the pointer is on one of them the field drops back to resting, so what
  // reads as lit is the button rather than the whole bar: the Mac lights the
  // button's own circle, not the address bar behind it.
  // The EDIT must give the accessories their room back the moment one
  // appears; nothing else re-fits it between layout changes.
  PositionUrlField(layout);
  const auto on_field = [&](const RECT& r) {
    return layout.url_field.right > layout.url_field.left && r.right > r.left &&
           r.left >= layout.url_field.left && r.right <= layout.url_field.right;
  };
  const auto accessory_hot = [&](int index, const RECT& r) {
    return hover_kind_ == HoverKind::kNav && hover_index_ == index &&
           r.right > r.left;
  };
  const auto accessory_wash = [&](const RECT& r) {
    // Inside the capsule the button lights the field's own emphasized fill;
    // out on the bar it takes the bar's plain wash, like the nav buttons.
    return on_field(r) ? kFieldBgHot : palette.hover;
  };
  const bool pointer_on_field_accessory =
      (accessory_hot(13, layout.reader) && on_field(layout.reader)) ||
      (accessory_hot(11, layout.copy_link) && on_field(layout.copy_link)) ||
      (accessory_hot(10, layout.extensions) && on_field(layout.extensions));
  const bool field_hot = url_field_hover_ && !pointer_on_field_accessory;

  if (layout.mode == PhiLayoutMode::kPerformance) {
    // Resting 4% wash, brightening to the emphasized 8% under the cursor
    // (SideAddressBar's hoveredColor).
    FillRounded(dc, layout.url_field, field_hot ? kFieldBgHot : kFieldBg,
                Scaled(kCornerRadius));
  } else if (layout.mode == PhiLayoutMode::kComfortable) {
    // Chrome's omnibox: a full pill standing on the toolbar, on the same 4%
    // wash the sidebar field uses, brightening under the cursor. Balanced's
    // field stays bare — it has the card's own ground behind it; this one
    // has only the flat toolbar, and would vanish into it.
    const int pill_r = (layout.url_field.bottom - layout.url_field.top) / 2;
    FillRounded(dc, layout.url_field, field_hot ? kFieldBgHot : kFieldBg,
                pill_r);
    // The page's globe leads the pill, where Chrome puts its site chip.
    if (layout.page_icon.right > layout.page_icon.left) {
      HGDIOBJ previous_icon_font =
          caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
      DrawGlyph(dc, layout.page_icon, L"\uE774", kLabelDim);
      if (previous_icon_font) {
        ::SelectObject(dc, previous_icon_font);
      }
    }
  } else if (field_hot &&
             layout.url_field.right > layout.url_field.left) {
    // The top-bar field rests BARE and takes the wash only under the
    // cursor (the Mac top bar's transparent-until-hovered field).
    FillRounded(dc, layout.url_field, kFieldBg, Scaled(kCornerRadius));
  }
  if (include_child_controls) {
    std::wstring shown = nav_url_;
    if (url_field_) {
      const int len = ::GetWindowTextLengthW(url_field_);
      std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
      if (len > 0) {
        ::GetWindowTextW(url_field_, buffer.data(), len + 1);
      }
      shown.assign(buffer.data());  // What the user actually sees, mid-edit.
    }
    ::SetTextColor(dc, shown.empty() ? kLabelDim : kLabel);
    RECT url_rc = layout.url_field;
    url_rc.left += Scaled(12);  // SideAddressBar.textFieldLeadingInset
    // Stop before the in-field trailing stack when it exists — the leftmost
    // accessory that actually sits inside the capsule (see PositionUrlField).
    int text_right = url_rc.right;
    for (const RECT& accessory : {layout.field_ext_strip, layout.copy_link,
                                  layout.extensions}) {
      if (accessory.right > accessory.left &&
          accessory.left > layout.url_field.left &&
          accessory.left < text_right) {
        text_right = accessory.left;
      }
    }
    url_rc.right = text_right - Scaled(8);
    PhiDrawTextAA(dc, shown.empty() ? L"Search or Enter URL" : shown.c_str(),
                -1, &url_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
  }
  // The pinned-extension icons leading the field's trailing stack: 16pt
  // icons in 24pt slots on the radius-4 hover wash (SideAddressBar's
  // extensionIconsStackView).
  if (layout.field_ext_strip.right > layout.field_ext_strip.left) {
    const COLORREF field_fill = url_field_hover_ ? kFieldBgHot : kFieldBg;
    for (size_t i = 0; i < field_extensions_.size(); ++i) {
      const RECT slot = FieldExtRect(layout, i);
      if (slot.right <= slot.left) {
        continue;
      }
      if (hover_kind_ == HoverKind::kFieldExt &&
          hover_index_ == static_cast<int>(i)) {
        FillRounded(dc, slot, palette.hover, Scaled(4));
      }
      const FieldExtension& ext = field_extensions_[i];
      const int mid_x = (slot.left + slot.right) / 2;
      const int mid_y = (slot.top + slot.bottom) / 2;
      RECT icon = {mid_x - Scaled(8), mid_y - Scaled(8), mid_x + Scaled(8),
                   mid_y + Scaled(8)};
      if (!ext.icon.empty()) {
        DrawFavicon(dc, icon, ext.icon.data(), ext.icon_w, ext.icon_h,
                    field_fill);
      } else {
        // Icon not decoded yet: the puzzle stands in.
        HGDIOBJ previous_ext_font =
            chip_icon_font_ ? ::SelectObject(dc, chip_icon_font_) : nullptr;
        DrawGlyph(dc, slot, L"", kLabelDim);
        if (previous_ext_font) {
          ::SelectObject(dc, previous_ext_font);
        }
      }
    }
  }
  // The field's in-field copy-link button (SideAddressBar's rightStack lead):
  // a 12pt link glyph on a radius-4 hover wash; a checkmark for the second
  // after a copy, the Mac's triggered swap.
  if (layout.reader.right > layout.reader.left) {
    // No icon font to select: this one is drawn, not typeset (see
    // DrawReaderGlyph).
    // Washed on hover, and held lit while the reader is UP — the Mac's
    // ReaderButtonView shows its circle for `isButtonHovering || isActive`.
    if (accessory_hot(13, layout.reader) || ActiveTabShowsReader()) {
      FillRounded(dc, layout.reader, accessory_wash(layout.reader), Scaled(4));
    }
    DrawReaderGlyph(dc, layout.reader, kLabel, dpi_);
  }
  if (layout.copy_link.right > layout.copy_link.left) {
    HGDIOBJ previous_link_font =
        chip_icon_font_ ? ::SelectObject(dc, chip_icon_font_) : nullptr;
    if (accessory_hot(11, layout.copy_link)) {
      FillRounded(dc, layout.copy_link, accessory_wash(layout.copy_link),
                  Scaled(4));
    }
    DrawGlyph(dc, layout.copy_link, copy_link_flash_ ? L"" : L"",
              kLabel);
    if (previous_link_font) {
      ::SelectObject(dc, previous_link_font);
    }
  }

  // Space pips: 24pt, icon only. The selected one sits on a selected-pill
  // card (SpacesStripView's matchedGeometry indicator); the trailing "+"
  // creates a Space.
  // The selected pill first, as a background layer (the Mac's strip-level
  // glass chip sits BEHIND the pips): mid-switch it slides across
  // neighboring pips' rects on the band slide's clock, and their glyphs
  // must draw over it, not vanish under it. The pips themselves stay put.
  // The hairline stands in for the Mac chip's drop shadow — without it the
  // white pill disappears on a white ground.
  // An overflowing row paints through the viewport clip: the peeked
  // neighbours show partly, the cue that the row continues.
  const int chips_clip_save = ::SaveDC(dc);
  if (chips_clip_.right > chips_clip_.left) {
    ::IntersectClipRect(dc, chips_clip_.left, layout.spaces_strip.top,
                        chips_clip_.right, layout.spaces_strip.bottom);
  }
  for (const SpaceChip& chip : chips_) {
    // A watched agent pip wears the selection pill too, so "which agent am I
    // watching" reads at a glance.
    if (chip.id != active_space_ && !(chip.is_agent && chip.open)) {
      continue;
    }
    if (creating_space_ && !chip.is_agent) {
      // The dashed placeholder is the Space in focus while the form is up;
      // the source Space must not keep reading as active beside it.
      continue;
    }
    RECT pill = PaintedChip(chip.bounds, Scaled(kSpaceChip));
    if (!chip.is_agent && swap_start_ &&
        swap_chip_from_.right > swap_chip_from_.left) {
      const double t = SwapEasedProgress();
      const auto mix = [t](LONG from, LONG to) {
        return from + static_cast<LONG>(std::lround((to - from) * t));
      };
      const RECT from = PaintedChip(swap_chip_from_, Scaled(kSpaceChip));
      const RECT to = PaintedChip(chip.bounds, Scaled(kSpaceChip));
      pill.left = mix(from.left, to.left);
      pill.top = mix(from.top, to.top);
      pill.right = mix(from.right, to.right);
      pill.bottom = mix(from.bottom, to.bottom);
    }
    StrokeRounded(dc, pill, kRowSelectedBg,
                  palette.dark ? kActivePillBorder : kBorder,
                  Scaled(kSpaceCardRadius) * 2);
  }
  for (const SpaceChip& chip : chips_) {
    const bool active = (chip.id == active_space_);
    if (!active && hover_kind_ == HoverKind::kChip &&
        hover_index_ == static_cast<int>(&chip - chips_.data())) {
      // The Mac's dim hover wash on an inactive pip (sidebarTabHovered).
      FillRounded(dc, PaintedChip(chip.bounds, Scaled(kSpaceChip)),
                  palette.hover, Scaled(kSpaceCardRadius) * 2);
    }
    // Every pip's icon is the label colour, active or not — the Mac tints
    // them all Color.primary and lets the selected chip alone say which one
    // is current. Only the trailing slot's "+" / "…" is secondary.
    ::SetTextColor(dc, kLabel);
    // The vertical layouts' pip fills the strip band; its glyph grows with
    // it, at the same proportion the 24pt pip gives its own.
    const bool big_pip = layout.chip_size >= kSpaceChipVertical;
    const int icon_pt = big_pip ? kSpaceIconVertical : kSpaceIcon;
    // The glyph box is INSET FROM THE PAINTED CHIP, not from the cell: the
    // colour pass repaints its whole rect from a pixel sampled at the
    // rect's corner, so a box reaching the chip's own corners would sample
    // the ground tint outside the rounded corner and clear the card away —
    // and, where it did survive, square off the corners it covered. 3pt
    // keeps the sample inside the radius-8 arc with room for the icon.
    RECT glyph_rc = PaintedChip(chip.bounds, Scaled(kSpaceChip));
    ::InflateRect(&glyph_rc, -Scaled(3), -Scaled(3));
    // Colour emoji take the same box: the colour pass centres their INK in
    // it, so the old hand-tuned lift (they ride low in their line box) is
    // gone — it left the repainted patch sitting higher than the pip it
    // belongs to.
    if (!chip.icon.empty() &&
        PhiDrawCatalogIcon(dc, glyph_rc, chip.icon, Scaled(icon_pt),
                           kLabel)) {
      // The Mac catalog's own artwork, tinted like the glyph would be.
    } else if (!chip.icon.empty() && !PhiIsPrivateGlyph(chip.icon) &&
        PhiDrawColorIcon(dc, glyph_rc, chip.icon, Scaled(icon_pt),
                         kLabel)) {
      // Emoji rendered in COLOUR via DirectWrite; GDI below is the fallback.
    } else if (!chip.icon.empty() &&
               (emoji_font_ || chip_icon_font_)) {
      // A private-use glyph is one of the Phi icon catalog's MDL2 stand-ins
      // (one step above the emoji, so the two align); everything else emoji.
      // Each face comes in both pip sizes; the small one stands in if the
      // big one failed to build.
      HFONT icon_font =
          PhiIsPrivateGlyph(chip.icon)
              ? (big_pip && pip_icon_font_ ? pip_icon_font_ : chip_icon_font_)
              : (big_pip && pip_emoji_font_ ? pip_emoji_font_ : emoji_font_);
      HGDIOBJ previous_icon_font =
          icon_font ? ::SelectObject(dc, icon_font) : nullptr;
      PhiDrawTextAA(dc, chip.icon.c_str(), -1, &glyph_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (previous_icon_font) {
        ::SelectObject(dc, previous_icon_font);
      }
    } else {
      // No icon: the name's first character stands in (the pip has no room
      // for the name itself). It rides the UI font, so the big pip needs the
      // strip's own text font to keep pace with its glyphs.
      const std::wstring initial =
          chip.label.empty() ? std::wstring(L"?") : chip.label.substr(0, 1);
      HGDIOBJ previous_initial_font =
          big_pip && pip_text_font_ ? ::SelectObject(dc, pip_text_font_)
                                    : nullptr;
      PhiDrawTextAA(dc, initial.c_str(), -1, const_cast<RECT*>(&chip.bounds),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (previous_initial_font) {
        ::SelectObject(dc, previous_initial_font);
      }
    }
  }
  ::RestoreDC(dc, chips_clip_save);
  if (creating_space_ &&
      creating_pip_bounds_.right > creating_pip_bounds_.left) {
    // The Mac's creatingSpacePlaceholderPip: a dashed outline where the new
    // Space will land, carrying the form's currently selected icon.
    const RECT pip = PaintedChip(creating_pip_bounds_, Scaled(kSpaceChip));
    PhiStrokeRoundRectDashedAA(dc, pip, Scaled(kCornerRadius),
                               BlendOver(kLabelDim, palette.ground_top, 166),
                               1, Scaled(3), Scaled(2));
    if (!creating_space_icon_text_.empty()) {
      // Sized like a real pip's icon, through the same ink-fitting draws:
      // the old fixed-point artwork/text path rendered the chosen icon
      // visibly smaller than the pips it stands among.
      const bool big_pip = layout.chip_size >= kSpaceChipVertical;
      const int icon_pt = big_pip ? kSpaceIconVertical : kSpaceIcon;
      RECT glyph_rc = pip;
      ::InflateRect(&glyph_rc, -Scaled(3), -Scaled(3));
      if (PhiDrawCatalogIcon(dc, glyph_rc, creating_space_icon_text_,
                             Scaled(icon_pt), kLabel)) {
        // The catalog's own artwork.
      } else if (!PhiIsPrivateGlyph(creating_space_icon_text_) &&
                 PhiDrawColorIcon(dc, glyph_rc, creating_space_icon_text_,
                                  Scaled(icon_pt), kLabel)) {
        // Colour emoji.
      } else if (emoji_font_ || chip_icon_font_) {
        HFONT pip_icon_font =
            PhiIsPrivateGlyph(creating_space_icon_text_)
                ? (big_pip && pip_icon_font_ ? pip_icon_font_
                                             : chip_icon_font_)
                : (big_pip && pip_emoji_font_ ? pip_emoji_font_
                                              : emoji_font_);
        HGDIOBJ previous_pip_font =
            pip_icon_font ? ::SelectObject(dc, pip_icon_font) : nullptr;
        ::SetTextColor(dc, kLabel);
        PhiDrawTextAA(dc, creating_space_icon_text_.c_str(), -1, &glyph_rc,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (previous_pip_font) {
          ::SelectObject(dc, previous_pip_font);
        }
      }
    }
  }
  if (!incognito_ && layout.add_space.right > layout.add_space.left) {
    // The Mac trailing slot: "+" creates; on overflow it becomes the "…"
    // switcher (moreButton) since not every pip is visible. The "+" is up
    // only while the pointer is over the strip's row (addButton's opacity
    // rides slot.isStripRowHovered, 0.15s easeInOut); the "…" always is.
    // Whichever shows wears the pips' hover wash (radius 6) under the
    // cursor. GDI has no opacity: the fade blends toward the ground the
    // slot sits on, sampled from its corner — nothing else paints there.
    const double reveal = chips_overflow_ ? 1.0 : AddSpaceRevealProgress();
    if (reveal > 0.0) {
      COLORREF ground =
          ::GetPixel(dc, layout.add_space.left, layout.add_space.top);
      if (ground == CLR_INVALID) {
        ground = palette.ground_top;
      }
      const int alpha = static_cast<int>(std::lround(reveal * 255.0));
      if (hover_kind_ == HoverKind::kAddSpace) {
        FillRounded(dc, layout.add_space,
                    BlendOver(palette.hover, ground, alpha), Scaled(6));
      }
      const COLORREF slot_ink = BlendOver(kLabelDim, ground, alpha);
      if (chips_overflow_) {
        // The Mac's moreButton is SF Symbols' `ellipsis` — three ROUND dots
        // at the icon size. A text "…" is not that: its dots are small,
        // square-cornered and sit on the baseline, which reads as a typo
        // beside the icons. Three circles, 3pt across on a 5pt pitch,
        // centred in the slot.
        const int dot = Scaled(3);
        const int pitch = Scaled(5);
        const int mid_x = (layout.add_space.left + layout.add_space.right) / 2;
        const int mid_y = (layout.add_space.top + layout.add_space.bottom) / 2;
        for (int i = -1; i <= 1; ++i) {
          const RECT d = {mid_x + i * pitch - dot / 2, mid_y - dot / 2,
                          mid_x + i * pitch - dot / 2 + dot,
                          mid_y - dot / 2 + dot};
          PhiFillEllipseAA(dc, d, slot_ink);
        }
      } else {
        HGDIOBJ previous_slot_font =
            layout.chip_size >= kSpaceChipVertical && pip_text_font_
                ? ::SelectObject(dc, pip_text_font_)
                : nullptr;
        DrawGlyph(dc, layout.add_space, L"+", slot_ink);
        if (previous_slot_font) {
          ::SelectObject(dc, previous_slot_font);
        }
      }
    }
  }

  PaintBandOrSwipe(dc, layout, palette);

  // Bottom bar — SidebarBottomBar, inch for inch: a 24pt row where the
  // download and memory icon buttons lead (24pt, rounded-6 hover wash),
  // and the Feedback capsule (16pt flag + 11pt label, hairline border,
  // hover wash) and Chat capsule (accent ground that darkens on hover;
  // white glyph flush LEFT, 2pt gap, 11pt white label, 8pt trailing pad)
  // trail. Functions arrive later; there is NO settings gear here —
  // settings opens from the menu bar, as on Mac.
  const auto bottom_hot = [&](int ordinal) {
    return hover_kind_ == HoverKind::kNav && hover_index_ == 4 + ordinal;
  };
  {
    HGDIOBJ previous_icon_font =
        chip_icon_font_ ? ::SelectObject(dc, chip_icon_font_) : nullptr;
    if (layout.downloads.right > layout.downloads.left) {
      if (bottom_hot(0)) {
        FillRounded(dc, layout.downloads, palette.hover, Scaled(6));
      }
      // The Mac's download icon (sidebar-download.imageset), textPrimary
      // as DownloadButtonView tints its Lottie.
      const int down_px = layout.downloads.right - layout.downloads.left;
      const std::vector<unsigned char> down_bgra =
          PhiTintedIconBgra(PhiDownloadIcon(), kLabel, down_px);
      PhiBlendBgra(dc, layout.downloads, down_bgra.data(), down_px,
                   down_px);
    }
    if (layout.memory.right > layout.memory.left) {
      if (bottom_hot(1)) {
        FillRounded(dc, layout.memory, palette.hover, Scaled(6));
      }
      // The Mac's memory icon (memory-icon.imageset), black/white by
      // appearance as its two PDF variants are.
      const int memory_px = layout.memory.right - layout.memory.left;
      const std::vector<unsigned char> memory_bgra = PhiTintedIconBgra(
          PhiMemoryIcon(),
          palette.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00),
          memory_px);
      PhiBlendBgra(dc, layout.memory, memory_bgra.data(), memory_px,
                   memory_px);
    }
    if (layout.extensions.right > layout.extensions.left) {
      // The extensions puzzle button (nav-hover ordinal 8 = bottom_hot(4)).
      // In-field (performance) it wears the header buttons' radius-4 wash;
      // in the bottom bar (balanced) the bar buttons' radius 6.
      if (bottom_hot(4)) {
        FillRounded(dc, layout.extensions, palette.hover,
                    Scaled(layout.mode == PhiLayoutMode::kPerformance ? 4
                                                                      : 6));
      }
      DrawGlyph(dc, layout.extensions, L"", kLabel);  // Puzzle.
    }
    if (previous_icon_font) {
      ::SelectObject(dc, previous_icon_font);
    }
  }
  if (layout.feedback.right > layout.feedback.left) {
    // ViewThatFits: the bottom bar's narrow slot takes the icon-only
    // variant; the comfortable header keeps icon + label.
    const bool icon_only = layout.feedback.right - layout.feedback.left <=
                           Scaled(kFeedbackIconWidth);
    if (bottom_hot(2)) {
      PhiFillCapsuleAA(dc, layout.feedback, palette.hover);
    }
    PhiStrokeCapsuleAA(dc, layout.feedback, kBorder, std::max(1, Scaled(1)));
    HGDIOBJ previous_small =
        small_font_ ? ::SelectObject(dc, small_font_) : nullptr;
    const int icon_w = Scaled(16);
    int group_w = icon_w;
    RECT measure = {0, 0, 0, 0};
    if (!icon_only) {
      PhiDrawTextAA(dc, L"Feedback", -1, &measure,
                  DT_CALCRECT | DT_SINGLELINE);
      group_w += Scaled(4) + measure.right;
    }
    const int left =
        (layout.feedback.left + layout.feedback.right - group_w) / 2;
    const int mid_y = (layout.feedback.top + layout.feedback.bottom) / 2;
    RECT icon_rc = {left, mid_y - icon_w / 2, left + icon_w,
                    mid_y + icon_w / 2};
    // The Mac's feedback icon (sidebar-feedback.imageset), textPrimary.
    const std::vector<unsigned char> feedback_bgra =
        PhiTintedIconBgra(PhiFeedbackIcon(), kLabel, icon_w);
    PhiBlendBgra(dc, icon_rc, feedback_bgra.data(), icon_w, icon_w);
    if (!icon_only) {
      if (small_font_) {
        ::SelectObject(dc, small_font_);
      }
      ::SetTextColor(dc, kLabel);
      RECT text_rc = {icon_rc.right + Scaled(4), layout.feedback.top,
                      layout.feedback.right, layout.feedback.bottom};
      PhiDrawTextAA(dc, L"Feedback", -1, &text_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    if (previous_small) {
      ::SelectObject(dc, previous_small);
    }
  }
  if (layout.chat.right > layout.chat.left) {
    // themeColorOnHover: the accent, darkened.
    const COLORREF chat_bg =
        bottom_hot(3) ? BlendOver(RGB(0x00, 0x00, 0x00), palette.accent, 40)
                      : palette.accent;
    PhiFillCapsuleAA(dc, layout.chat, chat_bg);
    // 22pt glyph box flush left (ChatButton's leadingPadding 0).
    const int icon_box = Scaled(22);
    const int mid = (layout.chat.top + layout.chat.bottom) / 2;
    RECT icon_rc = {layout.chat.left + Scaled(1), mid - icon_box / 2,
                    layout.chat.left + Scaled(1) + icon_box,
                    mid + icon_box / 2};
    // The Mac's chat icon (sidebar-chat.imageset), template white.
    const std::vector<unsigned char> chat_bgra =
        PhiTintedIconBgra(PhiChatIcon(), RGB(0xFF, 0xFF, 0xFF), icon_box);
    PhiBlendBgra(dc, icon_rc, chat_bgra.data(), icon_box, icon_box);
    HGDIOBJ previous_small =
        small_font_ ? ::SelectObject(dc, small_font_) : nullptr;
    ::SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    RECT chat_rc = {icon_rc.right + Scaled(2), layout.chat.top,
                    layout.chat.right - Scaled(8), layout.chat.bottom};
    PhiDrawTextAA(dc, L"Chat", -1, &chat_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (previous_small) {
      ::SelectObject(dc, previous_small);
    }
  }

  // The Space hover card floats over the window as a popup, the way the
  // Mac's panel does; it is measured with this DC's font.
  SyncHoverCardPopup(dc, layout);

  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

// A hovered pip's info card (the Mac's SpaceHoverCard): the Space as a
// tinted capsule (icon + name), the bound profile, and the Ctrl+N switch
// shortcut as keycaps, floating just above the pip on a bordered card. The
// header above the strip is all shell-drawn — no child window clips it —
// which is why the card anchors UPWARD, like the Mac panel.
bool PhiWindowController::MeasureHoverCard(HDC dc,
                                           const Layout& layout,
                                           HoverCardGeometry* out) {
  if (hover_card_space_.empty() || drag_active_) {
    return false;
  }
  const SpaceChip* chip = nullptr;
  for (const SpaceChip& c : chips_) {
    if (c.id == hover_card_space_) {
      chip = &c;
      break;
    }
  }
  if (!chip) {
    return false;  // The Space left the strip; the next hover change prunes state.
  }
  HoverCardGeometry g;
  g.icon = chip->icon;
  g.name = chip->label.empty() ? L"Space" : chip->label;
  g.profile = card_profile_.empty() ? L"Default" : card_profile_;

  // Measure the two labels in the card's (small) font, clamped so the card
  // always fits the window.
  HGDIOBJ previous_font =
      small_font_ ? ::SelectObject(dc, small_font_) : nullptr;
  const auto measure = [&](const std::wstring& text, int max_w) {
    RECT rc = {0, 0, 0, 0};
    PhiDrawTextAA(dc, text.c_str(), -1, &rc,
                DT_CALCRECT | DT_SINGLELINE | DT_LEFT);
    return std::min(static_cast<int>(rc.right), max_w);
  };
  // A compact take on SpaceHoverCard's metrics: an 18pt capsule with 6/3
  // padding around a 12pt icon and the name; 6pt between sections; 15pt
  // keycaps with 2pt gaps; 8/5 card padding.
  g.pill_h = Scaled(18);
  g.dot_w = Scaled(5);
  g.gap = Scaled(6);
  if (card_shortcut_ > 0) {
    g.keycaps.push_back(L"Ctrl");
    g.keycaps.push_back(std::to_wstring(card_shortcut_));
  }
  for (const std::wstring& token : g.keycaps) {
    const int w = std::max(Scaled(15), measure(token, Scaled(36)) + Scaled(6));
    g.key_widths.push_back(w);
    g.keys_w += (g.keys_w ? Scaled(2) : 0) + w;
  }

  // The popup floats over the whole window, content child included, so
  // only the window's own width bounds it. Shrink the name first, then the
  // profile — never the keycaps.
  const int band_right = static_cast<int>(layout.client.right) - Scaled(4);
  int name_w = measure(g.name, Scaled(100));
  int profile_w = measure(g.profile, Scaled(72));
  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
  const int fixed_w = 2 * Scaled(6) + Scaled(12) + Scaled(4) +  // Pill chrome.
                      g.gap + g.dot_w + g.gap +
                      (g.keys_w ? g.gap + g.dot_w + g.gap + g.keys_w : 0);
  const int avail = band_right - Scaled(4) - 2 * Scaled(8);
  int overflow = fixed_w + name_w + profile_w - avail;
  if (overflow > 0 && name_w > Scaled(44)) {
    const int take = std::min(overflow, name_w - Scaled(44));
    name_w -= take;
    overflow -= take;
  }
  if (overflow > 0 && profile_w > Scaled(32)) {
    profile_w -= std::min(overflow, profile_w - Scaled(32));
  }
  g.name_w = name_w;
  g.profile_w = profile_w;
  g.pill_w = 2 * Scaled(6) + Scaled(12) + Scaled(4) + name_w;
  int content_w = g.pill_w + g.gap + g.dot_w + g.gap + profile_w;
  if (g.keys_w) {
    content_w += g.gap + g.dot_w + g.gap + g.keys_w;
  }
  const int card_w = content_w + 2 * Scaled(8);
  const int card_h = g.pill_h + 2 * Scaled(5);

  // Centered under the pip, clamped to the window; always BELOW the strip,
  // so it never covers the caption row or the pips.
  const int mid_x = (chip->bounds.left + chip->bounds.right) / 2;
  int left = mid_x - card_w / 2;
  left = std::min(left, band_right - card_w);
  left = std::max(left, Scaled(4));
  int top = chip->bounds.bottom + Scaled(4);
  top = std::min(top,
                 static_cast<int>(layout.client.bottom) - Scaled(2) - card_h);
  g.card = {left, top, left + card_w, top + card_h};
  *out = std::move(g);
  return true;
}

void PhiWindowController::PaintHoverCard(HDC dc,
                                         const HoverCardGeometry& g,
                                         const PhiPalette& palette) {
  const COLORREF card_bg = palette.card;
  const COLORREF accent = card_accent_;
  const RECT& card = g.card;
  StrokeRounded(dc, card, card_bg, palette.border, Scaled(kHoverCardRadius));

  const int mid_y = (card.top + card.bottom) / 2;
  int x = card.left + Scaled(8);

  // The Space capsule: icon + name on the accent's 15% wash.
  const RECT pill = {x, mid_y - g.pill_h / 2, x + g.pill_w,
                     mid_y + g.pill_h / 2};
  FillRounded(dc, pill, BlendOver(accent, card_bg, 38), g.pill_h);
  RECT icon_rc = {pill.left + Scaled(6), mid_y - Scaled(6),
                  pill.left + Scaled(6) + Scaled(12), mid_y + Scaled(6)};
  // The icon keeps its strip colours — catalog art in the label ink, emoji
  // in their own — only the name and keycaps wear the accent.
  const COLORREF icon_ink = palette.label;
  if (!g.icon.empty() &&
      PhiDrawCatalogIcon(dc, icon_rc, g.icon, Scaled(12), icon_ink)) {
    // The Mac catalog's own artwork.
  } else if (!g.icon.empty() && !PhiIsPrivateGlyph(g.icon) &&
             PhiDrawColorIcon(dc, icon_rc, g.icon, Scaled(12), icon_ink)) {
    // Colour emoji via DirectWrite; the font paths below are the fallback.
  } else if (!g.icon.empty() && (emoji_font_ || chip_icon_font_)) {
    HFONT icon_font = PhiIsPrivateGlyph(g.icon) ? chip_icon_font_ : emoji_font_;
    HGDIOBJ previous_icon_font =
        icon_font ? ::SelectObject(dc, icon_font) : nullptr;
    ::SetTextColor(dc, icon_ink);
    PhiDrawTextAA(dc, g.icon.c_str(), -1, &icon_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (previous_icon_font) {
      ::SelectObject(dc, previous_icon_font);
    }
  } else {
    ::SetTextColor(dc, icon_ink);
    const std::wstring initial = g.name.substr(0, 1);
    PhiDrawTextAA(dc, initial.c_str(), -1, &icon_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  ::SetTextColor(dc, accent);
  RECT name_rc = {icon_rc.right + Scaled(4), pill.top, pill.right - Scaled(6),
                  pill.bottom};
  PhiDrawTextAA(dc, g.name.c_str(), -1, &name_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  x += g.pill_w + g.gap;

  // Middle-dot separators and the profile, dim like the Mac's secondary.
  const RECT dot1 = {x, card.top, x + g.dot_w, card.bottom};
  DrawGlyph(dc, dot1, L"·", palette.label_dim);
  x += g.dot_w + g.gap;
  ::SetTextColor(dc, palette.label_dim);
  RECT profile_rc = {x, card.top, x + g.profile_w, card.bottom};
  PhiDrawTextAA(dc, g.profile.c_str(), -1, &profile_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  x += g.profile_w;

  // The switch shortcut as keycaps, tinted like the address tooltip's.
  if (g.keys_w) {
    x += g.gap;
    const RECT dot2 = {x, card.top, x + g.dot_w, card.bottom};
    DrawGlyph(dc, dot2, L"·", palette.label_dim);
    x += g.dot_w + g.gap;
    for (size_t i = 0; i < g.keycaps.size(); ++i) {
      const RECT key = {x, mid_y - Scaled(7), x + g.key_widths[i],
                        mid_y + Scaled(8)};
      StrokeRounded(dc, key, BlendOver(accent, card_bg, 36),
                    BlendOver(accent, card_bg, 140), Scaled(4));
      DrawGlyph(dc, key, g.keycaps[i].c_str(), accent);
      x += g.key_widths[i] + Scaled(2);
    }
  }
}

void PhiWindowController::SyncHoverCardPopup(HDC dc, const Layout& layout) {
  HoverCardGeometry g;
  if (!shell_hwnd_ || !MeasureHoverCard(dc, layout, &g)) {
    if (hover_card_popup_ && ::IsWindowVisible(hover_card_popup_)) {
      ::ShowWindow(hover_card_popup_, SW_HIDE);
    }
    return;
  }
  // Layout → screen. The floating sidebar's panel shows the expanded layout
  // from floating_offset_y_ down at its own origin.
  POINT origin = {g.card.left, g.card.top};
  if (floating_layout_ && floating_sidebar_ && ::IsWindow(floating_sidebar_)) {
    origin.y -= floating_offset_y_;
    ::ClientToScreen(floating_sidebar_, &origin);
  } else {
    ::ClientToScreen(shell_hwnd_, &origin);
  }
  const int w = g.card.right - g.card.left;
  const int h = g.card.bottom - g.card.top;
  const RECT screen = {origin.x, origin.y, origin.x + w, origin.y + h};
  if (!hover_card_popup_) {
    static const ATOM atom = [] {
      WNDCLASSW cls = {};
      cls.lpfnWndProc = &PhiWindowController::HoverCardPopupProc;
      cls.hInstance = ::GetModuleHandleW(nullptr);
      cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
      cls.lpszClassName = L"PhiSpaceHoverCard";
      return ::RegisterClassW(&cls);
    }();
    (void)atom;
    // Owned by the shell, so it stays above the shell and its children
    // without WS_EX_TOPMOST; transparent to the pointer, so it never takes
    // the hover it is showing for.
    // Layered: the card's corners are alpha coverage (antialiased at any
    // radius), where a window region cuts them on the pixel grid and DWM's
    // own rounding is fixed at the pills' 8.
    hover_card_popup_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT |
            WS_EX_LAYERED,
        L"PhiSpaceHoverCard", L"", WS_POPUP, 0, 0, 0, 0, shell_hwnd_, nullptr,
        ::GetModuleHandleW(nullptr), this);
    if (!hover_card_popup_) {
      return;
    }
  }
  g.card = {0, 0, w, h};  // Painted at the popup's own origin.
  hover_card_geom_ = std::move(g);
  const bool moved = !::EqualRect(&screen, &hover_card_popup_rect_);
  const int r = Scaled(kHoverCardRadius);
  const bool layered =
      (::GetWindowLongPtrW(hover_card_popup_, GWL_EXSTYLE) & WS_EX_LAYERED) !=
      0;
  if (moved && !layered) {
    // The region path: the corners cut on the pixel grid.
    ::SetWindowRgn(hover_card_popup_,
                   ::CreateRoundRectRgn(0, 0, w + 1, h + 1, 2 * r, 2 * r),
                   FALSE);
  }
  if (moved) {
    hover_card_popup_rect_ = screen;
  }
  if (moved || !::IsWindowVisible(hover_card_popup_)) {
    ::SetWindowPos(hover_card_popup_, HWND_TOP, screen.left, screen.top, w, h,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  // Recommitted with every shell frame so it rides the theme ramp too.
  if (layered) {
    if (PhiCommitShapedLayer(
            hover_card_popup_, w, h,
            [&](HDC dc) { PaintHoverCardPopup(hover_card_popup_, dc); },
            [&](double px, double py) {
              return PhiRoundRectCoverage(px, py, w, h, r);
            })) {
      return;
    }
    // No layer to be had: fall back to the region for good.
    ::SetWindowLongPtrW(
        hover_card_popup_, GWL_EXSTYLE,
        ::GetWindowLongPtrW(hover_card_popup_, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    ::SetWindowRgn(hover_card_popup_,
                   ::CreateRoundRectRgn(0, 0, w + 1, h + 1, 2 * r, 2 * r),
                   FALSE);
  }
  ::InvalidateRect(hover_card_popup_, nullptr, FALSE);
}

void PhiWindowController::PaintHoverCardPopup(HWND popup, HDC dc) {
  RECT rc = {};
  ::GetClientRect(popup, &rc);
  if (rc.right <= 0 || rc.bottom <= 0) {
    return;
  }
  HDC mem = ::CreateCompatibleDC(dc);
  HBITMAP bitmap = ::CreateCompatibleBitmap(dc, rc.right, rc.bottom);
  HGDIOBJ old_bitmap = ::SelectObject(mem, bitmap);
  HFONT card_font = small_font_ ? small_font_ : ui_font_;
  HGDIOBJ old_font = card_font ? ::SelectObject(mem, card_font) : nullptr;
  ::SetBkMode(mem, TRANSPARENT);
  const PhiPalette palette = CurrentPalette();
  HBRUSH ground = ::CreateSolidBrush(palette.card);
  ::FillRect(mem, &rc, ground);  // Under the region's trimmed corners.
  ::DeleteObject(ground);
  PaintHoverCard(mem, hover_card_geom_, palette);
  ::BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
  if (old_font) {
    ::SelectObject(mem, old_font);
  }
  ::SelectObject(mem, old_bitmap);
  ::DeleteObject(bitmap);
  ::DeleteDC(mem);
}

// static
LRESULT CALLBACK PhiWindowController::HoverCardPopupProc(HWND hwnd,
                                                         UINT msg,
                                                         WPARAM wparam,
                                                         LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiWindowController*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // Never takes the pointer.
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_ERASEBKGND:
      return 1;  // PaintHoverCardPopup covers the whole client rect.
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        self->PaintHoverCardPopup(hwnd, dc);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

// static
void PhiWindowController::DrawSurfaceBadge(HDC dc,
                                           const RECT& box,
                                           SidebarSurface kind,
                                           bool hovered,
                                           COLORREF ground) {
  if (box.right <= box.left || kind == SidebarSurface::kContent || !host_) {
    return;
  }
  const PhiPalette& palette = host_->PaletteForSlot(slot_.value);
  // A small plate the accent tints, so it reads as "something is attached"
  // whether or not the row is selected.
  FillRounded(dc, box, BlendOver(palette.accent, ground, 46),
              Scaled(kCornerRadius) / 2);
  HGDIOBJ previous_font = caption_font_ ? ::SelectObject(dc, caption_font_)
                                        : nullptr;
  if (hovered) {
    DrawGlyph(dc, box, L"\u2212", palette.accent);  // Minus: close it.
  } else if (kind == SidebarSurface::kReader) {
    DrawReaderGlyph(dc, box, palette.accent, dpi_);
  } else {
    DrawGlyph(dc, box, L"\uE8A7", palette.accent);  // A window floating over.
  }
  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

void PhiWindowController::PaintSidebarBand(HDC dc,
                                           const Layout& layout,
                                           const PhiPalette& palette) {
  // Palette-derived locals the band rows use (same derivations as
  // PaintChrome's own).
  const COLORREF kTintTop = palette.ground_top;
  const COLORREF kRowSelectedBg = palette.selected;
  const COLORREF kLabel = palette.label;
  const COLORREF kLabelDim = palette.label_dim;
  const COLORREF kBorder = palette.border;
  const COLORREF kActivePillBorder =
      palette.dark ? BlendOver(RGB(0xFF, 0xFF, 0xFF), kRowSelectedBg, 51)
                   : kRowSelectedBg;

  // Pinned tabs: PinnedTabItem cells -- resting on the 4% wash, an 18pt
  // favicon centred, radius 8. The ACTIVE pin (its bound live tab is the
  // active tab) gets the selected wash and the Mac's 2pt accent ring; in the
  // horizontal strip the cells are the 28pt pinned section and the active one
  // reads like the neighbouring strip tabs (selected wash, no ring).
  const bool pin_sliding = drag_active_ && drag_kind_ == DragKind::kPin &&
                           !layout.horizontal_tabs && DragIndex() >= 0 &&
                           DragIndex() < static_cast<int>(pinned_.size());
  std::vector<size_t> pin_order;
  pin_order.reserve(pinned_.size());
  for (size_t i = 0; i < pinned_.size(); ++i) {
    if (!pin_sliding || static_cast<int>(i) != DragIndex()) {
      pin_order.push_back(i);
    }
  }
  if (pin_sliding) {
    pin_order.push_back(static_cast<size_t>(DragIndex()));
  }
  for (const size_t pin_order_index : pin_order) {
    const ShortcutRow& pin = pinned_[pin_order_index];
    const bool pin_dragged =
        pin_sliding && static_cast<int>(pin_order_index) == DragIndex();
    if (pin_dragged && !drag_capture_) {
      continue;  // It rides the drag image window; its slot is the gap.
    }
    // Siblings glide by their slide offsets (both axes — a grid gap wraps
    // rows); the dragged cell is only ever painted here at home, lifted,
    // for its snapshot.
    RECT cell = pin.bounds;
    if (!pin_dragged && drag_active_ && !layout.horizontal_tabs) {
      const SlideVec offset = RowSlideOffset(pin.guid);
      ::OffsetRect(&cell, static_cast<int>(offset.dx),
                   static_cast<int>(offset.dy));
    }
    const bool pin_hovered =
        hover_kind_ == HoverKind::kPin &&
        hover_index_ == static_cast<int>(&pin - pinned_.data());
    const COLORREF pin_bg = pin_dragged ? palette.hover_strong
                            : pin.active
                                ? palette.selected
                                : (pin_hovered ? palette.hover_strong
                                               : palette.hover);
    FillRounded(dc, cell, pin_bg, Scaled(kCornerRadius));
    if (pin_dragged) {
      StrokeRounded(dc, cell, pin_bg, palette.border, Scaled(kCornerRadius));
    }
    if (pin.active && !layout.horizontal_tabs) {
      RECT rr = cell;
      ::InflateRect(&rr, -Scaled(1), -Scaled(1));
      PhiStrokeRoundRectAA(dc, rr, Scaled(kCornerRadius), palette.accent,
                           Scaled(2));
    }
    if (!pin.url_b.empty()) {
      // A merged split cell: two 16pt favicons side by side, the Mac's
      // PinnedSplitItem (16 + 4 + 16 centred in the double-width cell).
      const int size = Scaled(16);
      const int cx = (cell.left + cell.right) / 2;
      const int cy = (cell.top + cell.bottom) / 2;
      const RECT icon_a = {cx - Scaled(2) - size, cy - size / 2,
                           cx - Scaled(2), cy + size / 2};
      const RECT icon_b = {cx + Scaled(2), cy - size / 2,
                           cx + Scaled(2) + size, cy + size / 2};
      const auto pane_icon = [&](const RECT& slot,
                                 const std::vector<unsigned char>& bgra,
                                 int width, int height) {
        if (!bgra.empty()) {
          DrawFavicon(dc, slot, bgra.data(), width, height, pin_bg);
        } else {
          HGDIOBJ previous_glyph_font =
              caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
          DrawGlyph(dc, slot, L"", kLabelDim);
          if (previous_glyph_font) {
            ::SelectObject(dc, previous_glyph_font);
          }
        }
      };
      pane_icon(icon_a, pin.favicon, pin.favicon_width, pin.favicon_height);
      pane_icon(icon_b, pin.favicon_b, pin.favicon_b_width,
                pin.favicon_b_height);
    } else if (!pin.favicon.empty()) {
      const int size = Scaled(layout.horizontal_tabs ? kStripPinFavicon
                                                     : kPinnedFavicon);
      const int cx = (cell.left + cell.right) / 2;
      const int cy = (cell.top + cell.bottom) / 2;
      const RECT icon = {cx - size / 2, cy - size / 2, cx + size / 2,
                         cy + size / 2};
      DrawFavicon(dc, icon, pin.favicon.data(), pin.favicon_width,
                  pin.favicon_height, pin_bg);
    } else {
      ::SetTextColor(dc, kLabel);
      const std::wstring initial =
          pin.title.empty() ? std::wstring(L"?") : pin.title.substr(0, 1);
      PhiDrawTextAA(dc, initial.c_str(), -1, &cell,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
      drag_active_ && drag_kind_ == DragKind::kBookmark &&
      !layout.horizontal_tabs && DragIndex() >= 0 &&
      DragIndex() < static_cast<int>(bookmarks_.size());
  std::vector<size_t> bookmark_order;
  bookmark_order.reserve(bookmarks_.size());
  for (size_t i = 0; i < bookmarks_.size(); ++i) {
    if (!bookmark_sliding || static_cast<int>(i) != DragIndex()) {
      bookmark_order.push_back(i);
    }
  }
  if (bookmark_sliding) {
    bookmark_order.push_back(static_cast<size_t>(DragIndex()));
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
        static_cast<int>(bookmark_order_index) == DragIndex();
    if (bookmark_dragged && !drag_capture_) {
      continue;  // It rides the drag image window; its slot is the gap.
    }
    RECT pill = bookmark.bounds;
    if (!bookmark_dragged && drag_active_ && !layout.horizontal_tabs) {
      const SlideVec offset = RowSlideOffset(bookmark.guid);
      ::OffsetRect(&pill, static_cast<int>(offset.dx),
                   static_cast<int>(offset.dy));
    }
    pill.top += Scaled(kRowPad);
    pill.bottom -= Scaled(kRowPad);
    // Outline depth indents the pill itself, as the Mac outline rows do.
    pill.left += bookmark.depth * Scaled(kBookmarkIndent);
    const bool bookmark_hovered =
        hover_kind_ == HoverKind::kBookmark &&
        hover_index_ == static_cast<int>(&bookmark - bookmarks_.data());
    if (bookmark_dragged) {
      // Lifted, like the dragged tab row.
      FillRounded(dc, pill, palette.hover_strong, Scaled(kCornerRadius));
      StrokeRounded(dc, pill, palette.hover_strong, palette.border,
                    Scaled(kCornerRadius));
    } else if (drag_active_ &&
               (drag_kind_ == DragKind::kBookmark ||
                drag_kind_ == DragKind::kPin) &&
               bookmark_drop_folder_row_ ==
                   static_cast<int>(bookmark_order_index)) {
      // The Mac resolver's folder highlight: this drop lands INSIDE.
      FillRounded(dc, pill, palette.hover_strong, Scaled(kCornerRadius));
      StrokeRounded(dc, pill, palette.hover_strong, palette.accent,
                    Scaled(kCornerRadius));
    } else if (bookmark.active) {
      // Its bound live tab is the active tab: the row IS the tab, so it
      // reads exactly like the active tab row.
      FillRounded(dc, pill, kRowSelectedBg, Scaled(kCornerRadius));
      if (palette.dark) {
        StrokeRounded(dc, pill, kRowSelectedBg, kActivePillBorder,
                      Scaled(kCornerRadius));
      }
    } else if (bookmark_hovered) {
      FillRounded(dc, pill, palette.hover, Scaled(kCornerRadius));
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
    if (bookmark.folder) {
      // The Mac paints a Lottie folder carrying the chosen symbol on its
      // face; with no Lottie runtime here the symbol itself IS the row icon,
      // drawn from the same imageset artwork the Mac's picker uses. The
      // catalog's "default" keeps the plain folder glyph.
      DrawFolderIcon(dc, icon, bookmark.icon, kLabelDim, bookmark.expanded);
    } else if (!bookmark.favicon.empty()) {
      DrawFavicon(dc, icon, bookmark.favicon.data(), bookmark.favicon_width,
                  bookmark.favicon_height,
                  bookmark.active ? kRowSelectedBg : kTintTop);
    } else {
      HGDIOBJ previous_glyph_font =
          caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
      DrawGlyph(dc, icon, L"\uE774", kLabelDim);
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
                    bookmark.active ? kRowSelectedBg : kTintTop);
      } else {
        HGDIOBJ previous_glyph_font =
            caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
        DrawGlyph(dc, icon_b, L"\uE774", kLabelDim);
        if (previous_glyph_font) {
          ::SelectObject(dc, previous_glyph_font);
        }
      }
      text.left += Scaled(24);
    }
    ::SetTextColor(dc, kLabel);
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
      const SlideVec offset = !bookmark_dragged && drag_active_
                                  ? RowSlideOffset(bookmark.guid)
                                  : SlideVec{};
      ::OffsetRect(&bookmark_badge, static_cast<int>(offset.dx),
                   static_cast<int>(offset.dy));
      text.right = bookmark_badge.left - Scaled(4);
    }
    HGDIOBJ folder_font_previous =
        bookmark.folder && ui_semibold_font_
            ? ::SelectObject(dc, ui_semibold_font_)
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
  const int band_shift = drag_active_ && !layout.horizontal_tabs
                             ? static_cast<int>(RowSlideOffset(kBandShiftKey).dy)
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
    if (hover_kind_ == HoverKind::kNewTabRow) {
      RECT pill = row;
      pill.top += Scaled(kRowPad);
      pill.bottom -= Scaled(kRowPad);
      FillRounded(dc, pill, palette.hover, Scaled(kCornerRadius));
    }
    const int mid = (row.top + row.bottom) / 2;
    const RECT icon = {row.left + Scaled(6), mid - Scaled(8),
                       row.left + Scaled(6) + Scaled(16), mid + Scaled(8)};
    DrawGlyph(dc, icon, L"+", palette.label_tertiary);
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
      StrokeRounded(dc, gh.container, palette.hover, palette.border,
                    Scaled(kCornerRadius));
    }
    if (gh.bounds.right <= gh.bounds.left) {
      continue;  // Header scrolled out of the viewport.
    }
    if (hover_kind_ == HoverKind::kGroupHeader &&
        hover_index_ == static_cast<int>(&gh - group_headers_.data())) {
      FillRounded(dc, gh.bounds, palette.hover_strong, Scaled(kCornerRadius));
    }
    // TabGroupHeaderView: 4pt leading, the 10pt chevron in a 20pt frame, the
    // 10pt dot, a 6pt gap, the semibold title.
    const int mid = (gh.bounds.top + gh.bounds.bottom) / 2;
    RECT chevron = {gh.bounds.left + Scaled(4), mid - Scaled(10),
                    gh.bounds.left + Scaled(4) + Scaled(20), mid + Scaled(10)};
    HGDIOBJ previous_glyph_font =
        caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
    DrawGlyph(dc, chevron, gh.collapsed ? L"\uE76C" : L"\uE70D", kLabelDim);
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
        ui_semibold_font_ ? ::SelectObject(dc, ui_semibold_font_) : nullptr;
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
  const bool sliding = drag_active_ && drag_kind_ == DragKind::kTab &&
                       !layout.horizontal_tabs && DragIndex() >= 0 &&
                       DragIndex() < static_cast<int>(tabs_.size());
  std::vector<size_t> draw_order;
  draw_order.reserve(tabs_.size());
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (!sliding || static_cast<int>(i) != DragIndex()) {
      draw_order.push_back(i);
    }
  }
  if (sliding) {
    draw_order.push_back(static_cast<size_t>(DragIndex()));
  }
  bool rows_clip_active = clip_rows;
  for (const size_t order_index : draw_order) {
    const TabRow& row = tabs_[order_index];
    if (row.bounds.right <= row.bounds.left) {
      continue;  // Scrolled out of the viewport.
    }
    const bool is_dragged =
        sliding && static_cast<int>(order_index) == DragIndex();
    if (is_dragged && !drag_capture_) {
      continue;  // It rides the drag image window; its slot is the gap.
    }
    // Siblings glide by their slide offsets; the dragged row is only ever
    // painted here at home, lifted, for its snapshot.
    int dy = band_shift;
    if (!is_dragged && sliding) {
      dy += static_cast<int>(DragSlideOffset(row.id));
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
        hover_kind_ == HoverKind::kTab &&
        hover_index_ == static_cast<int>(&row - tabs_.data());
    if (layout.horizontal_tabs) {
      // Chrome's strip. The active tab is the toolbar's own ground carried
      // up into the band, rounded on top and flush at the bottom so the two
      // read as one surface; a resting tab shows only the window tint it
      // sits on, divided from its neighbour by a hairline. Nothing here
      // rounds the bottom — that is the whole point of the merge.
      if (row.active) {
        FillRoundedTop(dc, pill, palette.window_bg, Scaled(kCornerRadius));
      } else {
        if (row_hovered) {
          FillRoundedTop(dc, pill, palette.hover, Scaled(kCornerRadius));
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
      FillRounded(dc, pill, palette.hover_strong, Scaled(kCornerRadius));
      StrokeRounded(dc, pill, palette.hover_strong, palette.border,
                    Scaled(kCornerRadius));
    } else if (row.active) {
      // Dark: the Mac's white-20% active border. Light: a hairline stands
      // in for the Mac's drop shadow — the same trade the Space chip makes,
      // or the white pill disappears on a white ground.
      StrokeRounded(dc, pill, kRowSelectedBg,
                    palette.dark ? kActivePillBorder : kBorder,
                    Scaled(kCornerRadius));
    } else if (row_hovered) {
      FillRounded(dc, pill, palette.hover, Scaled(kCornerRadius));
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
      if (!favicon.empty()) {
        DrawFavicon(dc, icon, favicon.data(),
                    pane_b ? r.b_favicon_width : r.favicon_width,
                    pane_b ? r.b_favicon_height : r.favicon_height, row_bg);
      } else if (r.internal_page && !logo.empty() &&
                 PhiDrawBgra(dc, icon, logo.data(), 256, 256)) {
        // Internal pages (chrome://newtab and friends) carry the Phi mark,
        // as the Mac shows its own logo there — not the generic globe.
      } else {
        // The Mac's no-favicon placeholder: the globe (FaviconProvider).
        HGDIOBJ previous_glyph_font =
            caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
        DrawGlyph(dc, icon, L"\uE774", kLabelDim);
        if (previous_glyph_font) {
          ::SelectObject(dc, previous_glyph_font);
        }
      }
      // A DORMANT tab (restored but never loaded, or discarded) wears the
      // Mac's dashed favicon ring and reads dimmer than a live one. The
      // active row is never dormant, so this never fights the selected pill.
      if (r.unloaded && !r.active) {
        DrawDormantRing(dc, icon, kLabelDim);
      }
      ::SetTextColor(dc, r.unloaded && !r.active ? kLabelDim : kLabel);
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
      DrawGlyph(dc, close_box, L"\u00D7", kLabelDim);
    }
    if (attached) {
      DrawSurfaceBadge(dc, surface_box, row.attached_surface,
                       row_hovered && !is_dragged, row_bg);
    }
    // The agent operating mask: a badge on rows an agent is holding. Accent
    // while the agent drives; dimmed once the user has taken the tab back
    // (the row keeps the badge so "Hand Back" stays discoverable).
    if (host_ && row.id != 0 && host_->IsTabAgentDriven(row.id)) {
      const bool reclaimed = host_->IsTabReclaimed(row.id);
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

void PhiWindowController::PaintBandOrSwipe(HDC dc,
                                           const Layout& layout,
                                           const PhiPalette& palette) {
  if (!swap_start_ || layout.horizontal_tabs) {
    PaintSidebarBand(dc, layout, palette);
    return;
  }
  // The Space switch slides the REAL rows (no snapshots): the leaving
  // Space's retained model paints shifted out at its frozen source palette,
  // the live (entering) model paints shifted in at the ramped palette, both
  // clipped to the band over the ramping ground.
  const double t = SwapEasedProgress();
  const int band_w = Scaled(sidebar_dip_);
  const int leave_dx = swap_forward_ ? -static_cast<int>(t * band_w + 0.5)
                                     : static_cast<int>(t * band_w + 0.5);
  const int enter_dx =
      swap_forward_ ? band_w + leave_dx : leave_dx - band_w;
  const int save = ::SaveDC(dc);
  ::IntersectClipRect(dc, 0, layout.favorites.top, band_w,
                      layout.tabs_limit);
  POINT origin = {};
  ::GetViewportOrgEx(dc, &origin);
  ::SetViewportOrgEx(dc, origin.x + leave_dx, origin.y, nullptr);
  SwapBandModel();
  PaintSidebarBand(dc, layout, swap_source_palette_);
  SwapBandModel();
  ::SetViewportOrgEx(dc, origin.x + enter_dx, origin.y, nullptr);
  PaintSidebarBand(dc, layout, palette);
  ::SetViewportOrgEx(dc, origin.x, origin.y, nullptr);
  ::RestoreDC(dc, save);
  if (t >= 1.0) {
    leaving_band_ = BandModel();
    swap_start_ = 0;
    RevealSwappedContent();  // The page flips only now, the Mac's order.
  }
}

void PhiWindowController::SwapBandModel() {
  tabs_.swap(leaving_band_.tabs);
  pinned_.swap(leaving_band_.pinned);
  bookmarks_.swap(leaving_band_.bookmarks);
  group_headers_.swap(leaving_band_.headers);
}

LRESULT CALLBACK PhiWindowController::ShellWndProc(HWND hwnd,
                                                   UINT msg,
                                                   WPARAM wparam,
                                                   LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_NCDESTROY) {
    // Final message: unhook the controller so nothing delivered to a
    // recycled HWND value can reach it (every popup and child proc below
    // does the same).
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiWindowController*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && self->destroying_) {
    // The destructor's DestroyWindow: the object is going, its members are
    // half torn down — every handler below would do work on it.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  switch (msg) {
    case WM_MOUSEACTIVATE: {
      // Let clicks activate without being eaten, and drive Chromium's own
      // activation across the boundary so input/focus/IME reach the page.
      // EXCEPT for clicks landing in our sidebar: the address field is a real
      // focusable control there, and handing focus to the page would fight the
      // click that is about to focus it.
      POINT cursor = {};
      ::GetCursorPos(&cursor);
      ::ScreenToClient(hwnd, &cursor);
      RECT content = {};
      if (self) {
        content = self->ComputeLayout().content;
      }
      const bool in_chrome = self && !::PtInRect(&content, cursor);
      // While the group overview covers the content, IT owns clicks there —
      // redirecting focus to the (hidden-under) browser child would close
      // the overview before its own WM_LBUTTONDOWN ever arrived.
      // Likewise while the native new-tab page covers the content: its own
      // window takes the click and decides where focus goes.
      if (!in_chrome && self && !PhiGroupOverview::IsOpen() &&
          !(self->new_tab_page_ && self->new_tab_page_->visible()) &&
          self->browser_hwnd_ && ::IsWindow(self->browser_hwnd_)) {
        ::SetFocus(self->browser_hwnd_);
        // The ACTIVE browser, never the slot's number: after a Space switch
        // the browser the slot opened with may already be closed.
        const BrowserId browser = self->active_browser_id();
        if (self->framework_ && browser.valid()) {
          self->framework_->FocusBrowserWindow(browser.value);
        }
      }
      return MA_ACTIVATE;
    }
    case WM_ACTIVATE:
    case WM_SETFOCUS:
      // Alt's summoned menu titles go back down when the window deactivates,
      // as Windows' own Alt-revealed bar does.
      if (self && msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE &&
          self->menu_alt_revealed_) {
        self->menu_alt_revealed_ = false;
        const RECT band = self->ComputeLayout().title_bar;
        ::InvalidateRect(hwnd, &band, FALSE);
      }
      // Activation/focus arriving at the shell top-level must be forwarded to
      // the child: as a WS_CHILD, Chromium's window reports IsTopLevelWindow()
      // == false and skips its own activation path.
      if (self && self->browser_hwnd_ && ::IsWindow(self->browser_hwnd_) &&
          ::GetFocus() != self->url_field_ &&
          !(msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
        if (self->new_tab_page_ && self->new_tab_page_->WantsFocus()) {
          // The native new-tab page is up: its omnibox is the focus.
          self->new_tab_page_->FocusField();
          return 0;
        }
        ::SetFocus(self->browser_hwnd_);
        const BrowserId browser = self->active_browser_id();
        if (self->framework_ && browser.valid()) {
          self->framework_->FocusBrowserWindow(browser.value);
        }
      }
      return 0;
    case WM_CTLCOLOREDIT:
      if (self && reinterpret_cast<HWND>(lparam) == self->url_field_ &&
          self->host_) {
        // CurrentPalette, not the host's: incognito overrides to fixed-dark.
        const PhiPalette palette = self->CurrentPalette();
        // In the sidebar the field rests on the 4% wash (8% while hovered,
        // tracking the capsule's brighten); in a top bar it sits bare on the
        // bar's own ground, washing only under the cursor.
        // Comfortable's omnibox is a filled pill like the sidebar's, not a
        // bare top-bar field: without this the EDIT paints the toolbar's
        // white straight over the pill and leaves only its rim showing.
        // Balanced and comfortable wash over the bar's own windowBackground,
        // so the EDIT carries no Space tint there; only performance's
        // sidebar pill blends over the tinted ground (see kFieldBg).
        const COLORREF bg =
            self->layout_mode_ == PhiLayoutMode::kBalanced
                ? (self->url_field_hover_ ? palette.bar_hover
                                          : palette.window_bg)
            : self->layout_mode_ == PhiLayoutMode::kComfortable
                ? (self->url_field_hover_ ? palette.bar_hover_strong
                                          : palette.bar_hover)
                : (self->url_field_hover_ ? palette.hover_strong
                                          : palette.hover);
        HDC edit_dc = reinterpret_cast<HDC>(wparam);
        ::SetTextColor(edit_dc, palette.label);
        ::SetBkColor(edit_dc, bg);
        if (!self->url_brush_ || self->url_brush_color_ != bg) {
          if (self->url_brush_) {
            ::DeleteObject(self->url_brush_);
          }
          self->url_brush_ = ::CreateSolidBrush(bg);
          self->url_brush_color_ = bg;
        }
        return reinterpret_cast<LRESULT>(self->url_brush_);
      }
      break;
    case WM_NCCALCSIZE:
      // Claim the caption for the client area: only the resize borders stay
      // non-client. The top edge keeps NO border — the caption band goes
      // flush to it (WM_NCHITTEST serves HTTOP from the band's first pixels).
      // BOTH wparam forms must be handled: the FIRST calc at creation comes
      // with wparam FALSE (a bare RECT), and skipping it leaves the OS
      // caption alive — its buttons then sit doubled above the band's.
      if (self) {
        if (self->content_fullscreen_) {
          return 0;  // Fullscreen: every pixel is client, no borders.
        }
        RECT* rect =
            wparam == TRUE
                ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                : reinterpret_cast<RECT*>(lparam);
        const UINT dpi = ::GetDpiForWindow(hwnd);
        const int border_x =
            ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
            ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int border_y =
            ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
            ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        rect->left += border_x;
        rect->right -= border_x;
        rect->bottom -= border_y;
        if (::IsZoomed(hwnd)) {
          rect->top += border_y;  // Maximized frames overhang the monitor.
        }
        return 0;
      }
      break;
    case WM_NCHITTEST: {
      if (!self) {
        break;
      }
      const LRESULT hit = ::DefWindowProcW(hwnd, msg, wparam, lparam);
      if (hit != HTCLIENT) {
        return hit;  // The real resize borders.
      }
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ::ScreenToClient(hwnd, &pt);
      const UINT dpi = ::GetDpiForWindow(hwnd);
      const int border_y =
          ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
          ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
      if (!::IsZoomed(hwnd) && pt.y < border_y) {
        return HTTOP;  // The band's first pixels still resize.
      }
      const int band_hit = self->CaptionHitTest(pt);
      return band_hit ? band_hit : HTCLIENT;
    }
    case WM_NCLBUTTONDOWN:
      // Swallow presses on our drawn caption buttons; the action fires on
      // release over the same button, like the OS ones.
      if (self && (wparam == HTMINBUTTON || wparam == HTMAXBUTTON ||
                   wparam == HTCLOSE)) {
        self->pressed_caption_ = static_cast<int>(wparam);
        return 0;
      }
      break;
    case WM_NCLBUTTONUP:
      if (self && self->pressed_caption_ &&
          static_cast<int>(wparam) == self->pressed_caption_) {
        const int pressed = self->pressed_caption_;
        self->pressed_caption_ = 0;
        if (pressed == HTMINBUTTON) {
          ::ShowWindow(hwnd, SW_MINIMIZE);
        } else if (pressed == HTMAXBUTTON) {
          ::ShowWindow(hwnd, ::IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        } else {
          ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
      }
      if (self) {
        self->pressed_caption_ = 0;
      }
      break;
    case WM_NCRBUTTONUP:
      // Comfortable's band IS the tab strip, and its bare run past the last
      // tab drags the window (CaptionHitTest serves HTCAPTION there), so a
      // right-click on it never reaches WM_RBUTTONUP — Windows would drop
      // the system menu instead of the strip's own. Claim it, the way the
      // Mac's tab-strip bar claims titlebar right-clicks
      // (shouldConsumeTitlebarEvent) to pop the same menu the sidebar shows.
      if (self && wparam == HTCAPTION) {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ::ScreenToClient(hwnd, &pt);
        const Layout band = self->ComputeLayout();
        if (band.horizontal_tabs && ::PtInRect(&band.title_bar, pt)) {
          self->OnChromeContextMenu(pt.x, pt.y);
          return 0;  // No WM_CONTEXTMENU, so no system menu behind ours.
        }
      }
      break;
    case WM_NCMOUSEMOVE:
      if (self) {
        self->SetCaptionHot(wparam == HTMINBUTTON || wparam == HTMAXBUTTON ||
                                    wparam == HTCLOSE
                                ? static_cast<int>(wparam)
                                : 0);
        // Most of the caption band hit-tests as HTCAPTION, so the pointer
        // crossing it arrives here and not as WM_MOUSEMOVE — this is where
        // the hidden menu titles come out. Anything the band's own controls
        // were washing is no longer under the pointer.
        {
          POINT band_pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
          ::ScreenToClient(hwnd, &band_pt);
          const RECT band = self->ComputeLayout().title_bar;
          const bool in_band = ::PtInRect(&band, band_pt) != 0;
          self->SetMenuRevealed(in_band);
          if (in_band) {
            // A non-client point is drag space, never a title: no title is
            // under the pointer, and nothing the band's controls washed is
            // either.
            self->SetMenuHot(-1);
            self->ClearHover();
          }
        }
        // The collapsed sidebar's hover trigger includes the left RESIZE
        // band — the outer frame pixels are non-client, so slamming the
        // cursor to the window edge (the Mac gesture) arrives here, not as
        // WM_MOUSEMOVE.
        if (self->sidebar_collapsed_ && !self->floating_sidebar_ &&
            wparam == HTLEFT) {
          POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
          ::ScreenToClient(hwnd, &pt);
          const Layout collapsed = self->ComputeLayout();
          if (!collapsed.horizontal_tabs &&
              pt.y > collapsed.title_bar.bottom) {
            self->ShowFloatingSidebar();
          }
        }
        // One leave notification when the mouse exits the frame entirely.
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE | TME_NONCLIENT,
                                 hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      break;
    case WM_NCMOUSELEAVE:
      // NOT "the pointer left the window". Revealing the titles turns the
      // band under the pointer from HTCAPTION into HTCLIENT, so this arrives
      // with the pointer standing perfectly still — and hiding on it put the
      // reveal in a blink loop: reveal, nc-leave, hide, nc-move, reveal.
      // Only where the cursor actually is decides.
      if (self) {
        self->SetCaptionHot(0);
        self->UpdateMenuRevealFromCursor();
        if (!self->menu_revealed_) {
          self->SetMenuHot(-1);
        }
      }
      break;
    case WM_MOUSEMOVE:
      if (self) {
        self->SetCaptionHot(0);  // Client moves mean we left the buttons.
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (self->sidebar_collapsed_ && !self->floating_sidebar_) {
          const Layout collapsed = self->ComputeLayout();
          if (!collapsed.horizontal_tabs &&
              pt.x < collapsed.content.left &&
              pt.y > collapsed.title_bar.bottom) {
            self->ShowFloatingSidebar();
          }
        }
        int hot = -1;
        const Layout band_layout = self->ComputeLayout();
        const bool in_band = ::PtInRect(&band_layout.title_bar, pt) != 0;
        self->SetMenuRevealed(in_band);
        if (in_band) {
          const size_t titles = self->VisibleMenuTitles(band_layout);
          for (size_t i = 0; i < titles; ++i) {
            if (::PtInRect(&self->menu_titles_[i].second, pt)) {
              hot = static_cast<int>(i);
              break;
            }
          }
        }
        self->SetMenuHot(hot);
        if (self->sidebar_resizing_) {
          const int dip = std::max(
              kSidebarWidthMin,
              std::min(static_cast<int>(pt.x) * 96 /
                           static_cast<int>(self->dpi_),
                       kSidebarWidthMax));
          if (dip != self->sidebar_dip_) {
            self->sidebar_dip_ = dip;
            self->LayoutContents();
            self->ScheduleRefresh();
          }
          return 0;
        }
        if ((wparam & MK_LBUTTON) && self->drag_kind_ != DragKind::kNone) {
          self->UpdateDrag(pt);
        } else {
          self->OnChromeMouseMove(pt);
        }
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      break;
    case WM_MOUSELEAVE:
      // The mirror of WM_NCMOUSELEAVE above: stepping off a menu title onto
      // the band's own drag space leaves the CLIENT area without leaving the
      // band, and the titles must stay put.
      if (self) {
        self->SetCaptionHot(0);
        self->UpdateMenuRevealFromCursor();
        if (!self->menu_revealed_) {
          self->SetMenuHot(-1);
        }
        self->ClearHover();
      }
      break;
    case kMsgPhiSyncNativePages:
      if (self) {
        self->SyncBookmarkManager();
        self->SyncNativeNtp();
      }
      return 0;
    case kMsgPhiRebuildRows:
      if (self) {
        self->rebuild_posted_ = false;
        if (self->rows_dirty_) {
          self->RebuildTabs();
        }
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case kMsgPhiDeferredClose:
      if (self) {
        self->RunDeferredClose();  // May destroy `self`.
      }
      return 0;
    case kMsgPhiToggleMenuReveal:
      if (self) {
        self->menu_alt_revealed_ = !self->menu_alt_revealed_;
        const RECT band = self->ComputeLayout().title_bar;
        ::InvalidateRect(hwnd, &band, FALSE);
      }
      return 0;
    case kMsgPhiSwitcherNext:
    case kMsgPhiSwitcherPrev:
      if (self) {
        PhiTabSwitcher::Cycle(self, msg == kMsgPhiSwitcherNext ? 1 : -1);
        return 0;
      }
      break;
    case kMsgPhiSwitcherCommit:
      PhiTabSwitcher::Commit();
      return 0;
    case WM_PHI_RAISE_CAPS:
      if (self) {
        if (self->new_tab_page_ && self->new_tab_page_->visible()) {
          // Above the browser child; the bubble and caps go above it next.
          ::SetWindowPos(self->new_tab_page_->hwnd(), HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        if (self->host_) {
          // The reader panel covers the whole card, so it has to come up
          // before the caps that round it.
          self->host_->RaiseReaderPanels(self->slot_.value);
        }
        if (self->status_bubble_ && ::IsWindow(self->status_bubble_) &&
            ::IsWindowVisible(self->status_bubble_)) {
          ::SetWindowPos(self->status_bubble_, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        for (HWND cap : self->corner_caps_) {
          if (cap && ::IsWindow(cap)) {
            ::SetWindowPos(cap, HWND_TOP, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
          }
        }
      }
      return 0;
    case WM_INITMENUPOPUP:
      // A menu is about to drop down — refresh its dynamic rows and enable
      // states, the Win32 twin of the Mac's menuWillOpen rebuilds.
      if (self && self->menu_bar_) {
        self->menu_bar_->OnMenuOpening(reinterpret_cast<HMENU>(wparam));
        return 0;
      }
      break;
    case WM_COMMAND:
      // lparam == 0 filters out child-control notifications (the address
      // field's EN_* arrive here too, carrying its HWND).
      if (self && lparam == 0 && HIWORD(wparam) == 0 && self->menu_bar_ &&
          self->menu_bar_->OnCommand(LOWORD(wparam))) {
        return 0;
      }
      if (self && self->url_field_ &&
          lparam == reinterpret_cast<LPARAM>(self->url_field_) &&
          HIWORD(wparam) == EN_CHANGE) {
        // The user typed: autocomplete runs.
        self->OnOmniboxFieldChanged(self->url_field_);
        return 0;
      }
      break;
    case WM_RBUTTONUP:
      if (self) {
        self->OnChromeContextMenu(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
      if (self) {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (self->InSidebarResizeBand(pt)) {
          self->sidebar_resizing_ = true;
          ::SetCapture(hwnd);
          return 0;
        }
        self->OnChromeClick(pt.x, pt.y);
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_MBUTTONDOWN:
      if (self) {
        self->OnChromeMiddleClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_LBUTTONDBLCLK:
      if (self) {
        // A group header's double-click opens the group overview; anywhere
        // else the second press behaves like a plain click.
        const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        self->RebuildTabs();
        for (const GroupHeader& gh : self->group_headers_) {
          if (::PtInRect(&gh.bounds, pt)) {
            PhiGroupOverview::Open(self, gh.id);
            return 0;
          }
        }
        self->OnChromeClick(pt.x, pt.y);
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_LBUTTONUP:
      if (self) {
        if (self->sidebar_resizing_) {
          self->sidebar_resizing_ = false;
          if (::GetCapture() == hwnd) {
            ::ReleaseCapture();
          }
          if (self->host_) {
            self->host_->RequestSidebarWidth(self->sidebar_dip_);
          }
          return 0;
        }
        self->CommitDrag();
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_CAPTURECHANGED:
      if (self) {
        if (self->sidebar_resizing_) {
          self->sidebar_resizing_ = false;
          if (self->host_) {
            self->host_->RequestSidebarWidth(self->sidebar_dip_);
          }
        }
        self->AbortDrag();
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_SETCURSOR:
      if (self && LOWORD(lparam) == HTCLIENT) {
        POINT pt = {};
        ::GetCursorPos(&pt);
        ::ScreenToClient(hwnd, &pt);
        if (self->sidebar_resizing_ || self->InSidebarResizeBand(pt)) {
          ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
          return TRUE;
        }
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_MOUSEWHEEL:
      if (self) {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ::ScreenToClient(hwnd, &pt);
        if (self->OnSidebarWheel(pt, GET_WHEEL_DELTA_WPARAM(wparam))) {
          return 0;
        }
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_TIMER:
      if (self && self->tab_preview_ && self->tab_preview_->OnTimer(wparam)) {
        return 0;
      }
      if (wparam == kDeferredCloseTimer) {
        ::KillTimer(hwnd, kDeferredCloseTimer);
        if (self) {
          self->RunDeferredClose();  // May destroy `self`.
        }
        return 0;
      }
      if (self && wparam == kHoverCardTimer) {
        self->PromoteHoverCard();  // Kills its own timer.
        return 0;
      }
      if (self && wparam == kCopyLinkRevertTimer) {
        ::KillTimer(hwnd, kCopyLinkRevertTimer);
        self->copy_link_flash_ = false;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      if (self && wparam == kAddSpaceFadeTimer) {
        // The "+" reveal fade: repaint until it settles.
        ::InvalidateRect(hwnd, nullptr, FALSE);
        if (!self->add_space_fade_start_ ||
            ::GetTickCount64() - self->add_space_fade_start_ >=
                static_cast<ULONGLONG>(kAddSpaceFadeMs)) {
          ::KillTimer(hwnd, kAddSpaceFadeTimer);
          self->add_space_fade_start_ = 0;
        }
        return 0;
      }
      if (self && wparam == kDragSlideTimer) {
        // The reorder slide: repaint until the current leg settles.
        ::InvalidateRect(hwnd, nullptr, FALSE);
        if (!self->drag_slide_start_ ||
            ::GetTickCount64() - self->drag_slide_start_ >= kDragSlideMs) {
          ::KillTimer(hwnd, kDragSlideTimer);
        }
        return 0;
      }
      if (self && wparam == kSpaceSwapTimer) {
        // The whole chrome repaints per tick: the band slide advances and
        // the theme ramp covers the statics. The caps ride the same ramp.
        ::InvalidateRect(hwnd, nullptr, FALSE);
        self->RefreshCornerCaps();
        self->RepaintUrlField();  // The omnibox EDIT rides the ramp too.
        if (self->RampEasedProgress() >= 1.0 && !self->swap_start_) {
          ::KillTimer(hwnd, kSpaceSwapTimer);
          self->ramp_start_ = 0;
        }
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_SIZE:
      if (self) {
        self->CancelSpaceSwap();  // Band geometry changed under the slide.
        self->LayoutContents();
      }
      return 0;
    case WM_DPICHANGED:
      // The window moved to a display at another scale. Windows hands us the
      // rect it wants in lparam; take it, then re-derive everything for the new
      // DPI (ComputeLayout picks it up, EnsureFont rebuilds the labels).
      if (self) {
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested) {
          ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        self->LayoutContents();
        self->RefreshTabs();
      }
      return 0;
    case WM_PRINTCLIENT: {
      // PrintWindow (used by screenshots/thumbnails) sends this instead of
      // WM_PAINT. Without handling it, a capture returns the LAST PAINTED
      // frame, not current state — so render fresh into the supplied DC.
      if (self) {
        HDC dc = reinterpret_cast<HDC>(wparam);
        self->RebuildTabs();
        self->PaintChrome(dc, self->ComputeLayout(),
                          /*include_child_controls=*/true);
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        // The model rebuilds on its own posted turn (kMsgPhiRebuildRows),
        // never here: a paint recomposes what the last rebuild placed. A
        // dirty model with no rebuild queued (a stale flag from a path that
        // did not post) gets one now for the next turn.
        if (self->rows_dirty_ && !self->rebuild_posted_) {
          self->rebuild_posted_ =
              ::PostMessageW(hwnd, kMsgPhiRebuildRows, 0, 0) != 0;
        }
        // Compose off-screen and blit once. Painting straight to the window
        // shows the chrome rebuilding element by element — the visible blink
        // on every tab/Space change, which invalidates the whole window. The
        // backbuffer bitmap is cached; a full-window alloc per paint is real
        // work at 4K.
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        const LONG bw = rc.right > 0 ? rc.right : 1;
        const LONG bh = rc.bottom > 0 ? rc.bottom : 1;
        if (self->back_buffer_ && (self->back_buffer_size_.cx != bw ||
                                   self->back_buffer_size_.cy != bh)) {
          ::DeleteObject(self->back_buffer_);
          self->back_buffer_ = nullptr;
        }
        if (!self->back_buffer_) {
          self->back_buffer_ = ::CreateCompatibleBitmap(dc, bw, bh);
          self->back_buffer_size_ = {bw, bh};
        }
        HDC mem = ::CreateCompatibleDC(dc);
        if (mem && self->back_buffer_) {
          HGDIOBJ old_bmp = ::SelectObject(mem, self->back_buffer_);
          // PaintChrome excludes the content rect (the browser child owns
          // it), so give those buffer pixels a defined color for the rare
          // blit with no child covering them (startup, degenerate states) —
          // the window ground, not white, or dark themes flash.
          HBRUSH fill = ::CreateSolidBrush(self->BackgroundFill());
          ::FillRect(mem, &rc, fill);
          ::DeleteObject(fill);
          self->ComposeFrame(mem);
          ::BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left,
                   ps.rcPaint.top, SRCCOPY);
          ::SelectObject(mem, old_bmp);
        } else {
          self->PaintChrome(dc, self->ComputeLayout(),
                            /*include_child_controls=*/false);
        }
        if (mem) {
          ::DeleteDC(mem);
        }
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      // Everything the shell owns is painted in WM_PAINT; a DefWindowProc
      // erase only flashes white under it.
      return 1;
    case WM_CLOSE:
      // Route through Chromium's normal close path (unload handlers, session
      // bookkeeping) for EVERY Space browser this slot hosts. When the last
      // Browser goes away the framework fires OnBrowserWindowClosing and the
      // coordinator destroys this controller — the shell window dies THEN, not
      // here. RequestClose may destroy `self`, so nothing touches it after.
      if (self) {
        self->RequestClose();
      } else {
        ::DestroyWindow(hwnd);
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace phi_client
