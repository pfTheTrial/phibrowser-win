// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_reader_panel.h"

#include <optional>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_reader.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

constexpr wchar_t kReaderPanelClassName[] = L"PhiReaderPanel";

// The page card's corner radius (PhiWindowController's kContentRadius,
// webContentContainerCornerRadius on macOS 26) — the reading surface wears
// the same corners as the page it covers.
constexpr int kContentRadius = 14;

ATOM EnsureClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PhiReaderPanel::WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kReaderPanelClassName;
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

}  // namespace

PhiReaderPanel::PhiReaderPanel(int64_t window_id,
                               HWND browser_hwnd,
                               int64_t origin_window_id,
                               int64_t origin_tab_id,
                               PhiWindowController* origin,
                               phi_framework::PhiFramework* framework,
                               PhiCoordinator* coordinator)
    : window_id_(BrowserId{window_id}),
      browser_hwnd_(browser_hwnd),
      origin_window_id_(BrowserId{origin_window_id}),
      origin_tab_id_(origin_tab_id),
      origin_(origin),
      framework_(framework),
      coordinator_(coordinator) {
  EnsureClass();
  HWND shell = origin_ ? origin_->shell_hwnd() : nullptr;
  if (!shell) {
    LOG(ERROR) << "[PhiClient] reader panel has no origin shell";
    return;
  }
  dpi_ = ::GetDpiForWindow(shell);

  // A child of the origin's shell, above the hosted browser — the peek
  // panel's trick, so the reader clips into the content card and travels
  // with the window.
  hwnd_ = ::CreateWindowExW(0, kReaderPanelClassName, L"",
                            WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, shell,
                            nullptr, ::GetModuleHandleW(nullptr), this);
  if (!hwnd_) {
    LOG(ERROR) << "[PhiClient] reader panel CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }

  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    LONG_PTR style = ::GetWindowLongPtrW(browser_hwnd_, GWL_STYLE);
    style &= ~(WS_POPUP | WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME |
               WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= WS_CHILD | WS_CLIPSIBLINGS;
    ::SetWindowLongPtrW(browser_hwnd_, GWL_STYLE, style);
    LONG_PTR ex = ::GetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE);
    ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
            WS_EX_STATICEDGE | WS_EX_APPWINDOW);
    ::SetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE, ex);
    ::SetParent(browser_hwnd_, hwnd_);
    ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_FRAMECHANGED);
  }

  Reposition();
  RefreshState();
  // Above the shell's hosted browser child, or the page would occlude it.
  ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
  if (origin_) {
    origin_->RaiseCornerCaps();
  }
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetFocus(browser_hwnd_);
  }
}

SidebarSurface PhiReaderPanel::kind() const {
  return SidebarSurface::kReader;
}
int64_t PhiReaderPanel::window_id() const {
  return window_id_.value;
}
int64_t PhiReaderPanel::origin_window_id() const {
  return origin_window_id_.value;
}
int64_t PhiReaderPanel::origin_tab_id() const {
  return origin_tab_id_;
}
HWND PhiReaderPanel::hwnd() const {
  return hwnd_;
}
void PhiReaderPanel::BindSurfaceTab(int64_t surface_tab_id) {
  surface_tab_id_ = surface_tab_id;
}

PhiReaderPanel::~PhiReaderPanel() {
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetParent(browser_hwnd_, nullptr);
  }
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd_);
  }
}

