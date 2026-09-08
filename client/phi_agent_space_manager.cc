// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_agent_space_manager.h"

#include <algorithm>
#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_agent_overlay.h"

namespace phi_client {

namespace {

// Epoch milliseconds, for the wire's createdAt.
int64_t EpochMsNow() {
  FILETIME ft = {};
  ::GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return static_cast<int64_t>((u.QuadPart - 116444736000000000ULL) / 10000);
}

}  // namespace

PhiAgentSpaceManager::PhiAgentSpaceManager(Host& host) : host_(host) {}

PhiAgentSpaceManager::~PhiAgentSpaceManager() = default;

void PhiAgentSpaceManager::Init(phi_framework::PhiFramework* framework) {
  framework_ = framework;
  // The operating mask: a hold changing repaints the owning shell, and the
  // reclaimed set is pushed to the browser, which then REFUSES drive commands
  // aimed at those tabs (the refusal is the browser's, so a driver cannot
  // decline to honour the takeover).
  drives_.set_changed_callback([this](int tab_id) { host_.InvalidateAll(); });
  drives_.set_reclaimed_callback([this](const std::vector<int>& tabs) {
    if (framework_) {
      framework_->SetUserReclaimedTabs(tabs.empty() ? nullptr : tabs.data(),
                                       static_cast<int>(tabs.size()));
    }
  });
}

PhiWindowController* PhiAgentSpaceManager::ControllerFor(
    const std::string& task_id) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  return space ? host_.ControllerForAgentWindow(space->window_id) : nullptr;
}

void PhiAgentSpaceManager::NoteAgentWindow(bool agent,
                                           const std::string& task_id,
                                           BrowserId window,
                                           const std::string& profile_id,
                                           const std::string& agent_name,
                                           bool persistent,
                                           bool incognito,
                                           uint64_t now_ms,
                                           base::DictValue* reply) {
  if (agent) {
    PhiAgentSpaces::Space& space =
        spaces_.Create(task_id, window, profile_id, persistent, now_ms);
    space.agent_name = agent_name;
    host_.InvalidateUserSlots();  // A new agent pip appears in the switcher.
    reply->Set("spaceId", space.space_id);
    reply->Set("ownership", "agent");
    return;
  }
  ShadowWindow& shadow = shadows_[task_id];
  shadow.task_id = task_id;
  shadow.window_id = window;
  shadow.profile_id = profile_id;
  shadow.incognito = incognito;
  shadow.created_ms = EpochMsNow();
  TouchShadow(&shadow, 0, now_ms);
  LOG(INFO) << "[PhiClient] shadow window created: " << task_id
            << " window=" << window.value << (incognito ? " (incognito)" : "");
  // Echoed on purpose: a driver that asked for incognito checks this flag
  // and refuses to proceed if it is missing (an older build).
  reply->Set("incognito", incognito);
}

bool PhiAgentSpaceManager::RetireHiddenWindow(BrowserId browser) {
  if (ShadowWindow* shadow = FindShadowByWindow(browser)) {
    const std::string task_id = shadow->task_id;  // Copied: erase frees it.
    shadows_.erase(task_id);
    return true;
  }
  if (PhiAgentSpaces::Space* space = spaces_.FindByWindow(browser)) {
    const std::string task_id = space->task_id;
    spaces_.Remove(task_id);
    PhiTranscriptPanel::CloseFor(task_id);
    if (watched_task_ == task_id) {
      watched_task_.clear();
    }
    return true;
  }
  return false;
}

void PhiAgentSpaceManager::HideOverlayBoundTo(
    const PhiWindowController* controller) {
  PhiAgentOverlay::HideIfBoundTo(controller);
}

