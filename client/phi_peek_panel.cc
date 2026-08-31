// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_peek_panel.h"

#include <windowsx.h>

#include <algorithm>
#include <optional>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

constexpr wchar_t kPeekClassName[] = L"PhiPeekPanel";

// The Mac's PeekPanelController metrics, in DIP.
constexpr double kPaneInsetRatio = 0.04;
constexpr int kMinPaneInset = 16;
constexpr int kPaneVerticalInset = 14;
constexpr int kControlGutterWidth = 44;
constexpr int kControlButtonSize = 28;
constexpr int kControlSpacing = 6;
constexpr int kControlTopInset = 6;

enum HotControl {
  kHotClose = 0,
  kHotExpand,
};

// Escapes, not literal glyphs: the private-use characters do not survive
// every editor round-trip (these two were silently written out empty once).
constexpr wchar_t kGlyphClose[] = L"\uE711";   // Cancel
constexpr wchar_t kGlyphExpand[] = L"\uE8A7";  // OpenInNewWindow

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(
      CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), out.data(), size);
  return out;
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) {
    return std::string();
  }
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()), nullptr,
                                         0, nullptr, nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
}

ATOM EnsureClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PhiPeekPanel::WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPeekClassName;
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

}  // namespace

PhiPeekPanel::PhiPeekPanel(int64_t window_id,
                           HWND browser_hwnd,
                           int64_t opener_window_id,
                           int64_t opener_tab_id,
                           PhiWindowController* opener,
                           phi_framework::PhiFramework* framework,
                           PhiCoordinator* coordinator)
    : window_id_(window_id),
      browser_hwnd_(browser_hwnd),
      opener_window_id_(opener_window_id),
      opener_tab_id_(opener_tab_id),
      opener_(opener),
      framework_(framework),
      coordinator_(coordinator) {
  EnsureClass();
  HWND shell = opener_ ? opener_->shell_hwnd() : nullptr;
  if (!shell) {
    LOG(ERROR) << "[PhiClient] peek has no opener shell";
    return;
  }
  dpi_ = ::GetDpiForWindow(shell);

  // A child of the opener's shell, above the hosted browser: it clips into the
  // content card and travels with the window, the same trick the bookmark
  // manager uses for its URL-mounted page.
  hwnd_ = ::CreateWindowExW(0, kPeekClassName, L"", WS_CHILD | WS_CLIPCHILDREN,
                            0, 0, 0, 0, shell, nullptr,
                            ::GetModuleHandleW(nullptr), this);
  if (!hwnd_) {
    LOG(ERROR) << "[PhiClient] peek CreateWindowEx failed err="
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
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetFocus(browser_hwnd_);
  }
}

PhiPeekPanel::~PhiPeekPanel() {
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetParent(browser_hwnd_, nullptr);
  }
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd_);
  }
}

