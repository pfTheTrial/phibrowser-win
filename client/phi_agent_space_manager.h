// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACE_MANAGER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACE_MANAGER_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_agent_drive_registry.h"
#include "phibrowser-win/client/phi_agent_spaces.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_transcript_panel.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

// The phi-browser skill's hidden windows (docs/architecture-review.md §3.4,
// lane 1 §1.2 clusters 12 and 18): agent Spaces (a hidden browser with a
// presence — a pip, a watch mode, a transcript and an ownership state),
// shadow windows (the same hidden surface WITHOUT a presence), the keep-alive
// sweep, and the operating mask over the user's OWN tabs.
//
// Invariants (§3.4): the watch overlay is owned per shell and unhooked
// before the shell it is bound to goes; ownership transitions are ENFORCED
// (the record moves first, then the driver is told — a driver cannot decline
// a takeover); and a task whose record is already gone is deliberately not
// announced, because drivers learn of an ended task from its absence in
// agentSpace.list.
class PhiAgentSpaceManager : public PhiTranscriptHost {
 public:
  // A shadow window is an agent-style off-screen window WITHOUT a presence:
  // no pip, no watch, no transcript, no ownership — the Mac's
  // AgentSpaceManager+Shadow registry, keyed by the skill's taskId. Kept out
  // of the agent Spaces so nothing that draws or surfaces agent Spaces sees
  // it; the window itself is adopted exactly like an agent Space's (hidden
  // controller, parked off-screen, composited for CDP).
  struct ShadowWindow {
    std::string task_id;
    BrowserId window_id;  // Its value is the CDP windowId.
    std::string profile_id;
    bool incognito = false;
    int64_t created_ms = 0;              // epoch ms (the list's createdAt)
    uint64_t keepalive_deadline_ms = 0;  // GetTickCount64 ms; swept when past
  };

  // What the manager needs from the app: the shell behind a hidden window,
  // the repaints, the sweep arming, the wire and the deferred replies.
  class Host {
   public:
    virtual ~Host() = default;
    // The controller hosting `window_id` (agent windows live in a slot of
    // their own, keyed by the browser that opened them), or null.
    virtual PhiWindowController* ControllerForAgentWindow(
        BrowserId window_id) = 0;
    // The first USER window's shell, for the floating transcript panel, with
    // its palette. Null when no user window is up.
    virtual PhiWindowController* FirstUserController() = 0;
    // The agent pips live in the user windows' switchers.
    virtual void InvalidateUserSlots() = 0;
    // The operating mask changed: every window repaints its rows.
    virtual void InvalidateAll() = 0;
    // Something joined or left the swept set.
    virtual void UpdateSweepTimer() = 0;
    // agentSpace.* events out to the connected drivers.
    virtual void BroadcastControlEvent(const std::string& type,
                                       const std::string& payload_json) = 0;
    // The waiting verb's deferred reply (window creation).
    virtual void CompleteAgentReply(int reply_id, std::string json) = 0;
  };

  explicit PhiAgentSpaceManager(Host& host);
  ~PhiAgentSpaceManager() override;

  PhiAgentSpaceManager(const PhiAgentSpaceManager&) = delete;
  PhiAgentSpaceManager& operator=(const PhiAgentSpaceManager&) = delete;

  // The framework arrives with OnFrameworkReady, and the operating mask's
  // callbacks are wired here. Every framework call below is guarded exactly
  // as the coordinator's `if (framework_)` was.
  void Init(phi_framework::PhiFramework* framework);

  // The records. Published rather than wrapped: the verb ladder addresses
  // both registries directly (the same documented exception
  // PhiSpaceManager::mutable_model() is).
  PhiAgentSpaces& spaces() { return spaces_; }
  std::map<std::string, ShadowWindow>& shadows() { return shadows_; }
  // Which of the user's own tabs an agent currently holds.
  PhiAgentDriveRegistry& drives() { return drives_; }
  const PhiAgentDriveRegistry& drives() const { return drives_; }
  bool Empty() const { return spaces_.empty() && shadows_.empty(); }

  // --- Adoption ------------------------------------------------------------
  // The hidden window for `task_id` has arrived: file it as an agent Space
  // (`agent` true) or a shadow, and fill `reply` with the wire fields the
  // waiting verb expects (spaceId + ownership, or incognito).
  void NoteAgentWindow(bool agent,
                       const std::string& task_id,
                       BrowserId window,
                       const std::string& profile_id,
                       const std::string& agent_name,
                       bool persistent,
                       bool incognito,
                       uint64_t now_ms,
                       base::DictValue* reply);
  // The hidden window behind `browser` is closing: drop its record. True
  // when it WAS one of ours (an agent Space or a shadow).
  bool RetireHiddenWindow(BrowserId browser);

  // --- Pips, watch mode and the overlay ------------------------------------
  std::vector<PhiAgentPipView> Pips() const;
  const std::string& watched_task() const { return watched_task_; }
  // Clicking the watched pip again parks it; clicking a different one swaps.
  void RequestWatch(const std::string& task_id);
  // Mounts / updates / tears down the watch-mode overlay for the watched task.
  void RefreshOverlay();
  // Hide() only if the overlay is bound to the shell that is going.
  void HideOverlayBoundTo(const PhiWindowController* controller);

  // --- Ownership -----------------------------------------------------------
  void RequestTakeControl(const std::string& task_id);
  void RequestHandBack(const std::string& task_id);
  // Ends the task outright — the pill's escape hatch when an agent is stuck.
  void RequestFinish(const std::string& task_id);
  // Pushes agentSpace.ownershipChanged to connected drivers — the Mac's
  // broadcastToTaskDriver. Called after every ownership transition.
  void PushOwnershipChanged(const std::string& task_id);
  // Tears down an agent Space: closes its hidden window and drops the record.
  void CloseAgentSpace(const std::string& task_id);

  // --- Shadow windows ------------------------------------------------------
  ShadowWindow* FindShadowByWindow(BrowserId window_id);
  // The Mac's keep-alive contract, shared with agent Spaces: extends the
  // deadline to max(existing, now + ttl); ttl <= 0 is the default round TTL.
  void TouchShadow(ShadowWindow* shadow, int ttl_seconds, uint64_t now_ms);
  void CloseShadowWindow(const std::string& task_id, const char* reason);

  // --- The keep-alive sweep ------------------------------------------------
  // Completes and closes expired Spaces and shadows. The coordinator's sweep
  // runs this, then expires the creations in flight.
  void Sweep(uint64_t now_ms);

  // PhiTranscriptHost:
  std::vector<std::string> TranscriptLines(const std::string& task_id) override;
  std::string TranscriptAgentName(const std::string& task_id) override;
  bool PushTranscriptMessage(const std::string& task_id,
                             const std::string& text) override;

 private:
  // The controller of the agent Space `task_id`'s window, or null.
  PhiWindowController* ControllerFor(const std::string& task_id);

  RAW_PTR_EXCLUSION Host& host_;
  // Non-owning, across a module boundary, so not a raw_ptr. Null until Init.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  PhiAgentSpaces spaces_;
  // The agent Space whose window is currently surfaced for the user to watch
  // (empty = none). At most one is watched at a time.
  std::string watched_task_;
  // The shadow registry, keyed by the skill's taskId.
  std::map<std::string, ShadowWindow> shadows_;
  // The operating mask over the user's OWN tabs, fed by the framework's
  // three drive reports.
  PhiAgentDriveRegistry drives_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_AGENT_SPACE_MANAGER_H_
