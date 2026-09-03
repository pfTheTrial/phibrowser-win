// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SPACE_THEME_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SPACE_THEME_PANEL_H_

#include <windows.h>

#include <string>

namespace phi_client {

class PhiWindowController;

// The Mac's SpaceThemeEditorView (ThemeOpacitySlider.swift), shown from a
// Space menu's "Set Theme" item: one Color row of theme swatches over one
// Saturation slider, both editing the Space named by `space_id`. Every
// change applies LIVE — the Mac panel has no confirm button — so the
// window behind it recolors as the pointer moves.
//
// Modal like the other client pickers: it runs its own message loop and
// returns once dismissed (Escape, or focus leaving the panel).
void PhiShowSpaceThemePanel(PhiWindowController* controller,
                            const RECT& anchor_screen,
                            const std::string& space_id);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SPACE_THEME_PANEL_H_
