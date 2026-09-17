// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_reader_panel.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_fluent.h"

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_reader.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"
#include "phibrowser-win/client/phi_window_model.h"

namespace phi_client {

namespace {

constexpr wchar_t kReaderPanelClassName[] = L"PhiReaderPanel";

// The page card's corner radius (PhiShellWindow's kContentRadius,
// webContentContainerCornerRadius on macOS 26) — the reading surface wears
// the same corners as the page it covers.
constexpr int kContentRadius = 8;  // §5.7: the sheet family's radius.
constexpr int kHeaderHeight = 36;
constexpr int kHeaderPad = 12;
constexpr int kFaviconSize = 16;
constexpr int kButtonSize = 28;
constexpr int kButtonGap = 4;
constexpr int kButtonRadius = 4;

}  // namespace

PhiReaderPanel::PhiReaderPanel(int64_t window_id,
                               HWND browser_hwnd,
                               int64_t origin_window_id,
                               int64_t origin_tab_id,
                               PhiShellWindow* origin,
                               phi_framework::PhiFramework* framework,
                               const PhiWindowModel* model,
                               PhiCoordinator* coordinator)
    : window_id_(BrowserId{window_id}),
      browser_hwnd_(browser_hwnd),
      origin_window_id_(BrowserId{origin_window_id}),
      origin_tab_id_(origin_tab_id),
      origin_(origin),
      framework_(framework),
      model_(model),
      coordinator_(coordinator) {
  HWND shell = origin_ ? origin_->shell_hwnd() : nullptr;
  if (!shell) {
    LOG(ERROR) << "[PhiClient] reader panel has no origin shell";
    return;
  }

  // A child of the origin's shell, above the hosted browser — the peek
  // panel's trick, so the reader clips into the content card and travels
  // with the window.
  ClassSpec spec;
  spec.name = kReaderPanelClassName;
  spec.style = CS_HREDRAW | CS_VREDRAW;
  Options options;
  options.style = WS_CLIPCHILDREN;
  const RECT nothing = {0, 0, 0, 0};
  if (!CreatePanel(spec, shell, nothing, options)) {
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
    ::SetParent(browser_hwnd_, hwnd());
    ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_FRAMECHANGED);
  }

  Reposition();
  RefreshState();
  // Above the shell's hosted browser child, or the page would occlude it.
  ShowPanel();
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
  // PhiSurfacePanel's spelling of the base's HWND; the two names meet here.
  return PhiWindow::hwnd();
}

PhiPalette PhiReaderPanel::QueryPalette() const {
  // The ORIGIN SLOT's palette, not its browser's: the two numbers only
  // coincide until that slot switches Space.
  if (coordinator_ && origin_) {
    return coordinator_->PaletteForSlot(origin_->slot().value);
  }
  return PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
}
void PhiReaderPanel::BindSurfaceTab(int64_t surface_tab_id) {
  surface_tab_id_ = surface_tab_id;
}

PhiReaderPanel::~PhiReaderPanel() {
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetParent(browser_hwnd_, nullptr);
  }
  // The base clears GWLP_USERDATA at WM_NCDESTROY, so nothing delivered
  // during the teardown can reach a half-destroyed object.
  Destroy();
}

void PhiReaderPanel::Reposition() {
  if (!alive() || !origin_) {
    return;
  }
  SyncDpiFromParent();
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
  SetPanelBounds(content);
  PositionContent();
  ApplyCardShape();
  Invalidate();
  // The caps were placed for the card this panel now covers; re-assert them
  // over it so the reading surface wears the same rounded corners.
  if (origin_) {
    origin_->RaiseCornerCaps();
  }
}

void PhiReaderPanel::ApplyCardShape() {
  if (!alive()) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
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
    ::SetWindowRgn(hwnd(), nullptr, TRUE);  // Square, as the card is.
    return;
  }
  // CreateRoundRectRgn takes the ELLIPSE size, so it is twice the radius.
  const int ellipse = Scaled(kContentRadius) * 2;
  // Rounding only the bottom pair is the status bubble's trick: oversize the
  // rect past the top edge so those corners' curves fall outside the window.
  const int top = round_top ? 0 : -ellipse;
  HRGN shape = ::CreateRoundRectRgn(0, top, client.right + 1,
                                    client.bottom + 1, ellipse, ellipse);
  ::SetWindowRgn(hwnd(), shape, TRUE);  // The window owns it now.
}

