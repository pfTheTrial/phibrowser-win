// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The agentSpace.account.* verbs (docs/login-port.md): the harness's view
// of the Auth0 session and the three things Settings ▸ You can do to it.
// Test rig only — the session is the user's, never an agent's to drive.

#include <optional>
#include <string>

#include "base/values.h"
#include "phibrowser-win/client/phi_account_feedback.h"
#include "phibrowser-win/client/phi_account_profile.h"
#include "phibrowser-win/client/phi_auth.h"
#include "phibrowser-win/client/phi_control_dispatcher.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_window_registry.h"

namespace phi_client {

PhiVerbResult PhiControlDispatcher::VerbAccountStatus(
    const PhiVerbContext& ctx) {
  const PhiAccountAndFeedback& account = coordinator_->account();
  const PhiAuth& auth = account.auth();
  base::DictValue out;
  out.Set("signedIn", auth.signed_in());
  out.Set("signingIn", auth.signing_in());
  out.Set("reauthRequired", account.AccountReauthRequired());
  out.Set("busy", account.AccountBusy());
  out.Set("lastError", account.AccountLastError());
  out.Set("tenantOrigin", auth.TenantOrigin());
  if (auth.signed_in()) {
    base::DictValue user;
    user.Set("sub", auth.user().sub);
    user.Set("name", auth.user().name);
    user.Set("email", auth.user().email);
    user.Set("picture", auth.user().picture);
    out.Set("user", std::move(user));
    std::string name;
    std::string email;
    account.AccountInfo(&name, &email);
    out.Set("displayName", name);
    out.Set("displayEmail", email);
  }
  const std::optional<PhiAccountProfile>& profile = account.Profile().profile();
  if (profile) {
    base::DictValue row;
    row.Set("id", profile->id);
    row.Set("auth0Id", profile->auth0_id);
    row.Set("name", profile->name);
    row.Set("email", profile->email);
    row.Set("picture", profile->picture);
    out.Set("profile", std::move(row));
  }
  out.Set("hasAvatar", account.Profile().has_avatar());
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbAccountSignIn(
    const PhiVerbContext& ctx) {
  // The hosted login rides in the first user window; without one there is
  // nowhere to open it.
  if (!coordinator_->registry().FirstUserSlot().valid()) {
    return PhiVerbResult::Fail(PhiErrorCode::kWindowNotOpen);
  }
  coordinator_->account().RequestSignIn();
  base::DictValue out;
  out.Set("signingIn", coordinator_->auth().signing_in());
  out.Set("tenantOrigin", coordinator_->auth().TenantOrigin());
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbAccountCancelSignIn(
    const PhiVerbContext& ctx) {
  coordinator_->account().RequestCancelSignIn();
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbAccountSignOut(
    const PhiVerbContext& ctx) {
  coordinator_->account().RequestSignOut();
  base::DictValue out;
  out.Set("signedIn", coordinator_->auth().signed_in());
  return PhiVerbResult::Ok(std::move(out));
}

}  // namespace phi_client
