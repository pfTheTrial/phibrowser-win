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
//     layout, in the header (phi_shell_window.cc);
//   * the browser-memory chip beside it.
//
// NOT the agent Space, the skill relay, or anything else under agentSpace.*:
// those are automation, not the AI suite, and they stay live.
inline constexpr bool kPhiAiEnabled = false;

// Developer-only hooks. A release build NEVER defines PHI_DEV_HOOKS
// (build-client.py --dev-hooks is the only way to set it), so everything
// behind this constant compiles out of the shipped exe:
//   * PHI_APPCAST_URL / PHI_APPCAST_ED25519 — replace the update feed and its
//     signing key (phi_updater.cc);
//   * PHI_VAULT_HELPER — replace the credential helper binary, and the
//     local "dev vault" fallback that accepts a PLAINTEXT PhiCredentials.dat
//     when the helper is absent (phi_coordinator.cc, phi_credentials.cc).
// Runtime test-rig switches (harness-only verbs, agent access to the user's
// own Spaces) are NOT here: they are PhiPrefs.json keys the agent cannot set
// (see PhiCoordinator::SetPref refusals), so one binary serves both the box
// and a release. Gate with `if constexpr (kPhiDevHooks)` so the code still
// compiles in both configurations.
#if defined(PHI_DEV_HOOKS)
inline constexpr bool kPhiDevHooks = true;
#else
inline constexpr bool kPhiDevHooks = false;
#endif

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FEATURES_H_
