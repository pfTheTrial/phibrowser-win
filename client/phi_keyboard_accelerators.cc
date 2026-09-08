// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_keyboard_accelerators.h"

#include "phibrowser-win/client/phi_menu_bar.h"
#include "phibrowser-win/client/phi_preferences.h"
#include "phibrowser-win/client/phi_tab_switcher.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {
// The one accelerator service, reachable from the static hook. The client
// creates exactly one for the process lifetime.
PhiKeyboardAccelerators* g_accelerators = nullptr;

// The menu bar's FIXED shortcuts, reserved the way Chrome reserves its
// browser shortcuts (they fire even while a page has focus). Edit keys
// (Ctrl+C/V/X/Z/A/F...) stay with the web content; everything a user can
// rebind lives in the preferences' rebindable table, not here.
constexpr PhiKeyboardAccelerators::Accelerator kAccelerators[] = {
    {'Q', true, true, false, kCmdQuit},
    {'O', true, false, false, kCmdOpenFile},
    {'P', true, false, false, kCmdPrint},
    {VK_F5, false, false, false, kCmdReload},
    {'U', true, false, false, kCmdViewSource},
    {'I', true, true, false, kCmdDevTools},
    {'J', true, true, false, kCmdJsConsole},
    // F11 deliberately unbound — see the menu bar's IDC_FULLSCREEN note.
    {'0', true, false, false, kCmdActualSize},
    {VK_OEM_PLUS, true, false, false, kCmdZoomIn},
    {VK_ADD, true, false, false, kCmdZoomIn},
    {VK_OEM_MINUS, true, false, false, kCmdZoomOut},
    {VK_SUBTRACT, true, false, false, kCmdZoomOut},
    {VK_OEM_COMMA, true, false, false, kCmdPreferences},
    // Ctrl+Tab / Ctrl+Shift+Tab are handled in OnKey — the MRU switcher.
    // Alt+Left/Right/Home — the history chords Chrome reserves.
    {VK_LEFT, false, false, true, kCmdBack},
    {VK_RIGHT, false, false, true, kCmdForward},
    {VK_HOME, false, false, true, kCmdHome},
    // Ctrl+1..9 switches Spaces (the Mac's ⌃1..⌃9; the Spaces menu and
    // the pips' hover cards advertise these).
    {'1', true, false, false, kCmdSpace1},
    {'2', true, false, false, kCmdSpace2},
    {'3', true, false, false, kCmdSpace3},
    {'4', true, false, false, kCmdSpace4},
    {'5', true, false, false, kCmdSpace5},
    {'6', true, false, false, kCmdSpace6},
    {'7', true, false, false, kCmdSpace7},
    {'8', true, false, false, kCmdSpace8},
    {'9', true, false, false, kCmdSpace9},
};
}  // namespace

PhiKeyboardAccelerators::PhiKeyboardAccelerators(
    const PhiPreferences& preferences,
    Host& host)
    : preferences_(preferences), host_(host) {
  g_accelerators = this;
}

PhiKeyboardAccelerators::~PhiKeyboardAccelerators() {
  if (hook_) {
    ::UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
  }
  g_accelerators = nullptr;
}

bool PhiKeyboardAccelerators::Install() {
  if (hook_) {
    return true;
  }
  hook_ = ::SetWindowsHookExW(WH_KEYBOARD, &PhiKeyboardAccelerators::HookProc,
                              nullptr, ::GetCurrentThreadId());
  return hook_ != nullptr;
}

// static
UINT PhiKeyboardAccelerators::MatchFixedAccelerator(UINT vk,
                                                    bool ctrl,
                                                    bool shift,
                                                    bool alt) {
  for (const Accelerator& accel : kAccelerators) {
    if (accel.vk == vk && accel.ctrl == ctrl && accel.shift == shift &&
        accel.alt == alt) {
      return accel.command;
    }
  }
  return 0;
}

