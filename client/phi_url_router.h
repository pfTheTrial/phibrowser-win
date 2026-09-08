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
// id read before a write stops resolving. Every write stores the CANONICAL
// pattern (NormalizeUrlRuleHost / NormalizeUrlRulePathPrefix below — the
// Mac's LocalStore.normalizedPathPrefix), so the routing table always
// compares against what GURL hands the throttle. A rule may target two
// ACTIONS beside the user's Spaces — Incognito (kPhiIncognitoRuleTargetId)
// and Kiosk (kPhiKioskRuleTargetId) — the Mac's incognitoRuleTargetId /
// kioskRuleTargetId; the Kiosk id is a wire value shared with the fork's
// phi::kKioskRuleTargetId and must never change. The ask prompt is
// DEFERRED: OnAskSpaceForURL arrives inside a navigation-throttle
// callback, and a modal loop there is the lane 1 §3.1 hazard — the request
// is queued and answered on a posted turn.
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

// --- Rule targets that are actions, not Spaces ---------------------------------
// Stable ids a rule (or the "ask first" chooser's pick) may carry instead
// of a Space id. Kiosk is shared with the fork's phi::kKioskRuleTargetId:
// the throttle resolves it itself (RouteKind::kKiosk). Incognito is the
// verb ladder's spelling, kept: the client resolves it to the profile's
// off-the-record window at route time.
inline constexpr char kPhiKioskRuleTargetId[] = "__phi_kiosk__";
inline constexpr char kPhiIncognitoRuleTargetId[] = "space.incognito";

inline bool UrlRuleTargetIsKiosk(std::string_view id) {
  return id == kPhiKioskRuleTargetId;
}
inline bool UrlRuleTargetIsIncognito(std::string_view id) {
  return id == kPhiIncognitoRuleTargetId;
}
inline bool UrlRuleTargetIsAction(std::string_view id) {
  return UrlRuleTargetIsKiosk(id) || UrlRuleTargetIsIncognito(id);
}

// --- Canonical pattern forms (host-tested) --------------------------------------

// The host as the router compares it: trimmed and ASCII-lowercased (GURL
// hosts are lowercase; the fork lowercases again on push, so this is about
// what the file and the editor show).
inline std::string NormalizeUrlRuleHost(std::string host) {
  size_t begin = 0;
  while (begin < host.size() &&
         (host[begin] == ' ' || host[begin] == '\t' || host[begin] == '\r' ||
          host[begin] == '\n')) {
    ++begin;
  }
  size_t end = host.size();
  while (end > begin &&
         (host[end - 1] == ' ' || host[end - 1] == '\t' ||
          host[end - 1] == '\r' || host[end - 1] == '\n')) {
    --end;
  }
  host = host.substr(begin, end - begin);
  for (char& c : host) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return host;
}

// The Mac's LocalStore.normalizedPathPrefix, step for step:
//   1. trim whitespace; empty -> "" (any path);
//   2. strip trailing slashes ("/foo/" == "/foo"), and a bare "/" becomes
//      "" — under the matcher's boundary check a sole "/" would match only
//      the root path, never "/foo", which is never what the user means;
//   3. ensure the leading "/" (GURL::path() always starts with one);
//   4. percent-DECODE then re-ENCODE, so "/résumé" and "/r%C3%A9sum%C3%A9"
//      store identically and the result is idempotent (a re-encode alone
//      would double-escape every "%" of an already-canonical prefix). A
//      literal "%" that is not a valid escape ("/100%complete") is kept as
//      typed by the decode and escaped to "%25" by the encode.
// The encode keeps exactly what URL path characters may appear unescaped
// (unreserved, sub-delims, ":" "@" "/") — Foundation's .urlPathAllowed,
// which is also what GURL leaves alone.
inline std::string NormalizeUrlRulePathPrefix(std::string raw) {
  auto is_space = [](char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  };
  size_t begin = 0;
  while (begin < raw.size() && is_space(raw[begin])) {
    ++begin;
  }
  size_t end = raw.size();
  while (end > begin && is_space(raw[end - 1])) {
    --end;
  }
  std::string s = raw.substr(begin, end - begin);
  if (s.empty()) {
    return std::string();
  }
  while (s.size() > 1 && s.back() == '/') {
    s.pop_back();
  }
  if (s == "/") {
    return std::string();
  }
  if (s.front() != '/') {
    s.insert(s.begin(), '/');
  }
  auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  // Decode only when EVERY "%" heads a valid escape — Foundation's
  // removingPercentEncoding leaves a string with a stray "%" untouched.
  bool decodable = true;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' &&
        (i + 2 >= s.size() || hex_value(s[i + 1]) < 0 ||
         hex_value(s[i + 2]) < 0)) {
      decodable = false;
      break;
    }
  }
  if (decodable && s.find('%') != std::string::npos) {
    std::string decoded;
    decoded.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '%') {
        decoded.push_back(static_cast<char>(hex_value(s[i + 1]) * 16 +
                                            hex_value(s[i + 2])));
        i += 2;
      } else {
        decoded.push_back(s[i]);
      }
    }
    s = std::move(decoded);
  }
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string encoded;
  encoded.reserve(s.size());
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    const bool allowed =
        (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
        (u >= '0' && u <= '9') || u == '-' || u == '.' || u == '_' ||
        u == '~' || u == '!' || u == '$' || u == '&' || u == '\'' ||
        u == '(' || u == ')' || u == '*' || u == '+' || u == ',' ||
        u == ';' || u == '=' || u == ':' || u == '@' || u == '/';
    if (allowed) {
      encoded.push_back(c);
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[u >> 4]);
      encoded.push_back(kHex[u & 0xF]);
    }
  }
  return encoded;
}

