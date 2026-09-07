// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_GROUP_OVERVIEW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_GROUP_OVERVIEW_H_

#include <string>

namespace phi_client {

class PhiWindowController;

// The Mac Group Overview: a full-content grid of one tab group's tabs
// (snapshot cards with favicon/title footers), opened by double-clicking
// the group's header. Click selects a tab, the card's ✕ closes it, the
// trailing "+" card appends a tab to the group, Escape (or focus loss)
// leaves the overview.
class PhiGroupOverview {
 public:
  static bool IsOpen();
  static void Open(PhiWindowController* controller,
                   const std::string& group_id);
  static void Close();
  // Close() only if the open overview belongs to `controller` (its shell is
  // being destroyed; called from ~PhiWindowController).
  static void CloseIfBoundTo(const PhiWindowController* controller);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_GROUP_OVERVIEW_H_
