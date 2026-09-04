// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_PROMPT_H_
#define PHIBROWSER_WIN_CLIENT_PHI_PROMPT_H_

#include <windows.h>

#include <string>
#include <vector>

namespace phi_client {

// The modal dialogs, all drawn in the Mac's PhiAlert layout (465 wide, the
// Phi mark, an 18pt title, 14pt copy, 130x36 actions with the primary
// rightmost) on the shared popup chrome — a caption band with the Windows
// close button in the right corner, no OS title bar. Each is modal against
// its owner, as NSAlert.runModal is, and returns once dismissed.

// One action button. Actions are listed in display order; `leading` pins
// one to the left edge (the Mac's tertiary "Skip This Version" slot), the
// rest group at the right with the LAST one rightmost.
enum class PhiAlertRole { kPrimary, kSecondary, kDestructive };
struct PhiAlertAction {
  std::wstring label;
  PhiAlertRole role = PhiAlertRole::kSecondary;
  bool leading = false;
};

// A PhiAlert: `title` over `message` (may hold newlines; wraps) and the
// actions. Returns the index of the pressed action; -1 when the alert was
// closed or escaped (`cancel_index` names an action Escape maps to instead).
// Return presses `default_index` (-1 = none, the Mac's irreversible-action
// mode). `owner` null centres on the work area.
int PhiAlert(HWND owner,
             const wchar_t* title,
             const wchar_t* message,
             const std::vector<PhiAlertAction>& actions,
             int default_index = -1,
             int cancel_index = -1);

// A one-button notice — the MessageBox(MB_OK) stand-in.
void PhiNotice(HWND owner, const wchar_t* title, const wchar_t* message);

// A modal one-line text prompt — the Mac's NSAlert with an accessory text
// field (rename / create dialogs). `value` carries the initial text in and
// the entered text out. False on Cancel or empty input. `confirm_label`
// null reads "OK".
bool PhiPromptText(HWND owner,
                   const wchar_t* title,
                   const wchar_t* message,
                   std::wstring* value,
                   const wchar_t* confirm_label = nullptr);

// A modal confirmation. The confirming button (`confirm_label`, null =
// "OK") is destructive red by default — "Delete …?" — or the accent when
// `destructive` is false. True when the user confirms.
bool PhiConfirm(HWND owner,
                const wchar_t* title,
                const wchar_t* message,
                const wchar_t* confirm_label = nullptr,
                bool destructive = true);

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
// a list with `default_index` preselected; the confirming button (or a
// double-click) returns the selected index, the cancelling button / close
// returns -1. The labels default to "Open" / "Keep Here".
int PhiPromptChoose(HWND owner,
                    const wchar_t* title,
                    const wchar_t* message,
                    const std::vector<std::wstring>& options,
                    int default_index,
                    const wchar_t* confirm_label = nullptr,
                    const wchar_t* cancel_label = nullptr);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_PROMPT_H_
