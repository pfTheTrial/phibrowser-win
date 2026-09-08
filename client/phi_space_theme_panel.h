// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SPACE_THEME_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SPACE_THEME_PANEL_H_

#include <windows.h>

#include <functional>
#include <string>

namespace phi_client {

class PhiShellWindow;

// The Mac's SpaceThemeEditorView (ThemeOpacitySlider.swift), shown from a
// Space menu's "Set Theme" item: one Color row of theme swatches over one
// Saturation slider, both editing the Space named by `space_id`. Every
// change applies LIVE — the Mac panel has no confirm button — so the
// window behind it recolors as the pointer moves.
//
// The panel is a PhiPopover (docs/architecture-review.md §3.6): it does
// NOT run a message loop. `on_closed` runs once, after the window is gone
// (Escape, or focus leaving the panel).
void PhiOpenSpaceThemePanel(PhiShellWindow* controller,
                            const RECT& anchor_screen,
                            const std::string& space_id,
                            std::function<void()> on_closed);

// TRANSITIONAL synchronous wrapper, exactly as for the folder icon picker:
// it drives the asynchronous panel through a short local message loop so
// the existing call site (in phi_shell_window.cc, a file phase 5 does
// not own) keeps compiling. Phase 6 moves that call site onto
// PhiOpenSpaceThemePanel and deletes this.
void PhiShowSpaceThemePanel(PhiShellWindow* controller,
                            const RECT& anchor_screen,
                            const std::string& space_id);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SPACE_THEME_PANEL_H_
