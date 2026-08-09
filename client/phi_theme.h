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
struct PhiPalette {
  COLORREF ground_top;
  COLORREF ground_bottom;
  COLORREF card;        // Selected rows, chips, the address field.
  COLORREF label;
  COLORREF label_dim;
  COLORREF border;
  COLORREF accent;
  bool dark = false;
};

// Resolves `theme` for `appearance`; kSystem asks Windows which it is.
PhiPalette PhiResolvePalette(const PhiTheme& theme, PhiAppearance appearance);
// True when Windows is currently set to a dark app appearance.
bool PhiSystemPrefersDark();

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_THEME_H_
