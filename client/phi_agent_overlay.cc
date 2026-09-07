// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_agent_overlay.h"

#include <windowsx.h>

// GDI+ draws the sprite layer (cursor + effects): antialiased paths with
// per-pixel alpha, which plain GDI cannot produce.
#include <objidl.h>

#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

constexpr wchar_t kFogClass[] = L"PhiAgentFog";
constexpr wchar_t kPillClass[] = L"PhiAgentPill";
// The sprite rides ABOVE the fog: a per-pixel-alpha layered child carrying
// the agent cursor and the transient input-mirror effects.
constexpr wchar_t kSpriteClass[] = L"PhiAgentSprite";
// The fog is a light wash, not a blackout: the user must still SEE what the
// agent is doing — that is the whole point of watch mode.
constexpr BYTE kFogAlpha = 46;
constexpr int kPillHeight = 44;
constexpr int kPillMaxWidth = 560;
constexpr int kPillBottomGap = 24;
constexpr int kButtonHeight = 26;
// The animation clock (~30fps) and the sprite timer id.
constexpr UINT kSpriteTickMs = 33;
constexpr UINT_PTR kSpriteTimer = 0x531;

// GDI+ is started once for the process and never shut down (the capture
// path's reasoning applies here too).
bool EnsureGdiPlus() {
  static const bool ok = [] {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
  }();
  return ok;
}

void FillRounded(HDC dc, const RECT& r, COLORREF color, int radius) {
  PhiFillRoundRectAA(dc, r, radius, color);
}

class Overlay {
 public:
  static Overlay* instance;

