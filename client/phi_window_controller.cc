// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_window_controller.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "base/base64.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_menu_bar.h"
#include "phibrowser-win/client/phi_prompt.h"

namespace phi_client {

namespace {

constexpr wchar_t kShellClassName[] = L"PhiClientShell";

// --- Metrics -----------------------------------------------------------------
// Design units (points at 100%), taken from the Mac client so the two look the
// same: WebContentConstant, SidebarHeaderView, SpacesStripView,
// PinnedTabLayout, TabStripMetrics and SidebarBottomBar.
constexpr int kSidebarWidth = 240;
// The caption band: the OS title bar is removed (WM_NCCALCSIZE) and this row
// holds the MENU TITLES and the minimize / maximize / close buttons together
// — the menu bar lives on the caption row, not under it.
constexpr int kTitleBarHeight = 32;
constexpr int kCaptionButtonWidth = 46;  // The Win11 caption button metric.
constexpr int kMenuTitlePad = 10;        // Each title's horizontal padding.
// WebContentConstant.edgesSpacing — THE horizontal gutter. There is no other
// sidebar padding on Mac; every row applies 8 on each side itself.
constexpr int kEdgeInset = 8;
constexpr int kNavRowTop = 8;   // defaultSidebarButtonTopOffset (performance)
constexpr int kNavRowTopNav = 15;  // legacySidebarButtonTopOffset, 15.5 rounded
constexpr int kNavButton = 24;     // Every header button is 24x24.
constexpr int kNavGroupSpacing = 1;   // back/forward/reload stack spacing.
constexpr int kNavRowRightInset = 9;  // stackView right inset (balanced).
constexpr int kSpacesStripTop = 40;      // navRow.bottom + 8 (performance)
constexpr int kSpacesStripTopNav = 47;   // 15.5 + 24 + 8 (balanced)
constexpr int kSpacesStripHeight = 32;   // SpacesStripView.sidebarHeight
constexpr int kSpacesStripPad = 10;      // horizontalPadding inside the strip.
constexpr int kSpaceChip = 24;           // stripItemWidth/Height: a 24pt pip.
constexpr int kSpaceChipGap = 4;         // stripSpacing.
constexpr int kAddressBarTop = 84;    // spacesStrip.bottom + 12
constexpr int kAddressBarHeight = 32;
constexpr int kHeaderHeight = 120;    // 80 base + 32 strip + 8 gap
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
constexpr int kContentTopGap = 8;
constexpr int kContentTopGapTopNav = 12;
constexpr int kFaviconSlot = 16;    // The 16pt slot in the row…
constexpr int kFaviconSize = 14;    // …holding a 14pt icon (radius 3).
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
constexpr int kTopBarLeading = 16;     // header leading padding.
constexpr int kTopBarSpacing = 8;      // header button spacing.
constexpr int kTopBarField = 26;       // the top address capsule's height.
constexpr int kHTabStripHeight = 36;   // TabStripMetrics.Strip.height
constexpr int kHTabStripTop = 6;       // Strip top inset (8 − 2).
constexpr int kHTabHeight = 32;        // Strip.tabHeight
constexpr int kHTabWidth = 180;        // Tab.idealWidth
constexpr int kHTabGap = 2;            // Tab.spacing
constexpr int kCloseBoxSize = 24;      // UnifiedTabCloseButton frame.
// The bottom bar's two capsules (Feedback bordered, Chat on the accent).
constexpr int kFeedbackWidth = 90;
constexpr int kChatWidth = 54;
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
  return base ? ::CallWindowProcW(base, hwnd, msg, wparam, lparam)
              : ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

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

ATOM EnsureShellClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
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
  const int w = out.client.right;
  const int h = out.client.bottom;
  const int sidebar = Scaled(kSidebarWidth);
  const int inset = Scaled(kEdgeInset);
  const int btn = Scaled(kNavButton);
  const int m = Scaled(kContentMargin);

  // The caption band tops every mode: menu titles leading, minimize /
  // maximize / close trailing. The OS caption is gone (WM_NCCALCSIZE), so
  // this row is both the title bar and the menu bar.
  const int tb = Scaled(kTitleBarHeight);
  out.title_bar = {0, 0, w, tb};
  const int cap = Scaled(kCaptionButtonWidth);
  out.caption_close = {w - cap, 0, w, tb};
  out.caption_max = {w - 2 * cap, 0, w - cap, tb};
  out.caption_min = {w - 3 * cap, 0, w - 2 * cap, tb};

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
  auto place_top_bar = [&](int left, int right, int top) {
    out.top_bar = {left, top, right, top + Scaled(kTopBarHeight)};
    const int mid = top + Scaled(kTopBarHeight) / 2;
    int x = left + Scaled(kTopBarLeading);
    out.back = {x, mid - btn / 2, x + btn, mid + btn / 2};
    x += btn + Scaled(kTopBarSpacing);
    out.forward = {x, mid - btn / 2, x + btn, mid + btn / 2};
    x += btn + Scaled(kTopBarSpacing);
    out.reload = {x, mid - btn / 2, x + btn, mid + btn / 2};
    return mid;
  };

  if (layout_mode_ == PhiLayoutMode::kComfortable) {
    // The traditional stack: horizontal tab strip on the tint, a 12pt breath,
    // then the card, whose first band is the header row. No sidebar.
    const int strip_top = tb + Scaled(kHTabStripTop);
    const int strip_h = Scaled(kHTabStripHeight);
    const int card_top = strip_top + strip_h + Scaled(kContentTopGapTopNav);
    const int mid = place_top_bar(m, w - m, card_top);
    out.content = {m, card_top + Scaled(kTopBarHeight), w - m, h - m};

    // The strip: the active Space's compact pip leads (the Mac clears the
    // traffic lights with 80pt; Windows has none, so the edge inset does),
    // then the tabs; the search-tabs magnifier rides at the far right.
    const int chip_top = strip_top + (strip_h - Scaled(4) - Scaled(kSpaceChip)) / 2;
    out.chips_left = inset;
    out.chips_top = chip_top;
    out.chips_limit = inset + Scaled(kSpaceChip);  // One compact pip.
    out.search_tabs = {w - inset - btn, chip_top, w - inset, chip_top + btn};
    out.tabs_left = out.chips_limit + Scaled(6);
    out.tabs_top = strip_top;
    out.tabs_limit = out.search_tabs.left - Scaled(kTopBarSpacing);
    out.horizontal_tabs = true;

    // Header trailing: downloads, then the Feedback capsule (chat is hidden
    // in comfortable, as on Mac).
    int r = w - m - Scaled(kTopBarLeading);
    out.downloads = {r - btn, mid - btn / 2, r, mid + btn / 2};
    r = out.downloads.left - Scaled(kTopBarSpacing);
    out.feedback = {r - Scaled(kFeedbackWidth), mid - Scaled(kPillHeight) / 2,
                    r, mid + Scaled(kPillHeight) / 2};
    const int field_h = Scaled(kTopBarField);
    out.url_field = {out.reload.right + Scaled(6), mid - field_h / 2,
                     out.feedback.left - Scaled(kTopBarSpacing),
                     mid + field_h / 2};
    return out;
  }

