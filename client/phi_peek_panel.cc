// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_peek_panel.h"

#include "phibrowser-win/client/phi_dwrite.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"
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

}  // namespace

// ---------------------------------------------------------------------------
// The drop shadow: a LAYERED, click-through sibling beneath the card. Its
// pixels are a signed-distance blur of the card's silhouette, so the
// corners fade like the edges instead of stepping.

class PhiPeekShadow : public PhiPanel {
 public:
  PhiPeekShadow() = default;
  ~PhiPeekShadow() override { Destroy(); }

  bool Open(HWND shell) {
    ClassSpec spec;
    spec.name = kPeekShadowClassName;
    spec.cursor = nullptr;
    Options options;
    options.layered = true;
    // A click in the blur is a click on the page around the card, which
    // dismisses the peek.
    options.click_through = true;
    options.ex_style = WS_EX_NOPARENTNOTIFY;
    const RECT nothing = {0, 0, 0, 0};
    return CreatePanel(spec, shell, nothing, options);
  }

  // `radius`, `blur` and `margin` are already in device pixels.
  void Render(int width,
              int height,
              int margin,
              double radius,
              double blur,
              double base_alpha) {
    if (!alive() || width <= 0 || height <= 0) {
      return;
    }
    PhiLayerSurface layer(width, height);
    if (!layer.valid()) {
      return;
    }
    // The card's silhouette, centred in the surface: everything is expressed
    // as a signed distance to it, which is what makes the corners fade like
    // the edges instead of stepping.
    const double cx = width / 2.0;
    const double cy = height / 2.0;
    const double half_x = (width - 2.0 * margin) / 2.0 - radius;
    const double half_y = (height - 2.0 * margin) / 2.0 - radius;
    // SAFETY: the layer holds exactly width*height 32-bit pixels.
    const base::span<uint32_t> pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
        static_cast<uint32_t*>(layer.bits()),
        static_cast<size_t>(width) * height));
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
        // Centred on the edge — full strength half a blur inside, nothing
        // half a blur outside, half strength ON it. Ramping from the edge
        // outwards instead leaves a hard band of full black under the
        // offset card.
        const double t = std::clamp(0.5 - distance / blur, 0.0, 1.0);
        const double alpha = base_alpha * t * t * (3.0 - 2.0 * t);
        // Premultiplied black: the colour channels stay 0 at every alpha.
        pixels[static_cast<size_t>(y) * width + x] =
            static_cast<uint32_t>(alpha * 255.0 + 0.5) << 24;
      }
    }
    layer.Commit(hwnd(), nullptr);
  }
};

// ---------------------------------------------------------------------------
// The control chips: a SHAPED LAYER, not a region union.
//
// Lane 3 §3.6 finding 3: this window hosted no child and cut itself into
// three rounded rects with CreateRoundRectRgn + CombineRgn, so every chip
// corner was a staircase. The silhouette is now per-pixel alpha out of
// phi_dwrite (PhiPanel::LayerShapes), and WM_NCHITTEST answers HTTRANSPARENT
// off the chips — which is what the region said by simply not being there,
// so a click in the air between two chips still reaches the page and
// dismisses the peek.

class PhiPeekControls : public PhiPanel {
 public:
  using PaintFn = std::function<void(HDC)>;
  using HoverFn = std::function<void(int)>;
  using ClickFn = std::function<void(int)>;

  PhiPeekControls() = default;
  ~PhiPeekControls() override { Destroy(); }

  bool Open(HWND card, PaintFn paint, HoverFn hover, ClickFn click) {
    paint_ = std::move(paint);
    hover_ = std::move(hover);
    click_ = std::move(click);
    ClassSpec spec;
    spec.name = kPeekControlsClassName;
    spec.style = CS_HREDRAW | CS_VREDRAW;
    Options options;
    options.shaped_layer = true;
    options.ex_style = WS_EX_NOPARENTNOTIFY;
    const RECT nothing = {0, 0, 0, 0};
    return CreatePanel(spec, card, nothing, options);
  }

  // The chips in THIS window's client pixels, in hit order. Rebuilding them
  // rebuilds hits_, which is where hover and clicks come from.
  void SetChips(const std::vector<RECT>& chips, int radius) {
    chips_.clear();
    ClearHits();
    for (size_t i = 0; i < chips.size(); ++i) {
      PhiLayerShape shape;
      shape.bounds = PhiFromRect(chips[i]);
      shape.radius = radius;
      chips_.push_back(shape);
      AddHit(chips[i], static_cast<int>(i), std::wstring(),
             PhiHitRole::kButton);
    }
  }

 protected:
  void Paint(HDC dc, const RECT&) override {
    if (paint_) {
      paint_(dc);
    }
  }

  void LayerShapes(int, int, std::vector<PhiLayerShape>* out) const override {
    *out = chips_;
  }

