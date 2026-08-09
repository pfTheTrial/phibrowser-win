// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_window_controller.h"

#include <windowsx.h>

#include <optional>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"

namespace phi_client {

namespace {

constexpr wchar_t kShellClassName[] = L"PhiClientShell";

// --- Metrics -----------------------------------------------------------------
// Design units (points at 100%), taken from the Mac client so the two look the
// same: SidebarViewController's stack, SidebarHeaderView's header and
// SpacesStripView / SidebarBottomBar's row heights.
constexpr int kSidebarWidth = 240;
constexpr int kEdgeInset = 10;        // Sidebar content inset.
constexpr int kNavRowTop = 8;         // SidebarHeaderView.defaultSidebarButtonTopOffset
constexpr int kNavButton = 24;        // Every header button is 24x24.
constexpr int kNavRowRightInset = 9;  // stackView right inset.
constexpr int kSpacesStripTop = 40;   // navRow.bottom + 8
constexpr int kSpacesStripHeight = 32;  // SpacesStripView.sidebarHeight
constexpr int kSpaceChipHeight = 24;  // 4pt above and below inside the strip.
constexpr int kAddressBarTop = 84;    // spacesStrip.bottom + 12
constexpr int kAddressBarHeight = 32;
constexpr int kHeaderHeight = 120;    // 80 base + 32 strip + 8 gap
constexpr int kHeaderSpacer = 5;
constexpr int kFavoritesSpacer = 3;
constexpr int kTabRowHeight = 36;     // TabGroupCellView.memberRowHeight
constexpr int kBottomBarHeight = 24;  // SidebarBottomBarState.singleRowHeight
constexpr int kBottomSpacer = 8;
constexpr int kCornerRadius = 8;
// The web content is an inset rounded card floating on the tint, not a flush
// pane — the single most recognisable thing about Phi's window.
constexpr int kContentMargin = 8;
constexpr int kContentRadius = 10;
constexpr int kFaviconSize = 16;
constexpr int kNewTabRowHeight = 32;

// Bands used by the two layouts that put navigation above the web content.
constexpr int kTopBarHeight = 44;
constexpr int kHTabStripHeight = 34;
constexpr int kHTabWidth = 168;
constexpr int kHTabGap = 4;
constexpr int kSpaceChipW = 64;
constexpr int kSpaceChipGap = 6;
constexpr int kCloseBoxSize = 16;
constexpr int kTabRowGap = 0;

// --- Palette (light) ---------------------------------------------------------
// From Assets.xcassets/ColorSet: primary-label 85% black, common-border 8%
// black, sidebar-tab-selected white. The sidebar ground stands in for the Mac's
// material + per-Space gradient tint, which GDI cannot reproduce.
// The window ground is the SPACE'S COLOUR — a vertical gradient, as in the Mac
// screenshots. This is the default (mint) Space tint; per-Space colours arrive
// with the Space model.
constexpr COLORREF kTintTop = RGB(0xC9, 0xE7, 0xE3);
constexpr COLORREF kTintBottom = RGB(0xA4, 0xD9, 0xD4);
constexpr COLORREF kRowSelectedBg = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kLabel = RGB(0x26, 0x26, 0x26);
constexpr COLORREF kLabelDim = RGB(0x8A, 0x8A, 0x8E);
constexpr COLORREF kBorder = RGB(0xE2, 0xE2, 0xE4);
constexpr COLORREF kFieldBg = RGB(0xFF, 0xFF, 0xFF);

// The stock EDIT window proc, saved when the first address field is
// subclassed. One per process — every EDIT shares the class proc.
WNDPROC g_edit_base_proc = nullptr;

ATOM EnsureShellClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PhiWindowController::ShellWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
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
    built = ::CreateFontIndirectW(&metrics.lfMessageFont);
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

