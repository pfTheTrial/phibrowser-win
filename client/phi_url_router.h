// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_URL_ROUTER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_URL_ROUTER_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_settings_types.h"
#include "phibrowser-win/client/phi_window_registry.h"

// The Space URL routing service (docs/architecture-review.md §3.4, the
// PhiUrlRouter row): the rules half of PhiSpaces.json, the routing
// decisions the framework hands back (Open Link in Space, a silent rule
// into a cold Space, the "ask first" prompt) and the framework's compiled
// PhiURLRouter table.
//
// THE MODEL IS SHARED: Spaces and rules are one file, so the router does
// not own a model — it holds a reference to the space manager's
// model::PhiSpacesModel and writes ONLY `url_rules` and
// `url_rules_generation` there (the list is the manager's; the router reads
// it for names and profiles). Both services mark the same store dirty; the
// coordinator coalesces the write.
//
// INVARIANTS. Rule ids ("g<gen>-<index>") regenerate on every write, so an
// id read before a write stops resolving. Incognito rules
// ("space.incognito") are stored and listed but stay OUT of the live
// router. The ask prompt is DEFERRED: OnAskSpaceForURL arrives inside a
// navigation-throttle callback, and a modal loop there is the lane 1 §3.1
// hazard — the request is queued and answered on a posted turn.
//
// HOST-CLEAN ON PURPOSE: STL plus the model, ids and registry headers the
// host tests already compile — no windows.h, and of base/ only the
// RAW_PTR_EXCLUSION macro (the host build shims it, test/model/stubs) — so
// the pure rule operations below are host-tested
// (test/model/url_router_test.cc).

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

// --- Pure rule operations (host-tested) ---------------------------------------
// Each write bumps the generation; each returns whether it changed anything.

// Incognito rules stay out of the live router until the client grows
// Incognito Spaces — a match would cancel the navigation with nowhere to
// send it.
inline bool UrlRuleIsLive(const model::UrlRule& rule) {
  return rule.space_id != "space.incognito";
}

// The urlRules.* verbs' wire encoding of the rule list
// (PhiUrlRuleWireRow, phi_settings_types.h). The SETTINGS pane takes
// RulesWithIds() instead — structs carrying each rule's wire id.
inline std::vector<std::string> UrlRuleRowsOf(
    const std::vector<model::UrlRule>& rules) {
  std::vector<std::string> rows;
  rows.reserve(rules.size());
  for (const model::UrlRule& rule : rules) {
    rows.push_back(PhiUrlRuleWireRow(rule));
  }
  return rows;
}

// Refused for an empty Space id or host.
inline bool AddUrlRuleTo(model::PhiSpacesModel* model,
                         const std::string& space_id,
                         const std::string& host,
                         const std::string& path_prefix,
                         bool ask) {
  if (space_id.empty() || host.empty()) {
    return false;
  }
  model->url_rules.push_back({space_id, host, path_prefix, ask});
  ++model->url_rules_generation;
  return true;
}

// Refused out of range or for an empty host.
inline bool SetUrlRulePatternIn(model::PhiSpacesModel* model,
                                int index,
                                const std::string& host,
                                const std::string& path_prefix) {
  if (index < 0 || index >= static_cast<int>(model->url_rules.size()) ||
      host.empty()) {
    return false;
  }
  model::UrlRule& rule = model->url_rules[static_cast<size_t>(index)];
  rule.host = host;
  rule.path_prefix = path_prefix;
  ++model->url_rules_generation;
  return true;
}

// Refused out of range or for an empty Space id.
inline bool SetUrlRuleTargetIn(model::PhiSpacesModel* model,
                               int index,
                               const std::string& space_id,
                               bool ask) {
  if (index < 0 || index >= static_cast<int>(model->url_rules.size()) ||
      space_id.empty()) {
    return false;
  }
  model::UrlRule& rule = model->url_rules[static_cast<size_t>(index)];
  rule.space_id = space_id;
  rule.ask = ask;
  ++model->url_rules_generation;
  return true;
}

inline bool RemoveUrlRuleFrom(model::PhiSpacesModel* model, int index) {
  if (index < 0 || index >= static_cast<int>(model->url_rules.size())) {
    return false;
  }
  model->url_rules.erase(model->url_rules.begin() + index);
  ++model->url_rules_generation;
  return true;
}

// Drops every rule targeting `space_id` (a deleted Space). The generation
// moves whether or not any rule went — the write happened.
inline size_t DropUrlRulesForSpace(model::PhiSpacesModel* model,
                                   std::string_view space_id) {
  const size_t dropped = std::erase_if(
      model->url_rules,
      [&](const model::UrlRule& r) { return r.space_id == space_id; });
  ++model->url_rules_generation;
  return dropped;
}

// Resolves a wire id against the model's CURRENT generation to an index
// that exists. False for a stale generation, a malformed id or an index
// past the list — the Mac contract that callers re-list before update or
// delete.
inline bool ResolveUrlRuleIndex(const model::PhiSpacesModel& model,
                                std::string_view wire_id,
                                size_t* index) {
  size_t parsed = 0;
  if (!model::ParseUrlRuleWireId(wire_id, model.url_rules_generation,
                                 &parsed) ||
      parsed >= model.url_rules.size()) {
    return false;
  }
  *index = parsed;
  return true;
}

// --- The service --------------------------------------------------------------

