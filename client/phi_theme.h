// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_THEME_H_
#define PHIBROWSER_WIN_CLIENT_PHI_THEME_H_

#include <windows.h>

#include <string>
#include <vector>

namespace phi_client {

// The Mac client's theme model, ported (Utilities/Theme/Theme.swift and
// ThemedColors.swift): eight built-in themes, each carrying a light and a dark
// palette, plus a separate three-state appearance choice. Ids and names match
// the Mac raw values so a stored preference means the same thing on both.
enum class PhiAppearance { kSystem, kLight, kDark };

const char* PhiAppearanceName(PhiAppearance appearance);
bool PhiAppearanceFromName(const std::string& name, PhiAppearance* out);

// One theme's colours for one appearance. `windowOverlayBackground` over
// `windowBackground` is what tints the window; `themeColor` is the accent the
// Mac uses for pills and controls.
struct PhiThemeColors {
  COLORREF overlay = RGB(0xFF, 0xFF, 0xFF);
  COLORREF background = RGB(0xFF, 0xFF, 0xFF);
  COLORREF accent = RGB(0x3A, 0xA4, 0xD5);
};

struct PhiTheme {
  const char* id;
  const char* name;
  PhiThemeColors light;
  PhiThemeColors dark;
};

// In the Mac's order: Pure, Mint, Mist, Aqua, Iris, Petal, Coral, Amber.
const std::vector<PhiTheme>& PhiBuiltInThemes();
// The theme with `id`, or Pure (the default) when unknown.
const PhiTheme& PhiThemeById(const std::string& id);

// What the chrome actually paints with, once a theme and an appearance have
// been resolved. Everything the client draws reads from here, so switching
// theme or appearance is a single recompute.
//
// GDI paints opaque, so the Mac's alpha overlays (black/white at 4%, 8%, …)
// are pre-blended over the ground here. The percentages are the Mac's asset
// values (Assets.xcassets ColorSet / DefaultColors.swift).
struct PhiPalette {
  COLORREF ground_top;
  COLORREF ground_bottom;
  COLORREF card;            // The settings window's section cards.
  COLORREF selected;        // sidebar-tab-selectedColor: white / white 30%.
  COLORREF hover;           // sidebar-tab-hoveredColor: ink 4%.
  COLORREF hover_strong;    // …-emphasized: ink 8%. Also resting field fill.
  COLORREF label;           // textPrimary: black 85% / white 80%.
  COLORREF label_dim;       // textSecondary.
  COLORREF label_tertiary;  // textTertiary: ink 30% — "New Tab", placeholders.
  COLORREF border;          // ink 8%.
  COLORREF separator;       // ink 6%.
  COLORREF window_bg;       // windowBackground — the top bar's own ground.
  COLORREF accent;
  bool dark = false;
};

// Resolves `theme` for `appearance`; kSystem asks Windows which it is.
// `saturation` is the Mac's slider position, 0..100: how much of the theme's
// overlay colour reaches the window (OverlaySaturationScale).
PhiPalette PhiResolvePalette(const PhiTheme& theme,
                             PhiAppearance appearance,
                             int saturation = 50);
// True when Windows is currently set to a dark app appearance.
bool PhiSystemPrefersDark();

// Parses the wire's "#RRGGBB" (case-insensitive; the leading '#' optional)
// into a COLORREF. False on anything else — the caller keeps its default.
bool PhiParseColorHex(const std::string& hex, COLORREF* out);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_THEME_H_