  Overlay(PhiWindowController* controller, const std::string& task_id)
      : controller_(controller),
        shell_(controller->shell_hwnd()),
        task_id_(task_id) {
    dpi_ = ::GetDpiForWindow(shell_);
    Register();
    fog_ = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_NOPARENTNOTIFY, kFogClass,
                             L"", WS_CHILD, 0, 0, 0, 0,
                             controller_->shell_hwnd(), nullptr,
                             ::GetModuleHandleW(nullptr), this);
    // WS_EX_TRANSPARENT: the sprite is pure output — hit tests fall through
    // to the fog barrier beneath it.
    sprite_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOPARENTNOTIFY,
        kSpriteClass, L"", WS_CHILD, 0, 0, 0, 0, controller_->shell_hwnd(),
        nullptr, ::GetModuleHandleW(nullptr), this);
    pill_ = ::CreateWindowExW(WS_EX_NOPARENTNOTIFY, kPillClass, L"", WS_CHILD,
                              0, 0, 0, 0, controller_->shell_hwnd(), nullptr,
                              ::GetModuleHandleW(nullptr), this);
    EnsureFonts();
  }

  ~Overlay() {
    for (HFONT font : {label_font_, caption_font_}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
    for (HWND window : {pill_, sprite_, fog_}) {
      if (window && ::IsWindow(window)) {
        ::DestroyWindow(window);
      }
    }
  }

  const std::string& task_id() const { return task_id_; }
  PhiWindowController* controller() const { return controller_; }

  void Update(const std::wstring& agent_label,
              const std::wstring& caption,
              bool agent_owns) {
    agent_label_ = agent_label;
    caption_ = caption;
    const bool owner_changed = agent_owns_ != agent_owns;
    agent_owns_ = agent_owns;
    Reposition();
    if (owner_changed) {
      // The drawn cursor follows ownership (see ComposeSprite): it leaves
      // with a takeover and returns where it was on hand-back.
      ComposeSprite();
    }
    // The fog only exists while the AGENT owns the Space; once the user takes
    // control the page must be usable, so the barrier comes down entirely.
    ::ShowWindow(fog_, agent_owns_ ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (agent_owns_) {
      ::SetLayeredWindowAttributes(fog_, 0, kFogAlpha, LWA_ALPHA);
    }
    ::ShowWindow(pill_, SW_SHOWNOACTIVATE);
    ::InvalidateRect(pill_, nullptr, TRUE);
  }

  void Reposition() {
    // Reached from EVERY shell's LayoutContents. The HWND is tested rather
    // than the controller: an HWND value is safe to ask about after its
    // window died, a freed controller is not.
    if (!controller_ || !shell_ || !::IsWindow(shell_)) {
      return;
    }
    const RECT content = controller_->ContentRect();
    const int width = content.right - content.left;
    const int height = content.bottom - content.top;
    ::SetWindowPos(fog_, HWND_TOP, content.left, content.top, width, height,
                   SWP_NOACTIVATE);
    // The sprite tracks the content rect exactly, stacked over the fog (the
    // later HWND_TOP wins the z race between the two).
    ::SetWindowPos(sprite_, HWND_TOP, content.left, content.top, width,
                   height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ComposeSprite();
    const int pill_w = std::min(Scaled(kPillMaxWidth), width - Scaled(32));
    const int pill_h = Scaled(kPillHeight);
    ::SetWindowPos(pill_, HWND_TOP,
                   content.left + (width - pill_w) / 2,
                   content.bottom - pill_h - Scaled(kPillBottomGap), pill_w,
                   pill_h, SWP_NOACTIVATE);
    // Shape the WINDOW to the pill: painting a rounded rect inside a square
    // window would leave its corners showing the unpainted background.
    if (HRGN region = ::CreateRoundRectRgn(0, 0, pill_w + 1, pill_h + 1,
                                           pill_h, pill_h)) {
      ::SetWindowRgn(pill_, region, TRUE);  // The window owns it now.
    }
    LayoutButtons(pill_w, pill_h);
  }

  bool agent_owns() const { return agent_owns_; }

  // ---- The agent cursor + input-mirror effects (the sprite layer) ----

  void SetAgentCursor(double css_x, double css_y) {
    const Gdiplus::PointF target(ScaledF(css_x), ScaledF(css_y));
    const ULONGLONG now = ::GetTickCount64();
    if (!cursor_shown_) {
      // First appearance teleports: no sliding in from a stale position.
      cursor_shown_ = true;
      cursor_from_ = cursor_to_ = target;
      glide_start_ = 0;
    } else {
      const Gdiplus::PointF current = CursorNow(now);
      const double distance =
          std::hypot(target.X - current.X, target.Y - current.Y);
      cursor_from_ = current;
      cursor_to_ = target;
      if (distance >= ScaledF(2)) {
        // The Mac glide: duration scales with DIP distance, decel-heavy.
        glide_start_ = now;
        glide_ms_ = static_cast<DWORD>(std::clamp(
            distance * 96.0 / dpi_, 180.0, 450.0));
      } else {
        glide_start_ = 0;
      }
    }
    StartTicking();
    ComposeSprite();
  }

  void PlayEffect(const std::string& kind,
                  bool has_point,
                  double css_x,
                  double css_y,
                  double css_w,
                  double css_h,
                  double dy) {
    Effect effect;
    if (kind == "click") {
      effect.kind = Effect::kClick;
    } else if (kind == "type") {
      effect.kind = Effect::kType;
      // Rapid fills must not stack outlines: a new pulse replaces the
      // still-visible one (the Mac's typingPulseLayer swap).
      effects_.erase(std::remove_if(effects_.begin(), effects_.end(),
                                    [](const Effect& e) {
                                      return e.kind == Effect::kType;
                                    }),
                     effects_.end());
    } else if (kind == "scroll") {
      effect.kind = Effect::kScroll;
    } else {
      return;  // Unknown kinds stay cosmetic no-ops.
    }
    effect.point = has_point
                       ? Gdiplus::PointF(ScaledF(css_x), ScaledF(css_y))
                       : CursorNow(::GetTickCount64());
    effect.width = ScaledF(css_w);
    effect.height = ScaledF(css_h);
    effect.down = dy >= 0;
    effect.start = ::GetTickCount64();
    effects_.push_back(effect);
    StartTicking();
    ComposeSprite();
  }

  void OnSpriteTick() {
    const ULONGLONG now = ::GetTickCount64();
    effects_.erase(std::remove_if(effects_.begin(), effects_.end(),
                                  [now](const Effect& e) {
                                    return now - e.start > e.LifetimeMs();
                                  }),
                   effects_.end());
    ComposeSprite();
    const bool gliding =
        glide_start_ && now - glide_start_ < glide_ms_;
    if (!gliding && effects_.empty()) {
      ::KillTimer(sprite_, kSpriteTimer);
      ticking_ = false;
    }
  }

 private:
  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }
  float ScaledF(double design) const {
    return static_cast<float>(design * dpi_ / 96.0);
  }

  struct Effect {
    enum Kind { kClick, kType, kScroll };
    Kind kind = kClick;
    Gdiplus::PointF point;
    float width = 0;
    float height = 0;
    bool down = true;  // Scroll direction: page scrolls down.
    ULONGLONG start = 0;
    ULONGLONG LifetimeMs() const {
      switch (kind) {
        case kClick:
          return 700;   // dot 400 + second ring 140+550.
        case kType:
          return 1950;  // The Mac pulse's full run.
        case kScroll:
          return 850;   // Third chevron: 2*120 + 600.
      }
      return 700;
    }
  };

  // Where the cursor is RIGHT NOW: mid-glide interpolation on the Mac's
  // decel-heavy bezier (0.3, 0, 0.15, 1), approximated by its y-progress.
  Gdiplus::PointF CursorNow(ULONGLONG now) const {
    if (!glide_start_ || now - glide_start_ >= glide_ms_) {
      return cursor_to_;
    }
    const double t =
        static_cast<double>(now - glide_start_) / glide_ms_;
    // Cubic-bezier(0.3, 0, 0.15, 1) sampled by t — close enough for a
    // 450ms glide: ease-in start, long soft landing.
    const double u = 1.0 - t;
    const double eased =
        3 * u * u * t * 0.0 + 3 * u * t * t * 1.0 + t * t * t;
    return Gdiplus::PointF(
        static_cast<float>(cursor_from_.X +
                           (cursor_to_.X - cursor_from_.X) * eased),
        static_cast<float>(cursor_from_.Y +
                           (cursor_to_.Y - cursor_from_.Y) * eased));
  }

  void StartTicking() {
    if (!ticking_ && sprite_) {
      ticking_ = true;
      ::SetTimer(sprite_, kSpriteTimer, kSpriteTickMs, nullptr);
    }
  }

  // Renders the sprite layer — cursor + live effects — into a premultiplied
  // ARGB surface and hands it to UpdateLayeredWindow.
  void ComposeSprite() {
    if (!sprite_ || !::IsWindow(sprite_) || !EnsureGdiPlus()) {
      return;
    }
    RECT rc = {};
    ::GetClientRect(sprite_, &rc);
    const int width = rc.right;
    const int height = rc.bottom;
    if (width <= 0 || height <= 0) {
      return;
    }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // Top-down.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP dib =
        ::CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (!dib) {
      ::DeleteDC(mem);
      return;
    }
    HGDIOBJ old = ::SelectObject(mem, dib);
    {
      Gdiplus::Bitmap surface(width, height, width * 4,
                              PixelFormat32bppPARGB,
                              static_cast<BYTE*>(bits));
      Gdiplus::Graphics graphics(&surface);
      graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
      graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
      const ULONGLONG now = ::GetTickCount64();
      for (const Effect& effect : effects_) {
        DrawEffect(&graphics, effect, now);
      }
      // Hidden while the USER holds control (the Mac's moveCursor guard):
      // the agent stops reporting on takeover, so its last position would
      // otherwise sit frozen beside the real cursor until the task ended.
      // The stored point survives the flip, so the cursor reappears where it
      // was when control is handed back.
      if (cursor_shown_ && agent_owns_) {
        DrawCursor(&graphics, CursorNow(now));
      }
    }
    POINT src = {0, 0};
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    ::UpdateLayeredWindow(sprite_, nullptr, nullptr, &size, mem, &src, 0,
                          &blend, ULW_ALPHA);
    ::SelectObject(mem, old);
    ::DeleteObject(dib);
    ::DeleteDC(mem);
  }

  Gdiplus::Color AccentColor(BYTE alpha) const {
    const COLORREF accent = controller_->CurrentPalette().accent;
    return Gdiplus::Color(alpha, GetRValue(accent), GetGValue(accent),
                          GetBValue(accent));
  }

  void DrawEffect(Gdiplus::Graphics* graphics,
                  const Effect& effect,
                  ULONGLONG now) const {
    const double ms = static_cast<double>(now - effect.start);
    switch (effect.kind) {
      case Effect::kClick: {
        // Center dot fading over 350ms.
        if (ms < 350) {
          const BYTE alpha = static_cast<BYTE>(255 * (1.0 - ms / 350));
          Gdiplus::SolidBrush dot(AccentColor(alpha));
          const float radius = ScaledF(4);
          graphics->FillEllipse(&dot, effect.point.X - radius,
                                effect.point.Y - radius, radius * 2,
                                radius * 2);
        }
        // Two staggered rings: scale 0.25→1 (ease-out), opacity 0→.85→0.
        const std::array<std::pair<double, float>, 2> rings = {
            {{0.0, ScaledF(18)}, {140.0, ScaledF(26)}}};
        for (const auto& [delay, full_radius] : rings) {
          const double t = (ms - delay) / 500.0;
          if (t < 0 || t > 1) {
            continue;
          }
          const double eased = 1.0 - (1.0 - t) * (1.0 - t);
          const float radius =
              full_radius * static_cast<float>(0.25 + 0.75 * eased);
          const double opacity =
              t < 0.25 ? 0.85 * (t / 0.25) : 0.85 * (1.0 - (t - 0.25) / 0.75);
          Gdiplus::Pen ring(AccentColor(static_cast<BYTE>(255 * opacity)),
                            ScaledF(2.5));
          graphics->DrawEllipse(&ring, effect.point.X - radius,
                                effect.point.Y - radius, radius * 2,
                                radius * 2);
        }
        break;
      }
      case Effect::kType: {
        // Pulsing outline around the field, dots blinking under its
        // trailing edge; the whole group fades out after 1.6s.
        const float min_w = ScaledF(40);
        const float min_h = ScaledF(22);
        const float box_w =
            std::max(effect.width > 0 ? effect.width : ScaledF(160), min_w) +
            ScaledF(8);
        const float box_h =
            std::max(effect.height > 0 ? effect.height : ScaledF(32),
                     min_h) +
            ScaledF(8);
        double group = 1.0;
        if (ms > 1600) {
          group = std::max(0.0, 1.0 - (ms - 1600) / 250.0);
        }
        // Opacity pulse 1 → 0.45 → 1 → 0.45 → 1 over the first 1.6s.
        const double phase = std::fmod(ms / 400.0, 2.0);
        const double pulse =
            1.0 - 0.55 * (phase < 1.0 ? phase : 2.0 - phase);
        const BYTE stroke_alpha =
            static_cast<BYTE>(255 * pulse * group);
        const float left = effect.point.X - box_w / 2;
        const float top = effect.point.Y - box_h / 2;
        const float corner = ScaledF(7) * 2;
        Gdiplus::GraphicsPath box;
        box.AddArc(left, top, corner, corner, 180, 90);
        box.AddArc(left + box_w - corner, top, corner, corner, 270, 90);
        box.AddArc(left + box_w - corner, top + box_h - corner, corner,
                   corner, 0, 90);
        box.AddArc(left, top + box_h - corner, corner, corner, 90, 90);
        box.CloseFigure();
        Gdiplus::SolidBrush fill(
            AccentColor(static_cast<BYTE>(255 * 0.07 * group)));
        graphics->FillPath(&fill, &box);
        Gdiplus::Pen outline(AccentColor(stroke_alpha), ScaledF(2));
        graphics->DrawPath(&outline, &box);
        // Three blinking dots below the trailing edge.
        for (int i = 0; i < 3; ++i) {
          const double blink_t =
              std::fmod((ms - i * 150.0) / 600.0, 1.0);
          double blink = 0.2;
          if (ms >= i * 150.0 && ms < i * 150.0 + 1800.0) {
            blink = blink_t < 0.5 ? 0.2 + 1.6 * blink_t
                                  : 1.0 - 1.6 * (blink_t - 0.5);
          }
          Gdiplus::SolidBrush dot(AccentColor(
              static_cast<BYTE>(255 * std::clamp(blink, 0.0, 1.0) *
                                group)));
          const float dot_r = ScaledF(2.5);
          const float dot_x =
              left + box_w - ScaledF(30) + i * ScaledF(11);
          const float dot_y = top + box_h + ScaledF(9);
          graphics->FillEllipse(&dot, dot_x - dot_r, dot_y - dot_r,
                                dot_r * 2, dot_r * 2);
        }
        break;
      }
      case Effect::kScroll: {
        // Three chevrons drifting in the scroll direction.
        const float travel = effect.down ? ScaledF(34) : -ScaledF(34);
        const float apex = effect.down ? ScaledF(5) : -ScaledF(5);
        for (int i = 0; i < 3; ++i) {
          const double t = (ms - i * 120.0) / 550.0;
          if (t < 0 || t > 1) {
            continue;
          }
          const double opacity =
              t < 0.35 ? 0.9 * (t / 0.35) : 0.9 * (1.0 - (t - 0.35) / 0.65);
          const float y = effect.point.Y - travel * 0.5f +
                          travel * 1.5f * static_cast<float>(t);
          Gdiplus::Pen pen(AccentColor(static_cast<BYTE>(255 * opacity)),
                           ScaledF(3));
          pen.SetStartCap(Gdiplus::LineCapRound);
          pen.SetEndCap(Gdiplus::LineCapRound);
          pen.SetLineJoin(Gdiplus::LineJoinRound);
          Gdiplus::PointF points[3] = {
              {effect.point.X - ScaledF(10), y - apex},
              {effect.point.X, y + apex},
              {effect.point.X + ScaledF(10), y - apex}};
          graphics->DrawLines(&pen, points, 3);
        }
        break;
      }
    }
  }

  // The Mac cursor path (a 14.0089 × 15.9927 viewBox, authored y-down — no
  // flip needed in GDI's y-down space), scaled to a 27pt-tall arrow.
  void DrawCursor(Gdiplus::Graphics* graphics,
                  const Gdiplus::PointF& at) const {
    const float scale = ScaledF(27) / 15.9927f;
    // The tip anchor: the Mac pins layer anchorPoint (0.1, 0.89) y-up —
    // (0.1, 0.11) of the glyph box in y-down terms.
    const float ox = at.X - 0.1f * 14.0089f * scale;
    const float oy = at.Y - 0.11f * 15.9927f * scale;
    auto point = [&](double x, double y) {
      return Gdiplus::PointF(ox + static_cast<float>(x) * scale,
                             oy + static_cast<float>(y) * scale);
    };
    Gdiplus::GraphicsPath path;
    Gdiplus::PointF segment_a[4] = {point(0.5039, 1.8181),
                                    point(0.5893, 0.6644),
                                    point(1.9677, 0.1012),
                                    point(2.8730, 0.8133)};
    path.AddBeziers(segment_a, 4);
    path.AddLine(point(2.8730, 0.8133), point(12.9990, 8.7879));
    Gdiplus::PointF segment_b[4] = {point(12.9990, 8.7879),
                                    point(14.0076, 9.5821),
                                    point(13.3921, 11.1219),
                                    point(12.1641, 11.1219)};
    path.AddBeziers(segment_b, 4);
    path.AddLine(point(12.1641, 11.1219), point(6.5400, 11.1219));
    Gdiplus::PointF segment_c[7] = {
        point(6.5400, 11.1219),  point(6.4112, 11.1219),
        point(6.2842, 11.1504),  point(6.1689, 11.2039),
        point(6.0539, 11.2573),  point(5.9535, 11.3341),
        point(5.8750, 11.4285)};
    path.AddBeziers(segment_c, 7);
    path.AddLine(point(5.8750, 11.4285), point(2.8984, 15.0076));
    Gdiplus::PointF segment_d[4] = {point(2.8984, 15.0076),
                                    point(2.1136, 15.9510),
                                    point(0.5001, 15.4459),
                                    point(0.5000, 14.1668)};
    path.AddBeziers(segment_d, 4);
    path.AddLine(point(0.5000, 14.1668), point(0.5000, 1.9314));
    path.CloseFigure();

    // Light: pale-tint → theme gradient with a dark-hue border; dark: flat
    // theme fill outlined in white (applyTheme's two appearances).
    const PhiPalette& palette = controller_->CurrentPalette();
    const COLORREF accent = palette.accent;
    const Gdiplus::Color accent_color(255, GetRValue(accent),
                                      GetGValue(accent), GetBValue(accent));
    if (palette.dark) {
      Gdiplus::SolidBrush fill(accent_color);
      graphics->FillPath(&fill, &path);
      Gdiplus::Pen border(Gdiplus::Color(255, 255, 255, 255),
                          1.0f * scale);
      border.SetLineJoin(Gdiplus::LineJoinRound);
      graphics->DrawPath(&border, &path);
    } else {
      const Gdiplus::Color pale(
          255, static_cast<BYTE>(GetRValue(accent) +
                                 (255 - GetRValue(accent)) * 72 / 100),
          static_cast<BYTE>(GetGValue(accent) +
                            (255 - GetGValue(accent)) * 72 / 100),
          static_cast<BYTE>(GetBValue(accent) +
                            (255 - GetBValue(accent)) * 72 / 100));
      Gdiplus::RectF bounds;
      path.GetBounds(&bounds);
      Gdiplus::LinearGradientBrush fill(bounds, pale, accent_color,
                                        Gdiplus::LinearGradientModeVertical);
      graphics->FillPath(&fill, &path);
      const Gdiplus::Color border_color(
          255, static_cast<BYTE>(GetRValue(accent) * 35 / 100),
          static_cast<BYTE>(GetGValue(accent) * 35 / 100),
          static_cast<BYTE>(GetBValue(accent) * 35 / 100));
      Gdiplus::Pen border(border_color, 1.0f * scale);
      border.SetLineJoin(Gdiplus::LineJoinRound);
      graphics->DrawPath(&border, &path);
    }
  }

  void Register() {
    static bool registered = [] {
      WNDCLASSEXW fog = {};
      fog.cbSize = sizeof(fog);
      fog.lpfnWndProc = &FogProc;
      fog.hInstance = ::GetModuleHandleW(nullptr);
      fog.hCursor = ::LoadCursorW(nullptr, IDC_NO);
      fog.lpszClassName = kFogClass;
      ::RegisterClassExW(&fog);
      WNDCLASSEXW pill = {};
      pill.cbSize = sizeof(pill);
      pill.lpfnWndProc = &PillProc;
      pill.hInstance = ::GetModuleHandleW(nullptr);
      pill.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
      pill.lpszClassName = kPillClass;
      ::RegisterClassExW(&pill);
      WNDCLASSEXW sprite = {};
      sprite.cbSize = sizeof(sprite);
      sprite.lpfnWndProc = &SpriteProc;
      sprite.hInstance = ::GetModuleHandleW(nullptr);
      sprite.lpszClassName = kSpriteClass;
      ::RegisterClassExW(&sprite);
      return true;
    }();
    (void)registered;
  }

  void EnsureFonts() {
    auto make = [&](int size, int weight) {
      return ::CreateFontW(-Scaled(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    };
    label_font_ = make(13, FW_SEMIBOLD);
    caption_font_ = make(12, FW_NORMAL);
  }

  void LayoutButtons(int pill_w, int pill_h) {
    const int mid = pill_h / 2;
    const int bh = Scaled(kButtonHeight);
    int right = pill_w - Scaled(10);
    if (!agent_owns_) {
      // User holds control: Finish trails, Hand back leads it.
      const int finish_w = Scaled(72);
      secondary_ = {right - finish_w, mid - bh / 2, right, mid + bh / 2};
      right = secondary_.left - Scaled(8);
    } else {
      secondary_ = RECT{};
    }
    const int primary_w = Scaled(agent_owns_ ? 104 : 92);
    primary_ = {right - primary_w, mid - bh / 2, right, mid + bh / 2};
  }

  void Paint(HDC dc) {
    RECT client = {};
    ::GetClientRect(pill_, &client);
    const PhiPalette& palette = controller_->CurrentPalette();
    const COLORREF ground = palette.dark ? RGB(0x2A, 0x2A, 0x2E)
                                         : RGB(0xFF, 0xFF, 0xFF);
    const COLORREF ink = palette.dark ? RGB(0xF2, 0xF2, 0xF4)
                                      : RGB(0x1A, 0x1A, 0x1C);
    const COLORREF dim = palette.dark ? RGB(0xA0, 0xA0, 0xA8)
                                      : RGB(0x6A, 0x6A, 0x72);
    FillRounded(dc, client, ground, (client.bottom - client.top) / 2);
    ::SetBkMode(dc, TRANSPARENT);

    // Leading: who is driving, then the status caption.
    RECT text = {Scaled(16), client.top, primary_.left - Scaled(12),
                 client.bottom};
    ::SelectObject(dc, label_font_);
    ::SetTextColor(dc, ink);
    RECT measured = text;
    ::DrawTextW(dc, agent_label_.c_str(), -1, &measured,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_CALCRECT);
    ::DrawTextW(dc, agent_label_.c_str(), -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT caption = {measured.right + Scaled(12), client.top,
                    primary_.left - Scaled(12), client.bottom};
    if (caption.right > caption.left) {
      ::SelectObject(dc, caption_font_);
      ::SetTextColor(dc, dim);
      ::DrawTextW(dc, caption_.c_str(), -1, &caption,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    }

    // The primary action carries the accent; the secondary stays quiet.
    FillRounded(dc, primary_, palette.accent, Scaled(6));
    ::SelectObject(dc, label_font_);
    ::SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    const wchar_t* primary_text = agent_owns_ ? L"Take control" : L"Hand back";
    ::DrawTextW(dc, primary_text, -1, &primary_,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (secondary_.right > secondary_.left) {
      FillRounded(dc, secondary_, palette.hover, Scaled(6));
      ::SetTextColor(dc, ink);
      ::DrawTextW(dc, L"Finish", -1, &secondary_,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }

  void OnPillClick(int x, int y) {
    const POINT pt = {x, y};
    PhiSlotHost* host = controller_ ? controller_->host() : nullptr;
    if (!host) {
      return;
    }
    // Copy what the handlers need: they can destroy this overlay.
    const std::string task = task_id_;
    if (::PtInRect(&primary_, pt)) {
      if (agent_owns_) {
        host->RequestAgentTakeControl(task);
      } else {
        host->RequestAgentHandBack(task);
      }
      return;
    }
    if (secondary_.right > secondary_.left && ::PtInRect(&secondary_, pt)) {
      host->RequestAgentFinish(task);
    }
  }

  static LRESULT CALLBACK FogProc(HWND hwnd, UINT message, WPARAM w,
                                  LPARAM l) {
    switch (message) {
      case WM_ERASEBKGND: {
        RECT client = {};
        ::GetClientRect(hwnd, &client);
        // A flat wash; WS_EX_LAYERED turns it into the fog.
        HBRUSH brush = ::CreateSolidBrush(RGB(0x10, 0x12, 0x18));
        ::FillRect(reinterpret_cast<HDC>(w), &client, brush);
        ::DeleteObject(brush);
        return 1;
      }
      // The barrier itself: content input dies here. MOUSEACTIVATE keeps the
      // click from even activating the browser child beneath.
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATEANDEAT;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
      case WM_MOUSEWHEEL:
      case WM_MOUSEMOVE:
        return 0;
      case WM_SETCURSOR:
        ::SetCursor(::LoadCursorW(nullptr, IDC_NO));
        return TRUE;
    }
    return ::DefWindowProcW(hwnd, message, w, l);
  }

  static LRESULT CALLBACK SpriteProc(HWND hwnd, UINT message, WPARAM w,
                                     LPARAM l) {
    if (message == WM_TIMER && w == kSpriteTimer) {
      if (Overlay::instance) {
        Overlay::instance->OnSpriteTick();
      } else {
        ::KillTimer(hwnd, kSpriteTimer);
      }
      return 0;
    }
    return ::DefWindowProcW(hwnd, message, w, l);
  }

  static LRESULT CALLBACK PillProc(HWND hwnd, UINT message, WPARAM w,
                                   LPARAM l) {
    switch (message) {
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // Clickable without stealing page focus.
      case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = ::BeginPaint(hwnd, &paint);
        RECT client = {};
        ::GetClientRect(hwnd, &client);
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP bitmap =
            ::CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = ::SelectObject(mem, bitmap);
        if (Overlay::instance) {
          Overlay::instance->Paint(mem);
        }
        ::BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        ::SelectObject(mem, old);
        ::DeleteObject(bitmap);
        ::DeleteDC(mem);
        ::EndPaint(hwnd, &paint);
        return 0;
      }
      case WM_LBUTTONUP:
        if (Overlay::instance) {
          Overlay::instance->OnPillClick(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        }
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, w, l);
  }

  RAW_PTR_EXCLUSION PhiWindowController* controller_;
  const HWND shell_;  // controller_'s shell at construction.
  std::string task_id_;
  std::wstring agent_label_;
  std::wstring caption_;
  bool agent_owns_ = true;
  HWND fog_ = nullptr;
  HWND sprite_ = nullptr;
  HWND pill_ = nullptr;
  UINT dpi_ = 96;
  // The agent cursor: target/source in sprite-local device px; a zero
  // glide_start_ means "sitting at cursor_to_".
  bool cursor_shown_ = false;
  Gdiplus::PointF cursor_from_;
  Gdiplus::PointF cursor_to_;
  ULONGLONG glide_start_ = 0;
  DWORD glide_ms_ = 0;
  std::vector<Effect> effects_;
  bool ticking_ = false;
  RECT primary_{};
  RECT secondary_{};
  HFONT label_font_ = nullptr;
  HFONT caption_font_ = nullptr;
};

Overlay* Overlay::instance = nullptr;

}  // namespace

// static
void PhiAgentOverlay::Show(PhiWindowController* controller,
                           const std::string& task_id,
                           const std::wstring& agent_label,
                           const std::wstring& caption,
                           bool agent_owns) {
  if (!controller || !controller->shell_hwnd()) {
    return;
  }
  if (Overlay::instance && (Overlay::instance->controller() != controller ||
                            Overlay::instance->task_id() != task_id)) {
    Hide();  // Watching a different Space now.
  }
  if (!Overlay::instance) {
    Overlay::instance = new Overlay(controller, task_id);
  }
  Overlay::instance->Update(agent_label, caption, agent_owns);
}

// static
void PhiAgentOverlay::Hide() {
  Overlay* overlay = Overlay::instance;
  Overlay::instance = nullptr;
  delete overlay;
}

// static
void PhiAgentOverlay::HideIfBoundTo(const PhiWindowController* controller) {
  if (Overlay::instance && Overlay::instance->controller() == controller) {
    Hide();
  }
}

// static
bool PhiAgentOverlay::IsShown() {
  return Overlay::instance != nullptr;
}

// static
void PhiAgentOverlay::Reposition() {
  if (Overlay::instance) {
    Overlay::instance->Reposition();
  }
}

// static
void PhiAgentOverlay::SetAgentCursor(const std::string& task_id,
                                     double x,
                                     double y) {
  if (Overlay::instance && Overlay::instance->task_id() == task_id) {
    Overlay::instance->SetAgentCursor(x, y);
  }
}

// static
void PhiAgentOverlay::PlayAgentEffect(const std::string& task_id,
                                      const std::string& kind,
                                      bool has_point,
                                      double x,
                                      double y,
                                      double w,
                                      double h,
                                      double dy) {
  if (Overlay::instance && Overlay::instance->task_id() == task_id) {
    Overlay::instance->PlayEffect(kind, has_point, x, y, w, h, dy);
  }
}

}  // namespace phi_client