class PhiUrlRouter {
 public:
  // What routing needs from the rest of the app (the coordinator
  // implements it): browser creation for a cold Space, the coalesced
  // pushes, and the posted turn the ask prompt runs on.
  class Host {
   public:
    // Asks the framework for a browser in `profile_id` carrying `pending`
    // (a cold Space's routed link opens once it attaches) — the
    // coordinator's RequestCreation for WindowKind::kSpace.
    virtual void RequestSpaceBrowser(const std::string& profile_id,
                                     PendingCreation pending) = 0;
    // The routing table is due a (coalesced) push — every rule write.
    virtual void SchedulePushRoutingTable() = 0;
    // The Space menu (and with it the table) is due a push — the visible-
    // Space map changed under a route.
    virtual void SchedulePushSpaceMenu() = 0;
    // An ask is queued: post RunPendingAsk() to the next turn.
    virtual void PostAskSpace() = 0;

   protected:
    virtual ~Host() = default;
  };

  // A mutation writes nothing itself: it calls `dirty`, and the owner folds
  // the write into its coalesced-save timer.
  using DirtyCallback = std::function<void()>;
  // `model` is the space manager's; every reference outlives the service.
  PhiUrlRouter(model::PhiSpacesModel& model,
               PhiWindowRegistry& registry,
               phi_framework::PhiFramework& framework,
               Host& host,
               DirtyCallback dirty);
  ~PhiUrlRouter();

  PhiUrlRouter(const PhiUrlRouter&) = delete;
  PhiUrlRouter& operator=(const PhiUrlRouter&) = delete;

  // --- Rules (the Settings host's spellings and the urlRules.* verbs' ops) --
  const std::vector<model::UrlRule>& rules() const { return model_.url_rules; }
  int generation() const { return model_.url_rules_generation; }
  // ListRules: the wire rows, and the wire id of the rule at `index`.
  std::vector<std::string> RuleRows() const {
    return UrlRuleRowsOf(model_.url_rules);
  }
  std::string RuleWireId(size_t index) const {
    return model::UrlRuleWireId(model_.url_rules_generation, index);
  }
  bool ResolveRuleIndex(std::string_view wire_id, size_t* index) const {
    return ResolveUrlRuleIndex(model_, wire_id, index);
  }
  // Every rule with its CURRENT wire id — what the Settings editor reads
  // and what its edits name back. Position is not identity here: a write
  // bumps the generation and every id with it, so an index the pane read
  // before an edit would land on whatever moved into that slot (lane 1 §5
  // D7). Reject a stale id instead of retargeting.
  std::vector<PhiUrlRuleRow> RulesWithIds() const;
  // Each write persists and re-pushes the routing table at once.
  void AddRule(const std::string& space_id,
               const std::string& host,
               const std::string& path_prefix,
               bool ask);
  void SetRulePattern(int index,
                      const std::string& host,
                      const std::string& path_prefix);
  void SetRuleTarget(int index, const std::string& space_id, bool ask);
  void RemoveRule(int index);
  // The same three writes addressed by wire id. False when the id no
  // longer names a rule (a prior generation, or a list that shrank) —
  // the caller re-lists rather than hitting a neighbour.
  bool SetRulePatternById(const std::string& rule_id,
                          const std::string& host,
                          const std::string& path_prefix);
  bool SetRuleTargetById(const std::string& rule_id,
                         const std::string& space_id,
                         bool ask);
  bool RemoveRuleById(const std::string& rule_id);
  // A Space is going: its rules go with it (the space manager's DeleteSpace,
  // through the coordinator).
  void DropRulesForSpace(const std::string& space_id);

  // --- Routing decisions (the framework delegate's three callbacks) --------
  // The page context menu's "Open Link in Space".
  void OnOpenLinkInSpace(int64_t source_window_id,
                         const char* space_id,
                         const char* url);
  // A silent rule matched but the target Space had no visible browser; the
  // navigation is already cancelled, so surface the Space and re-open the
  // URL there (never silently dropped).
  void OnRouteURLToSpace(const char* space_id,
                         const char* url,
                         int64_t source_window_id);
  // An "ask first" rule matched: queue the chooser for the next turn (see
  // the header comment). `source_is_new_tab` says the cancelled navigation
  // was replacing a fresh new tab, which the answer re-issues in place.
  void OnAskSpaceForURL(const char* url,
                        const char* default_space_id,
                        int64_t source_window_id,
                        bool source_is_new_tab);
  // The posted half of OnAskSpaceForURL: runs ONE queued chooser (modal)
  // and answers it — the chosen Space through RouteUrlToSpace, "keep it
  // here" through a routing-bypassing open in the source window.
  void RunPendingAsk();
  // Surfaces `space_id` (cold-spawning its browser if needed) and opens
  // `url` there with a one-shot routing bypass — the shared tail of the
  // context menu's "Open Link in Space", the silent auto-route into a
  // hidden/cold Space, the "ask first" prompt's pick and a tab's move to a
  // cold Space.
  void RouteUrlToSpace(const std::string& space_id,
                       const std::string& url,
                       BrowserId source_window);

  // --- The framework's copy -------------------------------------------------
  // Compiles the live rules plus the Space -> VISIBLE browser map into the
  // framework's PhiURLRouter table — the body; the coordinator coalesces
  // the calls to one per turn.
  void PushRoutingTableNow();

 private:
  struct PendingAsk {
    std::string url;
    std::string default_space_id;
    int64_t source_window_id = 0;
    bool source_is_new_tab = false;
  };
  void MarkDirty();

  // Non-owning; all outlive the service (see the constructor). The
  // framework is a singleton across a module boundary.
  RAW_PTR_EXCLUSION model::PhiSpacesModel& model_;
  RAW_PTR_EXCLUSION PhiWindowRegistry& registry_;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework& framework_;
  RAW_PTR_EXCLUSION Host& host_;
  DirtyCallback dirty_;
  // Asks queued for their posted turn, in arrival order.
  std::deque<PendingAsk> pending_asks_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_URL_ROUTER_H_
