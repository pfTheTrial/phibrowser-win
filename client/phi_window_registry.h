// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_WINDOW_REGISTRY_H_
#define PHIBROWSER_WIN_CLIENT_PHI_WINDOW_REGISTRY_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_ids.h"

namespace phi_client {

class PhiWindowController;
class PhiKioskWindow;

// What the client asked the framework for and has not yet been handed
// (docs/architecture-review.md §3.4, the PhiWindowRegistry row). Filed under
// the RequestBrowser token and taken back by the announcement that echoes
// it — never by "the next window that arrives", which is how two cold
// switches used to cross (lane 1 F5). The fields are a union over the
// requestable kinds; each kind reads the ones it needs.
struct PendingCreation {
  phi_framework::WindowKind kind = phi_framework::WindowKind::kSpace;
  // kSpace: the slot the browser attaches to. INVALID means "host it as a
  // new shell" — the default-shell rescue and a slot that closed while the
  // request was in flight both land there.
  SlotId slot;
  std::string space_id;    // kSpace: the Space the browser becomes.
  std::string profile_id;  // What was asked for, for the log.
  // kSpace: opened once the browser is attached, bypassing URL routing
  // (Open Link in Space / a routed URL into a cold Space, and a re-profiled
  // Space's tabs). One list: the order is the order they were asked in.
  std::vector<std::string> open_urls;
  std::string task_id;     // kAgent / kShadow: the skill's taskId.
  std::string agent_name;  // kAgent: the driver's name for the watch pill.
  bool persistent = false;  // kAgent
  // A deferred control reply (PhiCoordinator::RegisterPendingReply) that
  // the announcement completes; 0 when nobody is waiting.
  int pending_reply_id = 0;
  // The tick (GetTickCount64 ms) after which the request is presumed lost:
  // the framework has no failure callback for a request that produced no
  // window, so the sweep completes any waiting reply with create_timeout.
  uint64_t deadline_tick = 0;
};

// The client's window bookkeeping, and nothing else: which shell SLOTS
// exist, which BROWSER is hosted in which slot, the kiosks (no slot), and
// the creations in flight. Pure data — no framework calls, no Win32 — so it
// compiles on the Linux host for test/model/window_registry_test.cc, which
// is why the two window types are only forward-declared and owned through
// a deleter captured where they are complete (Add / AddKiosk).
class PhiWindowRegistry {
 public:
  using ControllerPtr =
      std::unique_ptr<PhiWindowController, void (*)(PhiWindowController*)>;
  using KioskPtr = std::unique_ptr<PhiKioskWindow, void (*)(PhiKioskWindow*)>;

  // One slot. `agent` / `incognito` are fixed at construction, so they are
  // recorded here rather than read back through the (incomplete) controller.
  struct Slot {
    ControllerPtr controller;
    bool agent = false;
    bool incognito = false;
  };

  PhiWindowRegistry();
  ~PhiWindowRegistry();
  PhiWindowRegistry(const PhiWindowRegistry&) = delete;
  PhiWindowRegistry& operator=(const PhiWindowRegistry&) = delete;

  // --- Slots -----------------------------------------------------------
  // Takes ownership; returns the raw controller for the caller's follow-up.
  // A template so the delete is instantiated where `Controller` is complete
  // (the coordinator, or a test stub), never in this header.
  template <typename Controller>
  PhiWindowController* Add(SlotId slot,
                           std::unique_ptr<Controller> controller,
                           bool agent,
                           bool incognito) {
    return AddOwned(slot,
                    ControllerPtr(controller.release(),
                                  [](PhiWindowController* c) {
                                    delete static_cast<Controller*>(c);
                                  }),
                    agent, incognito);
  }
  // Destroys the slot's controller and drops every browser bound to it.
  // False when no such slot.
  bool Remove(SlotId slot);
  PhiWindowController* Get(SlotId slot) const;
  const Slot* Find(SlotId slot) const;
  bool Has(SlotId slot) const { return slots_.count(slot) > 0; }
  bool empty() const { return slots_.empty(); }
  size_t size() const { return slots_.size(); }
  // Every slot, lowest id first.
  const std::map<SlotId, Slot>& slots() const { return slots_; }
  // Every controller, lowest slot first — a COPY, because callers routinely
  // close a slot (and so destroy its controller) mid-walk.
  std::vector<PhiWindowController*> Controllers() const;
  // The USER's shells: neither agent nor incognito, lowest slot first.
  std::vector<PhiWindowController*> UserControllers() const;
  // The lowest-id user slot — THE place the "slot 0 / no windowId means the
  // window" convention resolves. Invalid when the user has no shell.
  SlotId FirstUserSlot() const;
  PhiWindowController* FirstUserController() const;
  bool IsAgentSlot(SlotId slot) const;
  bool IsIncognitoSlot(SlotId slot) const;

