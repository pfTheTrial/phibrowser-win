// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TAB_SWITCHER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TAB_SWITCHER_H_

namespace phi_client {

class PhiShellWindow;

// The Mac Ctrl+Tab MRU switcher HUD: a centered card strip in most-recently-
// used order. Ctrl+Tab opens it selecting the previous tab; further presses
// advance (Shift reverses); RELEASING Ctrl commits the selection.
class PhiTabSwitcher {
 public:
  static bool IsActive();
  // Opens the HUD (when inactive) or advances the selection by `direction`.
  static void Cycle(PhiShellWindow* controller, int direction);
  // Activates the selection and closes.
  static void Commit();
  static void Cancel();
  // Cancel() only if the HUD is cycling `controller`'s tabs (its shell is
  // being destroyed; called from ~PhiShellWindow).
  static void CancelIfBoundTo(const PhiShellWindow* controller);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TAB_SWITCHER_H_
