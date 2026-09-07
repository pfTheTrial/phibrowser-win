// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_EXTENSIONS_POPOVER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_EXTENSIONS_POPOVER_H_

#include <windows.h>

#include <cstdint>

#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

// The Mac extensions popover, opened from the puzzle button: enabled
// extensions as icon+name rows with a trailing pin toggle; clicking a row
// runs its action (a popup-bearing one anchors Chromium's ExtensionPopup to
// the row). One popover at a time; deactivation dismisses it; the toggle
// races are handled the downloads popover's way.
class PhiExtensionsPopover {
 public:
  static void Toggle(HWND owner,
                     const RECT& anchor_screen,
                     const PhiPalette& palette,
                     phi_framework::PhiFramework* framework,
                     BrowserId browser);
  // Re-reads the list (the delegate's OnExtensionsChanged — the async
  // refresh completed or the set changed). No-op when closed.
  static void RefreshOpen();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_EXTENSIONS_POPOVER_H_
