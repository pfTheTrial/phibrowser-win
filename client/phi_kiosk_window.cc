// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_kiosk_window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <optional>
#include <string>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

namespace {

constexpr wchar_t kKioskClassName[] = L"PhiKioskShell";

// Toolbar controls, in the order they are drawn and hit-tested.
enum HotControl {
  kHotBack = 0,
  kHotForward,
  kHotReload,
  kHotExpand,
  kHotClose,
};

// Design metrics (DIP), following the Mac's compact Kiosk toolbar.
constexpr int kToolbarHeight = 44;
constexpr int kButton = 28;
constexpr int kEdge = 10;
constexpr int kGap = 2;

// Opening size, when the framework gives us no better hint. The Mac restores
// an autosaved frame here; that is not ported yet, so every Kiosk opens at
// this size, centered on the work area.
constexpr int kInitialWidth = 980;
constexpr int kInitialHeight = 680;

// Segoe MDL2 glyphs, the same ones the main window's nav cluster uses.
constexpr wchar_t kGlyphBack[] = L"";
constexpr wchar_t kGlyphForward[] = L"";
constexpr wchar_t kGlyphReload[] = L"";
constexpr wchar_t kGlyphStop[] = L"";
constexpr wchar_t kGlyphExpand[] = L"";  // Open in new window.
constexpr wchar_t kGlyphClose[] = L"";

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr,
                                         0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), out.data(), size);
  return out;
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) {
    return std::string();
  }
  const int size =
      ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr,
                            nullptr);
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
    wc.lpfnWndProc = &PhiKioskWindow::WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = PhiAppIcon(32);
    wc.hIconSm = PhiAppIcon(16);
    // As in the shell: no erase, the paint covers everything.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kKioskClassName;
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

}  // namespace

PhiKioskWindow::PhiKioskWindow(int64_t window_id,
                               HWND browser_hwnd,
                               phi_framework::PhiFramework* framework,
                               PhiCoordinator* coordinator)
    : window_id_(window_id),
      browser_hwnd_(browser_hwnd),
      framework_(framework),
      coordinator_(coordinator) {
  EnsureClass();

  RECT work = {0, 0, kInitialWidth, kInitialHeight};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int dpi = ::GetDpiForSystem();
  const int width = ::MulDiv(kInitialWidth, dpi, 96);
  const int height = ::MulDiv(kInitialHeight, dpi, 96);
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 2;

  hwnd_ = ::CreateWindowExW(0, kKioskClassName, L"Phi",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width,
                            height, nullptr, nullptr,
                            ::GetModuleHandleW(nullptr), this);
  if (!hwnd_) {
    LOG(ERROR) << "[PhiClient] kiosk CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }
  dpi_ = ::GetDpiForWindow(hwnd_);

  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                          sizeof(corner));

  // Same adoption the shell performs: strip Chromium's top-level chrome and
  // clip its HWND into our content rect, so it stops being its own taskbar
  // and Alt-Tab entry.
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

  RefreshState();
  PositionContent();
  ::ShowWindow(hwnd_, SW_SHOW);
  ::SetForegroundWindow(hwnd_);
}

PhiKioskWindow::~PhiKioskWindow() {
  // The browser HWND belongs to Chromium; hand it back to the desktop before
  // this window dies so its teardown is not reparented into a dead HWND.
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetParent(browser_hwnd_, nullptr);
  }
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd_);
  }
}

