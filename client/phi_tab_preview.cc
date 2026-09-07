// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_tab_preview.h"

#include <dwmapi.h>

#include <algorithm>
#include <string>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

// TabPreviewView.Metrics, in DIP. The snapshot keeps the Mac's 16:10 crop.
constexpr int kCardWidth = 280;
constexpr int kImageHeight = kCardWidth * 10 / 16;
constexpr int kPadX = 12;
constexpr int kPadY = 10;
constexpr int kTextGap = 4;   // Title-to-URL spacing.
constexpr int kTitleSize = 14;
constexpr int kUrlSize = 12;
// CustomTooltipGeometry: the gap an attached placement does NOT take, and
// the margin the card keeps off the screen edge.
constexpr int kScreenMargin = 4;

const wchar_t kClassName[] = L"PhiTabPreview";

void RegisterPreviewClass() {
  static const bool registered = [] {
    WNDCLASSW cls = {};
    cls.lpfnWndProc = &PhiTabPreview::WndProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.lpszClassName = kClassName;
    return ::RegisterClassW(&cls) != 0;
  }();
  (void)registered;
}

// SwiftUI's .truncationMode(.middle). GDI has DT_PATH_ELLIPSIS, but that is
// BACKSLASH-path logic — handed a URL it collapses the line to nothing — and
// DT_END_ELLIPSIS would drop the tail the Mac deliberately keeps.
std::wstring MiddleElide(HDC dc, const std::wstring& text, int width) {
  if (text.empty() || width <= 0) {
    return std::wstring();
  }
  SIZE extent = {};
  if (::GetTextExtentPoint32W(dc, text.c_str(),
                              static_cast<int>(text.size()), &extent) &&
      extent.cx <= width) {
    return text;
  }
  const std::wstring kEllipsis = L"…";
  std::wstring best = kEllipsis;
  int lo = 0;
  int hi = static_cast<int>(text.size());
  while (lo <= hi) {
    const int keep = (lo + hi) / 2;
    const size_t head = static_cast<size_t>((keep + 1) / 2);
    const size_t tail = static_cast<size_t>(keep / 2);
    std::wstring candidate =
        text.substr(0, head) + kEllipsis + text.substr(text.size() - tail);
    if (::GetTextExtentPoint32W(dc, candidate.c_str(),
                                static_cast<int>(candidate.size()),
                                &extent) &&
        extent.cx <= width) {
      best = std::move(candidate);
      lo = keep + 1;
    } else {
      hi = keep - 1;
    }
  }
  return best;
}

// NSAnimationContext's .easeInEaseOut, near enough for a 180ms slide.
double EaseInOut(double t) {
  t = std::clamp(t, 0.0, 1.0);
  return t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
}

int Lerp(int from, int to, double t) {
  return from + static_cast<int>((to - from) * t + (to >= from ? 0.5 : -0.5));
}

}  // namespace

PhiTabPreview::PhiTabPreview(PhiWindowController* controller)
    : controller_(controller) {
  if (controller_ && controller_->shell_hwnd()) {
    dpi_ = ::GetDpiForWindow(controller_->shell_hwnd());
  }
}

PhiTabPreview::~PhiTabPreview() {
  Hide(/*preserve_warmth=*/false);
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::DestroyWindow(hwnd_);
  }
  for (HFONT font : {title_font_, url_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
}

void PhiTabPreview::EnsureFonts() {
  if (title_font_) {
    return;
  }
  auto make = [&](int size, int weight) {
    return ::CreateFontW(-Scaled(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, L"Segoe UI");
  };
  title_font_ = make(kTitleSize, FW_SEMIBOLD);
  url_font_ = make(kUrlSize, FW_NORMAL);
}

void PhiTabPreview::EnsureWindow() {
  if (hwnd_ && ::IsWindow(hwnd_)) {
    return;
  }
  HWND owner = controller_ ? controller_->shell_hwnd() : nullptr;
  if (!owner) {
    return;
  }
  RegisterPreviewClass();
  // Owned by the shell, so Windows keeps it above the shell AND above the
  // Chromium child inside it — without WS_EX_TOPMOST, which would leave the
  // card floating over other applications.
  hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName,
                            L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr,
                            ::GetModuleHandleW(nullptr), this);
  if (!hwnd_) {
    return;
  }
  ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                          sizeof(corner));
}

