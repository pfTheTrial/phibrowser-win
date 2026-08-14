// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TOAST_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TOAST_H_

#include <windows.h>

#include <string>

#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

// The Mac OverlayToastCenter: a transient top-center text toast over
// `owner`. One toast at a time — a new one replaces the current (the Mac
// queues; replacement is the simpler v1). Click-through, non-activating,
// auto-dismisses after `duration_ms` with a short fade.
void PhiShowToast(HWND owner,
                  const PhiPalette& palette,
                  const std::wstring& title,
                  const std::wstring& message = std::wstring(),
                  int duration_ms = 3000);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TOAST_H_