void PhiReaderPanel::Reposition() {
  if (!hwnd_ || !::IsWindow(hwnd_) || !origin_) {
    return;
  }
  // The whole CARD, not just the page below the bar — on the Mac the reader
  // covers `pageCardView`, which is the container the navigation bar lives
  // INSIDE (see WebContentViewController's hierarchy). Covering only the page
  // would leave the panel's top corners in the middle of the card, square
  // against a card whose rounding is up at the bar.
  const RECT content = origin_->card_rect();
  const int width = content.right - content.left;
  const int height = content.bottom - content.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  ::SetWindowPos(hwnd_, nullptr, content.left, content.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  PositionContent();
  ApplyCardShape();
  ::InvalidateRect(hwnd_, nullptr, FALSE);
  // The caps were placed for the card this panel now covers; re-assert them
  // over it so the reading surface wears the same rounded corners.
  if (origin_) {
    origin_->RaiseCornerCaps();
  }
}

void PhiReaderPanel::ApplyCardShape() {
  if (!hwnd_ || !::IsWindow(hwnd_)) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  if (client.right <= 0 || client.bottom <= 0) {
    return;
  }
  // The page card's radius (kContentRadius), so the reading surface wears the
  // same corners as the page it covers. The shell rounds ITS card with
  // sibling corner caps because DirectComposition ignores a region on the
  // browser child — but the reader's browser is a child of THIS window, so
  // the region that clips it is this one's.
  bool round_top = false;
  bool round_bottom = false;
  if (origin_) {
    origin_->CardRoundedCorners(&round_top, &round_bottom);
  }
  if (!round_top && !round_bottom) {
    ::SetWindowRgn(hwnd_, nullptr, TRUE);  // Square, as the card is.
    return;
  }
  // CreateRoundRectRgn takes the ELLIPSE size, so it is twice the radius.
  const int ellipse = ::MulDiv(kContentRadius, dpi_, 96) * 2;
  // Rounding only the bottom pair is the status bubble's trick: oversize the
  // rect past the top edge so those corners' curves fall outside the window.
  const int top = round_top ? 0 : -ellipse;
  HRGN shape = ::CreateRoundRectRgn(0, top, client.right + 1,
                                    client.bottom + 1, ellipse, ellipse);
  ::SetWindowRgn(hwnd_, shape, TRUE);  // The window owns it now.
}

void PhiReaderPanel::Raise() {
  if (!hwnd_ || !::IsWindow(hwnd_) || !::IsWindowVisible(hwnd_)) {
    return;
  }
  ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void PhiReaderPanel::SetVisibleForActiveTab(int64_t active_tab_id) {
  if (!hwnd_ || !::IsWindow(hwnd_)) {
    return;
  }
  const bool mine = active_tab_id == origin_tab_id_;
  ::ShowWindow(hwnd_, mine ? SW_SHOWNA : SW_HIDE);
  if (mine) {
    ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (origin_) {
      origin_->RaiseCornerCaps();
    }
  }
}

void PhiReaderPanel::RefreshState() {
  if (!framework_ || expanded_) {
    return;
  }
  // Read the STRIP, not the nav state: a panel whose surface tab is being
  // removed (reader.close, or the reading page's own X) has an empty strip
  // and reports a placeholder page, which is not a navigation and must not
  // be mistaken for one — the browser is about to close on its own.
  std::optional<base::Value> tabs = base::JSONReader::Read(
      framework_->ListTabsJson(window_id_.value), base::JSON_PARSE_RFC);
  if (!tabs || !tabs->is_list() || tabs->GetList().size() != 1) {
    return;
  }
  const base::DictValue* row = tabs->GetList().front().GetIfDict();
  if (!row) {
    return;
  }
  const std::string* surface = row->FindString("surface");
  const bool is_surface = surface && *surface == "reader";
  if (!surface_tab_id_) {
    // The framework panelled the surface itself (the insertion divert), so
    // nobody bound it here: adopt the reader page standing for our origin.
    if (is_surface &&
        row->FindInt("originTabId").value_or(0) == origin_tab_id_) {
      surface_tab_id_ = row->FindInt("id").value_or(0);
    }
    return;
  }
  if (row->FindInt("id").value_or(0) != surface_tab_id_) {
    // Not the tab that was moved in: the surface has gone and this browser
    // is on its way out with a placeholder page. Nothing to hand back.
    return;
  }
  const std::string* url = row->FindString("url");
  if (!url || url->empty()) {
    return;
  }
  url_ = base::UTF8ToWide(*url);
  if (is_surface || *url == "about:blank") {
    return;
  }
  // A link clicked inside the reader. The reading surface is not a place to
  // browse from, so the destination becomes a normal tab in the origin
  // window and the panel goes away. Guarded so the teardown runs once.
  expanded_ = true;
  LOG(INFO) << "[PhiClient] reader panel " << window_id_.value
            << " navigated away — expanding into a tab";
  if (coordinator_) {
    coordinator_->ExpandReaderPanelIntoTab(window_id_.value, *url);
  }
}

void PhiReaderPanel::PositionContent() {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, client.right, client.bottom,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void PhiReaderPanel::Paint() {
  PAINTSTRUCT ps = {};
  HDC dc = ::BeginPaint(hwnd_, &ps);
  if (!dc) {
    return;
  }
  // Only ever seen in the instant before the hosted page covers it; painting
  // the window's own ground stops that instant reading as a white flash.
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  // The ORIGIN SLOT's palette, not its browser's: the two numbers only
  // coincide until that slot switches Space.
  const PhiPalette palette =
      coordinator_ && origin_
          ? coordinator_->PaletteForSlot(origin_->slot().value)
          : kFallback;
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  HBRUSH ground = ::CreateSolidBrush(palette.ground_top);
  ::FillRect(dc, &client, ground);
  ::DeleteObject(ground);
  ::EndPaint(hwnd_, &ps);
}

LRESULT PhiReaderPanel::Handle(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_PAINT:
      Paint();
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      PositionContent();
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd_, msg, wparam, lparam);
}

LRESULT CALLBACK PhiReaderPanel::WndProc(HWND hwnd,
                                         UINT msg,
                                         WPARAM wparam,
                                         LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    auto* self = static_cast<PhiReaderPanel*>(create->lpCreateParams);
    if (self) {
      self->hwnd_ = hwnd;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiReaderPanel*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) {
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->Handle(msg, wparam, lparam);
}

}  // namespace phi_client
