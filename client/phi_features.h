// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FEATURES_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FEATURES_H_

namespace phi_client {

// The Phi AI suite — everything the client puts on screen for it — is not
// supported on Windows yet. ONE switch, so the day it is, this flips back to
// true and every surface returns; nothing below it was deleted.
//
// What it gates:
//   * the "Phi & AI" pane in Settings (phi_settings_window.cc), including the
//     control channel's agentSpace.settings.set section;
//   * the Chat capsule in the sidebar's bottom bar and, in the balanced
//     layout, in the header (phi_window_controller.cc);
//   * the browser-memory chip beside it.
//
// NOT the agent Space, the skill relay, or anything else under agentSpace.*:
// those are automation, not the AI suite, and they stay live.
inline constexpr bool kPhiAiEnabled = false;

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FEATURES_H_