  // The nav row, laid out inside `band` (the sidebar in the vertical layouts,
  // the top bar in Comfortable): two leading buttons, then the back / forward /
  // reload group trailing — SidebarHeaderView's arrangement.
  auto place_nav_row = [&](int left, int right, int top) {
    int x = left + inset;
    out.sidebar_toggle = {x, top, x + btn, top + btn};
    x += btn + Scaled(4);
    out.search_tabs = {x, top, x + btn, top + btn};
    int r = right - Scaled(kNavRowRightInset);
    out.reload = {r - btn, top, r, top + btn};
    r -= btn + Scaled(4);
    out.forward = {r - btn, top, r, top + btn};
    r -= btn + Scaled(4);
    out.back = {r - btn, top, r, top + btn};
  };

  // The Spaces strip and its two trailing "+" chips (same profile / new
  // profile). The Mac opens a Create-Space panel from one "+"; until that panel
  // exists both destinations stay directly reachable.
  auto place_spaces = [&](int left, int right, int top) {
    out.spaces_strip = {left, top, right, top + Scaled(kSpacesStripHeight)};
    const int chip_top = top + Scaled((kSpacesStripHeight - kSpaceChipHeight) / 2);
    out.chips_left = left + inset;
    out.chips_top = chip_top;
    const int add_w = Scaled(kSpaceChipHeight);
    out.add_profile_space = {right - inset - add_w, chip_top, right - inset,
                             chip_top + Scaled(kSpaceChipHeight)};
    out.add_space = {out.add_profile_space.left - Scaled(4) - add_w, chip_top,
                     out.add_profile_space.left - Scaled(4),
                     chip_top + Scaled(kSpaceChipHeight)};
    out.chips_limit = out.add_space.left - Scaled(4);
  };

  // The bottom bar: chat / downloads / memory leading (placeholders, as on Mac
  // but not yet wired), settings trailing.
  auto place_bottom_bar = [&](int left, int right) {
    const int bar_h = Scaled(kBottomBarHeight);
    const int top = h - Scaled(kBottomSpacer) - bar_h;
    out.bottom_bar = {left, top, right, top + bar_h};
    int x = left + inset;
    out.chat = {x, top, x + btn, top + bar_h};
    x += btn + Scaled(6);
    out.downloads = {x, top, x + btn, top + bar_h};
    x += btn + Scaled(6);
    out.memory = {x, top, x + btn, top + bar_h};
    out.settings = {right - inset - btn, top, right - inset, top + bar_h};
  };

  if (layout_mode_ == PhiLayoutMode::kComfortable) {
    // Horizontal tabs on top, then the nav row; no sidebar, so the Spaces strip
    // and the bottom bar's buttons ride along the toolbar row.
    const int strip = Scaled(kHTabStripHeight);
    const int bar = Scaled(kTopBarHeight);
    out.top_bar = {0, 0, w, strip + bar};
    const int m = Scaled(kContentMargin);
    out.content = {m, strip + bar, w - m, h - m};
    out.tabs_left = inset;
    out.tabs_top = Scaled(2);
    out.tabs_limit = w - inset;
    out.horizontal_tabs = true;

    const int row_top = strip + (bar - btn) / 2;
    place_nav_row(0, w, row_top);
    // Address bar between the leading buttons and the trailing nav group.
    const int url_left = out.search_tabs.right + Scaled(8);
    const int url_right = out.back.left - Scaled(8);
    out.url_field = {url_left, row_top, (url_right > url_left + Scaled(80))
                                            ? url_right
                                            : url_left + Scaled(80),
                     row_top + btn};
    // No room for a Spaces strip in one row: chips follow the tab strip.
    place_spaces(0, w, 0);
    out.spaces_strip = {};
    out.chips_top = -Scaled(1000);  // Off-screen; Comfortable hides the strip.
    out.chips_limit = 0;
    out.add_space = {};
    out.add_profile_space = {};
    place_bottom_bar(0, w);
    out.bottom_bar = {};
    out.chat = out.downloads = out.memory = {};
    out.settings = {w - inset - btn, Scaled(4), w - inset, Scaled(4) + btn};
    return out;
  }

