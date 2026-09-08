// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_space_theme_panel.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_popover.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

// SpaceThemeEditorView's metrics, in DIP. The panel is the Mac's 328x99
// card with an inner 312x83 hairline card inside an 8pt margin.
constexpr int kPanelWidth = 328;
constexpr int kPanelHeight = 99;
constexpr int kCardInset = 8;
constexpr int kCardRadius = 7;
constexpr int kCardPadding = 12;
constexpr int kColorRowHeight = 40;
constexpr int kSaturationRowHeight = 42;
// The swatch group and the slider are both right-aligned inside the card.
constexpr int kSwatchGroupWidth = 200;
constexpr int kSwatchDot = 16;
constexpr int kSwatchRing = 20;
constexpr int kSliderWidth = 199;
constexpr int kSliderKnob = 16;
constexpr int kSliderTrackHeight = 10;
constexpr int kLabelSize = 13;
// The slider's hit rect is the track band grown by this much, top and
// bottom: anywhere on the saturation row grabs the knob (the Mac's
// slider behaviour — the press jumps it, the drag carries it).
constexpr int kSliderGrabPad = 8;
// hits_ key for the slider; swatch hits carry their catalog index.
constexpr int kSliderKey = -1;

void RgbToHsv(COLORREF c, double* h, double* s, double* v) {
  const double r = GetRValue(c) / 255.0, g = GetGValue(c) / 255.0,
               b = GetBValue(c) / 255.0;
  const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
  const double d = mx - mn;
  *v = mx;
  *s = mx <= 0.0 ? 0.0 : d / mx;
  if (d <= 0.0) {
    *h = 0.0;
  } else if (mx == r) {
    *h = 60.0 * std::fmod((g - b) / d, 6.0);
  } else if (mx == g) {
    *h = 60.0 * ((b - r) / d + 2.0);
  } else {
    *h = 60.0 * ((r - g) / d + 4.0);
  }
  if (*h < 0) {
    *h += 360.0;
  }
}

COLORREF HsvToRgb(double h, double s, double v) {
  const double c = v * s;
  const double x = c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0));
  const double m = v - c;
  double r = 0, g = 0, b = 0;
  if (h < 60) {
    r = c; g = x;
  } else if (h < 120) {
    r = x; g = c;
  } else if (h < 180) {
    g = c; b = x;
  } else if (h < 240) {
    g = x; b = c;
  } else if (h < 300) {
    r = x; b = c;
  } else {
    r = c; b = x;
  }
  return RGB(static_cast<int>((r + m) * 255 + 0.5),
             static_cast<int>((g + m) * 255 + 0.5),
             static_cast<int>((b + m) * 255 + 0.5));
}

// The panel keeps its swatch/slider model, its Paint() and its host edits.
// The window, the placement, hover, the capture and dismissal are
// PhiPopover's; the modal GetMessage loop is gone (lane 3 §4.4, the second
// of the two this phase deletes).
class Panel : public PhiPopover {
 public:
  Panel(PhiWindowController* controller,
        std::string space_id,
        std::function<void()> on_closed)
      : controller_(controller),
        space_id_(std::move(space_id)),
        on_closed_(std::move(on_closed)) {}

  ~Panel() override = default;

  bool Open(const RECT& anchor_screen) {
    HWND owner = controller_ ? controller_->shell_hwnd() : nullptr;
    if (!owner) {
      return false;
    }
    ClassSpec spec;
    spec.name = L"PhiSpaceThemePanel";

    Options options;

    Placement placement;
    placement.anchor = anchor_screen;
    placement.side = PhiPopoverSide::kBelow;
    // A point anchor (a menu click) hangs the panel off its left edge; an
    // anchor WIDER than the panel — a window's caption band — centres it
    // there instead, which is the Mac's nearTrigger / windowTop pair.
    placement.align = PhiPopoverAlign::kCenterIfWider;
    placement.gap_dip = 4;

    SIZE probe = {1, 1};
    if (!CreatePopover(spec, owner, probe, placement, options)) {
      return false;
    }
    BuildLayout();
    SIZE size = {Scaled(kPanelWidth), Scaled(kPanelHeight)};
    Reposition(placement, size);
    Show();
    return true;
  }

 protected:
  PhiPalette QueryPalette() const override {
    return controller_ ? controller_->CurrentPalette()
                       : PhiResolvePalette(PhiThemeById("pure"),
                                           PhiAppearance::kSystem);
  }

