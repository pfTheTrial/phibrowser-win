// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_UPDATER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_UPDATER_H_

namespace phi_client {

// The Windows update check — WinSparkle over the SAME appcast pipeline the
// Mac's Sparkle uses (ota.phibrowser.com/<channel>/appcast). WinSparkle.dll
// is vendored (third_party/winsparkle) and loaded DYNAMICALLY: a dev deploy
// without the DLL simply has no updater, it never fails to launch.
//
// The flow mirrors the Mac: background checks find an update and WinSparkle
// walks the user through download; the downloaded installer runs when the
// app quits and updates the per-user install in place (keeping a rollback
// snapshot — installer/phi-installer.nsi).
class PhiUpdater {
 public:
  // Loads the DLL, configures the appcast + app identity + the feed's
  // EdDSA public key, and starts the background check schedule. Safe to
  // call once at startup, after the first window exists. No-op (with a log
  // line) when the DLL is absent — and when no signing key is built in: an
  // update that cannot be verified is never offered, whatever the build.
  // A --dev-hooks build honours the PHI_APPCAST_URL / PHI_APPCAST_ED25519
  // environment overrides for dev/test rings.
  static void Init();

  // "Check for Update..." — the interactive check with WinSparkle's UI.
  // False when the updater is unavailable (DLL missing, no key).
  static bool CheckForUpdateWithUi();

  // True when the DLL loads and a signing key is built in (drives the menu
  // item's enabled state).
  static bool Available();

  // win_sparkle_cleanup on shutdown.
  static void Shutdown();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_UPDATER_H_