bool PhiTabPreview::IsWarm() const {
  return warm_until_ != 0 && ::GetTickCount64() < warm_until_;
}

void PhiTabPreview::ArmDismiss() {
  if (!controller_ || !controller_->shell_hwnd() || dismiss_armed_) {
    return;
  }
  dismiss_armed_ = true;
  ::SetTimer(controller_->shell_hwnd(), kDismissTimer, kHandoffDelayMs,
             nullptr);
}

void PhiTabPreview::CancelDismiss() {
  if (!dismiss_armed_) {
    return;
  }
  dismiss_armed_ = false;
  if (controller_ && controller_->shell_hwnd()) {
    ::KillTimer(controller_->shell_hwnd(), kDismissTimer);
  }
}

void PhiTabPreview::PointerEntered(const Target& target) {
  if (!controller_ || target.tab_id == 0) {
    PointerExited();
    return;
  }
  // Re-entering the row the card already belongs to must not restart
  // anything — the controller re-resolves hover on every mouse move.
  if (visible_ && shown_.key == target.key) {
    shown_.anchor = target.anchor;
    CancelDismiss();
    return;
  }
  if (armed_ && pending_.key == target.key) {
    pending_.anchor = target.anchor;
    return;
  }
  // Suppression is per-row: leaving the clicked row re-arms everything.
  if (!suppressed_key_.empty()) {
    if (suppressed_key_ == target.key) {
      return;
    }
    suppressed_key_.clear();
  }

  // Coming from another row cancels that row's pending dismissal: this is a
  // handoff, not a leave.
  CancelDismiss();
  pending_ = target;

  if (visible_ || IsWarm()) {
    // The Mac's hasActiveRequestInGroup / isWarm path: straight to screen,
    // with only the frame animating.
    armed_ = false;
    if (controller_->shell_hwnd()) {
      ::KillTimer(controller_->shell_hwnd(), kShowTimer);
    }
    Present();
    return;
  }

  armed_ = true;
  if (HWND owner = controller_->shell_hwnd()) {
    ::SetTimer(owner, kShowTimer, kShowDelayMs, nullptr);
  }
}

void PhiTabPreview::PointerExited() {
  suppressed_key_.clear();
  armed_ = false;
  pending_ = Target();
  if (controller_ && controller_->shell_hwnd()) {
    ::KillTimer(controller_->shell_hwnd(), kShowTimer);
  }
  // A visible card does NOT go down on the spot: the gap between two rows is
  // a leave, and dropping the card there would make every row-to-row move
  // pay the cold delay again. Give the next row the Mac's handoff grace.
  if (visible_) {
    ArmDismiss();
    return;
  }
  Hide(/*preserve_warmth=*/false);
}

void PhiTabPreview::Cancel() {
  // Whichever row the interaction landed on is the one to hold down.
  std::string key = visible_ ? shown_.key : pending_.key;
  armed_ = false;
  pending_ = Target();
  if (controller_ && controller_->shell_hwnd()) {
    ::KillTimer(controller_->shell_hwnd(), kShowTimer);
  }
  CancelDismiss();
  Hide(/*preserve_warmth=*/false);
  suppressed_key_ = std::move(key);
}

bool PhiTabPreview::OnTimer(UINT_PTR id) {
  if (id == kShowTimer) {
    if (armed_) {
      armed_ = false;
      if (controller_ && controller_->shell_hwnd()) {
        ::KillTimer(controller_->shell_hwnd(), kShowTimer);
      }
      Present();
    }
    return true;
  }
  if (id == kDismissTimer) {
    CancelDismiss();
    Hide(/*preserve_warmth=*/true);
    return true;
  }
  if (id == kSlideTimer) {
    StepSlide();
    return true;
  }
  return false;
}

