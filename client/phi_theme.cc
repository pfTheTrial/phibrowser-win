// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_theme.h"

#include <array>

#include "base/no_destructor.h"
#include "base/win/registry.h"

namespace phi_client {

namespace {

// The Mac writes colours as 0xRRGGBB; Win32 wants COLORREF (0x00BBGGRR).
constexpr COLORREF Hex(int rgb) {
  return RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

// Mixes `over` onto `base`; `alpha` is 0..255 of `over`.
COLORREF Blend(COLORREF over, COLORREF base, int alpha) {
  auto mix = [&](int a, int b) { return (a * alpha + b * (255 - alpha)) / 255; };
  return RGB(mix(GetRValue(over), GetRValue(base)),
             mix(GetGValue(over), GetGValue(base)),
             mix(GetBValue(over), GetBValue(base)));
}

// How much of the overlay colour reaches the window. The Mac composites the
// overlay at 0.8 and then scales it by the theme's saturation slider
// (OverlaySaturationScale, 0.1...0.9), so the slider — not the 0.8 — is what
// the user actually feels. Mapped here onto the same span: slider 0 is barely
// tinted, 100 is heavily tinted, and the default sits where the Mac's does.
constexpr int kTintMinAlpha = 20;
constexpr int kTintMaxAlpha = 160;
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
}

PhiPalette PhiResolvePalette(const PhiTheme& theme,
                             PhiAppearance appearance,
                             int saturation) {
  saturation = saturation < 0 ? 0 : (saturation > 100 ? 100 : saturation);
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
  palette.ground_top = Blend(colors.overlay, colors.background, top_alpha);
  palette.ground_bottom =
      Blend(colors.overlay, colors.background, bottom_alpha);
  palette.accent = colors.accent;
  palette.window_bg = colors.background;
  // The ink the Mac's percentage overlays are made of.
  const COLORREF ink = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
  // Alphas as /255: 4% ≈ 10, 6% ≈ 15, 8% ≈ 20, 30% ≈ 77, 80% ≈ 204, 85% ≈ 217.
  palette.hover = Blend(ink, palette.ground_top, 10);
  palette.hover_strong = Blend(ink, palette.ground_top, 20);
  palette.label_tertiary = Blend(ink, palette.ground_top, 77);
  palette.border = Blend(ink, palette.ground_bottom, 20);
  palette.separator = Blend(ink, palette.ground_top, 15);
  if (dark) {
    // sidebar-tab-selectedColor dark = WHITE at 30%, not black — the pill
    // lifts off the ground (Assets.xcassets ColorSet).
    palette.selected = Blend(RGB(0xFF, 0xFF, 0xFF), palette.ground_top, 77);
    palette.card = Blend(RGB(0xFF, 0xFF, 0xFF), palette.ground_top, 26);
    palette.label = Blend(RGB(0xFF, 0xFF, 0xFF), palette.ground_top, 204);
    palette.label_dim = RGB(0x9A, 0x9A, 0x9A);
  } else {
    palette.selected = RGB(0xFF, 0xFF, 0xFF);
    palette.card = RGB(0xFF, 0xFF, 0xFF);
    palette.label = RGB(0x26, 0x26, 0x26);   // primary-label, 85% black
    palette.label_dim = RGB(0x6E, 0x6E, 0x72);
  }
  return palette;
}

bool PhiParseColorHex(const std::string& hex, COLORREF* out) {
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
  *out = RGB(channels[0], channels[1], channels[2]);
  return true;
}

}  // namespace phi_client
