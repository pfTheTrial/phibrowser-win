// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_KEYBOARD_ACCELERATORS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_KEYBOARD_ACCELERATORS_H_

#include <windows.h>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_client {

class PhiPreferences;

// The client's menu-bar accelerators (docs/architecture-review.md §3.4,
// lane 1 §1.2 cluster 17). Keys land in the focused window — the browser
// child while browsing — so a plain WM_KEYDOWN handler on a shell never
// sees them; a thread-scoped WH_KEYBOARD hook on the UI thread does,
// before dispatch.
//
// It owns the hook, the FIXED accelerator table, the Alt-solo state and
// nothing else: the rebindable chords come from PhiPreferences (which owns
// the user's overrides) and every match is posted to the focused shell
// through the Host. No business state, no framework, no windows of its own.
class PhiKeyboardAccelerators {
 public:
  // What the hook needs from the app: which shell has focus, and the one
  // key it decides for something else (Esc over a Peek card).
  class Host {
   public:
    virtual ~Host() = default;
    // The shell window owning the focus HWND, or null (focus is elsewhere).
    virtual HWND FocusedShellHwnd() const = 0;
    // Esc with no modifiers over `shell`: dismiss the Peek card(s) floating
    // over it. True when one was dismissed — then the key is swallowed so
    // the page under the card does not also see it.
    virtual bool DismissPeeksForShell(HWND shell) = 0;
  };

  PhiKeyboardAccelerators(const PhiPreferences& preferences, Host& host);
  ~PhiKeyboardAccelerators();

  PhiKeyboardAccelerators(const PhiKeyboardAccelerators&) = delete;
  PhiKeyboardAccelerators& operator=(const PhiKeyboardAccelerators&) = delete;

  // Installs the WH_KEYBOARD hook on the CALLING thread (the UI thread).
  // False when Windows refused; the client then simply has no accelerators.
  // Idempotent: a second call is a no-op.
  bool Install();

  // One entry of the FIXED table — the shortcuts reserved the way Chrome
  // reserves its browser shortcuts (they fire even while a page has focus).
  struct Accelerator {
    UINT vk;
    bool ctrl;
    bool shift;
    bool alt;
    UINT command;
  };
  // The fixed table's command for a chord, or 0 when it binds none. Pure.
  static UINT MatchFixedAccelerator(UINT vk, bool ctrl, bool shift, bool alt);
  // What a key-down does to the Alt-solo state: VK_MENU arms it (a key
  // REPEAT is still a solo hold and leaves it as it was); any other
  // key-down ends it. Pure.
  static bool AltSoloAfterKeyDown(bool current, UINT vk, bool repeat);

 private:
  static LRESULT CALLBACK HookProc(int code, WPARAM wparam, LPARAM lparam);
  // The hook body proper. Returns 1 to swallow the key, 0 to pass it on.
  LRESULT OnKey(WPARAM wparam, LPARAM lparam);

  // The user's chord overrides (PhiPreferences::MatchRebindableChord) — the
  // service reads them live, so a rebind takes effect on the next key.
  RAW_PTR_EXCLUSION const PhiPreferences& preferences_;
  RAW_PTR_EXCLUSION Host& host_;
  HHOOK hook_ = nullptr;
  // Alt is down with nothing pressed since — a bare tap summons the caption
  // band's hidden menu titles on release. Cleared by any other key-down, so
  // Alt+Tab and the Alt+arrow history chords never trip it.
  bool alt_solo_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_KEYBOARD_ACCELERATORS_H_