  bool HitTestShape() const override { return true; }

  void OnHover(int hit, int) override {
    if (hover_) {
      hover_(hit);
    }
  }

  void OnClick(int hit, POINT) override {
    if (hit >= 0 && click_) {
      click_(hit);
    }
  }

 private:
  std::vector<PhiLayerShape> chips_;
  PaintFn paint_;
  HoverFn hover_;
  ClickFn click_;
};

PhiPeekPanel::PhiPeekPanel(int64_t window_id,
                           HWND browser_hwnd,
                           int64_t opener_window_id,
                           int64_t opener_tab_id,
                           PhiShellWindow* opener,
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
      coordinator_(coordinator),
      opened_tick_(::GetTickCount64()) {
  HWND shell = opener_ ? opener_->shell_hwnd() : nullptr;
  if (!shell) {
    LOG(ERROR) << "[PhiClient] peek has no opener shell";
    return;
  }

  shadow_ = std::make_unique<PhiPeekShadow>();
  shadow_->Open(shell);

  // A child of the opener's shell, above the hosted browser: it clips into the
  // content card and travels with the window, the same trick the bookmark
  // manager uses for its URL-mounted page.
  ClassSpec spec;
  spec.name = kPeekClassName;
  spec.style = CS_HREDRAW | CS_VREDRAW;
  Options options;
  // WS_CLIPCHILDREN for the hosted browser; WS_CLIPSIBLINGS is PhiPanel's
  // and keeps the card's corner caps visible.
  options.style = WS_CLIPCHILDREN;
  const RECT nothing = {0, 0, 0, 0};
  if (!CreatePanel(spec, shell, nothing, options)) {
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
    ::SetParent(browser_hwnd_, hwnd());
    ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_FRAMECHANGED);
  }

  // The chips are a child of the SHELL, not of the card, and sit right above
  // the card in the shell's card stack (Raise). A child of the card lost to
  // the hosted browser: HWNDMessageHandler::Activate() does SetWindowPos(
  // HWND_TOP) on Chromium's own window — on Show(), and again whenever the
  // page activates itself — which put the browser child above the chips as
  // soon as the peek was up, and no reposition came to lift them again. A
  // sibling of the card is out of that fight: nothing inside the card can
  // reorder above it.
  controls_ = std::make_unique<PhiPeekControls>();
  if (controls_->Open(
          shell, [this](HDC dc) { PaintControls(dc); },
          [this](int index) { HoverControl(index); },
          [this](int index) { ClickControl(index); })) {
    controls_->ShowPanel();
  }

  Reposition();
  RefreshState();
  Show();
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetFocus(browser_hwnd_);
  }
}

void PhiPeekPanel::Show() {
  // Above the shell's hosted browser child, or the page would occlude both.
  // The shadow goes up first so the card never ends up under its own shadow,
  // the chips last so they stay above the card; the card stack keeps that
  // order once all three are in it.
  if (shadow_) {
    shadow_->ShowPanel();
  }
  if (alive()) {
    ShowPanel();
  }
  if (controls_) {
    controls_->ShowPanel();
  }
}

void PhiPeekPanel::Raise() {
  // A z-order refresh for a peek that is UP. One that is down — its opener
  // is not the active tab — stays down: the caps raise (kMsgRaiseCaps) asks
  // every surface of the slot to come up, and used to bring another tab's
  // peek up over the page the user had switched to.
  if (!IsPanelVisible()) {
    return;
  }
  Show();
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
  // PhiSurfacePanel's spelling of the base's HWND; the two names meet here.
  return PhiWindow::hwnd();
}

bool PhiPeekPanel::ClickDismisses(POINT screen_point) const {
  const HWND shell = opener_ ? opener_->shell_hwnd() : nullptr;
  if (!shell) {
    return true;
  }
  POINT pt = screen_point;
  ::ScreenToClient(shell, &pt);
  const RECT content = opener_->ContentRect();
  return ::PtInRect(&content, pt) != FALSE;
}

bool PhiPeekPanel::ContainsWindow(HWND window) const {
  if (PhiSurfacePanel::ContainsWindow(window)) {
    return true;
  }
  // The chips are the shell's child, not the card's (see the constructor),
  // so a click on one is the card's own business too.
  return controls_ && controls_->alive() && window == controls_->hwnd();
}

PhiPalette PhiPeekPanel::QueryPalette() const {
  if (coordinator_ && opener_) {
    return coordinator_->PaletteForSlot(opener_->slot().value);
  }
  return PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
}

PhiPeekPanel::~PhiPeekPanel() {
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetParent(browser_hwnd_, nullptr);
  }
  // The chips first (they are this window's child), then the shadow, then
  // the card. Each destructor destroys its own HWND and the base clears
  // GWLP_USERDATA at WM_NCDESTROY, so no teardown message can reach a
  // half-destroyed object.
  controls_.reset();
  shadow_.reset();
  Destroy();
}