void PhiKioskWindow::RefreshState() {
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
    if (const std::string* title = nav->FindString("title")) {
      title_ = Widen(*title);
    }
    can_go_back_ = nav->FindBool("canGoBack").value_or(false);
    can_go_forward_ = nav->FindBool("canGoForward").value_or(false);
    loading_ = nav->FindBool("loading").value_or(false);
  }
  if (hwnd_ && ::IsWindow(hwnd_)) {
    // The page title names the window in the taskbar, as on the Mac.
    ::SetWindowTextW(hwnd_, title_.empty() ? L"Phi" : title_.c_str());
    ::InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

PhiKioskWindow::Layout PhiKioskWindow::ComputeLayout() const {
  Layout out = {};
  RECT client = {};
  if (hwnd_) {
    ::GetClientRect(hwnd_, &client);
  }
  const int bar = Scaled(kToolbarHeight);
  const int btn = Scaled(kButton);
  const int edge = Scaled(kEdge);
  const int gap = Scaled(kGap);
  const int mid = bar / 2;

  out.toolbar = {0, 0, client.right, bar};

  int x = edge;
  out.back = {x, mid - btn / 2, x + btn, mid + btn / 2};
  x = out.back.right + gap;
  out.forward = {x, mid - btn / 2, x + btn, mid + btn / 2};
  x = out.forward.right + gap;
  out.reload = {x, mid - btn / 2, x + btn, mid + btn / 2};

  // Trailing cluster, laid out from the right edge inwards.
  int r = client.right - edge;
  out.close = {r - btn, mid - btn / 2, r, mid + btn / 2};
  r = out.close.left - gap;
  out.expand = {r - btn, mid - btn / 2, r, mid + btn / 2};

  out.url = {out.reload.right + Scaled(8), 0, out.expand.left - Scaled(8), bar};
  out.content = {0, bar, client.right, client.bottom};
  return out;
}

void PhiKioskWindow::PositionContent() {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    return;
  }
  const Layout layout = ComputeLayout();
  ::SetWindowPos(browser_hwnd_, nullptr, layout.content.left,
                 layout.content.top, layout.content.right - layout.content.left,
                 layout.content.bottom - layout.content.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void PhiKioskWindow::Paint() {
  PAINTSTRUCT ps = {};
  HDC dc = ::BeginPaint(hwnd_, &ps);
  if (!dc) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd_, &client);

  // A Kiosk belongs to no Space, so PaletteForSlot falls through to the
  // app-wide palette — which is what this window should follow.
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  const PhiPalette palette =
      coordinator_ ? coordinator_->PaletteForSlot(window_id_) : kFallback;

  // Back-buffer: the toolbar sits directly against the accelerated web
  // surface, and painting it straight to the window tears while resizing.
  HDC mem = ::CreateCompatibleDC(dc);
  HBITMAP bmp = ::CreateCompatibleBitmap(dc, client.right, client.bottom);
  HGDIOBJ old_bmp = ::SelectObject(mem, bmp);

  const Layout layout = ComputeLayout();
  HBRUSH bar_brush = ::CreateSolidBrush(palette.window_bg);
  ::FillRect(mem, &layout.toolbar, bar_brush);
  ::DeleteObject(bar_brush);

  // Hairline under the toolbar, separating chrome from page.
  RECT rule = {0, layout.toolbar.bottom - 1, client.right,
               layout.toolbar.bottom};
  HBRUSH rule_brush = ::CreateSolidBrush(palette.separator);
  ::FillRect(mem, &rule, rule_brush);
  ::DeleteObject(rule_brush);

  HFONT glyph_font = ::CreateFontW(
      -Scaled(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH, L"Segoe MDL2 Assets");
  HFONT text_font = ::CreateFontW(
      -Scaled(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
      DEFAULT_PITCH, L"Segoe UI");

  ::SetBkMode(mem, TRANSPARENT);

  ::SelectObject(mem, glyph_font);
  auto draw_control = [&](int index, const RECT& rc, const wchar_t* glyph,
                          bool enabled) {
    if (index == hot_ && enabled) {
      HBRUSH hover = ::CreateSolidBrush(palette.hover_strong);
      ::FillRect(mem, &rc, hover);
      ::DeleteObject(hover);
    }
    ::SetTextColor(mem, enabled ? palette.label : palette.label_tertiary);
    RECT text_rc = rc;
    ::DrawTextW(mem, glyph, -1, &text_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  };
  draw_control(kHotBack, layout.back, kGlyphBack, can_go_back_);
  draw_control(kHotForward, layout.forward, kGlyphForward, can_go_forward_);
  draw_control(kHotReload, layout.reload,
               loading_ ? kGlyphStop : kGlyphReload, true);
  draw_control(kHotExpand, layout.expand, kGlyphExpand, true);
  draw_control(kHotClose, layout.close, kGlyphClose, true);

  // The URL, read-only: a Kiosk window has no omnibox — it shows what it is
  // displaying and nothing more.
  ::SelectObject(mem, text_font);
  ::SetTextColor(mem, palette.label_dim);
  RECT url_rc = layout.url;
  ::DrawTextW(mem, url_.c_str(), -1, &url_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);

  ::BitBlt(dc, 0, 0, client.right, layout.toolbar.bottom, mem, 0, 0, SRCCOPY);

  ::SelectObject(mem, old_bmp);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
  ::DeleteObject(glyph_font);
  ::DeleteObject(text_font);
  ::EndPaint(hwnd_, &ps);
}

void PhiKioskWindow::OnMouseMove(POINT pt) {
  const Layout layout = ComputeLayout();
  int hot = -1;
  if (::PtInRect(&layout.back, pt)) {
    hot = kHotBack;
  } else if (::PtInRect(&layout.forward, pt)) {
    hot = kHotForward;
  } else if (::PtInRect(&layout.reload, pt)) {
    hot = kHotReload;
  } else if (::PtInRect(&layout.expand, pt)) {
    hot = kHotExpand;
  } else if (::PtInRect(&layout.close, pt)) {
    hot = kHotClose;
  }
  if (hot != hot_) {
    hot_ = hot;
    ::InvalidateRect(hwnd_, &layout.toolbar, FALSE);
  }
  if (hot >= 0) {
    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd_, 0};
    ::TrackMouseEvent(&track);
  }
}

void PhiKioskWindow::OnClick(POINT pt) {
  if (!framework_) {
    return;
  }
  const Layout layout = ComputeLayout();
  if (::PtInRect(&layout.back, pt)) {
    framework_->GoBack(window_id_);
  } else if (::PtInRect(&layout.forward, pt)) {
    framework_->GoForward(window_id_);
  } else if (::PtInRect(&layout.reload, pt)) {
    framework_->Reload(window_id_, /*bypass_cache=*/false);
  } else if (::PtInRect(&layout.expand, pt)) {
    // "Promote to a real tab": the Mac's expand — hand the page to the main
    // window and let this ephemeral window go.
    if (coordinator_) {
      coordinator_->OpenUrlInMainWindow(Narrow(url_));
    }
    framework_->CloseBrowserWindow(window_id_);
  } else if (::PtInRect(&layout.close, pt)) {
    framework_->CloseBrowserWindow(window_id_);
  }
}

LRESULT PhiKioskWindow::Handle(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      Paint();
      return 0;
    case WM_SIZE:
      PositionContent();
      ::InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wparam);
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      if (suggested) {
        ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left,
                       suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
      }
      PositionContent();
      ::InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
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
    case WM_SETFOCUS:
      // Keystrokes belong to the page, never to this bare chrome.
      if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
        ::SetFocus(browser_hwnd_);
      }
      return 0;
    case WM_CLOSE:
      // Route through the framework so the Browser tears down properly; the
      // coordinator drops this window when OnBrowserWindowClosing arrives.
      if (framework_) {
        framework_->CloseBrowserWindow(window_id_);
        return 0;
      }
      break;
  }
  return ::DefWindowProcW(hwnd_, msg, wparam, lparam);
}

// static
LRESULT CALLBACK PhiKioskWindow::WndProc(HWND hwnd,
                                         UINT msg,
                                         WPARAM wparam,
                                         LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    auto* self =
        static_cast<PhiKioskWindow*>(create->lpCreateParams);
    if (self) {
      self->hwnd_ = hwnd;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiKioskWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) {
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->Handle(msg, wparam, lparam);
}

}  // namespace phi_client
