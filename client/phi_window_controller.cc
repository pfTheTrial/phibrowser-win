// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_window_controller.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

// dwmapi.h reaches rpcndr.h, which #defines `small` — this file uses it as
// an identifier.
#undef small

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
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
#include "phibrowser-win/client/phi_bookmark_manager.h"
#include "phibrowser-win/client/phi_folder_icon_picker.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_tab_preview.h"
#include "phibrowser-win/client/phi_tab_switcher.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_mac_icons.h"
#include "phibrowser-win/client/phi_menu_bar.h"
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
// The sidebar's own top inset. It used to be navRow.bottom + 8; with the nav
// row gone the Spaces strip starts at the gutter, and every band under it
// rides up by the 32pt the row occupied.
constexpr int kSpacesStripTop = 8;
constexpr int kSpacesStripHeight = 32;   // SpacesStripView.sidebarHeight
constexpr int kSpacesStripPad = 10;      // horizontalPadding inside the strip.
constexpr int kSpaceChip = 24;           // stripItemWidth/Height: a 24pt pip.
constexpr int kSpaceChipGap = 4;         // stripSpacing.
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
constexpr int kBookmarkIndent = 16;
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
constexpr UINT_PTR kHoverCardTimer = 0x516;
// Drag-reorder slide: Chrome's vertical-tab layout animation (200ms,
// EASE_IN_OUT) — sibling rows glide to open the insertion gap.
constexpr int kDragSlideMs = 200;
constexpr UINT_PTR kDragSlideTimer = 0x518;
constexpr UINT_PTR kFloatingTickTimer = 0x522;
// Reverts the copy-link checkmark after its second (triggeredRevertDelay).
constexpr UINT_PTR kCopyLinkRevertTimer = 0x523;

