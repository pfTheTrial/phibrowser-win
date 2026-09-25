// Host tests for the URL router's pure rule operations (docs/architecture-
// review.md §3.4, phase 4: phi_url_router.h). Run with test/model/run.sh.
// The header is host-clean, so what is covered here is the rule set's
// writes over the shared model — validation, the generation bump every
// write makes, wire-id resolution against the CURRENT generation, the
// action targets, the canonical pattern forms and the editor's match types,
// the whole-list replace and the Settings rows; the routing decisions
// themselves need the registry, the framework and a prompt and are
// exercised only in the client build.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_url_router.h"

using namespace phi_client;
using namespace phi_client::model;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

PhiSpacesModel ModelWithRules() {
  PhiSpacesModel m;
  m.url_rules = {{"1", "github.com", "", false, ""},
                 {"2", "*.example.com", "/docs", true, ""},
                 {"space.incognito", "*bank*", "", false, ""}};
  m.url_rules_generation = 5;
  return m;
}

void TestAdd() {
  PhiSpacesModel m;
  CHECK(m.url_rules_generation == 1);
  CHECK(AddUrlRuleTo(&m, "1", "github.com", "", false));
  CHECK(m.url_rules.size() == 1 && m.url_rules_generation == 2);
  // A new rule gets its stable id at once (the Mac's SpaceURLRule.id).
  CHECK(!m.url_rules[0].id.empty());
  CHECK(m.url_rules[0] ==
        UrlRule({"1", "github.com", "", false, m.url_rules[0].id}));
  CHECK(AddUrlRuleTo(&m, "2", "*.foo.com", "/p", true));
  CHECK(m.url_rules.size() == 2 && m.url_rules_generation == 3);
  // Refusals leave the model — generation included — untouched.
  CHECK(!AddUrlRuleTo(&m, "", "host", "", false));
  CHECK(!AddUrlRuleTo(&m, "1", "", "", false));
  CHECK(m.url_rules.size() == 2 && m.url_rules_generation == 3);
}

void TestSetPattern() {
  PhiSpacesModel m = ModelWithRules();
  CHECK(SetUrlRulePatternIn(&m, 0, "gitlab.com", "/x"));
  CHECK(m.url_rules[0].host == "gitlab.com" && m.url_rules[0].path_prefix == "/x");
  CHECK(m.url_rules[0].space_id == "1" && !m.url_rules[0].ask);  // Untouched.
  CHECK(m.url_rules_generation == 6);
  CHECK(!SetUrlRulePatternIn(&m, 3, "h", ""));   // Past the end.
  CHECK(!SetUrlRulePatternIn(&m, -1, "h", ""));  // Negative.
  CHECK(!SetUrlRulePatternIn(&m, 1, "", "/p"));  // Empty host.
  CHECK(m.url_rules_generation == 6);
}

void TestSetTarget() {
  PhiSpacesModel m = ModelWithRules();
  CHECK(SetUrlRuleTargetIn(&m, 1, "3", false));
  CHECK(m.url_rules[1].space_id == "3" && !m.url_rules[1].ask);
  CHECK(m.url_rules[1].host == "*.example.com");  // The pattern stays.
  CHECK(m.url_rules_generation == 6);
  CHECK(!SetUrlRuleTargetIn(&m, 1, "", true));
  CHECK(!SetUrlRuleTargetIn(&m, 7, "1", true));
  CHECK(m.url_rules_generation == 6);
}

void TestRemove() {
  PhiSpacesModel m = ModelWithRules();
  CHECK(RemoveUrlRuleFrom(&m, 1));
  CHECK(m.url_rules.size() == 2 && m.url_rules_generation == 6);
  CHECK(m.url_rules[1].space_id == "space.incognito");  // Shifted down.
  CHECK(!RemoveUrlRuleFrom(&m, 2));
  CHECK(!RemoveUrlRuleFrom(&m, -1));
  CHECK(m.url_rules.size() == 2 && m.url_rules_generation == 6);
}

