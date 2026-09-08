// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_native_pages_host.h"

#include "phibrowser-win/client/phi_bookmark_manager.h"
#include "phibrowser-win/client/phi_new_tab_page.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

PhiNativePagesHost::PhiNativePagesHost(PhiNativePagesShell* shell)
    : shell_(shell) {}

PhiNativePagesHost::~PhiNativePagesHost() = default;

void PhiNativePagesHost::Shutdown() {
  bookmark_manager_.reset();
  new_tab_page_.reset();
}

bool PhiNativePagesHost::bookmark_page_mounted() const {
  return bookmark_manager_ && bookmark_manager_->visible();
}

bool PhiNativePagesHost::ntp_mounted() const {
  return new_tab_page_ && new_tab_page_->visible();
}

HWND PhiNativePagesHost::ntp_hwnd() const {
  return new_tab_page_ ? new_tab_page_->hwnd() : nullptr;
}

HWND PhiNativePagesHost::ntp_field() const {
  return new_tab_page_ ? new_tab_page_->field() : nullptr;
}

bool PhiNativePagesHost::NtpBoxScreenRect(RECT* out) const {
  if (!new_tab_page_ || !out) {
    return false;
  }
  *out = new_tab_page_->BoxScreenRect();
  return true;
}

bool PhiNativePagesHost::ntp_wants_focus() const {
  return new_tab_page_ && new_tab_page_->WantsFocus();
}

void PhiNativePagesHost::FocusNtpField() {
  if (new_tab_page_) {
    new_tab_page_->FocusField();
  }
}

bool PhiNativePagesHost::HideNtpControls() {
  if (!ntp_mounted()) {
    return false;
  }
  new_tab_page_->HideControls();
  return true;
}

// The bookmark route: the Mac's URL-driven mount, refused off the record
// where there is nothing stored to manage.
bool PhiNativePagesHost::WantsBookmarkPage() const {
  return !shell_->NativePagesIncognito() &&
         PhiBookmarkManager::IsManagerUrl(shell_->NativePagesActiveUrl());
}

// The new-tab route: the ACTIVE tab sits on a new-tab URL — or the browser
// has no tabs at all (placeholder mode), where the page's omnibox is the
// way back in.
bool PhiNativePagesHost::WantsNtp() const {
  if (shell_->NativePagesIsAgent() || shell_->NativePagesContentFullscreen()) {
    return false;
  }
  if (shell_->NativePagesPlaceholding()) {
    return true;
  }
  return PhiNewTabPage::IsNewTabUrl(shell_->NativePagesActiveUrl());
}

void PhiNativePagesHost::SyncBookmarkPage() {
  if (!WantsBookmarkPage()) {
    if (bookmark_manager_) {
      bookmark_manager_->Hide();
    }
    return;
  }
  if (!bookmark_manager_) {
    bookmark_manager_ =
        std::make_unique<PhiBookmarkManager>(shell_->NativePagesOwner());
  }
  if (bookmark_manager_->visible()) {
    bookmark_manager_->Reposition();
    bookmark_manager_->Refresh();
  } else {
    bookmark_manager_->Show();
  }
}

void PhiNativePagesHost::SyncNtp() {
  if (shell_->NativePagesSwapPending()) {
    // A Space switch is mid-slide: the page arrives (or leaves) with the
    // content, when the slide's completion re-runs this — the Mac reveals
    // the target Space's content, native page included, on completion.
    return;
  }
  if (!WantsNtp()) {
    if (ntp_mounted()) {
      const HWND focus = ::GetFocus();
      const bool had_focus = focus && (focus == new_tab_page_->hwnd() ||
                                       focus == new_tab_page_->field());
      new_tab_page_->Hide();
      if (had_focus) {
        // Never leave the keyboard on a hidden field.
        shell_->NativePagesFocusContent();
      }
    }
    return;
  }
  if (!new_tab_page_) {
    new_tab_page_ =
        std::make_unique<PhiNewTabPage>(shell_->NativePagesOwner());
  }
  if (new_tab_page_->visible()) {
    // Controls blanked by a commit come back once that navigation is no
    // longer the active tab's business: another tab is active, or the load
    // ended and this tab is still a new tab (a new-tab URL typed in, a
    // navigation that never left).
    if (new_tab_page_->controls_hidden() &&
        !shell_->NativePagesCommitPending()) {
      new_tab_page_->Reset();
      FocusNtpIfIdle();
    } else {
      new_tab_page_->Refresh();  // A palette ramp repaints it with the chrome.
    }
    return;
  }
  new_tab_page_->Show();
  if (const HWND shell = shell_->NativePagesShellWindow()) {
    // The corner caps go back on top of the page, as they ride the browser.
    shell_->NativePagesRaiseCaps();
    // viewDidAppear focuses the omnibox — in the window the user is in; a
    // background window mounting its page must not pull the focus over.
    if (::GetForegroundWindow() == shell) {
      new_tab_page_->FocusField();
    }
  }
}

void PhiNativePagesHost::Sync() {
  SyncBookmarkPage();
  SyncNtp();
}

void PhiNativePagesHost::SyncIfRouteChanged() {
  const HWND shell = shell_->NativePagesShellWindow();
  if (!shell) {
    return;
  }
  const bool mounted = bookmark_page_mounted();
  const bool ntp = ntp_mounted();
  // A page blanked by a commit whose navigation is over (or belongs to
  // another tab now) needs the sync too, to bring its controls back.
  const bool ntp_stale = ntp && new_tab_page_->controls_hidden() &&
                         !shell_->NativePagesCommitPending();
  if (ntp) {
    new_tab_page_->Refresh();  // Palette ramps repaint it with the chrome.
  }
  if (mounted != WantsBookmarkPage() || ntp != WantsNtp() || ntp_stale) {
    ::PostMessageW(shell, kMsgPhiSyncNativePages, 0, 0);
  }
}

void PhiNativePagesHost::Reposition() {
  if (bookmark_page_mounted()) {
    bookmark_manager_->Reposition();
  }
  if (ntp_mounted()) {
    new_tab_page_->Reposition();
  }
}

void PhiNativePagesHost::Refresh() {
  if (ntp_mounted()) {
    new_tab_page_->Refresh();
  }
}

void PhiNativePagesHost::FocusNtpIfIdle() {
  const HWND shell = shell_->NativePagesShellWindow();
  if (!new_tab_page_ || !shell || ::GetForegroundWindow() != shell) {
    return;
  }
  const HWND focus = ::GetFocus();
  if (focus && focus != shell &&
      focus != shell_->NativePagesContentWindow() &&
      focus != new_tab_page_->hwnd() && focus != new_tab_page_->field()) {
    return;  // The user is in another control (the address field, a panel).
  }
  new_tab_page_->FocusField();
}

void PhiNativePagesHost::ResetNtp() {
  if (ntp_mounted()) {
    new_tab_page_->Reset();
    FocusNtpIfIdle();
    return;
  }
  SyncNtp();
}

}  // namespace phi_client
