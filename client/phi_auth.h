// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AUTH_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AUTH_H_

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace phi_client {

// The signed-in identity, from the Auth0 id_token's claims (the Mac
// AuthManager's UserInfo shape).
struct PhiAuthUser {
  std::string sub;      // The Auth0 subject — the Mac Account's userID.
  std::string name;
  std::string email;
  std::string picture;  // Avatar URL (unfetched here).
};

// The Windows twin of the Mac AuthManager's core: Auth0 sign-in and the
// session it leaves behind. Scope of this stage: the AUTH MECHANISM ONLY —
// no guest-migration journal, no onboarding phases, no account-scoped local
// stores (those stay bucket-D work on both platforms' terms).
//
// The sign-in is RFC 8252's native-app flow: authorization code + PKCE
// through a LOOPBACK redirect. Phi is a browser, so the authorize URL opens
// in Phi itself; a one-shot listener on 127.0.0.1:<ephemeral> catches the
// code and the token exchange happens over WinHTTP. The refresh token (and
// the cached identity) persist DPAPI-encrypted in PhiAuth.dat — the
// PhiCredentials.dat pattern; access tokens live in memory only.
//
// Threading: SINGLE WRITER. The session (signed_in_, user_, refresh_token_)
// is UI-thread state, read by the coordinator and written only by
// ApplyOutcome on the UI thread. The listener and refresh threads never
// touch it: they run the network round trip, then post an Outcome to
// PhiAuth's own message-only window, whose WndProc applies it, persists,
// and forwards `notify_msg` to the coordinator. What a worker needs to do
// its job (is there a session, which refresh token, the cached access
// token) lives in a lock-protected copy the UI thread keeps in step.
//
// The Auth0 application must list http://127.0.0.1:*/callback among its
// allowed callback URLs (a dashboard setting; the Mac app's entry is its
// bundle scheme).
class PhiAuth {
 public:
  PhiAuth();
  ~PhiAuth();

  PhiAuth(const PhiAuth&) = delete;
  PhiAuth& operator=(const PhiAuth&) = delete;

  // Loads the persisted session (if any) and kicks a BACKGROUND refresh to
  // validate it — an invalid_grant (revoked/expired session) signs out and
  // notifies, the Mac's refreshAuthStatus-at-launch. `notify_hwnd` gets
  // `notify_msg` posted after any state change, always from the UI thread
  // and only after the change has been applied. UI thread.
  void Load(const std::wstring& user_data_dir,
            HWND notify_hwnd,
            UINT notify_msg);

  // Starts the interactive sign-in: builds the PKCE authorize URL, opens it
  // through `open_url` (a Phi tab), and waits on the loopback listener in a
  // worker thread. No-op while one is already in flight. Completion (either
  // way) posts the notify message. UI thread.
  void BeginSignIn(std::function<void(const std::string& url)> open_url);

  // Drops the local session (PhiAuth.dat and memory). Local-only, like the
  // Mac's clearCachedAccount — the remote Auth0 session is not touched. UI
  // thread.
  void SignOut();

  // The bearer token for an account-service call, for a WORKER thread: the
  // cached access token, or a fresh one from a refresh round trip when none
  // is cached — the Mac's getAccessTokenSyncly. Empty when signed out or the
  // refresh fails. Blocks on the network, so never from the UI thread.
  std::string AccessTokenBlocking();
  // Forgets the cached access token so the next AccessTokenBlocking refreshes
  // — a caller's answer to a 401. Any thread.
  void InvalidateAccessToken();
  // The account service this tenant's tokens are for — the Mac APIClient's
  // accountBaseURL: the staging host for the staging Auth0 tenant, the
  // production host otherwise. PHI_ACCOUNT_BASE_URL overrides (no trailing
  // slash).
  static std::string AccountBaseUrl();

  bool signed_in() const { return signed_in_; }
  bool signing_in() const { return signing_in_; }
  const PhiAuthUser& user() const { return user_; }

 private:
  struct SignInJob;
  // A worker's result, applied on the UI thread.
  struct Outcome;

  // Worker side. Exchanges `code`; posts kSignedIn or kSignInFailed.
  void CompleteSignIn(const std::string& code,
                      const std::string& verifier,
                      const std::string& redirect_uri);
  // Worker side, under refresh_lock_: the grant_type=refresh_token round
  // trip. Posts kRefreshed or kSessionDead; nothing on a transport failure.
  // True when the session survives.
  bool RefreshSession();
  // Any thread: hands `outcome` to the UI thread.
  void PostOutcome(std::unique_ptr<Outcome> outcome);
  // UI thread: the ONE writer of the session.
  void ApplyOutcome(const Outcome& outcome);
  void Persist();
  void NotifyChanged();

  static DWORD WINAPI ListenerThread(void* param);
  static DWORD WINAPI RefreshThread(void* param);
  static LRESULT CALLBACK OutcomeWndProc(HWND hwnd,
                                         UINT msg,
                                         WPARAM wparam,
                                         LPARAM lparam);

  std::wstring store_path_;
  HWND notify_hwnd_ = nullptr;
  UINT notify_msg_ = 0;
  // PhiAuth's own message-only window (UI thread), the sole road back from
  // a worker: Outcomes arrive here and ApplyOutcome runs in its WndProc.
  HWND outcome_hwnd_ = nullptr;

  // UI-thread-owned session. Written by ApplyOutcome and SignOut only.
  bool signed_in_ = false;
  bool signing_in_ = false;
  PhiAuthUser user_;
  std::string refresh_token_;
  // Bumped on every sign-in and sign-out so an Outcome computed against an
  // earlier session (a refresh that was in flight when the user signed out)
  // is recognised and dropped rather than applied to the next one.
  uint32_t generation_ = 0;

  // The workers' copy of the session, under token_lock_. The UI thread
  // rewrites it on sign-in/out; a refresh round trip updates the tokens
  // itself (a rotating tenant revokes the old refresh token at once, so the
  // next worker must see the new one before the UI thread gets around to
  // the Outcome). The access token lives ONLY here: memory, never disk.
  CRITICAL_SECTION token_lock_;
  uint32_t worker_generation_ = 0;
  bool worker_signed_in_ = false;
  std::string worker_refresh_token_;
  std::string worker_access_token_;
  // Serializes refresh round trips: a rotating tenant hands out a new
  // refresh token per exchange, so two concurrent refreshes (the launch
  // validation and a worker asking for a bearer token) would invalidate
  // each other's session.
  CRITICAL_SECTION refresh_lock_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AUTH_H_