// Tab groups: the Mac's 32pt TabGroupHeaderView; the group cell sits on an
// 18% tint of the group color (~46/255).
constexpr int kGroupHeaderHeight = 32;
constexpr int kGroupTintAlpha = 46;
struct PhiGroupColor {
  const char* name;
  const wchar_t* label;
  COLORREF color;
};
// Chromium's TabGroupColorId palette (the Mac's GroupColor set is the same
// nine); the label doubles as the auto-title for unnamed groups.
constexpr std::array<PhiGroupColor, 9> kGroupColors = {{
    {"grey", L"Grey", RGB(0x80, 0x86, 0x8B)},
    {"blue", L"Blue", RGB(0x1A, 0x73, 0xE8)},
    {"red", L"Red", RGB(0xD9, 0x30, 0x25)},
    {"yellow", L"Yellow", RGB(0xF9, 0xAB, 0x00)},
    {"green", L"Green", RGB(0x18, 0x80, 0x38)},
    {"pink", L"Pink", RGB(0xD0, 0x18, 0x84)},
    {"purple", L"Purple", RGB(0xA1, 0x42, 0xF4)},
    {"cyan", L"Cyan", RGB(0x00, 0x7B, 0x83)},
    {"orange", L"Orange", RGB(0xFA, 0x90, 0x3E)},
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
  emoji.lfHeight = -Scaled(13);  // SpacesStripView.emojiFontSize
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
  caption.lfHeight = -Scaled(13);
  if (HFONT chip_built = ::CreateFontIndirectW(&caption)) {
    if (chip_icon_font_) {
      ::DeleteObject(chip_icon_font_);
    }
    chip_icon_font_ = chip_built;
  }
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
  int x = Scaled(kMenuTitlePad);
  const int band = Scaled(kTitleBarHeight);
  bool first = true;
  for (const std::wstring& title : titles) {
    SIZE extent = {};
    ::GetTextExtentPoint32W(dc, title.c_str(),
                            static_cast<int>(title.size()), &extent);
    int width = extent.cx + 2 * Scaled(kMenuTitlePad);
    if (first) {
      // The logo and "Phi" are ONE button: the first title's rect carries
      // the icon (18) and its gap (6) ahead of the text.
      width += Scaled(18) + Scaled(6);
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

  // The Spaces strip: 24pt pips from the left (10pt strip padding), one
  // trailing "+" chip. The Mac's second destination (new profile) moved to the
  // Spaces menu.
  auto place_spaces = [&](int left, int right, int top) {
    out.spaces_strip = {left, top, right, top + Scaled(kSpacesStripHeight)};
    const int chip_top =
        top + (Scaled(kSpacesStripHeight) - Scaled(kSpaceChip)) / 2;
    out.chips_left = left + Scaled(kSpacesStripPad);
    out.chips_top = chip_top;
    out.add_space = {right - Scaled(kSpacesStripPad) - Scaled(kSpaceChip),
                     chip_top, right - Scaled(kSpacesStripPad),
                     chip_top + Scaled(kSpaceChip)};
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

    // The active Space's compact pip leads the band (the Mac clears the
    // traffic lights with 80pt; Windows has none, so the edge inset does).
    out.chips_left = inset;
    out.chips_top = (tb - Scaled(kSpaceChip)) / 2;
    out.chips_limit = inset + Scaled(kSpaceChip);  // One compact pip.
    // search-tabs keeps the trailing spot the vertical layouts park it in,
    // so the button does not move when the layout does.
    out.search_tabs =
        band_control(out.caption_min.left - Scaled(kBandTrailingGap));
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
    const int r2 = w - m - Scaled(kTopBarLeading);
    out.chat = {r2 - Scaled(kChatWidth), mid - Scaled(kPillHeight) / 2, r2,
                mid + Scaled(kPillHeight) / 2};
    const int field_h = Scaled(kTopBarField);
    out.url_field = {out.reload.right + Scaled(6), mid - field_h / 2,
                     out.chat.left - Scaled(kTopBarSpacing),
                     mid + field_h / 2};
    out.tabs_top = out.spaces_strip.bottom + Scaled(kHeaderSpacer);
  } else {
    // Performance: back / forward / reload and search-tabs trail the caption
    // band, before the caption buttons — the sidebar has no header row at
    // all now, so the address bar sits directly under the Spaces strip. The
    // trio keeps its 1pt stack spacing and search-tabs follows it, at the
    // same x balanced puts it.
    int r = out.caption_min.left - Scaled(kBandTrailingGap);
    out.search_tabs = band_control(r);
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
      if (ActiveTabDistillable()) {
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
  out.new_tab_row = {inset, bookmarks_top + bookmarks_height, sidebar - inset,
                     bookmarks_top + bookmarks_height +
                         Scaled(kNewTabRowHeight)};
  out.tabs_top = out.new_tab_row.bottom + Scaled(kTabRowGap);

  // The bottom bar: downloads and memory leading (spacing 4), the Feedback
  // and Chat capsules trailing — SidebarBottomBar's row, settings not in it.
  const int bar_h = Scaled(kBottomBarHeight);
  const int bar_top = h - Scaled(kBottomSpacer) - bar_h;
  out.bottom_bar = {0, bar_top, sidebar, bar_top + bar_h};
  int bx = inset;
  out.downloads = {bx, bar_top, bx + btn, bar_top + bar_h};
  bx += btn + Scaled(4);
  out.memory = {bx, bar_top, bx + btn, bar_top + bar_h};
  if (layout_mode_ == PhiLayoutMode::kBalanced) {
    // Balanced keeps the puzzle in the bottom bar (its address bar lives in
    // the top bar); performance moved it inside the address capsule, where
    // the Mac SideAddressBar keeps its extension-menu button.
    bx += btn + Scaled(4);
    out.extensions = {bx, bar_top, bx + btn, bar_top + bar_h};
  }
  int br = sidebar - inset;
  const int pill_top = bar_top + (bar_h - Scaled(kPillHeight)) / 2;
  if (layout_mode_ != PhiLayoutMode::kBalanced) {
    // The Chat capsule fills the whole 24pt row (ChatButton contentHeight).
    // Balanced holds its top-bar chat rect instead: the Mac hides the
    // bottom-bar chat whenever navigation rides at the top
    // (updateChatButtonVisibility's navigationAtTop) — the capsule lives
    // in the header there, and this block must not overwrite it.
    out.chat = {br - Scaled(kChatWidth), bar_top, br, bar_top + bar_h};
    br = out.chat.left - Scaled(4);
  }
  // The narrow sidebar takes ViewThatFits' icon-only feedback variant.
  out.feedback = {br - Scaled(kFeedbackIconWidth), pill_top, br,
                  pill_top + Scaled(kPillHeight)};

  out.tabs_left = inset;
  out.tabs_limit = out.bottom_bar.top - Scaled(kFavoritesSpacer);
  if (hide_sidebar) {
    const RECT zero = {};
    out.favorites = zero;
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

int PhiWindowController::tabs_top() {
  return ComputeLayout().tabs_top;
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

PhiWindowController::PhiWindowController(int64_t window_id,
                                        HWND browser_hwnd,
                                        phi_framework::PhiFramework* framework,
                                        PhiSlotHost* host,
                                        bool agent,
                                        bool incognito)
    : window_id_(window_id),
      browser_hwnd_(browser_hwnd),
      framework_(framework),
      host_(host),
      agent_(agent),
      incognito_(incognito) {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    LOG(ERROR) << "[PhiClient] adopt: invalid browser HWND for window "
               << window_id_;
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
    const UINT dpi = ::GetDpiForWindow(browser_hwnd_);
    if (dpi) {
      dpi_ = dpi;  // Lay the first frame out for the monitor we open on.
    }
    // The shell's frame is CUSTOM (WM_NCCALCSIZE): the client reaches the
    // window's top edge and only the resize borders remain non-client, so
    // AdjustWindowRectEx (which assumes an OS caption) would be wrong. The
    // content region sits at (sidebar, titleBand + topGap) inside the client;
    // place the window so that region lands exactly on the browser's bounds.
    const UINT frame_dpi = dpi ? dpi : 96;
    const int border_x =
        ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, frame_dpi) +
        ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, frame_dpi);
    const int border_y =
        ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, frame_dpi) +
        ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, frame_dpi);
    x = wr.left - Scaled(sidebar_dip_) - border_x;
    y = wr.top - Scaled(kTitleBarHeight) - Scaled(kContentTopGap);
    w = (wr.right + Scaled(kContentMargin) + border_x) - x;
    h = (wr.bottom + Scaled(kContentMargin) + border_y) - y;
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
  // The window the slot was created with backs the default Space.
  space_browsers_["1"] = window_id_;
  browser_hwnds_[window_id_] = browser_hwnd_;
  space_order_.push_back("1");
  active_space_ = "1";
  active_browser_ = window_id_;
  if (host_ && !incognito_) {
    // The default Space binds here rather than through AttachSpaceBrowser,
    // so it needs its own note or lazy restore would never learn its window.
    host_->NoteSpaceWindow("1", window_id_);
  }
  if (framework_ && !incognito_) {
    // An incognito shell stays OUT of the Space registry: routed URLs must
    // never resolve into an off-the-record window.
    framework_->SetWindowSpace(window_id_, "1");
  }
  LayoutContents();
  ::ShowWindow(shell_hwnd_, agent_ ? SW_SHOWNOACTIVATE : SW_SHOW);
  LOG(INFO) << "[PhiClient] " << (agent_ ? "agent " : "") << "shell window "
            << window_id_ << " shell_hwnd=" << shell_hwnd_
            << " browser_hwnd=" << browser_hwnd_;
}

PhiWindowController::~PhiWindowController() {
  // Detach the browser child before tearing down our window, so its teardown
  // is not entangled with ours. Full close plumbing is a later slice.
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetParent(browser_hwnd_, nullptr);
  }
  if (shell_hwnd_ && ::IsWindow(shell_hwnd_)) {
    ::DestroyWindow(shell_hwnd_);  // Takes the address field child with it.
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
  if (small_font_) {
    ::DeleteObject(small_font_);
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
                 reinterpret_cast<LPARAM>(L"Search or Enter URL"));
  ::SetWindowLongPtrW(url_field_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
  WNDPROC previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
      url_field_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&UrlFieldProc)));
  if (!g_edit_base_proc) {
    g_edit_base_proc = previous;
  }
  PositionUrlField(ComputeLayout());
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
  const int right_stop =
      layout.field_ext_strip.right > layout.field_ext_strip.left
          ? layout.field_ext_strip.left
          : (layout.copy_link.right > layout.copy_link.left
                 ? layout.copy_link.left
                 : box.right);
  const int text_left = layout.page_icon.right > layout.page_icon.left
                           ? layout.page_icon.right + Scaled(8)
                           : box.left + Scaled(12);
  const int width = right_stop - Scaled(8) - text_left;
  ::MoveWindow(url_field_, text_left,
               box.top + (box.bottom - box.top - text_height) / 2,
               width > 0 ? width : 1, text_height, TRUE);
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
  if (!url_field_ || !framework_) {
    return;
  }
  const int len = ::GetWindowTextLengthW(url_field_);
  std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
  if (len > 0) {
    ::GetWindowTextW(url_field_, buffer.data(), len + 1);
  }
  std::wstring typed(buffer.data());
  const size_t first = typed.find_first_not_of(L" \t");
  const size_t last = typed.find_last_not_of(L" \t");
  typed = (first == std::wstring::npos) ? std::wstring()
                                        : typed.substr(first, last - first + 1);
  if (typed.empty()) {
    return;
  }
  // The framework resolves what was typed the way the omnibox would — a bare
  // host becomes a URL, free text becomes a search.
  const int64_t browser = active_browser_ ? active_browser_ : window_id_;
  if (omnibox_new_tab_pending_ || placeholder_browsers_.count(browser)) {
    // Omnibox new-tab mode — and a placeholder-mode browser, whose strip is
    // EMPTY (Navigate alone would no-op): the tab is born on commit. The
    // fresh tab is active, so Navigate applies the full typed-text
    // resolution to it (NewTab itself loads URLs literally).
    omnibox_new_tab_pending_ = false;
    framework_->NewTab(browser, "");
  }
  framework_->Navigate(browser, base::WideToUTF8(typed).c_str());
  FocusActiveBrowser();  // Keys belong to the page again once we've navigated.
  RefreshTabs();
}

void PhiWindowController::OnContentFullscreenChanged(int64_t browser_id,
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
        (fullscreen_browser_ && browser_id != fullscreen_browser_)) {
      return;
    }
    content_fullscreen_ = false;
    fullscreen_browser_ = 0;
    ::SetWindowLongPtrW(shell_hwnd_, GWL_STYLE, pre_fullscreen_style_);
    ::SetWindowPlacement(shell_hwnd_, &pre_fullscreen_placement_);
    ::SetWindowPos(shell_hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
  }
  LayoutContents();
  ::InvalidateRect(shell_hwnd_, nullptr, TRUE);
}

void PhiWindowController::OnPlaceholderModeChanged(int64_t browser_id,
                                                   bool entered) {
  if (entered) {
    placeholder_browsers_.insert(browser_id);
  } else {
    placeholder_browsers_.erase(browser_id);
  }
  const int64_t effective = active_browser_ ? active_browser_ : window_id_;
  if (browser_id == effective) {
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
  if (framework_) {
    framework_->NewTab(active_browser_ ? active_browser_ : window_id_, "");
    RefreshTabs();
  }
}

void PhiWindowController::OnUrlFieldChanged() {
  // EN_CHANGE while the user types: run/refresh the autocomplete query.
  // Programmatic sets (SyncUrlField, arrowing onto a row) are suppressed.
  if (url_field_programmatic_ || !url_field_ || !framework_ ||
      ::GetFocus() != url_field_) {
    return;
  }
  const int len = ::GetWindowTextLengthW(url_field_);
  std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
  if (len > 0) {
    ::GetWindowTextW(url_field_, buffer.data(), len + 1);
  }
  const std::wstring typed(buffer.data());
  if (typed.empty()) {
    CloseOmniboxPopup(/*stop_query=*/true);
    return;
  }
  framework_->StartOmniboxQuery(active_browser_ ? active_browser_ : window_id_,
                                base::WideToUTF8(typed).c_str());
}

void PhiWindowController::OnOmniboxResults(int64_t browser_id,
                                           const std::string& json) {
  const int64_t effective = active_browser_ ? active_browser_ : window_id_;
  if (browser_id != effective || !url_field_ ||
      ::GetFocus() != url_field_) {
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
  if (!shell_hwnd_ || !url_field_) {
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
  RECT field = {};
  ::GetWindowRect(url_field_, &field);
  const int width =
      std::max<int>(field.right - field.left + Scaled(16), Scaled(320));
  const int height =
      static_cast<int>(omnibox_rows_.size()) * Scaled(kOmniboxRowHeight) +
      Scaled(12);
  ::SetWindowPos(omnibox_popup_, HWND_TOP, field.left - Scaled(8),
                 field.bottom + Scaled(6), width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
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
  if (stop_query && framework_) {
    framework_->StopOmniboxQuery(active_browser_ ? active_browser_
                                                 : window_id_);
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
  if (omnibox_selected_ >= 0 && url_field_) {
    const OmniboxRow& row =
        omnibox_rows_[static_cast<size_t>(omnibox_selected_)];
    if (!row.fill.empty()) {
      // The row's fillIntoEdit lands in the field, caret at the end — the
      // omnibox arrow behavior.
      url_field_programmatic_ = true;
      ::SetWindowTextW(url_field_, row.fill.c_str());
      ::SendMessageW(url_field_, EM_SETSEL, row.fill.size(),
                     row.fill.size());
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
  framework_->OpenOmniboxMatch(active_browser_ ? active_browser_ : window_id_,
                               line, new_tab);
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
      HBRUSH sel = ::CreateSolidBrush(palette.hover_strong);
      HGDIOBJ prev_brush = ::SelectObject(dc, sel);
      HGDIOBJ prev_pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
      ::RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, Scaled(6),
                  Scaled(6));
      ::SelectObject(dc, prev_pen);
      ::SelectObject(dc, prev_brush);
      ::DeleteObject(sel);
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
    ::DrawTextW(dc, glyph_text, 1, &glyph_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (glyph_font) {
      ::SelectObject(dc, glyph_font);
    }
    RECT text_rc = {glyph_rc.right + Scaled(6), r.top, r.right - Scaled(8),
                    r.bottom};
    HGDIOBJ prev_font = ui_font_ ? ::SelectObject(dc, ui_font_) : nullptr;
    ::SetTextColor(dc, palette.label);
    RECT measure = text_rc;
    ::DrawTextW(dc, row.contents.c_str(), -1, &measure,
                DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    RECT contents_rc = text_rc;
    contents_rc.right = std::min(
        text_rc.right,
        static_cast<LONG>(text_rc.left + (measure.right - measure.left)));
    ::DrawTextW(dc, row.contents.c_str(), -1, &contents_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    if (!row.description.empty() &&
        contents_rc.right + Scaled(16) < text_rc.right) {
      RECT desc_rc = {contents_rc.right + Scaled(8), r.top, text_rc.right,
                      r.bottom};
      ::SetTextColor(dc, palette.label_dim);
      const std::wstring desc = L"— " + row.description;
      ::DrawTextW(dc, desc.c_str(), -1, &desc_rc,
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
  auto* self = reinterpret_cast<PhiWindowController*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
            self->CommitUrlField();
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
      // child swallows the shell's mouse moves over the text area).
      if (self && !self->url_field_hover_) {
        self->SetUrlFieldHover(true);
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      break;
    case WM_MOUSELEAVE:
      if (self) {
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

}  // namespace

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
  const int height = layout.client.bottom > 0 ? layout.client.bottom : 1;
  const int y = corner < 2 ? layout.content.top
                           : layout.content.bottom - rc.bottom;
  auto channel = [&](int top, int bottom) {
    return top + (bottom - top) * y / height;
  };
  HBRUSH tint = ::CreateSolidBrush(
      RGB(channel(GetRValue(palette.ground_top),
                  GetRValue(palette.ground_bottom)),
          channel(GetGValue(palette.ground_top),
                  GetGValue(palette.ground_bottom)),
          channel(GetBValue(palette.ground_top),
                  GetBValue(palette.ground_bottom))));
  ::FillRect(dc, &rc, tint);
  ::DeleteObject(tint);
  // The card's 1px border, continued around the arc.
  const int r = rc.right;
  HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
  // The full circle, clipped by the cap's region to this corner's arc.
  const int cx = (corner == 0 || corner == 2) ? r : -r;
  const int cy = corner < 2 ? r : -r;
  ::Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
  ::SelectObject(dc, old_brush);
  ::SelectObject(dc, old_pen);
  ::DeleteObject(pen);
}

void PhiWindowController::OnTargetURLChanged(int64_t browser_id,
                                             const std::wstring& url) {
  if (agent_) {
    return;  // Off-screen agent surfaces show no chrome.
  }
  const int64_t effective = active_browser_ ? active_browser_ : window_id_;
  if (browser_id != effective) {
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
    status_bubble_ = ::CreateWindowExW(
        0, L"PhiStatusBubble", L"", WS_CHILD, 0, 0, 0, 0, shell_hwnd_,
        nullptr, ::GetModuleHandleW(nullptr), this);
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
  // Only the top-right corner rounds: the rounded rect is oversized so its
  // other three corners fall outside the window.
  const int r = Scaled(8);
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
  // The card border along the exposed top and right edges.
  HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
  const int r = Scaled(8);
  ::RoundRect(dc, -2 * r, 0, rc.right, rc.bottom + 2 * r, 2 * r, 2 * r);
  ::SelectObject(dc, old_brush);
  ::SelectObject(dc, old_pen);
  ::DeleteObject(pen);
  RECT text = rc;
  text.left += Scaled(10);
  text.right -= Scaled(12);
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, palette.label);
  HGDIOBJ old_font = small_font_ ? ::SelectObject(dc, small_font_) : nullptr;
  // Middle truncation, the way a URL keeps its host and tail readable.
  ::DrawTextW(dc, status_url_.c_str(), -1, &text,
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
  if (!host_ || !framework_ || !shell_hwnd_) {
    return;
  }
  RECT anchor = ComputeLayout().downloads;
  ::MapWindowPoints(shell_hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor),
                    2);
  PhiDownloadsPopover::Toggle(shell_hwnd_, anchor,
                              host_->PaletteForSlot(window_id_), framework_,
                              active_browser_ ? active_browser_ : window_id_);
}

void PhiWindowController::LayoutContents() {
  if (bookmark_manager_ && bookmark_manager_->visible()) {
    bookmark_manager_->Reposition();
  }
  if (!shell_hwnd_) {
    return;
  }
  rows_dirty_ = true;  // Row bounds derive from the client size.
  SetStatusUrl(std::wstring());  // Anchored to the OLD content rect.
  CloseOmniboxPopup(/*stop_query=*/false);  // Anchored to the OLD field rect.
  const Layout layout = ComputeLayout();
  PositionUrlField(layout);
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
        placeholder_browsers_.count(active_browser_ ? active_browser_
                                                    : window_id_) > 0;
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
      corner_caps_[i] = ::CreateWindowExW(
          0, L"PhiCornerCap", L"", WS_CHILD, 0, 0, radius, radius,
          shell_hwnd_, nullptr, ::GetModuleHandleW(nullptr), this);
    }
    if (!corner_caps_[i]) {
      continue;
    }
    const bool shown = (i >= 2 || top_caps) && !content_fullscreen_;
    ::SetWindowPos(corner_caps_[i], HWND_TOP, positions[i].x, positions[i].y,
                   radius, radius,
                   SWP_NOACTIVATE | (shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    // Shape: the radius-square minus the quarter circle — only the bite
    // outside the card's curve remains visible.
    HRGN square = ::CreateRectRgn(0, 0, radius + 1, radius + 1);
    const int cx = (i == 0 || i == 2) ? radius : 0;
    const int cy = i < 2 ? radius : 0;
    HRGN circle = ::CreateEllipticRgn(cx - radius, cy - radius, cx + radius + 1,
                                      cy + radius + 1);
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

void PhiWindowController::ArmDrag(DragKind kind, int index, POINT pt) {
  drag_kind_ = kind;
  drag_index_ = index;
  drag_origin_ = pt;
  drag_point_ = pt;
  drag_active_ = false;
  drag_insert_ = -1;
  drag_grab_dx_ = 0;
  drag_grab_dy_ = 0;
  // Where inside the element the cursor grabbed it, so the floating copy
  // keeps that grip while it follows the cursor.
  if (kind == DragKind::kTab && index >= 0 &&
      index < static_cast<int>(tabs_.size())) {
    drag_grab_dy_ = pt.y - tabs_[static_cast<size_t>(index)].bounds.top;
  } else if (kind == DragKind::kPin && index >= 0 &&
             index < static_cast<int>(pinned_.size())) {
    drag_grab_dx_ = pt.x - pinned_[static_cast<size_t>(index)].bounds.left;
    drag_grab_dy_ = pt.y - pinned_[static_cast<size_t>(index)].bounds.top;
  } else if (kind == DragKind::kBookmark && index >= 0 &&
             index < static_cast<int>(bookmarks_.size())) {
    drag_grab_dy_ =
        pt.y - bookmarks_[static_cast<size_t>(index)].bounds.top;
  }
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
    if (drag_kind_ == DragKind::kTab && drag_index_ >= 0 &&
        drag_index_ < static_cast<int>(tabs_.size())) {
      // The dragged row vacates its list slot in EVERY band; where the gap
      // reopens depends on the target under the cursor.
      std::vector<const TabRow*> visible;
      int dragged_pos = -1;
      for (const TabRow& row : tabs_) {
        if (row.bounds.right <= row.bounds.left) {
          continue;
        }
        if (static_cast<int>(&row - tabs_.data()) == drag_index_) {
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
            // Leaving the list: later rows close the vacated slot; a
            // bookmark conversion also pushes the whole list down by the
            // row the bookmark band gains.
            if (i > dragged_pos) {
              dy = -pitch;
            }
            if (drag_band_ == DropBand::kBookmarks) {
              dy += pitch;
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
               drag_band_ == DropBand::kPins && drag_index_ >= 0 &&
               drag_index_ < static_cast<int>(pinned_.size())) {
      // Grid reorder: the dragged cell floats; a phantom of its own width
      // holds the target slot open.
      std::vector<int> units;
      for (int i = 0; i < static_cast<int>(pinned_.size()); ++i) {
        if (i == drag_index_) {
          continue;
        }
        units.push_back(
            pinned_[static_cast<size_t>(i)].url_b.empty() ? 1 : 2);
      }
      const int dragged_units =
          pinned_[static_cast<size_t>(drag_index_)].url_b.empty() ? 1 : 2;
      int to_pos = drag_insert_ > drag_index_ ? drag_insert_ - 1
                                              : drag_insert_;
      to_pos = std::max(0, std::min(to_pos, static_cast<int>(units.size())));
      std::vector<int> preview = units;
      preview.insert(preview.begin() + to_pos, dragged_units);
      const std::vector<RECT> target = PinnedGridWalk(preview, layout);
      for (int i = 0; i < static_cast<int>(pinned_.size()); ++i) {
        if (i == drag_index_) {
          continue;
        }
        const int j = i > drag_index_ ? i - 1 : i;
        const size_t jj = static_cast<size_t>(j >= to_pos ? j + 1 : j);
        const SlideVec v = {
            static_cast<double>(target[jj].left - pinned_[i].bounds.left),
            static_cast<double>(target[jj].top - pinned_[i].bounds.top)};
        if (v.dx != 0.0 || v.dy != 0.0) {
          row_to[pinned_[static_cast<size_t>(i)].guid] = v;
        }
      }
    } else if (drag_kind_ == DragKind::kBookmark && drag_index_ >= 0 &&
               drag_index_ < static_cast<int>(bookmarks_.size()) &&
               bookmark_drop_folder_row_ < 0) {
      // A drop INTO a collapsed folder opens no gap — the folder row
      // highlights instead and every displaced row settles home.
      const int to_pos = drag_insert_ > drag_index_ ? drag_insert_ - 1
                                                    : drag_insert_;
      for (int i = 0; i < static_cast<int>(bookmarks_.size()); ++i) {
        if (i == drag_index_) {
          continue;
        }
        double dy = 0.0;
        if (i > drag_index_ && i <= to_pos) {
          dy = -pitch;
        } else if (i >= to_pos && i < drag_index_) {
          dy = pitch;
        }
        if (dy != 0.0) {
          row_to[bookmarks_[static_cast<size_t>(i)].guid] =
              SlideVec{0.0, dy};
        }
      }
    }
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
  if (incognito_) {
    return DropBand::kTabs;  // No pin/bookmark conversions off the record.
  }
  const Layout layout = ComputeLayout();
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
  }
  // Tab and pin drags can cross bands (conversions, tear-off); the other
  // kinds stay home.
  drag_point_ = pt;
  DropBand band = DropBand::kTabs;
  if (drag_kind_ == DragKind::kTab || drag_kind_ == DragKind::kPin) {
    band = DropBandAt(pt);
  }
  const bool band_changed = band != drag_band_;
  drag_band_ = band;
  int slot = DragInsertSlot(pt);
  bool folder_target_changed = false;
  if (drag_kind_ == DragKind::kBookmark) {
    // Bookmark drags resolve a tree target, not just a flat slot; a change
    // between INTO-a-folder and a gap repaints even at the same slot.
    const int folder_before = bookmark_drop_folder_row_;
    slot = ResolveBookmarkDrop(pt);
    folder_target_changed = folder_before != bookmark_drop_folder_row_;
  }
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
  } else if (animated_kind && shell_hwnd_) {
    // The floating element follows every cursor move.
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

void PhiWindowController::CommitDrag() {
  const DragKind kind = drag_kind_;
  const DropBand band = drag_band_;
  const int from_pos = drag_index_;
  const int slot = drag_insert_;
  const bool active = drag_active_;
  const POINT release = drag_point_;
  // The tree target a bookmark drag resolved (CancelDrag clears it).
  const std::string bookmark_parent = bookmark_drop_parent_;
  const int bookmark_index = bookmark_drop_index_;
  CancelDrag();
  if (!active || slot < 0) {
    return;
  }

  // Cross-band drops first — the Mac's drag conversions.
  if (kind == DragKind::kTab && band != DropBand::kTabs) {
    if (from_pos < 0 || from_pos >= static_cast<int>(tabs_.size())) {
      return;
    }
    const TabRow& row = tabs_[static_cast<size_t>(from_pos)];
    const int64_t browser = active_browser_ ? active_browser_ : window_id_;
    if (band == DropBand::kOutside) {
      // Over ANOTHER shell window, the tab joins that window's strip (the
      // Mac's window-to-window drop); anywhere else it is a tear-off to a
      // fresh window (the new shell arrives through OnBrowserWindowCreated).
      POINT screen = release;
      ::ClientToScreen(shell_hwnd_, &screen);
      const int64_t target =
          host_ ? host_->SlotAtScreenPoint(screen, window_id_) : 0;
      if (!target || !host_ ||
          !host_->RequestDropTabOnWindow(window_id_, row.index, row.b_index,
                                         target)) {
        if (framework_) {
          framework_->MoveTabsToNewWindow(browser, row.index, row.b_index);
        }
      }
      RefreshTabs();
    } else if (band == DropBand::kPins && host_) {
      if (row.b_index >= 0) {
        host_->RequestDropSplit(window_id_, row.index, row.b_index,
                                /*as_bookmark=*/false, slot);
      } else {
        host_->RequestDropTab(window_id_, row.index, /*as_bookmark=*/false,
                              slot);
      }
    } else if (band == DropBand::kBookmarks && host_) {
      // The visible slot counts nested outline rows; conversions land at
      // the ROOT, so map it to a root sibling position first.
      int root_slot = 0;
      for (int i = 0; i < std::min(slot, static_cast<int>(bookmarks_.size()));
           ++i) {
        if (bookmarks_[static_cast<size_t>(i)].depth == 0) {
          ++root_slot;
        }
      }
      if (row.b_index >= 0) {
        host_->RequestDropSplit(window_id_, row.index, row.b_index,
                                /*as_bookmark=*/true, root_slot);
      } else {
        host_->RequestDropTab(window_id_, row.index, /*as_bookmark=*/true,
                              root_slot);
      }
    }
    return;
  }
  if (kind == DragKind::kPin && band == DropBand::kTabs) {
    // The Mac's movePinnedTabOut: the record dissolves and its tab(s)
    // join the tab list. An unopened record opens first (as a pair for a
    // split cell); RemoveStoredRow clears the tags, so bound tabs return
    // to the list as ordinary rows.
    if (from_pos < 0 || from_pos >= static_cast<int>(pinned_.size()) ||
        !host_) {
      return;
    }
    const ShortcutRow pin = pinned_[static_cast<size_t>(from_pos)];
    if (!pin.open && framework_ && !pin.url.empty()) {
      const int64_t browser = active_browser_ ? active_browser_ : window_id_;
      const int fresh_a = framework_->NewTab(browser, pin.url.c_str());
      if (!pin.url_b.empty() && fresh_a >= 0) {
        const int fresh_b = framework_->NewTab(browser, pin.url_b.c_str());
        framework_->ActivateTab(browser, fresh_a);
        if (fresh_b >= 0) {
          framework_->SplitActiveWith(browser, fresh_b);
        }
      }
    }
    host_->RemoveStoredRow(pin.guid);
    return;
  }

  // A bookmark reorder is a TREE move (the outline's resolved target), so
  // it must not fall into the flat same-slot early-out below: dropping INTO
  // a folder can share the visible slot with the dragged row's own.
  if (kind == DragKind::kBookmark) {
    if (host_ && from_pos >= 0 &&
        from_pos < static_cast<int>(bookmarks_.size()) &&
        bookmark_index >= 0) {
      const ShortcutRow& dragged = bookmarks_[static_cast<size_t>(from_pos)];
      // Skip the no-op: the same parent at the same sibling position.
      int current = 0;
      for (int j = 0; j < from_pos; ++j) {
        if (bookmarks_[static_cast<size_t>(j)].parent_guid ==
            dragged.parent_guid) {
          ++current;
        }
      }
      if (dragged.parent_guid != bookmark_parent ||
          bookmark_index != current) {
        host_->MoveStoredRow(dragged.guid, bookmark_parent, bookmark_index);
      }
    }
    return;
  }

  // Insert-before `slot` becomes the final sibling position by removing the
  // dragged element first.
  const int to_pos = slot > from_pos ? slot - 1 : slot;
  if (to_pos == from_pos) {
    return;
  }
  switch (kind) {
    case DragKind::kTab: {
      if (!framework_ || from_pos < 0 ||
          from_pos >= static_cast<int>(tabs_.size())) {
        return;
      }
      // Map the visual slot to a framework strip index (bound rows are
      // hidden, so visual neighbours carry the real indices).
      const int from = tabs_[static_cast<size_t>(from_pos)].index;
      int to;
      if (slot >= static_cast<int>(tabs_.size())) {
        to = tabs_.back().index;
      } else {
        to = tabs_[static_cast<size_t>(slot)].index;
        if (to > from) {
          --to;
        }
      }
      framework_->MoveTab(active_browser_ ? active_browser_ : window_id_,
                          from, to);
      RefreshTabs();
      break;
    }
    case DragKind::kPin:
      if (host_ && from_pos >= 0 &&
          from_pos < static_cast<int>(pinned_.size())) {
        host_->ReorderStoredRow(pinned_[static_cast<size_t>(from_pos)].guid,
                                to_pos);
      }
      break;
    case DragKind::kBookmark:
      break;  // Handled above — bookmarks resolve a tree target.
    case DragKind::kChip:
      if (host_ && from_pos >= 0 &&
          from_pos < static_cast<int>(chips_.size())) {
        host_->ReorderSpace(chips_[static_cast<size_t>(from_pos)].id, to_pos);
      }
      break;
    case DragKind::kNone:
      break;
  }
}

void PhiWindowController::CancelDrag() {
  const bool was_active = drag_active_;
  drag_kind_ = DragKind::kNone;
  drag_band_ = DropBand::kNone;
  drag_index_ = -1;
  drag_active_ = false;
  drag_insert_ = -1;
  drag_grab_dx_ = 0;
  drag_grab_dy_ = 0;
  bookmark_drop_parent_.clear();
  bookmark_drop_index_ = -1;
  bookmark_drop_folder_row_ = -1;
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

int PhiWindowController::ResolveBookmarkDrop(POINT pt) {
  bookmark_drop_parent_.clear();
  bookmark_drop_index_ = -1;
  bookmark_drop_folder_row_ = -1;
  const int n = static_cast<int>(bookmarks_.size());
  // The sibling position visible row `i` holds within its parent, with the
  // dragged row excluded — the store reinserts AFTER pulling it out.
  const auto sibling_pos = [&](int i) {
    int at = 0;
    for (int j = 0; j < i; ++j) {
      if (j != drag_index_ &&
          bookmarks_[static_cast<size_t>(j)].parent_guid ==
              bookmarks_[static_cast<size_t>(i)].parent_guid) {
        ++at;
      }
    }
    return at;
  };
  // The row under the pointer decides the target level (the Mac's
  // SidebarBookmarkFolderDropResolver); the floating row's own slot and the
  // gaps between rows fall through to the midpoint rule below.
  for (int i = 0; i < n; ++i) {
    const ShortcutRow& row = bookmarks_[static_cast<size_t>(i)];
    if (i == drag_index_ || pt.y < row.bounds.top ||
        pt.y > row.bounds.bottom) {
      continue;
    }
    const bool upper = pt.y < (row.bounds.top + row.bounds.bottom) / 2;
    if (row.folder && row.expanded) {
      // Anywhere on an expanded folder inserts as its first child — the
      // gap right below it IS the child slot.
      bookmark_drop_parent_ = row.guid;
      bookmark_drop_index_ = 0;
      return i + 1;
    }
    if (row.folder && upper) {
      // The upper half of a collapsed folder drops INTO it (appended);
      // the row highlights instead of a gap opening.
      bookmark_drop_parent_ = row.guid;
      bookmark_drop_index_ = std::numeric_limits<int>::max();
      bookmark_drop_folder_row_ = i;
      return i + 1;
    }
    // Before/after the row among ITS OWN siblings.
    bookmark_drop_parent_ = row.parent_guid;
    bookmark_drop_index_ = sibling_pos(i) + (upper ? 0 : 1);
    return i + (upper ? 0 : 1);
  }
  // A gap: insert before the first row whose midpoint sits below the
  // pointer, at that row's level; past the last row appends to the root.
  for (int i = 0; i < n; ++i) {
    const ShortcutRow& row = bookmarks_[static_cast<size_t>(i)];
    if (i != drag_index_ &&
        pt.y <= (row.bounds.top + row.bounds.bottom) / 2) {
      bookmark_drop_parent_ = row.parent_guid;
      bookmark_drop_index_ = sibling_pos(i);
      return i;
    }
  }
  bookmark_drop_index_ = std::numeric_limits<int>::max();
  return n;
}

int PhiWindowController::DragInsertSlot(POINT pt) {
  int slot = 0;
  // A pinned-grid slot (cell insertion order), shared by pin reorders and
  // a tab drag hovering the grid (the conversion drop).
  const auto pinned_slot = [&] {
    int at = 0;
    for (const ShortcutRow& pin : pinned_) {
      if (pt.y > pin.bounds.bottom ||
          (pt.y >= pin.bounds.top &&
           pt.x > (pin.bounds.left + pin.bounds.right) / 2)) {
        ++at;
      }
    }
    return at;
  };
  const auto bookmark_slot = [&] {
    int at = 0;
    for (const ShortcutRow& bookmark : bookmarks_) {
      if (pt.y > (bookmark.bounds.top + bookmark.bounds.bottom) / 2) {
        ++at;
      }
    }
    return at;
  };
  switch (drag_kind_) {
    case DragKind::kTab: {
      switch (drag_band_) {
        case DropBand::kOutside:
          return 0;  // Tear-off: any non-negative slot commits.
        case DropBand::kPins:
          return pinned_slot();
        case DropBand::kBookmarks:
          return bookmark_slot();
        default:
          break;
      }
      const Layout layout = ComputeLayout();
      for (const TabRow& row : tabs_) {
        if (row.bounds.right <= row.bounds.left) {
          continue;  // Hidden (scrolled out); not a drop neighbour.
        }
        if (layout.horizontal_tabs
                ? pt.x > (row.bounds.left + row.bounds.right) / 2
                : pt.y > (row.bounds.top + row.bounds.bottom) / 2) {
          ++slot;
        }
      }
      return slot;
    }
    case DragKind::kPin:
      if (drag_band_ == DropBand::kTabs) {
        return 0;  // Unpin into the list; the position is the list's own.
      }
      if (drag_band_ == DropBand::kOutside ||
          drag_band_ == DropBand::kBookmarks) {
        return -1;  // Not a pin target; the release is a no-op.
      }
      return pinned_slot();
    case DragKind::kBookmark:
      for (const ShortcutRow& bookmark : bookmarks_) {
        if (pt.y > (bookmark.bounds.top + bookmark.bounds.bottom) / 2) {
          ++slot;
        }
      }
      return slot;
    case DragKind::kChip:
      for (const SpaceChip& chip : chips_) {
        if (pt.x > (chip.bounds.left + chip.bounds.right) / 2) {
          ++slot;
        }
      }
      return slot;
    case DragKind::kNone:
      break;
  }
  return -1;
}

void PhiWindowController::DrawDragIndicator(HDC dc) {
  if (!drag_active_ || drag_insert_ < 0 || !host_) {
    return;
  }
  const PhiPalette& palette = host_->PaletteForSlot(window_id_);
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
    const std::array<RECT, 12> nav = {layout.back, layout.forward,
                                      layout.reload, layout.search_tabs,
                                      add_tab_button_, layout.overflow,
                                      layout.downloads, layout.memory,
                                      layout.feedback, layout.chat,
                                      layout.extensions, layout.copy_link};
    for (size_t i = 0; i < nav.size() && kind == HoverKind::kNone; ++i) {
      if (::PtInRect(&nav[i], client_pt)) {
        kind = HoverKind::kNav;
        index = static_cast<int>(i);
      }
    }
  }
  for (size_t i = 0; i < chips_.size() && kind == HoverKind::kNone; ++i) {
    if (::PtInRect(&chips_[i].bounds, client_pt)) {
      kind = HoverKind::kChip;
      index = static_cast<int>(i);
    }
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

// True when `tab_id`'s split stacks its panes. Asked only while a split row
// is actually hovered, so the framework round-trip stays off the paint path.
bool PhiWindowController::SplitIsStacked(int tab_id) const {
  if (!framework_ || tab_id == 0) {
    return false;
  }
  const int64_t browser = active_browser_ ? active_browser_ : window_id_;
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
  const int64_t browser = active_browser_ ? active_browser_ : window_id_;
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
  const int64_t browser = active_browser_ ? active_browser_ : window_id_;
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
          if (!row.partner_guid.empty() && row.tab_id_b != 0) {
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
  card_accent_ = host_ ? host_->PaletteForSlot(window_id_).accent
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
    const bool dark = host_->PaletteForSlot(window_id_).dark;
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
  return host_ ? host_->PaletteForSlot(window_id_).window_bg
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
  rows_dirty_ = true;
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

void PhiWindowController::AttachSpaceBrowser(const std::string& space_id,
                                             int64_t browser_window_id,
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
  space_browsers_[space_id] = browser_window_id;
  if (host_) {
    host_->NoteSpaceWindow(space_id, browser_window_id);
  }
  browser_hwnds_[browser_window_id] = browser_hwnd;
  active_space_ = space_id;
  active_browser_ = browser_window_id;
  if (framework_) {
    framework_->SetWindowSpace(browser_window_id, space_id.c_str());
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
  // Showing a different browser child is not enough: as a WS_CHILD it never
  // gets WM_ACTIVATE, so without this the keyboard still goes to the browser
  // that was visible before the Space switch (input appears dead until the
  // user clicks). Give the child Win32 focus AND drive Chromium's own
  // activation across the boundary.
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetFocus(browser_hwnd_);
  }
  if (framework_ && active_browser_) {
    framework_->FocusBrowserWindow(active_browser_);
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
  PhiPalette target = host_ ? host_->PaletteForSlot(window_id_) : kFallback;
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
  return true;
}

void PhiWindowController::CloseSpaceBrowser(const std::string& space_id) {
  auto it = space_browsers_.find(space_id);
  if (it == space_browsers_.end()) {
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
    framework_->CloseBrowserWindow(it->second);
  }
}

bool PhiWindowController::ForgetBrowser(int64_t browser_window_id) {
  browser_hwnds_.erase(browser_window_id);
  for (auto it = space_browsers_.begin(); it != space_browsers_.end();) {
    if (it->second == browser_window_id) {
      std::erase(space_order_, it->first);
      it = space_browsers_.erase(it);
    } else {
      ++it;
    }
  }
  if (active_browser_ == browser_window_id) {
    active_browser_ = 0;
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
  phi_framework::PhiFramework* framework = framework_;
  HWND shell = shell_hwnd_;
  std::vector<int64_t> browsers;
  for (const auto& [space_id, browser_id] : space_browsers_) {
    browsers.push_back(browser_id);
  }
  if (!framework || browsers.empty()) {
    if (shell && ::IsWindow(shell)) {
      ::DestroyWindow(shell);
    }
    return;
  }
  for (int64_t browser_id : browsers) {
    framework->CloseBrowserWindow(browser_id);
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
  if (!framework_) {
    return;
  }
  std::optional<base::DictValue> st = base::JSONReader::ReadDict(
      framework_->NavStateJson(active_browser_ ? active_browser_ : window_id_), base::JSON_PARSE_RFC);
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
  if (framework_) {
    const int64_t browser = active_browser_ ? active_browser_ : window_id_;
    std::optional<base::Value> parsed = base::JSONReader::Read(
        framework_->ListExtensionsJson(browser), base::JSON_PARSE_RFC);
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
  const Layout layout = ComputeLayout();
  add_space_button_ = layout.add_space;
  if (!host_ || incognito_) {
    add_space_button_ = RECT{};  // Incognito has no Spaces strip.
    return;
  }
  int x = layout.chips_left;
  for (const PhiSpaceView& space : host_->Spaces()) {
    // Comfortable shows a single compact pip — the ACTIVE Space; the rest
    // live in the popup it opens.
    if (layout.horizontal_tabs && space.id != active_space_) {
      continue;
    }
    if (x + Scaled(kSpaceChip) > layout.chips_limit) {
      // Row is full: the trailing slot becomes the Mac's "…" switcher.
      // The ACTIVE Space must stay visible (the Mac scrolls it into view):
      // evict the last shown pip for it when it fell past the break.
      chips_overflow_ = true;
      if (space.id == active_space_ && !chips_.empty() &&
          !layout.horizontal_tabs) {
        SpaceChip active_chip;
        active_chip.id = space.id;
        active_chip.label = !space.name.empty()
                                ? base::UTF8ToWide(space.name)
                                : L"Space " + base::UTF8ToWide(space.id);
        active_chip.icon = base::UTF8ToWide(space.icon_text);
        active_chip.open = HasSpace(space.id);
        active_chip.bounds = chips_.back().bounds;
        chips_.back() = std::move(active_chip);
      }
      continue;  // Keep scanning so a later active Space can still evict.
    }
    SpaceChip chip;
    chip.id = space.id;
    chip.label = !space.name.empty()
                     ? base::UTF8ToWide(space.name)
                     : L"Space " + base::UTF8ToWide(space.id);
    chip.icon = base::UTF8ToWide(space.icon_text);
    chip.open = HasSpace(space.id);
    chip.bounds = {x, layout.chips_top, x + Scaled(kSpaceChip),
                   layout.chips_top + Scaled(kSpaceChip)};
    chips_.push_back(std::move(chip));
    x += Scaled(kSpaceChip + kSpaceChipGap);
  }
  // Live agent Spaces trail the user Spaces as robot pips (only in a real
  // user window — the agent's own hidden window never lists them). Clicking
  // one surfaces its window so the user can watch the agent work live.
  if (!agent_ && !layout.horizontal_tabs) {
    for (const PhiAgentPipView& pip : host_->AgentSpaces()) {
      if (x + Scaled(kSpaceChip) > layout.chips_limit) {
        chips_overflow_ = true;
        break;
      }
      SpaceChip chip;
      chip.id = pip.task_id;
      chip.is_agent = true;
      chip.open = pip.watched;
      chip.label = pip.caption.empty()
                       ? L"Agent"
                       : base::UTF8ToWide(pip.caption);
      chip.icon = L"\U0001F916";  // 🤖
      chip.bounds = {x, layout.chips_top, x + Scaled(kSpaceChip),
                     layout.chips_top + Scaled(kSpaceChip)};
      chips_.push_back(std::move(chip));
      x += Scaled(kSpaceChip + kSpaceChipGap);
    }
  }
}


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

std::vector<RECT> PhiWindowController::PinnedGridWalk(
    const std::vector<int>& units, const Layout& layout) {
  // The exact placement math of the pinned grid (see RebuildShortcuts),
  // shared with the drag previews so a phantom cell lands where the real
  // one will.
  const int decision = sidebar_dip_ - 8;
  const int column_limit = decision < 236 ? 3
                           : decision < 294 ? 4
                           : decision < 352 ? 5
                                            : 6;
  const int avail = sidebar_dip_ - 2 * kEdgeInset;
  int total_units = 0;
  for (const int u : units) {
    total_units += u;
  }
  int columns = std::max(1, std::min(column_limit, total_units));
  while (columns > 1 &&
         (avail - (columns - 1) * kPinnedGap) / columns < kPinnedMinWidth) {
    --columns;
  }
  const int cell_h = Scaled(kPinnedCellHeight);
  const int gap = Scaled(kPinnedGap);
  const int row_span =
      Scaled(sidebar_dip_) - 2 * Scaled(kEdgeInset) - (columns - 1) * gap;
  const int inset = layout.favorites.left + Scaled(kEdgeInset);
  std::vector<RECT> rects;
  rects.reserve(units.size());
  int col = 0;
  int row_index = -1;
  for (const int u : units) {
    const int cu = std::min(u, columns);
    if (row_index < 0 || col + cu > columns) {
      ++row_index;
      col = 0;
    }
    const int y = layout.favorites.top + gap + row_index * (cell_h + gap);
    rects.push_back({inset + col * gap + col * row_span / columns, y,
                     inset + (col + cu - 1) * gap +
                         (col + cu) * row_span / columns,
                     y + cell_h});
    col += cu;
  }
  return rects;
}

void PhiWindowController::RebuildShortcuts() {
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
    row.partner_guid = std::move(view.partner_guid);
    row.icon = std::move(view.icon);
    row.layout = std::move(view.layout);
    return row;
  };
  for (PhiStoredRowView& view : host_->PinnedRows(window_id_)) {
    pinned_.push_back(consume(std::move(view)));
  }
  // The sidebar is an outline (the Mac's NSOutlineView): an expanded folder
  // splices its children in below itself, one indent step deeper.
  const auto descend = [&](auto&& self, const std::string& parent,
                           int depth) -> void {
    for (PhiStoredRowView& view : host_->BookmarkRows(window_id_, parent)) {
      ShortcutRow row = consume(std::move(view));
      row.depth = depth;
      row.parent_guid = parent;
      row.expanded = row.folder && expanded_bookmarks_.count(row.guid) > 0;
      const bool recurse = row.expanded;
      const std::string guid = row.guid;
      bookmarks_.push_back(std::move(row));
      if (recurse) {
        self(self, guid, depth + 1);
      }
    }
  };
  descend(descend, std::string(), 0);
  bookmark_row_count_ = static_cast<int>(bookmarks_.size());

  // Collapse pinned SPLIT pairs into one merged cell (the Mac's
  // PinnedSplitItem): the first member keeps its row and absorbs the
  // partner as pane B; the partner row is consumed. `url_b` non-empty is
  // what marks a pinned cell as merged from here on.
  for (size_t i = 0; i < pinned_.size(); ++i) {
    if (pinned_[i].partner_guid.empty()) {
      continue;
    }
    for (size_t j = i + 1; j < pinned_.size(); ++j) {
      if (pinned_[j].guid != pinned_[i].partner_guid) {
        continue;
      }
      pinned_[i].url_b = pinned_[j].url;
      pinned_[i].title_b = pinned_[j].title;
      pinned_[i].favicon_b = std::move(pinned_[j].favicon);
      pinned_[i].favicon_b_width = pinned_[j].favicon_width;
      pinned_[i].favicon_b_height = pinned_[j].favicon_height;
      pinned_.erase(pinned_.begin() + j);
      break;
    }
  }

  // A split bookmark's second favicon is never stored — it re-fetches from
  // the history cache, like the Mac's render-time fetch (misses schedule an
  // async load that repaints on arrival).
  if (framework_) {
    const int64_t icon_browser = active_browser_ ? active_browser_
                                                 : window_id_;
    for (ShortcutRow& bookmark : bookmarks_) {
      if (bookmark.url_b.empty()) {
        continue;
      }
      int width = 0, height = 0;
      const unsigned char* pixels = framework_->FaviconForUrlBgra(
          icon_browser, bookmark.url_b.c_str(), &width, &height);
      if (pixels && width > 0 && height > 0) {
        const size_t bytes =
            static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
        // SAFETY: FaviconForUrlBgra reports its own buffer's dimensions.
        const auto view =
            UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
        bookmark.favicon_b.assign(view.begin(), view.end());
        bookmark.favicon_b_width = width;
        bookmark.favicon_b_height = height;
      }
    }
  }

  if (host_->LayoutMode() == PhiLayoutMode::kComfortable) {
    // No sidebar: pins become the strip's leading pinned section — fixed
    // 28pt cells, 2pt apart, centred on the strip row (the Mac's
    // TabStripMetrics.PinnedTab). Normal chips start after them.
    pinned_row_count_ = 0;
    const Layout layout = ComputeLayout();
    const int cell = Scaled(kStripPinSize);
    const int mid_y = layout.tabs_top + Scaled(kHTabHeight) / 2;
    int x = layout.tabs_left;
    for (ShortcutRow& pin : pinned_) {
      // A merged split cell takes two strip slots.
      const int w = pin.url_b.empty() ? cell : 2 * cell + Scaled(kStripPinGap);
      pin.bounds = {x, mid_y - cell / 2, x + w, mid_y + cell / 2};
      x += w + Scaled(kStripPinGap);
    }
    int by = layout.favorites.bottom;
    for (ShortcutRow& row : bookmarks_) {
      row.bounds = {0, by, 0, by};  // Off-strip; bookmarks live in the menu.
    }
    return;
  }

  // Pinned tabs form the Mac's grid (PinnedTabLayout): 45pt cells, 8pt gaps
  // and insets. The width breakpoint (decisionWidth = width − 8: under 236 →
  // 3, under 294 → 4, under 352 → 5, else 6) is only the column LIMIT — the
  // column count is min(limit, pins), shrunk while a cell would dip under the
  // 53pt minimum, and cells STRETCH to fill the row: a lone pin spans the
  // sidebar, two pins split it in half.
  const int decision = sidebar_dip_ - 8;
  const int column_limit = decision < 236 ? 3
                           : decision < 294 ? 4
                           : decision < 352 ? 5
                                            : 6;
  const int avail = sidebar_dip_ - 2 * kEdgeInset;
  // Grid units: a merged split cell spans TWO slots (the Mac's grid
  // width-of-item), clamped to the column count when the sidebar is narrow.
  const auto cell_units = [](const ShortcutRow& pin) {
    return pin.url_b.empty() ? 1 : 2;
  };
  int total_units = 0;
  for (const ShortcutRow& pin : pinned_) {
    total_units += cell_units(pin);
  }
  int columns = std::max(1, std::min(column_limit, total_units));
  while (columns > 1 &&
         (avail - (columns - 1) * kPinnedGap) / columns < kPinnedMinWidth) {
    --columns;
  }
  // Cells walk into rows; one that would overflow its row wraps whole.
  pinned_row_count_ = 0;
  {
    int col = 0;
    int row_index = -1;
    for (const ShortcutRow& pin : pinned_) {
      const int units = std::min(cell_units(pin), columns);
      if (row_index < 0 || col + units > columns) {
        ++row_index;
        col = 0;
      }
      col += units;
    }
    pinned_row_count_ = row_index + 1;
  }
  // Derived only now: the two bands' sizes feed back into where everything
  // below them sits. The cell rects come from PinnedGridWalk — the shared
  // placement math the drag previews also use.
  const Layout layout = ComputeLayout();
  {
    std::vector<int> unit_list;
    unit_list.reserve(pinned_.size());
    for (const ShortcutRow& pin : pinned_) {
      unit_list.push_back(cell_units(pin));
    }
    const std::vector<RECT> cells = PinnedGridWalk(unit_list, layout);
    for (size_t i = 0; i < pinned_.size(); ++i) {
      pinned_[i].bounds = cells[i];
    }
  }

  // Bookmarks are ordinary rows above the open tabs (36pt + the 2pt gap).
  int y = layout.favorites.bottom +
          (pinned_row_count_ > 0 ? Scaled(kFavoritesSpacer) : 0);
  for (ShortcutRow& row : bookmarks_) {
    row.bounds = {layout.tabs_left, y,
                  Scaled(sidebar_dip_ - kEdgeInset),
                  y + Scaled(kTabRowHeight)};
    y += Scaled(kTabRowHeight + kTabRowGap);
  }
}

void PhiWindowController::RebuildTabs() {
  RebuildNavState();
  RefreshFieldExtensions();
  RebuildChips();
  RebuildShortcuts();
  tabs_.clear();
  group_headers_.clear();
  rows_dirty_ = false;
  if (!framework_) {
    return;
  }
  const int64_t browser = active_browser_ ? active_browser_ : window_id_;

  // Parse every live tab first — geometry comes after the pinned bindings
  // have pulled their tabs out of the list.
  std::vector<TabRow> rows;
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
  if (parsed && parsed->is_list()) {
    for (const base::Value& v : parsed->GetList()) {
      if (!v.is_dict()) {
        continue;
      }
      const base::DictValue& d = v.GetDict();
      TabRow row;
      row.index = d.FindInt("index").value_or(0);
      row.id = d.FindInt("id").value_or(0);
      row.active = d.FindBool("active").value_or(false);
      if (const std::string* split = d.FindString("splitId")) {
        row.split_id = *split;
      }
      if (const std::string* group = d.FindString("groupId")) {
        row.group_id = *group;
      }
      if (const std::string* tag = d.FindString("tag")) {
        row.tag = *tag;
      }
      const std::string* title = d.FindString("title");
      const std::string* url = d.FindString("url");
      std::string label = title && !title->empty()
                              ? *title
                              : (url ? *url : std::string("(untitled)"));
      if (url && PhiBookmarkManager::IsManagerUrl(*url)) {
        // The Chromium view behind the native page has no WebUI to load, so
        // its title would be the failed URL; the tab shows the page's name.
        label = "Bookmarks";
      }
      row.title = base::UTF8ToWide(label);
      row.url = url ? base::UTF8ToWide(*url) : std::wstring();
      row.internal_page = url && url->rfind("chrome://", 0) == 0;
      row.unloaded = d.FindBool("unloaded").value_or(false);
      if (row.active) {
        // The bookmark bar's "show on new tab" reads this.
        active_tab_url_ = url ? *url : std::string();
      }
      int fw = 0, fh = 0;
      const unsigned char* pixels =
          framework_->TabFaviconBgra(browser, row.index, &fw, &fh);
      if (pixels && fw > 0 && fh > 0) {
        // The framework's buffer is scratch — valid only until its next call.
        const size_t bytes =
            static_cast<size_t>(fw) * static_cast<size_t>(fh) * 4;
        // SAFETY: same contract — TabFaviconBgra reports the dimensions of
        // the buffer it just returned.
        const auto copy =
            UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
        row.favicon.assign(copy.begin(), copy.end());
        row.favicon_width = fw;
        row.favicon_height = fh;
      }
      rows.push_back(std::move(row));
    }
  }

  // Stored-row bindings, the Mac's customGuid model: a bound tab CARRIES its
  // pin/bookmark guid as its identity tag (PhiTabData, round-tripped by
  // session restore, so bindings survive a restart). A tag matching a live
  // stored row binds the tab to that row's cell. Bookmark bindings act only
  // in the vertical layouts — the Mac hosts them in a bookmark bar the
  // horizontal port doesn't have yet, so there the tab shows as a normal
  // chip and stays reachable.
  const auto is_pin = [this](const std::string& guid) {
    // A merged split cell owns BOTH pane guids (the partner row was
    // consumed into it), so pane B's tag binds here too.
    return std::any_of(pinned_.begin(), pinned_.end(),
                       [&](const ShortcutRow& p) {
                         return p.guid == guid ||
                                (!p.partner_guid.empty() &&
                                 p.partner_guid == guid);
                       });
  };
  const auto is_bookmark = [this](const std::string& guid) {
    return std::any_of(bookmarks_.begin(), bookmarks_.end(),
                       [&](const ShortcutRow& b) {
                         return !b.folder && b.guid == guid;
                       });
  };
  const auto flag_bound = [&rows](ShortcutRow& stored) {
    for (const TabRow& r : rows) {
      if (r.tag == stored.guid) {
        stored.open = true;
        stored.active = stored.active || r.active || r.b_active;
        stored.tab_id = r.id;
      } else if (!stored.partner_guid.empty() &&
                 r.tag == stored.partner_guid) {
        stored.open = true;
        stored.active = stored.active || r.active || r.b_active;
        stored.tab_id_b = r.id;
      }
    }
  };
  for (ShortcutRow& pin : pinned_) {
    flag_bound(pin);
  }
  for (ShortcutRow& bookmark : bookmarks_) {
    flag_bound(bookmark);
  }

  // Merge split partners (Chromium keeps them adjacent): the leading member
  // carries both panes; the partner is consumed and never placed.
  for (size_t i = 0; i + 1 < rows.size(); ++i) {
    TabRow& a = rows[i];
    TabRow& b = rows[i + 1];
    if (a.split_id.empty() || a.split_id != b.split_id || a.b_index >= 0) {
      continue;
    }
    a.b_index = b.index;
    a.b_id = b.id;
    a.b_active = b.active;
    a.b_title = std::move(b.title);
    a.b_url = std::move(b.url);
    a.b_favicon = std::move(b.favicon);
    a.b_favicon_width = b.favicon_width;
    a.b_favicon_height = b.favicon_height;
    a.active = a.active || b.active;
    rows.erase(rows.begin() + i + 1);
  }

  // The bookmark route can only be entered or left by a rebuild; mounting
  // the page here would run inside WM_PAINT, so post and do it after.
  if (shell_hwnd_) {
    const bool mounted = bookmark_manager_ && bookmark_manager_->visible();
    if (mounted != WantsBookmarkPage()) {
      ::PostMessageW(shell_hwnd_, kMsgPhiSyncBookmarkPage, 0, 0);
    }
  }

  // Group visuals for the header rows (title/color/collapsed by id).
  std::map<std::string, GroupHeader> group_info;
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
      info.title = title && !title->empty() ? base::UTF8ToWide(*title)
                                            : palette_entry.label;
      info.collapsed = d->FindBool("collapsed").value_or(false);
      group_info[info.id] = std::move(info);
    }
  }

  // Geometry for the unbound rows only. In the vertical layouts a group's
  // members sit under its header row and vanish while it is collapsed; the
  // horizontal strip has no headers yet, so members stay plain chips there.
  const Layout layout = ComputeLayout();
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
  int htab_rest_w = Scaled(kHTabWidth);
  int htab_active_w = Scaled(kHTabActiveWidth);
  if (layout.horizontal_tabs) {
    int resting = 0;
    bool has_active = false;
    for (const TabRow& row : rows) {
      if (!row.tag.empty() && is_pin(row.tag)) {
        continue;  // Bound to a pinned cell, not on the strip.
      }
      if (row.active) {
        has_active = true;
      } else {
        ++resting;
      }
    }
    const int floor_w = Scaled(kHTabMinWidth);
    const int room = layout.tabs_limit - x;
    if (!has_active) {
      htab_active_w = 0;
    }
    if (resting * htab_rest_w + htab_active_w > room) {
      if (resting > 0) {
        htab_rest_w =
            std::max(floor_w, (room - htab_active_w) / resting);
      }
      if (has_active && resting * htab_rest_w + htab_active_w > room) {
        htab_active_w = std::max(floor_w, room - resting * htab_rest_w);
      }
    }
  }
  std::string current_group;
  bool current_collapsed = false;
  if (!layout.horizontal_tabs) {
    y -= scroll_offset_;
  }
  const auto clip_to_viewport = [&layout](RECT* bounds) {
    if (bounds->bottom <= layout.tabs_top ||
        bounds->top >= layout.tabs_limit) {
      *bounds = RECT{};  // Scrolled out of the tab-list viewport.
    }
  };
  for (TabRow& row : rows) {
    // Bound rows live in their pin/bookmark cell; bookmark bindings release
    // their tab to the strip in the horizontal layout.
    if (!row.tag.empty() &&
        (is_pin(row.tag) ||
         (is_bookmark(row.tag) && !layout.horizontal_tabs))) {
      continue;
    }
    if (!layout.horizontal_tabs && row.group_id != current_group) {
      current_group = row.group_id;
      current_collapsed = false;
      if (!current_group.empty()) {
        auto info = group_info.find(current_group);
        if (info != group_info.end()) {
          GroupHeader header = info->second;
          header.bounds = {layout.tabs_left, y,
                           Scaled(sidebar_dip_ - kEdgeInset),
                           y + Scaled(kGroupHeaderHeight)};
          header.container = header.bounds;
          y += Scaled(kGroupHeaderHeight + kTabRowGap);
          current_collapsed = header.collapsed;
          clip_to_viewport(&header.bounds);
          group_headers_.push_back(std::move(header));
        }
      }
    }
    if (!layout.horizontal_tabs && current_collapsed) {
      continue;  // Hidden under a collapsed header.
    }
    const int close_box = Scaled(kCloseBoxSize);
    if (layout.horizontal_tabs) {
      // The active tab takes the wider slot — it is the one carrying the
      // page, and the extra room is what separates it from its neighbours
      // now that the tabs abut instead of standing apart.
      const int tab_w = row.active ? htab_active_w : htab_rest_w;
      if (x + tab_w > layout.tabs_limit) {
        break;  // Strip is full; overflow handling is future work.
      }
      // The tab runs from the strip's top inset to the band's bottom edge,
      // where the active one merges into the toolbar.
      row.bounds = {x, y, x + tab_w, y + Scaled(kHTabHeight)};
      x += tab_w + Scaled(kHTabGap);
    } else {
      row.bounds = {layout.tabs_left, y,
                    Scaled(sidebar_dip_ - kEdgeInset),
                    y + Scaled(kTabRowHeight)};
      y += Scaled(kTabRowHeight + kTabRowGap);
      clip_to_viewport(&row.bounds);
    }
    const int box_mid = (row.bounds.top + row.bounds.bottom) / 2;
    row.close_box = {row.bounds.right - close_box - Scaled(4),
                     box_mid - close_box / 2,
                     row.bounds.right - Scaled(4),
                     box_mid + close_box / 2};
    if (row.b_index >= 0) {
      // Right half of the merged row activates pane B (clicks in the close
      // box still win — it is tested first).
      row.pane_b = {(row.bounds.left + row.bounds.right) / 2, row.bounds.top,
                    row.bounds.right, row.bounds.bottom};
    }
    if (!layout.horizontal_tabs && !row.group_id.empty() &&
        !group_headers_.empty() && group_headers_.back().id == row.group_id &&
        row.bounds.right > row.bounds.left) {
      group_headers_.back().container.bottom = row.bounds.bottom;
    }
    tabs_.push_back(std::move(row));
  }
  if (layout.horizontal_tabs) {
    // The "+" follows the last tab, centred in the band. It only exists
    // once the strip's width is known, which is here and not in the layout.
    const int btn = Scaled(kNavButton);
    const int gap = Scaled(kTopBarSpacing);
    const int left = x + gap;
    // tabs_limit already held this button's room back from the strip, so it
    // fits unless the tabs overran — clamp rather than draw under search.
    add_tab_button_ = left + btn + gap <= layout.search_tabs.left
                          ? RECT{left, (layout.title_bar.bottom - btn) / 2,
                                 left + btn,
                                 (layout.title_bar.bottom + btn) / 2}
                          : RECT{};
  } else {
    add_tab_button_ = RECT{};
    // Overflow bookkeeping for the wheel handler; the tinted group cards are
    // clamped to the viewport so they never bleed into the fixed bands.
    const int viewport = layout.tabs_limit - layout.tabs_top;
    const int content = y - layout.tabs_top + scroll_offset_;
    tabs_scroll_max_ = content > viewport ? content - viewport : 0;
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
        return row.id == id || (row.b_index >= 0 && row.id == id);
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
    if (!pin.url_b.empty() &&
        (pin.guid == guid || pin.partner_guid == guid)) {
      OpenStoredSplit(pin, /*bookmark=*/false);
      return;
    }
  }
  for (const ShortcutRow& bookmark : bookmarks_) {
    if (!bookmark.url_b.empty() && bookmark.guid == guid) {
      OpenStoredSplit(bookmark, /*bookmark=*/true);
      return;
    }
  }
  const int64_t browser = active_browser_ ? active_browser_ : window_id_;
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

void PhiWindowController::OpenStoredSplit(const ShortcutRow& row,
                                          bool bookmark) {
  if (!framework_) {
    return;
  }
  const int64_t browser = active_browser_ ? active_browser_ : window_id_;
  // A pane binds by tag: the row's guid for pane A; for a pinned pair,
  // the partner row's guid for pane B. A bookmark split's second pane
  // carries NO tag — the Mac's model (its binding dies with the split).
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
  int index_a = -1;
  std::string split_a;
  lookup(row.guid, &index_a, &split_a);

  if (bookmark) {
    if (index_a >= 0) {
      // Already open (split or not): focus the primary, as on Mac.
      framework_->ActivateTab(browser, index_a);
      RefreshTabs();
      return;
    }
    if (row.url.empty() || row.url_b.empty()) {
      return;
    }
    // The Mac's openTwoURLsAsSplit: primary (bound by the bookmark guid),
    // secondary, married.
    index_a = framework_->NewTab(browser, row.url.c_str());
    if (index_a < 0) {
      return;
    }
    framework_->SetTabTag(browser, index_a, row.guid.c_str());
    const int index_b = framework_->NewTab(browser, row.url_b.c_str());
    framework_->ActivateTab(browser, index_a);
    if (index_b >= 0) {
      framework_->SplitActiveWith(browser, index_b);
      ApplySplitLayout(index_a, row.layout);
    }
    RefreshTabs();
    return;
  }

  // Pinned pair: ensure BOTH panes live (created panes bind by their own
  // row guid, the Mac's per-pane customGuid), then marry them if Chromium
  // lost the split — openPinnedSplit's silent re-creation.
  int index_b = -1;
  std::string split_b;
  lookup(row.partner_guid, &index_b, &split_b);
  if (index_a < 0 && !row.url.empty()) {
    index_a = framework_->NewTab(browser, row.url.c_str());
    if (index_a >= 0) {
      framework_->SetTabTag(browser, index_a, row.guid.c_str());
    }
  }
  if (index_b < 0 && !row.url_b.empty() && !row.partner_guid.empty()) {
    index_b = framework_->NewTab(browser, row.url_b.c_str());
    if (index_b >= 0) {
      framework_->SetTabTag(browser, index_b, row.partner_guid.c_str());
    }
  }
  if (index_a < 0 || index_b < 0) {
    if (index_a >= 0 || index_b >= 0) {
      framework_->ActivateTab(browser, index_a >= 0 ? index_a : index_b);
    }
    RefreshTabs();
    return;
  }
  framework_->ActivateTab(browser, index_a);
  if (split_a.empty() || split_a != split_b) {
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
  // The trailing cluster's controls take their own clicks — without this
  // the band would drag the window out from under the pointer instead.
  if (::PtInRect(&layout.back, client_pt) ||
      ::PtInRect(&layout.forward, client_pt) ||
      ::PtInRect(&layout.reload, client_pt) ||
      ::PtInRect(&layout.search_tabs, client_pt)) {
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
      if (::PtInRect(&chip.bounds, client_pt)) {
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

void PhiWindowController::SetMenuRevealed(bool revealed) {
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

void PhiWindowController::SetMenuHot(int index) {
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
        menu_bar_->ShowAt(i, anchor.x, anchor.y);
        // The popup is modal, so the bar stayed drawn throughout; Alt's
        // sticky reveal has done its job by the time it closes.
        menu_alt_revealed_ = false;
        return;
      }
    }
    return;  // The rest of the band is drag space; no chrome lives here.
  }
  if (framework_ && ::PtInRect(&layout.back, pt)) {
    framework_->GoBack(active_browser_ ? active_browser_ : window_id_);
    RefreshTabs();
    return;
  }
  if (framework_ && ::PtInRect(&layout.forward, pt)) {
    framework_->GoForward(active_browser_ ? active_browser_ : window_id_);
    RefreshTabs();
    return;
  }
  if (framework_ && ::PtInRect(&layout.reload, pt)) {
    framework_->Reload(active_browser_ ? active_browser_ : window_id_, /*bypass_cache=*/false);
    RefreshTabs();
    return;
  }
  if (::PtInRect(&layout.search_tabs, pt)) {
    PhiSearchTabsPanel::Toggle(this);  // The header's magnifier.
    return;
  }
  if (::PtInRect(&layout.downloads, pt)) {
    // The Mac DownloadButtonView's popover toggle.
    if (host_ && framework_) {
      RECT anchor = layout.downloads;
      ::MapWindowPoints(shell_hwnd_, nullptr,
                        reinterpret_cast<POINT*>(&anchor), 2);
      PhiDownloadsPopover::Toggle(shell_hwnd_, anchor,
                                  host_->PaletteForSlot(window_id_),
                                  framework_,
                                  active_browser_ ? active_browser_
                                                  : window_id_);
    }
    return;
  }
  if (::PtInRect(&layout.extensions, pt)) {
    // The extensions popover, off the puzzle button.
    if (host_ && framework_) {
      RECT anchor = layout.extensions;
      ::MapWindowPoints(shell_hwnd_, nullptr,
                        reinterpret_cast<POINT*>(&anchor), 2);
      PhiExtensionsPopover::Toggle(shell_hwnd_, anchor,
                                   host_->PaletteForSlot(window_id_),
                                   framework_,
                                   active_browser_ ? active_browser_
                                                   : window_id_);
    }
    return;
  }
  for (size_t i = 0; i < field_extensions_.size(); ++i) {
    const RECT slot = FieldExtRect(layout, i);
    if (slot.right > slot.left && ::PtInRect(&slot, pt)) {
      // A pinned extension's in-field icon: run its action anchored at the
      // icon (a popup-bearing action opens Chromium's popup there).
      if (framework_) {
        RECT anchor = slot;
        ::MapWindowPoints(shell_hwnd_, nullptr,
                          reinterpret_cast<POINT*>(&anchor), 2);
        framework_->TriggerExtensionAction(
            active_browser_ ? active_browser_ : window_id_,
            field_extensions_[i].id.c_str(), anchor.left, anchor.top,
            anchor.right - anchor.left, anchor.bottom - anchor.top);
      }
      return;
    }
  }
  if (::PtInRect(&layout.reader, pt)) {
    if (framework_) {
      const int64_t browser = active_browser_ ? active_browser_ : window_id_;
      for (const TabRow& row : tabs_) {
        if (row.active) {
          framework_->DistillTab(browser, row.index);
          break;
        }
      }
      RefreshTabs();
    }
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
      ArmDrag(DragKind::kPin, static_cast<int>(&pin - pinned_.data()), pt);
      OpenOrFocusPinned(pin.guid, pin.url);
      return;
    }
  }
  for (const ShortcutRow& bookmark : bookmarks_) {
    if (::PtInRect(&bookmark.bounds, pt)) {
      ArmDrag(DragKind::kBookmark,
              static_cast<int>(&bookmark - bookmarks_.data()), pt);
      if (bookmark.folder) {
        // A folder click discloses/collapses its children (the Mac's
        // toggleItemExpanded). Children splice in AFTER the folder row, so
        // the armed drag index stays valid across the rebuild.
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
    menu_bar_->ShowOverflow(anchor.x, anchor.y);
    return;
  }
  RebuildTabs();  // Chips and rows may be stale if no paint has run yet.
  for (const SpaceChip& chip : chips_) {
    if (!::PtInRect(&chip.bounds, pt)) {
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
      ArmDrag(DragKind::kChip, static_cast<int>(&chip - chips_.data()), pt);
    }
    if (layout.horizontal_tabs) {
      // Comfortable draws ONE compact pip — the active Space. Clicking it
      // opens the switcher popup over the FULL Space list, the Mac's
      // SpacesStripView compact-chip behaviour.
      std::vector<std::pair<std::string, std::wstring>> spaces;
      if (host_) {
        for (const PhiSpaceView& space : host_->Spaces()) {
          std::wstring title =
              base::UTF8ToWide(!space.name.empty()
                                   ? space.name
                                   : "Space " + space.id);
          const std::wstring glyph = base::UTF8ToWide(space.icon_text);
          if (!glyph.empty() && !PhiIsPrivateGlyph(glyph)) {
            title = glyph + L"  " + title;
          }
          spaces.emplace_back(space.id, std::move(title));
        }
      }
      HMENU menu = ::CreatePopupMenu();
      if (!menu || spaces.empty()) {
        if (menu) {
          ::DestroyMenu(menu);
        }
        return;
      }
      for (size_t i = 0; i < spaces.size(); ++i) {
        ::AppendMenuW(menu,
                      MF_STRING | (spaces[i].first == active_space_
                                       ? MF_CHECKED
                                       : 0),
                      static_cast<UINT>(i) + 1, spaces[i].second.c_str());
      }
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x,
          screen.y, 0, shell_hwnd_, nullptr));
      ::DestroyMenu(menu);
      if (chosen >= 1 && chosen <= spaces.size()) {
        const std::string space_id = spaces[chosen - 1].first;
        if (HasSpace(space_id)) {
          ShowSpace(space_id);
        } else if (host_) {
          host_->RequestSwitchSpace(window_id_, space_id);
        }
      }
      RefreshTabs();
      return;
    }
    if (chip.open) {
      ShowSpace(chip.id);
    } else if (host_) {
      // Not yet in this slot: the owner creates its browser in the Space's
      // profile and attaches it here.
      host_->RequestSwitchSpace(window_id_, chip.id);
    }
    RefreshTabs();
    return;
  }
  if (host_ && ::PtInRect(&layout.add_space, pt)) {
    if (!chips_overflow_) {
      host_->RequestCreateSpace(window_id_, /*new_profile=*/false);
      RefreshTabs();
      return;
    }
    // Overflow: the "…" opens the full switcher — every Space plus a
    // "New Space" row (the Mac moreButton's menu).
    std::vector<std::pair<std::string, std::wstring>> spaces;
    for (const PhiSpaceView& space : host_->Spaces()) {
      std::wstring title = base::UTF8ToWide(
          !space.name.empty() ? space.name : "Space " + space.id);
      const std::wstring glyph = base::UTF8ToWide(space.icon_text);
      if (!glyph.empty() && !PhiIsPrivateGlyph(glyph)) {
        title = glyph + L"  " + title;
      }
      spaces.emplace_back(space.id, std::move(title));
    }
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    for (size_t i = 0; i < spaces.size(); ++i) {
      ::AppendMenuW(menu,
                    MF_STRING |
                        (spaces[i].first == active_space_ ? MF_CHECKED : 0),
                    static_cast<UINT>(i) + 1, spaces[i].second.c_str());
    }
    const UINT kNewSpace = static_cast<UINT>(spaces.size()) + 1;
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kNewSpace, L"New Space");
    POINT screen = {layout.add_space.left, layout.add_space.bottom};
    ::ClientToScreen(shell_hwnd_, &screen);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x,
        screen.y, 0, shell_hwnd_, nullptr));
    ::DestroyMenu(menu);
    if (chosen == kNewSpace) {
      host_->RequestCreateSpace(window_id_, /*new_profile=*/false);
    } else if (chosen >= 1 && chosen <= spaces.size()) {
      const std::string space_id = spaces[chosen - 1].first;
      if (HasSpace(space_id)) {
        ShowSpace(space_id);
      } else {
        host_->RequestSwitchSpace(window_id_, space_id);
      }
    }
    RefreshTabs();
    return;
  }
  for (const GroupHeader& gh : group_headers_) {
    if (::PtInRect(&gh.bounds, pt)) {
      if (framework_) {
        framework_->SetTabGroupCollapsed(
            active_browser_ ? active_browser_ : window_id_, gh.id.c_str(),
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
      const int64_t browser = active_browser_ ? active_browser_ : window_id_;
      if (::PtInRect(&row.close_box, pt)) {
        if (row.b_index >= 0) {
          // Closing a merged row takes both panes; the higher index first so
          // the lower one keeps its meaning.
          framework_->CloseTab(browser, row.index > row.b_index ? row.index
                                                                : row.b_index);
          framework_->CloseTab(browser, row.index > row.b_index ? row.b_index
                                                                : row.index);
        } else {
          framework_->CloseTab(browser, row.index);
        }
      } else if (row.split_id.empty() && (::GetKeyState(VK_MENU) & 0x8000)) {
        // Alt-click, the Mac's option-click: split this tab with the active
        // one (performSplitAction).
        framework_->SplitActiveWith(browser, row.index);
      } else if (row.b_index >= 0 && ::PtInRect(&row.pane_b, pt)) {
        framework_->ActivateTab(browser, row.b_index);
      } else {
        ArmDrag(DragKind::kTab, static_cast<int>(&row - tabs_.data()), pt);
        framework_->ActivateTab(browser, row.index);
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
    const int64_t browser = active_browser_ ? active_browser_ : window_id_;
    if (row.b_index >= 0) {
      framework_->CloseTab(browser, row.index > row.b_index ? row.index
                                                            : row.b_index);
      framework_->CloseTab(browser, row.index > row.b_index ? row.b_index
                                                            : row.index);
    } else {
      framework_->CloseTab(browser, row.index);
    }
    RefreshTabs();
    return;
  }
}

namespace {

// A filled rounded rect, the Mac client's selection/field shape.
// A fill rounded on its TOP corners only, square along the bottom — the
// comfortable tab's shape, which has to sit flush on the toolbar it merges
// into. Drawing the round rect a radius taller and clipping to `r` puts the
// arcs where they belong and leaves the bottom edge straight.
void FillRoundedTop(HDC dc, const RECT& r, COLORREF fill, int radius) {
  if (r.right <= r.left || r.bottom <= r.top) {
    return;
  }
  const int save = ::SaveDC(dc);
  ::IntersectClipRect(dc, r.left, r.top, r.right + 1, r.bottom + 1);
  HBRUSH brush = ::CreateSolidBrush(fill);
  HGDIOBJ old_brush = ::SelectObject(dc, brush);
  HGDIOBJ old_pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
  ::RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1 + radius, radius,
              radius);
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  ::DeleteObject(brush);
  ::RestoreDC(dc, save);
}

void FillRounded(HDC dc, const RECT& r, COLORREF fill, int radius) {
  if (r.right <= r.left || r.bottom <= r.top) {
    return;
  }
  HBRUSH brush = ::CreateSolidBrush(fill);
  HGDIOBJ old_brush = ::SelectObject(dc, brush);
  HGDIOBJ old_pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
  ::RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  ::DeleteObject(brush);
}

// A rounded rect with a hairline border — the address field.
void StrokeRounded(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
                   int radius) {
  if (r.right <= r.left || r.bottom <= r.top) {
    return;
  }
  HBRUSH brush = ::CreateSolidBrush(fill);
  HPEN pen = ::CreatePen(PS_SOLID, 1, border);
  HGDIOBJ old_brush = ::SelectObject(dc, brush);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  ::RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  ::DeleteObject(pen);
  ::DeleteObject(brush);
}

void DrawGlyph(HDC dc, const RECT& r, const wchar_t* glyph, COLORREF ink) {
  if (r.right <= r.left) {
    return;
  }
  ::SetTextColor(dc, ink);
  ::DrawTextW(dc, glyph, -1, const_cast<RECT*>(&r),
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
  // "default" wears the plain folder, open or closed like the Mac's
  // fallback art; anything else is its catalog symbol.
  const PhiMacIcon* mask = resolved == PhiFolderIconDefaultId()
                               ? PhiPlainFolderMask(expanded)
                               : PhiFolderIconMask(resolved);
  if (mask) {
    const int px = std::min(slot.right - slot.left, slot.bottom - slot.top);
    if (px > 0) {
      RECT art = {slot.left + ((slot.right - slot.left) - px) / 2,
                  slot.top + ((slot.bottom - slot.top) - px) / 2, 0, 0};
      art.right = art.left + px;
      art.bottom = art.top + px;
      const std::vector<unsigned char> bgra =
          PhiTintedIconBgra(*mask, ink, px);
      if (!bgra.empty()) {
        PhiBlendBgra(dc, art, bgra.data(), px, px);
        return;
      }
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
    if (::PtInRect(&chip.bounds, pt)) {
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
                      kColorFirst + i, kGroupColors[i].label);
      }
      ::AppendMenuW(menu, MF_STRING, kRename, L"Rename Group...");
      ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(colors),
                    L"Change Color");
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      ::AppendMenuW(menu, MF_STRING, kUngroup, L"Ungroup");
      ::AppendMenuW(menu, MF_STRING, kCloseGroup, L"Close Group");
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN |
                    TPM_RIGHTBUTTON,
          screen.x, screen.y, 0, shell_hwnd_, nullptr));
      const std::string group_id = gh.id;
      const std::wstring title = gh.title;
      ::DestroyMenu(menu);  // Owns the submenu.
      const int64_t browser = active_browser_ ? active_browser_ : window_id_;
      if (!framework_) {
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
                    kHandBack };
      // Dynamic Move/Clone-to-Space rows ride these ranges (99 each).
      constexpr UINT kMoveSpaceFirst = 100;
      constexpr UINT kCloneSpaceFirst = 200;
      std::vector<std::string> space_targets;
      HMENU menu = ::CreatePopupMenu();
      if (!menu) {
        return;
      }
      const int64_t browser = active_browser_ ? active_browser_ : window_id_;
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
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        if (row.group_id.empty()) {
          ::AppendMenuW(menu, MF_STRING, kGroup, L"New Tab Group");
        } else {
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
      const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN |
                    TPM_RIGHTBUTTON,
          screen.x, screen.y, 0, shell_hwnd_, nullptr));
      ::DestroyMenu(menu);
      if (chosen >= kMoveSpaceFirst &&
          chosen < kMoveSpaceFirst + space_targets.size()) {
        host_->RequestTabToSpace(window_id_, row.index,
                                 space_targets[chosen - kMoveSpaceFirst],
                                 /*move=*/true);
      } else if (chosen >= kCloneSpaceFirst &&
                 chosen < kCloneSpaceFirst + space_targets.size()) {
        host_->RequestTabToSpace(window_id_, row.index,
                                 space_targets[chosen - kCloneSpaceFirst],
                                 /*move=*/false);
      } else if (chosen == kPin) {
        host_->RequestStoreTab(window_id_, row.index, /*as_bookmark=*/false);
      } else if (chosen == kBookmark) {
        host_->RequestStoreTab(window_id_, row.index, /*as_bookmark=*/true);
      } else if (chosen == kPinSplit) {
        host_->RequestStoreSplit(window_id_, row.index, row.b_index,
                                 /*as_bookmark=*/false);
      } else if (chosen == kBookmarkSplit) {
        host_->RequestStoreSplit(window_id_, row.index, row.b_index,
                                 /*as_bookmark=*/true);
      } else if (chosen == kDuplicate && framework_) {
        // A split duplicates as a fresh split (both panes), as on Mac —
        // in the same arrangement as the one it came from.
        const bool stacked = merged && SplitIsStacked(row.id);
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
      } else if (chosen == kSplitCurrent && framework_) {
        framework_->SplitActiveWith(browser, row.index);
        RefreshTabs();
      } else if (chosen == kOpenAsSplit && framework_) {
        // A fresh blank pane beside this tab, the Mac's openAsSplit.
        const int fresh = framework_->NewTab(browser, "");
        framework_->ActivateTab(browser, row.index);
        if (fresh >= 0) {
          framework_->SplitActiveWith(browser, fresh);
        }
        RefreshTabs();
      } else if (chosen == kTakeControl && host_) {
        host_->RequestReclaimTab(row.id);
      } else if (chosen == kHandBack && host_) {
        host_->RequestReleaseTab(row.id);
      } else if (chosen == kConvertSplit && framework_) {
        // Toggle the divider axis, and remember it on the stored rows the
        // panes are bound to so the split reopens the same way.
        const std::string split_id = SplitIdForTab(row.id);
        if (!split_id.empty()) {
          const bool stacked = SplitIsStacked(row.id);
          const char* next = stacked ? "vertical" : "horizontal";
          framework_->UpdateSplitView(browser, split_id.c_str(), next, -1.0);
          if (host_ && !row.tag.empty()) {
            host_->SetStoredRowLayout(row.tag, next);
          }
          RefreshTabs();
        }
      } else if (chosen == kRemoveSplit && framework_) {
        framework_->Unsplit(browser, row.index);
        RefreshTabs();
      } else if (chosen == kGroup && framework_) {
        framework_->AddTabToNewGroup(browser, row.index);
        RefreshTabs();
      } else if (chosen == kUngroupTab && framework_) {
        framework_->RemoveTabFromGroup(browser, row.index);
        RefreshTabs();
      } else if (chosen == kClose && framework_) {
        if (merged) {
          framework_->CloseTab(browser, row.index > row.b_index
                                            ? row.index
                                            : row.b_index);
          framework_->CloseTab(browser, row.index > row.b_index
                                            ? row.b_index
                                            : row.index);
        } else {
          framework_->CloseTab(browser, row.index);
        }
        RefreshTabs();
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
            if (index < 0 || index == row.index ||
                (merged && index == row.b_index) ||
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
                    kCopyRight, kRename, kClose, kConvertSplit };
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
      const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN |
                    TPM_RIGHTBUTTON,
          screen.x, screen.y, 0, shell_hwnd_, nullptr));
      const std::string guid = pin.guid;
      const std::string partner = pin.partner_guid;
      const std::string url = pin.url;
      const std::string url_b = pin.url_b;
      const std::wstring pin_title = pin.title;
      const std::string pin_layout = pin.layout;
      const int pin_tab_id = pin.tab_id;
      ::DestroyMenu(menu);
      const int64_t browser = active_browser_ ? active_browser_ : window_id_;
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
      } else if (chosen == kConvertSplit && host_) {
        // Flip what the pair REMEMBERS, and the live split too when it is
        // currently open — the Mac's convertClosedPinnedSplitLayout.
        const char* next =
            pin_layout == "horizontal" ? "vertical" : "horizontal";
        host_->SetStoredRowLayout(guid, next);
        if (framework_ && pin_tab_id != 0) {
          const std::string split_id = SplitIdForTab(pin_tab_id);
          if (!split_id.empty()) {
            framework_->UpdateSplitView(browser, split_id.c_str(), next,
                                        -1.0);
          }
        }
        RefreshTabs();
      } else if (chosen == kUnpin && !guid.empty()) {
        // Dropping the store row also drops the binding at the next rebuild,
        // so a live bound tab returns to the tab list as a normal tab. A
        // split pair goes as one unit.
        host_->RemoveStoredRow(guid);
      } else if (chosen == kClose && framework_) {
        // Close the bound live tab(s); the pinned record stays, as on Mac.
        const int index_a = IndexOfTagged(browser, guid);
        const int index_b =
            partner.empty() ? -1 : IndexOfTagged(browser, partner);
        if (index_a >= 0 && index_b >= 0) {
          framework_->CloseTab(browser,
                               index_a > index_b ? index_a : index_b);
          framework_->CloseTab(browser,
                               index_a > index_b ? index_b : index_a);
        } else if (index_a >= 0 || index_b >= 0) {
          framework_->CloseTab(browser, index_a >= 0 ? index_a : index_b);
        }
        RefreshTabs();
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
                    kConvertSplit };
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
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      ::AppendMenuW(menu, MF_STRING, kRemove,
                    bookmark.folder ? L"Delete Folder" : L"Delete");
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN |
                    TPM_RIGHTBUTTON,
          screen.x, screen.y, 0, shell_hwnd_, nullptr));
      const std::string guid = bookmark.guid;
      const std::string url = bookmark.url;
      const std::string url_b = bookmark.url_b;
      const std::wstring bookmark_title = bookmark.title;
      const bool folder = bookmark.folder;
      const std::string row_icon = bookmark.icon;
      const std::string row_layout = bookmark.layout;
      const int row_tab_id = bookmark.tab_id;
      const RECT row_bounds = bookmark.bounds;
      ::DestroyMenu(menu);
      const int64_t browser = active_browser_ ? active_browser_ : window_id_;
      if (chosen == kOpen && !folder && !url.empty()) {
        OpenOrFocusPinned(guid, url);
      } else if (chosen == kOpenSplit && !folder && framework_ &&
                 !url.empty()) {
        if (split_row) {
          // A split bookmark opens as its own pair either way.
          OpenOrFocusPinned(guid, url);
        } else {
          // Pair the bookmark with the currently focused tab (the Mac's
          // openAsSplit on a bookmark row).
          int current = -1;
          for (const TabRow& r : tabs_) {
            if (r.active && r.b_index < 0 && r.split_id.empty()) {
              current = r.index;
              break;
            }
          }
          const int fresh = framework_->NewTab(browser, url.c_str());
          if (current >= 0 && fresh >= 0) {
            framework_->ActivateTab(browser, current);
            framework_->SplitActiveWith(browser, fresh);
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
      } else if (chosen == kConvertSplit && host_) {
        const char* next =
            row_layout == "horizontal" ? "vertical" : "horizontal";
        host_->SetStoredRowLayout(guid, next);
        if (framework_ && row_tab_id != 0) {
          const std::string split_id = SplitIdForTab(row_tab_id);
          if (!split_id.empty()) {
            framework_->UpdateSplitView(browser, split_id.c_str(), next,
                                        -1.0);
          }
        }
        RefreshTabs();
      } else if (chosen == kChangeIcon && folder) {
        // The picker hangs off the row, as the Mac's popover does.
        POINT top_left = {row_bounds.left, row_bounds.top};
        POINT bottom_right = {row_bounds.right, row_bounds.bottom};
        ::ClientToScreen(shell_hwnd_, &top_left);
        ::ClientToScreen(shell_hwnd_, &bottom_right);
        const RECT anchor = {top_left.x, top_left.y, bottom_right.x,
                             bottom_right.y};
        const std::string picked =
            PhiPickFolderIcon(this, anchor, row_icon);
        if (!picked.empty()) {
          host_->SetStoredRowIcon(guid, picked);
        }
      } else if (chosen == kNewFolder && folder) {
        std::wstring name = L"Untitled";
        if (PhiPromptText(shell_hwnd_, L"New Folder", L"Folder name:",
                          &name)) {
          // Disclose the parent so the new child is visible where it lands.
          expanded_bookmarks_.insert(guid);
          host_->AddBookmarkFolder(window_id_, guid, base::WideToUTF8(name));
        }
      } else if (chosen == kRemove && !guid.empty()) {
        host_->RemoveStoredRow(guid);  // A folder takes its subtree.
      }
      return;
    }
    // The sidebar background — the Mac tab-area menu's bookmark block
    // (TabAreaContextMenuHelper): root folder creation lives here.
    const Layout layout = ComputeLayout();
    if (!layout.horizontal_tabs && pt.x < layout.sidebar.right &&
        pt.y > layout.title_bar.bottom) {
      enum : UINT { kNewRootFolder = 1 };
      HMENU menu = ::CreatePopupMenu();
      if (!menu) {
        return;
      }
      ::AppendMenuW(menu, MF_STRING, kNewRootFolder, L"New Folder...");
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN |
                    TPM_RIGHTBUTTON,
          screen.x, screen.y, 0, shell_hwnd_, nullptr));
      ::DestroyMenu(menu);
      if (chosen == kNewRootFolder) {
        std::wstring name = L"Untitled";
        if (PhiPromptText(shell_hwnd_, L"New Folder", L"Folder name:",
                          &name)) {
          host_->AddBookmarkFolder(window_id_, std::string(),
                                   base::WideToUTF8(name));
        }
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
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        agent_menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x,
        screen.y, 0, shell_hwnd_, nullptr));
    ::DestroyMenu(agent_menu);
    const std::string task = target->id;  // The handlers can rebuild chips_.
    if (chosen == kTranscript) {
      host_->RequestShowTranscript(task);
    } else if (chosen == kFinish) {
      host_->RequestAgentFinish(task);
      RefreshTabs();
    }
    return;
  }

  // The Mac's Space row menu: activate, rename, change icon, set theme,
  // delete. Rename prompts, delete confirms (refused for the default Space);
  // Change Icon lands on Settings > Spaces with this Space selected — the
  // detail column there IS the picker.
  enum : UINT {
    kActivate = 1,
    kRename,
    kChangeIcon,
    kDelete,
    kThemeFollow,
    kThemeFirst,  // + index into the built-in themes.
  };
  HMENU menu = ::CreatePopupMenu();
  HMENU themes = ::CreatePopupMenu();
  if (!menu || !themes) {
    if (menu) {
      ::DestroyMenu(menu);
    }
    if (themes) {
      ::DestroyMenu(themes);
    }
    return;
  }
  const std::string pinned = host_->SpaceThemeId(target->id);
  ::AppendMenuW(themes, MF_STRING | (pinned.empty() ? MF_CHECKED : 0),
                kThemeFollow, L"Follow app theme");
  ::AppendMenuW(themes, MF_SEPARATOR, 0, nullptr);
  const auto& built_in = PhiBuiltInThemes();
  for (size_t i = 0; i < built_in.size(); ++i) {
    const bool on = pinned == built_in[i].id;
    ::AppendMenuW(themes, MF_STRING | (on ? MF_CHECKED : 0),
                  kThemeFirst + static_cast<UINT>(i),
                  base::UTF8ToWide(built_in[i].name).c_str());
  }

  const bool active = target->id == active_space_;
  ::AppendMenuW(menu, MF_STRING | (active ? MF_DISABLED | MF_GRAYED : 0),
                kActivate, L"Switch to this Space");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kRename, L"Rename…");
  ::AppendMenuW(menu, MF_STRING, kChangeIcon, L"Change Icon…");
  ::AppendMenuW(menu, MF_POPUP | MF_STRING,
                reinterpret_cast<UINT_PTR>(themes), L"Set Theme");
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu,
                MF_STRING |
                    (target->id == "1" ? MF_DISABLED | MF_GRAYED : 0u),
                kDelete, L"Delete Space");

  POINT screen = pt;
  ::ClientToScreen(shell_hwnd_, &screen);
  // TPM_RETURNCMD keeps the choice here rather than routing a WM_COMMAND back
  // through a window proc that has no other commands to handle.
  const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
      screen.x, screen.y, 0, shell_hwnd_, nullptr));
  // `chips_` is rebuilt below; keep what the handlers need.
  const std::string space_id = target->id;
  std::wstring space_name = target->label;
  ::DestroyMenu(menu);  // Takes the submenu with it.

  if (chosen == kActivate) {
    if (HasSpace(space_id)) {
      ShowSpace(space_id);
    } else {
      host_->RequestSwitchSpace(window_id_, space_id);
    }
  } else if (chosen == kRename) {
    if (PhiPromptText(shell_hwnd_, L"Rename Space",
                      L"Enter a new name for this Space.", &space_name)) {
      host_->RenameSpace(space_id, base::WideToUTF8(space_name));
    }
  } else if (chosen == kChangeIcon) {
    host_->RequestSpaceIconPicker(space_id);
  } else if (chosen == kDelete) {
    if (PhiConfirm(shell_hwnd_, L"Delete Space",
                   L"Bookmarks and pinned tabs belonging to this Space "
                   L"will also be removed. This action cannot be undone.")) {
      host_->DeleteSpace(space_id);
    }
  } else if (chosen == kThemeFollow) {
    host_->SetSpaceTheme(space_id, std::string());
  } else if (chosen >= kThemeFirst &&
             chosen < kThemeFirst + built_in.size()) {
    host_->SetSpaceTheme(space_id, built_in[chosen - kThemeFirst].id);
  }
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
  rows_dirty_ = true;  // The shell's next paint recomputes collapsed bounds.
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
  auto* self = reinterpret_cast<PhiWindowController*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  // Events forward through the docked sidebar's handlers under the
  // expanded-layout override, shifted by the panel's band offset.
  const auto forward = [&](auto&& call) {
    self->floating_layout_ = true;
    self->RebuildTabs();
    call();
    self->floating_layout_ = false;
    self->rows_dirty_ = true;
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
          self->CancelDrag();  // Drags never start from the panel.
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
  if (placeholder_browsers_.count(active_browser_ ? active_browser_
                                                  : window_id_)) {
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
    ::DrawTextW(dc, L"Bookmarks you add appear here", -1, &hint,
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
    ::DrawTextW(dc, row.title.c_str(), -1, &text,
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
         host_->BookmarkRows(window_id_, parent)) {
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
  const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, 0,
      shell_hwnd_, nullptr));
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
  // The sidebar address bar rests on the 4% wash (sidebarTabHovered), NOT a
  // bordered white box — SideAddressBar's showBackgroundWhenInactive.
  const COLORREF kFieldBg = palette.hover;
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
    HPEN border_pen = ::CreatePen(PS_SOLID, 1, palette.border);
    HGDIOBJ old_frame_pen = ::SelectObject(dc, border_pen);
    HGDIOBJ old_frame_brush =
        ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    const int ellipse = 2 * Scaled(kContentRadius);
    ::RoundRect(dc, frame_rc.left, frame_rc.top, frame_rc.right,
                frame_rc.bottom, ellipse, ellipse);
    ::SelectObject(dc, old_frame_brush);
    ::SelectObject(dc, old_frame_pen);
    ::DeleteObject(border_pen);
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
    static HICON logo = nullptr;
    if (logo_bgra.empty() && !logo) {
      logo = PhiAppIcon(logo_size);  // Fallback: the nearest ICO entry.
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
        {
          const int logo_x = title_rc.left + Scaled(kMenuTitlePad);
          const int logo_y = (title_rc.bottom + title_rc.top - logo_size) / 2;
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
        }
        text_rc.left += Scaled(kMenuTitlePad) + logo_size + Scaled(6);
        ::DrawTextW(dc, menu_titles_[i].first.c_str(), -1, &text_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        continue;
      }
      ::DrawTextW(dc, menu_titles_[i].first.c_str(), -1, &text_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
    DrawGlyph(dc, layout.caption_min, L"\uE921", kLabel);
    DrawGlyph(dc, layout.caption_max, zoomed ? L"\uE923" : L"\uE922", kLabel);
    DrawGlyph(dc, layout.caption_close, L"\uE8BB",
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
      HGDIOBJ old_brush = ::SelectObject(dc, bg);
      HGDIOBJ old_pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
      ::RoundRect(dc, band.left, band.top, band.right + 1, band.bottom + 1,
                  radius, radius);
      RECT lower = {band.left, band.top + radius, band.right, band.bottom};
      ::FillRect(dc, &lower, bg);
      ::SelectObject(dc, old_pen);
      ::SelectObject(dc, old_brush);
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
      DrawGlyph(dc, button.rect, button.glyph,
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
  if (layout.mode == PhiLayoutMode::kPerformance) {
    // Resting 4% wash, brightening to the emphasized 8% under the cursor
    // (SideAddressBar's hoveredColor).
    FillRounded(dc, layout.url_field,
                url_field_hover_ ? palette.hover_strong : kFieldBg,
                Scaled(kCornerRadius));
  } else if (layout.mode == PhiLayoutMode::kComfortable) {
    // Chrome's omnibox: a full pill standing on the toolbar, on the same 4%
    // wash the sidebar field uses, brightening under the cursor. Balanced's
    // field stays bare — it has the card's own ground behind it; this one
    // has only the flat toolbar, and would vanish into it.
    const int pill_r = (layout.url_field.bottom - layout.url_field.top) / 2;
    FillRounded(dc, layout.url_field,
                url_field_hover_ ? palette.hover_strong : kFieldBg, pill_r);
    // The page's globe leads the pill, where Chrome puts its site chip.
    if (layout.page_icon.right > layout.page_icon.left) {
      HGDIOBJ previous_icon_font =
          caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
      DrawGlyph(dc, layout.page_icon, L"\uE774", kLabelDim);
      if (previous_icon_font) {
        ::SelectObject(dc, previous_icon_font);
      }
    }
  } else if (url_field_hover_ &&
             layout.url_field.right > layout.url_field.left) {
    // The top-bar field rests BARE and takes the wash only under the
    // cursor (the Mac top bar's transparent-until-hovered field).
    FillRounded(dc, layout.url_field, palette.hover, Scaled(kCornerRadius));
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
    // Stop before the in-field trailing stack when it exists.
    url_rc.right =
        (layout.field_ext_strip.right > layout.field_ext_strip.left
             ? layout.field_ext_strip.left
             : (layout.copy_link.right > layout.copy_link.left
                    ? layout.copy_link.left
                    : url_rc.right)) -
        Scaled(8);
    ::DrawTextW(dc, shown.empty() ? L"Search or Enter URL" : shown.c_str(),
                -1, &url_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
  }
  // The pinned-extension icons leading the field's trailing stack: 16pt
  // icons in 24pt slots on the radius-4 hover wash (SideAddressBar's
  // extensionIconsStackView).
  if (layout.field_ext_strip.right > layout.field_ext_strip.left) {
    const COLORREF field_fill =
        url_field_hover_ ? palette.hover_strong : kFieldBg;
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
    HGDIOBJ previous_reader_font =
        chip_icon_font_ ? ::SelectObject(dc, chip_icon_font_) : nullptr;
    if (hover_kind_ == HoverKind::kNav && hover_index_ == 11) {
      FillRounded(dc, layout.reader, palette.hover, Scaled(4));
    }
    DrawGlyph(dc, layout.reader, L"\uE736", kLabel);  // Reading mode.
    if (previous_reader_font) {
      ::SelectObject(dc, previous_reader_font);
    }
  }
  if (layout.copy_link.right > layout.copy_link.left) {
    HGDIOBJ previous_link_font =
        chip_icon_font_ ? ::SelectObject(dc, chip_icon_font_) : nullptr;
    if (hover_kind_ == HoverKind::kNav && hover_index_ == 9) {
      FillRounded(dc, layout.copy_link, palette.hover, Scaled(4));
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
  for (const SpaceChip& chip : chips_) {
    // A watched agent pip wears the selection pill too, so "which agent am I
    // watching" reads at a glance.
    if (chip.id != active_space_ && !(chip.is_agent && chip.open)) {
      continue;
    }
    RECT pill = chip.bounds;
    if (!chip.is_agent && swap_start_ &&
        swap_chip_from_.right > swap_chip_from_.left) {
      const double t = SwapEasedProgress();
      const auto mix = [t](LONG from, LONG to) {
        return from + static_cast<LONG>(std::lround((to - from) * t));
      };
      pill.left = mix(swap_chip_from_.left, chip.bounds.left);
      pill.top = mix(swap_chip_from_.top, chip.bounds.top);
      pill.right = mix(swap_chip_from_.right, chip.bounds.right);
      pill.bottom = mix(swap_chip_from_.bottom, chip.bounds.bottom);
    }
    StrokeRounded(dc, pill, kRowSelectedBg,
                  palette.dark ? kActivePillBorder : kBorder,
                  Scaled(kCornerRadius));
  }
  for (const SpaceChip& chip : chips_) {
    const bool active = (chip.id == active_space_);
    if (!active && hover_kind_ == HoverKind::kChip &&
        hover_index_ == static_cast<int>(&chip - chips_.data())) {
      // The Mac's dim hover wash on an inactive pip (sidebarTabHovered).
      FillRounded(dc, chip.bounds, palette.hover, Scaled(kCornerRadius));
    }
    ::SetTextColor(dc, active ? kLabel : kLabelDim);
    // The glyph box is INSET from the pip: the colour pass repaints its
    // whole rect from a sampled corner pixel, and on a selected pip the
    // PIP's corner is ground tint — a full-rect clear would erase the pill.
    RECT glyph_rc = chip.bounds;
    ::InflateRect(&glyph_rc, -Scaled(4), -Scaled(4));
    // Colour emoji ink rides LOW and LARGE in its line box; one size down
    // and a 1px lift centre it against the MDL2 glyphs (the Mac applies its
    // own emojiVerticalOffset for the same reason).
    RECT emoji_rc = glyph_rc;
    ::OffsetRect(&emoji_rc, 0, -Scaled(1));
    if (!chip.icon.empty() &&
        PhiDrawCatalogArtwork(dc, glyph_rc, chip.icon, Scaled(13),
                              active ? kLabel : kLabelDim)) {
      // The Mac catalog's own artwork, tinted like the glyph would be.
    } else if (!chip.icon.empty() && !PhiIsPrivateGlyph(chip.icon) &&
        PhiDrawColorText(dc, emoji_rc, chip.icon, Scaled(12),
                         active ? kLabel : kLabelDim)) {
      // Emoji rendered in COLOUR via DirectWrite; GDI below is the fallback.
    } else if (!chip.icon.empty() &&
               (emoji_font_ || chip_icon_font_)) {
      // A private-use glyph is one of the Phi icon catalog's MDL2 stand-ins
      // (same 13pt as the emoji, so the two align); everything else emoji.
      HFONT icon_font =
          PhiIsPrivateGlyph(chip.icon) ? chip_icon_font_ : emoji_font_;
      HGDIOBJ previous_icon_font =
          icon_font ? ::SelectObject(dc, icon_font) : nullptr;
      ::DrawTextW(dc, chip.icon.c_str(), -1, &glyph_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (previous_icon_font) {
        ::SelectObject(dc, previous_icon_font);
      }
    } else {
      // No icon: the name's first character stands in (the pip has no room
      // for the name itself).
      const std::wstring initial =
          chip.label.empty() ? std::wstring(L"?") : chip.label.substr(0, 1);
      ::DrawTextW(dc, initial.c_str(), -1, const_cast<RECT*>(&chip.bounds),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }
  if (!incognito_) {
    // The Mac trailing slot: "+" creates; on overflow it becomes the "…"
    // switcher (moreButton) since not every pip is visible.
    DrawGlyph(dc, layout.add_space, chips_overflow_ ? L"…" : L"+",
              kLabelDim);
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
      ::DrawTextW(dc, L"Feedback", -1, &measure,
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
      ::DrawTextW(dc, L"Feedback", -1, &text_rc,
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
    ::DrawTextW(dc, L"Chat", -1, &chat_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (previous_small) {
      ::SelectObject(dc, previous_small);
    }
  }

  // Last, so it overlays the header the way the Mac's floating panel
  // overlays the window.
  DrawHoverCard(dc, layout, palette);

  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

// A hovered pip's info card (the Mac's SpaceHoverCard): the Space as a
// tinted capsule (icon + name), the bound profile, and the Ctrl+N switch
// shortcut as keycaps, floating just above the pip on a bordered card. The
// header above the strip is all shell-drawn — no child window clips it —
// which is why the card anchors UPWARD, like the Mac panel.
void PhiWindowController::DrawHoverCard(HDC dc,
                                        const Layout& layout,
                                        const PhiPalette& palette) {
  if (hover_card_space_.empty() || drag_active_) {
    return;
  }
  const SpaceChip* chip = nullptr;
  for (const SpaceChip& c : chips_) {
    if (c.id == hover_card_space_) {
      chip = &c;
      break;
    }
  }
  if (!chip) {
    return;  // The Space left the strip; the next hover change prunes state.
  }
  const COLORREF card_bg = palette.card;
  const COLORREF accent = card_accent_;
  const std::wstring name = chip->label.empty() ? L"Space" : chip->label;
  const std::wstring profile =
      card_profile_.empty() ? L"Default" : card_profile_;

  // Measure the two labels, clamped so the card always fits the sidebar.
  const auto measure = [&](const std::wstring& text, int max_w) {
    RECT rc = {0, 0, 0, 0};
    ::DrawTextW(dc, text.c_str(), -1, &rc,
                DT_CALCRECT | DT_SINGLELINE | DT_LEFT);
    return std::min(static_cast<int>(rc.right), max_w);
  };
  // SpaceHoverCard's metrics: capsule 8/3 padding around a 14pt icon and the
  // name; 8pt between sections; 18pt keycaps with 3pt gaps; 10/7 card
  // padding.
  const int pill_h = Scaled(22);
  const int dot_w = Scaled(6);
  const int gap = Scaled(8);
  std::vector<std::wstring> keycaps;
  if (card_shortcut_ > 0) {
    keycaps.push_back(L"Ctrl");
    keycaps.push_back(std::to_wstring(card_shortcut_));
  }
  int keys_w = 0;
  std::vector<int> key_widths;
  for (const std::wstring& token : keycaps) {
    const int w = std::max(Scaled(18), measure(token, Scaled(40)) + Scaled(8));
    key_widths.push_back(w);
    keys_w += (keys_w ? Scaled(3) : 0) + w;
  }

  // The card must stay within the band the shell owns: the Mac's panel
  // floats over the whole window, but pixels past the sidebar here belong
  // to the content CHILD, which clips them. Shrink the name first, then
  // the profile — never the keycaps.
  const int band_right = layout.horizontal_tabs
                             ? layout.client.right - Scaled(4)
                             : Scaled(sidebar_dip_) - Scaled(4);
  int name_w = measure(name, Scaled(110));
  int profile_w = measure(profile, Scaled(80));
  const int fixed_w = 2 * Scaled(8) + Scaled(14) + Scaled(5) +  // Pill chrome.
                      gap + dot_w + gap +
                      (keys_w ? gap + dot_w + gap + keys_w : 0);
  const int avail = band_right - Scaled(4) - 2 * Scaled(10);
  int overflow = fixed_w + name_w + profile_w - avail;
  if (overflow > 0 && name_w > Scaled(50)) {
    const int take = std::min(overflow, name_w - Scaled(50));
    name_w -= take;
    overflow -= take;
  }
  if (overflow > 0 && profile_w > Scaled(36)) {
    profile_w -= std::min(overflow, profile_w - Scaled(36));
  }
  const int pill_w = 2 * Scaled(8) + Scaled(14) + Scaled(5) + name_w;
  int content_w = pill_w + gap + dot_w + gap + profile_w;
  if (keys_w) {
    content_w += gap + dot_w + gap + keys_w;
  }
  const int card_w = content_w + 2 * Scaled(10);
  const int card_h = pill_h + 2 * Scaled(7);

  // Centered over the pip, clamped to the band the shell owns; above the
  // strip, pushed down only when the window is too short for it.
  const int mid_x = (chip->bounds.left + chip->bounds.right) / 2;
  int left = mid_x - card_w / 2;
  left = std::min(left, band_right - card_w);
  left = std::max(left, Scaled(4));
  int top = chip->bounds.top - Scaled(6) - card_h;
  top = std::max(top, Scaled(2));
  const RECT card = {left, top, left + card_w, top + card_h};
  StrokeRounded(dc, card, card_bg, palette.border, Scaled(kCornerRadius));

  const int mid_y = (card.top + card.bottom) / 2;
  int x = card.left + Scaled(10);

  // The Space capsule: icon + name on the accent's 15% wash.
  const RECT pill = {x, mid_y - pill_h / 2, x + pill_w, mid_y + pill_h / 2};
  FillRounded(dc, pill, BlendOver(accent, card_bg, 38), pill_h);
  RECT icon_rc = {pill.left + Scaled(8), mid_y - Scaled(7),
                  pill.left + Scaled(8) + Scaled(14), mid_y + Scaled(7)};
  if (!chip->icon.empty() &&
      PhiDrawCatalogArtwork(dc, icon_rc, chip->icon, Scaled(13), accent)) {
    // The Mac catalog's own artwork, tinted the capsule's accent.
  } else if (!chip->icon.empty() && !PhiIsPrivateGlyph(chip->icon) &&
      PhiDrawColorText(dc, icon_rc, chip->icon, Scaled(12), accent)) {
    // Colour emoji via DirectWrite; the font paths below are the fallback.
  } else if (!chip->icon.empty() && (emoji_font_ || chip_icon_font_)) {
    HFONT icon_font =
        PhiIsPrivateGlyph(chip->icon) ? chip_icon_font_ : emoji_font_;
    HGDIOBJ previous_icon_font =
        icon_font ? ::SelectObject(dc, icon_font) : nullptr;
    ::SetTextColor(dc, accent);
    ::DrawTextW(dc, chip->icon.c_str(), -1, &icon_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (previous_icon_font) {
      ::SelectObject(dc, previous_icon_font);
    }
  } else {
    ::SetTextColor(dc, accent);
    const std::wstring initial = name.substr(0, 1);
    ::DrawTextW(dc, initial.c_str(), -1, &icon_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  ::SetTextColor(dc, accent);
  RECT name_rc = {icon_rc.right + Scaled(5), pill.top,
                  pill.right - Scaled(8), pill.bottom};
  ::DrawTextW(dc, name.c_str(), -1, &name_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  x += pill_w + gap;

  // Middle-dot separators and the profile, dim like the Mac's secondary.
  const RECT dot1 = {x, card.top, x + dot_w, card.bottom};
  DrawGlyph(dc, dot1, L"·", palette.label_dim);
  x += dot_w + gap;
  ::SetTextColor(dc, palette.label_dim);
  RECT profile_rc = {x, card.top, x + profile_w, card.bottom};
  ::DrawTextW(dc, profile.c_str(), -1, &profile_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  x += profile_w;

  // The switch shortcut as keycaps, tinted like the address tooltip's.
  if (keys_w) {
    x += gap;
    const RECT dot2 = {x, card.top, x + dot_w, card.bottom};
    DrawGlyph(dc, dot2, L"·", palette.label_dim);
    x += dot_w + gap;
    for (size_t i = 0; i < keycaps.size(); ++i) {
      const RECT key = {x, mid_y - Scaled(9), x + key_widths[i],
                        mid_y + Scaled(9)};
      StrokeRounded(dc, key, BlendOver(accent, card_bg, 36),
                    BlendOver(accent, card_bg, 140), Scaled(4));
      DrawGlyph(dc, key, keycaps[i].c_str(), accent);
      x += key_widths[i] + Scaled(3);
    }
  }
}

// static
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
                           !layout.horizontal_tabs && drag_index_ >= 0 &&
                           drag_index_ < static_cast<int>(pinned_.size());
  std::vector<size_t> pin_order;
  pin_order.reserve(pinned_.size());
  for (size_t i = 0; i < pinned_.size(); ++i) {
    if (!pin_sliding || static_cast<int>(i) != drag_index_) {
      pin_order.push_back(i);
    }
  }
  if (pin_sliding) {
    pin_order.push_back(static_cast<size_t>(drag_index_));
  }
  for (const size_t pin_order_index : pin_order) {
    const ShortcutRow& pin = pinned_[pin_order_index];
    const bool pin_dragged =
        pin_sliding && static_cast<int>(pin_order_index) == drag_index_;
    // The dragged cell floats at the cursor's grip; siblings glide by
    // their slide offsets (both axes — a grid gap wraps rows).
    RECT cell = pin.bounds;
    if (pin_dragged) {
      ::OffsetRect(&cell, drag_point_.x - drag_grab_dx_ - pin.bounds.left,
                   drag_point_.y - drag_grab_dy_ - pin.bounds.top);
    } else if (drag_active_ && !layout.horizontal_tabs) {
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
      HPEN ring = ::CreatePen(PS_SOLID, Scaled(2), palette.accent);
      HGDIOBJ old_pen = ::SelectObject(dc, ring);
      HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
      RECT rr = cell;
      ::InflateRect(&rr, -Scaled(1), -Scaled(1));
      const int d = 2 * Scaled(kCornerRadius);
      ::RoundRect(dc, rr.left, rr.top, rr.right, rr.bottom, d, d);
      ::SelectObject(dc, old_brush);
      ::SelectObject(dc, old_pen);
      ::DeleteObject(ring);
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
      ::DrawTextW(dc, initial.c_str(), -1, &cell,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }

  // Bookmarks: rows shaped like tab rows (BookmarkCellView) -- primary-label
  // titles, folders marked with the disclosure glyph.
  const bool bookmark_sliding =
      drag_active_ && drag_kind_ == DragKind::kBookmark &&
      !layout.horizontal_tabs && drag_index_ >= 0 &&
      drag_index_ < static_cast<int>(bookmarks_.size());
  std::vector<size_t> bookmark_order;
  bookmark_order.reserve(bookmarks_.size());
  for (size_t i = 0; i < bookmarks_.size(); ++i) {
    if (!bookmark_sliding || static_cast<int>(i) != drag_index_) {
      bookmark_order.push_back(i);
    }
  }
  if (bookmark_sliding) {
    bookmark_order.push_back(static_cast<size_t>(drag_index_));
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
        static_cast<int>(bookmark_order_index) == drag_index_;
    RECT pill = bookmark.bounds;
    if (bookmark_dragged) {
      ::OffsetRect(&pill, 0,
                   drag_point_.y - drag_grab_dy_ - bookmark.bounds.top);
    } else if (drag_active_ && !layout.horizontal_tabs) {
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
    } else if (drag_active_ && drag_kind_ == DragKind::kBookmark &&
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
    RECT icon = {pill.left + Scaled(6) +
                     (Scaled(kFaviconSlot) - Scaled(kFaviconSize)) / 2,
                 mid - Scaled(kFaviconSize) / 2, 0, 0};
    icon.right = icon.left + Scaled(kFaviconSize);
    icon.bottom = icon.top + Scaled(kFaviconSize);
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
    std::wstring label =
        bookmark.folder
            ? (std::wstring(bookmark.expanded ? L"\u25BE  " : L"\u25B8  ") +
               bookmark.title)
            : bookmark.title;
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
    ::DrawTextW(dc, label.c_str(), -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  // "New Tab": the row heading the tab list -- a 16pt "+" 6pt in, an 8pt
  // gap, the title in textTertiary (NewTabButtonCellView). Hover raises the
  // Mac cell's radius-8 wash on the 2pt-inset pill.
  if (layout.new_tab_row.right > layout.new_tab_row.left) {
    const RECT& row = layout.new_tab_row;
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
    ::DrawTextW(dc, L"New Tab", -1, &text,
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
  for (const GroupHeader& gh : group_headers_) {
    if (gh.container.right > gh.container.left) {
      FillRounded(dc, gh.container,
                  BlendOver(gh.color, kTintTop, kGroupTintAlpha),
                  Scaled(kCornerRadius));
    }
    if (gh.bounds.right <= gh.bounds.left) {
      continue;  // Header scrolled out of the viewport.
    }
    if (hover_kind_ == HoverKind::kGroupHeader &&
        hover_index_ == static_cast<int>(&gh - group_headers_.data())) {
      FillRounded(dc, gh.bounds, BlendOver(gh.color, kTintTop, 71),
                  Scaled(kCornerRadius));
    }
    const int mid = (gh.bounds.top + gh.bounds.bottom) / 2;
    RECT chevron = {gh.bounds.left + Scaled(8), mid - Scaled(8),
                    gh.bounds.left + Scaled(8) + Scaled(16), mid + Scaled(8)};
    HGDIOBJ previous_glyph_font =
        caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
    DrawGlyph(dc, chevron, gh.collapsed ? L"\uE76C" : L"\uE70D", kLabelDim);
    if (previous_glyph_font) {
      ::SelectObject(dc, previous_glyph_font);
    }
    // The dot: colored centre in a white ring (TabGroupHeaderColorIndicator).
    const int cx = chevron.right + Scaled(4) + Scaled(6);
    HGDIOBJ old_pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    HBRUSH ring = ::CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
    HGDIOBJ old_brush = ::SelectObject(dc, ring);
    ::Ellipse(dc, cx - Scaled(6), mid - Scaled(6), cx + Scaled(6),
              mid + Scaled(6));
    HBRUSH centre = ::CreateSolidBrush(gh.color);
    ::SelectObject(dc, centre);
    ::Ellipse(dc, cx - Scaled(5), mid - Scaled(5), cx + Scaled(5),
              mid + Scaled(5));
    ::SelectObject(dc, old_brush);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(ring);
    ::DeleteObject(centre);
    ::SetTextColor(dc, kLabel);
    RECT title_rc = {cx + Scaled(6) + Scaled(6), gh.bounds.top,
                     gh.bounds.right - Scaled(6), gh.bounds.bottom};
    ::DrawTextW(dc, gh.title.c_str(), -1, &title_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  // Tab rows: a 32pt pill inside the 36pt row (2pt vertical padding), radius
  // 8. The title is textPrimary whether active or not (SideTabView); only the
  // pill marks the active tab. The close box rides the ACTIVE row -- the Mac
  // shows it on hover, which GDI has no notion of yet.
  //
  // During a drag-reorder (Chrome's vertical-tab model) the DRAGGED row
  // floats with the cursor and paints LAST (on top); its siblings paint at
  // their slide offsets, gliding around the insertion gap.
  const bool sliding = drag_active_ && drag_kind_ == DragKind::kTab &&
                       !layout.horizontal_tabs && drag_index_ >= 0 &&
                       drag_index_ < static_cast<int>(tabs_.size());
  std::vector<size_t> draw_order;
  draw_order.reserve(tabs_.size());
  for (size_t i = 0; i < tabs_.size(); ++i) {
    if (!sliding || static_cast<int>(i) != drag_index_) {
      draw_order.push_back(i);
    }
  }
  if (sliding) {
    draw_order.push_back(static_cast<size_t>(drag_index_));
  }
  bool rows_clip_active = clip_rows;
  for (const size_t order_index : draw_order) {
    const TabRow& row = tabs_[order_index];
    if (row.bounds.right <= row.bounds.left) {
      continue;  // Scrolled out of the viewport.
    }
    const bool is_dragged =
        sliding && static_cast<int>(order_index) == drag_index_;
    if (is_dragged && rows_clip_active) {
      // The floating row escapes the list viewport — it may hover the
      // pinned grid or bookmark band above it.
      ::RestoreDC(dc, rows_clip_save);
      rows_clip_active = false;
    }
    int dy = 0;
    if (is_dragged) {
      // The floating row keeps the cursor's grip — clamped to the list
      // only while the list is the target; over the conversion bands it
      // follows the cursor to them.
      const int height = row.bounds.bottom - row.bounds.top;
      int top = drag_point_.y - drag_grab_dy_;
      if (drag_band_ == DropBand::kTabs) {
        top = std::max(top, static_cast<int>(layout.tabs_top));
        top = std::min(top, layout.tabs_limit - height);
      }
      dy = top - row.bounds.top;
    } else if (sliding) {
      dy = static_cast<int>(DragSlideOffset(row.id));
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
    COLORREF ground = kTintTop;
    if (!row.group_id.empty()) {
      for (const GroupHeader& gh : group_headers_) {
        if (gh.id == row.group_id) {
          ground = BlendOver(gh.color, kTintTop, kGroupTintAlpha);
          break;
        }
      }
    }
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
      // A DORMANT tab (restored but never loaded, or discarded) reads
      // dimmer than a live one — the Mac's isUnloaded treatment. The active
      // row is never dormant, so this never fights the selected pill.
      ::SetTextColor(dc, r.unloaded && !r.active ? kLabelDim : kLabel);
      RECT title_rc = {slot.right + Scaled(8), pill.top, right - Scaled(4),
                       pill.bottom};
      ::DrawTextW(dc, (pane_b ? r.b_title : r.title).c_str(), -1, &title_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    };
    // The close box shows on HOVER only (SideTabView) \u2014 the active row
    // included; the title reclaims its slot the rest of the time.
    const bool show_close = row_hovered && !is_dragged;
    if (row.b_index >= 0) {
      const int div_x = (pill.left + pill.right) / 2;
      draw_pane(pill.left, div_x, row, /*pane_b=*/false);
      draw_pane(div_x, show_close ? close_box.left : pill.right, row,
                /*pane_b=*/true);
      RECT divider = {div_x, pill.top + Scaled(6), div_x + 1,
                      pill.bottom - Scaled(6)};
      HBRUSH sep = ::CreateSolidBrush(palette.separator);
      ::FillRect(dc, &divider, sep);
      ::DeleteObject(sep);
    } else {
      draw_pane(pill.left, show_close ? close_box.left : pill.right, row,
                /*pane_b=*/false);
    }
    if (show_close) {
      DrawGlyph(dc, close_box, L"\u00D7", kLabelDim);
    }
    // The agent operating mask: a badge on rows an agent is holding. Accent
    // while the agent drives; dimmed once the user has taken the tab back
    // (the row keeps the badge so "Hand Back" stays discoverable).
    if (host_ && row.id != 0 && host_->IsTabAgentDriven(row.id)) {
      const bool reclaimed = host_->IsTabReclaimed(row.id);
      const int dot = Scaled(8);
      const int cx = (show_close ? close_box.left : pill.right) - Scaled(10);
      const int cy = (pill.top + pill.bottom) / 2;
      const COLORREF ink =
          reclaimed ? palette.label_tertiary : palette.accent;
      HBRUSH fill = ::CreateSolidBrush(ink);
      HPEN pen = ::CreatePen(PS_SOLID, 1, ink);
      HGDIOBJ prev_brush = ::SelectObject(dc, fill);
      HGDIOBJ prev_pen = ::SelectObject(dc, pen);
      ::Ellipse(dc, cx - dot / 2, cy - dot / 2, cx + dot / 2, cy + dot / 2);
      ::SelectObject(dc, prev_pen);
      ::SelectObject(dc, prev_brush);
      ::DeleteObject(pen);
      ::DeleteObject(fill);
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
  auto* self = reinterpret_cast<PhiWindowController*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
      if (!in_chrome && self && !PhiGroupOverview::IsOpen() &&
          self->browser_hwnd_ && ::IsWindow(self->browser_hwnd_)) {
        ::SetFocus(self->browser_hwnd_);
        if (self->framework_) {
          self->framework_->FocusBrowserWindow(self->window_id_);
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
        ::SetFocus(self->browser_hwnd_);
        if (self->framework_) {
          self->framework_->FocusBrowserWindow(self->window_id_);
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
        const COLORREF bg =
            self->layout_mode_ == PhiLayoutMode::kBalanced
                ? (self->url_field_hover_ ? palette.hover
                                          : palette.window_bg)
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
      if (self) {
        self->SetCaptionHot(0);
        self->SetMenuHot(-1);
        self->SetMenuRevealed(false);
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
      if (self) {
        self->SetCaptionHot(0);
        self->SetMenuHot(-1);
        self->SetMenuRevealed(false);
        self->ClearHover();
      }
      break;
    case kMsgPhiSyncBookmarkPage:
      if (self) {
        self->SyncBookmarkManager();
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
        self->OnUrlFieldChanged();  // The user typed: autocomplete runs.
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
        self->CancelDrag();
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
        for (HWND cap : self->corner_caps_) {
          if (cap) {
            ::InvalidateRect(cap, nullptr, TRUE);
          }
        }
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
        // The model rebuilds on notifications and intents (rows_dirty_), not
        // per paint — a paint with a clean model just recomposes.
        if (self->rows_dirty_) {
          self->RebuildTabs();
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
