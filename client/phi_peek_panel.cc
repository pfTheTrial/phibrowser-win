// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_peek_panel.h"

#include "phibrowser-win/client/phi_dwrite.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"
#include "phibrowser-win/client/phi_window_model.h"

namespace phi_client {

namespace {

constexpr wchar_t kPeekClassName[] = L"PhiPeekPanel";
constexpr wchar_t kPeekShadowClassName[] = L"PhiPeekShadow";
constexpr wchar_t kPeekControlsClassName[] = L"PhiPeekControls";

// The Mac's PeekPanelController metrics, in DIP.
constexpr double kPaneInsetRatio = 0.04;
constexpr int kMinPaneInset = 24;
constexpr int kPaneVerticalInset = 24;
constexpr int kControlGutterWidth = 44;
constexpr int kControlButtonSize = 28;
constexpr int kControlSpacing = 6;
constexpr int kControlTopInset = 6;
// The card itself (PeekPanelController): a 12pt rounded sheet under a soft
// drop shadow, so a peek reads as something floating OVER the page rather
// than as part of it.
constexpr int kCardRadius = 12;
constexpr int kControlRadius = 8;
// The Mac's card shadow — blur 20, 6pt down, black at 44% (60% on dark, where
// a soft black all but disappears against the page) — and the margin the
// blur needs around the card to fade out inside.
constexpr int kShadowMargin = 24;
constexpr int kShadowBlur = 20;
constexpr int kShadowOffsetY = 6;
constexpr double kShadowAlphaLight = 0.44;
constexpr double kShadowAlphaDark = 0.60;

enum HotControl {
  kHotClose = 0,
  kHotExpand,
  kHotSplit,
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

// The shadow surface never paints through its window proc — its pixels come
// from UpdateLayeredWindow — so the default proc is the whole class.
ATOM EnsureShadowClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kPeekShadowClassName;
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

ATOM EnsureControlsClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PhiPeekPanel::ControlsWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPeekControlsClassName;
    return ::RegisterClassExW(&wc);
  }();
  return atom;
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
                           const PhiWindowModel* model,
                           PhiCoordinator* coordinator)
    : window_id_(BrowserId{window_id}),
      browser_hwnd_(browser_hwnd),
      opener_window_id_(BrowserId{opener_window_id}),
      opener_tab_id_(opener_tab_id),
      opener_(opener),
      framework_(framework),
      model_(model),
      coordinator_(coordinator) {
  EnsureClass();
  HWND shell = opener_ ? opener_->shell_hwnd() : nullptr;
  if (!shell) {
    LOG(ERROR) << "[PhiClient] peek has no opener shell";
    return;
  }
  dpi_ = ::GetDpiForWindow(shell);

  EnsureShadowClass();
  // WS_EX_TRANSPARENT keeps the shadow out of hit testing: a click in the
  // blur is a click on the page around the card, which dismisses the peek.
  shadow_ = ::CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY,
      kPeekShadowClassName, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0,
      shell, nullptr, ::GetModuleHandleW(nullptr), nullptr);

  // A child of the opener's shell, above the hosted browser: it clips into the
  // content card and travels with the window, the same trick the bookmark
  // manager uses for its URL-mounted page.
  hwnd_ = ::CreateWindowExW(0, kPeekClassName, L"",
                          // Keeps the card's corner caps visible.
                          WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
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

  EnsureControlsClass();
  controls_ = ::CreateWindowExW(WS_EX_NOPARENTNOTIFY, kPeekControlsClassName,
                                L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                                hwnd_, nullptr, ::GetModuleHandleW(nullptr),
                                this);
  if (controls_) {
    ::ShowWindow(controls_, SW_SHOWNA);
  }

  Reposition();
  RefreshState();
  Raise();
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetFocus(browser_hwnd_);
  }
}

void PhiPeekPanel::Raise() {
  // Above the shell's hosted browser child, or the page would occlude both.
  // The shadow goes up first so the card never ends up under its own shadow.
  if (shadow_ && ::IsWindow(shadow_)) {
    ::SetWindowPos(shadow_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
  }
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
  }
}

SidebarSurface PhiPeekPanel::kind() const {
  return SidebarSurface::kPeek;
}
int64_t PhiPeekPanel::window_id() const {
  return window_id_.value;
}
int64_t PhiPeekPanel::origin_window_id() const {
  return opener_window_id_.value;
}
int64_t PhiPeekPanel::origin_tab_id() const {
  return opener_tab_id_;
}
HWND PhiPeekPanel::hwnd() const {
  return hwnd_;
}