// A deleted Space takes its rules with it; the generation moves either
// way, because the write to the shared file happened.
void TestDropForSpace() {
  PhiSpacesModel m = ModelWithRules();
  AddUrlRuleTo(&m, "2", "b.example.com", "", false);  // gen 6, two for "2".
  CHECK(DropUrlRulesForSpace(&m, "2") == 2);
  CHECK(m.url_rules.size() == 2 && m.url_rules_generation == 7);
  CHECK(m.url_rules[0].space_id == "1");
  CHECK(m.url_rules[1].space_id == "space.incognito");
  CHECK(DropUrlRulesForSpace(&m, "nobody") == 0);
  CHECK(m.url_rules_generation == 8);
}

// Wire ids resolve only against the CURRENT generation and only to an
// index that exists — a caller must re-list after any write.
void TestResolveWireId() {
  PhiSpacesModel m = ModelWithRules();  // gen 5, three rules.
  size_t index = 99;
  CHECK(ResolveUrlRuleIndex(m, UrlRuleWireId(5, 2), &index) && index == 2);
  CHECK(ResolveUrlRuleIndex(m, "1", &index) && index == 1);  // Bare index.
  CHECK(!ResolveUrlRuleIndex(m, UrlRuleWireId(4, 0), &index));  // Stale.
  CHECK(!ResolveUrlRuleIndex(m, UrlRuleWireId(5, 3), &index));  // Past end.
  CHECK(!ResolveUrlRuleIndex(m, "g5-", &index));
  CHECK(!ResolveUrlRuleIndex(m, "", &index));
  // After a write, yesterday's id is dead and today's works.
  const std::string before = UrlRuleWireId(m.url_rules_generation, 0);
  RemoveUrlRuleFrom(&m, 2);
  CHECK(!ResolveUrlRuleIndex(m, before, &index));
  CHECK(ResolveUrlRuleIndex(m, UrlRuleWireId(m.url_rules_generation, 0),
                            &index) &&
        index == 0);
}

void TestLiveAndRows() {
  const PhiSpacesModel m = ModelWithRules();
  CHECK(UrlRuleIsLive(m.url_rules[0]));
  CHECK(UrlRuleIsLive(m.url_rules[1]));
  CHECK(UrlRuleIsLive(m.url_rules[2]));  // Incognito routes now (client-resolved).
  CHECK(!UrlRuleIsLive(UrlRule({"1", "", "", false, ""})));
  CHECK(!UrlRuleIsLive(UrlRule({"", "h", "", false, ""})));
  const std::vector<std::string> rows = UrlRuleRowsOf(m.url_rules);
  CHECK(rows.size() == 3);
  CHECK(rows[0] == "1\tgithub.com\t\t0");
  CHECK(rows[1] == "2\t*.example.com\t/docs\t1");
  CHECK(rows[2] == "space.incognito\t*bank*\t\t0");  // Listed all the same.
  CHECK(UrlRuleRowsOf({}).empty());
}

// The action targets beside the Spaces: the Kiosk id is the fork's wire
// value and must never change.
void TestActionTargets() {
  CHECK(std::string(kPhiKioskRuleTargetId) == "__phi_kiosk__");
  CHECK(std::string(kPhiIncognitoRuleTargetId) == "space.incognito");
  CHECK(UrlRuleTargetIsKiosk("__phi_kiosk__"));
  CHECK(UrlRuleTargetIsIncognito("space.incognito"));
  CHECK(UrlRuleTargetIsAction("__phi_kiosk__"));
  CHECK(UrlRuleTargetIsAction("space.incognito"));
  CHECK(!UrlRuleTargetIsAction("1"));
  CHECK(!UrlRuleTargetIsAction(""));
}