void PhiReaderPanel::Raise() {
  if (!IsPanelVisible()) {
    return;
  }
  RaisePanel();
}

void PhiReaderPanel::SetVisibleForActiveTab(int64_t active_tab_id) {
  if (!alive()) {
    return;
  }
  const bool mine = active_tab_id == origin_tab_id_;
  if (mine) {
    ShowPanel();
    if (origin_) {
      origin_->RaiseCornerCaps();
    }
  } else {
    HidePanel();
  }
}

void PhiReaderPanel::RefreshState() {
  if (!framework_ || expanded_) {
    return;
  }
  // Read the STRIP, not the nav state: a panel whose surface tab is being
  // removed (reader.close, or the reading page's own X) has an empty strip
  // and reports a placeholder page, which is not a navigation and must not
  // be mistaken for one — the browser is about to close on its own. The
  // strip is the model's snapshot of this browser, pulled typed when the
  // model has not been pushed it yet (the panel is built inside its
  // Browser::Create).
  const phi_framework::WindowSnapshot* snapshot =
      model_ ? model_->Get(window_id_) : nullptr;
  phi_framework::WindowSnapshot pulled;
  if (!snapshot && PhiWindowModel::Pull(framework_, window_id_, &pulled)) {
    snapshot = &pulled;
  }
  if (!snapshot || snapshot->tabs.size() != 1) {
    return;
  }
  const phi_framework::TabSnapshot& row = snapshot->tabs.front();
  const bool is_surface = row.surface == phi_framework::TabSurface::kReader;
  // The header shows the ORIGIN page's favicon (reader.html has none) and
  // the reading surface's url below it.
  if (framework_ && origin_tab_id_ > 0) {
    int width = 0;
    int height = 0;
    const unsigned char* pixels = framework_->TabFaviconBgraById(
        origin_window_id_.value, static_cast<int>(origin_tab_id_), &width,
        &height);
    favicon_.clear();
    favicon_w_ = favicon_h_ = 0;
    if (pixels && width > 0 && height > 0) {
      const size_t bytes =
          static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
      // SAFETY: the framework reports the buffer's own dimensions.
      const auto view =
          UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
      favicon_.assign(view.begin(), view.end());
      favicon_w_ = width;
      favicon_h_ = height;
    }
    if (alive()) {
      Invalidate(HeaderRect());
    }
  }
  if (!surface_tab_id_) {
    // The framework panelled the surface itself (the insertion divert), so
    // nobody bound it here: adopt the reader page standing for our origin.
    if (is_surface && row.origin.value == origin_tab_id_) {
      surface_tab_id_ = row.id.value;
    }
    return;
  }
  if (row.id.value != surface_tab_id_) {
    // Not the tab that was moved in: the surface has gone and this browser
    // is on its way out with a placeholder page. Nothing to hand back.
    return;
  }
  if (row.url.empty()) {
    return;
  }
  url_ = base::UTF8ToWide(row.url);
  if (is_surface || row.url == "about:blank") {
    return;
  }
  // A link clicked inside the reader. The reading surface is not a place to
  // browse from, so the destination becomes a normal tab in the origin
  // window and the panel goes away. Guarded so the teardown runs once.
  expanded_ = true;
  LOG(INFO) << "[PhiClient] reader panel " << window_id_.value
            << " navigated away — expanding into a tab";
  if (coordinator_) {
    coordinator_->ExpandReaderPanelIntoTab(window_id_.value, row.url);
  }
}

void PhiReaderPanel::PositionContent() {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const int header = HeaderRect().bottom;
  ::SetWindowPos(browser_hwnd_, nullptr, 0, header, client.right,
                 std::max(0, static_cast<int>(client.bottom) - header),
                 SWP_NOZORDER | SWP_NOACTIVATE);
  LayoutHits();
}