void PhiPeekPanel::Reposition() {
  if (!alive() || !opener_) {
    return;
  }
  SyncDpiFromParent();
  const RECT content = opener_->ContentRect();
  const int width = content.right - content.left;
  const int height = content.bottom - content.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  // The card sits CENTRED in the page card, inset by the Mac's margins on
  // both sides (Jixiang: never narrowed on the right for the chips). The
  // chips stand in the right margin, beside the card and over the page,
  // never on the page the card shows — PositionControls lays them out in
  // the strip Reposition leaves there.
  const int inset_x =
      std::max(Scaled(kMinPaneInset),
               static_cast<int>(width * kPaneInsetRatio));
  const int inset_y = Scaled(kPaneVerticalInset);
  right_margin_ = inset_x;
  const RECT bounds = {content.left + inset_x, content.top + inset_y,
                       content.left + inset_x + std::max(0, width - inset_x * 2),
                       content.top + inset_y +
                           std::max(0, height - inset_y * 2)};
  SetPanelBounds(bounds);
  ApplyCardShape();
  PositionContent();
  UpdateShadow();
  Invalidate();
}

void PhiPeekPanel::ApplyCardShape() {
  if (!alive()) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
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
  ::SetWindowRgn(hwnd(), shape, TRUE);  // The window owns it now.
}

void PhiPeekPanel::UpdateShadow() {
  if (!shadow_ || !shadow_->alive() || !alive()) {
    return;
  }
  RECT card = {};
  ::GetWindowRect(hwnd(), &card);
  const int margin = Scaled(kShadowMargin);
  const int width = (card.right - card.left) + margin * 2;
  const int height = (card.bottom - card.top) + margin * 2;
  if (width <= 0 || height <= 0) {
    return;
  }
  // The card's own top-left, in the shell's client space, then out by the
  // margin and down by the shadow's offset.
  POINT origin = {card.left, card.top};
  ::ScreenToClient(shadow_->parent(), &origin);
  const RECT bounds = {origin.x - margin,
                       origin.y - margin + Scaled(kShadowOffsetY),
                       origin.x - margin + width,
                       origin.y - margin + Scaled(kShadowOffsetY) + height};
  shadow_->SetPanelBounds(bounds);

  const PhiPalette colors = QueryPalette();
  shadow_->Render(width, height, margin, Scaled(kCardRadius),
                  std::max(1, Scaled(kShadowBlur)),
                  colors.dark ? kShadowAlphaDark : kShadowAlphaLight);
}

void PhiPeekPanel::SetVisibleForActiveTab(int64_t active_tab_id) {
  if (!alive()) {
    return;
  }
  const bool mine = active_tab_id == opener_tab_id_;
  if (mine) {
    Show();  // The shadow, then the card, then the chips, in that order.
  } else {
    HidePanel();
    if (shadow_) {
      shadow_->HidePanel();
    }
    if (controls_) {
      controls_->HidePanel();
    }
  }
}

void PhiPeekPanel::RefreshState() {
  // The peek's own browser's snapshot: the model's copy, or — for a browser
  // the model has not been pushed yet (this panel is built inside its
  // Browser::Create) — pulled typed from the framework.
  const phi_framework::WindowSnapshot* snapshot =
      model_ ? model_->Get(window_id_) : nullptr;
  phi_framework::WindowSnapshot pulled;
  if (!snapshot && PhiWindowModel::Pull(framework_, window_id_, &pulled)) {
    snapshot = &pulled;
  }
  if (snapshot) {
    url_ = Widen(snapshot->nav.url);
  }
  if (alive()) {
    Invalidate();
  }
}

