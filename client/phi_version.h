// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_VERSION_H_
#define PHIBROWSER_WIN_CLIENT_PHI_VERSION_H_

// The client's own version — what the updater reports to the appcast and
// what About shows. The release pipeline (installer/make-installer.sh)
// stamps the installer from this same value, so bump it here and only here.
#define PHI_WIN_VERSION_STRING "0.1.0"
#define PHI_WIN_VERSION_WIDE L"0.1.0"

// The release ring this build ships on, as feedback reports name it (the
// Mac maps its nightly builds to "canary" and everything else to "stable";
// a DCHECK build reports "debug" regardless). Set alongside the version
// when a ring changes.
#define PHI_WIN_CHANNEL "canary"

#endif  // PHIBROWSER_WIN_CLIENT_PHI_VERSION_H_