bool PhiTabPreview::Resolve() {
  snapshot_.clear();
  snapshot_width_ = 0;
  snapshot_height_ = 0;
  snapshot_b_.clear();
  snapshot_b_width_ = 0;
  snapshot_b_height_ = 0;
  compact_ = false;
  if (!controller_ || pending_.tab_id == 0) {
    return false;
  }
  const bool split = pending_.tab_id_b != 0;
  // The Mac's previewMode: a pane the user is already looking at, or a page
  // the snapshot policy refuses, drops the card to text only.
  compact_ = pending_.foreground || !pending_.allow_snapshot ||
             (split && pending_.foreground_b);
  if (compact_) {
    return true;
  }
  phi_framework::PhiFramework* framework = controller_->framework();
  if (!framework) {
    compact_ = true;
    return true;
  }
  const BrowserId browser = controller_->active_browser_id();
  if (!browser.valid()) {
    compact_ = true;  // No browser attached: no page to snapshot.
    return true;
  }
  const auto pull = [&](int tab_id, std::vector<unsigned char>* out,
                        int* out_width, int* out_height) {
    int width = 0;
    int height = 0;
    const unsigned char* pixels =
        framework->TabThumbnailBgra(browser.value, tab_id, &width, &height);
    if (!pixels || width <= 0 || height <= 0) {
      return;
    }
    // The framework's buffer is scratch — valid only until its next call, so
    // pane A is copied out before pane B is asked for.
    const size_t bytes =
        static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    // SAFETY: TabThumbnailBgra reports the dimensions of the buffer it just
    // returned, the same contract TabFaviconBgra carries.
    const auto copy =
        UNSAFE_BUFFERS(base::span<const unsigned char>(pixels, bytes));
    out->assign(copy.begin(), copy.end());
    *out_width = width;
    *out_height = height;
  };
  pull(pending_.tab_id, &snapshot_, &snapshot_width_, &snapshot_height_);
  if (split) {
    pull(pending_.tab_id_b, &snapshot_b_, &snapshot_b_width_,
         &snapshot_b_height_);
  }
  return true;
}

