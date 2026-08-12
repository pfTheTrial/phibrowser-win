// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_PROMPT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_PROMPT_H_

#include <windows.h>

#include <string>
#include <vector>

namespace phi_client {

// A modal one-line text prompt — the Win32 stand-in for the Mac's NSAlert
// with an accessory text field (rename / create dialogs). `value` carries the
// initial text in and the entered text out. False on Cancel or empty input.
bool PhiPromptText(HWND owner,
                   const wchar_t* title,
                   const wchar_t* message,
                   std::wstring* value);

// A modal confirmation with a destructive primary button ("Delete …?").
// True when the user confirms.
bool PhiConfirm(HWND owner, const wchar_t* title, const wchar_t* message);

// A modal list chooser — the Win32 stand-in for the Mac's Space chooser
// shown when a navigation matches an "ask first" URL rule. `options` fills
// a list box with `default_index` preselected; Open (or a double-click)
// returns the selected index, "Keep Here" / close returns -1.
int PhiPromptChoose(HWND owner,
                    const wchar_t* title,
                    const wchar_t* message,
                    const std::vector<std::wstring>& options,
                    int default_index);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_PROMPT_H_
