// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TRANSCRIPT_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TRANSCRIPT_PANEL_H_

#include <windows.h>

#include <string>
#include <vector>

#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

// What the panel needs from its owner (the coordinator). All calls on the
// browser UI thread.
class PhiTranscriptHost {
 public:
  virtual ~PhiTranscriptHost() = default;
  // The rendered transcript lines for `task_id`, oldest first (empty when
  // the task is unknown or has logged nothing).
  virtual std::vector<std::string> TranscriptLines(
      const std::string& task_id) = 0;
  // The driving agent's display name, for the panel title.
  virtual std::string TranscriptAgentName(const std::string& task_id) = 0;
  // Queues a console message from the user (the driver drains it on its
  // next poll). False when the task is gone.
  virtual bool PushTranscriptMessage(const std::string& task_id,
                                     const std::string& text) = 0;
};

// The live transcript console for one agent Space — the Windows shape of the
// Mac AgentTranscriptPanel's floating-window placement: a resizable tool
// window with the task's log feed above and a console input row below. One
// panel per process, retargeted by Toggle; the feed follows agentSpace.log
// through RefreshOpen (the extensions popover's pattern).
class PhiTranscriptPanel {
 public:
  // Opens the panel on `task_id` (or closes it when it is already showing
  // that task — the Mac toggle). `owner` is the shell window the panel
  // parks beside.
  static void Toggle(HWND owner,
                     const PhiPalette& palette,
                     const std::string& task_id,
                     PhiTranscriptHost* host);
  // Re-reads the feed if the panel is open (new log lines arrived).
  static void RefreshOpen();
  // Closes the panel if it is showing `task_id` (the task ended).
  static void CloseFor(const std::string& task_id);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TRANSCRIPT_PANEL_H_
