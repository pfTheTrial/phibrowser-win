// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CLIENT_ANALYTICS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CLIENT_ANALYTICS_H_

#include <string>

#include "base/values.h"
#include "phibrowser-win/client/phi_app_model.h"

namespace phi_client {

// The CLIENT-originated half of the product-analytics namespace — the events
// the Mac app captures from its own shell code (docs/analytics.md there),
// carried here under the SAME names and property keys so one PostHog insight
// spans both platforms. The Chromium-originated half (memory_saver_state,
// renderer_health_*) arrives through the framework delegate and is
// registered in chrome/browser/phinomenon/analytics/README.md.
//
// This file only BUILDS property sets; nothing here captures. Every event
// goes out through PhiCoordinator::CaptureAnalytics, which is the client's
// one capture path (the harness ring, then PhiTelemetry's consent gate and
// allow-list). Adding an event is three edits: a builder here, its capture
// site, and the module's row in PhiTelemetry::AllowedProperties — the
// allow-list is the privacy review's reading list, so a property that is
// not fenced there is dropped at egress, however it was built.
//
// The registry of these events is docs/analytics.md in this repository.

// Module tags — the rows in PhiTelemetry::AllowedProperties.
inline constexpr char kAnalyticsModuleApp[] = "app";
inline constexpr char kAnalyticsModuleSettings[] = "settings";
inline constexpr char kAnalyticsModuleSpaces[] = "spaces";
inline constexpr char kAnalyticsModuleAgent[] = "agent";

// Event names. The four capitalised ones are what the Mac PostHog SDK
// auto-captures for the app lifecycle; matching them verbatim is what makes
// the Mac's DAU and retention insights count Windows sessions too.
inline constexpr char kEventAppOpened[] = "Application Opened";
inline constexpr char kEventAppBackgrounded[] = "Application Backgrounded";
inline constexpr char kEventAppInstalled[] = "Application Installed";
inline constexpr char kEventAppUpdated[] = "Application Updated";
inline constexpr char kEventUserDefaultsSnapshot[] = "user_defaults_snapshot";
inline constexpr char kEventSpaceCreated[] = "space_created";
inline constexpr char kEventSpaceSwitched[] = "space_switched";
inline constexpr char kEventAgentTaskStarted[] = "agent_task_started";
inline constexpr char kEventAgentTaskCompleted[] = "agent_task_completed";
inline constexpr char kEventAgentUserSpaceCommand[] =
    "agent_user_space_command";

// "Application Opened": the Mac SDK's lifecycle event plus the enrichment
// AppController's beforeSend hook adds (layout_mode, ai_enabled,
// is_guest_mode). `from_background` is false for the launch and true for
// every later return to the foreground.
base::DictValue PhiAppOpenedProperties(const model::PhiSettings& settings,
                                       bool from_background);

// "user_defaults_snapshot": the launch-time state of the settings the Mac
// snapshot reports, under the Mac's keys, for the ones that exist on
// Windows. `is_default_browser` is the caller's (a registry read the
// settings window already performs).
base::DictValue PhiUserDefaultsSnapshotProperties(
    const model::PhiSettings& settings,
    bool is_default_browser);

// "Application Installed" on a profile's first launch, "Application Updated"
// when the version differs from the one recorded at the previous launch,
// nothing otherwise. Reads and rewrites `<user_data_dir>\phi_last_version`.
// `event` is null when there is nothing to capture.
struct PhiVersionTransition {
  const char* event = nullptr;
  base::DictValue properties;
};
PhiVersionTransition PhiNoteAppVersion(const std::wstring& user_data_dir);

// "space_created" / "space_switched": the Mac's counts, never a Space's
// name, id or profile.
base::DictValue PhiSpaceCreatedProperties(int total_spaces,
                                          bool non_default_profile);
base::DictValue PhiSpaceSwitchedProperties(int total_spaces);

// The Mac's AgentDriverBadge.telemetryName: a known brand's product label
// ("Claude Code"), "other" for a name that is not one, "unknown" for none.
// Never the raw name — a process name or path is not for the collector.
std::string PhiAgentTelemetryName(const std::string& agent_name);

// "agent_task_started" / "agent_task_completed" / "agent_user_space_command".
// `origin` is the Mac's "cdp" or "phi_agent"; on Windows every task arrives
// over the control pipe, so the callers pass "phi_agent".
base::DictValue PhiAgentTaskProperties(const std::string& origin,
                                       bool persistent,
                                       const std::string& agent_name);
base::DictValue PhiAgentTaskCompletedProperties(const std::string& origin,
                                                bool persistent,
                                                const std::string& agent_name,
                                                bool success);
base::DictValue PhiAgentUserSpaceCommandProperties(
    const std::string& command,
    const std::string& agent_name);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CLIENT_ANALYTICS_H_