PhiPeekPanel::~PhiPeekPanel() {
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetParent(browser_hwnd_, nullptr);
  }
  if (controls_ && ::IsWindow(controls_)) {
    ::DestroyWindow(controls_);
  }
  if (shadow_ && ::IsWindow(shadow_)) {
    ::DestroyWindow(shadow_);
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
  ApplyCardShape();
  PositionContent();
  UpdateShadow();
  ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhiPeekPanel::ApplyCardShape() {
  if (!hwnd_ || !::IsWindow(hwnd_)) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  if (client.right <= 0 || client.bottom <= 0) {
    return;
  }
  // CreateRoundRectRgn takes the ELLIPSE size, so it is twice the radius.
  // The page is a child of this window, so this region rounds IT too — the
  // shell needs sibling corner caps for the same job only because
  // DirectComposition ignores a region set on the browser child itself.
  const int ellipse = Scaled(kCardRadius) * 2;
  HRGN shape = ::CreateRoundRectRgn(0, 0, client.right + 1, client.bottom + 1,
                                    ellipse, ellipse);
  ::SetWindowRgn(hwnd_, shape, TRUE);  // The window owns it now.
}

void PhiPeekPanel::UpdateShadow() {
  if (!shadow_ || !::IsWindow(shadow_) || !hwnd_ || !::IsWindow(hwnd_)) {
    return;
  }
  RECT card = {};
  ::GetWindowRect(hwnd_, &card);
  const int margin = Scaled(kShadowMargin);
  const int width = (card.right - card.left) + margin * 2;
  const int height = (card.bottom - card.top) + margin * 2;
  if (width <= 0 || height <= 0) {
    return;
  }
  // The card's own top-left, in the shell's client space, then out by the
  // margin and down by the shadow's offset.
  POINT origin = {card.left, card.top};
  ::ScreenToClient(::GetParent(shadow_), &origin);
  ::SetWindowPos(shadow_, nullptr, origin.x - margin,
                 origin.y - margin + Scaled(kShadowOffsetY), width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);

  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // Top-down, so y runs with the loop.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = ::GetDC(nullptr);
  HBITMAP dib =
      ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  HDC mem = ::CreateCompatibleDC(screen);
  ::ReleaseDC(nullptr, screen);
  if (!dib || !mem || !bits) {
    if (dib) {
      ::DeleteObject(dib);
    }
    if (mem) {
      ::DeleteDC(mem);
    }
    return;
  }
  HGDIOBJ old_bitmap = ::SelectObject(mem, dib);

  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  const PhiPalette palette =
      coordinator_ && opener_
          ? coordinator_->PaletteForSlot(opener_->slot().value)
          : kFallback;
  const double base = palette.dark ? kShadowAlphaDark : kShadowAlphaLight;
  const double radius = Scaled(kCardRadius);
  const double blur = std::max(1, Scaled(kShadowBlur));
  // The card's silhouette, centred in the surface: everything is expressed as
  // a signed distance to it, which is what makes the corners fade like the
  // edges instead of stepping.
  const double cx = width / 2.0;
  const double cy = height / 2.0;
  const double half_x = (width - 2.0 * margin) / 2.0 - radius;
  const double half_y = (height - 2.0 * margin) / 2.0 - radius;
  // SAFETY: CreateDIBSection allocated width*height 32-bit pixels above.
  const base::span<uint32_t> pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
      static_cast<uint32_t*>(bits), static_cast<size_t>(width) * height));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const double dx = std::abs(x + 0.5 - cx) - half_x;
      const double dy = std::abs(y + 0.5 - cy) - half_y;
      double distance;
      if (dx <= 0.0 && dy <= 0.0) {
        distance = std::max(dx, dy) - radius;  // Well inside: no sqrt.
      } else {
        const double ox = std::max(dx, 0.0);
        const double oy = std::max(dy, 0.0);
        distance = std::sqrt(ox * ox + oy * oy) - radius;
      }
      // Centred on the edge — full strength half a blur inside, nothing half
      // a blur outside, half strength ON it. Ramping from the edge outwards
      // instead leaves a hard band of full black under the offset card.
      const double t =
          std::clamp(0.5 - distance / blur, 0.0, 1.0);
      const double alpha = base * t * t * (3.0 - 2.0 * t);  // Smoothstep.
      // Premultiplied black: the colour channels stay 0 at every alpha.
      pixels[static_cast<size_t>(y) * width + x] =
          static_cast<uint32_t>(alpha * 255.0 + 0.5) << 24;
    }
  }

  POINT src = {0, 0};
  SIZE size = {width, height};
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  ::UpdateLayeredWindow(shadow_, nullptr, nullptr, &size, mem, &src, 0, &blend,
                        ULW_ALPHA);
  ::SelectObject(mem, old_bitmap);
  ::DeleteObject(dib);
  ::DeleteDC(mem);
}

