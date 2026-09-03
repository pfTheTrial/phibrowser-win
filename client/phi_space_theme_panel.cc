// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_space_theme_panel.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"
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

const wchar_t kClassName[] = L"PhiSpaceThemePanel";

struct PanelState {
  // Stack-scoped for one modal run; the controller outlives it.
  RAW_PTR_EXCLUSION PhiWindowController* controller = nullptr;
  std::string space_id;
  bool done = false;
  bool dragging = false;
  int hover = -1;  // Index into the swatch row.
  UINT dpi = 96;
  std::vector<RECT> swatches;  // Ring bounds, index-aligned with the catalog.
  RECT slider{};               // The track's full travel band.
};

int Scaled(const PanelState& state, int design) {
  return ::MulDiv(design, static_cast<int>(state.dpi), 96);
}

// The panel edits through the host, so both values are read back fresh on
// every paint — a click applies immediately and the window behind recolors.
std::string CurrentThemeId(const PanelState& state) {
  PhiSlotHost* host = state.controller ? state.controller->host() : nullptr;
  if (!host) {
    return std::string();
  }
  // The Mac's resolvedThemeId: a Space with no pin of its own shows — and
  // edits from — the app-wide theme, so the row always has a selection.
  const std::string pinned = host->SpaceThemeId(state.space_id);
  return pinned.empty() ? host->ThemeId() : pinned;
}

int CurrentSaturation(const PanelState& state) {
  PhiSlotHost* host = state.controller ? state.controller->host() : nullptr;
  if (!host) {
    return 0;
  }
  // -1 means the Space follows the app-wide slider, which is the value its
  // own slider then shows (the Spaces settings pane's rule).
  const int stored = host->SpaceSaturation(state.space_id);
  return stored < 0 ? host->Saturation() : stored;
}

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

void BuildLayout(PanelState& state) {
  state.swatches.clear();
  const int card_left = Scaled(state, kCardInset) + Scaled(state, kCardPadding);
  const int card_right = Scaled(state, kPanelWidth) -
                         Scaled(state, kCardInset) - Scaled(state, kCardPadding);
  const int card_top = Scaled(state, kCardInset);

  // Color row: the swatch group hugs the card's right edge, its rings evenly
  // spread across the group's width (the Mac's maxWidth: .infinity cells).
  const size_t count = PhiBuiltInThemes().size();
  const int group_left = card_right - Scaled(state, kSwatchGroupWidth);
  const int ring = Scaled(state, kSwatchRing);
  const int row_mid = card_top + Scaled(state, kColorRowHeight) / 2;
  for (size_t i = 0; i < count; ++i) {
    const int cell_left =
        group_left + static_cast<int>(Scaled(state, kSwatchGroupWidth) *
                                      static_cast<int>(i) /
                                      static_cast<int>(count));
    const int cell_width =
        Scaled(state, kSwatchGroupWidth) / static_cast<int>(count);
    const int x = cell_left + (cell_width - ring) / 2;
    state.swatches.push_back({x, row_mid - ring / 2, x + ring,
                              row_mid + ring / 2});
  }

  // Saturation row: the slider sits below the divider, right-aligned too.
  const int slider_mid = card_top + Scaled(state, kColorRowHeight) + 1 +
                         Scaled(state, kSaturationRowHeight) / 2;
  const int knob = Scaled(state, kSliderKnob);
  state.slider = {card_right - Scaled(state, kSliderWidth),
                  slider_mid - knob / 2, card_right, slider_mid + knob / 2};
  (void)card_left;
}