// --- The editor's match types (host-tested) -------------------------------------
// The Mac URLRulesEditor.MatchType: what a stored (host, pathPrefix) pair
// SHOWS as, and how a typed value under a chosen type ENCODES back. The
// type is the single source of truth — typed "*." / "*…*" sigils are
// stripped and the type's own form re-added, so the stored host is always
// one of the three wire forms.

enum class UrlRuleMatchType {
  kDomainSuffix,    // host + all subdomains ("*.host")
  kDomain,          // exact host only
  kDomainContains,  // host contains a substring ("*needle*")
  kUrl,             // exact host + path prefix
};

inline UrlRuleMatchType UrlRuleMatchTypeOf(const std::string& host,
                                           const std::string& path_prefix) {
  if (!path_prefix.empty()) {
    return UrlRuleMatchType::kUrl;
  }
  if (host.size() > 2 && host.front() == '*' && host.back() == '*') {
    return UrlRuleMatchType::kDomainContains;
  }
  if (host.rfind("*.", 0) == 0) {
    return UrlRuleMatchType::kDomainSuffix;
  }
  return UrlRuleMatchType::kDomain;
}

// The user-facing value: the stored pair minus the wire sigils (the Mac's
// MatchType.decode).
inline std::string UrlRuleDisplayValue(const std::string& host,
                                       const std::string& path_prefix) {
  switch (UrlRuleMatchTypeOf(host, path_prefix)) {
    case UrlRuleMatchType::kDomainSuffix:
      return host.substr(2);
    case UrlRuleMatchType::kDomainContains:
      return host.substr(1, host.size() - 2);
    case UrlRuleMatchType::kUrl:
      return host + path_prefix;
    case UrlRuleMatchType::kDomain:
      break;
  }
  return host;
}

// Drops a pasted ":8080" — GURL hosts never carry one. Only cuts when
// everything after the last ':' is digits, leaving IPv6 "[::1]" alone.
inline std::string StripUrlRulePort(const std::string& host) {
  const size_t colon = host.rfind(':');
  if (colon == std::string::npos || colon + 1 >= host.size()) {
    return host;
  }
  for (size_t i = colon + 1; i < host.size(); ++i) {
    if (host[i] < '0' || host[i] > '9') {
      return host;
    }
  }
  return host.substr(0, colon);
}

// The Mac's MatchType.encode: (type, typed value) -> the canonical stored
// (host, pathPrefix). The host-only types reduce the value to a bare host
// (a pasted full URL would otherwise leave the scheme or a "/" inside the
// stored host and the rule would silently never match); kUrl keeps the
// path, split at the first "/". An input that reduces to nothing (a bare
// "*.") yields an EMPTY host, which the caller drops.
inline void UrlRuleEncode(UrlRuleMatchType type,
                          std::string value,
                          std::string* host,
                          std::string* path_prefix) {
  host->clear();
  path_prefix->clear();
  value = NormalizeUrlRuleHost(std::move(value));  // Trim + lowercase.
  const size_t scheme = value.find("://");
  if (scheme != std::string::npos) {
    value = value.substr(scheme + 3);
  }
  if (type == UrlRuleMatchType::kUrl) {
    std::string host_part = value;
    std::string path_part;
    const size_t slash = value.find('/');
    if (slash != std::string::npos) {
      host_part = value.substr(0, slash);
      path_part = value.substr(slash);
    }
    if (host_part.rfind("*.", 0) == 0) {
      host_part = host_part.substr(2);
    }
    *host = StripUrlRulePort(host_part);
    *path_prefix = NormalizeUrlRulePathPrefix(path_part);
    return;
  }
  const size_t slash = value.find('/');
  if (slash != std::string::npos) {
    value = value.substr(0, slash);
  }
  value = StripUrlRulePort(value);
  switch (type) {
    case UrlRuleMatchType::kDomainSuffix: {
      const std::string bare =
          value.rfind("*.", 0) == 0 ? value.substr(2) : value;
      *host = bare.empty() ? std::string() : "*." + bare;
      break;
    }
    case UrlRuleMatchType::kDomain:
      *host = value.rfind("*.", 0) == 0 ? value.substr(2) : value;
      break;
    case UrlRuleMatchType::kDomainContains: {
      std::string needle = value;
      while (!needle.empty() && needle.front() == '*') {
        needle.erase(needle.begin());
      }
      while (!needle.empty() && needle.back() == '*') {
        needle.pop_back();
      }
      *host = needle.empty() ? std::string() : "*" + needle + "*";
      break;
    }
    case UrlRuleMatchType::kUrl:
      break;  // Handled above.
  }
}

