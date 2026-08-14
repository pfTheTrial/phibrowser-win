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

// One keyboard chord — a virtual key plus its modifiers. vk 0 means
// "unbound" (a rebindable shortcut whose chord was stolen by another).
struct PhiShortcutChord {
  UINT vk = 0;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
  bool operator==(const PhiShortcutChord&) const = default;
};

// "Ctrl+Alt+Shift+Key" display form, matching the menu bar's labels;
// an unbound chord renders as an em dash.
std::wstring PhiFormatChord(const PhiShortcutChord& chord);

// A modal chord recorder — the Mac Shortcuts pane's inline capture. Shows
// `message` over a live preview of the held modifiers and waits for one
// complete chord (Ctrl or Alt required, except for function keys). Escape
// or Cancel returns false; a captured chord lands in `out`.
bool PhiPromptShortcut(HWND owner,
                       const wchar_t* title,
                       const wchar_t* message,
                       PhiShortcutChord* out);

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