RECT PhiReaderPanel::HeaderRect() const {
  RECT client = {};
  if (alive()) {
    ::GetClientRect(hwnd(), &client);
  }
  return {0, 0, client.right, Scaled(kHeaderHeight)};
}

void PhiReaderPanel::LayoutHits() {
  ClearHits();
  if (!alive()) {
    return;
  }
  const RECT header = HeaderRect();
  const int btn = Scaled(kButtonSize);
  const int mid = header.bottom / 2;
  const RECT close = {header.right - Scaled(kButtonGap) - btn, mid - btn / 2,
                      header.right - Scaled(kButtonGap), mid + btn / 2};
  AddHit(close, 0, L"Close Reader", PhiHitRole::kButton);
}

void PhiReaderPanel::OnHover(int hit, int) {
  hot_ = hit;
  if (alive()) {
    Invalidate(HeaderRect());
  }
}

void PhiReaderPanel::OnClick(int hit, POINT) {
  if (hit == 0 && coordinator_) {
    coordinator_->CloseSurfacesForOrigin(
        TabId{static_cast<int32_t>(origin_tab_id_)}, SidebarSurface::kReader);
  }
}

int64_t PhiReaderPanel::surface_tab_id() const {
  return surface_tab_id_;
}

void PhiReaderPanel::Paint(HDC dc, const RECT&) {
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const PhiPalette palette = QueryPalette();
  HBRUSH ground = ::CreateSolidBrush(palette.ground_top);
  ::FillRect(dc, &client, ground);
  ::DeleteObject(ground);
  // The header: flyout ground, the origin page's favicon and host, Close.
  const RECT header = HeaderRect();
  HBRUSH band = ::CreateSolidBrush(palette.flyout);
  ::FillRect(dc, &header, band);
  ::DeleteObject(band);
  RECT rule = {header.left, header.bottom - 1, header.right, header.bottom};
  HBRUSH sep = ::CreateSolidBrush(palette.separator);
  ::FillRect(dc, &rule, sep);
  ::DeleteObject(sep);
  ::SetBkMode(dc, TRANSPARENT);
  const int mid = header.bottom / 2;
  const int icon = Scaled(kFaviconSize);
  const RECT favicon = {Scaled(kHeaderPad), mid - icon / 2,
                        Scaled(kHeaderPad) + icon, mid + icon / 2};
  if (!favicon_.empty()) {
    PhiDrawBgra(dc, favicon, favicon_.data(), favicon_w_, favicon_h_);
  } else {
    PhiDrawIconGlyph(dc, favicon, fluent::kGlobe, icon, palette.label_dim);
  }
  const int btn = Scaled(kButtonSize);
  const RECT close = {header.right - Scaled(kButtonGap) - btn, mid - btn / 2,
                      header.right - Scaled(kButtonGap), mid + btn / 2};
  std::wstring host = url_;
  const size_t scheme = host.find(L"://");
  if (scheme != std::wstring::npos) {
    host.erase(0, scheme + 3);
  }
  const size_t slash = host.find(L'/');
  if (slash != std::wstring::npos) {
    host.resize(slash);
  }
  PhiFontDesc text_desc;
  text_desc.size = 12;
  text_desc.face = PhiTextFaceName(/*display=*/false);
  HGDIOBJ old_font = ::SelectObject(dc, fonts().Get(text_desc));
  RECT text = {favicon.right + Scaled(8), header.top, close.left - Scaled(8),
               header.bottom};
  ::SetTextColor(dc, palette.label);
  PhiDrawTextAA(dc, host.c_str(), -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
  ::SelectObject(dc, old_font);
  if (hot_ == 0) {
    PhiFillRoundRectAA(dc, close, Scaled(kButtonRadius), palette.hover);
  }
  PhiDrawIconGlyph(dc, close, fluent::kCancel, Scaled(16), palette.label);
}

void PhiReaderPanel::OnSize(int, int) {
  PositionContent();
}

}  // namespace phi_client