  // --- Browsers ----------------------------------------------------------
  // The slot hosting `browser` right now, or nullopt: the browser closed,
  // is a kiosk / surface panel, or was never bound.
  std::optional<SlotId> SlotFor(BrowserId browser) const;
  // SlotFor + Get in one step.
  PhiWindowController* ControllerForBrowser(BrowserId browser) const;
  // The one resolver for a `windowId` that came over the wire or from a
  // record that predates the slot/browser split: a slot id, else the id of
  // a browser bound to a slot (a Space browser attached after the slot
  // opened — what spaces.list reports as windowIds), else 0 = the first
  // user slot. Null when nothing matches.
  PhiWindowController* Resolve(int64_t window_id) const;
  void Bind(BrowserId browser, SlotId slot);
  void Unbind(BrowserId browser);
  bool IsBound(BrowserId browser) const;
  // Every bound browser, in id order.
  std::vector<BrowserId> Browsers() const;

  // --- Kiosks ------------------------------------------------------------
  template <typename Kiosk>
  PhiKioskWindow* AddKiosk(BrowserId browser, std::unique_ptr<Kiosk> kiosk) {
    return AddKioskOwned(
        browser, KioskPtr(kiosk.release(), [](PhiKioskWindow* k) {
          delete static_cast<Kiosk*>(k);
        }));
  }
  bool RemoveKiosk(BrowserId browser);
  PhiKioskWindow* Kiosk(BrowserId browser) const;

  // --- Pending creations -------------------------------------------------
  // Files a RequestBrowser request under its token. A second request under
  // the same token replaces the first (the framework never reuses tokens).
  void AddPending(phi_framework::RequestToken token, PendingCreation creation);
  // The request the announcement carrying `token` completes, removed from
  // the queue; nullopt when the token is unknown or already expired.
  std::optional<PendingCreation> TakePending(phi_framework::RequestToken token);
  // The kSpace request already in flight for (slot, space), so a repeat
  // switch adds to it instead of asking for a second browser. Mutable so
  // the caller can append open_urls.
  PendingCreation* FindPendingSpace(SlotId slot, const std::string& space_id);
  // Whether a kAgent / kShadow request for `task_id` is in flight.
  bool HasPendingTask(const std::string& task_id) const;
  // A ghost materialization (MaterializeGhostWindow) is announced with the
  // PREVIOUS session's window id, not a token — that id is its identity.
  void AddPendingGhost(BrowserId restored_from, PendingCreation creation);
  std::optional<PendingCreation> TakePendingGhost(BrowserId restored_from);
  bool HasPending() const {
    return !pending_.empty() || !pending_ghosts_.empty();
  }
  // Removes and returns every request whose deadline has passed.
  std::vector<PendingCreation> ExpirePending(uint64_t now_tick);

 private:
  PhiWindowController* AddOwned(SlotId slot,
                                ControllerPtr controller,
                                bool agent,
                                bool incognito);
  PhiKioskWindow* AddKioskOwned(BrowserId browser, KioskPtr kiosk);

  std::map<SlotId, Slot> slots_;
  std::map<BrowserId, SlotId> slot_of_browser_;
  std::map<BrowserId, KioskPtr> kiosks_;
  std::map<phi_framework::RequestToken, PendingCreation> pending_;
  std::map<BrowserId, PendingCreation> pending_ghosts_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_WINDOW_REGISTRY_H_
