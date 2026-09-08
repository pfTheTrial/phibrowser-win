// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_FEEDBACK_H_
#define PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_FEEDBACK_H_

#include <windows.h>

#include <map>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_auth.h"
#include "phibrowser-win/client/phi_feedback_window.h"

namespace phi_client {

// The account and the feedback pipeline (docs/architecture-review.md §3.4,
// lane 1 §1.2 cluster 15): the Auth0 session (`PhiAuth`, whose ONE writer
// is the UI thread — workers post their results back), the feedback outbox
// the session names the account of, and the Send Feedback window's host.
//
// It owns the session and the harness stand-in account and nothing else.
// Everything it needs from the app — a tab to open the hosted login in,
// raising a window, telling the Settings panes the account moved — comes
// through Host; the framework calls (system logs, the extension list) go
// straight to the framework it was initialised with.
class PhiAccountAndFeedback : public PhiFeedbackHost {
 public:
  class Host {
   public:
    virtual ~Host() = default;
    // Opens `url` as a foreground tab in the FIRST user window (the hosted
    // login — Phi IS the browser RFC 8252 wants the flow to ride in). A no-op
    // when no user window is up.
    virtual void OpenUrlInFirstUserSlot(const std::string& url) = 0;
    // Raises the shell showing `browser_window_id`.
    virtual void FocusWindowForBrowser(int64_t browser_window_id) = 0;
    // The session state moved: the Account pane re-reads it.
    virtual void NoteAccountChanged() = 0;
  };

  explicit PhiAccountAndFeedback(Host& host);
  ~PhiAccountAndFeedback() override;

  PhiAccountAndFeedback(const PhiAccountAndFeedback&) = delete;
  PhiAccountAndFeedback& operator=(const PhiAccountAndFeedback&) = delete;

  // Restores the persisted session (validated in the background; `notify_hwnd`
  // gets `notify_msg` posted on every settled state change) and starts the
  // outbox, whose jobs earlier runs queued resume uploading once the account
  // is known. UI thread, from OnFrameworkReady.
  void Init(phi_framework::PhiFramework* framework,
            const std::wstring& user_data_dir,
            HWND notify_hwnd,
            UINT notify_msg);
  // The notify message arrived: the Account pane re-reads state and the
  // outbox learns whose jobs to work on.
  void OnAuthChanged();
  // Everything that must reach disk before the session ends. Idempotent.
  void FlushForShutdown();

  // --- The account ---------------------------------------------------------
  // The signed-in user's name / email, or false when signed out. Either
  // out-parameter may be null.
  bool AccountInfo(std::string* name, std::string* email) const;
  bool AccountSigningIn() const;
  void RequestSignIn();
  void RequestSignOut();
  // The Auth0 session, for the feedback harness verb's signedIn report.
  const PhiAuth& auth() const { return auth_; }
  // Harness only (agentSpace.feedback.setAccount): a stand-in account for a
  // box with no Auth0 session; empty clears. Ignored while a real session
  // is signed in.
  void SetTestFeedbackAccount(const std::string& user_id,
                              const std::string& email);

  // --- Send Feedback -------------------------------------------------------
  // Opens (or rebinds) the app-global feedback window over `owner`.
  void OpenFeedback(HWND owner, const PhiFeedbackContext& context);
  // The framework answered a FetchFeedbackSystemLogs (or it timed out).
  void OnFeedbackSystemLogs(int request_id, const char* text);

  // PhiFeedbackHost:
  bool FeedbackAccount(std::string* email) override;
  void RequestFeedbackSignIn() override;
  void OpenFeedbackLink(int64_t window_id, const std::string& url) override;
  bool FetchFeedbackSystemLogs(int64_t window_id, int request_id) override;
  std::map<std::string, std::string> PhiExtensionVersions(
      int64_t window_id) override;

 private:
  // Hands the signed-in account to the outbox (whose jobs are filed and
  // uploaded per account) after every auth change.
  void SyncFeedbackAccount();

  RAW_PTR_EXCLUSION Host& host_;
  // The framework command interface (a singleton in chrome.dll); non-owning,
  // across a module boundary, so not a raw_ptr. Null until Init.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  // The Auth0 session (bucket D auth, stage 1).
  PhiAuth auth_;
  // A harness-set stand-in account (agentSpace.feedback.setAccount) for a
  // box with no Auth0 session: reports file and prepare under it, and the
  // upload then fails for want of a token — which is the testable part.
  // Ignored while a real session is signed in.
  std::string test_feedback_user_id_;
  std::string test_feedback_email_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_ACCOUNT_FEEDBACK_H_
