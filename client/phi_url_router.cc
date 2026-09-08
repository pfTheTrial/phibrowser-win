// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_url_router.h"

#include <windows.h>

#include <utility>

#include "base/json/json_writer.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_space_manager.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

PhiUrlRouter::PhiUrlRouter(model::PhiSpacesModel& model,
                           PhiWindowRegistry& registry,
                           phi_framework::PhiFramework& framework,
                           Host& host,
                           DirtyCallback dirty)
    : model_(model),
      registry_(registry),
      framework_(framework),
      host_(host),
      dirty_(std::move(dirty)) {}

PhiUrlRouter::~PhiUrlRouter() = default;

// --- Rules ----------------------------------------------------------------------

void PhiUrlRouter::AddRule(const std::string& space_id,
                           const std::string& host,
                           const std::string& path_prefix,
                           bool ask) {
  if (!AddUrlRuleTo(&model_, space_id, host, path_prefix, ask)) {
    return;
  }
  MarkDirty();
  host_.SchedulePushRoutingTable();
}

void PhiUrlRouter::SetRulePattern(int index,
                                  const std::string& host,
                                  const std::string& path_prefix) {
  if (!SetUrlRulePatternIn(&model_, index, host, path_prefix)) {
    return;
  }
  MarkDirty();
  host_.SchedulePushRoutingTable();
}

void PhiUrlRouter::SetRuleTarget(int index,
                                 const std::string& space_id,
                                 bool ask) {
  if (!SetUrlRuleTargetIn(&model_, index, space_id, ask)) {
    return;
  }
  MarkDirty();
  host_.SchedulePushRoutingTable();
}

void PhiUrlRouter::RemoveRule(int index) {
  if (!RemoveUrlRuleFrom(&model_, index)) {
    return;
  }
  MarkDirty();
  host_.SchedulePushRoutingTable();
}

void PhiUrlRouter::DropRulesForSpace(const std::string& space_id) {
  // The generation moves either way (the Space list's write is the same
  // file); the manager's own save carries the rules, so no push of its own
  // is needed beyond the one every Spaces flush makes — but one is cheap
  // and keeps the table honest should the flush be far off.
  DropUrlRulesForSpace(&model_, space_id);
  MarkDirty();
  host_.SchedulePushRoutingTable();
}

// --- Routing decisions ----------------------------------------------------------

void PhiUrlRouter::OnOpenLinkInSpace(int64_t source_window_id,
                                     const char* space_id_c,
                                     const char* url_c) {
  const std::string space_id = space_id_c ? space_id_c : "";
  const std::string url = url_c ? url_c : "";
  if (space_id.empty() || url.empty()) {
    return;
  }
  RouteUrlToSpace(space_id, url, BrowserId{source_window_id});
}

void PhiUrlRouter::OnRouteURLToSpace(const char* space_id_c,
                                     const char* url_c,
                                     int64_t source_window_id) {
  const std::string space_id = space_id_c ? space_id_c : "";
  const std::string url = url_c ? url_c : "";
  if (space_id.empty() || url.empty()) {
    return;
  }
  RouteUrlToSpace(space_id, url, BrowserId{source_window_id});
}

void PhiUrlRouter::OnAskSpaceForURL(const char* url_c,
                                    const char* default_space_id_c,
                                    int64_t source_window_id,
                                    bool source_is_new_tab) {
  PendingAsk ask;
  ask.url = url_c ? url_c : "";
  ask.default_space_id = default_space_id_c ? default_space_id_c : "";
  ask.source_window_id = source_window_id;
  ask.source_is_new_tab = source_is_new_tab;
  if (ask.url.empty() || model_.spaces.empty()) {
    return;
  }
  // Deferred (see the header): the throttle callback this arrives in is no
  // place for a modal loop. The navigation is already cancelled, so the
  // chooser loses nothing by running a turn later.
  pending_asks_.push_back(std::move(ask));
  host_.PostAskSpace();
}