// The match type a typed pattern implies (the verb ladder and a pasted
// value): an explicit path means URL, explicit sigils keep their form, and
// a bare host defaults to the Mac's domain suffix (host + subdomains).
inline UrlRuleMatchType UrlRuleMatchTypeForInput(const std::string& value) {
  std::string work = value;
  const size_t scheme = work.find("://");
  if (scheme != std::string::npos) {
    work = work.substr(scheme + 3);
  }
  const size_t slash = work.find('/');
  if (slash != std::string::npos && slash + 1 < work.size()) {
    return UrlRuleMatchType::kUrl;
  }
  if (slash != std::string::npos) {
    work = work.substr(0, slash);
  }
  if (work.size() > 2 && work.front() == '*' && work.back() == '*') {
    return UrlRuleMatchType::kDomainContains;
  }
  return UrlRuleMatchType::kDomainSuffix;  // Also the bare-host default.
}

// --- Pure rule operations (host-tested) ---------------------------------------
// Each write bumps the generation; each returns whether it changed anything.
// Every write stores the canonical forms above.

// A rule the router should compile: a host to match and somewhere to send
// it — a Space, Incognito or Kiosk. (Incognito rules used to be held back
// while the client had no Incognito windows to route into; the client
// resolves them now, see PhiUrlRouter::RouteUrlToSpace.)
inline bool UrlRuleIsLive(const model::UrlRule& rule) {
  return !rule.host.empty() && !rule.space_id.empty();
}

// The canonical copy of a rule as every write stores it.
inline model::UrlRule CanonicalUrlRule(const model::UrlRule& rule) {
  model::UrlRule out = rule;
  out.host = NormalizeUrlRuleHost(rule.host);
  out.path_prefix = NormalizeUrlRulePathPrefix(rule.path_prefix);
  return out;
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
  const model::UrlRule rule =
      CanonicalUrlRule({space_id, host, path_prefix, ask});
  if (rule.space_id.empty() || rule.host.empty()) {
    return false;
  }
  model->url_rules.push_back(rule);
  ++model->url_rules_generation;
  return true;
}

// Refused out of range or for an empty host.
inline bool SetUrlRulePatternIn(model::PhiSpacesModel* model,
                                int index,
                                const std::string& host,
                                const std::string& path_prefix) {
  const std::string canonical_host = NormalizeUrlRuleHost(host);
  if (index < 0 || index >= static_cast<int>(model->url_rules.size()) ||
      canonical_host.empty()) {
    return false;
  }
  model::UrlRule& rule = model->url_rules[static_cast<size_t>(index)];
  rule.host = canonical_host;
  rule.path_prefix = NormalizeUrlRulePathPrefix(path_prefix);
  ++model->url_rules_generation;
  return true;
}

// The universal editor's Save (the Mac's SpaceManager.setAllRules): the
// whole list at once, in the order the user authored — the vector order IS
// the sortOrder tiebreak. Rules that reduce to nothing (empty host after
// canonicalization, or no target) are dropped rather than stored inert.
// Returns false when the canonical result equals what is stored, in which
// case nothing moves — not even the generation.
inline bool ReplaceUrlRulesIn(model::PhiSpacesModel* model,
                              const std::vector<model::UrlRule>& rules) {
  std::vector<model::UrlRule> canonical;
  canonical.reserve(rules.size());
  for (const model::UrlRule& rule : rules) {
    model::UrlRule c = CanonicalUrlRule(rule);
    if (c.host.empty() || c.space_id.empty()) {
      continue;
    }
    canonical.push_back(std::move(c));
  }
  if (canonical == model->url_rules) {
    return false;
  }
  model->url_rules = std::move(canonical);
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
  // The universal editor's Save: the whole list, authored order. A no-op
  // list (canonically equal to what is stored) writes and pushes nothing.
  void ReplaceRules(const std::vector<model::UrlRule>& rules);
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
  // cold Space. The two ACTION targets take their own exits: Incognito
  // opens in the source profile's off-the-record window (an open one, else
  // a new one — the Mac's resolveIncognitoRouteTarget, created on demand);
  // Kiosk opens a fresh ephemeral Kiosk window.
  void RouteUrlToSpace(const std::string& space_id,
                       const std::string& url,
                       BrowserId source_window);

  // --- The framework's copy -------------------------------------------------
  // Compiles the live rules plus the Space -> VISIBLE browser map into the
  // framework's PhiURLRouter table — the body; the coordinator coalesces
  // the calls to one per turn. The Incognito target is aliased to the first
  // open Incognito shell's browser (the Mac's currentSpaceWindowMap alias)
  // so the fork routes straight into it; with none open the id stays
  // unmapped and the match comes back through OnRouteURLToSpace.
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
