// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_THEME_H_
#define PHIBROWSER_WIN_CLIENT_PHI_THEME_H_

#if defined(_WIN32)
#include <windows.h>
#endif

#include <string>
#include <vector>

#include "phibrowser-win/client/phi_util.h"

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
  PhiColor overlay = PhiRgb(0xFF, 0xFF, 0xFF);
  PhiColor background = PhiRgb(0xFF, 0xFF, 0xFF);
  PhiColor accent = PhiRgb(0x3A, 0xA4, 0xD5);
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
  PhiColor ground_top;
  PhiColor ground_bottom;
  PhiColor card;            // The settings window's section cards.
  PhiColor selected;        // sidebar-tab-selectedColor: white / white 30%.
  PhiColor hover;           // sidebar-tab-hoveredColor: ink 4%.
  PhiColor hover_strong;    // …-emphasized: ink 8%. Also resting field fill.
  // The same two overlays composited over `window_bg` instead of the tinted
  // ground — what the Mac gets for free by keeping them translucent. The top
  // bar stands on windowBackground, so anything washed THERE (the balanced /
  // comfortable address field) must blend over that, not over the Space tint:
  // blending over the tint paints a themed pill on an untinted bar.
  PhiColor bar_hover;
  PhiColor bar_hover_strong;
  PhiColor label;           // textPrimary: black 85% / white 80%.
  PhiColor label_dim;       // textSecondary.
  PhiColor label_tertiary;  // textTertiary: ink 30% — "New Tab", placeholders.
  PhiColor border;          // ink 8%.
  PhiColor separator;       // ink 6%.
  PhiColor window_bg;       // windowBackground — the top bar's own ground.
  PhiColor accent;

  // The roles individual files invented for themselves, brought into the one
  // token set (lane 7 §2.2 counted 143 hard-coded RGB() outside this file).
  // Each is the value those files were already using, so adopting a token is
  // not a redesign — it is a deletion.

  // The ink the percentage overlays above are made of: black in light,
  // white in dark. Nine files derive their own washes from a literal
  // RGB(0,0,0) / RGB(0xFF,0xFF,0xFF); this is that constant, once.
  PhiColor ink;
  // The FIXED window ground — #EAEAEA / #252525, the Mac's
  // DefaultColors.windowBackground. Secondary windows (Settings, Feedback,
  // PhiAlert, the transcript) stand on this and never on the per-Space tint,
  // which is why phi_settings_window.cc:2441 defines a parallel palette
  // today and phi_popup_chrome.h:26 a second struct.
  PhiColor ground_fixed;
  // What is legible ON `accent`: the label of an accent-filled button, the
  // glyph in an accent pill. Files pick a literal white today, which is
  // wrong for the light Amber accent.
  PhiColor on_accent;
  // The accent under the cursor — PhiAlertButton's .primary hover.
  PhiColor accent_hover;
  // systemRed and its hover: the close-button wash (two files carry
  // RGB(0xC4,0x2B,0x1C)), a Delete action, a failed download.
  PhiColor destructive;
  PhiColor destructive_hover;
  // A counter near its limit, and a hard failure — the feedback form's two.
  PhiColor warning;
  PhiColor error;
  // The dim laid over the content behind a modal, the group overview and
  // the tab switcher. Opaque here for the same reason everything else is:
  // GDI does not composite. Blend it yourself when the surface is layered.
  PhiColor overlay_scrim;

  bool dark = false;
};

// The design-unit table: the metrics that recur ACROSS surfaces, in Mac
// design points at 96 dpi. Scale exactly one way, with PhiDip(dpi, design)
// from phi_util.h — never MulDiv by hand, never a second `Scaled()` member.
//
// Surface-specific metrics stay with their surface (sidebar_metrics in
// phi_sidebar_view.h is the model: named, commented against the Mac view
// they came from, and in ONE place). What belongs here is what two or more
// unrelated files would otherwise both invent.
struct PhiMetrics {
  // Corner radii. The client draws every one of these through phi_dwrite's
  // antialiased path — never GDI's RoundRect.
  int corner_chip = 6;    // Hover washes behind an icon button.
  int corner = 8;         // THE pill radius: rows, buttons, fields.
  int corner_card = 12;   // Section cards, popover bodies.
  int corner_window = 14; // A layered window's own corners.

  // Rhythm.
  int pad_tight = 4;
  int pad = 8;
  int pad_card = 12;
  int pad_window = 16;

  // Rows and controls.
  int row_height = 32;     // A sidebar row's pill, a menu row.
  int row_gap = 4;
  int control_height = 28; // A field, a segmented control.
  int hairline = 1;        // A separator, a border. Scale it too: a 1px
                           // hairline at 200% is a half-weight line.

  // PhiAlert's actions (phi_prompt.cc's 130x36 buttons).
  int button_width = 130;
  int button_height = 36;

  // Icons.
  int icon = 16;
  int icon_large = 20;

  // Type sizes, design points — the argument PhiPopupFont takes.
  int text_small = 12;
  int text_body = 13;
  int text_title = 18;
};
inline constexpr PhiMetrics kPhiMetrics = {};

// Resolves `theme` for `appearance`; kSystem asks Windows which it is.
// `saturation` is the Mac's slider position, 0..100: how much of the theme's
// overlay colour reaches the window (OverlaySaturationScale).
PhiPalette PhiResolvePalette(const PhiTheme& theme,
                             PhiAppearance appearance,
                             int saturation = 10);
// True when Windows is currently set to a dark app appearance.
bool PhiSystemPrefersDark();

// Parses the wire's "#RRGGBB" (case-insensitive; the leading '#' optional)
// into a COLORREF. False on anything else — the caller keeps its default.
bool PhiParseColorHex(const std::string& hex, PhiColor* out);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_THEME_H_