  // Vertical layouts: the Mac sidebar stack, top to bottom.
  out.sidebar = {0, 0, sidebar, h};

  if (layout_mode_ == PhiLayoutMode::kBalanced) {
    // Balanced header: the toggle moves to the RIGHT (inset 9), the
    // search-tabs button beside it (gap 2) — the only mode that shows it.
    // Navigation and the address bar live in the top bar.
    const int top = tb + Scaled(kNavRowTopNav);
    int r = sidebar - Scaled(kNavRowRightInset);
    out.sidebar_toggle = {r - btn, top, r, top + btn};
    out.search_tabs = {out.sidebar_toggle.left - Scaled(2) - btn, top,
                       out.sidebar_toggle.left - Scaled(2), top + btn};
    place_spaces(0, sidebar, tb + Scaled(kSpacesStripTopNav));

    const int gap = tb + Scaled(kContentTopGapTopNav);
    const int mid = place_top_bar(sidebar, w - m, gap);
    out.content = {sidebar, gap + Scaled(kTopBarHeight), w - m, h - m};
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
    // Performance header: the toggle leads; back / forward / reload trail
    // (spacing 1, right inset 9); the address bar sits under the strip.
    const int top = tb + Scaled(kNavRowTop);
    out.sidebar_toggle = {inset, top, inset + btn, top + btn};
    int r = sidebar - Scaled(kNavRowRightInset);
    out.reload = {r - btn, top, r, top + btn};
    r -= btn + Scaled(kNavGroupSpacing);
    out.forward = {r - btn, top, r, top + btn};
    r -= btn + Scaled(kNavGroupSpacing);
    out.back = {r - btn, top, r, top + btn};
    place_spaces(0, sidebar, tb + Scaled(kSpacesStripTop));
    out.url_field = {inset, tb + Scaled(kAddressBarTop), sidebar - inset,
                     tb + Scaled(kAddressBarTop + kAddressBarHeight)};
    out.content = {sidebar, tb + Scaled(kContentTopGap), w - m, h - m};
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
  int br = sidebar - inset;
  const int pill_top = bar_top + (bar_h - Scaled(kPillHeight)) / 2;
  out.chat = {br - Scaled(kChatWidth), pill_top, br,
              pill_top + Scaled(kPillHeight)};
  br = out.chat.left - Scaled(4);
  out.feedback = {br - Scaled(kFeedbackWidth), pill_top, br,
                  pill_top + Scaled(kPillHeight)};

  out.tabs_left = inset;
  out.tabs_limit = out.bottom_bar.top - Scaled(kFavoritesSpacer);
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
  return Scaled(kTitleBarHeight);
}

int PhiWindowController::sidebar_width() const {
  return Scaled(kSidebarWidth);
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
                                        PhiSlotHost* host)
    : window_id_(window_id),
      browser_hwnd_(browser_hwnd),
      framework_(framework),
      host_(host) {
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
  int w = Scaled(1200 + kSidebarWidth), h = Scaled(800);
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
    x = wr.left - Scaled(kSidebarWidth) - border_x;
    y = wr.top - Scaled(kTitleBarHeight) - Scaled(kContentTopGap);
    w = (wr.right + Scaled(kContentMargin) + border_x) - x;
    h = (wr.bottom + Scaled(kContentMargin) + border_y) - y;
  }