// The Mac's normalizedPathPrefix, pinned by URLRouterTests.
void TestNormalizePathPrefix() {
  // The Mac's semantics: "/" is the ROOT only (not "any path"), and so is
  // a run of slashes; empty is any path.
  CHECK(NormalizeUrlRulePathPrefix("/") == "/");
  CHECK(NormalizeUrlRulePathPrefix("///") == "/");
  CHECK(NormalizeUrlRulePathPrefix("") == "");
  CHECK(NormalizeUrlRulePathPrefix("   ") == "");
  CHECK(NormalizeUrlRulePathPrefix("/foo/") == "/foo");
  CHECK(NormalizeUrlRulePathPrefix("foo") == "/foo");
  CHECK(NormalizeUrlRulePathPrefix("  /foo/bar/  ") == "/foo/bar");
  const std::string from_raw = NormalizeUrlRulePathPrefix("/r\xC3\xA9sum\xC3\xA9");
  const std::string from_encoded = NormalizeUrlRulePathPrefix("/r%C3%A9sum%C3%A9");
  CHECK(from_raw == "/r%C3%A9sum%C3%A9");
  CHECK(from_encoded == "/r%C3%A9sum%C3%A9");
  CHECK(NormalizeUrlRulePathPrefix(from_raw) == from_raw);  // Idempotent.
  CHECK(NormalizeUrlRulePathPrefix("/100%complete") == "/100%25complete");
  CHECK(NormalizeUrlRulePathPrefix("/a b") == "/a%20b");
  CHECK(NormalizeUrlRulePathPrefix("/a?b#c") == "/a%3Fb%23c");
  // Foundation's urlPathAllowed keeps every sub-delim but ';'.
  CHECK(NormalizeUrlRulePathPrefix("/ok-._~!$&'()*+,;=:@/x") ==
        "/ok-._~!$&'()*+,%3B=:@/x");
}

void TestNormalizeHost() {
  CHECK(NormalizeUrlRuleHost("  GitHub.COM ") == "github.com");
  CHECK(NormalizeUrlRuleHost("*.Example.com") == "*.example.com");
  CHECK(NormalizeUrlRuleHost("") == "");
}

// The editor's match types: decode a stored pair, encode a typed value.
void TestMatchTypes() {
  CHECK(UrlRuleMatchTypeOf("github.com", "") == UrlRuleMatchType::kDomain);
  CHECK(UrlRuleMatchTypeOf("*.github.com", "") ==
        UrlRuleMatchType::kDomainSuffix);
  CHECK(UrlRuleMatchTypeOf("*git*", "") == UrlRuleMatchType::kDomainContains);
  CHECK(UrlRuleMatchTypeOf("github.com", "/a") == UrlRuleMatchType::kUrl);
  CHECK(UrlRuleDisplayValue("*.github.com", "") == "github.com");
  CHECK(UrlRuleDisplayValue("*git*", "") == "git");
  CHECK(UrlRuleDisplayValue("github.com", "/a/b") == "github.com/a/b");
  CHECK(UrlRuleDisplayValue("github.com", "") == "github.com");

  std::string host, prefix;
  UrlRuleEncode(UrlRuleMatchType::kDomainSuffix, "https://Example.com/x", &host,
                &prefix);
  CHECK(host == "*.example.com" && prefix == "");
  UrlRuleEncode(UrlRuleMatchType::kDomainSuffix, "*.example.com:8080", &host,
                &prefix);
  CHECK(host == "*.example.com" && prefix == "");
  UrlRuleEncode(UrlRuleMatchType::kDomainSuffix, "*.", &host, &prefix);
  CHECK(host == "");  // Reduces to nothing: dropped by the caller.
  UrlRuleEncode(UrlRuleMatchType::kDomain, "*.www.example.com/", &host,
                &prefix);
  CHECK(host == "www.example.com" && prefix == "");
  UrlRuleEncode(UrlRuleMatchType::kDomainContains, "**git**", &host, &prefix);
  CHECK(host == "*git*" && prefix == "");
  UrlRuleEncode(UrlRuleMatchType::kDomainContains, "***", &host, &prefix);
  CHECK(host == "");
  UrlRuleEncode(UrlRuleMatchType::kUrl, "https://github.com/Anthropics/",
                &host, &prefix);
  CHECK(host == "github.com" && prefix == "/anthropics");
  UrlRuleEncode(UrlRuleMatchType::kUrl, "*.github.com:443/a b", &host, &prefix);
  CHECK(host == "github.com" && prefix == "/a%20b");
  UrlRuleEncode(UrlRuleMatchType::kUrl, "github.com", &host, &prefix);
  CHECK(host == "github.com" && prefix == "");
  CHECK(StripUrlRulePort("[::1]") == "[::1]");
  CHECK(StripUrlRulePort("h:80") == "h");
  CHECK(StripUrlRulePort("h:") == "h:");

  CHECK(UrlRuleMatchTypeForInput("example.com") ==
        UrlRuleMatchType::kDomainSuffix);
  CHECK(UrlRuleMatchTypeForInput("example.com/") ==
        UrlRuleMatchType::kDomainSuffix);
  CHECK(UrlRuleMatchTypeForInput("example.com/p") == UrlRuleMatchType::kUrl);
  CHECK(UrlRuleMatchTypeForInput("*x*") == UrlRuleMatchType::kDomainContains);
  CHECK(UrlRuleMatchTypeForInput("*.x") == UrlRuleMatchType::kDomainSuffix);
}

