// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SEARCH_TABS_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SEARCH_TABS_PANEL_H_

namespace phi_client {

class PhiWindowController;

// The Mac SearchTabs palette (Ctrl+Shift+A): a centered floating panel
// searching across open tabs (every window of the profile), pinned tabs,
// bookmarks, and recently-closed entries. Singleton; deactivation or Escape
// dismisses; Enter/click activates the selection.
class PhiSearchTabsPanel {
 public:
  // Opens the palette for `controller`'s shell (or closes it if open).
  static void Toggle(PhiWindowController* controller);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SEARCH_TABS_PANEL_H_