void PhiAgentSpaceManager::Sweep(uint64_t now_ms) {
  for (const std::string& task_id : spaces_.Expired(now_ms)) {
    LOG(INFO) << "[PhiClient] agent space expired: " << task_id;
    CloseAgentSpace(task_id);
  }
  // Shadow windows have no user to hold them open: the sweep is their only
  // backstop against an abandoned driver.
  std::vector<std::string> expired_shadows;
  for (const auto& [task_id, shadow] : shadows_) {
    if (shadow.keepalive_deadline_ms && shadow.keepalive_deadline_ms < now_ms) {
      expired_shadows.push_back(task_id);
    }
  }
  for (const std::string& task_id : expired_shadows) {
    CloseShadowWindow(task_id, "keep-alive expired");
  }
}

std::vector<PhiAgentPipView> PhiAgentSpaceManager::Pips() const {
  std::vector<PhiAgentPipView> out;
  for (const auto& [task_id, space] : spaces_.entries()) {
    PhiAgentPipView pip;
    pip.task_id = task_id;
    pip.caption = space.caption;
    pip.watched = task_id == watched_task_;
    out.push_back(std::move(pip));
  }
  return out;
}

void PhiAgentSpaceManager::RequestWatch(const std::string& task_id) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (!space) {
    return;
  }
  auto controller_for = [this](const std::string& id) {
    return ControllerFor(id);
  };
  // Clicking the watched pip again parks it; clicking a different one swaps.
  if (watched_task_ == task_id) {
    if (PhiWindowController* c = controller_for(task_id)) {
      c->SetAgentVisible(false);
    }
    watched_task_.clear();
  } else {
    if (!watched_task_.empty()) {
      if (PhiWindowController* prev = controller_for(watched_task_)) {
        prev->SetAgentVisible(false);
      }
    }
    if (PhiWindowController* c = controller_for(task_id)) {
      c->SetAgentVisible(true);
    }
    watched_task_ = task_id;
  }
  RefreshOverlay();
  host_.InvalidateUserSlots();
}

void PhiAgentSpaceManager::RefreshOverlay() {
  if (watched_task_.empty()) {
    PhiAgentOverlay::Hide();
    return;
  }
  PhiAgentSpaces::Space* space = spaces_.Find(watched_task_);
  if (!space) {
    PhiAgentOverlay::Hide();
    return;
  }
  PhiWindowController* controller =
      host_.ControllerForAgentWindow(space->window_id);
  if (!controller) {
    PhiAgentOverlay::Hide();
    return;
  }
  const std::wstring label = space->agent_name.empty()
                                 ? L"Agent"
                                 : base::UTF8ToWide(space->agent_name);
  const bool agent_owns = space->ownership == PhiAgentSpaces::Ownership::kAgent;
  std::wstring caption = base::UTF8ToWide(space->caption);
  if (caption.empty()) {
    caption = agent_owns ? L"Agent is working\u2026" : L"Agent paused";
  }
  if (space->status == "failed") {
    caption = L"\u26A0 " + caption;
  }
  PhiAgentOverlay::Show(controller, watched_task_, label, caption, agent_owns);
}

void PhiAgentSpaceManager::RequestTakeControl(const std::string& task_id) {
  if (!spaces_.Find(task_id)) {
    return;
  }
  spaces_.SetOwnership(task_id, PhiAgentSpaces::Ownership::kUser,
                       static_cast<uint64_t>(::GetTickCount64()));
  PushOwnershipChanged(task_id);  // The driver must learn it lost control.
  RefreshOverlay();
}

void PhiAgentSpaceManager::RequestHandBack(const std::string& task_id) {
  if (!spaces_.Find(task_id)) {
    return;
  }
  spaces_.SetOwnership(task_id, PhiAgentSpaces::Ownership::kAgent,
                       static_cast<uint64_t>(::GetTickCount64()));
  PushOwnershipChanged(task_id);
  RefreshOverlay();
}

void PhiAgentSpaceManager::RequestFinish(const std::string& task_id) {
  // Ends the task outright — the pill's escape hatch when an agent is stuck.
  CloseAgentSpace(task_id);
  if (watched_task_ == task_id) {
    watched_task_.clear();
  }
  PhiAgentOverlay::Hide();
  host_.InvalidateUserSlots();
}