  void OnDpiChanged(UINT) override {
    BuildLayout();
    ::SetWindowPos(hwnd(), nullptr, 0, 0, Scaled(kPanelWidth),
                   Scaled(kPanelHeight),
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override {
    // A drag owns the pointer: no hover churn while the knob is held.
    if (message == WM_MOUSEMOVE && dragging_) {
      *handled = true;
      ApplySaturationAt(GET_X_LPARAM(lparam));
      Invalidate();
      return 0;
    }
    if (message == WM_LBUTTONUP && dragging_) {
      *handled = true;
      EndDrag();
      return 0;
    }
    return PhiPopover::OnMessage(message, wparam, lparam, handled);
  }

  void OnMouseDown(int hit, POINT client_pt) override {
    if (hit < 0) {
      return;
    }
    const PhiHit& target = hits_[static_cast<size_t>(hit)];
    if (target.key == kSliderKey) {
      capture_ = BeginCapture();
      dragging_ = true;
      ApplySaturationAt(client_pt.x);
      Invalidate();
      return;
    }
    const std::vector<PhiTheme>& themes = PhiBuiltInThemes();
    const size_t index = static_cast<size_t>(target.key);
    if (index < themes.size()) {
      if (PhiSlotHost* host = controller_ ? controller_->host() : nullptr) {
        host->SetSpaceTheme(space_id_, themes[index].id);
      }
      Invalidate();
    }
  }

  // The knob is let go — by the user, or by the system taking the capture
  // away (Alt+Tab, a system menu, another window's SetCapture). Without
  // this the knob stayed "dragging" forever: every later mouse move kept
  // driving the slider and the dismissal stayed suppressed (lane 3 §4.2).
  void OnCaptureLost() override { EndDrag(); }

  // Focus can leave mid-drag (the shell repaint behind steals activation);
  // the dismissal is deferred to the end of the drag instead of dropped.
  void OnDismiss() override {
    if (dragging_) {
      dismiss_pending_ = true;
      return;
    }
    PhiPopover::OnDismiss();
  }

  void OnNcDestroy() override {
    if (on_closed_) {
      std::function<void()> callback = std::move(on_closed_);
      on_closed_ = std::function<void()>();
      callback();
    }
  }

  void Paint(HDC dc, const RECT&) override {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const PhiPalette& colors = palette();
    HBRUSH ground = ::CreateSolidBrush(colors.ground_top);
    ::FillRect(dc, &rc, ground);
    ::DeleteObject(ground);

    // The inner hairline card.
    RECT card = {Scaled(kCardInset), Scaled(kCardInset),
                 Scaled(kPanelWidth) - Scaled(kCardInset),
                 Scaled(kPanelHeight) - Scaled(kCardInset)};
    PhiStrokeRoundRectAA(dc, card, Scaled(kCardRadius), colors.border, 1);

    const int card_left = card.left + Scaled(kCardPadding);
    const int card_top = card.top;
    RECT color_label = {card_left, card_top, card.right,
                        card_top + Scaled(kColorRowHeight)};
    DrawLabel(dc, color_label, PhiStr("settings.spaces.theme.colorLabel"),
              colors.label);

    // The divider between the two rows, the Mac's separatorColor hairline.
    const int divider_y = card_top + Scaled(kColorRowHeight);
    RECT divider = {card.left + 1, divider_y, card.right - 1, divider_y + 1};
    HBRUSH line = ::CreateSolidBrush(colors.separator);
    ::FillRect(dc, &divider, line);
    ::DeleteObject(line);

    RECT sat_label = {card_left, divider_y + 1, card.right,
                      divider_y + 1 + Scaled(kSaturationRowHeight)};
    DrawLabel(dc, sat_label, PhiStr("settings.spaces.theme.saturationLabel"),
              colors.label);

    // The swatches: Pure reads as a white dot with a contrast border,
    // every other theme as its own accent; the selected one wears the ring.
    const std::string selected = CurrentThemeId();
    const std::vector<PhiTheme>& themes = PhiBuiltInThemes();
    const bool dark = colors.dark;
    for (size_t i = 0; i < themes.size() && i < hits_.size(); ++i) {
      if (hits_[i].key == kSliderKey) {
        break;
      }
      const RECT ring_rect = PhiToRect(hits_[i].bounds);
      const PhiThemeColors& swatch = dark ? themes[i].dark : themes[i].light;
      const COLORREF accent = swatch.accent;
      const bool is_selected = selected == themes[i].id;
      if (is_selected || hovered() == static_cast<int>(i)) {
        PhiStrokeCapsuleAA(dc, ring_rect, accent, is_selected ? 2 : 1);
      }
      const int dot = Scaled(kSwatchDot);
      RECT dot_rect = {
          ring_rect.left + ((ring_rect.right - ring_rect.left) - dot) / 2,
          ring_rect.top + ((ring_rect.bottom - ring_rect.top) - dot) / 2, 0,
          0};
      dot_rect.right = dot_rect.left + dot;
      dot_rect.bottom = dot_rect.top + dot;
      // Pure has no color of its own — the Mac fills its dot white and
      // rings it so it stays visible on a white card.
      const bool is_pure = themes[i].id == std::string("pure");
      PhiFillCapsuleAA(dc, dot_rect, is_pure ? RGB(0xFF, 0xFF, 0xFF) : accent);
      if (is_pure) {
        PhiStrokeCapsuleAA(dc, dot_rect, colors.border, 1);
      }
    }

    PaintSlider(dc, colors, selected, dark);
  }

 private:
  // The panel edits through the host, so both values are read back fresh
  // on every paint — a click applies immediately and the window behind
  // recolors.
  std::string CurrentThemeId() const {
    PhiSlotHost* host = controller_ ? controller_->host() : nullptr;
    if (!host) {
      return std::string();
    }
    // The Mac's resolvedThemeId: a Space with no pin of its own shows —
    // and edits from — the app-wide theme, so the row always has a
    // selection.
    const std::string pinned = host->SpaceThemeId(space_id_);
    return pinned.empty() ? host->ThemeId() : pinned;
  }

  int CurrentSaturation() const {
    PhiSlotHost* host = controller_ ? controller_->host() : nullptr;
    if (!host) {
      return 0;
    }
    // -1 means the Space follows the app-wide slider, which is the value
    // its own slider then shows (the Spaces settings pane's rule).
    const int stored = host->SpaceSaturation(space_id_);
    return stored < 0 ? host->Saturation() : stored;
  }

  void DrawLabel(HDC dc, const RECT& bounds, const wchar_t* text,
                 COLORREF ink) {
    HGDIOBJ prev = ::SelectObject(dc, fonts().Get(kLabelSize, FW_NORMAL));
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, ink);
    RECT rect = bounds;
    ::DrawTextW(dc, text, -1, &rect,
                DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
    ::SelectObject(dc, prev);
  }

  // The Mac's 10pt track carrying a gradient of the SELECTED theme's
  // overlay — saturation 0.1 -> 0.9, or, for Pure, brightness from its
  // light/dark range (brightest at the left) — under a white knob.
  void PaintSlider(HDC dc, const PhiPalette& colors,
                   const std::string& selected, bool dark) {
    const PhiTheme& theme = PhiThemeById(selected);
    const PhiThemeColors& swatch = dark ? theme.dark : theme.light;
    const RECT band = SliderTrack();
    const int span = band.right - band.left;
    const int half = Scaled(kSliderTrackHeight) / 2;
    double hue = 0, sat = 0, val = 0;
    RgbToHsv(swatch.overlay, &hue, &sat, &val);
    // Pure ramps brightness where every other theme ramps saturation.
    const bool is_pure = selected == "pure";
    const int bands = 32;
    for (int i = 0; i < bands; ++i) {
      RECT strip = {band.left + span * i / bands, band.top,
                    band.left + span * (i + 1) / bands, band.bottom};
      const double t = (i + 0.5) / bands;
      const COLORREF tint =
          is_pure
              ? HsvToRgb(hue, sat,
                         (dark ? 0.40 : 0.94) +
                             ((dark ? 0.0 : 0.84) - (dark ? 0.40 : 0.94)) * t)
              : HsvToRgb(hue, 0.1 + 0.8 * t, val);
      HBRUSH brush = ::CreateSolidBrush(tint);
      ::FillRect(dc, &strip, brush);
      ::DeleteObject(brush);
    }
    // The capsule ends are KNOCKED OUT of the square gradient, not clipped
    // to a region: CreateRoundRectRgn + ExtSelectClipRgn cut the curve on
    // the pixel grid and stair-stepped both ends (lane 3 §3.6 violation 1;
    // the create-space panel's identical slider already does it this way).
    PhiKnockOutRoundRectAA(dc, band, half, colors.ground_top);
    PhiStrokeCapsuleAA(dc, band, colors.border, 1);

    const int knob = Scaled(kSliderKnob);
    const int mid = (band.top + band.bottom) / 2;
    const int at = band.left + (span - knob) * CurrentSaturation() / 100;
    RECT dot = {at, mid - knob / 2, at + knob, mid + knob / 2};
    PhiFillCapsuleAA(dc, dot, RGB(0xFF, 0xFF, 0xFF));
    PhiStrokeCapsuleAA(dc, dot, colors.border, 1);
  }

  // The knob's full travel band, in client coordinates. The slider's HIT
  // rect (in hits_) is this grown vertically by kSliderGrabPad.
  RECT SliderTravel() const {
    const int card_right = Scaled(kPanelWidth) - Scaled(kCardInset) -
                           Scaled(kCardPadding);
    const int slider_mid = Scaled(kCardInset) + Scaled(kColorRowHeight) + 1 +
                           Scaled(kSaturationRowHeight) / 2;
    const int knob = Scaled(kSliderKnob);
    RECT travel = {card_right - Scaled(kSliderWidth), slider_mid - knob / 2,
                   card_right, slider_mid + knob / 2};
    return travel;
  }

  RECT SliderTrack() const {
    const RECT travel = SliderTravel();
    const int mid = (travel.top + travel.bottom) / 2;
    const int half = Scaled(kSliderTrackHeight) / 2;
    RECT track = {travel.left, mid - half, travel.right, mid + half};
    return track;
  }

  // THE layout pass: eight swatch rings then the slider band, into the
  // base's hit list.
  void BuildLayout() {
    ClearHits();
    const std::vector<PhiTheme>& themes = PhiBuiltInThemes();
    const int card_right = Scaled(kPanelWidth) - Scaled(kCardInset) -
                           Scaled(kCardPadding);
    const int card_top = Scaled(kCardInset);
    const int group_left = card_right - Scaled(kSwatchGroupWidth);
    const int ring = Scaled(kSwatchRing);
    const int row_mid = card_top + Scaled(kColorRowHeight) / 2;
    const int count = static_cast<int>(themes.size());
    for (int i = 0; i < count; ++i) {
      // The rings spread evenly across the group's width (the Mac's
      // maxWidth: .infinity cells).
      const int cell_left =
          group_left + Scaled(kSwatchGroupWidth) * i / count;
      const int cell_width = Scaled(kSwatchGroupWidth) / count;
      const int x = cell_left + (cell_width - ring) / 2;
      RECT bounds = {x, row_mid - ring / 2, x + ring, row_mid + ring / 2};
      AddHit(bounds, i,
             base::UTF8ToWide(themes[static_cast<size_t>(i)].name),
             PhiHitRole::kListItem);
    }
    RECT grab = SliderTravel();
    grab.top -= Scaled(kSliderGrabPad);
    grab.bottom += Scaled(kSliderGrabPad);
    AddHit(grab, kSliderKey,
           std::wstring(PhiStr("settings.spaces.theme.saturationLabel")),
           PhiHitRole::kSlider);
  }

  // Maps a pointer x onto the slider's 0..100 travel, knob-width aware so
  // the knob's centre lands under the cursor at both ends.
  void ApplySaturationAt(int x) {
    PhiSlotHost* host = controller_ ? controller_->host() : nullptr;
    if (!host) {
      return;
    }
    const RECT travel = SliderTravel();
    const int knob = Scaled(kSliderKnob);
    const int span = (travel.right - travel.left) - knob;
    if (span <= 0) {
      return;
    }
    const int offset = x - travel.left - knob / 2;
    const int value = std::max(0, std::min(100, offset * 100 / span));
    host->SetSpaceSaturation(space_id_, value);
  }

  void EndDrag() {
    if (!dragging_) {
      return;
    }
    dragging_ = false;
    capture_.Release();
    if (dismiss_pending_ || ::GetActiveWindow() != hwnd()) {
      dismiss_pending_ = false;
      PhiPopover::OnDismiss();
    }
  }

  // The controller outlives the panel (it owns the shell that owns it).
  RAW_PTR_EXCLUSION PhiWindowController* const controller_;
  std::string space_id_;
  std::function<void()> on_closed_;
  Capture capture_;
  bool dragging_ = false;
  bool dismiss_pending_ = false;
};

}  // namespace

void PhiOpenSpaceThemePanel(PhiWindowController* controller,
                            const RECT& anchor_screen,
                            const std::string& space_id,
                            std::function<void()> on_closed) {
  if (!controller || !controller->shell_hwnd() || space_id.empty() ||
      !controller->host()) {
    if (on_closed) {
      on_closed();
    }
    return;
  }
  Panel* panel = new Panel(controller, space_id, std::move(on_closed));
  panel->DeleteWhenDestroyed();
  if (!panel->Open(anchor_screen)) {
    delete panel;
  }
}

void PhiShowSpaceThemePanel(PhiWindowController* controller,
                            const RECT& anchor_screen,
                            const std::string& space_id) {
  bool done = false;
  PhiOpenSpaceThemePanel(controller, anchor_screen, space_id,
                         [&done] { done = true; });
  if (done) {
    return;  // The panel never opened.
  }
  // A pipe verb that tore the controller down mid-loop would leave the
  // panel's back-pointer dangling; the coordinator defers them while a
  // modal scope is open. Unlike the loop this replaced, nothing lives on
  // this frame but the flag.
  PhiModalScope modal;
  MSG msg;
  while (!done && ::GetMessageW(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }
}

}  // namespace phi_client