void PhiUrlRouter::RunPendingAsk() {
  if (pending_asks_.empty()) {
    return;
  }
  const PendingAsk ask = std::move(pending_asks_.front());
  pending_asks_.pop_front();
  if (model_.spaces.empty()) {
    return;
  }
  // The source slot's shell owns the (modal) chooser; its active Space is
  // the "keep here" target.
  const BrowserId source{ask.source_window_id};
  PhiWindowController* controller = registry_.ControllerForBrowser(source);
  const std::string current_space_id =
      controller ? controller->active_space() : std::string();

  std::vector<std::wstring> options;
  int default_index = 0;
  for (size_t i = 0; i < model_.spaces.size(); ++i) {
    std::wstring label = base::UTF8ToWide(model_.spaces[i].name);
    if (model_.spaces[i].id == current_space_id) {
      label += L"  (current)";
    }
    options.push_back(std::move(label));
    if (model_.spaces[i].id == ask.default_space_id) {
      default_index = static_cast<int>(i);
    }
  }
  const int chosen = PhiPromptChoose(
      controller ? controller->shell_hwnd() : nullptr, L"Open in Space",
      base::UTF8ToWide(ask.url).c_str(), options, default_index);
  const std::string chosen_space =
      chosen >= 0 && chosen < static_cast<int>(model_.spaces.size())
          ? model_.spaces[static_cast<size_t>(chosen)].id
          : std::string();
  if (chosen_space.empty() || chosen_space == current_space_id) {
    // Keep it here. The cancelled navigation was replacing the active tab
    // when it started from a fresh new tab — re-issue it in place there;
    // otherwise open a fresh foreground tab (the Mac split). Both bypass
    // routing so the same rule cannot re-prompt.
    if (ask.source_is_new_tab) {
      framework_.NavigateActiveTabBypassingSpaceRouting(ask.source_window_id,
                                                        ask.url.c_str());
    } else {
      framework_.OpenTabBypassingSpaceRouting(ask.source_window_id,
                                              ask.url.c_str());
    }
    if (controller) {
      controller->ScheduleRefresh();
    }
    return;
  }
  RouteUrlToSpace(chosen_space, ask.url, source);
}

void PhiUrlRouter::RouteUrlToSpace(const std::string& space_id,
                                   const std::string& url,
                                   BrowserId source_window) {
  // A slot that already holds the Space: switch to it there and open the
  // link (the Mac activates the Space's window and opens the tab). The open
  // bypasses URL routing — the URL may be the very navigation a rule just
  // re-homed, and re-matching it would loop.
  for (PhiWindowController* controller : registry_.Controllers()) {
    if (!controller->HasSpace(space_id)) {
      continue;
    }
    controller->ShowSpace(space_id);
    framework_.OpenTabBypassingSpaceRouting(
        controller->active_browser_id().value, url.c_str());
    if (controller->shell_hwnd()) {
      ::SetForegroundWindow(controller->shell_hwnd());
    }
    controller->ScheduleRefresh();
    host_.SchedulePushSpaceMenu();  // The visible-Space routing map changed.
    return;
  }
  // Cold Space: ask for its browser in its profile; the link opens once the
  // browser attaches (the spaces.switch machinery), so a request already in
  // flight for this slot and Space just gains the url.
  const model::Space* space = FindSpaceIn(model_.spaces, space_id);
  if (!space) {
    return;
  }
  const SlotId slot =
      registry_.SlotFor(source_window).value_or(registry_.FirstUserSlot());
  if (!slot.valid()) {
    return;
  }
  if (PendingCreation* in_flight = registry_.FindPendingSpace(slot, space_id)) {
    in_flight->open_urls.push_back(url);
    return;
  }
  PendingCreation pending;
  pending.slot = slot;
  pending.space_id = space_id;
  pending.open_urls.push_back(url);
  host_.RequestSpaceBrowser(space->profile_id, std::move(pending));
}

// --- The framework's copy -------------------------------------------------------

void PhiUrlRouter::PushRoutingTableNow() {
  base::DictValue root;
  base::ListValue rules;
  int sort_order = 0;
  for (const model::UrlRule& rule : model_.url_rules) {
    if (!UrlRuleIsLive(rule)) {
      continue;
    }
    base::DictValue d;
    d.Set("spaceId", rule.space_id);
    d.Set("host", rule.host);
    if (!rule.path_prefix.empty()) {
      d.Set("pathPrefix", rule.path_prefix);
    }
    d.Set("ask", rule.ask);
    d.Set("sortOrder", sort_order++);
    rules.Append(std::move(d));
  }
  root.Set("rules", std::move(rules));
  // Each Space's VISIBLE browser only — a Space hidden behind a slot's
  // active sibling is deliberately absent, so a match on it resolves to
  // OnRouteURLToSpace (which re-enters ShowSpace) instead of a direct open
  // into an off-screen browser.
  base::DictValue map;
  for (PhiWindowController* controller : registry_.Controllers()) {
    const std::string& space_id = controller->active_space();
    const BrowserId browser_id = controller->active_browser_id();
    if (space_id.empty() || !browser_id.valid() || map.Find(space_id)) {
      continue;
    }
    map.Set(space_id, static_cast<double>(browser_id.value));
  }
  root.Set("spaceWindows", std::move(map));
  std::string json;
  base::JSONWriter::Write(root, &json);
  framework_.SetSpaceRoutingTable(json.c_str());
}

void PhiUrlRouter::MarkDirty() {
  if (dirty_) {
    dirty_();
  }
}

}  // namespace phi_client