int PhiTabPreview::TextBlockHeight(const std::wstring& title,
                                   const std::wstring& url,
                                   int width) const {
  HDC screen = ::GetDC(nullptr);
  if (!screen) {
    return 0;
  }
  HGDIOBJ prev = ::SelectObject(screen, title_font_);
  RECT measure = {0, 0, std::max(1, width), 0};
  ::DrawTextW(screen, title.c_str(), -1, &measure,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  TEXTMETRICW metrics = {};
  ::GetTextMetricsW(screen, &metrics);
  const int line = metrics.tmHeight;
  int height = std::min<int>(measure.bottom - measure.top, 2 * line);
  height = std::max(height, line);
  if (!url.empty()) {
    ::SelectObject(screen, url_font_);
    ::GetTextMetricsW(screen, &metrics);
    height += Scaled(kTextGap) + metrics.tmHeight;
  }
  ::SelectObject(screen, prev);
  ::ReleaseDC(nullptr, screen);
  return height;
}

SIZE PhiTabPreview::CardSize() const {
  const int width = Scaled(kCardWidth);
  const int pad_x = Scaled(kPadX);
  const int pad_y = Scaled(kPadY);
  SIZE size = {width, 0};

  if (!IsSplit()) {
    // A single tab shows its band only when there IS a cached snapshot —
    // the Mac's `if let image`, no placeholder.
    const int band =
        (!compact_ && !snapshot_.empty()) ? Scaled(kImageHeight) : 0;
    size.cy = band + TextBlockHeight(shown_.title, shown_.url,
                                     width - 2 * pad_x) +
              2 * pad_y;
    return size;
  }

  if (!shown_.stacked) {
    // Side by side (SplitLayout.vertical): one full-height band across two
    // half-width panes, sized to the taller text block.
    const int pane_w = (width - 1) / 2;
    const int band = compact_ ? 0 : Scaled(kImageHeight);
    const int text = std::max(
        TextBlockHeight(shown_.title, shown_.url, pane_w - 2 * pad_x),
        TextBlockHeight(shown_.title_b, shown_.url_b, pane_w - 2 * pad_x));
    size.cy = band + text + 2 * pad_y;
    return size;
  }

  // Stacked (SplitLayout.horizontal): two full-width panes, each on a
  // half-height band, with a hairline between them.
  const int band = compact_ ? 0 : Scaled(kImageHeight) / 2;
  const int avail = width - 2 * pad_x;
  size.cy = band + TextBlockHeight(shown_.title, shown_.url, avail) +
            2 * pad_y + 1 + band +
            TextBlockHeight(shown_.title_b, shown_.url_b, avail) + 2 * pad_y;
  return size;
}

POINT PhiTabPreview::CardOrigin(const SIZE& size) const {
  const RECT& anchor = shown_.anchor;
  POINT origin = {};
  switch (shown_.placement) {
    case Placement::kRightTopAttached:
      origin = {anchor.right, anchor.top};
      break;
    case Placement::kBelowAttached:
      origin = {(anchor.left + anchor.right) / 2 - size.cx / 2, anchor.bottom};
      break;
  }

  RECT work = {};
  HMONITOR monitor = ::MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info = {sizeof(info)};
  if (::GetMonitorInfoW(monitor, &info)) {
    work = info.rcWork;
  } else {
    return origin;
  }

  // Flip to the other side when the preferred one does not fit, exactly as
  // CustomTooltipGeometry does, before clamping.
  const LONG margin = kScreenMargin;
  if (shown_.placement == Placement::kRightTopAttached) {
    const LONG left_x = anchor.left - size.cx;
    if (origin.x + size.cx > work.right - margin &&
        left_x >= work.left + margin) {
      origin.x = left_x;
    }
  } else {
    const LONG above_y = anchor.top - size.cy;
    if (origin.y + size.cy > work.bottom - margin &&
        above_y >= work.top + margin) {
      origin.y = above_y;
    }
  }

  const LONG min_x = work.left + margin;
  const LONG min_y = work.top + margin;
  const LONG max_x = std::max(min_x, work.right - size.cx - margin);
  const LONG max_y = std::max(min_y, work.bottom - size.cy - margin);
  origin.x = std::min(std::max(origin.x, min_x), max_x);
  origin.y = std::min(std::max(origin.y, min_y), max_y);
  return origin;
}

void PhiTabPreview::Present() {
  if (!controller_ || pending_.tab_id == 0) {
    return;
  }
  if (!Resolve()) {
    Hide(/*preserve_warmth=*/false);
    return;
  }
  EnsureWindow();
  EnsureFonts();
  if (!hwnd_) {
    return;
  }
  const bool handoff = visible_;
  shown_ = pending_;
  const SIZE size = CardSize();
  const POINT origin = CardOrigin(size);
  const RECT target = {origin.x, origin.y, origin.x + size.cx,
                       origin.y + size.cy};

  if (handoff) {
    // Slide the panel to its new row instead of blinking it out and back.
    ::GetWindowRect(hwnd_, &slide_from_);
    slide_to_ = target;
    slide_elapsed_ms_ = 0;
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    if (HWND owner = controller_->shell_hwnd()) {
      ::SetTimer(owner, kSlideTimer, kSlideTickMs, nullptr);
    }
    return;
  }

  ::SetWindowPos(hwnd_, nullptr, target.left, target.top, size.cx, size.cy,
                 SWP_NOACTIVATE | SWP_NOZORDER);
  ::InvalidateRect(hwnd_, nullptr, TRUE);
  ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  visible_ = true;
  warm_until_ = 0;
}

void PhiTabPreview::StepSlide() {
  if (!hwnd_ || !::IsWindow(hwnd_)) {
    return;
  }
  slide_elapsed_ms_ += kSlideTickMs;
  const double t = EaseInOut(static_cast<double>(slide_elapsed_ms_) /
                             kSlideDurationMs);
  const int x = Lerp(slide_from_.left, slide_to_.left, t);
  const int y = Lerp(slide_from_.top, slide_to_.top, t);
  const int w = Lerp(slide_from_.right - slide_from_.left,
                     slide_to_.right - slide_to_.left, t);
  const int h = Lerp(slide_from_.bottom - slide_from_.top,
                     slide_to_.bottom - slide_to_.top, t);
  ::SetWindowPos(hwnd_, nullptr, x, y, w, h,
                 SWP_NOACTIVATE | SWP_NOZORDER);
  ::InvalidateRect(hwnd_, nullptr, TRUE);
  if (slide_elapsed_ms_ >= kSlideDurationMs && controller_) {
    if (HWND owner = controller_->shell_hwnd()) {
      ::KillTimer(owner, kSlideTimer);
    }
  }
}

void PhiTabPreview::Hide(bool preserve_warmth) {
  if (controller_ && controller_->shell_hwnd()) {
    ::KillTimer(controller_->shell_hwnd(), kSlideTimer);
  }
  const bool was_visible = visible_;
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::ShowWindow(hwnd_, SW_HIDE);
  }
  visible_ = false;
  shown_ = Target();
  snapshot_.clear();
  snapshot_width_ = 0;
  snapshot_height_ = 0;
  snapshot_b_.clear();
  snapshot_b_width_ = 0;
  snapshot_b_height_ = 0;
  compact_ = false;
  // Warmth only means anything after a card was actually up: a hover landing
  // inside the grace skips the cold delay, as on Mac.
  warm_until_ = (preserve_warmth && was_visible)
                    ? ::GetTickCount64() + kWarmGraceMs
                    : 0;
}

