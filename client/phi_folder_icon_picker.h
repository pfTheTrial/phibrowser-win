// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FOLDER_ICON_PICKER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FOLDER_ICON_PICKER_H_

#include <windows.h>

#include <string>

namespace phi_client {

class PhiWindowController;

// The Mac's BookmarkFolderIconPicker, shown from a bookmark folder's
// "Change Icon..." menu item: the 24 catalog symbols in a 6-column grid,
// the current one marked, a click choosing.
//
// Modal like the other client pickers — it runs its own message loop and
// returns the chosen id, or an empty string when dismissed.
std::string PhiPickFolderIcon(PhiWindowController* controller,
                              const RECT& anchor_screen,
                              const std::string& selected);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FOLDER_ICON_PICKER_H_