  // Vertical layouts: the Mac sidebar stack, top to bottom.
  out.sidebar = {0, 0, sidebar, h};
  place_nav_row(0, sidebar, Scaled(kNavRowTop));
  place_spaces(0, sidebar, Scaled(kSpacesStripTop));

  if (layout_mode_ == PhiLayoutMode::kBalanced) {
    // The address bar moves out of the sidebar and across the content's top.
    const int bar = Scaled(kTopBarHeight);
    out.top_bar = {sidebar, 0, w, bar};
    const int m = Scaled(kContentMargin);
    out.content = {sidebar + m, bar, w - m, h - m};
    const int row_top = (bar - btn) / 2;
    int r = w - Scaled(kNavRowRightInset);
    out.reload = {r - btn, row_top, r, row_top + btn};
    r -= btn + Scaled(4);
    out.forward = {r - btn, row_top, r, row_top + btn};
    r -= btn + Scaled(4);
    out.back = {r - btn, row_top, r, row_top + btn};
    out.url_field = {sidebar + Scaled(8), row_top, out.back.left - Scaled(8),
                     row_top + btn};
    // Header ends after the Spaces strip; the tab list starts higher.
    out.tabs_top = out.spaces_strip.bottom + Scaled(kHeaderSpacer +
                                                    kFavoritesSpacer);
  } else {
    const int m = Scaled(kContentMargin);
    out.content = {sidebar + m, m, w - m, h - m};
    out.url_field = {inset, Scaled(kAddressBarTop), sidebar - inset,
                     Scaled(kAddressBarTop + kAddressBarHeight)};
    out.tabs_top = Scaled(kHeaderHeight + kHeaderSpacer + kFavoritesSpacer);
  }

  // The pinned-tab band sits between the header and the tab list. Empty until
  // favourites exist, exactly as on Mac (which mounts a zero-height container).
  out.favorites = {inset, out.tabs_top, sidebar - inset, out.tabs_top};
  out.new_tab_row = {inset, out.tabs_top, sidebar - inset,
                     out.tabs_top + Scaled(kNewTabRowHeight)};
  out.tabs_top = out.new_tab_row.bottom + Scaled(2);
  place_bottom_bar(0, sidebar);
  out.tabs_left = inset;
  out.tabs_limit = out.bottom_bar.top - Scaled(kFavoritesSpacer);
  return out;
}

RECT PhiWindowController::content_rect() {
  return ComputeLayout().content;
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
    RECT frame = {wr.left - Scaled(kSidebarWidth), wr.top, wr.right, wr.bottom};
    if (dpi) {
      ::AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    } else {
      ::AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
    }
    x = frame.left;
    y = frame.top;
    w = frame.right - frame.left;
    h = frame.bottom - frame.top;
  }