// Every write stores the canonical form.
void TestWritesCanonicalize() {
  PhiSpacesModel m;
  CHECK(AddUrlRuleTo(&m, "1", " GitHub.com ", "docs/", false));
  CHECK(m.url_rules[0].host == "github.com");
  CHECK(m.url_rules[0].path_prefix == "/docs");
  CHECK(!AddUrlRuleTo(&m, "1", "   ", "", false));  // Whitespace = no host.
  CHECK(SetUrlRulePatternIn(&m, 0, "*.Example.COM", "/r\xC3\xA9sum\xC3\xA9/"));
  CHECK(m.url_rules[0].host == "*.example.com");
  CHECK(m.url_rules[0].path_prefix == "/r%C3%A9sum%C3%A9");
  CHECK(!SetUrlRulePatternIn(&m, 0, " ", "/p"));
}

// The editor's Save: the whole list at once, canonical, empties dropped, a
// no-op when nothing changed.
void TestReplace() {
  PhiSpacesModel m = ModelWithRules();  // gen 5.
  CHECK(!ReplaceUrlRulesIn(&m, m.url_rules));  // Same list: untouched.
  CHECK(m.url_rules_generation == 5);
  std::vector<UrlRule> next = {{"2", "*.Example.com", "/docs/", true, ""},
                               {"", "dropped.com", "", false, ""},
                               {"1", "", "", false, ""},
                               {"__phi_kiosk__", "news.com", "", false, ""}};
  CHECK(ReplaceUrlRulesIn(&m, next));
  CHECK(m.url_rules_generation == 6);
  CHECK(m.url_rules.size() == 2);
  // The editor's new rows get ids as they are stored.
  CHECK(!m.url_rules[0].id.empty() && !m.url_rules[1].id.empty() &&
        m.url_rules[0].id != m.url_rules[1].id);
  CHECK(m.url_rules[0] ==
        UrlRule({"2", "*.example.com", "/docs", true, m.url_rules[0].id}));
  CHECK(m.url_rules[1] ==
        UrlRule({"__phi_kiosk__", "news.com", "", false, m.url_rules[1].id}));
  // Saving the list it read back (ids and all) is canonically equal.
  const std::vector<UrlRule> stored = m.url_rules;
  CHECK(!ReplaceUrlRulesIn(&m, stored));
  CHECK(m.url_rules_generation == 6);
  // Rule ids resolve; so does the old generation spelling.
  size_t at = 9;
  CHECK(ResolveUrlRuleIndex(m, m.url_rules[1].id, &at) && at == 1);
  CHECK(!ResolveUrlRuleIndex(m, "no-such-rule", &at));
  CHECK(ReplaceUrlRulesIn(&m, {}));
  CHECK(m.url_rules.empty() && m.url_rules_generation == 7);
}

}  // namespace

int main() {
  TestAdd();
  TestActionTargets();
  TestNormalizePathPrefix();
  TestNormalizeHost();
  TestMatchTypes();
  TestWritesCanonicalize();
  TestReplace();
  TestSetPattern();
  TestSetTarget();
  TestRemove();
  TestDropForSpace();
  TestResolveWireId();
  TestLiveAndRows();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("url_router_test: all passed\n");
  return EXIT_SUCCESS;
}
