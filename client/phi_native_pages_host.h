// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_NATIVE_PAGES_HOST_H_
#define PHIBROWSER_WIN_CLIENT_PHI_NATIVE_PAGES_HOST_H_

#include <windows.h>

#include <memory>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_client {

class PhiBookmarkManager;
class PhiNewTabPage;
class PhiShellWindow;

// THE two client-drawn pages that mount OVER the content card by URL
// (docs/architecture-review.md §3.5, lane 1 cluster M): the bookmark
// manager and the native new-tab page. The Mac mounts both the same way —
// the tab stays an ordinary browser tab and navigating it away restores
// the Chromium view — so the whole component is one predicate per page
// plus the mount/unmount it drives.
//
// INVARIANT: a page is mounted or unmounted ONLY from Sync(), and Sync()
// runs only on the posted kMsgPhiSyncNativePages turn (or on the two
// explicit resumption points — the Space swap landing, and ResetNtp). The
// rebuild that NOTICES the route change calls SyncIfRouteChanged(), which
// posts; it never mounts inline, because the rebuild can run under
// WM_PAINT and a mount creates windows and moves focus.
class PhiNativePagesShell {
 public:
  virtual HWND NativePagesShellWindow() const = 0;
  // The controller both panels are constructed against: they read the
  // shell's layout, palette and omnibox session through it. Kept as the
  // owner pointer rather than a fourth interface because the panels are
  // the next things to move onto ShellServices, not this component.
  virtual PhiShellWindow* NativePagesOwner() = 0;
  // Off-the-record: nothing persists, so there is no bookmark manager.
  virtual bool NativePagesIncognito() const = 0;
  // A hidden agent window keeps Chromium's web NTP (the agent drives it
  // over CDP and no one looks at a native one there).
  virtual bool NativePagesIsAgent() const = 0;
  virtual bool NativePagesContentFullscreen() const = 0;
  // The active browser has no tabs at all: the NTP's omnibox is the way
  // back in.
  virtual bool NativePagesPlaceholding() const = 0;
  // A Space switch is mid-slide: the page arrives (or leaves) with the
  // content, when the slide's completion re-runs Sync().
  virtual bool NativePagesSwapPending() const = 0;
  // The ACTIVE tab's URL, which is what selects the route.
  virtual const std::string& NativePagesActiveUrl() const = 0;
  virtual void NativePagesRaiseCaps() = 0;
  // Never leave the keyboard on a hidden field.
  virtual void NativePagesFocusContent() = 0;
  virtual HWND NativePagesContentWindow() const = 0;
  // True while the commit that blanked the NTP's controls is still the
  // ACTIVE tab's business: the controls come back once another tab is
  // active, or that load has ended.
  virtual bool NativePagesCommitPending() const = 0;

 protected:
  // Non-virtual and protected: the shell OWNS the host, never the other way
  // round, so nothing ever deletes through this pointer.
  ~PhiNativePagesShell() = default;
};

class PhiNativePagesHost {
 public:
  explicit PhiNativePagesHost(PhiNativePagesShell* shell);
  ~PhiNativePagesHost();

  PhiNativePagesHost(const PhiNativePagesHost&) = delete;
  PhiNativePagesHost& operator=(const PhiNativePagesHost&) = delete;

  // THE mount point: brings both pages into line with the current route.
  // Runs on the posted turn.
  void Sync();
  // The new-tab page alone: the Space-switch slide defers its mount past
  // the reveal, exactly as the content itself is deferred.
  void SyncNtp();
  // A rebuild noticed the route may have moved. Refreshes a mounted NTP in
  // place (a palette ramp repaints it with the chrome) and posts
  // kMsgPhiSyncNativePages when the mount state actually has to change.
  void SyncIfRouteChanged();
  // The content card moved or resized.
  void Reposition();
  // A palette change (the create-Space form's live preview, a theme ramp).
  void Refresh();
  // A Space URL rule re-homed a navigation that began on the new tab: the
  // page returns to its clean state (the Mac's resetToInitialState).
  void ResetNtp();
  // Both pages go before the shell window they sit over.
  void Shutdown();

  // Whether either page is currently mounted over the content card.
  bool bookmark_page_mounted() const;
  bool ntp_mounted() const;
  // The NTP's own window and its omnibox EDIT — the shell's focus routing
  // and the omnibox session's field adoption read these. Null when the
  // page has never been created.
  HWND ntp_hwnd() const;
  HWND ntp_field() const;
  // The page's search box in screen coordinates, for the omnibox session's
  // suggestion dropdown. False when the page has never been created.
  bool NtpBoxScreenRect(RECT* out) const;
  // The page is up and its field is where the keyboard belongs (the shell
  // forwards activation and focus to it rather than to the browser child).
  bool ntp_wants_focus() const;
  void FocusNtpField();
  // omniBoxDidClear: the page blanks its controls so nothing of the new-tab
  // page lingers over the arriving one; the rebuild that sees the new URL
  // unmounts it. No-op unless the page is mounted; true when it blanked.
  bool HideNtpControls();

 private:
  // The two route predicates. Pure functions of the shell's state.
  bool WantsBookmarkPage() const;
  bool WantsNtp() const;
  void SyncBookmarkPage();
  // Focuses the page's omnibox unless the user is busy in another control.
  void FocusNtpIfIdle();

  // Owns this host and outlives it.
  RAW_PTR_EXCLUSION PhiNativePagesShell* const shell_;
  std::unique_ptr<PhiBookmarkManager> bookmark_manager_;
  std::unique_ptr<PhiNewTabPage> new_tab_page_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_NATIVE_PAGES_HOST_H_
