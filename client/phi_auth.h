// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AUTH_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AUTH_H_

#include <windows.h>

#include <functional>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"

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
  // `notify_msg` posted after any state change from a worker thread.
  void Load(const std::wstring& user_data_dir,
            HWND notify_hwnd,
            UINT notify_msg);

  // Starts the interactive sign-in: builds the PKCE authorize URL, opens it
  // through `open_url` (a Phi tab), and waits on the loopback listener in a
  // worker thread. No-op while one is already in flight. Completion (either
  // way) posts the notify message.
  void BeginSignIn(std::function<void(const std::string& url)> open_url);

  // Drops the local session (PhiAuth.dat and memory). Local-only, like the
  // Mac's clearCachedAccount — the remote Auth0 session is not touched.
  void SignOut();

  bool signed_in() const { return signed_in_; }
  bool signing_in() const { return signing_in_; }
  const PhiAuthUser& user() const { return user_; }

 private:
  // Exchanges `code` and commits the session; runs on the listener thread.
  void CompleteSignIn(const std::string& code,
                      const std::string& verifier,
                      const std::string& redirect_uri);
  // grant_type=refresh_token round trip; true when the session survives.
  bool RefreshSession();
  void Persist();
  void NotifyChanged();

  static DWORD WINAPI ListenerThread(void* param);
  static DWORD WINAPI RefreshThread(void* param);

  std::wstring store_path_;
  HWND notify_hwnd_ = nullptr;
  UINT notify_msg_ = 0;

  // UI-thread-owned state; worker threads hand results back through the
  // message window before it changes.
  bool signed_in_ = false;
  volatile bool signing_in_ = false;
  PhiAuthUser user_;
  std::string refresh_token_;
  std::string access_token_;  // Memory only; empty until a refresh/exchange.

  struct SignInJob;
  // Owned by the listener thread once started; excluded from raw_ptr as a
  // cross-thread handoff cleared by that thread.
  RAW_PTR_EXCLUSION SignInJob* job_ = nullptr;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AUTH_H_