  shell_hwnd_ = ::CreateWindowExW(
      0, kShellClassName, L"Phi", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w,
      h, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
  if (!shell_hwnd_) {
    LOG(ERROR) << "[PhiClient] shell CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }

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
  const int width = box.right - box.left - Scaled(12);
  ::MoveWindow(url_field_, box.left + Scaled(6),
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
  ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
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
    // Rounded corners on the content itself, so the page reads as a card
    // floating on the Space tint. A window region clips the child for real —
    // there is no other way to round a window we do not paint.
    if (w > 0 && h > 0) {
      const int radius = Scaled(kContentRadius) * 2;
      HRGN rgn = ::CreateRoundRectRgn(0, 0, w + 1, h + 1, radius, radius);
      ::SetWindowRgn(browser_hwnd_, rgn, TRUE);  // Window owns `rgn` now.
    }
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
  if (previous && previous != browser_hwnd && ::IsWindow(previous)) {
    ::ShowWindow(previous, SW_HIDE);
  }
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
  if (browser_hwnd_ && browser_hwnd_ != hwnd_it->second &&
      ::IsWindow(browser_hwnd_)) {
    ::ShowWindow(browser_hwnd_, SW_HIDE);
  }
  browser_hwnd_ = hwnd_it->second;
  active_space_ = space_id;
  active_browser_ = it->second;
  LayoutContents();  // Also re-shows the newly visible child.
  FocusActiveBrowser();
  RefreshTabs();
  return true;
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
  add_profile_space_button_ = layout.add_profile_space;
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
    if (x + Scaled(kSpaceChipW) > layout.chips_limit) {
      break;  // Row is full; a scroller is future work.
    }
    SpaceChip chip;
    chip.id = *id;
    chip.label = L"S" + base::UTF8ToWide(*id);
    chip.open = HasSpace(*id);
    chip.bounds = {x, layout.chips_top, x + Scaled(kSpaceChipW),
                   layout.chips_top + Scaled(kSpaceChipHeight)};
    chips_.push_back(std::move(chip));
    x += Scaled(kSpaceChipW + kSpaceChipGap);
  }
}

void PhiWindowController::RebuildTabs() {
  RebuildNavState();
  RebuildChips();
  tabs_.clear();
  if (framework_) {
    std::optional<base::Value> parsed = base::JSONReader::Read(
        framework_->ListTabsJson(active_browser_ ? active_browser_ : window_id_), base::JSON_PARSE_RFC);
    if (parsed && parsed->is_list()) {
      const Layout layout = ComputeLayout();
      int x = layout.tabs_left;
      int y = layout.tabs_top;
      for (const base::Value& v : parsed->GetList()) {
        if (!v.is_dict()) {
          continue;
        }
        const base::DictValue& d = v.GetDict();
        TabRow row;
        row.index = d.FindInt("index").value_or(0);
        row.active = d.FindBool("active").value_or(false);
        const std::string* title = d.FindString("title");
        const std::string* url = d.FindString("url");
        std::string label = title && !title->empty()
                                ? *title
                                : (url ? *url : std::string("(untitled)"));
        row.title = base::UTF8ToWide(label);
        const int close_box = Scaled(kCloseBoxSize);
        if (layout.horizontal_tabs) {
          if (x + Scaled(kHTabWidth) > layout.tabs_limit) {
            break;  // Strip is full; overflow handling is future work.
          }
          row.bounds = {x, y, x + Scaled(kHTabWidth),
                        y + Scaled(kHTabStripHeight - 4)};
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
        tabs_.push_back(std::move(row));
      }
    }
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
  if (host_ && ::PtInRect(&layout.settings, pt)) {
    // Cycles Performance -> Balanced -> Comfortable. The host applies it to
    // every slot, so two windows never disagree about the layout.
    PhiLayoutMode next = PhiLayoutMode::kPerformance;
    if (layout_mode_ == PhiLayoutMode::kPerformance) {
      next = PhiLayoutMode::kBalanced;
    } else if (layout_mode_ == PhiLayoutMode::kBalanced) {
      next = PhiLayoutMode::kComfortable;
    }
    host_->RequestLayoutMode(next);
    return;
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
  if (host_ && ::PtInRect(&layout.add_profile_space, pt)) {
    host_->RequestCreateSpace(window_id_, /*new_profile=*/true);
    RefreshTabs();
    return;
  }
  for (const TabRow& row : tabs_) {
    if (!::PtInRect(&row.bounds, pt)) {
      continue;
    }
    if (framework_) {
      if (::PtInRect(&row.close_box, pt)) {
        framework_->CloseTab(active_browser_ ? active_browser_ : window_id_, row.index);
      } else {
        framework_->ActivateTab(active_browser_ ? active_browser_ : window_id_, row.index);
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

}  // namespace

void PhiWindowController::PaintChrome(HDC dc,
                                     const Layout& layout,
                                     bool include_child_controls) {
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

  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = ui_font_ ? ::SelectObject(dc, ui_font_) : nullptr;

  // Nav row. Glyphs stand in for the Mac's SF Symbols.
  DrawGlyph(dc, layout.sidebar_toggle, L"\u25A3", kLabel);   // sidebar toggle
  DrawGlyph(dc, layout.search_tabs, L"\u2315", kLabel);      // search tabs
  DrawGlyph(dc, layout.back, L"\u2039", can_go_back_ ? kLabel : kLabelDim);
  DrawGlyph(dc, layout.forward, L"\u203A",
            can_go_forward_ ? kLabel : kLabelDim);
  DrawGlyph(dc, layout.reload, nav_loading_ ? L"\u00D7" : L"\u21BB", kLabel);

  // Address field: a white rounded box with a hairline border. The EDIT child
  // draws the text on screen; on the capture path (no children rendered) we
  // draw it here so a screenshot still shows the address.
  StrokeRounded(dc, layout.url_field, kFieldBg, kBorder, Scaled(kCornerRadius));
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
    ::SetTextColor(dc, kLabel);
    RECT url_rc = layout.url_field;
    url_rc.left += Scaled(10);
    url_rc.right -= Scaled(10);
    ::DrawTextW(dc, shown.empty() ? L"Search or enter address" : shown.c_str(),
                -1, &url_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
  }

  // Space chips, then the two "+" chips that create one.
  for (const SpaceChip& chip : chips_) {
    const bool active = (chip.id == active_space_);
    if (active) {
      FillRounded(dc, chip.bounds, kRowSelectedBg, Scaled(kCornerRadius));
    }
    ::SetTextColor(dc, active ? kLabel : kLabelDim);
    ::DrawTextW(dc, chip.label.c_str(), -1, const_cast<RECT*>(&chip.bounds),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  DrawGlyph(dc, layout.add_space, L"+", kLabelDim);
  DrawGlyph(dc, layout.add_profile_space, L"\u29C9", kLabelDim);

  // Tab rows: the active one gets the white rounded chip, each row a close box.
  for (const TabRow& row : tabs_) {
    if (row.active) {
      FillRounded(dc, row.bounds, kRowSelectedBg, Scaled(kCornerRadius));
    }
    // Favicon slot. A neutral square until favicons are plumbed through —
    // the row reads like the Mac one either way.
    RECT favicon = {row.bounds.left + Scaled(10), 0,
                    row.bounds.left + Scaled(10) + Scaled(kFaviconSize), 0};
    const int mid = (row.bounds.top + row.bounds.bottom) / 2;
    favicon.top = mid - Scaled(kFaviconSize) / 2;
    favicon.bottom = favicon.top + Scaled(kFaviconSize);
    FillRounded(dc, favicon, kBorder, Scaled(4));

    ::SetTextColor(dc, row.active ? kLabel : kLabelDim);
    RECT title_rc = row.bounds;
    title_rc.left = favicon.right + Scaled(8);
    title_rc.right = row.close_box.left - Scaled(4);
    ::DrawTextW(dc, row.title.c_str(), -1, &title_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    DrawGlyph(dc, row.close_box, L"\u00D7", kLabelDim);
  }

  // "+ New Tab", the row that heads the Mac sidebar's tab list.
  if (layout.new_tab_row.right > layout.new_tab_row.left) {
    RECT row = layout.new_tab_row;
    row.left += Scaled(10);
    ::SetTextColor(dc, kLabelDim);
    ::DrawTextW(dc, L"+   New Tab", -1, &row,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  }

  // Bottom bar. Chat / downloads / memory are PLACEHOLDERS — drawn so the
  // window reads like the Mac one, wired to nothing yet.
  DrawGlyph(dc, layout.chat, L"\u25CB", kLabelDim);
  DrawGlyph(dc, layout.downloads, L"\u2913", kLabelDim);
  DrawGlyph(dc, layout.memory, L"\u25C7", kLabelDim);
  DrawGlyph(dc, layout.settings, L"\u2699", kLabelDim);

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
        self->PaintChrome(dc, self->ComputeLayout(),
                          /*include_child_controls=*/false);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
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
