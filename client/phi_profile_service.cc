// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_profile_service.h"

#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_client_analytics.h"

namespace phi_client {

namespace {
constexpr char kCreateFailedReply[] =
    "{\"ok\":false,\"error\":\"create_failed\"}";
}  // namespace

PhiProfileService::PhiProfileService(phi_framework::PhiFramework& framework,
                                     Host& host)
    : framework_(framework), host_(host) {}

PhiProfileService::~PhiProfileService() = default;

void PhiProfileService::RequestProfile(PendingProfile pending) {
  if (pending.display_name.empty()) {
    if (pending.pending_reply_id) {
      host_.CompleteProfileReply(pending.pending_reply_id, kCreateFailedReply);
    }
    return;
  }
  pending.deadline_tick = ::GetTickCount64() + kCreateTimeoutMs;
  const std::string name = pending.display_name;
  pending_.push_back(std::move(pending));
  host_.UpdateSweepTimer();
  framework_.CreateProfile(name.c_str());
}

void PhiProfileService::OnProfileCreated(const char* profile_id) {
  const std::string id = profile_id ? profile_id : "";
  LOG(INFO) << "[PhiClient] OnProfileCreated "
            << (id.empty() ? "(failed)" : id);
  if (!id.empty()) {
    // The Mac's profile_created: a creation landed, whoever asked (this
    // client has no agent fallback profile to exclude). The count only.
    host_.CaptureAnalytics(
        kAnalyticsModuleProfiles, kEventProfileCreated,
        PhiProfileCountProperties(static_cast<int>(Profiles().size())));
  }
  if (pending_.empty()) {
    return;  // Settings' "New Profile" (CreateProfileNamed) asks for nothing.
  }
  // The request this profile answers: by the display name it was asked
  // under, else the oldest — the framework reports the name only once the
  // attributes entry exists, which a failed creation never reaches.
  auto pending = pending_.end();
  const std::string name = id.empty() ? std::string() : ProfileDisplayName(id);
  if (!name.empty()) {
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->display_name == name) {
        pending = it;
        break;
      }
    }
  }
  if (pending == pending_.end()) {
    pending = pending_.begin();
  }
  const PendingProfile request = std::move(*pending);
  pending_.erase(pending);
  host_.UpdateSweepTimer();
  if (id.empty()) {
    LOG(ERROR) << "[PhiClient] profile creation failed for '"
               << request.display_name << "'";
    if (request.pending_reply_id) {
      host_.CompleteProfileReply(request.pending_reply_id, kCreateFailedReply);
    }
    return;
  }
  switch (request.then) {
    case PendingProfile::Then::kReplyOnly: {
      base::DictValue out;
      out.Set("ok", true);
      out.Set("profileId", id);
      std::string json;
      base::JSONWriter::Write(out, &json);
      host_.CompleteProfileReply(request.pending_reply_id, std::move(json));
      return;
    }
    case PendingProfile::Then::kFillForm:
      // The create-Space form asked for it: the form's picker selects it,
      // and the Space follows when the form is submitted.
      host_.FillSpaceFormProfile(request.slot, id);
      return;
    case PendingProfile::Then::kOpenSpace:
      // "+ Space & profile": the Space binds the new profile — bindings are
      // immutable, which is why the Space waited for the basename — and the
      // slot enters it (a RequestBrowser, since the profile has no window).
      host_.OpenSpaceOnNewProfile(request.slot, id);
      return;
  }
}

void PhiProfileService::ExpirePending(uint64_t now_ms,
                                      const char* timeout_reply) {
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->deadline_tick && it->deadline_tick <= now_ms) {
      LOG(ERROR) << "[PhiClient] profile creation timed out: '"
                 << it->display_name << "'";
      if (it->pending_reply_id) {
        host_.CompleteProfileReply(it->pending_reply_id, timeout_reply);
      }
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<PhiProfileSummary> PhiProfileService::Profiles() const {
  std::vector<PhiProfileSummary> out;
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework_.ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return out;
  }
  for (const base::Value& value : parsed->GetList()) {
    if (!value.is_dict()) {
      continue;
    }
    const std::string* id = value.GetDict().FindString("id");
    const std::string* name = value.GetDict().FindString("name");
    if (id) {
      // A nameless profile shows as its id, as it always has.
      out.push_back({*id, name ? *name : *id});
    }
  }
  return out;
}