// static
bool PhiKeyboardAccelerators::AltSoloAfterKeyDown(bool current,
                                                  UINT vk,
                                                  bool repeat) {
  if (vk == VK_MENU) {
    // A repeat (bit 30) is still a solo hold.
    return repeat ? current : true;
  }
  return false;
}

// static
LRESULT CALLBACK PhiKeyboardAccelerators::HookProc(int code,
                                                   WPARAM wparam,
                                                   LPARAM lparam) {
  PhiKeyboardAccelerators* self = g_accelerators;
  if (code != HC_ACTION || !self) {
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
  }
  if (self->OnKey(wparam, lparam)) {
    return 1;
  }
  return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT PhiKeyboardAccelerators::OnKey(WPARAM wparam, LPARAM lparam) {
  if (lparam & 0x80000000) {  // Key-up.
    // Releasing Ctrl commits an in-flight MRU switch (the Mac's ⌃⇥ HUD).
    if (wparam == VK_CONTROL && PhiTabSwitcher::IsActive()) {
      if (HWND shell = host_.FocusedShellHwnd()) {
        ::PostMessageW(shell, kMsgPhiSwitcherCommit, 0, 0);
      } else {
        PhiTabSwitcher::Cancel();
      }
    }
    // Alt pressed and released with nothing in between summons the caption
    // band's hidden menu titles — the keyboard's way to the bar the pointer
    // reveals by hovering. Any chord (Alt+Tab, Alt+Left, Alt+F4, a mnemonic)
    // cleared alt_solo_ on its key-down, so only a bare tap gets here. The
    // key is NOT swallowed: Chromium still sees the release it expects.
    if (wparam == VK_MENU && alt_solo_) {
      alt_solo_ = false;
      if (HWND shell = host_.FocusedShellHwnd()) {
        ::PostMessageW(shell, kMsgPhiToggleMenuReveal, 0, 0);
      }
    }
    return 0;
  }
  // Track whether Alt is being held alone, for the reveal gesture above.
  alt_solo_ = AltSoloAfterKeyDown(alt_solo_, static_cast<UINT>(wparam),
                                  (lparam & 0x40000000) != 0);
  const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
  const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
  const bool alt = (lparam & 0x20000000) != 0;  // The context code bit.
  // Esc dismisses the Peek card floating over the focused shell (the Mac's
  // window-level Escape), swallowed so the page under it does not also see
  // it. With no card open the key goes to the page as usual.
  if (wparam == VK_ESCAPE && !ctrl && !shift && !alt) {
    if (HWND shell = host_.FocusedShellHwnd()) {
      if (host_.DismissPeeksForShell(shell)) {
        return 1;
      }
    }
  }
  if (wparam == VK_TAB && ctrl && !alt) {
    // Ctrl+Tab drives the MRU switcher HUD, not a plain next-tab hop.
    if (HWND shell = host_.FocusedShellHwnd()) {
      ::PostMessageW(shell, shift ? kMsgPhiSwitcherPrev : kMsgPhiSwitcherNext,
                     0, 0);
      return 1;
    }
  }
  // The REBINDABLE shortcuts first (their effective chords carry the user's
  // overrides), so a custom chord shadows any fixed one it collides with.
  if (const UINT command = preferences_.MatchRebindableChord(
          static_cast<UINT>(wparam), ctrl, shift, alt)) {
    if (HWND shell = host_.FocusedShellHwnd()) {
      ::PostMessageW(shell, WM_COMMAND, MAKEWPARAM(command, 0), 0);
      return 1;
    }
  }
  if (const UINT command =
          MatchFixedAccelerator(static_cast<UINT>(wparam), ctrl, shift, alt)) {
    if (HWND shell = host_.FocusedShellHwnd()) {
      ::PostMessageW(shell, WM_COMMAND, MAKEWPARAM(command, 0), 0);
      return 1;  // Swallow: Chromium must not double-handle it.
    }
  }
  return 0;
}

}  // namespace phi_client
