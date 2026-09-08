// Host tests for the URL router's pure rule operations (docs/architecture-
// review.md §3.4, phase 4: phi_url_router.h). Run with test/model/run.sh.
// The header is host-clean, so what is covered here is the rule set's
// writes over the shared model — validation, the generation bump every
// write makes, wire-id resolution against the CURRENT generation, the
// incognito exclusion from the live router and the Settings rows; the
// routing decisions themselves need the registry, the framework and a
// prompt and are exercised only in the client build.

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
  m.url_rules = {{"1", "github.com", "", false},
                 {"2", "*.example.com", "/docs", true},
                 {"space.incognito", "*bank*", "", false}};
  m.url_rules_generation = 5;
  return m;
}

void TestAdd() {
  PhiSpacesModel m;
  CHECK(m.url_rules_generation == 1);
  CHECK(AddUrlRuleTo(&m, "1", "github.com", "", false));
  CHECK(m.url_rules.size() == 1 && m.url_rules_generation == 2);
  CHECK(m.url_rules[0] == UrlRule({"1", "github.com", "", false}));
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
  CHECK(!UrlRuleIsLive(m.url_rules[2]));  // Incognito: stored, not routed.
  const std::vector<std::string> rows = UrlRuleRowsOf(m.url_rules);
  CHECK(rows.size() == 3);
  CHECK(rows[0] == "1\tgithub.com\t\t0");
  CHECK(rows[1] == "2\t*.example.com\t/docs\t1");
  CHECK(rows[2] == "space.incognito\t*bank*\t\t0");  // Listed all the same.
  CHECK(UrlRuleRowsOf({}).empty());
}

}  // namespace

int main() {
  TestAdd();
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
