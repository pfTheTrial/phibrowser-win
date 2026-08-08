// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_window_controller.h"

#include <windowsx.h>

#include <string>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"

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

PhiWindowController::PhiWindowController(int64_t window_id,
                                        HWND browser_hwnd,
                                        phi_framework::PhiFramework* framework)
    : window_id_(window_id),
      browser_hwnd_(browser_hwnd),
      framework_(framework) {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    LOG(ERROR) << "[PhiClient] adopt: invalid browser HWND for window "
               << window_id_;
    return;
  }
  EnsureShellClass();

  // Create the client shell top-level at the browser window's current bounds,
  // so the transition from Chromium's own frame to ours is in-place.
  RECT wr = {};
  ::GetWindowRect(browser_hwnd_, &wr);
  int x = wr.left, y = wr.top;
  int w = wr.right - wr.left, h = wr.bottom - wr.top;
  if (w <= 0 || h <= 0) {
    w = 1200;
    h = 800;
    x = CW_USEDEFAULT;
    y = CW_USEDEFAULT;
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
    ::DestroyWindow(shell_hwnd_);
  }
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
  RECT rc = {};
  ::GetClientRect(shell_hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    // Content fills everything right of the sidebar.
    ::SetWindowPos(browser_hwnd_, nullptr, kSidebarWidth, 0,
                   (w > kSidebarWidth) ? (w - kSidebarWidth) : 0, h,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
}

void PhiWindowController::PaintSidebar(HDC dc, const RECT& client) {
  RECT bar = {0, 0, kSidebarWidth, client.bottom};
  HBRUSH band = ::CreateSolidBrush(kBandColor);
  ::FillRect(dc, &bar, band);
  ::DeleteObject(band);

  RECT stripe = {kSidebarWidth - 3, 0, kSidebarWidth, client.bottom};
  HBRUSH accent = ::CreateSolidBrush(kAccentColor);
  ::FillRect(dc, &stripe, accent);
  ::DeleteObject(accent);

  std::wstring text = L"Phi client shell\n(Option B: client owns window,\n"
                      L"Chromium content reparented)\n\nwindowId " +
                      base::UTF8ToWide(std::to_string(window_id_));
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, kInkColor);
  RECT text_rc = {16, 20, kSidebarWidth - 12, kNewWindowButton.top - 8};
  ::DrawTextW(dc, text.c_str(), -1, &text_rc, DT_LEFT | DT_TOP | DT_WORDBREAK);

  // "+ New Window" button — proves the slot model (a click spawns a second
  // shell window via the framework).
  RECT btn = kNewWindowButton;
  HBRUSH btn_brush = ::CreateSolidBrush(kButtonColor);
  ::FillRect(dc, &btn, btn_brush);
  ::DeleteObject(btn_brush);
  ::SetTextColor(dc, kButtonInk);
  ::DrawTextW(dc, L"+  New Window", -1, &btn,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
    case WM_MOUSEACTIVATE:
      // Let clicks activate without being eaten, and drive Chromium's own
      // activation across the boundary so input/focus/IME reach the page.
      if (self && self->browser_hwnd_ && ::IsWindow(self->browser_hwnd_)) {
        ::SetFocus(self->browser_hwnd_);
        if (self->framework_) {
          self->framework_->FocusBrowserWindow(self->window_id_);
        }
      }
      return MA_ACTIVATE;
    case WM_ACTIVATE:
    case WM_SETFOCUS:
      // Activation/focus arriving at the shell top-level must be forwarded to
      // the child: as a WS_CHILD, Chromium's window reports IsTopLevelWindow()
      // == false and skips its own activation path.
      if (self && self->browser_hwnd_ && ::IsWindow(self->browser_hwnd_) &&
          !(msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
        ::SetFocus(self->browser_hwnd_);
        if (self->framework_) {
          self->framework_->FocusBrowserWindow(self->window_id_);
        }
      }
      return 0;
    case WM_LBUTTONDOWN:
      if (self && self->framework_) {
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (::PtInRect(&kNewWindowButton, pt)) {
          self->framework_->NewWindow(self->window_id_);
          return 0;
        }
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_SIZE:
      if (self) {
        self->LayoutContents();
      }
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        self->PaintSidebar(dc, rc);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_CLOSE:
      // Route through Chromium's normal close path (unload handlers, session
      // bookkeeping). When the Browser actually goes away the framework fires
      // OnBrowserWindowClosing and the coordinator destroys this controller —
      // the shell window dies THEN, not here.
      if (self && self->framework_) {
        self->framework_->CloseBrowserWindow(self->window_id_);
      } else {
        ::DestroyWindow(hwnd);
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace phi_client
