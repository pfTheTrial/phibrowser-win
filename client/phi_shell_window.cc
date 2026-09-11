// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_shell_window.h"

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

#include "base/auto_reset.h"
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
#include "phibrowser-win/client/phi_agent_spaces.h"
#include "phibrowser-win/client/phi_downloads_popover.h"
#include "phibrowser-win/client/phi_extensions_popover.h"
#include "phibrowser-win/client/phi_group_overview.h"
#include "phibrowser-win/client/phi_search_tabs_panel.h"
#include "phibrowser-win/client/phi_space_theme_panel.h"
#include "phibrowser-win/client/phi_bookmark_manager.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_create_space_panel.h"
#include "phibrowser-win/client/phi_create_space_window.h"
#include "phibrowser-win/client/phi_folder_icon_picker.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_tab_preview.h"
#include "phibrowser-win/client/phi_tab_switcher.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_mac_icons.h"
#include "phibrowser-win/client/phi_menu_bar.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_new_tab_page.h"
#include "phibrowser-win/client/phi_reader.h"
#include "phibrowser-win/client/phi_uia.h"
#include "phibrowser-win/client/phi_prompt.h"

namespace phi_client {

namespace {

constexpr wchar_t kShellClassName[] = L"PhiClientShell";

using phi_framework::SplitSnapshot;
using phi_framework::TabSnapshot;
using phi_framework::WindowSnapshot;

// --- Metrics -----------------------------------------------------------------
// Design units (points at 100%), taken from the Mac client so the two look the
// same: WebContentConstant, SidebarHeaderView, SpacesStripView,
// PinnedTabLayout, TabStripMetrics and SidebarBottomBar. The ones the
// geometry is built from live in phi_shell_layout.h (the pure, host-tested
// layout reads them); the paint-only ones stay here.
using shell_metrics::kAddressBarHeight;
using shell_metrics::kAddressBarTop;
using shell_metrics::kBandDividerHeight;
using shell_metrics::kBandGroupGap;
using shell_metrics::kBandTrailingGap;
using shell_metrics::kBookmarkBarHeight;
using shell_metrics::kBookmarkSeparatorHeight;
using shell_metrics::kBottomBarHeight;
using shell_metrics::kBottomSpacer;
using shell_metrics::kCaptionButtonWidth;
using shell_metrics::kChatWidth;
using shell_metrics::kComfortableLeading;
using shell_metrics::kContentMargin;
using shell_metrics::kContentTopGap;
using shell_metrics::kEdgeInset;
using shell_metrics::kFaviconSlot;
using shell_metrics::kFavoritesSpacer;
using shell_metrics::kFeedbackIconWidth;
using shell_metrics::kHTabStripLeading;
using shell_metrics::kHTabStripTop;
using shell_metrics::kHeaderHeight;
using shell_metrics::kHeaderSpacer;
using shell_metrics::kNavButton;
using shell_metrics::kNavGroupSpacing;
using shell_metrics::kNewTabRowHeight;
using shell_metrics::kOmniboxHeight;
using shell_metrics::kPillHeight;
using shell_metrics::kPinnedCellHeight;
using shell_metrics::kPinnedGap;
using shell_metrics::kSpacesStripHeight;
using shell_metrics::kSpacesStripPad;
using shell_metrics::kSpacesStripTop;
using shell_metrics::kTabRowGap;
using shell_metrics::kTabRowHeight;
using shell_metrics::kTitleBarHeight;
using shell_metrics::kTitleBarHeightTabs;
using shell_metrics::kTopBarField;
using shell_metrics::kTopBarHeight;
using shell_metrics::kTopBarHeightNav;
using shell_metrics::kTopBarLeading;
using shell_metrics::kTopBarSpacing;
// (The menu titles' padding metrics moved with them, into
// phi_caption_band.cc.)
// The selected card's corner RADIUS — and it is passed DOUBLED to the
// draws: GDI's RoundRect takes the corner's ellipse, not its radius, so
// every rounded rect in this file that hands its radius straight over is
// rounding half as hard as the Mac constant it is named for. The Mac draws
// 8 on its 24pt chip (a third of the side) as a `.continuous` squircle;
// 9 of real radius stands in for that on a circular arc. It is also about
// as far as the corner goes: the colour pass clears the glyph box whole,
// and a rounder card would have the box's own corners poking out of it and
// squaring it back off.
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
// How far a shell with no placement to inherit sits off the work area's
// top-left corner (see the first-run placement in the constructor).
constexpr int kDefaultPlacementMargin = 24;
constexpr int kContentRadius = 14;  // webContentContainerCornerRadius (macOS 26)
// 0x515 and 0x516 belonged to the Space swap and hover-card clocks; the
// swap now rides PhiAnimator and the card's dwell timer is
// PhiSpacesStrip::kHoverCardTimer (still 0x516), which the strip owns.
// 0x518 is PhiSidebarDragSession::kSlideTimer — the reorder slide, whose
// clock the drag session owns and whose timer rides this window; 0x519 is
// its kHoverExpandTimer (a drag resting on a collapsed folder or group).
constexpr UINT_PTR kFloatingTickTimer = 0x522;
// 0x523 is PhiOmniboxSession::kCopyLinkTimer — the shell routes it back to
// the session, which owns the copy-link flash.
// 0x524 was the Spaces strip's "+" fade; it rides PhiAnimator now.
// Re-arms a kMsgPhiDeferredClose that found a nested loop still on the
// stack (see PhiShellWindow::RequestClose).
constexpr UINT_PTR kDeferredCloseTimer = 0x525;
constexpr UINT kDeferredCloseRetryMs = 50;

// The row metrics the sidebar owns and the chrome below shares — the pill
// radius, the favicon and close-box sizes, the outline's indent
// (phi_sidebar_view.h). The tab-group and strip-tab constants went with the
// rows they belong to.
using sidebar_metrics::kBookmarkIndent;
using sidebar_metrics::kCloseBoxSize;
using sidebar_metrics::kCornerRadius;
using sidebar_metrics::kFaviconSize;
using sidebar_metrics::kFolderIconSize;
using sidebar_metrics::kRowPad;
// The bottom bar's two capsules (Feedback bordered, Chat on the accent).
// The sidebar bottom bar is the only place Feedback has a band now: the
// comfortable header that carried the labelled variant is gone, and its
// entry moved into the overflow menu. The paint keeps the label branch —
// ViewThatFits picks it as soon as a band is wide enough for it again.

// --- Palette (light) ---------------------------------------------------------
// From Assets.xcassets/ColorSet: primary-label 85% black, common-border 8%
// black, sidebar-tab-selected white. The sidebar ground stands in for the Mac's
// material + per-Space gradient tint, which GDI cannot reproduce.
// Every colour now comes from the active theme (phi_theme.h), resolved by the
// coordinator and read at paint time.

ATOM EnsureShellClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    // CS_DBLCLKS: a group header's double-click opens the group overview.
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &PhiShellWindow::ShellWndProc;
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

int PhiShellWindow::Scaled(int design_units) const {
  return ::MulDiv(design_units, static_cast<int>(dpi_), 96);
}

RECT PhiShellWindow::Scaled(const RECT& design_units) const {
  return {Scaled(design_units.left), Scaled(design_units.top),
          Scaled(design_units.right), Scaled(design_units.bottom)};
}

void PhiShellWindow::EnsureFont() {
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
  if (omnibox_) {
    omnibox_->SetFont(ui_font_);
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

void PhiShellWindow::RebuildMenuBar() {
  if (caption_band_) {
    caption_band_->Rebuild();
  }
}

// ShellLayout → the RECT-based Layout the paint and hit-test code reads.
// static
PhiShellWindow::Layout PhiShellWindow::ToLayout(
    const ShellLayout& shell) {
  Layout out;
  out.mode = shell.mode;
  out.client = PhiToRect(shell.client);
  out.title_bar = PhiToRect(shell.title_bar);
  out.caption_min = PhiToRect(shell.caption_min);
  out.caption_max = PhiToRect(shell.caption_max);
  out.caption_close = PhiToRect(shell.caption_close);
  out.band_limit = shell.band_limit;
  out.sidebar = PhiToRect(shell.sidebar);
  out.top_bar = PhiToRect(shell.top_bar);
  out.content = PhiToRect(shell.content);
  out.bookmark_bar = PhiToRect(shell.bookmark_bar);
  out.search_tabs = PhiToRect(shell.search_tabs);
  out.band_divider = PhiToRect(shell.band_divider);
  out.back = PhiToRect(shell.back);
  out.forward = PhiToRect(shell.forward);
  out.reload = PhiToRect(shell.reload);
  out.spaces_strip = PhiToRect(shell.spaces_strip);
  out.url_field = PhiToRect(shell.url_field);
  out.favorites = PhiToRect(shell.favorites);
  out.bookmarks = PhiToRect(shell.bookmarks);
  out.bookmark_separator = PhiToRect(shell.bookmark_separator);
  out.new_tab_row = PhiToRect(shell.new_tab_row);
  out.bottom_bar = PhiToRect(shell.bottom_bar);
  out.downloads = PhiToRect(shell.downloads);
  out.memory = PhiToRect(shell.memory);
  out.feedback = PhiToRect(shell.feedback);
  out.chat = PhiToRect(shell.chat);
  out.extensions = PhiToRect(shell.extensions);
  out.overflow = PhiToRect(shell.overflow);
  out.phi_menu = PhiToRect(shell.phi_menu);
  out.page_icon = PhiToRect(shell.page_icon);
  out.copy_link = PhiToRect(shell.copy_link);
  out.reader = PhiToRect(shell.reader);
  out.field_ext_strip = PhiToRect(shell.field_ext_strip);
  out.add_space = PhiToRect(shell.add_space);
  out.chip_size = shell.chip_size;
  out.chips_left = shell.chips_left;
  out.chips_top = shell.chips_top;
  out.chips_limit = shell.chips_limit;
  out.tabs_left = shell.tabs_left;
  out.tabs_top = shell.tabs_top;
  out.tabs_limit = shell.tabs_limit;
  out.horizontal_tabs = shell.horizontal_tabs;
  out.spaces_switch = shell.spaces_switch;
  return out;
}

ShellLayoutInputs PhiShellWindow::CurrentLayoutInputs() const {
  ShellLayoutInputs in;
  in.mode = layout_mode_;
  if (shell_hwnd_) {
    RECT client = {};
    ::GetClientRect(shell_hwnd_, &client);
    in.client_width = client.right;
    in.client_height = client.bottom;
  }
  in.dpi = static_cast<int>(dpi_);
  in.sidebar_dip = sidebar_dip_;
  // Collapsed: the content card widens to the left margin (which becomes
  // the floating sidebar's hover trigger). The floating panel paints with
  // floating_layout_ set, which restores the expanded layout.
  in.sidebar_hidden = sidebar_collapsed_ && !floating_layout_;
  in.content_fullscreen = browsers_->content_fullscreen();
  in.bookmark_bar = layout_bookmark_bar_;
  in.reader_offerable = layout_reader_;
  in.ai_enabled = kPhiAiEnabled;
  in.field_ext_count = field_ext_count_;
  in.pinned_row_count = pinned_row_count_;
  in.bookmark_row_count = bookmark_row_count_;
  in.pinned_drop_zone = pinned_drop_zone_;
  in.bookmark_drop_zone = bookmark_drop_zone_;
  // The Mac's updateSpaceSwitchVisibility: the switch shows only with more
  // than one Space to switch between — or while the create form is up,
  // whose placeholder pip lives in the strip (forcesSpaceSwitchVisible).
  in.spaces_switch =
      spaces_ && (spaces_->SwitchTargets() > 1 || spaces_->creating());
  return in;
}

void PhiShellWindow::RefreshLayoutInputs() {
  // The sidebar width is the host's pref except while the user drags the
  // edge, when the live value wins until release persists it.
  if (!sidebar_resizing_ && services_.prefs) {
    sidebar_dip_ = std::max(kSidebarWidthMin,
                            std::min(services_.prefs->SidebarWidth(),
                                     kSidebarWidthMax));
  }
  layout_bookmark_bar_ = BookmarkBarVisible();
  layout_reader_ = ActiveTabReaderOfferable();
}

PhiShellWindow::Layout PhiShellWindow::ComputeLayout() {
  // A value, cached by its inputs: the geometry is recomputed only when
  // something it depends on moved (a resize, a DPI change, a mode or pref
  // change, the rebuild's counts), never per read — the old getter derived
  // it eight times per pointer move, each time re-querying the DPI and
  // creating fonts (lane 3 §3.2). dpi_ is written by WM_DPICHANGED and the
  // constructor alone.
  const ShellLayoutInputs inputs = CurrentLayoutInputs();
  if (!layout_cache_valid_ || inputs != layout_inputs_) {
    layout_inputs_ = inputs;
    layout_cache_ = ToLayout(ComputeShellLayout(inputs));
    layout_cache_valid_ = true;
  }
  return layout_cache_;
}

RECT PhiShellWindow::content_rect() {
  return ComputeLayout().content;
}

RECT PhiShellWindow::url_field_rect() {
  return ComputeLayout().url_field;
}

RECT PhiShellWindow::reader_button_rect() {
  return ComputeLayout().reader;
}

bool PhiShellWindow::ActiveTabExtensionOfferable() const {
  return services_.sidebar && active_tab_id().valid() &&
         services_.sidebar->IsReaderOfferable(active_tab_id().value,
                                              active_tab_url());
}

int PhiShellWindow::tabs_top() {
  return ComputeLayout().tabs_top;
}


int PhiShellWindow::tab_row_height() const {
  return Scaled(kTabRowHeight);
}

int PhiShellWindow::tab_row_gap() const {
  return Scaled(kTabRowGap);
}

int PhiShellWindow::title_bar_height() const {
  // Comfortable's band carries the tab strip as well as the window controls,
  // so it is taller there — this is what the control socket reports, and a
  // fixed answer would put every caller's arithmetic 8pt out in that layout.
  return Scaled(layout_mode_ == PhiLayoutMode::kComfortable
                    ? kTitleBarHeightTabs
                    : kTitleBarHeight);
}

int PhiShellWindow::sidebar_width() const {
  return Scaled(sidebar_dip_);
}

int PhiShellWindow::content_margin() const {
  return Scaled(kContentMargin);
}

POINT PhiShellWindow::frame_offset() const {
  POINT origin = {0, 0};
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return origin;
  }
  RECT window = {};
  ::GetWindowRect(shell_hwnd_, &window);
  ::ClientToScreen(shell_hwnd_, &origin);
  return {origin.x - window.left, origin.y - window.top};
}

void PhiShellWindow::ApplyPalette() {
  RefreshTabs();  // Every colour is read from the host at paint time.
}

void PhiShellWindow::ApplyLayoutMode(PhiLayoutMode mode) {
  if (layout_mode_ == mode) {
    return;
  }
  layout_mode_ = mode;
  LayoutContents();  // Also repositions the address field for the new mode.
  RefreshTabs();
}

PhiShellWindow::PhiShellWindow(SlotId slot,
                                        BrowserId browser,
                                        HWND browser_hwnd,
                                        phi_framework::PhiFramework* framework,
                                        const ShellServices& services,
                                        const PhiWindowModel* model,
                                        bool agent,
                               bool incognito,
                               bool start_hidden)
    : // The URL-driven client-drawn pages (bookmark manager, new-tab page).
      // First, so the early return below still leaves a usable host.
      native_pages_(std::make_unique<PhiNativePagesHost>(
          static_cast<PhiNativePagesShell*>(this))),
      // The comfortable layout's Space switch: the whole client area slides.
      content_slide_(std::make_unique<PhiContentSlide>(
          static_cast<PhiContentSlideShell*>(this))),
      slot_(slot),
      // The content area's component: the adopted browser child and the
      // slot's Space -> browser table.
      browsers_(std::make_unique<PhiBrowserHost>(
          static_cast<PhiBrowserHostShell*>(this))),
      // The caption band, which hosts the menu bar.
      caption_band_(std::make_unique<PhiCaptionBand>(
          static_cast<PhiCaptionShell*>(this))),
      // The address field's component.
      omnibox_(std::make_unique<PhiOmniboxSession>(
          static_cast<PhiOmniboxShell*>(this))),
      framework_(framework),
      services_(services),
      model_(model),
      // The Spaces strip: pips, hover card, create form, switch animation.
      spaces_(std::make_unique<PhiSpacesStrip>(
          static_cast<PhiSpacesStripShell*>(this),
          services.spaces,
          services.prefs,
          services.agent)),
      // The sidebar: the row pipeline, its band's paint and its drags.
      sidebar_(std::make_unique<PhiSidebarView>(
          static_cast<PhiSidebarShell*>(this),
          slot,
          framework,
          services.sidebar,
          services.spaces,
          services.prefs,
          services.agent,
          services.window,
          model,
          agent,
          incognito)),
      agent_(agent),
      incognito_(incognito) {
  if (!browser_hwnd || !::IsWindow(browser_hwnd)) {
    LOG(ERROR) << "[PhiClient] adopt: invalid browser HWND for browser "
               << browser.value << " (slot " << slot_.value << ")";
    return;
  }
  EnsureShellClass();
  if (services_.prefs) {
    // One layout for the whole client.
    layout_mode_ = services_.prefs->LayoutMode();
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
  ::GetWindowRect(browser_hwnd, &wr);
  if (const UINT dpi = ::GetDpiForWindow(browser_hwnd)) {
    dpi_ = dpi;  // Lay the first frame out for the monitor we open on.
  }
  EnsureFont();  // The fonts follow dpi_: here and in WM_DPICHANGED only.
  RefreshLayoutInputs();
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
          ::MonitorFromWindow(browser_hwnd, MONITOR_DEFAULTTOPRIMARY),
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
  if (!agent_ && services_.window && services_.window->TakeTearOffDrop(&drop)) {
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
  caption_band_->Install(shell_hwnd_);

  // The window the slot was created with backs the DEFAULT Space — whichever
  // Space holds that role now (it moves when its holder is deleted).
  const std::string default_space =
      services_.spaces ? services_.spaces->DefaultSpaceId() : std::string("1");
  browsers_->AdoptFirst(default_space, browser, browser_hwnd);
  omnibox_->EnsureField();
  if (services_.spaces && !incognito_ && !agent_) {
    // The default Space binds here rather than through AttachSpaceBrowser,
    // so it needs its own note or lazy restore would never learn its window.
    // NEVER for an agent/shadow window: it nominally binds the default Space
    // too, and noting it made the Space's remembered window THE AGENT'S —
    // an agent alive at quit then put the agent window (omitted from the
    // session) in the cold-start eager set and PARKED the user's real one.
    services_.spaces->NoteSpaceWindow(default_space, browser.value);
  }
  if (framework_ && !incognito_ && !agent_) {
    // An incognito shell stays OUT of the Space registry: routed URLs must
    // never resolve into an off-the-record window — and so does an agent
    // window, which the framework registers as "space.agent" itself.
    framework_->SetWindowSpace(browser.value, default_space.c_str());
  }
  LayoutContents();
  if (start_hidden && !agent_) {
    // The onboarding gate: the shell exists (its browser is alive and laid
    // out) but stays off screen until access is granted. Chromium shows the
    // browser HWND itself right after creation, so the child stays parked
    // where LayoutContents put it — inside a window that is not shown.
    shell_hidden_ = true;
  } else {
    ::ShowWindow(shell_hwnd_, agent_ ? SW_SHOWNOACTIVATE : SW_SHOW);
  }
  LOG(INFO) << "[PhiClient] " << (agent_ ? "agent " : "") << "shell window "
            << slot_.value << " browser=" << browser.value
            << " shell_hwnd=" << shell_hwnd_ << " browser_hwnd="
            << browser_hwnd;
}

PhiShellWindow::~PhiShellWindow() {
  // From here on the shell's own messages (DestroyWindow below still
  // delivers WM_ACTIVATE / WM_SIZE / WM_CAPTURECHANGED) find a controller
  // that must do nothing but die.
  destroying_ = true;
  // A hosted agent Space goes back to its own shell first, whole: the
  // browser is the agent's, and it must not die with this window — the
  // one sliding home, then the one on screen.
  FinishUnhost();
  if (HostsAgentSpace()) {
    leaving_agent_task_ = hosted_agent_task_;
    leaving_agent_space_ = hosted_agent_space_;
    hosted_agent_task_.clear();
    hosted_agent_space_.clear();
    FinishUnhost();
  }
  // A drag image that is sliding home has OUTLIVED its session (the slide
  // is the image window's own animation), so the window it was lifted out
  // of disowns it here — and BEFORE the drag is aborted below, so a window
  // closing under a live drag takes the image straight down rather than
  // animating it home over a shell that is going away.
  PhiDragImage::OwnerGone(shell_hwnd_);
  // The drag itself — the split-drop cards, the capture, the slide clock —
  // comes down with the session: ~PhiSidebarDragSession IS AbortDrag.
  sidebar_->AbortDrag();
  // The process-wide satellites bound to this window: each keeps a bare
  // PhiShellWindow* and outlives its shell otherwise (the agent
  // overlay's Reposition runs from EVERY shell's LayoutContents, the
  // overview and the switcher answer keys, the search panel a click).
  PhiAgentOverlay::HideIfBoundTo(this);
  PhiCreateSpaceWindow::CloseIfBoundTo(this);
  PhiGroupOverview::CloseIfBoundTo(this);
  PhiTabSwitcher::CancelIfBoundTo(this);
  PhiSearchTabsPanel::CloseIfBoundTo(this);
  // Owned components and popups go BEFORE the shell: their teardown calls
  // back into a controller that is still whole, rather than running as a
  // side effect of the shell's DestroyWindow on one that is half gone.
  tab_preview_.reset();
  sidebar_.reset();
  native_pages_->Shutdown();
  spaces_->Shutdown();
  content_slide_.reset();  // Its clock stops; no landing callback runs.
  caption_band_.reset();
  if (floating_sidebar_ && ::IsWindow(floating_sidebar_)) {
    ::DestroyWindow(floating_sidebar_);
  }
  floating_sidebar_ = nullptr;
  // Detach the browser child before tearing down our window, so its teardown
  // is not entangled with ours. Full close plumbing is a later slice. The
  // caps and the status bubble are the content host's and go with it, after
  // the shell window they are children of.
  browsers_->ReleaseContentChild();
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
}

HWND PhiShellWindow::OmniboxShellWindow() const {
  return shell_hwnd_;
}

bool PhiShellWindow::OmniboxShellDestroying() const {
  return destroying_;
}

int PhiShellWindow::OmniboxScaled(int design_units) const {
  return Scaled(design_units);
}

HFONT PhiShellWindow::OmniboxTextFont() const {
  return ui_font_;
}

HFONT PhiShellWindow::OmniboxIconFont() const {
  return chip_icon_font_;
}

PhiPalette PhiShellWindow::OmniboxPalette() const {
  // CurrentPalette, not the host's: incognito overrides to fixed-dark, and
  // a Space switch ramps.
  return CurrentPalette();
}

PhiLayoutMode PhiShellWindow::OmniboxLayoutMode() const {
  return layout_mode_;
}

phi_framework::PhiFramework* PhiShellWindow::OmniboxFramework() const {
  return framework_;
}

BrowserId PhiShellWindow::OmniboxBrowser() const {
  return active_browser_id();
}

void PhiShellWindow::OmniboxFocusContent() {
  FocusActiveBrowser();
}

void PhiShellWindow::OmniboxLeaveReader() {
  LeaveReaderViewForNavigation();
}

// --- PhiCaptionShell: what the caption band asks of its shell -------------

HWND PhiShellWindow::CaptionShellWindow() const {
  return shell_hwnd_;
}

int PhiShellWindow::CaptionScaled(int design_units) const {
  return Scaled(design_units);
}

UINT PhiShellWindow::CaptionDpi() const {
  return dpi_;
}

HFONT PhiShellWindow::CaptionTextFont() {
  EnsureFont();
  return ui_font_;
}

HFONT PhiShellWindow::CaptionGlyphFont() const {
  return caption_font_;
}

PhiPalette PhiShellWindow::CaptionPalette() const {
  return CurrentPalette();
}

PhiCaptionRects PhiShellWindow::CaptionRects() {
  const Layout layout = ComputeLayout();
  PhiCaptionRects rects;
  rects.mode = layout.mode;
  rects.title_bar = layout.title_bar;
  rects.caption_min = layout.caption_min;
  rects.caption_max = layout.caption_max;
  rects.caption_close = layout.caption_close;
  rects.band_divider = layout.band_divider;
  rects.back = layout.back;
  rects.forward = layout.forward;
  rects.reload = layout.reload;
  rects.search_tabs = layout.search_tabs;
  rects.phi_menu = layout.phi_menu;
  rects.band_limit = layout.band_limit;
  rects.tabs_top = layout.tabs_top;
  rects.horizontal_tabs = layout.horizontal_tabs;
  return rects;
}

bool PhiShellWindow::CaptionStripTakesPoint(POINT client_pt) {
  // Comfortable's band IS the tab strip, and the strip is the shell's: the
  // band asks whether anything drawn there is under the pointer.
  if (::PtInRect(&sidebar_->add_tab_button(), client_pt)) {
    return true;
  }
  for (const TabRow& row : sidebar_->tabs()) {
    if (::PtInRect(&row.bounds, client_pt)) {
      return true;
    }
  }
  for (const GroupHeader& chip : sidebar_->group_headers()) {
    if (::PtInRect(&chip.bounds, client_pt)) {
      return true;  // The strip's group chips.
    }
  }
  if (spaces_->ChipAt(client_pt) >= 0) {
    return true;
  }
  for (const ShortcutRow& pin : sidebar_->pinned()) {
    if (::PtInRect(&pin.bounds, client_pt)) {
      return true;
    }
  }
  return false;
}

bool PhiShellWindow::CaptionPhiMenuHovered() const {
  // Its index in the band's hover table.
  return hover_kind_ == HoverKind::kNav && hover_index_ == 12;
}

void PhiShellWindow::CaptionDrawLogo(HDC dc, int x, int y, int size) {
  // The logo is loaded AT the size it draws (the multi-size ico has exact
  // entries per DPI), so no GDI rescale blurs it.
  const std::vector<unsigned char>& logo_bgra = PhiLogoBgra256();
  constexpr int kLogoNative = 256;
  if (logo_bgra.empty()) {
    // Fallback: the nearest ICO entry, held per window at the size this DPI
    // draws — a process static loaded at the first window's DPI drew blurred
    // (and leaked) on every other display.
    if (logo_icon_ && logo_icon_size_ != size) {
      ::DestroyIcon(logo_icon_);
      logo_icon_ = nullptr;
    }
    if (!logo_icon_) {
      logo_icon_ = PhiAppIcon(size);
      logo_icon_size_ = size;
    }
  }
  RECT logo_rc = {x, y, x + size, y + size};
  const bool drew = !logo_bgra.empty() &&
                    PhiDrawBgra(dc, logo_rc, logo_bgra.data(), kLogoNative,
                                kLogoNative);
  if (!drew && logo_icon_) {
    ::DrawIconEx(dc, x, y, logo_icon_, size, size, 0, nullptr, DI_NORMAL);
  }
}

void PhiShellWindow::CaptionClearShellHover() {
  ClearHover();
}

bool PhiShellWindow::CaptionContentFullscreen() const {
  return browsers_->content_fullscreen();
}

PhiMenuBarShell* PhiShellWindow::CaptionMenuServices() {
  return this;
}

// --- PhiMenuBarShell: what the menu bar asks of its shell -----------------
// This interface RETIRED `friend class PhiMenuBar` (lane 1 §2.1 item 3): the
// bar used to read seventeen private members by name, which is why the shell
// had to publish const reference views onto its own components' state just
// to keep the bar compiling. Nothing below hands out a reference into a
// cache the bar could outlive.

HWND PhiShellWindow::MenuShellWindow() const {
  return shell_hwnd_;
}

SlotId PhiShellWindow::MenuSlot() const {
  return slot_;
}

bool PhiShellWindow::MenuIncognito() const {
  return incognito_;
}

SidebarHost* PhiShellWindow::MenuSidebarHost() const {
  return services_.sidebar;
}

SpacesHost* PhiShellWindow::MenuSpacesHost() const {
  return services_.spaces;
}

ChromePrefsHost* PhiShellWindow::MenuPrefsHost() const {
  return services_.prefs;
}

WindowHost* PhiShellWindow::MenuWindowHost() const {
  return services_.window;
}

phi_framework::PhiFramework* PhiShellWindow::MenuFramework() const {
  return framework_;
}

const PhiWindowModel* PhiShellWindow::MenuModel() const {
  return model_;
}

BrowserId PhiShellWindow::MenuActiveBrowser() const {
  return active_browser_id();
}

TabId PhiShellWindow::MenuActiveTab() const {
  return active_tab_id();
}

std::string PhiShellWindow::MenuActiveSpace() const {
  return active_space();
}

PhiPalette PhiShellWindow::MenuPalette() const {
  return CurrentPalette();
}

int PhiShellWindow::MenuScaled(int design_units) const {
  return Scaled(design_units);
}

HWND PhiShellWindow::MenuAddressField() const {
  return omnibox_->field();
}

void PhiShellWindow::MenuRefreshNavState() {
  // Re-reads nav from the window model and re-syncs the field. NOT a row
  // rebuild: the rows a menu shows are always the finished ones (phase 2
  // made rebuilds posted, and a menu opening is not a reason to rebuild).
  RebuildNavState();
}

PhiMenuBarNavState PhiShellWindow::MenuNavState() const {
  PhiMenuBarNavState nav;
  nav.url = omnibox_->nav_url();
  nav.can_go_back = omnibox_->can_go_back();
  nav.can_go_forward = omnibox_->can_go_forward();
  nav.loading = omnibox_->nav_loading();
  return nav;
}

std::vector<PhiMenuBarTab> PhiShellWindow::MenuTabRows() const {
  // A SNAPSHOT of the finished rows. The bar holds it for the length of one
  // menu build; a rebuild under an open menu cannot reach into it.
  std::vector<PhiMenuBarTab> rows;
  rows.reserve(sidebar_->tabs().size());
  for (const TabRow& row : sidebar_->tabs()) {
    rows.push_back({row.id, row.title, row.active});
  }
  return rows;
}

bool PhiShellWindow::MenuReaderActive() const {
  return ActiveTabShowsReader();
}

bool PhiShellWindow::MenuReaderOfferable() const {
  return ActiveTabCanUseReader();
}

bool PhiShellWindow::MenuLeaveReaderForNavigation() {
  return LeaveReaderViewForNavigation();
}

std::vector<PhiAgentPipView> PhiShellWindow::MenuAgentSpaces() const {
  // This window's own list (PhiAgentSpaceManager::Presents); an agent
  // window's menus never switch agent Spaces — its strip does.
  if (!services_.agent || agent_ || incognito_) {
    return {};
  }
  return services_.agent->AgentSpaces(slot_.value);
}

void PhiShellWindow::MenuSwitchSpace(const std::string& space_id) {
  SwitchToSpace(space_id);
}

void PhiShellWindow::MenuNewTab() {
  StartNewTab();  // Honors the "New tab behavior" setting.
}

void PhiShellWindow::MenuToggleSidebar() {
  ToggleSidebar();
}

bool PhiShellWindow::MenuAgentHoldsInput() const {
  return AgentHoldsInput();
}

void PhiShellWindow::MenuToggleReader(bool from_accelerator) {
  ToggleReaderView(from_accelerator ? "shortcut" : "view_menu");
}

void PhiShellWindow::MenuToggleDownloads() {
  ToggleDownloadsPopover();
}

void PhiShellWindow::MenuToggleSearchTabs() {
  PhiSearchTabsPanel::Toggle(this);
}

void PhiShellWindow::ShowHeldShell() {
  if (!shell_hidden_ || !shell_hwnd_) {
    return;
  }
  shell_hidden_ = false;
  ::ShowWindow(shell_hwnd_, SW_SHOW);
  ::SetForegroundWindow(shell_hwnd_);
  LayoutContents();
  browsers_->FocusActiveBrowser();
}

void PhiShellWindow::HideShell() {
  if (shell_hidden_ || !shell_hwnd_ || agent_) {
    return;
  }
  shell_hidden_ = true;
  ::ShowWindow(shell_hwnd_, SW_HIDE);
}

void PhiShellWindow::MenuOpenFeedback() {
  OpenFeedback();
}

void PhiShellWindow::MenuCreateSpace() {
  ShowCreateSpaceForm();  // The sidebar form, or at once without one.
}

void PhiShellWindow::MenuEditSpaceTheme() {
  // The Mac's .windowTop placement: the menu bar has no trigger point to
  // hang the panel off, so it drops under the caption, centred on the window
  // (the panel centres itself in an anchor wider than it).
  if (!services_.spaces || active_space().empty() || !shell_hwnd_) {
    return;
  }
  RECT shell = {};
  ::GetWindowRect(shell_hwnd_, &shell);
  const RECT anchor = {shell.left, shell.top, shell.right, shell.top};
  PhiShowSpaceThemePanel(this, anchor, active_space());
}

// --- PhiBrowserHostShell: what the content host asks of its shell ---------

HWND PhiShellWindow::BrowserHostShellWindow() const {
  return shell_hwnd_;
}

int PhiShellWindow::BrowserHostScaled(int design_units) const {
  return Scaled(design_units);
}

PhiPalette PhiShellWindow::BrowserHostPalette() const {
  return CurrentPalette();
}

RECT PhiShellWindow::BrowserHostContentRect() {
  return ComputeLayout().content;
}

RECT PhiShellWindow::BrowserHostClientRect() {
  return ComputeLayout().client;
}

HFONT PhiShellWindow::BrowserHostStatusFont() const {
  return small_font_;
}

phi_framework::PhiFramework* PhiShellWindow::BrowserHostFramework()
    const {
  return framework_;
}

bool PhiShellWindow::BrowserHostIsAgent() const {
  return agent_;
}

void PhiShellWindow::BrowserHostRelayout() {
  LayoutContents();
}

void PhiShellWindow::BrowserHostRefreshTabs() {
  RefreshTabs();
}

void PhiShellWindow::BrowserHostScheduleRefresh() {
  ScheduleRefresh();
}

void PhiShellWindow::BrowserHostBeginSpaceSwap(
    const std::string& space_id, BrowserId entering) {
  if (layout_mode_ == PhiLayoutMode::kComfortable) {
    // No band to push in: the whole client area slides (phi_content_slide.h).
    // The content host defers the page exactly as for the band slide
    // (BrowserHostSwapRunning): the entering child parks until the slide
    // lands, the leaving one stays up until the slide starts.
    BeginContentSlide(space_id, entering);
    return;
  }
  spaces_->BeginSwap(space_id);
}

bool PhiShellWindow::BrowserHostSwapRunning() const {
  return spaces_->swap_running() || content_slide_->running();
}

void PhiShellWindow::BrowserHostNoteSwapChildren(HWND reveal,
                                                      HWND conceal) {
  swap_reveal_ = reveal;
  swap_conceal_ = conceal;
}

void PhiShellWindow::BrowserHostEndSpacePreview() {
  if (spaces_->preview_awaits_switch()) {
    spaces_->EndPreview();
  }
}

void PhiShellWindow::BrowserHostNoteSpaceWindow(
    const std::string& space_id,
    BrowserId browser) {
  if (services_.spaces) {
    services_.spaces->NoteSpaceWindow(space_id, browser.value);
  }
}

void PhiShellWindow::BrowserHostNoteActiveSpace(
    const std::string& space_id) {
  if (services_.spaces && !agent_ && !PhiIsAgentSpaceId(space_id)) {
    services_.spaces->NoteSlotActiveSpace(space_id);
  }
}

void PhiShellWindow::BrowserHostAbandonTyping() {
  omnibox_->ClosePopup(/*stop_query=*/true);
}

void PhiShellWindow::BrowserHostRetintChrome() {
  omnibox_->Repaint();
}

void PhiShellWindow::BrowserHostFocusAddressField() {
  omnibox_->FocusFieldIfVisible();
}

bool PhiShellWindow::BrowserHostFocusNativePage() {
  if (!native_pages_->ntp_wants_focus()) {
    return false;
  }
  native_pages_->FocusNtpField();
  return true;
}

bool PhiShellWindow::BrowserHostDeferSpaceClose(
    const std::string& space_id) {
  if (PhiModalDepth() == 0) {
    return false;
  }
  deferred_space_closes_.push_back(space_id);
  PostDeferredClose();
  return true;
}

// --- The content area, forwarded to the component -------------------------

void PhiShellWindow::AttachSpaceBrowser(const std::string& space_id,
                                             BrowserId browser,
                                             HWND browser_hwnd) {
  browsers_->AttachSpaceBrowser(space_id, browser, browser_hwnd);
}

bool PhiShellWindow::ShowSpace(const std::string& space_id) {
  if (HostsAgentSpace() && !PhiIsAgentSpaceId(space_id) &&
      active_space() == hosted_agent_space_) {
    // Any road out of a hosted agent Space — a pip, Ctrl+N, the Spaces
    // menu, a verb — is the end of the watch: the owner runs the swap
    // back (LeaveHostedAgentSpace) and hands the browser home after it.
    if (services_.agent) {
      services_.agent->RequestLeaveWatchedAgentSpace(space_id);
      return true;
    }
  }
  return browsers_->ShowSpace(space_id);
}

bool PhiShellWindow::HasSpace(const std::string& space_id) const {
  return browsers_->spaces().Has(space_id);
}

void PhiShellWindow::SwitchToSpace(const std::string& space_id) {
  if (space_id.empty() || space_id == active_space()) {
    return;
  }
  if (PhiIsAgentSpaceId(space_id)) {
    // An agent pip's click: the watch starts (or, hosted here already,
    // the hosted one hands over to it — one swap). The pip carries the
    // task the manager keys on.
    if (!services_.agent) {
      return;
    }
    for (const PhiAgentPipView& pip :
         services_.agent->AgentSpaces(agent_ ? 0 : slot_.value)) {
      if (pip.space_id == space_id) {
        services_.agent->RequestWatchAgentSpace(pip.task_id, slot_.value);
        ScheduleRefresh();  // The pip's watched state is the owner's.
        return;
      }
    }
    return;
  }
  if (agent_ || HostsAgentSpace()) {
    // A user Space picked while the agent Space is up: the Mac's
    // slot.activate — leave the agent Space and show that Space in the
    // window that was watching. The agent's hidden browser never switches
    // Space itself.
    if (services_.agent) {
      services_.agent->RequestLeaveWatchedAgentSpace(space_id);
    }
    return;
  }
  // Below here the user is switching Spaces (a pip, the menu, Ctrl+N): the
  // Mac's space_switched, counted whichever way the browser arrives.
  if (services_.window) {
    services_.window->NoteUserSpaceSwitched();
  }
  if (HasSpace(space_id)) {
    ShowSpace(space_id);
  } else if (services_.spaces) {
    // Not yet in this slot: the owner creates its browser in the Space's
    // profile and attaches it here.
    services_.spaces->RequestSwitchSpace(slot_.value, space_id);
  }
}

void PhiShellWindow::CloseSpaceBrowser(const std::string& space_id) {
  browsers_->CloseSpaceBrowser(space_id);
}

bool PhiShellWindow::ForgetBrowser(BrowserId browser) {
  return browsers_->ForgetBrowser(browser);
}

void PhiShellWindow::FocusActiveBrowser() {
  browsers_->FocusActiveBrowser();
}

void PhiShellWindow::RaiseCornerCaps() {
  browsers_->RequestRaiseCaps();
}

void PhiShellWindow::OnTargetURLChanged(BrowserId browser_id,
                                             const std::wstring& url) {
  browsers_->OnTargetURLChanged(browser_id, url);
}

void PhiShellWindow::OnPlaceholderModeChanged(BrowserId browser_id,
                                                   bool entered) {
  browsers_->OnPlaceholderModeChanged(browser_id, entered);
}

void PhiShellWindow::OnContentFullscreenChanged(BrowserId browser_id,
                                                     bool fullscreen) {
  browsers_->OnContentFullscreenChanged(browser_id, fullscreen);
}

void PhiShellWindow::OnContentBoundsChanged(BrowserId browser_id) {
  if (browser_id == active_browser_id()) {
    native_pages_->Reposition();
  }
}

void PhiShellWindow::ActivatePaneTab(int tab_id) {
  const BrowserId browser = active_browser_id();
  if (!framework_ || !browser.valid() || tab_id <= 0 ||
      active_tab_id().value == tab_id) {
    return;
  }
  framework_->ActivateTabById(browser.value, tab_id);
}

PhiOmniboxCapsule PhiShellWindow::OmniboxCapsule() {
  const Layout layout = ComputeLayout();
  auto to_pure = [](const RECT& r) {
    return SidebarRect{r.left, r.top, r.right, r.bottom};
  };
  PhiOmniboxCapsule capsule;
  capsule.field = to_pure(layout.url_field);
  capsule.page_icon = to_pure(layout.page_icon);
  capsule.field_ext_strip = to_pure(layout.field_ext_strip);
  capsule.reader = to_pure(layout.reader);
  capsule.copy_link = to_pure(layout.copy_link);
  capsule.extensions = to_pure(layout.extensions);
  return capsule;
}

bool PhiShellWindow::OmniboxBrowserPlaceholding() const {
  return browsers_->ActiveBrowserPlaceholding();
}

bool PhiShellWindow::OmniboxNewTabFocusesField() const {
  return services_.prefs && services_.prefs->NewTabOpensOmnibox();
}

HWND PhiShellWindow::OmniboxNativePageField() const {
  return native_pages_->ntp_field();
}

bool PhiShellWindow::OmniboxNativePageBox(RECT* out) const {
  return native_pages_->NtpBoxScreenRect(out);
}

void PhiShellWindow::OmniboxNativePageCommitting() {
  // omniBoxDidClear: the native page blanks its controls now, so nothing of
  // the new-tab page lingers over the arriving one; the rebuild that sees
  // the new URL unmounts it.
  const TabId committing = active_tab_id();
  if (native_pages_->HideNtpControls()) {
    ntp_commit_tab_id_ = committing;
  }
}

// --- PhiNativePagesShell ----------------------------------------------------

HWND PhiShellWindow::NativePagesShellWindow() const {
  return shell_hwnd_;
}

PhiShellWindow* PhiShellWindow::NativePagesOwner() {
  return this;
}

bool PhiShellWindow::NativePagesIncognito() const {
  return incognito_;
}

bool PhiShellWindow::NativePagesIsAgent() const {
  return agent_;
}

bool PhiShellWindow::NativePagesContentFullscreen() const {
  return browsers_->content_fullscreen();
}

bool PhiShellWindow::NativePagesPlaceholding() const {
  return browsers_->ActiveBrowserPlaceholding();
}

bool PhiShellWindow::NativePagesSwapPending() const {
  return swap_reveal_ != nullptr;
}

const std::string& PhiShellWindow::NativePagesActiveUrl() const {
  return active_tab_url();
}

int PhiShellWindow::NativePagesActiveTabId() const {
  const TabId active = active_tab_id();
  return active.valid() ? active.value : 0;
}

std::vector<NativePane> PhiShellWindow::NativePagesSplitPanes() const {
  std::vector<NativePane> panes;
  const BrowserId browser = active_browser_id();
  const TabId active = active_tab_id();
  const SplitSnapshot* split = model_ && browser.valid() && active.valid()
                                   ? model_->SplitOfTab(browser, active)
                                   : nullptr;
  if (!split || !framework_ || !shell_hwnd_) {
    return panes;
  }
  for (const TabId tab : {split->primary, split->secondary}) {
    NativePane pane;
    pane.tab_id = tab.value;
    if (const TabSnapshot* snapshot = model_->FindTab(browser, tab)) {
      pane.url = snapshot->url;
    }
    // Where Chromium laid the pane out, in SCREEN pixels; mapped into the
    // shell, whose child the page is. Empty until the layout has run —
    // the framework's OnContentBoundsChanged repositions then.
    int left = 0, top = 0, right = 0, bottom = 0;
    if (framework_->TabContentBoundsById(browser.value, tab.value, &left,
                                         &top, &right, &bottom)) {
      RECT rect = {left, top, right, bottom};
      ::MapWindowPoints(nullptr, shell_hwnd_, reinterpret_cast<POINT*>(&rect),
                        2);
      pane.rect = rect;
    }
    panes.push_back(std::move(pane));
  }
  return panes;
}

void PhiShellWindow::NativePagesRaiseCaps() {
  browsers_->RequestRaiseCaps();
}

void PhiShellWindow::NativePagesFocusContent() {
  FocusActiveBrowser();
}

HWND PhiShellWindow::NativePagesContentWindow() const {
  return browsers_->content_hwnd();
}

bool PhiShellWindow::NativePagesCommitPending() const {
  return active_tab_id() == ntp_commit_tab_id_ && omnibox_->nav_loading();
}

// --- PhiSidebarShell: what the sidebar asks of its shell --------------------

PhiSidebarRects PhiShellWindow::BandRects(const Layout& layout) const {
  PhiSidebarRects rects;
  rects.sidebar = layout.sidebar;
  rects.title_bar = layout.title_bar;
  rects.content = layout.content;
  rects.favorites = layout.favorites;
  rects.bookmarks = layout.bookmarks;
  rects.bookmark_separator = layout.bookmark_separator;
  rects.new_tab_row = layout.new_tab_row;
  rects.bottom_bar = layout.bottom_bar;
  rects.spaces_strip = layout.spaces_strip;
  rects.search_tabs = layout.search_tabs;
  rects.tabs_left = layout.tabs_left;
  rects.tabs_top = layout.tabs_top;
  rects.tabs_limit = layout.tabs_limit;
  rects.horizontal_tabs = layout.horizontal_tabs;
  rects.spaces_switch = layout.spaces_switch;
  return rects;
}

HWND PhiShellWindow::SidebarShellWindow() const {
  return shell_hwnd_;
}

PhiShellWindow* PhiShellWindow::SidebarOwner() {
  return this;
}

int PhiShellWindow::SidebarScaled(int design_units) const {
  return Scaled(design_units);
}

UINT PhiShellWindow::SidebarDpi() const {
  return dpi_;
}

int PhiShellWindow::SidebarWidthDip() const {
  return sidebar_dip_;
}

PhiSidebarRects PhiShellWindow::SidebarRects() {
  return BandRects(ComputeLayout());
}

void PhiShellWindow::SidebarSetRowCounts(int pinned_rows,
                                              int bookmark_rows) {
  pinned_row_count_ = pinned_rows;
  bookmark_row_count_ = bookmark_rows;
}

void PhiShellWindow::SidebarSetDropZones(bool pinned, bool bookmark) {
  pinned_drop_zone_ = pinned;
  bookmark_drop_zone_ = bookmark;
}

void PhiShellWindow::SidebarRefreshLayoutInputs() {
  RefreshLayoutInputs();
}

PhiPalette PhiShellWindow::SidebarPalette() const {
  return CurrentPalette();
}

std::string PhiShellWindow::SidebarActiveSpace() const {
  return active_space();
}

BrowserId PhiShellWindow::SidebarActiveBrowser() const {
  return active_browser_id();
}

bool PhiShellWindow::SidebarPullSnapshot(
    BrowserId browser,
    phi_framework::WindowSnapshot* out) const {
  return PullSnapshot(browser, out);
}

bool PhiShellWindow::SidebarSplitIsStacked(TabId tab_id) const {
  return SplitIsStacked(tab_id);
}

HFONT PhiShellWindow::SidebarGlyphFont() const {
  return caption_font_;
}

HFONT PhiShellWindow::SidebarSemiboldFont() const {
  return ui_semibold_font_;
}

HFONT PhiShellWindow::SidebarTextFont() const {
  return ui_font_;
}

PhiChromeHover PhiShellWindow::SidebarHoverKind() const {
  return hover_kind_;
}

int PhiShellWindow::SidebarHoverIndex() const {
  return hover_index_;
}

PhiSpacesStrip* PhiShellWindow::SidebarSpacesStrip() {
  return spaces_.get();
}

void PhiShellWindow::SidebarScheduleRefresh() {
  ScheduleRefresh();
}

void PhiShellWindow::SidebarSyncNativePages() {
  native_pages_->SyncIfRouteChanged();
}

PhiDragBitmap PhiShellWindow::SidebarCaptureChrome(
    const RECT& client_rect) {
  return CaptureChromeRect(client_rect);
}

HWND PhiShellWindow::SidebarContentWindow() const {
  return browsers_->content_hwnd();
}

void PhiShellWindow::SidebarStartNewTab() {
  StartNewTab();
}

// The rows, as assistive technology reads them. The list is THE data source
// the UIA provider (phi_uia.h) walks — one fragment per hit, named, typed
// and invokable through the same messages a mouse click sends.
void PhiShellWindow::SidebarPublishHits(std::vector<PhiHit> hits) {
  hits_ = std::move(hits);
  ++hits_version_;
  // Cheap unless a provider was actually handed out: it tells a listening
  // screen reader the tab list changed shape rather than leaving it on a
  // stale tree.
  PhiUiaHitsChanged(this);
}

HWND PhiShellWindow::hwnd() const {
  return shell_hwnd_;
}

const std::vector<PhiHit>& PhiShellWindow::hits() const {
  return hits_;
}

uint32_t PhiShellWindow::hits_version() const {
  return hits_version_;
}

int PhiShellWindow::HitTest(POINT client_pt) const {
  return PhiHitAt(hits_, client_pt.x, client_pt.y);
}

bool PhiShellWindow::InvokeHit(int index) {
  // The mouse path, not a copy of it — the same clicks the sidebar handles.
  int x = 0;
  int y = 0;
  if (!shell_hwnd_ || !PhiUiaInvokePoint(hits_, index, &x, &y)) {
    return false;
  }
  // LOCALS ONLY below: a click can close this Space and destroy the shell.
  const HWND hwnd = shell_hwnd_;
  const LPARAM point =
      MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
  ::SendMessageW(hwnd, WM_LBUTTONDOWN, 0, point);
  if (::IsWindow(hwnd)) {
    ::SendMessageW(hwnd, WM_LBUTTONUP, 0, point);
  }
  return true;
}

// --- PhiSpacesStripShell ----------------------------------------------------

PhiSpacesStripRects PhiShellWindow::StripRects(
    const Layout& layout) const {
  PhiSpacesStripRects rects;
  rects.client = layout.client;
  rects.strip = layout.spaces_strip;
  rects.strip_repaint =
      layout.horizontal_tabs ? layout.title_bar : layout.spaces_strip;
  rects.sidebar = layout.sidebar;
  rects.add_space = layout.add_space;
  rects.chip_size = layout.chip_size;
  rects.chips_left = layout.chips_left;
  rects.chips_top = layout.chips_top;
  rects.chips_limit = layout.chips_limit;
  rects.horizontal_tabs = layout.horizontal_tabs;
  rects.spaces_switch = layout.spaces_switch;
  return rects;
}

HWND PhiShellWindow::SpacesShellWindow() const {
  return shell_hwnd_;
}

PhiShellWindow* PhiShellWindow::SpacesOwner() {
  return this;
}

SlotId PhiShellWindow::SpacesSlot() const {
  return slot_;
}

bool PhiShellWindow::SpacesIncognito() const {
  return incognito_;
}

bool PhiShellWindow::SpacesIsAgent() const {
  return agent_;
}

int PhiShellWindow::SpacesScaled(int design_units) const {
  return Scaled(design_units);
}

PhiSpacesStripRects PhiShellWindow::SpacesRects() {
  return StripRects(ComputeLayout());
}

const std::string& PhiShellWindow::SpacesActiveSpace() const {
  return active_space();
}

bool PhiShellWindow::SpacesHasSpace(const std::string& space_id) const {
  return HasSpace(space_id);
}

PhiPalette PhiShellWindow::SpacesPalette() const {
  return CurrentPalette();
}

bool PhiShellWindow::SpacesDragActive() const {
  return sidebar_ && sidebar_->drag_active();
}

int PhiShellWindow::SpacesHoveredChip() const {
  return hover_kind_ == HoverKind::kChip ? hover_index_ : -1;
}

bool PhiShellWindow::SpacesAddSlotHovered() const {
  return hover_kind_ == HoverKind::kAddSpace;
}

RECT PhiShellWindow::SpacesAddSlotDirtyRect() {
  return HoverRect(HoverKind::kAddSpace, 0);
}

void PhiShellWindow::SpacesForgetHover() {
  hover_kind_ = HoverKind::kNone;
  hover_index_ = -1;
}

void PhiShellWindow::SpacesRehoverAtCursor() {
  POINT cursor = {};
  if (shell_hwnd_ && ::GetCursorPos(&cursor)) {
    ::ScreenToClient(shell_hwnd_, &cursor);
    OnChromeMouseMove(cursor);
  }
}

void PhiShellWindow::SpacesScheduleRefresh() {
  ScheduleRefresh();
}

void PhiShellWindow::SpacesApplyPalette() {
  ApplyPalette();
}

void PhiShellWindow::SpacesRefreshTinted() {
  native_pages_->Refresh();
  browsers_->RefreshCornerCaps();
}

void PhiShellWindow::SpacesRepaintOmnibox() {
  omnibox_->Repaint();
}

void PhiShellWindow::SpacesFocusContent() {
  FocusActiveBrowser();
}

void PhiShellWindow::SpacesSetCoveredContentSuppressed(bool suppressed) {
  // The EDIT has no WS_CLIPSIBLINGS, so it paints straight through a panel
  // raised over it; only hiding it keeps the address text off the form.
  if (suppressed) {
    omnibox_->ClosePopup(/*stop_query=*/true);
  }
  omnibox_->SetFieldVisible(!OmniboxFieldHidden());
}

bool PhiShellWindow::OmniboxFieldHidden() const {
  // While the comfortable Space slide runs the EDIT is down and the frame
  // paints the address itself (include_child_controls): a real child window
  // moves a frame apart from the parent's paint and would ghost.
  return browsers_->content_fullscreen() ||
         (sidebar_collapsed_ && layout_mode_ == PhiLayoutMode::kPerformance) ||
         spaces_->creating() || (content_slide_ && content_slide_->running());
}

void PhiShellWindow::SpacesRevealSwappedContent() {
  RevealSwappedContent();
}

void PhiShellWindow::SpacesSwapBandModel() {
  sidebar_->SwapBandModel();
}

void PhiShellWindow::SpacesDropLeavingBand() {
  sidebar_->DropLeavingBand();
}

HFONT PhiShellWindow::SpacesUiFont() const {
  return ui_font_;
}

HFONT PhiShellWindow::SpacesSmallFont() const {
  return small_font_;
}

HFONT PhiShellWindow::SpacesEmojiFont() const {
  return emoji_font_;
}

HFONT PhiShellWindow::SpacesChipIconFont() const {
  return chip_icon_font_;
}

HFONT PhiShellWindow::SpacesPipIconFont() const {
  return pip_icon_font_;
}

HFONT PhiShellWindow::SpacesPipEmojiFont() const {
  return pip_emoji_font_;
}

HFONT PhiShellWindow::SpacesPipTextFont() const {
  return pip_text_font_;
}

bool PhiShellWindow::SpacesSidebarCanHostForm(const RECT& form) const {
  // CreateSpacePanel.requestCreation: the vertical layouts host the form
  // inline in the docked sidebar. The horizontal layout, a collapsed
  // sidebar (the floating panel does not host it here) and windows without
  // a Spaces strip create at once instead.
  return !incognito_ && !agent_ && !floating_layout_ && !sidebar_collapsed_ &&
         layout_mode_ != PhiLayoutMode::kComfortable &&
         form.right - form.left > Scaled(120) &&
         form.bottom - form.top > Scaled(200);
}

HWND PhiShellWindow::SpacesFloatingPanel(int* offset_y) const {
  if (floating_layout_ && floating_sidebar_ && ::IsWindow(floating_sidebar_)) {
    if (offset_y) {
      *offset_y = floating_offset_y_;
    }
    return floating_sidebar_;
  }
  return nullptr;
}

void PhiShellWindow::AttachOmniboxField(HWND edit) {
  omnibox_->AttachField(edit);
}

void PhiShellWindow::OnOmniboxFieldChanged(HWND field) {
  omnibox_->OnFieldChanged(field);
}

void PhiShellWindow::OnOmniboxResults(BrowserId browser_id,
                                           const std::string& json) {
  omnibox_->OnResults(browser_id, json);
}

void PhiShellWindow::StartNewTab() {
  omnibox_->StartNewTab();
}

void PhiShellWindow::RebuildNavState() {
  const BrowserId browser = active_browser_id();
  const phi_framework::WindowSnapshot* snapshot =
      model_ && browser.valid() ? model_->Get(browser) : nullptr;
  omnibox_->RebuildNavState(snapshot ? &snapshot->nav : nullptr);
}

void PhiShellWindow::OpenFeedback() {
  if (!services_.window) {
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
  for (const TabRow& row : sidebar_->tabs()) {
    if (row.active) {
      context.url = base::WideToUTF8(row.url);
      context.title = base::WideToUTF8(row.title);
      break;
    }
  }
  if (context.url.empty()) {
    context.url = active_tab_url();
  }
  services_.window->RequestFeedback(slot_.value, context);
}

RECT PhiShellWindow::ContentRect() {
  return ComputeLayout().content;
}

void PhiShellWindow::ToggleSidebar() {
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

void PhiShellWindow::ToggleDownloadsPopover() {
  // The menu / Ctrl+J entry to the same popover the sidebar button toggles.
  // The popover's own opening grace period absorbs the menu teardown's
  // activation churn, so this can run inline.
  const BrowserId browser = active_browser_id();
  if (!services_.spaces || !framework_ || !shell_hwnd_ || !browser.valid()) {
    return;
  }
  RECT anchor = ComputeLayout().downloads;
  ::MapWindowPoints(shell_hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor),
                    2);
  PhiDownloadsPopover::Toggle(shell_hwnd_, anchor,
                              services_.spaces->PaletteForSlot(slot_.value),
                              framework_, browser);
}

void PhiShellWindow::LayoutContents() {
  if (destroying_) {
    return;  // The shell's own DestroyWindow sends WM_SIZE on the way down.
  }
  native_pages_->Reposition();
  // Reader and peek panels are hosted OVER the content card, in windows the
  // coordinator owns rather than this controller — they have to follow it.
  if (services_.sidebar) {
    services_.sidebar->NoteContentLayoutChanged(slot_.value);
  }
  if (!shell_hwnd_) {
    return;
  }
  RefreshLayoutInputs();  // The owner pushes width / bookmark-bar prefs here.
  ScheduleRefresh();      // Row bounds derive from the client size.
  ::InvalidateRect(shell_hwnd_, nullptr, FALSE);  // Every band moved.
  browsers_->SetStatusUrl(std::wstring());  // Anchored to the OLD rect.
  omnibox_->ClosePopup(/*stop_query=*/false);  // Anchored to the OLD rect.
  const Layout layout = ComputeLayout();
  omnibox_->Position();
  if (spaces_->creating()) {
    // The form lives only in a docked vertical sidebar: a layout switch or
    // a collapse ends it (the Mac re-routes to its standalone window).
    if (layout_mode_ == PhiLayoutMode::kComfortable || sidebar_collapsed_) {
      spaces_->CancelCreateForm();
    } else {
      spaces_->RepositionForm();
    }
  }
  omnibox_->SetFieldVisible(!OmniboxFieldHidden());
  PlaceContentChildren(layout);
}

void PhiShellWindow::PlaceContentChildren(const Layout& layout) {
  PhiContentLayout content;
  content.content = layout.content;
  content.client = layout.client;
  // Top caps only where the content's top edge meets the tint (performance);
  // under a top bar the card's top corners belong to the bar band.
  content.top_caps = layout.mode == PhiLayoutMode::kPerformance;
  content.corner_radius = Scaled(kContentRadius);
  // Either Space switch parks the entering child until it lands.
  content.parked = BrowserHostSwapRunning() ? swap_reveal_ : nullptr;
  browsers_->PositionContentChild(content);

  // Watch-mode overlay (fog + control pill) tracks the content rect. It goes
  // between the two: the caps must end up above it.
  PhiAgentOverlay::Reposition();

  browsers_->PositionCornerCaps(content);
}

// --- The comfortable layout's Space switch (phi_content_slide.h) ------------

void PhiShellWindow::BeginContentSlide(const std::string& space_id,
                                       BrowserId entering) {
  if (!shell_hwnd_ || destroying_ || incognito_ || agent_ || shell_hidden_ ||
      space_id == active_space()) {
    return;
  }
  if (content_slide_->running()) {
    // A switch on top of a switch: land the first, and let the screen show
    // it before the composite is read off — the desktop composes on its own
    // clock.
    CancelContentSlide();
    ::RedrawWindow(shell_hwnd_, nullptr, nullptr,
                   RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    ::DwmFlush();
  }
  RECT client = {};
  ::GetClientRect(shell_hwnd_, &client);
  HBITMAP leaving = CaptureLeavingComposite();
  if (!leaving) {
    return;  // Nothing to slide out: the switch shows at once.
  }
  // The entering page comes from the framework once the switch under this
  // has run (StartSlideCapture, on the posted turn): the Space's browser
  // — the one being attached, or the one the table holds — and that
  // browser's active tab.
  slide_entering_browser_ = entering;
  slide_entering_tab_ = TabId();
  if (!slide_entering_browser_.valid() && browsers_->spaces().Has(space_id)) {
    slide_entering_browser_ = browsers_->spaces().ForSpace(space_id);
  }
  if (const phi_framework::TabSnapshot* tab =
          model_ && slide_entering_browser_.valid()
              ? model_->ActiveTab(slide_entering_browser_)
              : nullptr) {
    slide_entering_tab_ = tab->id;
  }
  // A fresh id per switch: a capture from a switch already landed or cut
  // short must not fill in this one's page.
  static int next_request = 0;
  slide_capture_request_ = ++next_request;
  slide_capture_tries_ = 0;
  content_slide_->Begin(leaving, client.right, client.bottom,
                        spaces_->SlideForward(space_id));
  if (!content_slide_->running()) {
    return;
  }
  // The EDIT goes down for the slide (OmniboxFieldHidden); the content
  // host's relayout right after this would show it again otherwise.
  omnibox_->SetFieldVisible(!OmniboxFieldHidden());
  ::PostMessageW(shell_hwnd_, kMsgPhiSlideCapture, 0, 0);
}

HBITMAP PhiShellWindow::CaptureLeavingComposite() {
  if (!shell_hwnd_ || ::IsIconic(shell_hwnd_) ||
      !::IsWindowVisible(shell_hwnd_)) {
    return nullptr;
  }
  RECT client = {};
  ::GetClientRect(shell_hwnd_, &client);
  if (client.right <= 0 || client.bottom <= 0) {
    return nullptr;
  }
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HBITMAP bitmap = ::CreateCompatibleBitmap(screen, client.right, client.bottom);
  if (!mem || !bitmap) {
    if (bitmap) {
      ::DeleteObject(bitmap);
    }
    if (mem) {
      ::DeleteDC(mem);
    }
    ::ReleaseDC(nullptr, screen);
    return nullptr;
  }
  HGDIOBJ old = ::SelectObject(mem, bitmap);
  HBRUSH fill = ::CreateSolidBrush(BackgroundFill());
  ::FillRect(mem, &client, fill);
  ::DeleteObject(fill);
  const Layout layout = ComputeLayout();
  {
    // The same frame WM_PAINT composes, minus the caption buttons: they
    // stay put over the slide and are painted once, unslid, by the frame.
    // The address text is painted in (the EDIT itself is not captured).
    base::AutoReset<bool> suppress(&caption_buttons_suppressed_, true);
    PaintChrome(mem, layout, /*include_child_controls=*/true);
  }
  // The page — and whatever sits over it (a native page, the watch fog):
  // the screen has it right now.
  const int w = layout.content.right - layout.content.left;
  const int h = layout.content.bottom - layout.content.top;
  if (w > 0 && h > 0) {
    POINT origin = {layout.content.left, layout.content.top};
    ::ClientToScreen(shell_hwnd_, &origin);
    ::BitBlt(mem, layout.content.left, layout.content.top, w, h, screen,
             origin.x, origin.y, SRCCOPY);
  }
  ::GdiFlush();
  ::SelectObject(mem, old);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  return bitmap;
}

void PhiShellWindow::StartSlideCapture() {
  if (!content_slide_->holding()) {
    return;
  }
  const bool asked =
      framework_ && slide_entering_browser_.valid() &&
      slide_entering_tab_.valid() &&
      framework_->StartTabSurfaceCapture(slide_entering_browser_.value,
                                         slide_entering_tab_.value,
                                         slide_capture_request_);
  if (!asked) {
    ReleaseContentSlide();  // Nothing to fetch: the page area slides blank.
    return;
  }
  // The view was shown a turn ago and may hold no frame yet: ask again
  // every tick until one comes, for a few ticks.
  if (slide_capture_tries_ == 0) {
    ::SetTimer(shell_hwnd_, kSlideHoldTimer, kSlideHoldTickMs, nullptr);
  }
  ++slide_capture_tries_;
}

bool PhiShellWindow::OnTabSurfaceCaptured(int request_id,
                                          const unsigned char* bgra,
                                          int width,
                                          int height) {
  if (request_id != slide_capture_request_) {
    return false;
  }
  if (!content_slide_->holding()) {
    return true;  // Released already (the hold ran out, or a cancel).
  }
  if (!bgra || width <= 0 || height <= 0) {
    // No frame yet: the hold timer asks again, or gives up.
    return true;
  }
  content_slide_->SetEnteringPage(bgra, width, height,
                                  ComputeLayout().content);
  ReleaseContentSlide();
  return true;
}

void PhiShellWindow::ReleaseContentSlide() {
  if (shell_hwnd_) {
    ::KillTimer(shell_hwnd_, kSlideHoldTimer);
  }
  if (!content_slide_->holding()) {
    return;
  }
  content_slide_->Release();
  // The leaving page leaves the screen for its composite: the slide owns
  // the content rect from here until it lands (RevealSwappedContent). The
  // composite goes under the child FIRST — a GDI paint reaches the desktop
  // ahead of a window hide made in the same turn, and the other way round
  // shows the shell's bare ground for a frame where the page was.
  ComposeSlideFrame();
  if (swap_conceal_ && ::IsWindow(swap_conceal_) &&
      swap_conceal_ != browsers_->content_hwnd()) {
    ::ShowWindow(swap_conceal_, SW_HIDE);
  }
}

void PhiShellWindow::CancelContentSlide() {
  if (shell_hwnd_) {
    ::KillTimer(shell_hwnd_, kSlideHoldTimer);
  }
  content_slide_->Cancel();
}

HWND PhiShellWindow::ContentSlideWindow() const {
  return shell_hwnd_;
}

void PhiShellWindow::ContentSlideMoved() {
  if (destroying_ || !shell_hwnd_) {
    return;
  }
  ComposeSlideFrame();
}

void PhiShellWindow::ContentSlideLanded() {
  if (destroying_ || !shell_hwnd_) {
    return;
  }
  ::KillTimer(shell_hwnd_, kSlideHoldTimer);
  RevealSwappedContent();  // The entering child shows; the leaving hides.
  LayoutContents();        // The EDIT comes back; the frame repaints.
}

bool PhiShellWindow::InSidebarResizeBand(POINT client_pt) {
  if (!services_.prefs ||
      services_.prefs->LayoutMode() == PhiLayoutMode::kComfortable) {
    return false;
  }
  const Layout layout = ComputeLayout();
  return client_pt.y >= layout.title_bar.bottom &&
         client_pt.x >= Scaled(sidebar_dip_) - Scaled(4) &&
         client_pt.x <= Scaled(sidebar_dip_);
}






PhiDragBitmap PhiShellWindow::CaptureChromeRect(const RECT& client_rect) {
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



std::array<RECT, 14> PhiShellWindow::NavHoverTable(
    const Layout& layout) const {
  // The nav buttons' hover washes (back/forward/reload/search).
  // The first six MUST stay in the paint table's order — PaintChrome keys
  // a button's hover wash on this index.
  // APPEND ONLY: PaintChrome keys a button's hover wash on its index here,
  // so inserting into the middle silently moves someone else's wash.
  return {layout.back,        layout.forward,    layout.reload,
          layout.search_tabs, sidebar_->add_tab_button(),   layout.overflow,
          layout.downloads,   layout.memory,     layout.feedback,
          layout.chat,        layout.extensions, layout.copy_link,
          layout.phi_menu,    layout.reader};
}

// The client rect a hovered element's wash covers, empty for none — so a
// hover change invalidates the old and the new element, not the whole
// shell (lane 3 §7 item 3). The sidebar answers for its own rows; the
// bands the shell still paints answer here.
RECT PhiShellWindow::HoverRect(HoverKind kind, int index) {
  RECT r = sidebar_->RowHoverRect(kind, index);
  switch (kind) {
    case HoverKind::kNone:
    case HoverKind::kTab:
    case HoverKind::kPin:
    case HoverKind::kBookmark:
    case HoverKind::kGroupHeader:
      break;
    case HoverKind::kChip:
      r = spaces_->ChipBounds(index);
      break;
    case HoverKind::kNav: {
      const std::array<RECT, 14> nav = NavHoverTable(ComputeLayout());
      r = index >= 0 && index < static_cast<int>(nav.size())
              ? nav[static_cast<size_t>(index)]
              : RECT{};
      break;
    }
    case HoverKind::kNewTabRow:
      r = ComputeLayout().new_tab_row;
      break;
    case HoverKind::kFieldExt:
      r = FieldExtRect(ComputeLayout(),
                       static_cast<size_t>(std::max(index, 0)));
      break;
    case HoverKind::kAddSpace:
      r = spaces_->add_slot();
      break;
  }
  if (r.right > r.left && r.bottom > r.top) {
    // The wash's antialiased rim, and a pill that overhangs its row by a
    // pixel at odd scales.
    ::InflateRect(&r, Scaled(2), Scaled(2));
  }
  return r;
}

void PhiShellWindow::SetHover(HoverKind kind, int index) {
  if (kind == hover_kind_ && index == hover_index_) {
    return;
  }
  // The model is unchanged: only the element the pointer left and the one
  // it landed on recompose (lane 3 §7 item 3), never the whole shell.
  const RECT before = HoverRect(hover_kind_, hover_index_);
  hover_kind_ = kind;
  hover_index_ = index;
  spaces_->UpdateHoverCard(kind == HoverKind::kChip ? index : -1);
  UpdateTabPreview();
  const RECT after = HoverRect(kind, index);
  if (!shell_hwnd_) {
    return;
  }
  for (const RECT* r : {&before, &after}) {
    if (r->right > r->left && r->bottom > r->top) {
      ::InvalidateRect(shell_hwnd_, r, FALSE);
    }
  }
}

void PhiShellWindow::OnChromeMouseMove(POINT client_pt) {
  HoverKind kind = HoverKind::kNone;
  int index = -1;
  {
    const Layout layout = ComputeLayout();
    const std::array<RECT, 14> nav = NavHoverTable(layout);
    for (size_t i = 0; i < nav.size() && kind == HoverKind::kNone; ++i) {
      if (::PtInRect(&nav[i], client_pt)) {
        kind = HoverKind::kNav;
        index = static_cast<int>(i);
      }
    }
  }
  if (kind == HoverKind::kNone) {
    if (const int chip = spaces_->ChipAt(client_pt); chip >= 0) {
      kind = HoverKind::kChip;
      index = chip;
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
    const RECT add_slot = spaces_->add_slot();
    if (add_slot.right > add_slot.left) {
      if (kind == HoverKind::kNone && ::PtInRect(&add_slot, client_pt)) {
        kind = HoverKind::kAddSpace;
        index = 0;
      }
      RECT row = ComputeLayout().spaces_strip;
      ::InflateRect(&row, -Scaled(kSpacesStripPad), 0);
      row_hovered = ::PtInRect(&row, client_pt) != FALSE;
    }
    spaces_->SetRowHovered(row_hovered);
  }
  const std::vector<ShortcutRow>& pins = sidebar_->pinned();
  for (size_t i = 0; i < pins.size() && kind == HoverKind::kNone; ++i) {
    if (::PtInRect(&pins[i].bounds, client_pt)) {
      kind = HoverKind::kPin;
      index = static_cast<int>(i);
    }
  }
  const std::vector<ShortcutRow>& marks = sidebar_->bookmarks();
  for (size_t i = 0; i < marks.size() && kind == HoverKind::kNone; ++i) {
    if (::PtInRect(&marks[i].bounds, client_pt)) {
      kind = HoverKind::kBookmark;
      index = static_cast<int>(i);
    }
  }
  const std::vector<GroupHeader>& headers = sidebar_->group_headers();
  for (size_t i = 0; i < headers.size() && kind == HoverKind::kNone; ++i) {
    if (::PtInRect(&headers[i].bounds, client_pt)) {
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
    omnibox_->SetFieldHover(::PtInRect(&layout.url_field, client_pt) !=
                            FALSE);
    for (size_t i = 0;
         i < field_extensions_.size() && kind == HoverKind::kNone; ++i) {
      const RECT slot = FieldExtRect(layout, i);
      if (::PtInRect(&slot, client_pt)) {
        kind = HoverKind::kFieldExt;
        index = static_cast<int>(i);
      }
    }
  }
  const std::vector<TabRow>& rows = sidebar_->tabs();
  for (size_t i = 0; i < rows.size() && kind == HoverKind::kNone; ++i) {
    if (::PtInRect(&rows[i].bounds, client_pt)) {
      kind = HoverKind::kTab;
      index = static_cast<int>(i);
    }
  }
  SetHover(kind, index);
}

void PhiShellWindow::ClearHover() {
  spaces_->DismissHoverCard(std::string());
  spaces_->SetRowHovered(false);
  if (tab_preview_) {
    tab_preview_->PointerExited();
  }
  SetHover(HoverKind::kNone, -1);
}

void PhiShellWindow::ShowCreateSpaceForm() {
  spaces_->ShowCreateForm();
}

void PhiShellWindow::CommitCreateSpaceForm(const PhiNewSpaceSpec& spec) {
  spaces_->CommitCreateForm(spec);
}

void PhiShellWindow::CancelCreateSpaceForm() {
  spaces_->CancelCreateForm();
}

void PhiShellWindow::PreviewCreateSpaceTheme(const std::string& theme_id,
                                                  int saturation) {
  spaces_->PreviewTheme(theme_id, saturation);
}

void PhiShellWindow::SetCreateSpaceIcon(const std::string& storage_value) {
  spaces_->SetFormIcon(storage_value);
}

void PhiShellWindow::OnSpaceFormProfileCreated(
    const std::string& profile_id) {
  spaces_->OnFormProfileCreated(profile_id);
  PhiCreateSpaceWindow::NoteProfileCreated(this, profile_id);
}

// True when `tab_id`'s split stacks its panes.
bool PhiShellWindow::SplitIsStacked(TabId tab_id) const {
  const BrowserId browser = active_browser_id();
  const SplitSnapshot* split = model_ && browser.valid() && tab_id.valid()
                                   ? model_->SplitOfTab(browser, tab_id)
                                   : nullptr;
  return split && split->layout == phi_framework::SplitLayout::kHorizontal;
}

bool PhiShellWindow::PullSnapshot(BrowserId browser,
                                       WindowSnapshot* out) const {
  return framework_ && browser.valid() && framework_->Snapshot(browser, out);
}

// The hovered row's page-snapshot card (the Mac's TabPreviewRegistration,
// which every tab surface installs on its cells). Eligibility is the Mac's:
// the setting on, no drag in flight, and a LIVE tab behind the row. A split
// row previews BOTH its panes in one card, the Mac's SplitTabPreview.
void PhiShellWindow::ResetNativeNtp() {
  native_pages_->ResetNtp();
}

void PhiShellWindow::NoteTabDistillable(TabId tab_id,
                                             bool distillable) {
  const bool changed = distillable ? distillable_tabs_.insert(tab_id).second
                                   : distillable_tabs_.erase(tab_id) > 0;
  if (!changed) {
    return;
  }
  // The verdict decides whether the address capsule carves a reader slot:
  // a layout input, so the capsule (and what moves beside it) repaints.
  RefreshLayoutInputs();
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

bool PhiShellWindow::ActiveTabDistillable() const {
  for (const TabRow& row : sidebar_->tabs()) {
    if (row.active) {
      return distillable_tabs_.count(row.id) > 0;
    }
  }
  return false;
}

bool PhiShellWindow::ActiveTabReaderOfferable() const {
  if (!active_tab_id().valid() || !CanOfferReaderForUrl(active_tab_url())) {
    return false;
  }
  if (ActiveTabDistillable()) {
    return true;
  }
  return services_.sidebar &&
         (services_.sidebar->IsReaderOfferable(active_tab_id().value,
                                               active_tab_url()) ||
          services_.sidebar->IsReaderActive(active_tab_id().value,
                                            active_tab_url()));
}

bool PhiShellWindow::ActiveTabShowsReader() const {
  return services_.sidebar && active_tab_id().valid() &&
         services_.sidebar->IsReaderActive(active_tab_id().value,
                                           active_tab_url());
}

bool PhiShellWindow::ActiveTabCanUseReader() const {
  return active_tab_id().valid() && CanOfferReaderForUrl(active_tab_url());
}

// static
bool PhiShellWindow::CanOfferReaderForUrl(const std::string& url) {
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

RECT PhiShellWindow::card_rect() {
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

void PhiShellWindow::CardRoundedCorners(bool* top, bool* bottom) const {
  const bool any = !browsers_->content_fullscreen();
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


void PhiShellWindow::ToggleReaderView(const char* entry_point) {
  if (!services_.sidebar || !active_tab_id().valid()) {
    return;
  }
  services_.sidebar->RequestToggleReaderView(active_tab_id().value,
                                             active_tab_url(), entry_point);
}

bool PhiShellWindow::LeaveReaderViewForNavigation() {
  // Every surface attached to the active tab — the reader covering it, a
  // peek floating over it — closes before the page underneath changes.
  if (!services_.sidebar || !active_tab_id().valid()) {
    return false;
  }
  const bool reader = services_.sidebar->IsReaderActive(active_tab_id().value,
                                                         active_tab_url());
  bool peek = false;
  for (const SidebarContentNode& node : sidebar_->tree().content) {
    if (node.active && node.attached_surface != SidebarSurface::kContent) {
      peek = true;
    }
  }
  for (const SidebarCell& cell : sidebar_->tree().pins) {
    if (cell.binding.active && cell.attached_surface != SidebarSurface::kContent) {
      peek = true;
    }
  }
  for (const SidebarCell& cell : sidebar_->tree().bookmarks) {
    if (cell.binding.active && cell.attached_surface != SidebarSurface::kContent) {
      peek = true;
    }
  }
  if (!reader && !peek) {
    return false;
  }
  services_.sidebar->RequestCloseSurfaces(active_tab_id().value);
  return true;
}

const char* PhiShellWindow::PeekOpenerKind(int tab_id) const {
  const SidebarTree& tree = sidebar_->tree();
  const auto owner = tree.owner_of.find(tab_id);
  if (owner == tree.owner_of.end() ||
      owner->second.kind != SidebarKey::Kind::kCell) {
    return "normal";
  }
  const SidebarCell* cell = tree.CellFor(owner->second.id);
  if (!cell) {
    return "normal";
  }
  return cell->kind == SidebarStoredKind::kPin ? "pinned" : "bookmark";
}

void PhiShellWindow::UpdateTabPreview() {
  if (!services_.prefs || !services_.prefs->ShowTabPreviews() ||
      sidebar_->drag_active()) {
    if (tab_preview_) {
      tab_preview_->PointerExited();
    }
    return;
  }

  PhiTabPreview::Target target;
  bool eligible = false;
  switch (hover_kind_) {
    case HoverKind::kTab: {
      const std::vector<TabRow>& tab_rows = sidebar_->tabs();
      if (hover_index_ >= 0 &&
          hover_index_ < static_cast<int>(tab_rows.size())) {
        const TabRow& row = tab_rows[static_cast<size_t>(hover_index_)];
        if (row.id.valid()) {
          target.key = "tab:" + base::NumberToString(row.id.value);
          target.tab_id = row.id;
          target.title = row.title;
          target.url = row.url;
          target.foreground = row.active && row.b_index < 0;
          target.anchor = row.bounds;
          if (row.b_index >= 0 && row.b_id.valid()) {
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
    }
    case HoverKind::kPin:
    case HoverKind::kBookmark: {
      const std::vector<ShortcutRow>& rows =
          hover_kind_ == HoverKind::kPin ? sidebar_->pinned()
                                         : sidebar_->bookmarks();
      if (hover_index_ >= 0 && hover_index_ < static_cast<int>(rows.size())) {
        const ShortcutRow& row = rows[static_cast<size_t>(hover_index_)];
        // Folders have no page, and an unopened row has no live tab to
        // snapshot. A BOOKMARK split carries both panes under one guid, so
        // there is no second tab id to ask for — it keeps the single card.
        if (!row.folder && row.open && row.tab_id.valid()) {
          target.key = "row:" + row.guid;
          target.tab_id = row.tab_id;
          target.title = row.title;
          target.url = base::UTF8ToWide(row.url);
          target.foreground = row.active;
          target.anchor = row.bounds;
          if (!row.url_b.empty() && row.tab_id_b.valid()) {
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
  if (target.tab_id_b.valid()) {
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
  const bool comfortable =
      services_.prefs->LayoutMode() == PhiLayoutMode::kComfortable;
  target.placement =
      (hover_kind_ == HoverKind::kPin || comfortable)
          ? PhiTabPreview::Placement::kBelowAttached
          : PhiTabPreview::Placement::kRightTopAttached;

  if (!tab_preview_) {
    tab_preview_ = std::make_unique<PhiTabPreview>(this);
  }
  tab_preview_->PointerEntered(target);
}

void PhiShellWindow::SetAgentVisible(bool visible,
                                     const RECT* frame,
                                     HWND owner) {
  if (!agent_ || !shell_hwnd_ || visible == agent_visible_) {
    return;
  }
  agent_visible_ = visible;
  if (visible && frame) {
    // Owned by the watching window for the duration, then take its frame
    // exactly, so the swap reads as that window changing Space (the Mac's
    // slot.activate), not a second one. The owner stays shown underneath:
    // an owned window is kept above its owner by the system.
    ::SetWindowLongPtrW(shell_hwnd_, GWLP_HWNDPARENT,
                        reinterpret_cast<LONG_PTR>(owner));
    ::SetWindowPos(shell_hwnd_, HWND_TOP, frame->left, frame->top,
                   frame->right - frame->left, frame->bottom - frame->top,
                   SWP_SHOWWINDOW);
    ::SetForegroundWindow(shell_hwnd_);
    return;
  }
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
    // the agent's CDP capture, just invisible to the user. The watcher it
    // covered is painted already, so nothing shows through in between;
    // then the ownership is dropped (the park moves it out from over its
    // owner first — an owned window is z-ordered with its owner).
    ::SetWindowPos(shell_hwnd_, HWND_BOTTOM, -32000, -32000, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE);
    ::SetWindowLongPtrW(shell_hwnd_, GWLP_HWNDPARENT, 0);
  }
}

void PhiShellWindow::SetAgentOwned(bool agent_owned) {
  if ((!agent_ && !HostsAgentSpace()) || agent_owned_ == agent_owned) {
    return;
  }
  agent_owned_ = agent_owned;
  sidebar_->set_agent_holds_input(AgentHoldsInput());
}

bool PhiShellWindow::AgentHoldsInput() const {
  if (!agent_owned_) {
    return false;
  }
  return agent_ || (HostsAgentSpace() && active_space() == hosted_agent_space_);
}

void PhiShellWindow::HostAgentSpace(const std::string& space_id,
                                    BrowserId browser,
                                    HWND browser_hwnd,
                                    const std::string& task_id) {
  // A browser still sliding home goes now: the swap below cuts that slide
  // short anyway, and no two agent browsers wait in this frame.
  FinishUnhost();
  const bool hand_over = HostsAgentSpace();
  const std::string previous_task = hosted_agent_task_;
  const std::string previous_space = hosted_agent_space_;
  if (!hand_over) {
    hosted_return_space_ = active_space();
  }
  hosted_agent_task_ = task_id;
  hosted_agent_space_ = space_id;
  // The slot's own Space swap: band slide, pill slide, the page revealed
  // when the slide lands — the agent Space is a Space of this slot now.
  browsers_->AttachSpaceBrowser(space_id, browser, browser_hwnd,
                                /*agent=*/true);
  if (hand_over) {
    // Agent to agent: the one that was on screen is the swap's leaving
    // page. Noted only now — the swap's start settles any earlier one
    // (RevealSwappedContent), which must not send THIS browser home before
    // its page has slid out.
    leaving_agent_task_ = previous_task;
    leaving_agent_space_ = previous_space;
    if (!BrowserHostSwapRunning()) {
      FinishUnhost();
    }
  }
}

void PhiShellWindow::LeaveHostedAgentSpace(const std::string& space_id) {
  if (!HostsAgentSpace()) {
    return;
  }
  FinishUnhost();  // A hand-over still in flight: that browser goes first.
  std::string target = space_id;
  if (target.empty() || !HasSpace(target) || PhiIsAgentSpaceId(target)) {
    target = hosted_return_space_;
  }
  if (target.empty() || !HasSpace(target) || PhiIsAgentSpaceId(target)) {
    for (const std::string& other : browsers_->spaces().order()) {
      if (!PhiIsAgentSpaceId(other)) {
        target = other;
        break;
      }
    }
  }
  if (!target.empty() && active_space() != target) {
    browsers_->ShowSpace(target);  // The normal swap back (or instant).
  }
  // The hosted page is the leaving one: it paints until the slide lands.
  leaving_agent_task_ = hosted_agent_task_;
  leaving_agent_space_ = hosted_agent_space_;
  hosted_agent_task_.clear();
  hosted_agent_space_.clear();
  hosted_return_space_.clear();
  agent_owned_ = false;
  if (sidebar_) {
    sidebar_->set_agent_holds_input(false);
  }
  if (!BrowserHostSwapRunning()) {
    FinishUnhost();
  }
}

void PhiShellWindow::FinishUnhost() {
  if (leaving_agent_task_.empty()) {
    return;
  }
  const std::string task_id = leaving_agent_task_;
  HWND child = browsers_->DetachSpaceBrowser(leaving_agent_space_);
  leaving_agent_task_.clear();
  leaving_agent_space_.clear();
  if (services_.agent) {
    services_.agent->OnAgentBrowserReturned(task_id, child);
  }
  ScheduleRefresh();  // The strip's pill is the entering Space's alone.
}

HWND PhiShellWindow::ReleaseAgentBrowserForHosting() {
  return agent_ ? browsers_->ReleaseContentChildKeepRecord() : nullptr;
}

void PhiShellWindow::ReadoptAgentBrowser(HWND browser_hwnd) {
  if (agent_) {
    browsers_->ReadoptContentChild(browser_hwnd);
  }
}

void PhiShellWindow::SetConcealedForWatch(bool concealed) {
  if (agent_ || !shell_hwnd_ || concealed) {
    return;  // Stays shown under the agent window (see the header).
  }
  ::SetForegroundWindow(shell_hwnd_);
}

void PhiShellWindow::BringToFront() {
  if (!shell_hwnd_) {
    return;
  }
  if (::IsIconic(shell_hwnd_)) {
    ::ShowWindow(shell_hwnd_, SW_RESTORE);
  }
  ::SetForegroundWindow(shell_hwnd_);
}


COLORREF PhiShellWindow::BackgroundFill() const {
  return services_.spaces
             ? services_.spaces->PaletteForSlot(slot_.value).window_bg
             : RGB(0xFF, 0xFF, 0xFF);
}

void PhiShellWindow::RefreshTabs() {
  // A structural view change (a Space switch, a layout mode, the DPI): the
  // rows must exist in the new geometry before the next frame, so the
  // rebuild runs here and the whole shell repaints. Never for a framework
  // mutation — its push schedules the rebuild.
  rows_dirty_ = true;
  RebuildTabs();
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

void PhiShellWindow::ScheduleRefresh() {
  // Coalesced: one posted rebuild per burst of changes. The rebuild turn
  // invalidates only the bands whose rows changed; nothing is invalidated
  // here, because nothing has changed on screen yet.
  rows_dirty_ = true;
  if (shell_hwnd_ && !rebuild_posted_) {
    rebuild_posted_ =
        ::PostMessageW(shell_hwnd_, kMsgPhiRebuildRows, 0, 0) != 0;
  }
}

void PhiShellWindow::FlushPendingRebuild() {
  if (!rows_dirty_) {
    return;
  }
  const BandSignature before = CurrentBandSignature();
  RebuildTabs();
  InvalidateChangedBands(before);
  // A posted kMsgPhiRebuildRows still in the queue finds the rows clean.
}

PhiShellWindow::BandSignature PhiShellWindow::CurrentBandSignature()
    const {
  // Cheap but complete: everything a band paints from its rows — key,
  // place, title, state, and a fold of the favicon bytes so an icon that
  // changes at the same size still repaints (kFaviconReady).
  const auto fold = [](const std::vector<unsigned char>& bytes) {
    uint32_t h = 2166136261u;
    for (unsigned char b : bytes) {
      h = (h ^ b) * 16777619u;
    }
    return h;
  };
  const auto rect_key = [](const RECT& r) {
    return std::to_string(r.left) + "," + std::to_string(r.top) + "," +
           std::to_string(r.right) + "," + std::to_string(r.bottom);
  };
  BandSignature out;
  for (const TabRow& row : sidebar_->tabs()) {
    out.rows.emplace_back(
        row.key.ToString() + "|" + base::WideToUTF8(row.title) + "|" +
            base::WideToUTF8(row.b_title) + "|" + (row.active ? "a" : "") +
            (row.b_active ? "b" : "") + (row.unloaded ? "u" : "") +
            std::to_string(static_cast<int>(row.attached_surface)) + "|" +
            std::to_string(fold(row.favicon)) + "|" +
            std::to_string(fold(row.b_favicon)) + "|" + rect_key(row.pane_b),
        row.bounds);
  }
  for (const GroupHeader& header : sidebar_->group_headers()) {
    out.rows.emplace_back("g:" + header.id + "|" +
                              base::WideToUTF8(header.title) + "|" +
                              std::to_string(header.color) +
                              (header.collapsed ? "c" : "") + "|" +
                              rect_key(header.container),
                          header.bounds);
  }
  const auto stored = [&](const char* band, const ShortcutRow& row) {
    out.rows.emplace_back(
        std::string(band) + row.key.ToString() + "|" +
            base::WideToUTF8(row.title) + "|" + base::WideToUTF8(row.title_b) +
            "|" + (row.open ? "o" : "") + (row.active ? "a" : "") +
            (row.expanded ? "e" : "") + std::to_string(row.depth) + "|" +
            std::to_string(static_cast<int>(row.attached_surface)) + "|" +
            std::to_string(fold(row.favicon)) + "|" +
            std::to_string(fold(row.favicon_b)),
        row.bounds);
  };
  for (const ShortcutRow& pin : sidebar_->pinned()) {
    stored("p:", pin);
  }
  for (const ShortcutRow& bookmark : sidebar_->bookmarks()) {
    stored("b:", bookmark);
  }
  for (const PhiSpaceChip& chip : spaces_->chips()) {
    out.chips.emplace_back(chip.id + "|" + base::WideToUTF8(chip.label) + "|" +
                               base::WideToUTF8(chip.icon) +
                               (chip.open ? "o" : "") +
                               (chip.is_agent ? "g" : "") +
                               std::to_string(chip.badge) +
                               (chip.id == active_space() ? "*" : ""),
                           chip.bounds);
  }
  out.chips.emplace_back("+" + std::to_string(spaces_->overflow()) +
                             (spaces_->creating() ? "c" : ""),
                         spaces_->add_slot());
  out.nav_url = omnibox_->nav_url();
  out.can_go_back = omnibox_->can_go_back();
  out.can_go_forward = omnibox_->can_go_forward();
  out.nav_loading = omnibox_->nav_loading();
  out.layout = CurrentLayoutInputs();
  return out;
}

void PhiShellWindow::InvalidateChangedBands(
    const BandSignature& before) {
  if (!shell_hwnd_) {
    return;
  }
  const BandSignature after = CurrentBandSignature();
  const auto same_rects =
      [](const std::vector<std::pair<std::string, RECT>>& a,
         const std::vector<std::pair<std::string, RECT>>& b) {
        return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                          [](const auto& x, const auto& y) {
                            return x.first == y.first &&
                                   ::EqualRect(&x.second, &y.second);
                          });
      };
  if (after.layout != before.layout) {
    // The geometry itself moved (a reader slot, the bookmark bar, a band's
    // row count): every band is placed anew.
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    return;
  }
  const Layout layout = ComputeLayout();
  const auto invalidate = [&](RECT r) {
    if (r.right > r.left && r.bottom > r.top) {
      ::InvalidateRect(shell_hwnd_, &r, FALSE);
    }
  };
  if (!same_rects(after.rows, before.rows)) {
    // The rows' band: the whole sidebar under the caption in the vertical
    // layouts, the caption band (the strip) in comfortable. The caption
    // band and the content are untouched by a row change otherwise.
    invalidate(layout.horizontal_tabs
                   ? layout.title_bar
                   : RECT{0, layout.title_bar.bottom, layout.sidebar.right,
                          layout.client.bottom});
    invalidate(layout.bookmark_bar);  // Its pills are bookmark rows.
  }
  if (after.nav_url != before.nav_url ||
      after.can_go_back != before.can_go_back ||
      after.can_go_forward != before.can_go_forward ||
      after.nav_loading != before.nav_loading) {
    // Where the nav controls and the address capsule live per layout.
    invalidate(layout.title_bar);
    invalidate(layout.top_bar);
    RECT field = layout.url_field;
    ::InflateRect(&field, Scaled(2), Scaled(2));
    invalidate(field);
  }
  if (!same_rects(after.chips, before.chips)) {
    invalidate(layout.horizontal_tabs ? layout.title_bar
                                      : layout.spaces_strip);
  }
}

PhiPalette PhiShellWindow::CurrentPalette() const {
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  if (incognito_) {
    // Incognito is FIXED-DARK regardless of theme/appearance (the Mac's
    // incognito window contract).
    static const PhiPalette kIncognito =
        PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kDark);
    return kIncognito;
  }
  // The strip owns the create form's live preview and the switch ramp: THE
  // one place a palette is overridden (phi_spaces_strip.h).
  return spaces_->RampPalette(
      services_.spaces ? services_.spaces->PaletteForSlot(slot_.value)
                       : kFallback);
}

HDC PhiShellWindow::AcquireBackBuffer(HDC dc, HGDIOBJ* old_bitmap) {
  RECT rc = {};
  ::GetClientRect(shell_hwnd_, &rc);
  const LONG bw = rc.right > 0 ? rc.right : 1;
  const LONG bh = rc.bottom > 0 ? rc.bottom : 1;
  if (back_buffer_ &&
      (back_buffer_size_.cx != bw || back_buffer_size_.cy != bh)) {
    ::DeleteObject(back_buffer_);
    back_buffer_ = nullptr;
  }
  if (!back_buffer_) {
    back_buffer_ = ::CreateCompatibleBitmap(dc, bw, bh);
    back_buffer_size_ = {bw, bh};
  }
  if (!back_buffer_) {
    return nullptr;
  }
  HDC mem = ::CreateCompatibleDC(dc);
  if (!mem) {
    return nullptr;
  }
  *old_bitmap = ::SelectObject(mem, back_buffer_);
  return mem;
}

void PhiShellWindow::ComposeSlideFrame() {
  if (!shell_hwnd_ || destroying_ || !content_slide_->running()) {
    return;
  }
  HDC dc = ::GetDC(shell_hwnd_);
  if (!dc) {
    return;
  }
  HGDIOBJ old_bmp = nullptr;
  if (HDC mem = AcquireBackBuffer(dc, &old_bmp)) {
    content_slide_->Compose(mem);
    caption_band_->PaintButtons(mem);
    ::BitBlt(dc, 0, 0, back_buffer_size_.cx, back_buffer_size_.cy, mem, 0, 0,
             SRCCOPY);
    ::SelectObject(mem, old_bmp);
    ::DeleteDC(mem);
  }
  ::ReleaseDC(shell_hwnd_, dc);
}

void PhiShellWindow::ComposeFrame(HDC mem) {
  if (HDC entering = content_slide_->EnteringDC()) {
    // The comfortable Space slide: the entering chrome is painted at rest
    // into the slide's scratch — no caption buttons — with the entering
    // page laid into its content rect, and the composite goes into the
    // frame beside the leaving one at their slid positions; the buttons go
    // on last, standing still over both. A tick composes from the scratch
    // as it is (ComposeSlideFrame); this repaint is a real one's — the
    // rebuild that brings the entering Space's rows, a hover.
    RECT client = {};
    ::GetClientRect(shell_hwnd_, &client);
    HBRUSH fill = ::CreateSolidBrush(BackgroundFill());
    ::FillRect(entering, &client, fill);
    ::DeleteObject(fill);
    {
      base::AutoReset<bool> suppress(&caption_buttons_suppressed_, true);
      // The address text painted in: the EDIT is hidden for the slide.
      PaintChrome(entering, ComputeLayout(), /*include_child_controls=*/true);
    }
    content_slide_->LayEnteringPage();
    content_slide_->Compose(mem);
    caption_band_->PaintButtons(mem);
    return;
  }
  PaintChrome(mem, ComputeLayout(), /*include_child_controls=*/false);
  sidebar_->DrawDragIndicator(mem);
}

void PhiShellWindow::RevealSwappedContent() {
  if (swap_reveal_ && ::IsWindow(swap_reveal_)) {
    const Layout layout = ComputeLayout();
    ::SetWindowPos(swap_reveal_, nullptr, layout.content.left,
                   layout.content.top,
                   layout.content.right - layout.content.left,
                   layout.content.bottom - layout.content.top,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  if (swap_conceal_ && ::IsWindow(swap_conceal_) &&
      swap_conceal_ != browsers_->content_hwnd()) {
    ::ShowWindow(swap_conceal_, SW_HIDE);
  }
  const bool had_reveal = swap_reveal_ != nullptr;
  swap_reveal_ = nullptr;
  swap_conceal_ = nullptr;
  if (had_reveal) {
    // Deferred past the slide, like the content itself.
    native_pages_->SyncNtp();
    FocusActiveBrowser();
  }
  FinishUnhost();  // An agent's page was the leaving one: hand it back.
}

void PhiShellWindow::RequestClose() {
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
  for (const auto& [space_id, browser_id] : browsers_->spaces().all()) {
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

void PhiShellWindow::PostDeferredClose() {
  if (shell_hwnd_ && ::IsWindow(shell_hwnd_)) {
    ::PostMessageW(shell_hwnd_, kMsgPhiDeferredClose, 0, 0);
  }
}

void PhiShellWindow::RunDeferredClose() {
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

bool PhiShellWindow::GetBounds(int* x, int* y, int* w, int* h) const {
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

bool PhiShellWindow::SetBounds(int x, int y, int w, int h) {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return false;
  }
  return ::SetWindowPos(shell_hwnd_, nullptr, x, y, w, h,
                        SWP_NOZORDER | SWP_NOACTIVATE) != 0;
}

void PhiShellWindow::RefreshFieldExtensions() {
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
    omnibox_->Position();  // The EDIT yields the strip's width.
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

RECT PhiShellWindow::FieldExtRect(const Layout& layout, size_t i) const {
  if (layout.field_ext_strip.right <= layout.field_ext_strip.left) {
    return RECT{};
  }
  const int btn = Scaled(kNavButton);
  const LONG left = layout.field_ext_strip.left +
                    static_cast<LONG>(i) * (btn + Scaled(2));
  return {left, layout.field_ext_strip.top, left + btn,
          layout.field_ext_strip.bottom};
}

void PhiShellWindow::RebuildChips() {
  spaces_->Rebuild();
}

// The rebuild turn (docs/architecture-review.md §3.1 principle 3): the
// chrome's own caches first, then the sidebar's one pass over the model.
// Nothing here paints, and nothing else writes a row.
void PhiShellWindow::RebuildTabs() {
  // The floating panel shows the EXPANDED sidebar of a collapsed shell.
  // While it is up the rows are built in that geometry — the docked,
  // collapsed shell shows no rows of its own — so the panel paints them as
  // they are and never rebuilds inside its paint (lane 3 §3.1).
  const base::AutoReset<bool> floating_scope(
      &floating_layout_, floating_layout_ || floating_sidebar_ != nullptr);

  RebuildNavState();
  RefreshFieldExtensions();
  RebuildChips();
  rows_dirty_ = false;
  sidebar_->Rebuild();
  // The strip row came or went (a second Space created, the last other one
  // deleted): the address bar and every band under it moved, and nothing
  // else repaints a band that was empty a moment ago.
  const bool spaces_switch = ComputeLayout().spaces_switch;
  if (spaces_switch != laid_out_spaces_switch_) {
    laid_out_spaces_switch_ = spaces_switch;
    LayoutContents();
  }
}

// --- The sidebar's public surface, forwarded --------------------------------
// The shell keeps the spellings its callers (the coordinator, the switcher,
// the search palette, the agent dialect) already use; the rows themselves
// live in the component.

TabId PhiShellWindow::active_tab_id() const {
  return sidebar_->active_tab_id();
}

const std::string& PhiShellWindow::active_tab_url() const {
  return sidebar_->active_tab_url();
}

bool PhiShellWindow::ShowsTab(TabId tab_id) const {
  return sidebar_->ShowsTab(tab_id);
}

const std::vector<TabId>& PhiShellWindow::TabMruOrder() const {
  return sidebar_->mru();
}

void PhiShellWindow::OpenOrFocusPinned(const std::string& guid,
                                            const std::string& url) {
  sidebar_->OpenOrFocusPinned(guid, url);
}

base::ListValue PhiShellWindow::LayoutRowsJson() const {
  return sidebar_->LayoutRowsJson();
}

void PhiShellWindow::OnChromeMiddleClick(int x, int y) {
  sidebar_->OnMiddleClick(POINT{x, y});
}






void PhiShellWindow::OnChromeClick(int x, int y) {
  // Any press on the chrome is an interaction: the hover card goes down and
  // stays down until the pointer leaves the row it was pressed on.
  if (tab_preview_) {
    tab_preview_->Cancel();
  }
  // Clicking anywhere else in the chrome means the user is done editing the
  // address; hand the keyboard back to the page (the field is a child window,
  // so clicks on it never reach here).
  if (omnibox_->FieldHasFocus()) {
    FocusActiveBrowser();
  }
  const Layout layout = ComputeLayout();
  POINT pt = {x, y};
  // The caption band takes its own clicks: comfortable's Phi button, a menu
  // title, and the drag space around them (phi_caption_band.h). The band's
  // TRAILING controls fall through to the block below.
  if (caption_band_ && caption_band_->OnClick(pt)) {
    return;
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
    return;
  }
  if (framework_ && ::PtInRect(&layout.forward, pt)) {
    if (LeaveReaderViewForNavigation()) {
      return;
    }
    if (const BrowserId browser = active_browser_id(); browser.valid()) {
      framework_->GoForward(browser.value);
    }
    return;
  }
  if (framework_ && ::PtInRect(&layout.reload, pt)) {
    if (const BrowserId browser = active_browser_id(); browser.valid()) {
      framework_->Reload(browser.value, /*bypass_cache=*/false);
    }
    return;
  }
  if (::PtInRect(&layout.search_tabs, pt)) {
    PhiSearchTabsPanel::Toggle(this);  // The header's magnifier.
    return;
  }
  if (::PtInRect(&layout.downloads, pt)) {
    // The Mac DownloadButtonView's popover toggle — and its
    // feature_entry_tapped: the button lives in the sidebar's bottom bar,
    // or in Comfortable's toolbar (the Mac's WebContentHeader).
    if (services_.window) {
      services_.window->CaptureAnalytics(
          kAnalyticsModuleFeature, kEventFeatureEntryTapped,
          PhiFeatureEntryProperties(
              "download", layout_mode_ == PhiLayoutMode::kComfortable
                              ? "web_content_header"
                              : "sidebar"));
    }
    const BrowserId browser = active_browser_id();
    if (services_.spaces && framework_ && browser.valid()) {
      RECT anchor = layout.downloads;
      ::MapWindowPoints(shell_hwnd_, nullptr,
                        reinterpret_cast<POINT*>(&anchor), 2);
      PhiDownloadsPopover::Toggle(shell_hwnd_, anchor,
                                  services_.spaces->PaletteForSlot(slot_.value),
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
    if (services_.spaces && framework_ && browser.valid()) {
      RECT anchor = layout.extensions;
      ::MapWindowPoints(shell_hwnd_, nullptr,
                        reinterpret_cast<POINT*>(&anchor), 2);
      PhiExtensionsPopover::Toggle(
          shell_hwnd_, anchor, services_.spaces->PaletteForSlot(slot_.value),
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
    ToggleReaderView("address_bar");  // The in-field reader button.
    return;
  }
  if (::PtInRect(&layout.copy_link, pt)) {
    // The in-field copy-link button: current URL to the clipboard, glyph
    // swaps to a checkmark for a second (the Mac copyURLButton).
    omnibox_->CopyLinkToClipboard();
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
               item.bookmark < static_cast<int>(sidebar_->bookmarks().size())) {
      const ShortcutRow& row =
          sidebar_->bookmarks()[static_cast<size_t>(item.bookmark)];
      if (row.folder) {
        ShowBookmarkBarMenu(item.bounds, row.guid, 0);
      } else if (!row.url.empty()) {
        OpenOrFocusPinned(row.guid, row.url);
      }
    }
    return;
  }
  // The stored bands are the sidebar's own (phi_sidebar_view.h): the pinned
  // grid and the bookmark outline, hit-tested here so the bookmark BAR's
  // pills above still take their clicks first.
  if (sidebar_->OnStoredRowClick(pt)) {
    return;
  }
  if (framework_ && (::PtInRect(&layout.new_tab_row, pt) ||
                     ::PtInRect(&sidebar_->add_tab_button(), pt))) {
    if (!AgentHoldsInput()) {  // The Mac refuses New Tab under the agent.
      StartNewTab();
    }
    return;
  }
  if (caption_band_ && ::PtInRect(&layout.overflow, pt)) {
    // Comfortable's only way to the menu bar. Anchored to the button's
    // trailing edge, under the toolbar, and right-aligned there.
    caption_band_->ShowOverflow({layout.overflow.right, layout.top_bar.bottom});
    return;
  }
  // The chips and rows are hit-tested as they are — what the last frame
  // showed under the pointer (docs/architecture-review.md §3.1 principle 7).
  if (const int hit = spaces_->ChipAt(pt); hit >= 0) {
    const PhiSpaceChip chip = spaces_->chips()[static_cast<size_t>(hit)];
    // A click means "switch", not "hover": drop the pip's card and keep it
    // down until the pointer leaves this pip — without this the switched-to
    // strip's fresh hover re-presents it right after the swap (the Mac's
    // suppressHoverCard). The modal popup below would freeze it, too.
    spaces_->DismissHoverCard(chip.id);
    // An agent pip starts watching that agent Space — or, from another
    // agent Space, swaps to it. It never reorders.
    if (chip.is_agent) {
      // The agent Space on screen (hosted in this slot, or this IS its
      // window) is the active Space: a click on its pip does nothing, as
      // on Mac. Another agent's pip swaps.
      if (chip.id == active_space() || (agent_ && chip.open)) {
        return;
      }
      SwitchToSpace(chip.id);
      return;
    }
    if (agent_ || HostsAgentSpace()) {
      // A user pip inside the watched agent window: the end of the watch.
      SwitchToSpace(chip.id);
      return;
    }
    if (!layout.horizontal_tabs) {
      // Vertical strips show every pip: reorderable by drag.
      sidebar_->ArmDrag(SidebarKey::Space(chip.id), pt);
    }
    if (layout.horizontal_tabs) {
      // Comfortable draws ONE compact pip — the active Space. Clicking it
      // opens the switcher popup over the FULL Space list with "New Space…"
      // after a rule, the Mac's spaceSwitcherMenu on its compact chip.
      ShowSpaceSwitcherMenu(pt, /*overflow=*/false);
      return;
    }
    if (spaces_->creating()) {
      return;  // Read-only while the form is up (the Mac's isCreatingSpace).
    }
    SwitchToSpace(chip.id);
    return;
  }
  if (services_.spaces && ::PtInRect(&layout.add_space, pt)) {
    if (spaces_->creating()) {
      return;  // The form is already up: requestCreation is a no-op then.
    }
    if (!spaces_->overflow()) {
      // The pointer is on the row, so the "+" is up — or would be, had the
      // hover-in reached us: a click under a pointer that never moved since
      // a menu dismissed over it reveals the button rather than being lost.
      // (The Mac's flag is watchdog-verified against the real pointer, so
      // its hidden-and-disabled "+" never sits under a pointer on the row.)
      spaces_->SetRowHovered(true);
      spaces_->ShowCreateForm();
      return;
    }
    // Overflow: the "…" opens the full switcher — every Space plus a
    // "New Space" row (the Mac moreButton's menu). Anchored under the
    // button, not at the pointer.
    const POINT anchor = {layout.add_space.left, layout.add_space.bottom};
    ShowSpaceSwitcherMenu(anchor, /*overflow=*/true);
    return;
  }
  // The content rows last, as the pixels stack: a group header or a tab row.
  sidebar_->OnContentRowClick(pt);
}


namespace {

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

}  // namespace


// --- Context menus (phi_shell_menus.h) -------------------------------------
// Every menu below is DATA (docs/architecture-review.md §3.1 principle 4,
// §3.5, lane 1 §3.2): the row's identity is copied into a snapshot, a pure
// builder turns that snapshot plus the live prefs into a PhiMenuSpec, the
// popup runs under a PhiModalScope, and the answer comes back as a key plus
// the target it names. Nothing here holds a reference into the sidebar's
// rows or the strip's chips across the popup's nested message
// loop, so the rebuild a title or favicon notification posts under an open
// menu cannot leave a dangling row behind — by construction, not by
// remembering to copy fields out first.

PhiPopupMenuRequest PhiShellWindow::PopupAt(POINT client_pt,
                                                 UINT align) const {
  POINT screen = client_pt;
  ::ClientToScreen(shell_hwnd_, &screen);
  PhiPopupMenuRequest request;
  request.owner = shell_hwnd_;
  request.x = screen.x;
  request.y = screen.y;
  request.align = align;
  return request;
}

std::vector<PhiMenuChoice> PhiShellWindow::SpaceMenuChoices() const {
  // Every Space but the one this slot shows — the Mac's
  // appendSpaceTransferMenuItems. Ids, never rows: the list may be rebuilt
  // while the menu is up.
  std::vector<PhiMenuChoice> choices;
  if (!services_.spaces) {
    return choices;
  }
  for (const PhiSpaceView& space : services_.spaces->Spaces()) {
    if (space.id == active_space() || choices.size() >= 99u) {
      continue;
    }
    PhiMenuChoice choice;
    choice.id = space.id;
    choice.label = base::UTF8ToWide(!space.name.empty() ? space.name
                                                        : "Space " + space.id);
    choices.push_back(std::move(choice));
  }
  return choices;
}

std::vector<PhiMenuChoice> PhiShellWindow::BookmarkFolderChoices() const {
  // This Space's bookmark folders in outline order, nesting indented.
  std::vector<PhiMenuChoice> choices;
  if (!services_.sidebar) {
    return choices;
  }
  const std::function<void(const std::string&, int)> walk =
      [&](const std::string& parent, int depth) {
        for (const PhiStoredRowView& stored :
             services_.sidebar->BookmarkRows(slot_.value, parent)) {
          if (!stored.folder || choices.size() >= 99u) {
            continue;
          }
          PhiMenuChoice choice;
          choice.id = stored.guid;
          choice.label.assign(static_cast<size_t>(depth) * 2, L' ');
          choice.label += base::UTF8ToWide(
              stored.title.empty() ? "Untitled" : stored.title);
          choices.push_back(std::move(choice));
          walk(stored.guid, depth + 1);
        }
      };
  walk(std::string(), 0);
  return choices;
}

std::vector<PhiMenuChoice> PhiShellWindow::GroupMenuChoices(
    const std::string& exclude) const {
  // A group's menu title: its name (or "Untitled") with its member count,
  // the Mac's displayTitle(memberCount:).
  std::vector<PhiMenuChoice> choices;
  for (const GroupHeader& gh : sidebar_->group_headers()) {
    if (gh.id == exclude || choices.size() >= 99u) {
      continue;
    }
    int members = 0;
    for (const TabRow& tab : sidebar_->tabs()) {
      if (tab.group_id == gh.id) {
        ++members;
      }
    }
    PhiMenuChoice choice;
    choice.id = gh.id;
    choice.label = (gh.name.empty() ? std::wstring(L"Untitled") : gh.name) +
                   L" (" + std::to_wstring(members) + L")";
    choices.push_back(std::move(choice));
  }
  return choices;
}

std::vector<PhiMenuChoice> PhiShellWindow::SpaceSwitcherChoices() const {
  // The FULL Space list, with each Space's glyph — the strip's chips only
  // mirror what fits.
  std::vector<PhiMenuChoice> choices;
  if (!services_.spaces) {
    return choices;
  }
  for (const PhiSpaceView& space : services_.spaces->Spaces()) {
    PhiMenuChoice choice;
    choice.id = space.id;
    choice.label = base::UTF8ToWide(!space.name.empty() ? space.name
                                                        : "Space " + space.id);
    choice.glyph = base::UTF8ToWide(space.icon_text);
    choices.push_back(std::move(choice));
  }
  return choices;
}

std::vector<PhiMenuBookmarkNode> PhiShellWindow::BookmarkTreeSnapshot(
    const std::string& parent_guid,
    int depth) const {
  // The store's outline as a VALUE, read before the dropdown opens.
  std::vector<PhiMenuBookmarkNode> rows;
  if (!services_.sidebar || depth > 5) {
    return rows;
  }
  for (const PhiStoredRowView& row :
       services_.sidebar->BookmarkRows(slot_.value, parent_guid)) {
    PhiMenuBookmarkNode node;
    node.guid = row.guid;
    node.url = row.url;
    node.title = base::UTF8ToWide(row.title.empty() ? row.url : row.title);
    node.folder = row.folder;
    if (row.folder) {
      node.children = BookmarkTreeSnapshot(row.guid, depth + 1);
    }
    rows.push_back(std::move(node));
  }
  return rows;
}

void PhiShellWindow::ShowGroupHeaderMenu(POINT pt,
                                              std::string group_id,
                                              std::wstring title,
                                              COLORREF color) {
  std::vector<PhiMenuColorOption> colors;
  colors.reserve(kPhiGroupColors.size());
  for (const PhiGroupColor& option : kPhiGroupColors) {
    colors.push_back({option.name, option.color == color});
  }
  const PhiMenuAction action =
      PhiRunPopupMenu(PhiBuildGroupHeaderMenu(colors),
                      PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN |
                                      TPM_RIGHTBUTTON));
  // Re-read AFTER the loop: the slot may have lost its browser under it.
  const int64_t browser = active_browser_id().value;
  if (!framework_ || !browser) {
    return;
  }
  if (action.is("group.rename")) {
    std::wstring value = title;
    if (PhiPromptText(shell_hwnd_, L"Rename Group", L"Group name:", &value)) {
      framework_->RenameTabGroup(browser, group_id.c_str(),
                                 base::WideToUTF8(value).c_str());
    }
  } else if (action.is("group.ungroup")) {
    framework_->UngroupTabGroup(browser, group_id.c_str());
  } else if (action.is("group.close")) {
    framework_->CloseTabGroup(browser, group_id.c_str());
  } else if (action.is("group.color")) {
    framework_->SetTabGroupColor(browser, group_id.c_str(),
                                 action.target.id.c_str());
  }
}

void PhiShellWindow::ShowTabRowMenu(POINT pt, const MenuRow& row) {
  // The Mac tab menu (TabModel+Sidebar), in its order: pin, duplicate, copy,
  // the split block, bookmarks, groups, then close.
  const BrowserId opening_browser = active_browser_id();
  if (!opening_browser.valid()) {
    return;  // A row of a browser that is already gone.
  }
  PhiTabMenuInputs in;
  in.merged = row.merged;
  in.incognito = incognito_;
  in.grouped = !row.group_id.empty();
  in.split_bound = !row.split_id.empty();
  in.split_stacked = row.merged && SplitIsStacked(row.id);
  if (services_.agent && row.id.valid()) {
    in.agent_driven = services_.agent->IsTabAgentDriven(row.id.value);
    in.agent_reclaimed =
        in.agent_driven && services_.agent->IsTabReclaimed(row.id.value);
  }
  if (!row.merged && row.split_id.empty()) {
    // "Split with Current" pairs this row with the FOCUSED tab; on the
    // focused row itself — or when the focused tab is already split — it
    // becomes "Open as Split", a fresh pane (the Mac's two labels).
    for (const TabRow& other : sidebar_->tabs()) {
      if (other.key != row.key && other.active && other.b_index < 0 &&
          other.split_id.empty()) {
        in.current_can_pair = true;
        break;
      }
    }
  }
  // Groups take a split row whole (both panes), so every row gets the list.
  in.groups = GroupMenuChoices(row.group_id);
  if (!row.merged) {
    in.bookmark_folders = !incognito_ && services_.sidebar != nullptr;
    if (in.bookmark_folders) {
      in.folders = BookmarkFolderChoices();
    }
    if (!incognito_) {
      in.spaces = SpaceMenuChoices();
    }
  }
  const PhiMenuAction action =
      PhiRunPopupMenu(PhiBuildTabRowMenu(in),
                      PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN |
                                      TPM_RIGHTBUTTON));
  if (!action.valid()) {
    return;
  }
  // Fresh state, read after the loop — the row's identity below is the
  // snapshot's, which no rebuild can have touched.
  const BrowserId browser = active_browser_id();
  if (!browser.valid()) {
    return;
  }
  if (action.is("tab.moveToSpace")) {
    sidebar_->Apply(
        SidebarMoveToSpace{row.key, action.target.id, /*clone=*/false});
  } else if (action.is("tab.cloneToSpace")) {
    sidebar_->Apply(
        SidebarMoveToSpace{row.key, action.target.id, /*clone=*/true});
  } else if (action.is("tab.addToFolder") && services_.sidebar) {
    services_.sidebar->AddBookmarkRow(slot_.value, action.target.id,
                          base::WideToUTF8(row.title), row.url);
  } else if (action.is("tab.newFolder") && services_.sidebar) {
    std::wstring name = L"Untitled";
    if (PhiPromptText(shell_hwnd_, L"New Folder", L"Folder name:", &name)) {
      const std::string folder = services_.sidebar->AddBookmarkFolder(
          slot_.value, std::string(), base::WideToUTF8(name));
      if (!folder.empty()) {
        services_.sidebar->AddBookmarkRow(slot_.value, folder,
                              base::WideToUTF8(row.title), row.url);
      }
    }
  } else if (action.is("tab.joinGroup")) {
    // Joining (or moving to) an existing group: the model takes the tab out
    // of its current group atomically, so one call covers both.
    sidebar_->Apply(SidebarJoinGroup{row.key, action.target.id});
  } else if (action.is("tab.moveNewGroup") || action.is("tab.newGroup")) {
    sidebar_->Apply(SidebarJoinGroup{row.key, std::string()});
  } else if (action.is("tab.reversePanes")) {
    sidebar_->Apply(SidebarSwapPanes{row.key});
  } else if (action.is("tab.pin")) {
    // Pinning MOVES the tab (or the pair) into a cell at the grid's end.
    SidebarPlace grid;
    grid.section = SidebarPlace::Section::kPinned;
    sidebar_->Apply(SidebarMove{row.key, grid});
  } else if (action.is("tab.bookmark")) {
    // The menu's bookmark is an unbound copy (the drag path binds).
    sidebar_->Apply(SidebarBookmarkCopy{row.key});
  } else if (action.is("tab.duplicate") && framework_) {
    // A split duplicates as a fresh split (both panes), as on Mac — in the
    // same arrangement as the one it came from. Ids all the way: OpenTab
    // answers with the stable id, and the split is created in its axis
    // rather than re-oriented after the fact.
    const bool stacked = row.merged && SplitIsStacked(row.id);
    const TabId fresh_a =
        row.url.empty() ? TabId() : framework_->OpenTab(browser, row.url.c_str());
    if (row.merged && fresh_a.valid() && !row.url_b.empty()) {
      const TabId fresh_b = framework_->OpenTab(browser, row.url_b.c_str());
      framework_->ActivateTabById(browser.value, fresh_a.value);
      if (fresh_b.valid()) {
        framework_->CreateSplitViewForTabs(browser.value, fresh_a.value,
                                           fresh_b.value,
                                           stacked ? "horizontal" : "vertical");
      }
    }
  } else if (action.is("tab.copyUrl") || action.is("tab.copyLeft")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url));
  } else if (action.is("tab.copyRight")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url_b));
  } else if (action.is("tab.splitCurrent")) {
    sidebar_->Apply(
        SidebarFormSplit{SidebarKey::Tab(sidebar_->tree().active_tab),
                         row.key, "vertical"});
  } else if (action.is("tab.openAsSplit") && framework_) {
    // A fresh blank pane beside this tab, the Mac's openAsSplit. The row
    // becomes the active pane first; everything is by stable id, so the new
    // tab's insertion shifting the strip changes nothing here.
    const TabId fresh = framework_->OpenTab(browser, "");
    if (fresh.valid()) {
      framework_->ActivateTabById(browser.value, row.id.value);
      sidebar_->SplitTabsNormalized(browser, row.id, fresh);
    }
  } else if (action.is("tab.takeControl") && services_.agent) {
    services_.agent->RequestReclaimTab(row.id.value);
  } else if (action.is("tab.handBack") && services_.agent) {
    services_.agent->RequestReleaseTab(row.id.value);
  } else if (action.is("tab.convertSplit")) {
    // Toggle the divider axis; the stored cell a pane is bound to remembers
    // it so the split reopens the same way.
    sidebar_->Apply(SidebarSetLayout{row.key,
                           SplitIsStacked(row.id) ? "vertical" : "horizontal"});
  } else if (action.is("tab.removeSplit")) {
    sidebar_->Apply(SidebarBreakSplit{row.key});
  } else if (action.is("tab.leaveGroup")) {
    sidebar_->Apply(SidebarLeaveGroup{row.key});
  } else if (action.is("tab.close")) {
    sidebar_->Apply(SidebarClose{row.key, -1});
  } else if (action.is("tab.closeOthers") && framework_ && model_) {
    // Every UNBOUND tab except this row (both panes of a merged one);
    // pin/bookmark-bound tabs live in their cells and stay. Closed by stable
    // id, so the ids are collected first and the shifting strip is nobody's
    // problem.
    std::vector<TabId> doomed;
    if (const WindowSnapshot* snapshot = model_->Get(browser)) {
      for (const TabSnapshot& tab : snapshot->tabs) {
        if (tab.id == row.id || tab.id == row.b_id || !tab.tag.empty()) {
          continue;
        }
        doomed.push_back(tab.id);
      }
    }
    for (TabId id : doomed) {
      framework_->CloseTabById(browser.value, id.value);
    }
  }
}

void PhiShellWindow::ShowPinnedCellMenu(POINT pt, const MenuRow& row) {
  // The Mac pinned-cell menu: open, duplicate, copy, rename, unpin, and
  // close (the bound live tab or pair) when open.
  PhiPinnedMenuInputs in;
  in.split_cell = !row.url_b.empty();
  in.horizontal = row.layout == "horizontal";
  in.open = row.open;
  const PhiMenuAction action =
      PhiRunPopupMenu(PhiBuildPinnedCellMenu(in),
                      PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN |
                                      TPM_RIGHTBUTTON));
  if (!action.valid()) {
    return;
  }
  const BrowserId browser = active_browser_id();
  if (!browser.valid()) {
    return;  // The slot lost its last browser under the menu.
  }
  if (action.is("pin.open") && !row.url.empty()) {
    OpenOrFocusPinned(row.guid, row.url);
  } else if (action.is("pin.duplicate") && framework_ && !row.url.empty()) {
    // A fresh unbound copy — for a split cell, a fresh split, in the axis
    // the cell remembers.
    const TabId fresh_a = framework_->OpenTab(browser, row.url.c_str());
    if (in.split_cell && fresh_a.valid() && !row.url_b.empty()) {
      const TabId fresh_b = framework_->OpenTab(browser, row.url_b.c_str());
      framework_->ActivateTabById(browser.value, fresh_a.value);
      if (fresh_b.valid()) {
        framework_->CreateSplitViewForTabs(
            browser.value, fresh_a.value, fresh_b.value,
            in.horizontal ? "horizontal" : "vertical");
      }
    }
  } else if (action.is("pin.copyUrl") || action.is("pin.copyLeft")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url));
  } else if (action.is("pin.copyRight")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url_b));
  } else if (action.is("pin.rename") && services_.sidebar) {
    std::wstring name = row.title;
    if (PhiPromptText(shell_hwnd_, L"Rename Pinned Tab",
                      L"Enter a new name for this pinned tab.", &name)) {
      services_.sidebar->RenameStoredRow(row.guid, base::WideToUTF8(name));
    }
  } else if (action.is("pin.convertSplit")) {
    // Flip what the cell REMEMBERS, and the live split too when it is
    // currently open — the Mac's convertClosedPinnedSplitLayout.
    sidebar_->Apply(SidebarSetLayout{SidebarKey::Cell(row.guid),
                           in.horizontal ? "vertical" : "horizontal"});
  } else if (action.is("pin.editUrl") && services_.sidebar &&
             !row.guid.empty()) {
    std::wstring edited = base::UTF8ToWide(row.url);
    if (PhiPromptText(shell_hwnd_, L"Edit Pinned Tab", L"Address:", &edited) &&
        !edited.empty()) {
      services_.sidebar->SetStoredRowUrl(row.guid, base::WideToUTF8(edited));
    }
  } else if (action.is("pin.unpin") && !row.guid.empty()) {
    // The cell goes; a live bound tab returns to the tab list as a normal
    // tab (the tags are cleared).
    sidebar_->Apply(SidebarRemoveCell{SidebarKey::Cell(row.guid)});
  } else if (action.is("pin.close")) {
    // Close the bound live tab(s); the pinned cell stays, as on Mac.
    sidebar_->Apply(SidebarClose{SidebarKey::Cell(row.guid), -1});
  }
}

void PhiShellWindow::ShowBookmarkRowMenu(POINT pt, const MenuRow& row) {
  // The Mac bookmark-row menu: open, open as split, copy, rename, delete
  // (folders keep rename / new-nested-folder / delete).
  PhiBookmarkMenuInputs in;
  in.folder = row.folder;
  in.split_row = !row.url_b.empty();
  in.horizontal = row.layout == "horizontal";
  if (!row.folder && !incognito_) {
    in.spaces = SpaceMenuChoices();
  }
  const PhiMenuAction action =
      PhiRunPopupMenu(PhiBuildBookmarkRowMenu(in),
                      PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN |
                                      TPM_RIGHTBUTTON));
  if (!action.valid()) {
    return;
  }
  const BrowserId browser = active_browser_id();
  if (!browser.valid()) {
    return;  // The slot lost its last browser under the menu.
  }
  if (action.is("bm.moveToSpace")) {
    sidebar_->Apply(SidebarMoveToSpace{SidebarKey::Cell(row.guid),
                                       action.target.id, /*clone=*/false});
  } else if (action.is("bm.cloneToSpace")) {
    sidebar_->Apply(SidebarMoveToSpace{SidebarKey::Cell(row.guid),
                                       action.target.id, /*clone=*/true});
  } else if (action.is("bm.editUrl") && services_.sidebar) {
    std::wstring edited = base::UTF8ToWide(row.url);
    if (PhiPromptText(shell_hwnd_, L"Edit Bookmark", L"Address:", &edited) &&
        !edited.empty()) {
      services_.sidebar->SetStoredRowUrl(row.guid, base::WideToUTF8(edited));
    }
  } else if (action.is("bm.open") && !row.url.empty()) {
    OpenOrFocusPinned(row.guid, row.url);
  } else if (action.is("bm.openSplit") && framework_ && !row.url.empty()) {
    if (in.split_row) {
      // A split bookmark opens as its own pair either way.
      OpenOrFocusPinned(row.guid, row.url);
    } else {
      // Pair the bookmark with the currently focused tab (the Mac's
      // openAsSplit on a bookmark row → formSplitFromBookmark). The partner
      // is the MODEL's active tab, not a paint row's: a focused tab bound to a
      // pinned cell or another bookmark is not a row there, and skipping it
      // would silently do nothing. It is unbound with the new pane below, so
      // the pair lands in the normal tab list.
      const TabSnapshot* active = model_ ? model_->ActiveTab(browser) : nullptr;
      const TabId partner_id =
          active && active->split_id.empty() ? active->id : TabId();
      const TabId fresh = framework_->OpenTab(browser, row.url.c_str());
      if (partner_id.valid() && fresh.valid()) {
        framework_->ActivateTabById(browser.value, partner_id.value);
        sidebar_->SplitTabsNormalized(browser, partner_id, fresh);
      }
    }
  } else if (action.is("bm.copyUrl") || action.is("bm.copyLeft")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url));
  } else if (action.is("bm.copyRight")) {
    CopyTextToClipboard(shell_hwnd_, base::UTF8ToWide(row.url_b));
  } else if (action.is("bm.rename") && services_.sidebar) {
    std::wstring name = row.title;
    if (PhiPromptText(shell_hwnd_,
                      row.folder ? L"Rename Folder" : L"Rename Bookmark",
                      row.folder ? L"Enter a new name for this folder."
                                 : L"Enter a new name for this bookmark.",
                      &name)) {
      services_.sidebar->RenameStoredRow(row.guid, base::WideToUTF8(name));
    }
  } else if (action.is("bm.convertSplit")) {
    sidebar_->Apply(SidebarSetLayout{SidebarKey::Cell(row.guid),
                           in.horizontal ? "vertical" : "horizontal"});
  } else if (action.is("bm.changeIcon") && row.folder && services_.sidebar) {
    // The picker hangs off the row, as the Mac's popover does.
    POINT top_left = {row.bounds.left, row.bounds.top};
    POINT bottom_right = {row.bounds.right, row.bounds.bottom};
    ::ClientToScreen(shell_hwnd_, &top_left);
    ::ClientToScreen(shell_hwnd_, &bottom_right);
    const RECT anchor = {top_left.x, top_left.y, bottom_right.x,
                         bottom_right.y};
    std::string picked;
    {
      PhiModalScope modal;  // The picker pumps its own loop.
      picked = PhiPickFolderIcon(this, anchor, row.icon);
    }
    if (!picked.empty()) {
      services_.sidebar->SetStoredRowIcon(row.guid, picked);
    }
  } else if (action.is("bm.newFolder") && row.folder && services_.sidebar) {
    std::wstring name = L"Untitled";
    if (PhiPromptText(shell_hwnd_, L"New Folder", L"Folder name:", &name)) {
      // Disclose the parent so the new child is visible where it lands.
      sidebar_->Expand(row.guid);
      services_.sidebar->AddBookmarkFolder(slot_.value, row.guid,
                                           base::WideToUTF8(name));
    }
  } else if (action.is("bm.remove") && !row.guid.empty()) {
    // A folder takes its subtree.
    sidebar_->Apply(SidebarRemoveCell{row.folder ? SidebarKey::Folder(row.guid)
                                       : SidebarKey::Cell(row.guid)});
  }
}

void PhiShellWindow::ShowTabAreaMenu(POINT pt) {
  // The Mac tab-area menu (TabAreaContextMenuHelper): new tab, reopen, the
  // bookmark block, the layout switch, then the active Space's own controls
  // (the Spaces menu's rows, for the Space under the pointer).
  if (!services_.spaces) {
    return;
  }
  PhiTabAreaMenuInputs in;
  in.incognito = incognito_;
  in.tabs_empty = sidebar_->tabs().empty();
  in.mode = layout_mode_;
  if (!incognito_) {
    in.can_delete_space = services_.spaces->CanDeleteSpace(active_space());
    for (const PhiSpaceView& space : services_.spaces->Spaces()) {
      if (space.id == active_space()) {
        in.active_profile = space.profile_id;
        break;
      }
    }
    // The default Space's window cannot be re-bound (a fresh shell's first
    // browser lands there), so its rows read the binding out but stay
    // disabled.
    in.can_reprofile = active_space() != services_.spaces->DefaultSpaceId();
    for (const std::string& summary : services_.spaces->ProfileSummaries()) {
      const size_t tab = summary.find('\t');
      const std::string id = summary.substr(0, tab);
      if (id.empty()) {
        continue;
      }
      PhiMenuChoice choice;
      choice.id = id;
      choice.label = base::UTF8ToWide(
          tab == std::string::npos ? id : summary.substr(tab + 1));
      in.profiles.push_back(std::move(choice));
    }
  }
  const PhiPopupMenuRequest request =
      PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON);
  const PhiMenuAction action =
      PhiRunPopupMenu(PhiBuildTabAreaMenu(in), request);
  if (action.is("area.setTheme")) {
    // Anchored on the click, like the Mac's .nearTrigger placement.
    const RECT anchor = {request.x, request.y, request.x, request.y};
    PhiShowSpaceThemePanel(this, anchor, active_space());
  } else if (action.is("area.profile")) {
    // Confirmed like the Mac's: the Space's window is replaced and its tabs
    // reload in the new profile, where the old profile's logins do not
    // exist.
    if (PhiConfirm(shell_hwnd_,
                   PhiStrF("app.changeSpaceProfileConfirmation.title",
                           {action.target.text})
                       .c_str(),
                   PhiStr("app.changeSpaceProfileConfirmation."
                          "profileScopedMessage"))) {
      services_.spaces->SetSpaceProfile(active_space(), action.target.id);
    }
  } else if (action.is("area.newTab")) {
    StartNewTab();
  } else if (action.is("area.newRootFolder")) {
    std::wstring name = L"Untitled";
    if (PhiPromptText(shell_hwnd_, L"New Folder", L"Folder name:", &name)) {
      services_.sidebar->AddBookmarkFolder(slot_.value, std::string(),
                               base::WideToUTF8(name));
    }
  } else if (action.is("area.layout")) {
    services_.prefs->RequestLayoutMode(action.target.id == "balanced"
                                 ? PhiLayoutMode::kBalanced
                             : action.target.id == "comfortable"
                                 ? PhiLayoutMode::kComfortable
                                 : PhiLayoutMode::kPerformance);
  } else if (PhiMenuBar* bar = menu_bar(); bar && action.valid()) {
    // The rest are the menu bar's own commands, run through it so the
    // sidebar and the bar cannot drift apart.
    if (action.is("area.reopen")) {
      bar->OnCommand(kCmdReopenClosedTab);
    } else if (action.is("area.bookmarkAll")) {
      bar->OnCommand(kCmdBookmarkAllTabs);
    } else if (action.is("area.newSpace")) {
      bar->OnCommand(kCmdNewSpace);
    } else if (action.is("area.renameSpace")) {
      bar->OnCommand(kCmdRenameSpace);
    } else if (action.is("area.changeIcon")) {
      bar->OnCommand(kCmdChangeIcon);
    } else if (action.is("area.deleteSpace")) {
      bar->OnCommand(kCmdDeleteSpace);
    }
  }
}

void PhiShellWindow::ShowAgentChipMenu(POINT pt, std::string task) {
  // The agent pip's own menu (the Mac agentContextMenu): the transcript
  // console and finishing the task. The Space menu below would misapply
  // rename/theme/delete to a task id.
  const PhiMenuAction action = PhiRunPopupMenu(
      PhiBuildAgentChipMenu(), PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN));
  if (!services_.agent) {
    return;
  }
  if (action.is("agent.transcript")) {
    services_.agent->RequestShowTranscript(task);
  } else if (action.is("agent.finish")) {
    services_.agent->RequestAgentFinish(task);
    ScheduleRefresh();  // The pip leaves the strip with the task.
  }
}

void PhiShellWindow::ShowSpaceSwitcherMenu(POINT pt, bool overflow) {
  // The full Space list as a popup, then "New Space…": comfortable's
  // compact pip opens it over every Space (the Mac's spaceSwitcherMenu on
  // its compact chip), and the overflow "…" adds the new-profile row too
  // (the moreButton's menu).
  if (!services_.spaces) {
    return;
  }
  const std::vector<PhiMenuChoice> spaces = SpaceSwitcherChoices();
  PhiPopupMenuRequest request = PopupAt(pt, TPM_LEFTALIGN | TPM_TOPALIGN);
  // Each row wears its Space's icon as a bitmap — the strip's artwork or
  // colour emoji, which menu text cannot render.
  request.glyph_px = Scaled(16);
  std::vector<PhiMenuChoice> agents;
  std::string watched_agent;
  if (services_.agent && !agent_) {
    // This window's own list: an agent Space on screen in another window
    // is not offered here (PhiAgentSpaceManager::Presents).
    for (const PhiAgentPipView& pip :
         services_.agent->AgentSpaces(slot_.value)) {
      PhiMenuChoice choice;
      choice.id = pip.task_id;
      choice.label = PhiAgentPipLabel(pip);
      choice.glyph = pip.icon;
      if (pip.watched) {
        watched_agent = pip.task_id;
      }
      agents.push_back(std::move(choice));
    }
  }
  const PhiMenuAction action = PhiRunPopupMenu(
      PhiBuildSpaceSwitcherMenu(spaces, agents, watched_agent, active_space(),
                                /*with_new_space=*/true,
                                /*with_new_profile=*/overflow),
      request);
  if (action.is("agent.watch")) {
    if (services_.agent) {
      services_.agent->RequestWatchAgentSpace(action.target.id, slot_.value);
    }
  } else if (action.is("space.new")) {
    ShowCreateSpaceForm();
  } else if (action.is("space.newWithProfile")) {
    services_.spaces->RequestCreateSpace(slot_.value, /*new_profile=*/true);
  } else if (action.is("space.switch")) {
    SwitchToSpace(action.target.id);
  }
}

void PhiShellWindow::OnChromeContextMenu(int x, int y) {
  if (!services_.sidebar || !shell_hwnd_) {
    return;
  }
  const POINT pt = {x, y};
  // The chips and rows are hit-tested as the last frame showed them; what a
  // hit produces is a SNAPSHOT, copied here, and every menu below runs from
  // that copy alone.
  if (const int hit = spaces_->ChipAt(pt); hit >= 0) {
    const PhiSpaceChip& chip = spaces_->chips()[static_cast<size_t>(hit)];
    if (chip.is_agent) {
      ShowAgentChipMenu(pt, chip.task_id);
    }
    // A user Space pip has no context menu: switching is a click, and
    // rename / icon / theme / profile / delete live in the sidebar's own
    // menu (right-click its empty space), Settings > Spaces and the Spaces
    // menu.
    return;
  }
  // Not a chip: the row context menus. Tab rows work in both orientations
  // (the strip's horizontal tabs share sidebar_->tabs()). None of them
  // while the agent holds the window — the Mac's sidebar refuses its whole
  // tab-row menu then; the pill is the one way to take over.
  if (AgentHoldsInput()) {
    return;
  }
  for (const GroupHeader& gh : sidebar_->group_headers()) {
    if (::PtInRect(&gh.bounds, pt)) {
      ShowGroupHeaderMenu(pt, gh.id, gh.name, gh.color);
      return;
    }
  }
  for (const TabRow& row : sidebar_->tabs()) {
    if (!::PtInRect(&row.bounds, pt)) {
      continue;
    }
    MenuRow snapshot;
    snapshot.key = row.key;
    snapshot.id = row.id;
    snapshot.merged = row.b_index >= 0;
    snapshot.b_id = snapshot.merged ? row.b_id : TabId();
    snapshot.title = row.title;
    snapshot.url = base::WideToUTF8(row.url);
    snapshot.url_b =
        snapshot.merged ? base::WideToUTF8(row.b_url) : std::string();
    snapshot.split_id = row.split_id;
    snapshot.group_id = row.group_id;
    ShowTabRowMenu(pt, snapshot);
    return;
  }
  for (const ShortcutRow& pin : sidebar_->pinned()) {
    if (!::PtInRect(&pin.bounds, pt)) {
      continue;
    }
    MenuRow snapshot;
    snapshot.key = pin.key;
    snapshot.guid = pin.guid;
    snapshot.url = pin.url;
    snapshot.url_b = pin.url_b;
    snapshot.title = pin.title;
    snapshot.layout = pin.layout;
    snapshot.open = pin.open;
    ShowPinnedCellMenu(pt, snapshot);
    return;
  }
  for (const ShortcutRow& bookmark : sidebar_->bookmarks()) {
    if (!::PtInRect(&bookmark.bounds, pt)) {
      continue;
    }
    MenuRow snapshot;
    snapshot.key = bookmark.key;
    snapshot.guid = bookmark.guid;
    snapshot.url = bookmark.url;
    snapshot.url_b = bookmark.url_b;
    snapshot.title = bookmark.title;
    snapshot.layout = bookmark.layout;
    snapshot.icon = bookmark.icon;
    snapshot.folder = bookmark.folder;
    snapshot.bounds = bookmark.bounds;
    ShowBookmarkRowMenu(pt, snapshot);
    return;
  }
  // The tab area's background — the Mac tab-area menu's bookmark block
  // (TabAreaContextMenuHelper): root folder creation lives here. Both
  // orientations share it, as the Mac's two hosts share one
  // TabAreaContextMenuHelper: the sidebar below the band in the vertical
  // layouts, and in comfortable the caption band itself, whose bare run past
  // the last tab IS that layout's empty tab-strip space
  // (TabStripBarController hangs the same stripContextMenu off the bar).
  const Layout layout = ComputeLayout();
  const bool in_tab_area =
      layout.horizontal_tabs
          ? ::PtInRect(&layout.title_bar, pt) != 0
          : (pt.x < layout.sidebar.right && pt.y > layout.title_bar.bottom);
  if (in_tab_area) {
    ShowTabAreaMenu(pt);
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

void PhiShellWindow::ShowFloatingSidebar() {
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
    cls.lpfnWndProc = &PhiShellWindow::FloatingSidebarProc;
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
  // The rows move into the panel's geometry NOW, before the slide's first
  // frame renders them (AnimateWindow paints synchronously through
  // WM_PRINTCLIENT): a viewport change, like a layout mode change, not a
  // paint-time rebuild.
  rows_dirty_ = true;
  RebuildTabs();
  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(floating_sidebar_, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner, sizeof(corner));
  // The Mac's 0.15s ease-out slide-in.
  ::AnimateWindow(floating_sidebar_, 140, AW_SLIDE | AW_HOR_POSITIVE);
  ::SetTimer(floating_sidebar_, kFloatingTickTimer, 100, nullptr);
  floating_shown_tick_ = ::GetTickCount64();
  floating_outside_tick_ = 0;
}

void PhiShellWindow::HideFloatingSidebar(bool animated) {
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
  ScheduleRefresh();  // Back to the docked geometry on the next turn.
}

void PhiShellWindow::FloatingSidebarTick() {
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

void PhiShellWindow::PaintFloatingSidebar(HDC dc) {
  if (!floating_sidebar_) {
    return;
  }
  RECT panel_rc = {};
  ::GetClientRect(floating_sidebar_, &panel_rc);
  // The rows were built for this geometry on the rebuild turn (RebuildTabs
  // builds in the expanded layout while the panel is up); only the layout
  // override is flipped here, for PaintChrome's rects.
  floating_layout_ = true;
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
LRESULT CALLBACK PhiShellWindow::FloatingSidebarProc(HWND hwnd,
                                                          UINT msg,
                                                          WPARAM wparam,
                                                          LPARAM lparam) {
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiShellWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self || self->destroying_) {
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  // Events forward through the docked sidebar's handlers under the
  // expanded-layout override, shifted by the panel's band offset.
  const auto forward = [&](auto&& call) {
    self->floating_layout_ = true;
    call();  // Reads the rows as they are (built for this geometry).
    self->floating_layout_ = false;
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
          self->sidebar_->AbortDrag();  // Drags never start from the panel.
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
          self->sidebar_->OnWheel(pt, GET_WHEEL_DELTA_WPARAM(wparam));
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

bool PhiShellWindow::BookmarkBarVisible() {
  if (!services_.prefs || incognito_ || browsers_->content_fullscreen()) {
    return false;
  }
  if (services_.prefs->AlwaysShowBookmarkBar()) {
    return true;
  }
  if (!services_.prefs->BookmarkBarOnNewTab()) {
    return false;
  }
  // "On new tab": Chromium's NTP, a blank tab, or the placeholder shell.
  if (browsers_->ActiveBrowserPlaceholding()) {
    return true;
  }
  return active_tab_url().empty() || active_tab_url() == "about:blank" ||
         active_tab_url().rfind("chrome://newtab", 0) == 0 ||
         active_tab_url().rfind("chrome://new-tab-page", 0) == 0;
}

void PhiShellWindow::PaintBookmarkBar(HDC dc,
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
  for (size_t i = 0; i < sidebar_->bookmarks().size(); ++i) {
    if (sidebar_->bookmarks()[i].depth == 0) {
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
    const ShortcutRow& row = sidebar_->bookmarks()[static_cast<size_t>(index)];
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
      sidebar_->DrawFolderIcon(dc, icon, row.icon, palette.label_dim, false);
    } else if (!row.favicon.empty()) {
      sidebar_->DrawFavicon(dc, icon, row.favicon.data(), row.favicon_width,
                  row.favicon_height, palette.ground_top);
    } else {
      HGDIOBJ previous = caption_font_ ? ::SelectObject(dc, caption_font_)
                                       : nullptr;
      PhiDrawGlyph(dc, icon, L"", palette.label_dim);  // Globe.
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
    PhiDrawGlyph(dc, more, L"", palette.label_dim);  // Chevron right.
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

void PhiShellWindow::ShowBookmarkBarMenu(RECT anchor,
                                              std::string parent_guid,
                                              size_t skip_first) {
  if (!services_.sidebar) {
    return;
  }
  // The outline is read into a value FIRST: the dropdown's loop can dispatch
  // a rebuild, and the rows it walks must not be the store's live ones.
  const PhiMenuSpec spec = PhiBuildBookmarkBarMenu(
      BookmarkTreeSnapshot(parent_guid, 0), skip_first);
  const POINT at = {anchor.left, anchor.bottom};
  const PhiMenuAction action =
      PhiRunPopupMenu(spec, PopupAt(at, TPM_LEFTALIGN | TPM_TOPALIGN));
  if (action.is("bar.open")) {
    OpenOrFocusPinned(action.target.id, action.target.url);
  }
}

void PhiShellWindow::PaintChrome(HDC dc,
                                     const Layout& layout,
                                     bool include_child_controls) {
  // The SLOT's palette (a Space may pin its own theme), ramped source →
  // target while a Space swap slide is in flight.
  const PhiPalette palette = CurrentPalette();
  const COLORREF kTintTop = palette.ground_top;
  const COLORREF kTintBottom = palette.ground_bottom;
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
  // the band draws its own titles, glyphs and hot states over that
  // (phi_caption_band.h owns every one of them).
  caption_band_->Paint(dc, /*with_buttons=*/!caption_buttons_suppressed_);

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
        {layout.back, L"\uE72B", omnibox_->can_go_back()},
        {layout.forward, L"\uE72A", omnibox_->can_go_forward()},
        {layout.reload, omnibox_->nav_loading() ? L"\uE711" : L"\uE72C", true},
        {layout.search_tabs, L"\uE721", true},
        // Comfortable only: the strip's "+" and the toolbar's overflow.
        {sidebar_->add_tab_button(), L"\uE710", true},
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
        PhiFillRounded(dc, button.rect, palette.hover, Scaled(4));
      }
      PhiDrawGlyphIcon(dc, button.rect, button.glyph, Scaled(kSpaceIcon),
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
  omnibox_->Position();
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
  const bool field_hot =
      omnibox_->field_hover() && !pointer_on_field_accessory;

  if (layout.mode == PhiLayoutMode::kPerformance) {
    // Resting 4% wash, brightening to the emphasized 8% under the cursor
    // (SideAddressBar's hoveredColor).
    PhiFillRounded(dc, layout.url_field, field_hot ? kFieldBgHot : kFieldBg,
                Scaled(kCornerRadius));
  } else if (layout.mode == PhiLayoutMode::kComfortable) {
    // Chrome's omnibox: a full pill standing on the toolbar, on the same 4%
    // wash the sidebar field uses, brightening under the cursor. Balanced's
    // field stays bare — it has the card's own ground behind it; this one
    // has only the flat toolbar, and would vanish into it.
    const int pill_r = (layout.url_field.bottom - layout.url_field.top) / 2;
    PhiFillRounded(dc, layout.url_field, field_hot ? kFieldBgHot : kFieldBg,
                pill_r);
    // The page's globe leads the pill, where Chrome puts its site chip.
    if (layout.page_icon.right > layout.page_icon.left) {
      HGDIOBJ previous_icon_font =
          caption_font_ ? ::SelectObject(dc, caption_font_) : nullptr;
      PhiDrawGlyph(dc, layout.page_icon, L"\uE774", kLabelDim);
      if (previous_icon_font) {
        ::SelectObject(dc, previous_icon_font);
      }
    }
  } else if (field_hot &&
             layout.url_field.right > layout.url_field.left) {
    // The top-bar field rests BARE and takes the wash only under the
    // cursor (the Mac top bar's transparent-until-hovered field).
    PhiFillRounded(dc, layout.url_field, kFieldBg, Scaled(kCornerRadius));
  }
  if (include_child_controls) {
    // What the user actually sees, mid-edit.
    const std::wstring shown = omnibox_->DisplayedUrl();
    ::SetTextColor(dc, shown.empty() ? kLabelDim : kLabel);
    RECT url_rc = layout.url_field;
    url_rc.left += Scaled(12);  // SideAddressBar.textFieldLeadingInset
    // Stop before the in-field trailing stack when it exists — the leftmost
    // accessory that actually sits inside the capsule (the same rule
    // PhiOmniboxTextRect applies to the EDIT).
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
    const COLORREF field_fill =
        omnibox_->field_hover() ? kFieldBgHot : kFieldBg;
    for (size_t i = 0; i < field_extensions_.size(); ++i) {
      const RECT slot = FieldExtRect(layout, i);
      if (slot.right <= slot.left) {
        continue;
      }
      if (hover_kind_ == HoverKind::kFieldExt &&
          hover_index_ == static_cast<int>(i)) {
        PhiFillRounded(dc, slot, palette.hover, Scaled(4));
      }
      const FieldExtension& ext = field_extensions_[i];
      const int mid_x = (slot.left + slot.right) / 2;
      const int mid_y = (slot.top + slot.bottom) / 2;
      RECT icon = {mid_x - Scaled(8), mid_y - Scaled(8), mid_x + Scaled(8),
                   mid_y + Scaled(8)};
      if (!ext.icon.empty()) {
        sidebar_->DrawFavicon(dc, icon, ext.icon.data(), ext.icon_w, ext.icon_h,
                    field_fill);
      } else {
        // Icon not decoded yet: the puzzle stands in.
        HGDIOBJ previous_ext_font =
            chip_icon_font_ ? ::SelectObject(dc, chip_icon_font_) : nullptr;
        PhiDrawGlyph(dc, slot, L"", kLabelDim);
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
      PhiFillRounded(dc, layout.reader, accessory_wash(layout.reader), Scaled(4));
    }
    PhiDrawReaderGlyph(dc, layout.reader, kLabel, dpi_);
  }
  if (layout.copy_link.right > layout.copy_link.left) {
    HGDIOBJ previous_link_font =
        chip_icon_font_ ? ::SelectObject(dc, chip_icon_font_) : nullptr;
    if (accessory_hot(11, layout.copy_link)) {
      PhiFillRounded(dc, layout.copy_link, accessory_wash(layout.copy_link),
                  Scaled(4));
    }
    PhiDrawGlyph(dc, layout.copy_link,
              omnibox_->copy_link_flash() ? L"" : L"", kLabel);
    if (previous_link_font) {
      ::SelectObject(dc, previous_link_font);
    }
  }

  // Space pips: 24pt, icon only. The selected one sits on a selected-pill
  // card (SpacesStripView's matchedGeometry indicator); the trailing "+"
  // creates a Space.
  // The pips, the create form's placeholder and the trailing "+" / "…"
  // (phi_spaces_strip.h). Only the strip paints them, and it never paints
  // anything else.
  spaces_->Paint(dc, StripRects(layout), palette);

  sidebar_->Paint(dc, BandRects(layout), palette);

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
        PhiFillRounded(dc, layout.downloads, palette.hover, Scaled(6));
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
        PhiFillRounded(dc, layout.memory, palette.hover, Scaled(6));
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
        PhiFillRounded(dc, layout.extensions, palette.hover,
                    Scaled(layout.mode == PhiLayoutMode::kPerformance ? 4
                                                                      : 6));
      }
      PhiDrawGlyph(dc, layout.extensions, L"", kLabel);  // Puzzle.
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
        bottom_hot(3) ? PhiBlendOver(RGB(0x00, 0x00, 0x00), palette.accent, 40)
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
  spaces_->SyncHoverCardPopup(dc, StripRects(layout));

  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}




LRESULT CALLBACK PhiShellWindow::ShellWndProc(HWND hwnd,
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
    if (auto* dying = reinterpret_cast<PhiShellWindow*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA))) {
      PhiUiaWindowDestroyed(dying);  // Release any provider a client holds.
    }
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_GETOBJECT) {
    // The sidebar's rows reach assistive technology from here: the shell is
    // the hit source (PhiHitSource), so the provider needs no window of its
    // own. Every other object id falls through to DefWindowProc, which
    // keeps answering MSAA for the child controls as before.
    auto* source = reinterpret_cast<PhiShellWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    bool handled = false;
    const LRESULT result =
        source ? PhiUiaOnGetObject(source, wparam, lparam, &handled) : 0;
    if (handled) {
      return result;
    }
  }
  auto* self = reinterpret_cast<PhiShellWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && self->destroying_) {
    // The destructor's DestroyWindow: the object is going, its members are
    // half torn down — every handler below would do work on it.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  switch (msg) {
    case WM_ACTIVATEAPP:
      // Phi as a whole gained or lost the foreground (every top-level
      // window hears it; the host keeps the one edge). The Mac SDK's
      // Application Opened / Backgrounded pair, for the analytics.
      if (self && self->services_.window) {
        self->services_.window->NoteAppActivation(wparam != FALSE);
      }
      break;
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
      const HWND page = self ? self->browsers_->content_hwnd() : nullptr;
      if (!in_chrome && self && !PhiGroupOverview::IsOpen() &&
          !self->native_pages_->ntp_mounted() && page &&
          ::IsWindow(page)) {
        ::SetFocus(page);
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
          self->caption_band_) {
        self->caption_band_->OnWindowDeactivated();
      }
      // Activation/focus arriving at the shell top-level must be forwarded to
      // the child: as a WS_CHILD, Chromium's window reports IsTopLevelWindow()
      // == false and skips its own activation path.
      if (const HWND page = self ? self->browsers_->content_hwnd() : nullptr;
          self && page && ::IsWindow(page) &&
          !self->omnibox_->FieldHasFocus() &&
          !(msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
        if (self->native_pages_->ntp_wants_focus()) {
          // The native new-tab page is up: its omnibox is the focus.
          self->native_pages_->FocusNtpField();
          return 0;
        }
        ::SetFocus(page);
        const BrowserId browser = self->active_browser_id();
        if (self->framework_ && browser.valid()) {
          self->framework_->FocusBrowserWindow(browser.value);
        }
      }
      return 0;
    case WM_CTLCOLOREDIT:
      // The EDIT would otherwise paint itself in SYSTEM colours; the session
      // hands it the palette's brush instead.
      if (self && self->services_.spaces) {
        if (HBRUSH brush = self->omnibox_->OnCtlColorEdit(
                reinterpret_cast<HDC>(wparam),
                reinterpret_cast<HWND>(lparam))) {
          return reinterpret_cast<LRESULT>(brush);
        }
      }
      break;
    case WM_NCCALCSIZE:
      // The custom frame is the caption band's (phi_caption_band.h): it
      // claims the caption for the client area and leaves only the resize
      // borders non-client.
      if (self && self->caption_band_) {
        LRESULT result = 0;
        if (self->caption_band_->OnNcCalcSize(hwnd, wparam, lparam, &result)) {
          return result;
        }
      }
      break;
    case WM_NCHITTEST:
      if (self && self->caption_band_) {
        LRESULT result = 0;
        if (self->caption_band_->OnNcHitTest(hwnd, lparam, &result)) {
          return result;
        }
      }
      break;
    case WM_NCLBUTTONDOWN:
      // The band swallows presses on its own caption buttons; the action
      // fires on release over the same button, like the OS ones.
      if (self && self->caption_band_ &&
          self->caption_band_->OnNcLButtonDown(wparam)) {
        return 0;
      }
      break;
    case WM_NCLBUTTONUP:
      if (self && self->caption_band_ &&
          self->caption_band_->OnNcLButtonUp(hwnd, wparam)) {
        return 0;
      }
      break;
    case WM_NCRBUTTONUP:
      // Comfortable's band IS the tab strip, and its bare run past the last
      // tab drags the window (the band's HitTest serves HTCAPTION there), so a
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
        POINT band_pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ::ScreenToClient(hwnd, &band_pt);
        // The caption buttons' hover and the hidden titles' reveal: most of
        // the band hit-tests as HTCAPTION, so the pointer crossing it
        // arrives here and not as WM_MOUSEMOVE.
        if (self->caption_band_) {
          self->caption_band_->OnNcMouseMove(wparam, band_pt);
        }
        // The collapsed sidebar's hover trigger includes the left RESIZE
        // band — the outer frame pixels are non-client, so slamming the
        // cursor to the window edge (the Mac gesture) arrives here, not as
        // WM_MOUSEMOVE.
        if (self->sidebar_collapsed_ && !self->floating_sidebar_ &&
            wparam == HTLEFT) {
          const Layout collapsed = self->ComputeLayout();
          if (!collapsed.horizontal_tabs &&
              band_pt.y > collapsed.title_bar.bottom) {
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
      if (self && self->caption_band_) {
        self->caption_band_->OnNcMouseLeave();
      }
      break;
    case WM_MOUSEMOVE:
      if (self) {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        // The Phi button's held press: past the drag threshold it hands
        // itself to the frame's move loop (phi_caption_band.h), and until it
        // decides nothing else in the chrome may see the travel.
        if (self->caption_band_ && self->caption_band_->OnPressMove(pt)) {
          return 0;
        }
        if (self->sidebar_collapsed_ && !self->floating_sidebar_) {
          const Layout collapsed = self->ComputeLayout();
          if (!collapsed.horizontal_tabs &&
              pt.x < collapsed.content.left &&
              pt.y > collapsed.title_bar.bottom) {
            self->ShowFloatingSidebar();
          }
        }
        // The band's own hover: the caption buttons leave (a client move
        // means we are off them) and the titles reveal or highlight.
        if (self->caption_band_) {
          self->caption_band_->OnMouseMove(pt);
        }
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
        if ((wparam & MK_LBUTTON) && self->sidebar_->drag_armed()) {
          self->sidebar_->UpdateDrag(pt);
        } else {
          self->OnChromeMouseMove(pt);
        }
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      break;
    case WM_MOUSELEAVE:
      if (self) {
        if (self->caption_band_) {
          self->caption_band_->OnMouseLeave();
        }
        self->ClearHover();
      }
      break;
    case kMsgPhiSyncNativePages:
      if (self) {
        self->native_pages_->Sync();
      }
      return 0;
    case kMsgPhiSlideCapture:
      if (self) {
        self->StartSlideCapture();
      }
      return 0;
    case kMsgPhiRebuildRows:
      if (self) {
        self->rebuild_posted_ = false;
        if (self->rows_dirty_) {
          // Once per turn, then only the bands whose rows changed repaint —
          // the caption band and the content stay put for a sidebar change.
          const BandSignature before = self->CurrentBandSignature();
          self->RebuildTabs();
          self->InvalidateChangedBands(before);
        }
      }
      return 0;
    case kMsgPhiDeferredClose:
      if (self) {
        self->RunDeferredClose();  // May destroy `self`.
      }
      return 0;
    case kMsgPhiToggleMenuReveal:
      if (self && self->caption_band_) {
        self->caption_band_->ToggleAltReveal();
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
    case PhiBrowserHost::kMsgRaiseCaps:
      if (self) {
        // Above the browser child; the bubble and caps go above it next.
        self->native_pages_->RaiseNtp();
        if (self->services_.sidebar) {
          // The reader panel covers the whole card, so it has to come up
          // before the caps that round it.
          self->services_.sidebar->RaiseReaderPanels(self->slot_.value);
        }
        self->browsers_->RaiseOverlays();
      }
      return 0;
    case WM_INITMENUPOPUP:
      // A menu is about to drop down — refresh its dynamic rows and enable
      // states, the Win32 twin of the Mac's menuWillOpen rebuilds.
      if (self && self->caption_band_) {
        self->caption_band_->OnMenuOpening(reinterpret_cast<HMENU>(wparam));
        return 0;
      }
      break;
    case WM_COMMAND:
      // lparam == 0 filters out child-control notifications (the address
      // field's EN_* arrive here too, carrying its HWND). HIWORD 0 is a
      // menu pick, 1 an accelerator — Win32's own convention, which the
      // keyboard service follows when it posts a chord (the reader's usage
      // metric tells the two apart; nothing else does).
      if (self && lparam == 0 && HIWORD(wparam) <= 1 && self->caption_band_ &&
          self->caption_band_->OnCommand(LOWORD(wparam),
                                         HIWORD(wparam) == 1)) {
        return 0;
      }
      if (self && self->omnibox_->OnShellCommand(wparam, lparam)) {
        return 0;  // The address field's EN_CHANGE: autocomplete runs.
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
        for (const GroupHeader& gh : self->sidebar_->group_headers()) {
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
        // A Phi-button press that never travelled: the menu drops here.
        if (self->caption_band_ && self->caption_band_->OnPressUp()) {
          return 0;
        }
        if (self->sidebar_resizing_) {
          self->sidebar_resizing_ = false;
          if (::GetCapture() == hwnd) {
            ::ReleaseCapture();
          }
          if (self->services_.prefs) {
            self->services_.prefs->RequestSidebarWidth(self->sidebar_dip_);
          }
          return 0;
        }
        self->sidebar_->CommitDrag();
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_CAPTURECHANGED:
      if (self) {
        if (self->caption_band_) {
          self->caption_band_->CancelPress();
        }
        if (self->sidebar_resizing_) {
          self->sidebar_resizing_ = false;
          if (self->services_.prefs) {
            self->services_.prefs->RequestSidebarWidth(self->sidebar_dip_);
          }
        }
        self->sidebar_->AbortDrag();
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
        if (self->sidebar_->OnWheel(pt, GET_WHEEL_DELTA_WPARAM(wparam))) {
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
      if (self && wparam == PhiSpacesStrip::kHoverCardTimer) {
        self->spaces_->PromoteHoverCard();  // Kills its own timer.
        return 0;
      }
      if (self && wparam == PhiOmniboxSession::kCopyLinkTimer) {
        self->omnibox_->RevertCopyLinkFlash();
        return 0;
      }
      if (self && wparam == PhiSidebarDragSession::kHoverExpandTimer) {
        ::KillTimer(hwnd, PhiSidebarDragSession::kHoverExpandTimer);
        self->sidebar_->OnHoverExpandTimer();
        return 0;
      }
      if (self && wparam == kSlideHoldTimer) {
        // The entering page has not come: ask once more, or slide without.
        if (self->slide_capture_tries_ < kSlideHoldTries) {
          self->StartSlideCapture();
        } else {
          self->ReleaseContentSlide();
        }
        return 0;
      }
      if (self && wparam == PhiSidebarDragSession::kSlideTimer) {
        // The reorder slide: repaint until the current leg settles. The
        // session owns the clock; the timer is the shell's because the
        // window is.
        if (!self->sidebar_->OnSlideTimer()) {
          ::KillTimer(hwnd, PhiSidebarDragSession::kSlideTimer);
        }
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_SIZE:
      if (self) {
        // Band geometry changed under the slide.
        self->spaces_->CancelSwap();
        self->CancelContentSlide();
        self->LayoutContents();
      }
      return 0;
    case WM_DPICHANGED:
      // The window moved to a display at another scale. Windows hands us the
      // new DPI in wparam and the rect it wants in lparam; take both, rebuild
      // the fonts, then re-derive everything (the layout cache sees the new
      // DPI in its inputs). This and the constructor are the ONLY writers of
      // dpi_.
      if (self) {
        if (const UINT dpi = LOWORD(wparam)) {
          self->dpi_ = dpi;
        }
        self->EnsureFont();
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
      // The rows are painted as they are — a capture verb that needs them
      // current runs FlushPendingRebuild first; nothing rebuilds here.
      if (self) {
        HDC dc = reinterpret_cast<HDC>(wparam);
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
        HGDIOBJ old_bmp = nullptr;
        HDC mem = self->AcquireBackBuffer(dc, &old_bmp);
        if (mem) {
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