void DrawLabel(HDC dc,
               const PanelState& state,
               const RECT& bounds,
               const wchar_t* text,
               COLORREF ink) {
  HFONT font = ::CreateFontW(-Scaled(state, kLabelSize), 0, 0, 0, FW_NORMAL,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  HGDIOBJ prev = ::SelectObject(dc, font);
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, ink);
  RECT rect = bounds;
  ::DrawTextW(dc, text, -1, &rect,
              DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
  ::SelectObject(dc, prev);
  ::DeleteObject(font);
}

void Paint(HWND hwnd, PanelState& state, HDC dc) {
  RECT rc = {};
  ::GetClientRect(hwnd, &rc);
  const PhiPalette palette = state.controller
                                 ? state.controller->CurrentPalette()
                                 : PhiResolvePalette(PhiThemeById("pure"),
                                                     PhiAppearance::kSystem);
  HBRUSH ground = ::CreateSolidBrush(palette.ground_top);
  ::FillRect(dc, &rc, ground);
  ::DeleteObject(ground);

  // The inner hairline card.
  RECT card = {Scaled(state, kCardInset), Scaled(state, kCardInset),
               Scaled(state, kPanelWidth) - Scaled(state, kCardInset),
               Scaled(state, kPanelHeight) - Scaled(state, kCardInset)};
  PhiStrokeRoundRectAA(dc, card, Scaled(state, kCardRadius), palette.border,
                       1);

  const int card_left = card.left + Scaled(state, kCardPadding);
  const int card_top = card.top;
  RECT color_label = {card_left, card_top, card.right,
                      card_top + Scaled(state, kColorRowHeight)};
  DrawLabel(dc, state, color_label, PhiStr("settings.spaces.theme.colorLabel"),
            palette.label);

  // The divider between the two rows, the Mac's separatorColor hairline.
  const int divider_y = card_top + Scaled(state, kColorRowHeight);
  RECT divider = {card.left + 1, divider_y, card.right - 1, divider_y + 1};
  HBRUSH line = ::CreateSolidBrush(palette.separator);
  ::FillRect(dc, &divider, line);
  ::DeleteObject(line);

  RECT sat_label = {card_left, divider_y + 1, card.right,
                    divider_y + 1 + Scaled(state, kSaturationRowHeight)};
  DrawLabel(dc, state, sat_label,
            PhiStr("settings.spaces.theme.saturationLabel"), palette.label);

  // The swatches: Pure reads as a white dot with a contrast border, every
  // other theme as its own accent; the selected one wears the ring.
  const std::string selected = CurrentThemeId(state);
  const auto& themes = PhiBuiltInThemes();
  const bool dark = palette.dark;
  for (size_t i = 0; i < themes.size() && i < state.swatches.size(); ++i) {
    const RECT& ring_rect = state.swatches[i];
    const PhiThemeColors& colors = dark ? themes[i].dark : themes[i].light;
    const COLORREF accent = colors.accent;
    const bool is_selected = selected == themes[i].id;
    if (is_selected || state.hover == static_cast<int>(i)) {
      PhiStrokeCapsuleAA(dc, ring_rect, accent, is_selected ? 2 : 1);
    }
    const int dot = Scaled(state, kSwatchDot);
    RECT dot_rect = {ring_rect.left + ((ring_rect.right - ring_rect.left) - dot) / 2,
                     ring_rect.top + ((ring_rect.bottom - ring_rect.top) - dot) / 2,
                     0, 0};
    dot_rect.right = dot_rect.left + dot;
    dot_rect.bottom = dot_rect.top + dot;
    // Pure has no color of its own — the Mac fills its dot white and rings
    // it so it stays visible on a white card.
    const bool is_pure = themes[i].id == std::string("pure");
    PhiFillCapsuleAA(dc, dot_rect, is_pure ? RGB(0xFF, 0xFF, 0xFF) : accent);
    if (is_pure) {
      PhiStrokeCapsuleAA(dc, dot_rect, palette.border, 1);
    }
  }

  // The slider: the Mac's 10pt track carrying a gradient of the SELECTED
  // theme's overlay — saturation 0.1 -> 0.9, or, for Pure, brightness from
  // its light/dark range (brightest at the left) — under a white knob.
  const PhiTheme& theme = PhiThemeById(selected);
  const PhiThemeColors& colors = dark ? theme.dark : theme.light;
  const int mid = (state.slider.top + state.slider.bottom) / 2;
  const int half = Scaled(state, kSliderTrackHeight) / 2;
  RECT track = {state.slider.left, mid - half, state.slider.right, mid + half};
  const int span = track.right - track.left;
  double hue = 0, sat = 0, val = 0;
  RgbToHsv(colors.overlay, &hue, &sat, &val);
  const int save_clip = ::SaveDC(dc);
  HRGN clip = ::CreateRoundRectRgn(track.left, track.top, track.right + 1,
                                   track.bottom + 1, half * 2, half * 2);
  ::ExtSelectClipRgn(dc, clip, RGN_AND);
  // Pure ramps brightness where every other theme ramps saturation.
  const bool is_pure = selected == "pure";
  const int bands = 32;
  for (int i = 0; i < bands; ++i) {
    RECT band = {track.left + span * i / bands, track.top,
                 track.left + span * (i + 1) / bands, track.bottom};
    const double t = (i + 0.5) / bands;
    const COLORREF tint =
        is_pure ? HsvToRgb(hue, sat,
                           (dark ? 0.40 : 0.94) +
                               ((dark ? 0.0 : 0.84) - (dark ? 0.40 : 0.94)) * t)
                : HsvToRgb(hue, 0.1 + 0.8 * t, val);
    HBRUSH brush = ::CreateSolidBrush(tint);
    ::FillRect(dc, &band, brush);
    ::DeleteObject(brush);
  }
  ::RestoreDC(dc, save_clip);
  ::DeleteObject(clip);
  PhiStrokeCapsuleAA(dc, track, palette.border, 1);

  const int knob = Scaled(state, kSliderKnob);
  const int at = track.left + (span - knob) * CurrentSaturation(state) / 100;
  RECT dot = {at, mid - knob / 2, at + knob, mid + knob / 2};
  PhiFillCapsuleAA(dc, dot, RGB(0xFF, 0xFF, 0xFF));
  PhiStrokeCapsuleAA(dc, dot, palette.border, 1);
}

// Maps a pointer x onto the slider's 0..100 travel, knob-width aware so the
// knob's centre lands under the cursor at both ends.
void ApplySaturationAt(PanelState& state, int x) {
  PhiSlotHost* host = state.controller ? state.controller->host() : nullptr;
  if (!host) {
    return;
  }
  const int knob = Scaled(state, kSliderKnob);
  const int span = (state.slider.right - state.slider.left) - knob;
  if (span <= 0) {
    return;
  }
  const int offset = x - state.slider.left - knob / 2;
  const int value = std::max(0, std::min(100, offset * 100 / span));
  host->SetSpaceSaturation(state.space_id, value);
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  auto* state = reinterpret_cast<PanelState*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (state) {
        Paint(hwnd, *state, dc);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!state) {
        break;
      }
      const POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      if (state->dragging) {
        ApplySaturationAt(*state, pt.x);
        ::InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      int hover = -1;
      for (size_t i = 0; i < state->swatches.size(); ++i) {
        if (::PtInRect(&state->swatches[i], pt)) {
          hover = static_cast<int>(i);
          break;
        }
      }
      if (hover != state->hover) {
        state->hover = hover;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      if (state && state->hover != -1) {
        state->hover = -1;
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      if (!state) {
        break;
      }
      const POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      const auto& themes = PhiBuiltInThemes();
      for (size_t i = 0; i < state->swatches.size() && i < themes.size(); ++i) {
        if (::PtInRect(&state->swatches[i], pt)) {
          PhiSlotHost* host =
              state->controller ? state->controller->host() : nullptr;
          if (host) {
            host->SetSpaceTheme(state->space_id, themes[i].id);
          }
          ::InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
      }
      // Anywhere on the slider row grabs the knob, the Mac's slider
      // behaviour: the press jumps it, the drag carries it.
      RECT band = state->slider;
      band.top -= Scaled(*state, 8);
      band.bottom += Scaled(*state, 8);
      if (::PtInRect(&band, pt)) {
        state->dragging = true;
        ::SetCapture(hwnd);
        ApplySaturationAt(*state, pt.x);
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONUP:
      if (state && state->dragging) {
        state->dragging = false;
        ::ReleaseCapture();
      }
      return 0;
    case WM_KEYDOWN:
      if (state && w == VK_ESCAPE) {
        state->done = true;
        ::PostMessageW(hwnd, WM_NULL, 0, 0);  // Wake the modal loop.
      }
      return 0;
    case WM_ACTIVATE:
      // Dismiss like a popover the moment focus goes elsewhere — but not
      // while the knob is captured, where the shell repaint behind can
      // steal activation mid-drag.
      if (state && LOWORD(w) == WA_INACTIVE && !state->dragging) {
        state->done = true;
        ::PostMessageW(hwnd, WM_NULL, 0, 0);
      }
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

void RegisterPanelClass() {
  static const bool registered = [] {
    WNDCLASSW cls = {};
    cls.lpfnWndProc = &PanelProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kClassName;
    return ::RegisterClassW(&cls) != 0;
  }();
  (void)registered;
}

}  // namespace

void PhiShowSpaceThemePanel(PhiWindowController* controller,
                            const RECT& anchor_screen,
                            const std::string& space_id) {
  HWND owner = controller ? controller->shell_hwnd() : nullptr;
  if (!owner || space_id.empty() || !controller->host()) {
    return;
  }
  RegisterPanelClass();

  PanelState state;
  state.controller = controller;
  state.space_id = space_id;
  state.dpi = ::GetDpiForWindow(owner);
  BuildLayout(state);

  const int w = Scaled(state, kPanelWidth);
  const int h = Scaled(state, kPanelHeight);
  // A point anchor (a menu click) hangs the panel off its left edge; an
  // anchor WIDER than the panel — a window's caption band — centres it
  // there instead, which is the Mac's two placements, nearTrigger and
  // windowTop.
  const int anchor_width = anchor_screen.right - anchor_screen.left;
  int x = anchor_width > w ? anchor_screen.left + (anchor_width - w) / 2
                           : anchor_screen.left;
  int y = anchor_screen.bottom + Scaled(state, 4);
  HMONITOR monitor = ::MonitorFromRect(&anchor_screen, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info = {sizeof(info)};
  if (::GetMonitorInfoW(monitor, &info)) {
    x = std::max(static_cast<int>(info.rcWork.left),
                 std::min(x, static_cast<int>(info.rcWork.right) - w));
    if (y + h > info.rcWork.bottom) {
      y = std::max(static_cast<int>(info.rcWork.top),
                   static_cast<int>(anchor_screen.top) - h - Scaled(state, 4));
    }
  }

  HWND panel = ::CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP,
                                 x, y, w, h, owner, nullptr,
                                 ::GetModuleHandleW(nullptr), nullptr);
  if (!panel) {
    return;
  }
  ::SetWindowLongPtrW(panel, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&state));
  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(panel, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                          sizeof(corner));
  ::ShowWindow(panel, SW_SHOW);
  ::SetForegroundWindow(panel);

  MSG msg;
  while (!state.done && ::GetMessageW(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  ::SetWindowLongPtrW(panel, GWLP_USERDATA, 0);
  ::DestroyWindow(panel);
}

}  // namespace phi_client