std::vector<std::string> PhiAgentSpaceManager::TranscriptLines(
    const std::string& task_id) {
  std::vector<std::string> lines;
  if (PhiAgentSpaces::Space* space = spaces_.Find(task_id)) {
    lines.assign(space->log.begin(), space->log.end());
  }
  return lines;
}

std::string PhiAgentSpaceManager::TranscriptAgentName(
    const std::string& task_id) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  return space && !space->agent_name.empty() ? space->agent_name : "Agent";
}

bool PhiAgentSpaceManager::PushTranscriptMessage(const std::string& task_id,
                                                 const std::string& text) {
  const std::string id = spaces_.PushMessage(
      task_id, text, base::Time::Now().InMillisecondsSinceUnixEpoch());
  if (id.empty()) {
    return false;
  }
  // The console shows the user's line inline, like the wire's userMessage.
  spaces_.AppendLog(task_id, "You: " + text);
  return true;
}

void PhiAgentSpaceManager::CloseAgentSpace(const std::string& task_id) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (!space) {
    return;
  }
  const BrowserId window_id = space->window_id;
  spaces_.Remove(task_id);
  PhiTranscriptPanel::CloseFor(task_id);
  if (watched_task_ == task_id) {
    watched_task_.clear();
  }
  if (PhiWindowController* shell = host_.ControllerForAgentWindow(window_id)) {
    shell->RequestClose();  // Fires OnBrowserWindowClosing → cleanup.
  } else if (framework_ && window_id.valid()) {
    framework_->CloseBrowserWindow(window_id.value);
  }
  host_.UpdateSweepTimer();
  host_.InvalidateUserSlots();  // The agent pip disappears.
}

PhiAgentSpaceManager::ShadowWindow* PhiAgentSpaceManager::FindShadowByWindow(
    BrowserId window_id) {
  for (auto& [task_id, shadow] : shadows_) {
    if (shadow.window_id == window_id) {
      return &shadow;
    }
  }
  return nullptr;
}

void PhiAgentSpaceManager::CloseShadowWindow(const std::string& task_id,
                                             const char* reason) {
  auto it = shadows_.find(task_id);
  if (it == shadows_.end()) {
    return;
  }
  const BrowserId window_id = it->second.window_id;
  shadows_.erase(it);
  LOG(INFO) << "[PhiClient] shadow window closed (" << reason
            << "): " << task_id;
  if (PhiWindowController* shell = host_.ControllerForAgentWindow(window_id)) {
    shell->RequestClose();  // Fires OnBrowserWindowClosing.
  } else if (framework_ && window_id.valid()) {
    framework_->CloseBrowserWindow(window_id.value);
  }
  host_.UpdateSweepTimer();
}

void PhiAgentSpaceManager::TouchShadow(ShadowWindow* shadow,
                                       int ttl_seconds,
                                       uint64_t now_ms) {
  if (ttl_seconds <= 0) {
    ttl_seconds = PhiAgentSpaces::kDefaultTtlSeconds;
  }
  ttl_seconds = std::clamp(ttl_seconds, 1, PhiAgentSpaces::kMaxTtlSeconds);
  shadow->keepalive_deadline_ms =
      std::max(shadow->keepalive_deadline_ms,
               now_ms + static_cast<uint64_t>(ttl_seconds) * 1000);
}

void PhiAgentSpaceManager::PushOwnershipChanged(const std::string& task_id) {
  PhiAgentSpaces::Space* space = spaces_.Find(task_id);
  if (!space) {
    return;  // Ended task: its disappearance from list() is the signal.
  }
  base::DictValue payload;
  payload.Set("taskId", task_id);
  payload.Set("owner", space->ownership == PhiAgentSpaces::Ownership::kUser
                           ? "user"
                           : "agent");
  std::string payload_json;
  base::JSONWriter::Write(payload, &payload_json);
  host_.BroadcastControlEvent("agentSpace.ownershipChanged", payload_json);
}

}  // namespace phi_client