void PhiPeekPanel::Reposition() {
  if (!hwnd_ || !::IsWindow(hwnd_) || !opener_) {
    return;
  }
  const RECT content = opener_->ContentRect();
  const int width = content.right - content.left;
  const int height = content.bottom - content.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  // The panel spans the card minus the Mac's insets; the control gutter is
  // kept INSIDE that reach, to the card's right, so the panel still stops at
  // the page card's edges.
  const int inset_x =
      std::max(Scaled(kMinPaneInset),
               static_cast<int>(width * kPaneInsetRatio));
  const int inset_y = Scaled(kPaneVerticalInset);
  ::SetWindowPos(hwnd_, nullptr, content.left + inset_x, content.top + inset_y,
                 std::max(0, width - inset_x * 2),
                 std::max(0, height - inset_y * 2),
                 SWP_NOZORDER | SWP_NOACTIVATE);
  PositionContent();
  ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhiPeekPanel::SetVisibleForActiveTab(int64_t active_tab_id) {
  if (!hwnd_ || !::IsWindow(hwnd_)) {
    return;
  }
  const bool mine = active_tab_id == opener_tab_id_;
  ::ShowWindow(hwnd_, mine ? SW_SHOWNA : SW_HIDE);
  if (mine) {
    ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
}

void PhiPeekPanel::RefreshState() {
  if (!framework_) {
    return;
  }
  const char* json = framework_->NavStateJson(window_id_);
  std::optional<base::DictValue> nav =
      json ? base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC)
           : std::nullopt;
  if (nav) {
    if (const std::string* url = nav->FindString("url")) {
      url_ = Widen(*url);
    }
  }
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

PhiPeekPanel::Layout PhiPeekPanel::ComputeLayout() const {
  Layout out = {};
  RECT client = {};
  if (hwnd_) {
    ::GetClientRect(hwnd_, &client);
  }
  const int gutter = Scaled(kControlGutterWidth);
  const int btn = Scaled(kControlButtonSize);
  const int spacing = Scaled(kControlSpacing);
  const int top = Scaled(kControlTopInset);

  out.card = {0, 0, std::max(0L, client.right - gutter), client.bottom};
  out.gutter = {out.card.right, 0, client.right, client.bottom};

  const int cx = out.gutter.left + (gutter - btn) / 2;
  out.close = {cx, top, cx + btn, top + btn};
  out.expand = {cx, out.close.bottom + spacing, cx + btn,
                out.close.bottom + spacing + btn};
  return out;
}

void PhiPeekPanel::PositionContent() {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    return;
  }
  const Layout layout = ComputeLayout();
  ::SetWindowPos(browser_hwnd_, nullptr, layout.card.left, layout.card.top,
                 layout.card.right - layout.card.left,
                 layout.card.bottom - layout.card.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void PhiPeekPanel::Paint() {
  PAINTSTRUCT ps = {};
  HDC dc = ::BeginPaint(hwnd_, &ps);
  if (!dc) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  const PhiPalette palette =
      coordinator_ ? coordinator_->PaletteForSlot(opener_window_id_)
                   : kFallback;

  const Layout layout = ComputeLayout();
  // Only the gutter is ours to paint; the card is the page's own surface.
  HBRUSH ground = ::CreateSolidBrush(palette.ground_top);
  RECT gutter = layout.gutter;
  ::FillRect(dc, &gutter, ground);
  ::DeleteObject(ground);

  HFONT glyphs = ::CreateFontW(-Scaled(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                               FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe MDL2 Assets");
  HGDIOBJ old_font = ::SelectObject(dc, glyphs);
  ::SetBkMode(dc, TRANSPARENT);

  auto draw = [&](int index, const RECT& rc, const wchar_t* glyph) {
    // The Mac draws these as chips over the page, so they need to read
    // against arbitrary page content — a resting ink-4% wash does not.
    HBRUSH fill = ::CreateSolidBrush(index == hot_ ? palette.selected
                                                   : palette.card);
    ::FillRect(dc, &rc, fill);
    ::DeleteObject(fill);
    HBRUSH edge = ::CreateSolidBrush(palette.border);
    ::FrameRect(dc, &rc, edge);
    ::DeleteObject(edge);
    ::SetTextColor(dc, palette.label);
    RECT text = rc;
    ::DrawTextW(dc, glyph, -1, &text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  };
  draw(kHotClose, layout.close, kGlyphClose);
  draw(kHotExpand, layout.expand, kGlyphExpand);

  ::SelectObject(dc, old_font);
  ::DeleteObject(glyphs);
  ::EndPaint(hwnd_, &ps);
}

void PhiPeekPanel::OnMouseMove(POINT pt) {
  const Layout layout = ComputeLayout();
  int hot = -1;
  if (::PtInRect(&layout.close, pt)) {
    hot = kHotClose;
  } else if (::PtInRect(&layout.expand, pt)) {
    hot = kHotExpand;
  }
  if (hot != hot_) {
    hot_ = hot;
    ::InvalidateRect(hwnd_, &layout.gutter, FALSE);
  }
  if (hot >= 0) {
    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd_, 0};
    ::TrackMouseEvent(&track);
  }
}

void PhiPeekPanel::Dismiss() {
  if (framework_) {
    framework_->CloseBrowserWindow(window_id_);
  }
}

void PhiPeekPanel::OnClick(POINT pt) {
  const Layout layout = ComputeLayout();
  if (::PtInRect(&layout.close, pt)) {
    Dismiss();
  } else if (::PtInRect(&layout.expand, pt)) {
    // Expand: the page becomes a real tab in the opener window. The Mac moves
    // the live tab across; here the peek is its own browser, so the URL is
    // re-opened and the preview closed (see phi_peek_window.h).
    if (framework_ && !url_.empty()) {
      framework_->NewTab(opener_window_id_, Narrow(url_).c_str());
    }
    Dismiss();
  }
}

LRESULT PhiPeekPanel::Handle(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      Paint();
      return 0;
    case WM_SIZE:
      PositionContent();
      return 0;
    case WM_MOUSEMOVE: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      OnMouseMove(pt);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hot_ != -1) {
        hot_ = -1;
        ::InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      OnClick(pt);
      return 0;
    }
  }
  return ::DefWindowProcW(hwnd_, msg, wparam, lparam);
}

// static
LRESULT CALLBACK PhiPeekPanel::WndProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wparam,
                                       LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    auto* self = static_cast<PhiPeekPanel*>(create->lpCreateParams);
    if (self) {
      self->hwnd_ = hwnd;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self =
      reinterpret_cast<PhiPeekPanel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) {
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->Handle(msg, wparam, lparam);
}

}  // namespace phi_client