void PhiPeekPanel::SetVisibleForActiveTab(int64_t active_tab_id) {
  if (!hwnd_ || !::IsWindow(hwnd_)) {
    return;
  }
  const bool mine = active_tab_id == opener_tab_id_;
  ::ShowWindow(hwnd_, mine ? SW_SHOWNA : SW_HIDE);
  if (shadow_ && ::IsWindow(shadow_)) {
    ::ShowWindow(shadow_, mine ? SW_SHOWNA : SW_HIDE);
  }
  if (mine) {
    Raise();
  }
}

void PhiPeekPanel::RefreshState() {
  // The peek's own browser's snapshot: the model's copy, or — for a browser
  // the model has not been pushed yet (this panel is built inside its
  // Browser::Create) — pulled typed from the framework.
  const phi_framework::WindowSnapshot* snapshot =
      model_ ? model_->Get(window_id_) : nullptr;
  phi_framework::WindowSnapshot pulled;
  if (!snapshot && framework_ && framework_->Snapshot(window_id_, &pulled)) {
    snapshot = &pulled;
  }
  if (snapshot) {
    url_ = Widen(snapshot->nav.url);
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

  // The page takes the whole card — the controls FLOAT over its top-right
  // corner rather than sitting in a chrome strip beside it, which is what the
  // Mac does and what keeps the preview reading as a page and not a panel.
  out.card = {0, 0, client.right, client.bottom};
  out.gutter = {std::max(0L, client.right - gutter), 0, client.right,
                top * 2 + btn * 3 + spacing * 2};

  const int cx = out.gutter.left + (gutter - btn) / 2;
  const int cy = out.gutter.top + top;
  out.close = {cx, cy, cx + btn, cy + btn};
  out.expand = {cx, out.close.bottom + spacing, cx + btn,
                out.close.bottom + spacing + btn};
  out.split = {cx, out.expand.bottom + spacing, cx + btn,
               out.expand.bottom + spacing + btn};
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
  PositionControls();
}

void PhiPeekPanel::PositionControls() {
  if (!controls_ || !::IsWindow(controls_)) {
    return;
  }
  const Layout layout = ComputeLayout();
  const int width = layout.gutter.right - layout.gutter.left;
  const int height = layout.gutter.bottom - layout.gutter.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  // HWND_TOP on every move: the chips have to stay above the hosted browser,
  // which reorders itself as it renders.
  ::SetWindowPos(controls_, HWND_TOP, layout.gutter.left, layout.gutter.top,
                 width, height, SWP_NOACTIVATE);
  // Only the chips themselves are the window — the space between them is cut
  // away, so the page shows through and a click there lands on the page.
  const int chip = Scaled(kControlRadius) * 2;
  HRGN shape = ::CreateRectRgn(0, 0, 0, 0);
  for (const RECT& rc : {layout.close, layout.expand, layout.split}) {
    HRGN one = ::CreateRoundRectRgn(rc.left - layout.gutter.left,
                                    rc.top - layout.gutter.top,
                                    rc.right - layout.gutter.left + 1,
                                    rc.bottom - layout.gutter.top + 1, chip,
                                    chip);
    ::CombineRgn(shape, shape, one, RGN_OR);
    ::DeleteObject(one);
  }
  ::SetWindowRgn(controls_, shape, TRUE);  // The window owns it now.
  ::InvalidateRect(controls_, nullptr, FALSE);
}

void PhiPeekPanel::Paint() {
  PAINTSTRUCT ps = {};
  HDC dc = ::BeginPaint(hwnd_, &ps);
  if (!dc) {
    return;
  }
  // The page covers the whole card; this only shows through in the moment
  // before the browser child has drawn, and behind its rounded corners.
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  const PhiPalette palette =
      coordinator_ && opener_
          ? coordinator_->PaletteForSlot(opener_->slot().value)
          : kFallback;
  HBRUSH ground = ::CreateSolidBrush(palette.card);
  ::FillRect(dc, &client, ground);
  ::DeleteObject(ground);
  ::EndPaint(hwnd_, &ps);
}

void PhiPeekPanel::PaintControls() {
  PAINTSTRUCT ps = {};
  HDC dc = ::BeginPaint(controls_, &ps);
  if (!dc) {
    return;
  }
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  const PhiPalette palette =
      coordinator_ && opener_
          ? coordinator_->PaletteForSlot(opener_->slot().value)
          : kFallback;
  const Layout layout = ComputeLayout();
  const POINT origin = {layout.gutter.left, layout.gutter.top};

  HFONT glyphs = ::CreateFontW(-Scaled(11), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                               FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Segoe MDL2 Assets");
  HGDIOBJ old_font = ::SelectObject(dc, glyphs);
  ::SetBkMode(dc, TRANSPARENT);

  // The chips read against arbitrary page content, so each is an opaque
  // rounded plate — the Mac's 8pt corners — not a bare glyph.
  auto plate = [&](int index, const RECT& rc) {
    const int chip = Scaled(kControlRadius);
    PhiFillRoundRectAA(dc, rc, chip,
                       index == hot_ ? palette.selected : palette.card);
    PhiStrokeRoundRectAA(dc, rc, chip, palette.border, 1);
  };
  auto shift = [&origin](const RECT& rc) {
    return RECT{rc.left - origin.x, rc.top - origin.y, rc.right - origin.x,
                rc.bottom - origin.y};
  };
  auto glyph_chip = [&](int index, const RECT& panel_rect,
                        const wchar_t* glyph) {
    const RECT rc = shift(panel_rect);
    plate(index, rc);
    ::SetTextColor(dc, palette.label);
    RECT text = rc;
    PhiDrawTextAA(dc, glyph, -1, &text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  };
  glyph_chip(kHotClose, layout.close, kGlyphClose);
  glyph_chip(kHotExpand, layout.expand, kGlyphExpand);

  // Split view is DRAWN, not lettered: Segoe MDL2 has no side-by-side mark
  // that renders the same across the Windows versions this ships on, and a
  // missing glyph shows as a hollow box.
  {
    const RECT rc = shift(layout.split);
    plate(kHotSplit, rc);
    RECT mark = rc;
    ::InflateRect(&mark, -Scaled(8), -Scaled(9));
    HPEN pen = ::CreatePen(PS_SOLID, std::max(1, Scaled(1)), palette.label);
    HGDIOBJ old_pen = ::SelectObject(dc, pen);
    HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, mark.left, mark.top, mark.right, mark.bottom);
    const int mid = (mark.left + mark.right) / 2;
    ::MoveToEx(dc, mid, mark.top, nullptr);
    ::LineTo(dc, mid, mark.bottom);
    ::SelectObject(dc, old_brush);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(pen);
  }

  ::SelectObject(dc, old_font);
  ::DeleteObject(glyphs);
  ::EndPaint(controls_, &ps);
}

void PhiPeekPanel::OnMouseMove(POINT pt) {
  const Layout layout = ComputeLayout();
  int hot = -1;
  if (::PtInRect(&layout.close, pt)) {
    hot = kHotClose;
  } else if (::PtInRect(&layout.expand, pt)) {
    hot = kHotExpand;
  } else if (::PtInRect(&layout.split, pt)) {
    hot = kHotSplit;
  }
  if (hot != hot_) {
    hot_ = hot;
    if (controls_ && ::IsWindow(controls_)) {
      ::InvalidateRect(controls_, nullptr, FALSE);
    }
  }
  if (hot >= 0 && controls_) {
    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, controls_, 0};
    ::TrackMouseEvent(&track);
  }
}

void PhiPeekPanel::Dismiss() {
  if (framework_) {
    framework_->CloseBrowserWindow(window_id_.value);
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
      framework_->NewTab(opener_window_id_.value, Narrow(url_).c_str());
    }
    Dismiss();
  } else if (::PtInRect(&layout.split, pt)) {
    // Split: the same page as a fresh tab, put beside the one that opened
    // the peek — which is what the reader is comparing it against. Both by
    // stable id: OpenTab answers with the new tab's, the opener's is known.
    if (framework_ && !url_.empty()) {
      const TabId fresh =
          framework_->OpenTab(opener_window_id_, Narrow(url_).c_str());
      if (fresh.valid() && opener_tab_id_ > 0) {
        framework_->CreateSplitViewForTabs(
            opener_window_id_.value, fresh.value,
            static_cast<int>(opener_tab_id_), "vertical");
      }
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
  }
  return ::DefWindowProcW(hwnd_, msg, wparam, lparam);
}

LRESULT PhiPeekPanel::HandleControls(UINT msg, WPARAM wparam, LPARAM lparam) {
  // The chips are laid out in the PANEL's space; this window is a viewport
  // onto its top-right corner, so every point comes back translated.
  const Layout layout = ComputeLayout();
  const POINT origin = {layout.gutter.left, layout.gutter.top};
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      PaintControls();
      return 0;
    case WM_MOUSEMOVE: {
      POINT pt = {GET_X_LPARAM(lparam) + origin.x,
                  GET_Y_LPARAM(lparam) + origin.y};
      OnMouseMove(pt);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hot_ != -1) {
        hot_ = -1;
        ::InvalidateRect(controls_, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lparam) + origin.x,
                  GET_Y_LPARAM(lparam) + origin.y};
      OnClick(pt);
      return 0;
    }
  }
  return ::DefWindowProcW(controls_, msg, wparam, lparam);
}

// static
LRESULT CALLBACK PhiPeekPanel::ControlsWndProc(HWND hwnd,
                                               UINT msg,
                                               WPARAM wparam,
                                               LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiPeekPanel*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self || !self->controls_) {
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->HandleControls(msg, wparam, lparam);
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
