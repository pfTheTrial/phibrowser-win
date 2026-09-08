// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_account_feedback.h"

#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_feedback_outbox.h"
#include "phibrowser-win/client/phi_ids.h"

namespace phi_client {

PhiAccountAndFeedback::PhiAccountAndFeedback(Host& host) : host_(host) {}

PhiAccountAndFeedback::~PhiAccountAndFeedback() = default;

void PhiAccountAndFeedback::Init(phi_framework::PhiFramework* framework,
                                 const std::wstring& user_data_dir,
                                 HWND notify_hwnd,
                                 UINT notify_msg) {
  framework_ = framework;
  // Restore the Auth0 session (validated in the background; the message
  // refreshes the settings pane whenever the state settles).
  auth_.Load(user_data_dir, notify_hwnd, notify_msg);
  // The feedback outbox: reports queued by earlier runs resume uploading
  // once the account is known (the Mac's FeedbackOutboxUploader.start).
  PhiFeedbackOutbox::Get().Start(user_data_dir, &auth_);
  SyncFeedbackAccount();
}

void PhiAccountAndFeedback::OnAuthChanged() {
  host_.NoteAccountChanged();
  SyncFeedbackAccount();
}

void PhiAccountAndFeedback::FlushForShutdown() {
  // Best-effort and idempotent: a second call finds no thread to join.
  PhiFeedbackOutbox::Get().Stop();
}

bool PhiAccountAndFeedback::AccountInfo(std::string* name,
                                        std::string* email) const {
  if (!auth_.signed_in()) {
    return false;
  }
  if (name) {
    *name = auth_.user().name;
  }
  if (email) {
    *email = auth_.user().email;
  }
  return true;
}

bool PhiAccountAndFeedback::AccountSigningIn() const {
  return auth_.signing_in();
}

void PhiAccountAndFeedback::RequestSignIn() {
  auth_.BeginSignIn([this](const std::string& url) {
    // The hosted login opens as an ordinary Phi tab — Phi IS the browser
    // RFC 8252 wants the flow to ride in.
    host_.OpenUrlInFirstUserSlot(url);
  });
}

void PhiAccountAndFeedback::RequestSignOut() {
  auth_.SignOut();
  host_.NoteAccountChanged();
}

void PhiAccountAndFeedback::SetTestFeedbackAccount(const std::string& user_id,
                                                   const std::string& email) {
  test_feedback_user_id_ = user_id;
  test_feedback_email_ = email;
  SyncFeedbackAccount();
}

void PhiAccountAndFeedback::SyncFeedbackAccount() {
  if (auth_.signed_in()) {
    PhiFeedbackOutbox::Get().SetAccount(auth_.user().sub, auth_.user().email);
    return;
  }
  PhiFeedbackOutbox::Get().SetAccount(test_feedback_user_id_,
                                      test_feedback_email_);
}

void PhiAccountAndFeedback::OpenFeedback(HWND owner,
                                         const PhiFeedbackContext& context) {
  PhiFeedbackWindow::Open(this, context, owner);
}

void PhiAccountAndFeedback::OnFeedbackSystemLogs(int request_id,
                                                 const char* text) {
  PhiFeedbackWindow::OnSystemLogs(request_id, text);
}

bool PhiAccountAndFeedback::FeedbackAccount(std::string* email) {
  if (auth_.signed_in()) {
    *email = auth_.user().email;
    return true;
  }
  if (!test_feedback_user_id_.empty()) {
    *email = test_feedback_email_;
    return true;
  }
  return false;
}

void PhiAccountAndFeedback::RequestFeedbackSignIn() {
  RequestSignIn();
}

void PhiAccountAndFeedback::OpenFeedbackLink(int64_t window_id,
                                             const std::string& url) {
  if (!framework_) {
    return;
  }
  framework_->OpenTab(BrowserId{window_id}, url.c_str());
  host_.FocusWindowForBrowser(window_id);
}

bool PhiAccountAndFeedback::FetchFeedbackSystemLogs(int64_t window_id,
                                                    int request_id) {
  return framework_ &&
         framework_->FetchFeedbackSystemLogs(window_id, request_id);
}

std::map<std::string, std::string> PhiAccountAndFeedback::PhiExtensionVersions(
    int64_t window_id) {
  std::map<std::string, std::string> versions;
  if (!framework_) {
    return versions;
  }
  const char* json = framework_->ListExtensionsJson(window_id);
  std::optional<base::Value> list =
      base::JSONReader::Read(json ? json : "[]", base::JSON_PARSE_RFC);
  if (!list || !list->is_list()) {
    return versions;
  }
  for (const base::Value& row : list->GetList()) {
    const base::DictValue* dict = row.GetIfDict();
    const std::string* id = dict ? dict->FindString("id") : nullptr;
    const std::string* name = dict ? dict->FindString("name") : nullptr;
    const std::string* version = dict ? dict->FindString("version") : nullptr;
    if (id && name && version && PhiFeedbackOutbox::IsPhiExtensionId(*id)) {
      versions[*name] = *version;
    }
  }
  return versions;
}

}  // namespace phi_client
