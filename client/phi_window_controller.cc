// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_window_controller.h"

#include <windowsx.h>

#include <optional>
#include <string>

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

// Tab list geometry, below the button.
constexpr int kTabsTop = 304;
constexpr int kTabsInset = 12;
constexpr int kTabRowHeight = 30;
constexpr int kTabRowGap = 4;
constexpr int kCloseBoxSize = 16;
constexpr COLORREF kTabActiveBg = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kTabActiveInk = RGB(0x2B, 0x2D, 0x31);
constexpr COLORREF kTabInk = RGB(0xE6, 0xEA, 0xFF);

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

void PhiWindowController::RefreshTabs() {
  RebuildTabs();
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
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
      framework_->NavStateJson(window_id_), base::JSON_PARSE_RFC);
  if (!st) {
    return;
  }
  if (const std::string* u = st->FindString("url")) {
    nav_url_ = base::UTF8ToWide(*u);
  }
  can_go_back_ = st->FindBool("canGoBack").value_or(false);
  can_go_forward_ = st->FindBool("canGoForward").value_or(false);
  nav_loading_ = st->FindBool("loading").value_or(false);
}

void PhiWindowController::RebuildTabs() {
  RebuildNavState();
  tabs_.clear();
  if (framework_) {
    std::optional<base::Value> parsed = base::JSONReader::Read(
        framework_->ListTabsJson(window_id_), base::JSON_PARSE_RFC);
    if (parsed && parsed->is_list()) {
      int y = kTabsTop;
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
        row.bounds = {kTabsInset, y, kSidebarWidth - kTabsInset,
                      y + kTabRowHeight};
        row.close_box = {row.bounds.right - kCloseBoxSize - 4,
                         y + (kTabRowHeight - kCloseBoxSize) / 2,
                         row.bounds.right - 4,
                         y + (kTabRowHeight + kCloseBoxSize) / 2};
        tabs_.push_back(std::move(row));
        y += kTabRowHeight + kTabRowGap;
      }
    }
  }
}

void PhiWindowController::OnSidebarClick(int x, int y) {
  POINT pt = {x, y};
  if (framework_ && ::PtInRect(&kBackButton, pt)) {
    framework_->GoBack(window_id_);
    RefreshTabs();
    return;
  }
  if (framework_ && ::PtInRect(&kForwardButton, pt)) {
    framework_->GoForward(window_id_);
    RefreshTabs();
    return;
  }
  if (framework_ && ::PtInRect(&kReloadButton, pt)) {
    framework_->Reload(window_id_, /*bypass_cache=*/false);
    RefreshTabs();
    return;
  }
  if (::PtInRect(&kNewWindowButton, pt)) {
    if (framework_) {
      framework_->NewWindow(window_id_);
    }
    return;
  }
  for (const TabRow& row : tabs_) {
    if (!::PtInRect(&row.bounds, pt)) {
      continue;
    }
    if (framework_) {
      if (::PtInRect(&row.close_box, pt)) {
        framework_->CloseTab(window_id_, row.index);
      } else {
        framework_->ActivateTab(window_id_, row.index);
      }
    }
    RefreshTabs();
    return;
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

  std::wstring text =
      L"Phi  ·  window " + base::UTF8ToWide(std::to_string(window_id_));
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, kInkColor);
  RECT text_rc = {16, 20, kSidebarWidth - 12, kUrlField.top - 8};
  ::DrawTextW(dc, text.c_str(), -1, &text_rc, DT_LEFT | DT_TOP | DT_WORDBREAK);

  // URL field: the client's own address display (Chromium's omnibox is
  // hollowed away).
  HBRUSH url_bg = ::CreateSolidBrush(kUrlFieldBg);
  ::FillRect(dc, &kUrlField, url_bg);
  ::DeleteObject(url_bg);
  ::SetTextColor(dc, kUrlInk);
  RECT url_rc = kUrlField;
  url_rc.left += 8;
  url_rc.right -= 8;
  ::DrawTextW(dc, nav_url_.empty() ? L"(no page)" : nav_url_.c_str(), -1,
              &url_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);

  // Back / forward / reload, greyed when unavailable.
  auto draw_button = [&](RECT rect, const wchar_t* glyph, bool enabled) {
    ::SetTextColor(dc, enabled ? kBtnEnabled : kBtnDisabled);
    ::DrawTextW(dc, glyph, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  };
  draw_button(kBackButton, L"<", can_go_back_);
  draw_button(kForwardButton, L">", can_go_forward_);
  draw_button(kReloadButton, nav_loading_ ? L"x" : L"R", true);

  // "+ New Window" button — proves the slot model (a click spawns a second
  // shell window via the framework).
  RECT btn = kNewWindowButton;
  HBRUSH btn_brush = ::CreateSolidBrush(kButtonColor);
  ::FillRect(dc, &btn, btn_brush);
  ::DeleteObject(btn_brush);
  ::SetTextColor(dc, kButtonInk);
  ::DrawTextW(dc, L"+  New Window", -1, &btn,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

  // Tab rows, driven by live framework state: the active tab gets a white
  // chip, each row a close box.
  for (const TabRow& row : tabs_) {
    if (row.bounds.bottom > client.bottom) {
      break;  // Out of sidebar space.
    }
    if (row.active) {
      HBRUSH chip = ::CreateSolidBrush(kTabActiveBg);
      ::FillRect(dc, &row.bounds, chip);
      ::DeleteObject(chip);
    }
    ::SetTextColor(dc, row.active ? kTabActiveInk : kTabInk);
    RECT title_rc = row.bounds;
    title_rc.left += 8;
    title_rc.right = row.close_box.left - 4;
    ::DrawTextW(dc, row.title.c_str(), -1, &title_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // Close affordance: a small x.
    ::DrawTextW(dc, L"x", -1, const_cast<RECT*>(&row.close_box),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
      if (self) {
        self->OnSidebarClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
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
        // Re-read framework state at paint time: the sidebar then reflects
        // reality even if a change notification was missed or coalesced.
        self->RebuildTabs();
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