void PhiTabPreview::DrawPane(HDC dc,
                             const RECT& bounds,
                             int image_height,
                             const std::vector<unsigned char>& shot,
                             int shot_width,
                             int shot_height,
                             const std::wstring& title,
                             const std::wstring& url,
                             const PhiPalette& palette) {
  int y = bounds.top;
  if (image_height > 0) {
    RECT band = {bounds.left, y, bounds.right, y + image_height};
    if (!shot.empty()) {
      // Aspect-fill the band: crop the snapshot's overhang rather than
      // squashing it, the way SwiftUI's .aspectRatio(.fill) + .clipped() does.
      const double target = static_cast<double>(band.right - band.left) /
                            std::max(1, static_cast<int>(band.bottom - band.top));
      const double source =
          static_cast<double>(shot_width) / std::max(1, shot_height);
      RECT fill = band;
      if (source > target) {
        const int wide =
            static_cast<int>((band.bottom - band.top) * source + 0.5);
        const int overhang = (wide - (band.right - band.left)) / 2;
        fill.left -= overhang;
        fill.right += overhang;
      } else if (source < target) {
        const int tall =
            static_cast<int>((band.right - band.left) / source + 0.5);
        const int overhang = (tall - (band.bottom - band.top)) / 2;
        fill.top -= overhang;
        fill.bottom += overhang;
      }
      HRGN clip = ::CreateRectRgnIndirect(&band);
      ::SelectClipRgn(dc, clip);
      PhiDrawBgra(dc, fill, shot.data(), shot_width, shot_height);
      ::SelectClipRgn(dc, nullptr);
      ::DeleteObject(clip);
    } else {
      // A pane whose snapshot has not been recorded yet still keeps its
      // frame, so a split card never goes lopsided.
      HBRUSH ground = ::CreateSolidBrush(palette.hover);
      ::FillRect(dc, &band, ground);
      ::DeleteObject(ground);
    }
    y = band.bottom;
  }

  RECT text = {bounds.left + Scaled(kPadX), y + Scaled(kPadY),
               bounds.right - Scaled(kPadX), bounds.bottom - Scaled(kPadY)};
  if (text.right <= text.left || text.bottom <= text.top) {
    return;
  }
  HGDIOBJ prev = ::SelectObject(dc, title_font_);
  ::SetTextColor(dc, palette.label);
  TEXTMETRICW metrics = {};
  ::GetTextMetricsW(dc, &metrics);
  // Reserve what the title ACTUALLY takes, capped at two lines — reserving
  // the cap unconditionally would push the URL past the bottom of a card
  // that CardSize() measured for a one-line title, and it would never draw.
  RECT measured = {0, 0, text.right - text.left, 0};
  ::DrawTextW(dc, title.c_str(), -1, &measured,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  const LONG title_height =
      std::max<LONG>(metrics.tmHeight,
                     std::min<LONG>(measured.bottom - measured.top,
                                    2 * metrics.tmHeight));
  RECT title_rc = text;
  title_rc.bottom = std::min<LONG>(text.bottom, text.top + title_height);
  ::DrawTextW(dc, title.c_str(), -1, &title_rc,
              DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
  if (!url.empty()) {
    ::SelectObject(dc, url_font_);
    ::SetTextColor(dc, palette.label_dim);
    RECT url_rc = text;
    url_rc.top = title_rc.bottom + Scaled(kTextGap);
    if (url_rc.top < url_rc.bottom) {
      const std::wstring line =
          MiddleElide(dc, url, url_rc.right - url_rc.left);
      ::DrawTextW(dc, line.c_str(), -1, &url_rc,
                  DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    }
  }
  ::SelectObject(dc, prev);
}

void PhiTabPreview::Paint(HDC dc) {
  if (!controller_) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd_, &rc);
  const PhiPalette palette = controller_->CurrentPalette();

  // The Mac card is .themedBackground(.windowBackground) — DWM rounds the
  // corners, so the fill is a plain rect.
  HBRUSH ground = ::CreateSolidBrush(palette.window_bg);
  ::FillRect(dc, &rc, ground);
  ::DeleteObject(ground);
  ::SetBkMode(dc, TRANSPARENT);

  if (!IsSplit()) {
    const int band =
        (!compact_ && !snapshot_.empty()) ? Scaled(kImageHeight) : 0;
    DrawPane(dc, rc, band, snapshot_, snapshot_width_, snapshot_height_,
             shown_.title, shown_.url, palette);
  } else if (!shown_.stacked) {
    const int band = compact_ ? 0 : Scaled(kImageHeight);
    const LONG split_x = rc.left + (rc.right - rc.left - 1) / 2;
    RECT left = {rc.left, rc.top, split_x, rc.bottom};
    RECT right = {split_x + 1, rc.top, rc.right, rc.bottom};
    DrawPane(dc, left, band, snapshot_, snapshot_width_, snapshot_height_,
             shown_.title, shown_.url, palette);
    DrawPane(dc, right, band, snapshot_b_, snapshot_b_width_,
             snapshot_b_height_, shown_.title_b, shown_.url_b, palette);
    RECT divider = {split_x, rc.top, split_x + 1, rc.bottom};
    HBRUSH line = ::CreateSolidBrush(palette.separator);
    ::FillRect(dc, &divider, line);
    ::DeleteObject(line);
  } else {
    const int band = compact_ ? 0 : Scaled(kImageHeight) / 2;
    const int top_h =
        band + TextBlockHeight(shown_.title, shown_.url,
                               rc.right - rc.left - 2 * Scaled(kPadX)) +
        2 * Scaled(kPadY);
    RECT top = {rc.left, rc.top, rc.right, rc.top + top_h};
    RECT bottom = {rc.left, top.bottom + 1, rc.right, rc.bottom};
    DrawPane(dc, top, band, snapshot_, snapshot_width_, snapshot_height_,
             shown_.title, shown_.url, palette);
    DrawPane(dc, bottom, band, snapshot_b_, snapshot_b_width_,
             snapshot_b_height_, shown_.title_b, shown_.url_b, palette);
    RECT divider = {rc.left, top.bottom, rc.right, top.bottom + 1};
    HBRUSH line = ::CreateSolidBrush(palette.separator);
    ::FillRect(dc, &divider, line);
    ::DeleteObject(line);
  }

  // A hairline rim, as the Mac tooltip chrome carries.
  HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
  ::Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
  ::SelectObject(dc, old_brush);
  ::SelectObject(dc, old_pen);
  ::DeleteObject(pen);
}

// static
LRESULT CALLBACK PhiTabPreview::WndProc(HWND hwnd,
                                        UINT msg,
                                        WPARAM w,
                                        LPARAM l) {
  auto* self = reinterpret_cast<PhiTabPreview*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCHITTEST:
      // Never take the pointer: the card lives entirely off the hover it is
      // showing for.
      return HTTRANSPARENT;
    case WM_ERASEBKGND:
      return 1;  // Paint() covers the whole client rect.
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        // Through a back buffer: the handoff slide resizes the panel every
        // frame, and painting straight to the window tears the card apart
        // between the band fill and the text.
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        const int card_w = rc.right - rc.left;
        const int card_h = rc.bottom - rc.top;
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP buffer =
            mem ? ::CreateCompatibleBitmap(dc, card_w, card_h) : nullptr;
        if (mem && buffer) {
          HGDIOBJ prev = ::SelectObject(mem, buffer);
          self->Paint(mem);
          ::BitBlt(dc, 0, 0, card_w, card_h, mem, 0, 0, SRCCOPY);
          ::SelectObject(mem, prev);
        } else {
          self->Paint(dc);
        }
        if (buffer) {
          ::DeleteObject(buffer);
        }
        if (mem) {
          ::DeleteDC(mem);
        }
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

}  // namespace phi_client
