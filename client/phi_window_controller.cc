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
constexpr int kSidebarWidth = 240;

// Placeholder sidebar colors (bold, so the client region is unmistakable).
constexpr COLORREF kBandColor = RGB(0x5A, 0x78, 0xFF);
constexpr COLORREF kAccentColor = RGB(0x1A, 0x2A, 0x88);
constexpr COLORREF kInkColor = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kButtonColor = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kButtonInk = RGB(0x2B, 0x2D, 0x31);

// The "+ New Window" button in the sidebar (client rect coords).
constexpr RECT kNewWindowButton = {16, 240, 224, 284};

// Toolbar: back / forward / reload buttons and the URL field.
constexpr RECT kBackButton    = {16, 190, 52, 226};
constexpr RECT kForwardButton = {56, 190, 92, 226};
constexpr RECT kReloadButton  = {96, 190, 132, 226};
constexpr RECT kUrlField      = {16, 146, 224, 182};
constexpr COLORREF kUrlFieldBg = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kUrlInk = RGB(0x2B, 0x2D, 0x31);
constexpr COLORREF kBtnEnabled = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kBtnDisabled = RGB(0x8C, 0xA4, 0xFF);

// Space chips row (between the button and the tab list).
constexpr int kSpacesTop = 296;
constexpr int kSpaceChipH = 32;
constexpr int kSpaceChipW = 64;
constexpr int kSpaceChipGap = 6;
constexpr COLORREF kSpaceActiveBg = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kSpaceActiveInk = RGB(0x2B, 0x2D, 0x31);
constexpr COLORREF kSpaceInk = RGB(0xE6, 0xEA, 0xFF);

// Tab list geometry, below the spaces row.
constexpr int kTabsTop = 374;  // below the chips row AND the create buttons
constexpr int kTabsInset = 12;
constexpr int kTabRowHeight = 30;
constexpr int kTabRowGap = 4;
constexpr int kCloseBoxSize = 16;
constexpr COLORREF kTabActiveBg = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kTabActiveInk = RGB(0x2B, 0x2D, 0x31);
constexpr COLORREF kTabInk = RGB(0xE6, 0xEA, 0xFF);

