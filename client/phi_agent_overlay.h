// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AGENT_OVERLAY_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AGENT_OVERLAY_H_

#include <windows.h>

#include <string>

#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

class PhiWindowController;

// The watch-mode overlay for an agent Space's content area — the Windows twin
// of the Mac AgentSpaceOverlayView. Two sibling child windows above the
// reparented browser child:
//
//   the FOG   a layered wash over the whole content area that SWALLOWS mouse
//             input while the agent owns the Space, so a watching user cannot
//             disturb the page by accident; ownership changes only through
//             the pill's explicit button.
//   the PILL  agent name, status caption, and the control buttons
//             ("Take control" while the agent owns; "Hand back" + "Finish"
//             once the user has it).
//
// Native windows rather than injected DOM, for the Mac's reasons: they
// survive navigation, work over any page, and can reliably act as the input
// interceptor.
class PhiAgentOverlay {
 public:
  // Mounts (or updates) the overlay over `controller`'s content area.
  // `agent_owns` drives both the barrier and the button set.
  static void Show(PhiWindowController* controller,
                   const std::string& task_id,
                   const std::wstring& agent_label,
                   const std::wstring& caption,
                   bool agent_owns);
  // Tears the overlay down (watch mode ended, or the task is gone).
  static void Hide();
  // True while mounted — the shell consults this before handing content
  // clicks to the browser child.
  static bool IsShown();
  // Follows a content-rect change (window resize, layout mode switch).
  static void Reposition();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AGENT_OVERLAY_H_