PhiPeekPanel::Layout PhiPeekPanel::ComputeLayout() const {
  Layout out = {};
  RECT client = {};
  if (alive()) {
    ::GetClientRect(hwnd(), &client);
  }
  const int gutter = Scaled(kControlGutterWidth);
  const int btn = Scaled(kControlButtonSize);
  const int spacing = Scaled(kControlSpacing);
  const int top = Scaled(kControlTopInset);

  // The page takes the whole card; the controls stand in the page card's
  // right margin, beside the card and over the page around the peek, the
  // way the Mac's control column does — so the card reads as a page and the
  // chips never cover what it shows. The strip is the margin Reposition
  // measured (the Mac's 44 gutter where the margin has room for it), spelled
  // in the card's client space past its right edge; PositionControls carries
  // it into the shell's.
  const int strip = std::min(gutter, std::max(btn, right_margin_));
  out.card = {0, 0, client.right, client.bottom};
  out.gutter = {client.right, 0, client.right + strip,
                top * 2 + btn * 3 + spacing * 2};

  const int cx = out.gutter.left + (strip - btn) / 2;
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
    PositionControls();
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
  if (!controls_ || !controls_->alive()) {
    return;
  }
  const Layout layout = ComputeLayout();
  const int width = layout.gutter.right - layout.gutter.left;
  const int height = layout.gutter.bottom - layout.gutter.top;
  if (width <= 0 || height <= 0) {
    return;
  }
  // The gutter is laid out in the card's client space; the chips live in the
  // shell's, so the card's own origin carries them over.
  RECT card = {};
  ::GetWindowRect(hwnd(), &card);
  POINT origin_in_shell = {card.left, card.top};
  ::ScreenToClient(controls_->parent(), &origin_in_shell);
  const RECT gutter = {origin_in_shell.x + layout.gutter.left,
                       origin_in_shell.y + layout.gutter.top,
                       origin_in_shell.x + layout.gutter.right,
                       origin_in_shell.y + layout.gutter.bottom};
  controls_->SetPanelBounds(gutter);
  // Only the chips themselves are the layer — the space between them is
  // transparent, so the page shows through and a click there lands on the
  // page. This used to be a CreateRoundRectRgn union, which stair-stepped
  // every chip corner (lane 3 §3.6 finding 3).
  const POINT origin = {layout.gutter.left, layout.gutter.top};
  const auto shift = [&origin](const RECT& rc) {
    return RECT{rc.left - origin.x, rc.top - origin.y, rc.right - origin.x,
                rc.bottom - origin.y};
  };
  const std::vector<RECT> chips = {shift(layout.close), shift(layout.expand),
                                   shift(layout.split)};
  controls_->SetChips(chips, Scaled(kControlRadius));
  controls_->Invalidate();
}

void PhiPeekPanel::Paint(HDC dc, const RECT&) {
  // The page covers the whole card; this only shows through in the moment
  // before the browser child has drawn, and behind its rounded corners.
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  // Live, as before: the opener slot's palette ramps between frames.
  HBRUSH ground = ::CreateSolidBrush(QueryPalette().card);
  ::FillRect(dc, &client, ground);
  ::DeleteObject(ground);
}

void PhiPeekPanel::OnSize(int, int) {
  PositionContent();
}

void PhiPeekPanel::PaintControls(HDC dc) {
  const PhiPalette palette = QueryPalette();
  const Layout layout = ComputeLayout();
  const POINT origin = {layout.gutter.left, layout.gutter.top};

  PhiFontDesc glyph_desc;
  glyph_desc.size = 11;
  glyph_desc.face = PhiIconFaceName();
  HGDIOBJ old_font = ::SelectObject(dc, fonts().Get(glyph_desc));
  ::SetBkMode(dc, TRANSPARENT);

  // The chips read against arbitrary page content, so each is an opaque
  // rounded plate — the Mac's 8pt corners — not a bare glyph. The plate's
  // FILL is square here on purpose: the LAYER's alpha carries the rounded
  // silhouette, so an antialiased fill under it would blend its own edge
  // against an empty buffer and leave a dark fringe once the mask ramps.
  auto plate = [&](int index, const RECT& rc) {
    const int chip = Scaled(kControlRadius);
    HBRUSH fill = ::CreateSolidBrush(index == hot_ ? palette.selected
                                                   : palette.card);
    ::FillRect(dc, &rc, fill);
    ::DeleteObject(fill);
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
}

void PhiPeekPanel::HoverControl(int index) {
  // The chip layer's hits_ ARE the chips, in kHotClose / kHotExpand /
  // kHotSplit order, so the hovered index is the hot control. The base has
  // already invalidated the two chip rects that changed.
  hot_ = index;
}

void PhiPeekPanel::NoteEndReason(const char* reason) {
  end_reason_ = reason;
}

void PhiPeekPanel::Dismiss() {
  if (framework_) {
    framework_->CloseBrowserWindow(window_id_.value);
  }
}

void PhiPeekPanel::ClickControl(int index) {
  // The chip names the end for the metric (the Mac's PeekViewAnalytics
  // reasons); the close that follows is what reports it.
  if (index == kHotClose) {
    NoteEndReason("closed");
    Dismiss();
  } else if (index == kHotExpand) {
    // Expand: the page becomes a real tab in the opener window. The Mac moves
    // the live tab across; here the peek is its own browser, so the URL is
    // re-opened and the preview closed (see phi_peek_window.h).
    if (framework_ && !url_.empty()) {
      framework_->NewTab(opener_window_id_.value, Narrow(url_).c_str());
    }
    NoteEndReason("opened_as_tab");
    Dismiss();
  } else if (index == kHotSplit) {
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
    NoteEndReason("opened_as_split");
    Dismiss();
  }
}

}  // namespace phi_client