// Bands used by the two layouts that put navigation above the web content.
constexpr int kTopBarHeight = 44;      // Balanced / Comfortable toolbar row.
constexpr int kHTabStripHeight = 34;   // Comfortable horizontal tab strip.
constexpr int kSpacesRowHeight = 34;   // Comfortable Spaces row.
constexpr int kHTabWidth = 168;
constexpr int kHTabGap = 4;
constexpr int kNavButtonSize = 36;
constexpr int kLayoutButtonHeight = 34;

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
  const int inset = Scaled(kTabsInset);
  const int chip_h = Scaled(kSpaceChipH);
  const int nav = Scaled(kNavButtonSize);
  const int top_bar = Scaled(kTopBarHeight);
  const int strip = Scaled(kHTabStripHeight);
  const int spaces_row = Scaled(kSpacesRowHeight);
  const int pill_h = Scaled(kLayoutButtonHeight);

  if (layout_mode_ == PhiLayoutMode::kPerformance) {
    // Everything in the sidebar. These are the ORIGINAL constants and must stay
    // put: the pixel suites mirror them.
    out.sidebar = {0, 0, sidebar, h};
    out.content = {sidebar, 0, w, h};
    out.url_field = Scaled(kUrlField);
    out.back = Scaled(kBackButton);
    out.forward = Scaled(kForwardButton);
    out.reload = Scaled(kReloadButton);
    out.new_window = Scaled(kNewWindowButton);
    // Tucked into the empty header band above the URL field, clear of every
    // region the pixel suites scan.
    out.layout_button = {inset + Scaled(4), Scaled(100), sidebar - inset - Scaled(4),
                         Scaled(100) + pill_h};
    out.chips_left = inset;
    out.chips_top = Scaled(kSpacesTop);
    out.chips_limit = sidebar - inset;
    out.add_space = {inset, out.chips_top + chip_h + Scaled(6),
                     inset + Scaled(100), out.chips_top + chip_h + Scaled(34)};
    out.add_profile_space = {inset + Scaled(106), out.chips_top + chip_h + Scaled(6),
                             sidebar - inset,
                             out.chips_top + chip_h + Scaled(34)};
    out.tabs_left = inset;
    out.tabs_top = Scaled(kTabsTop);
    out.tabs_limit = h;
    return out;
  }

  if (layout_mode_ == PhiLayoutMode::kBalanced) {
    // Vertical tabs stay in the sidebar; navigation moves to a band across the
    // top of the web content, so the sidebar's own rows shift up to fill the
    // space the toolbar used to occupy.
    out.sidebar = {0, 0, sidebar, h};
    out.top_bar = {sidebar, 0, w, top_bar};
    out.content = {sidebar, top_bar, w, h};

    const int bar_y = (top_bar - nav) / 2;
    int x = sidebar + Scaled(8);
    out.back = {x, bar_y, x + nav, bar_y + nav};
    x += nav + Scaled(4);
    out.forward = {x, bar_y, x + nav, bar_y + nav};
    x += nav + Scaled(4);
    out.reload = {x, bar_y, x + nav, bar_y + nav};
    x += nav + Scaled(8);
    out.url_field = {x, bar_y, (w - Scaled(8) > x) ? (w - Scaled(8)) : (x + 1),
                     bar_y + nav};

    out.layout_button = {inset + Scaled(4), Scaled(20), sidebar - inset - Scaled(4),
                         Scaled(20) + pill_h};
    out.new_window = {inset + Scaled(4), Scaled(64), sidebar - inset - Scaled(4),
                      Scaled(108)};
    out.chips_left = inset;
    out.chips_top = Scaled(120);
    out.chips_limit = sidebar - inset;
    out.add_space = {inset, Scaled(158), inset + Scaled(100), Scaled(186)};
    out.add_profile_space = {inset + Scaled(106), Scaled(158), sidebar - inset,
                             Scaled(186)};
    out.tabs_left = inset;
    out.tabs_top = Scaled(198);
    out.tabs_limit = h;
    return out;
  }

  // Comfortable: the traditional stack — horizontal tabs, then the toolbar,
  // then the Spaces row — with no sidebar at all and the content full width.
  const int spaces_top = strip + top_bar;
  out.top_bar = {0, 0, w, spaces_top + spaces_row};
  out.content = {0, spaces_top + spaces_row, w, h};

  out.tabs_left = inset;
  out.tabs_top = Scaled(2);
  out.tabs_limit = w - inset;
  out.horizontal_tabs = true;

  const int bar_y = strip + (top_bar - nav) / 2;
  int x = inset;
  out.back = {x, bar_y, x + nav, bar_y + nav};
  x += nav + Scaled(4);
  out.forward = {x, bar_y, x + nav, bar_y + nav};
  x += nav + Scaled(4);
  out.reload = {x, bar_y, x + nav, bar_y + nav};
  x += nav + Scaled(8);
  const int right_group = Scaled(150 + 8 + 130);  // Layout button + New Window.
  const int url_right = (w - inset - right_group > x + Scaled(80))
                            ? (w - inset - right_group)
                            : (x + Scaled(80));
  out.url_field = {x, bar_y, url_right, bar_y + nav};
  out.layout_button = {url_right + Scaled(8), bar_y, url_right + Scaled(158),
                       bar_y + pill_h};
  out.new_window = {url_right + Scaled(166), bar_y, url_right + Scaled(296),
                    bar_y + pill_h};

  out.chips_left = inset;
  out.chips_top = spaces_top + Scaled(1);
  out.chips_limit = (w - inset - Scaled(212) > inset)
                        ? (w - inset - Scaled(212))
                        : inset;
  out.add_space = {out.chips_limit + Scaled(6), spaces_top + Scaled(3),
                   out.chips_limit + Scaled(106), spaces_top + Scaled(31)};
  out.add_profile_space = {out.chips_limit + Scaled(112), spaces_top + Scaled(3),
                           out.chips_limit + Scaled(212), spaces_top + Scaled(31)};
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
  layout_button_ = layout.layout_button;
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
                   layout.chips_top + Scaled(kSpaceChipH)};
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
                        Scaled(kSidebarWidth - kTabsInset),
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
  if (host_ && ::PtInRect(&layout.layout_button, pt)) {
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
  if (::PtInRect(&layout.new_window, pt)) {
    if (framework_) {
      framework_->NewWindow(window_id_);
    }
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

void PhiWindowController::PaintChrome(HDC dc,
                                     const Layout& layout,
                                     bool include_child_controls) {
  // Paint only the bands we own. Whatever is left is the content region, and
  // the reparented browser child covers it.
  HBRUSH band = ::CreateSolidBrush(kBandColor);
  if (layout.sidebar.right > layout.sidebar.left) {
    ::FillRect(dc, &layout.sidebar, band);
  }
  if (layout.top_bar.right > layout.top_bar.left) {
    ::FillRect(dc, &layout.top_bar, band);
  }
  ::DeleteObject(band);

  HBRUSH accent = ::CreateSolidBrush(kAccentColor);
  if (layout.sidebar.right > layout.sidebar.left) {
    RECT stripe = {layout.sidebar.right - 3, layout.sidebar.top,
                   layout.sidebar.right, layout.sidebar.bottom};
    ::FillRect(dc, &stripe, accent);
  }
  if (layout.top_bar.bottom > layout.top_bar.top) {
    RECT edge = {layout.top_bar.left, layout.top_bar.bottom - 3,
                 layout.top_bar.right, layout.top_bar.bottom};
    ::FillRect(dc, &edge, accent);
  }
  ::DeleteObject(accent);

  ::SetBkMode(dc, TRANSPARENT);
  // Labels must scale with the boxes drawn for them; the DC's stock font is a
  // fixed-size bitmap font that would look tiny at 200%.
  HGDIOBJ previous_font = ui_font_ ? ::SelectObject(dc, ui_font_) : nullptr;
  if (layout.mode != PhiLayoutMode::kComfortable) {
    // The sidebar header. Comfortable has no sidebar to put it in.
    std::wstring text =
        L"Phi  ·  window " + base::UTF8ToWide(std::to_string(window_id_));
    ::SetTextColor(dc, kInkColor);
    RECT text_rc = {Scaled(16), Scaled(20), Scaled(kSidebarWidth - 12),
                    Scaled((layout.mode == PhiLayoutMode::kPerformance) ? 96
                                                                        : 18)};
    if (text_rc.bottom > text_rc.top) {
      ::DrawTextW(dc, text.c_str(), -1, &text_rc,
                  DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
  }

  // URL field: the box behind the editable EDIT child. The child draws the
  // text on screen; on the capture path (no children rendered) we draw it here
  // so a screenshot still shows the address.
  HBRUSH url_bg = ::CreateSolidBrush(kUrlFieldBg);
  ::FillRect(dc, &layout.url_field, url_bg);
  ::DeleteObject(url_bg);
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
    ::SetTextColor(dc, kUrlInk);
    RECT url_rc = layout.url_field;
    url_rc.left += Scaled(8);
    url_rc.right -= Scaled(8);
    ::DrawTextW(dc, shown.empty() ? L"(no page)" : shown.c_str(), -1, &url_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
  }

  // Back / forward / reload, greyed when unavailable.
  auto draw_button = [&](RECT rect, const wchar_t* glyph, bool enabled) {
    ::SetTextColor(dc, enabled ? kBtnEnabled : kBtnDisabled);
    ::DrawTextW(dc, glyph, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  };
  draw_button(layout.back, L"<", can_go_back_);
  draw_button(layout.forward, L">", can_go_forward_);
  draw_button(layout.reload, nav_loading_ ? L"x" : L"R", true);

  auto draw_pill = [&](RECT r, const wchar_t* text) {
    if (r.right <= r.left) {
      return;
    }
    HBRUSH b = ::CreateSolidBrush(kButtonColor);
    ::FillRect(dc, &r, b);
    ::DeleteObject(b);
    ::SetTextColor(dc, kButtonInk);
    ::DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  };

  // "+ New Window" — a click spawns a second shell window via the framework.
  draw_pill(layout.new_window, L"+  New Window");
  // The layout switch, showing what a click would give you next.
  const wchar_t* layout_label = L"Layout: Performance";
  if (layout.mode == PhiLayoutMode::kBalanced) {
    layout_label = L"Layout: Balanced";
  } else if (layout.mode == PhiLayoutMode::kComfortable) {
    layout_label = L"Layout: Comfortable";
  }
  draw_pill(layout.layout_button, layout_label);

  // Space chips: EVERY Space (not only those open here) so any can be entered.
  for (const SpaceChip& chip : chips_) {
    const bool active = (chip.id == active_space_);
    if (active) {
      HBRUSH b = ::CreateSolidBrush(kSpaceActiveBg);
      ::FillRect(dc, &chip.bounds, b);
      ::DeleteObject(b);
    }
    ::SetTextColor(dc, active ? kSpaceActiveInk : kSpaceInk);
    ::DrawTextW(dc, chip.label.c_str(), -1, const_cast<RECT*>(&chip.bounds),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  // Create-Space buttons: same profile as the current Space, or a new profile.
  draw_pill(layout.add_space, L"+ Space");
  draw_pill(layout.add_profile_space, L"+ Space & profile");

  // Tab rows (a vertical list, or cells along the strip in Comfortable),
  // driven by live framework state: the active tab gets a white chip, each row
  // a close box.
  for (const TabRow& row : tabs_) {
    if (row.active) {
      HBRUSH chip = ::CreateSolidBrush(kTabActiveBg);
      ::FillRect(dc, &row.bounds, chip);
      ::DeleteObject(chip);
    }
    ::SetTextColor(dc, row.active ? kTabActiveInk : kTabInk);
    RECT title_rc = row.bounds;
    title_rc.left += Scaled(8);
    title_rc.right = row.close_box.left - Scaled(4);
    ::DrawTextW(dc, row.title.c_str(), -1, &title_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // Close affordance: a small x.
    ::DrawTextW(dc, L"x", -1, const_cast<RECT*>(&row.close_box),
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
