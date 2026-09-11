// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_client_analytics.h"

#include <utility>

#include "phibrowser-win/client/phi_agent_icons.h"
#include "phibrowser-win/client/phi_file_io.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_shell_layout.h"
#include "phibrowser-win/client/phi_version.h"

namespace phi_client {

base::DictValue PhiAppOpenedProperties(const model::PhiSettings& settings,
                                       bool from_background) {
  base::DictValue props;
  props.Set("from_background", from_background);
  props.Set("version", PHI_WIN_VERSION_STRING);
  props.Set("layout_mode", PhiLayoutModeName(settings.layout_mode));
  // The Mac's beforeSend enrichment. Neither AI features nor Guest Mode
  // exist in this client yet, so both are reported false rather than
  // omitted: an insight that filters on them keeps counting Windows.
  props.Set("ai_enabled", false);
  props.Set("is_guest_mode", false);
  return props;
}

base::DictValue PhiUserDefaultsSnapshotProperties(
    const model::PhiSettings& settings,
    bool is_default_browser) {
  base::DictValue props;
  props.Set("new_tab_behavior",
            settings.new_tab_behavior == model::NewTabBehavior::kOmnibox
                ? "omnibox"
                : "ntp");
  props.Set("layout_mode", PhiLayoutModeName(settings.layout_mode));
  // The locale the process is running in, which is the stored preference
  // or, when that is empty, the system's — the Mac's
  // activeProcessAppLanguage.
  props.Set("app_language", PhiActiveLocale());
  // The user's CHOICE (the Mac's UserAppearanceChoice), not the resolved
  // appearance; the model already speaks the Mac's wire names.
  props.Set("appearance", model::AppearanceName(settings.appearance));
  props.Set("is_phi_default_browser", is_default_browser);
  props.Set("peek_view_enabled", settings.peek_view_enabled);
  props.Set("always_show_full_url", settings.always_show_full_url);
  props.Set("show_tab_previews", settings.show_tab_previews);
  props.Set("show_open_tab_indicators", settings.show_open_tab_indicators);
  props.Set("dim_unloaded_tab_icons", settings.dim_unloaded_tab_icons);
  props.Set("restore_last_session_enabled", settings.restore_last_session);
  props.Set("developer_mode_enabled", settings.developer_mode);
  // The Mac's cdpAgentAccessEnabled is the same switch as Developer ▸
  // "Allow agents to control Phi".
  props.Set("cdp_agent_access_enabled", settings.allow_agent_control);
  // Windows-only switches, named for what they are.
  props.Set("agent_user_spaces_enabled", settings.allow_agent_user_spaces);
  props.Set("always_show_bookmark_bar", settings.always_show_bookmark_bar);
  return props;
}

PhiVersionTransition PhiNoteAppVersion(const std::wstring& user_data_dir) {
  PhiVersionTransition out;
  if (user_data_dir.empty()) {
    return out;
  }
  const std::wstring path = user_data_dir + L"\\phi_last_version";
  std::string previous;
  PhiReadFileBytes(path, &previous);
  while (!previous.empty() &&
         (previous.back() == '\n' || previous.back() == '\r' ||
          previous.back() == ' ')) {
    previous.pop_back();
  }
  const std::string current = PHI_WIN_VERSION_STRING;
  if (previous == current) {
    return out;  // The common case: nothing to say.
  }
  // Recorded BEFORE the event is built, so a launch that dies between here
  // and the upload reports the transition once, not on every launch until
  // one survives. The event itself is best-effort, as all of them are.
  PhiWriteFileAtomic(path, current);
  out.properties.Set("version", current);
  out.properties.Set("build", PHI_WIN_VERSION_BUILD);
  if (previous.empty()) {
    out.event = kEventAppInstalled;
  } else {
    out.event = kEventAppUpdated;
    out.properties.Set("previous_version", previous);
  }
  return out;
}

base::DictValue PhiSpaceCreatedProperties(int total_spaces,
                                          bool non_default_profile) {
  base::DictValue props;
  props.Set("total_spaces", total_spaces);
  props.Set("non_default_profile", non_default_profile);
  return props;
}

base::DictValue PhiSpaceSwitchedProperties(int total_spaces) {
  base::DictValue props;
  props.Set("total_spaces", total_spaces);
  return props;
}

std::string PhiAgentTelemetryName(const std::string& agent_name) {
  bool blank = true;
  for (const char c : agent_name) {
    if (c != ' ' && c != '\t') {
      blank = false;
      break;
    }
  }
  if (blank) {
    return "unknown";
  }
  // The badge resolver is the one place a name is matched to a brand; a
  // name it gives no mark to is "other", never the name itself.
  const PhiAgentDriverBadge badge = PhiAgentDriverBadgeFor(agent_name);
  return badge.icon.empty() ? std::string("other") : badge.label;
}

base::DictValue PhiAgentTaskProperties(const std::string& origin,
                                       bool persistent,
                                       const std::string& agent_name) {
  base::DictValue props;
  props.Set("origin", origin);
  props.Set("persistent", persistent);
  props.Set("agent_name", PhiAgentTelemetryName(agent_name));
  return props;
}

base::DictValue PhiAgentTaskCompletedProperties(const std::string& origin,
                                                bool persistent,
                                                const std::string& agent_name,
                                                bool success) {
  base::DictValue props = PhiAgentTaskProperties(origin, persistent, agent_name);
  props.Set("success", success);
  return props;
}

base::DictValue PhiAgentUserSpaceCommandProperties(
    const std::string& command,
    const std::string& agent_name) {
  base::DictValue props;
  props.Set("command", command);
  props.Set("agent_name", PhiAgentTelemetryName(agent_name));
  return props;
}

}  // namespace phi_client