  shell_hwnd_ = ::CreateWindowExW(
      0, kShellClassName, L"Phi", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w,
      h, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
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
  if (framework_) {
    framework_->SetWindowSpace(window_id_, "1");
  }
  LayoutContents();
  ::ShowWindow(shell_hwnd_, SW_SHOW);
  LOG(INFO) << "[PhiClient] shell window " << window_id_
            << " shell_hwnd=" << shell_hwnd_ << " browser_hwnd=" << browser_hwnd_;
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
  // at the trailing end.
  const int width = box.right - box.left - Scaled(12) - Scaled(8);
  ::MoveWindow(url_field_, box.left + Scaled(12),
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
  ::SetWindowTextW(url_field_, nav_url_.c_str());
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
  framework_->Navigate(active_browser_ ? active_browser_ : window_id_,
                       base::WideToUTF8(typed).c_str());
  FocusActiveBrowser();  // Keys belong to the page again once we've navigated.
  RefreshTabs();
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
          self->CommitUrlField();
        }
        return 0;
      }
      if (wparam == VK_ESCAPE) {
        if (self) {
          self->SyncUrlField(/*force=*/true);
          self->FocusActiveBrowser();
        }
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
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  const PhiPalette& palette =
      host_ ? host_->PaletteForSlot(window_id_) : kFallback;
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

void PhiWindowController::LayoutContents() {
  if (!shell_hwnd_) {
    return;
  }
  const Layout layout = ComputeLayout();
  PositionUrlField(layout);
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    const int w = layout.content.right - layout.content.left;
    const int h = layout.content.bottom - layout.content.top;
    ::SetWindowPos(browser_hwnd_, nullptr, layout.content.left,
                   layout.content.top, w > 0 ? w : 0, h > 0 ? h : 0,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }

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
    const bool shown = i >= 2 || top_caps;
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

void PhiWindowController::RefreshTabs() {
  RebuildTabs();
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    ::UpdateWindow(shell_hwnd_);  // Paint now, not at the next idle.
  }
}

void PhiWindowController::ScheduleRefresh() {
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
  browser_hwnd_ = browser_hwnd;
  AdoptBrowserAsChild();
  if (space_browsers_.find(space_id) == space_browsers_.end()) {
    space_order_.push_back(space_id);
  }
  space_browsers_[space_id] = browser_window_id;
  browser_hwnds_[browser_window_id] = browser_hwnd;
  active_space_ = space_id;
  active_browser_ = browser_window_id;
  if (framework_) {
    framework_->SetWindowSpace(browser_window_id, space_id.c_str());
  }
  LayoutContents();
  // Same order as ShowSpace: the outgoing browser goes away only after the
  // incoming one is shown in place, so the content rect is never uncovered.
  if (previous && previous != browser_hwnd && ::IsWindow(previous)) {
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
  browser_hwnd_ = hwnd_it->second;
  active_space_ = space_id;
  active_browser_ = it->second;
  LayoutContents();  // Shows + positions the incoming child...
  // ...and only then drop the outgoing one. Hiding first uncovers the shell
  // background across the whole content rect for a frame — the window-wide
  // blink on every Space switch.
  if (previous) {
    ::ShowWindow(previous, SW_HIDE);
  }
  FocusActiveBrowser();
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
  can_go_back_ = st->FindBool("canGoBack").value_or(false);
  can_go_forward_ = st->FindBool("canGoForward").value_or(false);
  nav_loading_ = st->FindBool("loading").value_or(false);
  SyncUrlField(/*force=*/false);
}

void PhiWindowController::RebuildChips() {
  chips_.clear();
  const Layout layout = ComputeLayout();
  add_space_button_ = layout.add_space;
  if (!host_) {
    return;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(host_->AllSpacesJson(), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return;
  }
  int x = layout.chips_left;
  for (const base::Value& v : parsed->GetList()) {
    if (!v.is_dict()) {
      continue;
    }
    const base::DictValue& d = v.GetDict();
    const std::string* id = d.FindString("id");
    if (!id) {
      continue;
    }
    // Comfortable shows a single compact pip — the ACTIVE Space; the rest
    // live in the popup it opens.
    if (layout.horizontal_tabs && *id != active_space_) {
      continue;
    }
    if (x + Scaled(kSpaceChip) > layout.chips_limit) {
      break;  // Row is full; the Mac scrolls here — future work.
    }
    SpaceChip chip;
    chip.id = *id;
    if (const std::string* name = d.FindString("name")) {
      chip.label = base::UTF8ToWide(*name);
    }
    if (chip.label.empty()) {
      chip.label = L"Space " + base::UTF8ToWide(*id);
    }
    if (const std::string* icon_text = d.FindString("iconText")) {
      chip.icon = base::UTF8ToWide(*icon_text);
    }
    chip.open = HasSpace(*id);
    chip.bounds = {x, layout.chips_top, x + Scaled(kSpaceChip),
                   layout.chips_top + Scaled(kSpaceChip)};
    chips_.push_back(std::move(chip));
    x += Scaled(kSpaceChip + kSpaceChipGap);
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

void PhiWindowController::RebuildShortcuts() {
  pinned_.clear();
  bookmarks_.clear();
  if (!host_) {
    return;
  }
  auto parse = [&](const std::string& json, std::vector<ShortcutRow>* out) {
    std::optional<base::Value> parsed =
        base::JSONReader::Read(json, base::JSON_PARSE_RFC);
    if (!parsed || !parsed->is_list()) {
      return;
    }
    for (const base::Value& value : parsed->GetList()) {
      if (!value.is_dict()) {
        continue;
      }
      const base::DictValue& d = value.GetDict();
      ShortcutRow row;
      if (const std::string* guid = d.FindString("guid")) {
        row.guid = *guid;
      }
      if (const std::string* url = d.FindString("url")) {
        row.url = *url;
      }
      const std::string* title = d.FindString("title");
      row.title = base::UTF8ToWide(title && !title->empty() ? *title : row.url);
      row.folder = d.FindBool("folder").value_or(false);
      if (const std::string* favicon = d.FindString("favicon")) {
        std::string decoded;
        if (!favicon->empty() && base::Base64Decode(*favicon, &decoded)) {
          row.favicon.assign(decoded.begin(), decoded.end());
          row.favicon_width = d.FindInt("faviconWidth").value_or(0);
          row.favicon_height = d.FindInt("faviconHeight").value_or(0);
        }
      }
      out->push_back(std::move(row));
    }
  };
  parse(host_->PinnedJson(window_id_), &pinned_);
  // The sidebar shows the root level; folders open from the Bookmarks menu.
  parse(host_->BookmarksJson(window_id_, std::string()), &bookmarks_);
  bookmark_row_count_ = static_cast<int>(bookmarks_.size());

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
      pin.bounds = {x, mid_y - cell / 2, x + cell, mid_y + cell / 2};
      x += cell + Scaled(kStripPinGap);
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
  const int decision = kSidebarWidth - 8;
  const int column_limit = decision < 236 ? 3
                           : decision < 294 ? 4
                           : decision < 352 ? 5
                                            : 6;
  const int avail = kSidebarWidth - 2 * kEdgeInset;
  int columns =
      std::max(1, std::min(column_limit, static_cast<int>(pinned_.size())));
  while (columns > 1 &&
         (avail - (columns - 1) * kPinnedGap) / columns < kPinnedMinWidth) {
    --columns;
  }
  pinned_row_count_ =
      (static_cast<int>(pinned_.size()) + columns - 1) / columns;
  // Derived only now: the two bands' sizes feed back into where everything
  // below them sits.
  const Layout layout = ComputeLayout();
  const int cell_h = Scaled(kPinnedCellHeight);
  const int gap = Scaled(kPinnedGap);
  // Both cell edges come from dividing the row span per column, not from one
  // truncated cell width: the rounding remainder spreads across the row and
  // the last edge lands exactly on the inset, like the Mac's float division.
  const int row_span =
      Scaled(kSidebarWidth) - 2 * Scaled(kEdgeInset) - (columns - 1) * gap;
  for (size_t i = 0; i < pinned_.size(); ++i) {
    const int col = static_cast<int>(i) % columns;
    const int row_index = static_cast<int>(i) / columns;
    const int band = layout.favorites.left + Scaled(kEdgeInset) + col * gap;
    const int y = layout.favorites.top + gap + row_index * (cell_h + gap);
    pinned_[i].bounds = {band + col * row_span / columns, y,
                         band + (col + 1) * row_span / columns, y + cell_h};
  }

  // Bookmarks are ordinary rows above the open tabs (36pt + the 2pt gap).
  int y = layout.favorites.bottom +
          (pinned_row_count_ > 0 ? Scaled(kFavoritesSpacer) : 0);
  for (ShortcutRow& row : bookmarks_) {
    row.bounds = {layout.tabs_left, y,
                  Scaled(kSidebarWidth - kEdgeInset),
                  y + Scaled(kTabRowHeight)};
    y += Scaled(kTabRowHeight + kTabRowGap);
  }
}

void PhiWindowController::RebuildTabs() {
  RebuildNavState();
  RebuildChips();
  RebuildShortcuts();
  tabs_.clear();
  group_headers_.clear();
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
      const std::string* title = d.FindString("title");
      const std::string* url = d.FindString("url");
      std::string label = title && !title->empty()
                              ? *title
                              : (url ? *url : std::string("(untitled)"));
      row.title = base::UTF8ToWide(label);
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

  // Stored-row bindings, the Mac's openOrFocusPinnedTab / bookmarkGuidBacking
  // model: a bound tab lives in its pin cell or sidebar bookmark row, not in
  // the tab list. Prune entries whose row was removed or whose tab has
  // closed, then flag the rows for the paint pass. Bookmark bindings act
  // only in the vertical layouts — the Mac hosts them in a bookmark bar the
  // horizontal port doesn't have yet, so there the tab shows as a normal
  // chip and stays reachable.
  auto& bindings = pin_bindings_[browser];
  const auto is_pin = [this](const std::string& guid) {
    return std::any_of(pinned_.begin(), pinned_.end(),
                       [&](const ShortcutRow& p) { return p.guid == guid; });
  };
  const auto is_bookmark = [this](const std::string& guid) {
    return std::any_of(bookmarks_.begin(), bookmarks_.end(),
                       [&](const ShortcutRow& b) {
                         return !b.folder && b.guid == guid;
                       });
  };
  for (auto it = bindings.begin(); it != bindings.end();) {
    const std::string& row_guid = it->first;
    const int tab_id = it->second;
    const bool tab_alive =
        std::any_of(rows.begin(), rows.end(),
                    [&](const TabRow& r) { return r.id == tab_id; });
    if ((is_pin(row_guid) || is_bookmark(row_guid)) && tab_alive) {
      ++it;
    } else {
      it = bindings.erase(it);
    }
  }
  const auto flag_bound = [&](ShortcutRow& stored) {
    auto bound = bindings.find(stored.guid);
    if (bound == bindings.end()) {
      return;
    }
    stored.open = true;
    for (const TabRow& r : rows) {
      if (r.id == bound->second) {
        stored.active = r.active;
        break;
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
    a.b_active = b.active;
    a.b_title = std::move(b.title);
    a.b_favicon = std::move(b.favicon);
    a.b_favicon_width = b.favicon_width;
    a.b_favicon_height = b.favicon_height;
    a.active = a.active || b.active;
    rows.erase(rows.begin() + i + 1);
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
  std::string current_group;
  bool current_collapsed = false;
  for (TabRow& row : rows) {
    bool hidden_by_binding = false;
    for (const auto& binding : bindings) {
      if (binding.second != row.id) {
        continue;
      }
      // After the prune a binding's guid is a pin or a bookmark; bookmark
      // bindings release their tab to the strip in the horizontal layout.
      hidden_by_binding =
          is_pin(binding.first) || !layout.horizontal_tabs;
      break;
    }
    if (hidden_by_binding) {
      continue;
    }
    if (!layout.horizontal_tabs && row.group_id != current_group) {
      current_group = row.group_id;
      current_collapsed = false;
      if (!current_group.empty()) {
        auto info = group_info.find(current_group);
        if (info != group_info.end()) {
          if (y + Scaled(kGroupHeaderHeight) > layout.tabs_limit) {
            break;
          }
          GroupHeader header = info->second;
          header.bounds = {layout.tabs_left, y,
                           Scaled(kSidebarWidth - kEdgeInset),
                           y + Scaled(kGroupHeaderHeight)};
          header.container = header.bounds;
          y += Scaled(kGroupHeaderHeight + kTabRowGap);
          current_collapsed = header.collapsed;
          group_headers_.push_back(std::move(header));
        }
      }
    }
    if (!layout.horizontal_tabs && current_collapsed) {
      continue;  // Hidden under a collapsed header.
    }
    const int close_box = Scaled(kCloseBoxSize);
    if (layout.horizontal_tabs) {
      if (x + Scaled(kHTabWidth) > layout.tabs_limit) {
        break;  // Strip is full; overflow handling is future work.
      }
      // TabStripMetrics: 32pt tabs at the strip's top, 4pt below.
      row.bounds = {x, y, x + Scaled(kHTabWidth), y + Scaled(kHTabHeight)};
      x += Scaled(kHTabWidth + kHTabGap);
    } else {
      if (y + Scaled(kTabRowHeight) > layout.tabs_limit) {
        break;
      }
      row.bounds = {layout.tabs_left, y,
                    Scaled(kSidebarWidth - kEdgeInset),
                    y + Scaled(kTabRowHeight)};
      y += Scaled(kTabRowHeight + kTabRowGap);
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
        !group_headers_.empty() && group_headers_.back().id == row.group_id) {
      group_headers_.back().container.bottom = row.bounds.bottom;
    }
    tabs_.push_back(std::move(row));
  }
}

void PhiWindowController::OpenOrFocusPinned(const std::string& guid,
                                            const std::string& url) {
  if (!framework_ || guid.empty()) {
    return;
  }
  const int64_t browser = active_browser_ ? active_browser_ : window_id_;
  auto& bindings = pin_bindings_[browser];
  auto bound = bindings.find(guid);
  if (bound != bindings.end()) {
    const int index = IndexOfTabId(browser, bound->second);
    if (index >= 0) {
      framework_->ActivateTab(browser, index);
      RefreshTabs();
      return;
    }
    bindings.erase(bound);
  }
  if (url.empty()) {
    return;
  }
  const int index = framework_->NewTab(browser, url.c_str());
  if (index >= 0) {
    // Bind the fresh tab by its stable id — the Mac's createTab(customGuid:)
    // handing the new tab the pin's identity.
    std::optional<base::Value> parsed = base::JSONReader::Read(
        framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
    if (parsed && parsed->is_list()) {
      for (const base::Value& v : parsed->GetList()) {
        const base::DictValue* d = v.GetIfDict();
        if (d && d->FindInt("index").value_or(-1) == index) {
          const int id = d->FindInt("id").value_or(0);
          if (id) {
            bindings[guid] = id;
          }
          break;
        }
      }
    }
  }
  RefreshTabs();
}

int PhiWindowController::IndexOfTabId(int64_t browser, int id) const {
  if (!framework_ || !id) {
    return -1;
  }
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework_->ListTabsJson(browser), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return -1;
  }
  for (const base::Value& v : parsed->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    if (d && d->FindInt("id").value_or(0) == id) {
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
  EnsureMenuTitleRects();
  for (const auto& [title, rect] : menu_titles_) {
    if (::PtInRect(&rect, client_pt)) {
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
  // Clicking anywhere else in the chrome means the user is done editing the
  // address; hand the keyboard back to the page (the field is a child window,
  // so clicks on it never reach here).
  if (url_field_ && ::GetFocus() == url_field_) {
    FocusActiveBrowser();
  }
  const Layout layout = ComputeLayout();
  POINT pt = {x, y};
  // A menu title in the caption band drops its menu, aligned under the title.
  if (::PtInRect(&layout.title_bar, pt) && menu_bar_) {
    EnsureMenuTitleRects();
    for (size_t i = 0; i < menu_titles_.size(); ++i) {
      if (::PtInRect(&menu_titles_[i].second, pt)) {
        POINT anchor = {menu_titles_[i].second.left, layout.title_bar.bottom};
        ::ClientToScreen(shell_hwnd_, &anchor);
        menu_bar_->ShowAt(i, anchor.x, anchor.y);
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
  for (const ShortcutRow& pin : pinned_) {
    if (::PtInRect(&pin.bounds, pt)) {
      OpenOrFocusPinned(pin.guid, pin.url);
      return;
    }
  }
  for (const ShortcutRow& bookmark : bookmarks_) {
    if (::PtInRect(&bookmark.bounds, pt)) {
      if (!bookmark.folder && !bookmark.url.empty()) {
        // Sidebar bookmark rows bind like pins (bookmarkGuidBacking); the
        // Bookmarks menu keeps opening plain tabs.
        OpenOrFocusPinned(bookmark.guid, bookmark.url);
      }
      return;
    }
  }
  if (framework_ && ::PtInRect(&layout.new_tab_row, pt)) {
    framework_->NewTab(active_browser_ ? active_browser_ : window_id_, "");
    RefreshTabs();
    return;
  }
  if (framework_ && ::PtInRect(&layout.sidebar_toggle, pt)) {
    framework_->NewWindow(window_id_);  // Stands in until the toggle lands.
    return;
  }
  RebuildTabs();  // Chips and rows may be stale if no paint has run yet.
  for (const SpaceChip& chip : chips_) {
    if (!::PtInRect(&chip.bounds, pt)) {
      continue;
    }
    if (layout.horizontal_tabs) {
      // Comfortable draws ONE compact pip — the active Space. Clicking it
      // opens the switcher popup over the FULL Space list, the Mac's
      // SpacesStripView compact-chip behaviour.
      std::vector<std::pair<std::string, std::wstring>> spaces;
      if (host_) {
        std::optional<base::Value> all = base::JSONReader::Read(
            host_->AllSpacesJson(), base::JSON_PARSE_RFC);
        if (all && all->is_list()) {
          for (const base::Value& v : all->GetList()) {
            if (!v.is_dict() || !v.GetDict().FindString("id")) {
              continue;
            }
            const base::DictValue& d = v.GetDict();
            const std::string* name = d.FindString("name");
            const std::string* icon = d.FindString("iconText");
            std::wstring title =
                base::UTF8ToWide(name && !name->empty()
                                     ? *name
                                     : "Space " + *d.FindString("id"));
            if (icon && !icon->empty()) {
              const std::wstring glyph = base::UTF8ToWide(*icon);
              if (!PhiIsPrivateGlyph(glyph)) {
                title = glyph + L"  " + title;
              }
            }
            spaces.emplace_back(*d.FindString("id"), std::move(title));
          }
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
    host_->RequestCreateSpace(window_id_, /*new_profile=*/false);
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
        framework_->ActivateTab(browser, row.index);
      }
    }
    RefreshTabs();
    return;
  }
}

namespace {

// A filled rounded rect, the Mac client's selection/field shape.
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

}  // namespace

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
      enum : UINT { kPin = 1, kBookmark, kClose, kSplit, kUnsplit,
                    kGroup, kUngroupTab };
      HMENU menu = ::CreatePopupMenu();
      if (!menu) {
        return;
      }
      const bool merged = row.b_index >= 0;
      if (!merged) {
        ::AppendMenuW(menu, MF_STRING, kPin, L"Pin Tab");
        ::AppendMenuW(menu, MF_STRING, kBookmark, L"Bookmark Tab...");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        // Splitting needs a partner: the active tab, itself unsplit.
        if (!row.active && row.split_id.empty()) {
          ::AppendMenuW(menu, MF_STRING, kSplit, L"Split With Active Tab");
        }
        if (row.group_id.empty()) {
          ::AppendMenuW(menu, MF_STRING, kGroup, L"Add Tab to New Group");
        } else {
          ::AppendMenuW(menu, MF_STRING, kUngroupTab, L"Remove from Group");
        }
      } else {
        ::AppendMenuW(menu, MF_STRING, kUnsplit, L"Unsplit");
      }
      ::AppendMenuW(menu, MF_STRING, kClose,
                    merged ? L"Close Split" : L"Close Tab");
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN |
                    TPM_RIGHTBUTTON,
          screen.x, screen.y, 0, shell_hwnd_, nullptr));
      ::DestroyMenu(menu);
      const int64_t browser = active_browser_ ? active_browser_ : window_id_;
      if (chosen == kPin) {
        host_->RequestStoreTab(window_id_, row.index, /*as_bookmark=*/false);
      } else if (chosen == kBookmark) {
        host_->RequestStoreTab(window_id_, row.index, /*as_bookmark=*/true);
      } else if (chosen == kSplit && framework_) {
        framework_->SplitActiveWith(browser, row.index);
        RefreshTabs();
      } else if (chosen == kUnsplit && framework_) {
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
      }
      return;
    }
    for (const ShortcutRow& pin : pinned_) {
      if (!::PtInRect(&pin.bounds, pt)) {
        continue;
      }
      enum : UINT { kOpen = 1, kUnpin };
      HMENU menu = ::CreatePopupMenu();
      if (!menu) {
        return;
      }
      ::AppendMenuW(menu, MF_STRING, kOpen, L"Open");
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      ::AppendMenuW(menu, MF_STRING, kUnpin, L"Unpin");
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN |
                    TPM_RIGHTBUTTON,
          screen.x, screen.y, 0, shell_hwnd_, nullptr));
      const std::string guid = pin.guid;
      const std::string url = pin.url;
      ::DestroyMenu(menu);
      if (chosen == kOpen && !url.empty()) {
        OpenOrFocusPinned(guid, url);
      } else if (chosen == kUnpin && !guid.empty()) {
        // Dropping the store row also drops the binding at the next rebuild,
        // so a live bound tab returns to the tab list as a normal tab.
        host_->RemoveStoredRow(guid);
      }
      return;
    }
    for (const ShortcutRow& bookmark : bookmarks_) {
      if (!::PtInRect(&bookmark.bounds, pt)) {
        continue;
      }
      enum : UINT { kOpen = 1, kRemove };
      HMENU menu = ::CreatePopupMenu();
      if (!menu) {
        return;
      }
      if (!bookmark.folder) {
        ::AppendMenuW(menu, MF_STRING, kOpen, L"Open");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      }
      ::AppendMenuW(menu, MF_STRING, kRemove,
                    bookmark.folder ? L"Delete Folder" : L"Delete Bookmark");
      POINT screen = pt;
      ::ClientToScreen(shell_hwnd_, &screen);
      const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN |
                    TPM_RIGHTBUTTON,
          screen.x, screen.y, 0, shell_hwnd_, nullptr));
      const std::string guid = bookmark.guid;
      const std::string url = bookmark.url;
      const bool folder = bookmark.folder;
      ::DestroyMenu(menu);
      if (chosen == kOpen && !folder && !url.empty()) {
        OpenOrFocusPinned(guid, url);
      } else if (chosen == kRemove && !guid.empty()) {
        host_->RemoveStoredRow(guid);  // A folder takes its subtree.
      }
      return;
    }
    return;
  }

  // The Mac's Space row menu: activate, rename, change icon, set theme,
  // delete. Rename prompts, delete confirms (refused for the default Space);
  // Change Icon still waits for an inline picker — Settings > Spaces has the
  // real one.
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
  ::AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_GRAYED, kChangeIcon,
                L"Change Icon…");
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

void PhiWindowController::PaintChrome(HDC dc,
                                     const Layout& layout,
                                     bool include_child_controls) {
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  // The SLOT's palette: a Space may pin its own theme.
  const PhiPalette& palette =
      host_ ? host_->PaletteForSlot(window_id_) : kFallback;
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
  for (int i = 0; i < bands; ++i) {
    RECT band = {0, height * i / bands, layout.client.right,
                 height * (i + 1) / bands};
    const int r = GetRValue(kTintTop) +
                  (GetRValue(kTintBottom) - GetRValue(kTintTop)) * i / bands;
    const int g = GetGValue(kTintTop) +
                  (GetGValue(kTintBottom) - GetGValue(kTintTop)) * i / bands;
    const int b = GetBValue(kTintTop) +
                  (GetBValue(kTintBottom) - GetBValue(kTintTop)) * i / bands;
    HBRUSH brush = ::CreateSolidBrush(RGB(r, g, b));
    ::FillRect(dc, &band, brush);
    ::DeleteObject(brush);
  }

  ::SelectClipRgn(dc, nullptr);

  // The card's hairline: 1px of `border` just outside the content region
  // (the Mac's contentOuterBorder), ROUNDED to follow the card's corners —
  // a square frame would cut straight across the corner arcs. Under a top
  // bar the card begins at the bar's top edge, so the outline wraps both.
  if (layout.content.right > layout.content.left) {
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

  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = ui_font_ ? ::SelectObject(dc, ui_font_) : nullptr;

  // The caption band: menu titles leading, minimize / maximize / close
  // trailing — the menu bar ON the title row. The ground already painted it;
  // only the titles, glyphs and hot states go on top.
  if (layout.title_bar.right > layout.title_bar.left) {
    // The logo is loaded AT the size it draws (the multi-size ico has exact
    // entries per DPI), so no GDI rescale blurs it.
    const int logo_size = Scaled(18);
    // The logo's pixels, extracted ONCE from the ICO's 256px entry; the
    // D2D path scales them to any DPI without an exact-size ICO entry.
    static base::NoDestructor<std::vector<unsigned char>> logo_store;
    std::vector<unsigned char>& logo_bgra = *logo_store;
    static int logo_native = 0;
    static HICON logo = nullptr;
    if (logo_bgra.empty() && !logo) {
      if (HICON big = PhiAppIcon(256)) {
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
          if (screen &&
              ::GetDIBits(screen, info.hbmColor, 0, 256, straight.data(),
                          &bmi, DIB_RGB_COLORS) == 256) {
            // Straight alpha -> premultiplied, what the D2D path expects.
            for (size_t px = 0; px + 3 < straight.size(); px += 4) {
              const unsigned a = straight[px + 3];
              straight[px] =
                  static_cast<unsigned char>(straight[px] * a / 255);
              straight[px + 1] =
                  static_cast<unsigned char>(straight[px + 1] * a / 255);
              straight[px + 2] =
                  static_cast<unsigned char>(straight[px + 2] * a / 255);
            }
            logo_bgra = std::move(straight);
            logo_native = 256;
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
      }
      if (logo_bgra.empty()) {
        logo = PhiAppIcon(logo_size);  // Fallback: the nearest ICO entry.
      }
    }
    EnsureMenuTitleRects();
    for (size_t i = 0; i < menu_titles_.size(); ++i) {
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
    HGDIOBJ old_brush = ::SelectObject(dc, bg);
    HGDIOBJ old_pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::RoundRect(dc, band.left, band.top, band.right + 1, band.bottom + 1,
                radius, radius);
    RECT lower = {band.left, band.top + radius, band.right, band.bottom};
    ::FillRect(dc, &lower, bg);
    ::SelectObject(dc, old_pen);
    ::SelectObject(dc, old_brush);
    ::DeleteObject(bg);
    RECT sep = {band.left, band.bottom - 1, band.right, band.bottom};
    HBRUSH sep_brush = ::CreateSolidBrush(palette.separator);
    ::FillRect(dc, &sep, sep_brush);
    ::DeleteObject(sep_brush);
  }

  // Nav row. Glyphs stand in for the Mac's SF Symbols. Each mode empties the
  // rects it hides, so nothing draws where a control does not exist.
  DrawGlyph(dc, layout.sidebar_toggle, L"\u25A3", kLabel);   // sidebar toggle
  DrawGlyph(dc, layout.search_tabs, L"\u2315", kLabel);      // search tabs
  DrawGlyph(dc, layout.back, L"\u2039", can_go_back_ ? kLabel : kLabelDim);
  DrawGlyph(dc, layout.forward, L"\u203A",
            can_go_forward_ ? kLabel : kLabelDim);
  DrawGlyph(dc, layout.reload, nav_loading_ ? L"\u00D7" : L"\u21BB", kLabel);

  // Address field. In the sidebar it is a pill on the 4% wash with no border
  // (SideAddressBar); in the top bar it is bare -- transparent until hovered on
  // Mac -- so only the EDIT's text shows there. The capture path (no children
  // rendered) draws the text itself so a screenshot still shows the address.
  if (layout.mode == PhiLayoutMode::kPerformance) {
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
    url_rc.right -= Scaled(8);
    ::DrawTextW(dc, shown.empty() ? L"Search or Enter URL" : shown.c_str(),
                -1, &url_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
  }

  // Space pips: 24pt, icon only. The selected one sits on a selected-pill
  // card (SpacesStripView's matchedGeometry indicator); the trailing "+"
  // creates a Space.
  for (const SpaceChip& chip : chips_) {
    const bool active = (chip.id == active_space_);
    if (active) {
      FillRounded(dc, chip.bounds, kRowSelectedBg, Scaled(kCornerRadius));
      if (palette.dark) {
        StrokeRounded(dc, chip.bounds, kRowSelectedBg, kActivePillBorder,
                      Scaled(kCornerRadius));
      }
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
    if (!chip.icon.empty() && !PhiIsPrivateGlyph(chip.icon) &&
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
  DrawGlyph(dc, layout.add_space, L"+", kLabelDim);

  // Tab groups: TabGroupCellView — the whole group cell rests on an 18%
  // tint of the group color; the 32pt header row carries the chevron, the
  // color dot (white ring) and the title. Clicking the header collapses.
  for (const GroupHeader& gh : group_headers_) {
    FillRounded(dc, gh.container, BlendOver(gh.color, kTintTop, kGroupTintAlpha),
                Scaled(kCornerRadius));
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
  for (const TabRow& row : tabs_) {
    RECT pill = row.bounds;
    if (!layout.horizontal_tabs) {
      pill.top += Scaled(kRowPad);
      pill.bottom -= Scaled(kRowPad);
    }
    if (row.active) {
      FillRounded(dc, pill, kRowSelectedBg, Scaled(kCornerRadius));
      if (palette.dark) {
        StrokeRounded(dc, pill, kRowSelectedBg, kActivePillBorder,
                      Scaled(kCornerRadius));
      }
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
    const COLORREF row_bg = row.active ? kRowSelectedBg : ground;
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
      if (!favicon.empty()) {
        DrawFavicon(dc, icon, favicon.data(),
                    pane_b ? r.b_favicon_width : r.favicon_width,
                    pane_b ? r.b_favicon_height : r.favicon_height, row_bg);
      } else {
        // The Mac's no-favicon placeholder: the globe (FaviconProvider).
        HGDIOBJ previous_glyph_font =
            caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
        DrawGlyph(dc, icon, L"\uE774", kLabelDim);
        if (previous_glyph_font) {
          ::SelectObject(dc, previous_glyph_font);
        }
      }
      ::SetTextColor(dc, kLabel);
      RECT title_rc = {slot.right + Scaled(8), pill.top, right - Scaled(4),
                       pill.bottom};
      ::DrawTextW(dc, (pane_b ? r.b_title : r.title).c_str(), -1, &title_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    };
    if (row.b_index >= 0) {
      const int div_x = (pill.left + pill.right) / 2;
      draw_pane(pill.left, div_x, row, /*pane_b=*/false);
      draw_pane(div_x, row.active ? row.close_box.left : pill.right, row,
                /*pane_b=*/true);
      RECT divider = {div_x, pill.top + Scaled(6), div_x + 1,
                      pill.bottom - Scaled(6)};
      HBRUSH sep = ::CreateSolidBrush(palette.separator);
      ::FillRect(dc, &divider, sep);
      ::DeleteObject(sep);
    } else {
      draw_pane(pill.left, row.active ? row.close_box.left : pill.right, row,
                /*pane_b=*/false);
    }
    if (row.active) {
      DrawGlyph(dc, row.close_box, L"\u00D7", kLabelDim);
    }
  }

  // Pinned tabs: PinnedTabItem cells -- resting on the 4% wash, an 18pt
  // favicon centred, radius 8. The ACTIVE pin (its bound live tab is the
  // active tab) gets the selected wash and the Mac's 2pt accent ring; in the
  // horizontal strip the cells are the 28pt pinned section and the active one
  // reads like the neighbouring strip tabs (selected wash, no ring).
  for (const ShortcutRow& pin : pinned_) {
    const COLORREF pin_bg = pin.active ? palette.selected : palette.hover;
    FillRounded(dc, pin.bounds, pin_bg, Scaled(kCornerRadius));
    if (pin.active && !layout.horizontal_tabs) {
      HPEN ring = ::CreatePen(PS_SOLID, Scaled(2), palette.accent);
      HGDIOBJ old_pen = ::SelectObject(dc, ring);
      HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
      RECT rr = pin.bounds;
      ::InflateRect(&rr, -Scaled(1), -Scaled(1));
      const int d = 2 * Scaled(kCornerRadius);
      ::RoundRect(dc, rr.left, rr.top, rr.right, rr.bottom, d, d);
      ::SelectObject(dc, old_brush);
      ::SelectObject(dc, old_pen);
      ::DeleteObject(ring);
    }
    if (!pin.favicon.empty()) {
      const int size = Scaled(layout.horizontal_tabs ? kStripPinFavicon
                                                     : kPinnedFavicon);
      const int cx = (pin.bounds.left + pin.bounds.right) / 2;
      const int cy = (pin.bounds.top + pin.bounds.bottom) / 2;
      const RECT icon = {cx - size / 2, cy - size / 2, cx + size / 2,
                         cy + size / 2};
      DrawFavicon(dc, icon, pin.favicon.data(), pin.favicon_width,
                  pin.favicon_height, pin_bg);
    } else {
      ::SetTextColor(dc, kLabel);
      const std::wstring initial =
          pin.title.empty() ? std::wstring(L"?") : pin.title.substr(0, 1);
      ::DrawTextW(dc, initial.c_str(), -1, const_cast<RECT*>(&pin.bounds),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }

  // Bookmarks: rows shaped like tab rows (BookmarkCellView) -- primary-label
  // titles, folders marked with the disclosure glyph.
  for (const ShortcutRow& bookmark : bookmarks_) {
    RECT pill = bookmark.bounds;
    pill.top += Scaled(kRowPad);
    pill.bottom -= Scaled(kRowPad);
    if (bookmark.active) {
      // Its bound live tab is the active tab: the row IS the tab, so it
      // reads exactly like the active tab row.
      FillRounded(dc, pill, kRowSelectedBg, Scaled(kCornerRadius));
      if (palette.dark) {
        StrokeRounded(dc, pill, kRowSelectedBg, kActivePillBorder,
                      Scaled(kCornerRadius));
      }
    }
    RECT text = pill;
    const int mid = (pill.top + pill.bottom) / 2;
    RECT icon = {pill.left + Scaled(6) +
                     (Scaled(kFaviconSlot) - Scaled(kFaviconSize)) / 2,
                 mid - Scaled(kFaviconSize) / 2, 0, 0};
    icon.right = icon.left + Scaled(kFaviconSize);
    icon.bottom = icon.top + Scaled(kFaviconSize);
    if (!bookmark.favicon.empty()) {
      DrawFavicon(dc, icon, bookmark.favicon.data(), bookmark.favicon_width,
                  bookmark.favicon_height,
                  bookmark.active ? kRowSelectedBg : kTintTop);
    } else if (!bookmark.folder) {
      HGDIOBJ previous_glyph_font =
          caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
      DrawGlyph(dc, icon, L"\uE774", kLabelDim);
      if (previous_glyph_font) {
        ::SelectObject(dc, previous_glyph_font);
      }
    }
    text.left = pill.left + Scaled(6) + Scaled(kFaviconSlot) + Scaled(8);
    ::SetTextColor(dc, kLabel);
    const std::wstring label =
        bookmark.folder ? (std::wstring(L"\u25B8  ") + bookmark.title)
                        : bookmark.title;
    ::DrawTextW(dc, label.c_str(), -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  }

  // "New Tab": the row heading the tab list -- a 16pt "+" 6pt in, an 8pt
  // gap, the title in textTertiary (NewTabButtonCellView).
  if (layout.new_tab_row.right > layout.new_tab_row.left) {
    const RECT& row = layout.new_tab_row;
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

  // Bottom bar (SidebarBottomBar): downloads and memory leading, then the
  // Feedback capsule (hairline border) and the Chat capsule (accent ground).
  // All placeholders -- drawn so the window reads like the Mac one; their
  // functions arrive later. There is NO settings gear here: settings opens
  // from the menu bar, as on Mac.
  DrawGlyph(dc, layout.downloads, L"\u2913", kLabel);
  DrawGlyph(dc, layout.memory, L"\u25C7", kLabel);
  if (layout.feedback.right > layout.feedback.left) {
    const int pill_radius = layout.feedback.bottom - layout.feedback.top;
    StrokeRounded(dc, layout.feedback, kTintBottom, kBorder, pill_radius);
    ::SetTextColor(dc, kLabel);
    RECT feedback_rc = layout.feedback;
    ::DrawTextW(dc, L"\u2690  Feedback", -1, &feedback_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  if (layout.chat.right > layout.chat.left) {
    const int pill_radius = layout.chat.bottom - layout.chat.top;
    FillRounded(dc, layout.chat, palette.accent, pill_radius);
    ::SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    RECT chat_rc = layout.chat;
    ::DrawTextW(dc, L"Chat", -1, &chat_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

// static
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
      if (!in_chrome && self && self->browser_hwnd_ &&
          ::IsWindow(self->browser_hwnd_)) {
        ::SetFocus(self->browser_hwnd_);
        if (self->framework_) {
          self->framework_->FocusBrowserWindow(self->window_id_);
        }
      }
      return MA_ACTIVATE;
    }
    case WM_ACTIVATE:
    case WM_SETFOCUS:
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
        const PhiPalette& palette =
            self->host_->PaletteForSlot(self->window_id_);
        // In the sidebar the field rests on the 4% wash; in a top bar it sits
        // bare on the bar's own ground.
        const COLORREF bg =
            self->layout_mode_ == PhiLayoutMode::kPerformance
                ? palette.hover
                : palette.window_bg;
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
      }
      break;
    case WM_MOUSEMOVE:
      if (self) {
        self->SetCaptionHot(0);  // Client moves mean we left the buttons.
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        int hot = -1;
        const RECT band = self->ComputeLayout().title_bar;
        if (::PtInRect(&band, pt)) {
          self->EnsureMenuTitleRects();
          for (size_t i = 0; i < self->menu_titles_.size(); ++i) {
            if (::PtInRect(&self->menu_titles_[i].second, pt)) {
              hot = static_cast<int>(i);
              break;
            }
          }
        }
        self->SetMenuHot(hot);
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      break;
    case WM_MOUSELEAVE:
      if (self) {
        self->SetCaptionHot(0);
        self->SetMenuHot(-1);
      }
      break;
    case WM_PHI_RAISE_CAPS:
      if (self) {
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
      break;
    case WM_RBUTTONUP:
      if (self) {
        self->OnChromeContextMenu(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
      if (self) {
        self->OnChromeClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_SIZE:
      if (self) {
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
        // Re-read framework state at paint time: the sidebar then reflects
        // reality even if a change notification was missed or coalesced.
        self->RebuildTabs();
        // Compose off-screen and blit once. Painting straight to the window
        // shows the chrome rebuilding element by element — the visible blink
        // on every tab/Space change, which invalidates the whole window.
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP bmp = mem ? ::CreateCompatibleBitmap(
                                dc, rc.right > 0 ? rc.right : 1,
                                rc.bottom > 0 ? rc.bottom : 1)
                          : nullptr;
        if (bmp) {
          HGDIOBJ old_bmp = ::SelectObject(mem, bmp);
          // PaintChrome excludes the content rect (the browser child owns
          // it), so give those buffer pixels a defined color for the rare
          // blit with no child covering them (startup, degenerate states).
          ::FillRect(mem, &rc, reinterpret_cast<HBRUSH>(
                                   ::GetStockObject(WHITE_BRUSH)));
          self->PaintChrome(mem, self->ComputeLayout(),
                            /*include_child_controls=*/false);
          ::BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left,
                   ps.rcPaint.top, SRCCOPY);
          ::SelectObject(mem, old_bmp);
          ::DeleteObject(bmp);
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
