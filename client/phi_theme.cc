// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_theme.h"

#include <array>

#include "base/no_destructor.h"
#include "phibrowser-win/client/phi_util.h"

#if defined(_WIN32)
#include "base/win/registry.h"
#endif

namespace phi_client {

namespace {

// The Mac writes colours as 0xRRGGBB; Win32 wants COLORREF (0x00BBGGRR).
constexpr PhiColor Hex(int rgb) {
  return PhiRgb((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// The one blend now lives in phi_util.h (PhiBlend); this file's private copy
// was the original of the eight the review found.

// How much of the overlay colour reaches the window. The Mac composites the
// overlay at 0.8 and then scales it by the theme's saturation slider
// (OverlaySaturationScale, 0.1...0.9), so the slider — not the 0.8 — is what
// the user actually feels. Mapped here onto HALF that span (Jixiang,
// 2026-09-10: the top of the slider was too heavy; the ceiling is half of
// what it was, and the default sits at 10, a light wash): slider 0 is
// barely tinted, 100 is a moderate tint.
constexpr int kTintMinAlpha = 20;
constexpr int kTintMaxAlpha = 80;
// The gradient's foot is a little stronger than its head.
constexpr int kTintGradientSpread = 36;

}  // namespace

const char* PhiAppearanceName(PhiAppearance appearance) {
  switch (appearance) {
    case PhiAppearance::kLight:
      return "light";
    case PhiAppearance::kDark:
      return "dark";
    case PhiAppearance::kSystem:
      break;
  }
  return "system";
}

bool PhiAppearanceFromName(const std::string& name, PhiAppearance* out) {
  if (name == "system") {
    *out = PhiAppearance::kSystem;
  } else if (name == "light") {
    *out = PhiAppearance::kLight;
  } else if (name == "dark") {
    *out = PhiAppearance::kDark;
  } else {
    return false;
  }
  return true;
}

const std::vector<PhiTheme>& PhiBuiltInThemes() {
  // Values transcribed from ThemedColors.swift's makeDesignTheme calls.
  static const base::NoDestructor<std::vector<PhiTheme>> themes({
      {"pure", "Pure",
       {Hex(0xFFFFFF), Hex(0xFFFFFF), Hex(0x3AA4D5)},
       {Hex(0x383838), Hex(0x383838), Hex(0x1E7099)}},
      {"mint", "Mint",
       {Hex(0x8AE25A), Hex(0xFFFFFF), Hex(0x73BD4B)},
       {Hex(0x1B380B), Hex(0x1B380B), Hex(0x48802A)}},
      {"mist", "Mist",
       {Hex(0x66CCFF), Hex(0xFFFFFF), Hex(0x57AED9)},
       {Hex(0x0B2938), Hex(0x0B2938), Hex(0x1E7099)}},
      {"aqua", "Aqua",
       {Hex(0x5BDEE3), Hex(0xFFFFFF), Hex(0x4BB9BD)},
       {Hex(0x0B3738), Hex(0x0B3738), Hex(0x2A7D80)}},
      {"iris", "Iris",
       {Hex(0x7566FF), Hex(0xFFFFFF), Hex(0x6357D9)},
       {Hex(0x100B38), Hex(0x100B38), Hex(0x3D339C)}},
      {"petal", "Petal",
       {Hex(0xD966FF), Hex(0xFFFFFF), Hex(0xB857D9)},
       {Hex(0x2D0B38), Hex(0x2D0B38), Hex(0x82339C)}},
      {"coral", "Coral",
       {Hex(0xFF6666), Hex(0xFFFFFF), Hex(0xD95757)},
       {Hex(0x380B0B), Hex(0x380B0B), Hex(0x9C3E3E)}},
      {"amber", "Amber",
       {Hex(0xFFD966), Hex(0xFFFFFF), Hex(0xD9B857)},
       {Hex(0x382D0B), Hex(0x382D0B), Hex(0x9C8133)}},
  });
  return *themes;
}

const PhiTheme& PhiThemeById(const std::string& id) {
  for (const PhiTheme& theme : PhiBuiltInThemes()) {
    if (id == theme.id) {
      return theme;
    }
  }
  return PhiBuiltInThemes().front();  // Pure is the Mac's default.
}

bool PhiSystemPrefersDark() {
#if !defined(_WIN32)
  // The host tests resolve palettes without a registry; "system" reads
  // light there, which is what an unconfigured Windows reports too.
  return false;
#else
  // The same key the Windows shell uses for app appearance; 0 means dark.
  base::win::RegKey key(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      KEY_READ);
  DWORD light = 1;
  if (key.Valid() && key.ReadValueDW(L"AppsUseLightTheme", &light) ==
                         ERROR_SUCCESS) {
    return light == 0;
  }
  return false;
#endif
}

PhiPalette PhiResolvePalette(const PhiTheme& theme,
                             PhiAppearance appearance,
                             int saturation) {
  saturation = PhiClamp(saturation, 0, 100);
  const int top_alpha =
      kTintMinAlpha + (kTintMaxAlpha - kTintMinAlpha) * saturation / 100;
  const int bottom_alpha = top_alpha + kTintGradientSpread > 255
                               ? 255
                               : top_alpha + kTintGradientSpread;
  const bool dark = appearance == PhiAppearance::kDark ||
                    (appearance == PhiAppearance::kSystem &&
                     PhiSystemPrefersDark());
  const PhiThemeColors& colors = dark ? theme.dark : theme.light;

  PhiPalette palette;
  palette.dark = dark;
  palette.ground_top = PhiBlend(colors.overlay, colors.background, top_alpha);
  palette.ground_bottom =
      PhiBlend(colors.overlay, colors.background, bottom_alpha);
  palette.accent = colors.accent;
  palette.window_bg = colors.background;
  // The ink the Mac's percentage overlays are made of.
  const PhiColor ink = dark ? PhiRgb(0xFF, 0xFF, 0xFF) : PhiRgb(0, 0, 0);
  palette.ink = ink;
  // Alphas as /255: 4% ≈ 10, 6% ≈ 15, 8% ≈ 20, 30% ≈ 77, 80% ≈ 204, 85% ≈ 217.
  palette.hover = PhiBlend(ink, palette.ground_top, 10);
  palette.hover_strong = PhiBlend(ink, palette.ground_top, 20);
  // The top bar's copies: same ink, its own ground.
  palette.bar_hover = PhiBlend(ink, palette.window_bg, 10);
  palette.bar_hover_strong = PhiBlend(ink, palette.window_bg, 20);
  palette.label_tertiary = PhiBlend(ink, palette.ground_top, 77);
  palette.border = PhiBlend(ink, palette.ground_bottom, 20);
  palette.separator = PhiBlend(ink, palette.ground_top, 15);
  if (dark) {
    // sidebar-tab-selectedColor dark = WHITE at 30%, not black — the pill
    // lifts off the ground (Assets.xcassets ColorSet).
    const PhiColor white = PhiRgb(0xFF, 0xFF, 0xFF);
    palette.selected = PhiBlend(white, palette.ground_top, 77);
    palette.card = PhiBlend(white, palette.ground_top, 26);
    palette.label = PhiBlend(white, palette.ground_top, 204);
    palette.label_dim = PhiRgb(0x9A, 0x9A, 0x9A);
  } else {
    palette.selected = PhiRgb(0xFF, 0xFF, 0xFF);
    palette.card = PhiRgb(0xFF, 0xFF, 0xFF);
    palette.label = PhiRgb(0x26, 0x26, 0x26);   // primary-label, 85% black
    palette.label_dim = PhiRgb(0x6E, 0x6E, 0x72);
  }

  // The roles the individual files were inventing. Values transcribed from
  // where they already exist: phi_popup_chrome.h:26-44 (the fixed ground and
  // the button ladder), phi_settings_window.cc:2441-2446 and :2491 (the
  // parallel palette and the close-button red), phi_shell_window.cc (the
  // second copy of that red).
  palette.ground_fixed =
      dark ? PhiRgb(0x25, 0x25, 0x25) : PhiRgb(0xEA, 0xEA, 0xEA);
  // Legibility over the accent, not a fixed white: the light Amber accent
  // (#D9B857) carries black text, everything darker carries white. The
  // threshold is Rec. 601 luma, which is what the Mac's
  // NSColor.usingColorSpace check comes out to for these eight accents.
  const int luma = (PhiRed(palette.accent) * 299 +
                    PhiGreen(palette.accent) * 587 +
                    PhiBlue(palette.accent) * 114) / 1000;
  palette.on_accent = luma > 150 ? PhiRgb(0x1A, 0x1A, 0x1A)
                                 : PhiRgb(0xFF, 0xFF, 0xFF);
  // Hover deepens the accent by pushing it a step toward the ink of the
  // OPPOSITE appearance, the way PhiAlertButton's .primary does.
  palette.accent_hover =
      PhiBlend(dark ? PhiRgb(0xFF, 0xFF, 0xFF) : PhiRgb(0, 0, 0),
               palette.accent, 26);
  palette.destructive = PhiRgb(0xFF, 0x3B, 0x30);  // systemRed
  palette.destructive_hover = PhiRgb(0xE0, 0x34, 0x2A);
  palette.warning = dark ? PhiRgb(0xD9, 0xA5, 0x27) : PhiRgb(0xB0, 0x80, 0x00);
  palette.error = dark ? PhiRgb(0xE8, 0x5D, 0x5D) : PhiRgb(0xC0, 0x1E, 0x1E);
  // The dim behind a modal / the group overview: 40% black over the ground
  // in light, 55% in dark (a dark ground needs more to read as "behind").
  palette.overlay_scrim =
      PhiBlend(PhiRgb(0, 0, 0), palette.ground_top, dark ? 140 : 102);
  return palette;
}

bool PhiParseColorHex(const std::string& hex, PhiColor* out) {
  const size_t start = !hex.empty() && hex[0] == '#' ? 1 : 0;
  if (hex.size() - start != 6) {
    return false;
  }
  std::array<int, 3> channels = {};
  for (size_t i = 0; i < channels.size(); ++i) {
    for (size_t nibble = 0; nibble < 2; ++nibble) {
      const char c = hex[start + i * 2 + nibble];
      int value = 0;
      if (c >= '0' && c <= '9') {
        value = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        value = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        value = c - 'A' + 10;
      } else {
        return false;
      }
      channels[i] = channels[i] * 16 + value;
    }
  }
  *out = PhiRgb(channels[0], channels[1], channels[2]);
  return true;
}

}  // namespace phi_client