std::vector<std::string> PhiProfileService::ProfileSummaries() const {
  std::vector<std::string> out;
  for (const PhiProfileSummary& profile : Profiles()) {
    out.push_back(profile.id + "\t" + profile.name);
  }
  return out;
}

std::string PhiProfileService::ProfileDisplayName(
    const std::string& profile_id) const {
  if (profile_id.empty()) {
    return std::string();
  }
  std::optional<base::Value> profiles = base::JSONReader::Read(
      framework_.ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!profiles || !profiles->is_list()) {
    return std::string();
  }
  for (const base::Value& v : profiles->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    const std::string* id = d ? d->FindString("id") : nullptr;
    const std::string* name = d ? d->FindString("name") : nullptr;
    if (id && *id == profile_id) {
      return name ? *name : std::string();
    }
  }
  return std::string();
}

bool PhiProfileService::KnowsProfile(const std::string& profile_id) const {
  std::optional<base::Value> profiles = base::JSONReader::Read(
      framework_.ListProfilesJson(), base::JSON_PARSE_RFC);
  if (!profiles || !profiles->is_list()) {
    return false;
  }
  for (const base::Value& v : profiles->GetList()) {
    const base::DictValue* d = v.GetIfDict();
    const std::string* id = d ? d->FindString("id") : nullptr;
    if (id && *id == profile_id) {
      return true;
    }
  }
  return false;
}

void PhiProfileService::CreateProfileNamed(const std::string& name) {
  if (!name.empty()) {
    framework_.CreateProfile(name.c_str());
  }
}

bool PhiProfileService::RenameProfileById(const std::string& profile_id,
                                          const std::string& name) {
  if (profile_id.empty() || name.empty()) {
    return false;
  }
  // Refuse an id the framework does not know — the rename callback is
  // fire-and-forget, so the check has to happen here.
  if (!KnowsProfile(profile_id)) {
    return false;
  }
  framework_.RenameProfile(profile_id.c_str(), name.c_str());
  host_.NoteProfilesChanged();
  return true;
}

bool PhiProfileService::DeleteProfileById(const std::string& profile_id) {
  // Default is permanent, and a profile still bound to a Space stays — the
  // Spaces must be re-profiled or deleted first.
  if (profile_id.empty() || profile_id == "Default") {
    return false;
  }
  if (host_.AnySpaceUsesProfile(profile_id)) {
    return false;
  }
  framework_.DeleteProfile(profile_id.c_str());
  host_.NoteProfilesChanged();
  return true;
}

std::vector<PhiSearchEngineRow> PhiProfileService::SearchEngines(
    const std::string& profile_id) const {
  std::vector<PhiSearchEngineRow> out;
  std::optional<base::Value> parsed = base::JSONReader::Read(
      framework_.ListSearchEnginesJson(profile_id.c_str()),
      base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return out;
  }
  for (const base::Value& value : parsed->GetList()) {
    if (!value.is_dict()) {
      continue;
    }
    const std::string* keyword = value.GetDict().FindString("keyword");
    const std::string* name = value.GetDict().FindString("name");
    if (!keyword) {
      continue;
    }
    // A nameless engine shows as its keyword, as it always has.
    out.push_back({*keyword, name && !name->empty() ? *name : *keyword,
                   value.GetDict().FindBool("default").value_or(false)});
  }
  return out;
}

bool PhiProfileService::SetSearchEngine(const std::string& profile_id,
                                        const std::string& keyword) {
  return framework_.SetDefaultSearchEngine(profile_id.c_str(), keyword.c_str());
}

std::string PhiProfileService::DownloadDirectory(
    const std::string& profile_id) const {
  return framework_.DownloadDirectoryUtf8(profile_id.c_str());
}

bool PhiProfileService::SetDownloadDirectory(const std::string& profile_id,
                                             const std::string& dir) {
  return framework_.SetDownloadDirectory(profile_id.c_str(), dir.c_str());
}

}  // namespace phi_client
