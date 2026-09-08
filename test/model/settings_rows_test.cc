// Host tests for the settings panes' typed rows (docs/architecture-review.md
// §3.5 phase 6, lane 1 §1.3 and §5 D7: phi_settings_types.h). The rows the
// window reads are structs now; three of them still travel as TEXT, and
// those three are what is covered here — a round trip through each
// encoding, the refusals a malformed record gets, and the Shortcuts pane's
// row order, which used to live inside the preferences service where no
// host test could reach it. Run with test/model/run.sh.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_settings_types.h"

using namespace phi_client;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

void TestSplit() {
  const std::vector<std::string> parts = PhiSplitTabRow("a\tb\tc");
  CHECK(parts.size() == 3);
  CHECK(parts[0] == "a" && parts[1] == "b" && parts[2] == "c");
  // Empty fields are KEPT: a record whose middle field is blank must not
  // shift every field after it up one — the whole failure mode the typed
  // rows exist to end.
  const std::vector<std::string> holes = PhiSplitTabRow("a\t\tc\t");
  CHECK(holes.size() == 4);
  CHECK(holes[1].empty() && holes[2] == "c" && holes[3].empty());
  CHECK(PhiSplitTabRow("").size() == 1);
}

// agentSpace.settings.get answers `searchEngines` as these strings.
void TestSearchEngineWire() {
  const PhiSearchEngineRow row{"google.com", "Google", true};
  const std::string wire = PhiSearchEngineWireRow(row);
  CHECK(wire == "google.com\tGoogle\t1");
  PhiSearchEngineRow back;
  CHECK(PhiParseSearchEngineWireRow(wire, &back));
  CHECK(back.keyword == row.keyword);
  CHECK(back.name == row.name);
  CHECK(back.is_default == row.is_default);

  // Not the default, and a name holding a space.
  const PhiSearchEngineRow other{"ddg", "DuckDuckGo Lite", false};
  CHECK(PhiParseSearchEngineWireRow(PhiSearchEngineWireRow(other), &back));
  CHECK(back.name == "DuckDuckGo Lite" && !back.is_default);

  // Refusals: too few fields, or no keyword to name the engine by.
  CHECK(!PhiParseSearchEngineWireRow("google.com\tGoogle", &back));
  CHECK(!PhiParseSearchEngineWireRow("\tGoogle\t1", &back));
}

// The urlRules.* verbs read these rows.
void TestUrlRuleWire() {
  const model::UrlRule rule{"2", "*.example.com", "/docs", true};
  const std::string wire = PhiUrlRuleWireRow(rule);
  CHECK(wire == "2\t*.example.com\t/docs\t1");
  model::UrlRule back;
  CHECK(PhiParseUrlRuleWireRow(wire, &back));
  CHECK(back == rule);

  // An empty path prefix is the common case and must survive the hole.
  const model::UrlRule bare{"1", "github.com", "", false};
  CHECK(PhiParseUrlRuleWireRow(PhiUrlRuleWireRow(bare), &back));
  CHECK(back == bare);

  // A rule with no Space or no host routes nowhere; refuse it rather than
  // hand back a half-built rule.
  CHECK(!PhiParseUrlRuleWireRow("\tgithub.com\t\t0", &back));
  CHECK(!PhiParseUrlRuleWireRow("1\t\t\t0", &back));
  CHECK(!PhiParseUrlRuleWireRow("1\tgithub.com\t", &back));
}

// The consent FILE stores the audit trail as these lines, so this one has
// to survive a restart.
void TestCredentialAuditWire() {
  const PhiCredentialAuditRow row{"2026-09-08 11:04",
                                  model::CredentialAccessKind::kReveal,
                                  "github.com", "allowed (10 minutes)"};
  const std::string wire = PhiCredentialAuditWireRow(row);
  CHECK(wire == "2026-09-08 11:04\treveal\tgithub.com\tallowed (10 minutes)");
  PhiCredentialAuditRow back;
  CHECK(PhiParseCredentialAuditWireRow(wire, &back));
  CHECK(back.stamp == row.stamp);
  CHECK(back.kind == row.kind);
  CHECK(back.scope == row.scope);
  CHECK(back.decision == row.decision);

  const PhiCredentialAuditRow fill{"2026-09-08 11:05",
                                   model::CredentialAccessKind::kFill,
                                   "mail.example.com", "denied"};
  CHECK(PhiParseCredentialAuditWireRow(PhiCredentialAuditWireRow(fill), &back));
  CHECK(back.kind == model::CredentialAccessKind::kFill);
  CHECK(back.decision == "denied");

  // An unreadable kind must not WIDEN the exposure: anything that is not
  // the literal "reveal" reads back as a fill.
  CHECK(PhiParseCredentialAuditWireRow("s\tREVEAL\tscope\tdenied", &back));
  CHECK(back.kind == model::CredentialAccessKind::kFill);
  CHECK(PhiCredentialKindFromName("reveal") ==
        model::CredentialAccessKind::kReveal);
  CHECK(PhiCredentialKindFromName("") == model::CredentialAccessKind::kFill);
  CHECK(std::string(PhiCredentialKindName(
            model::CredentialAccessKind::kReveal)) == "reveal");

  CHECK(!PhiParseCredentialAuditWireRow("s\tfill\tscope", &back));
}

// The Shortcuts pane's shape: group order, and each group's rebindable rows
// ahead of its fixed, informational ones.
void TestShortcutRowOrder() {
  // A stand-in table: two File rows, one History row, one Tab row.
  const auto table = [](std::string_view group,
                        std::vector<PhiShortcutRow>* out) {
    if (group == "File") {
      out->push_back({"File", "New Tab", "Ctrl+T", "newTab", false});
      out->push_back({"File", "New Window", "Ctrl+N", "newWindow", true});
    } else if (group == "History") {
      out->push_back({"History", "Show Full History", "Ctrl+H",
                      "showFullHistory", false});
    } else if (group == "Tab") {
      out->push_back({"Tab", "Pin Tab", "Ctrl+P", "pinTab", false});
    }
  };
  const std::vector<PhiShortcutRow> rows = PhiBuildShortcutRows(table);

  // App leads, with its two fixed rows and no rebindable ones.
  CHECK(rows.size() == 2 + 2 + 1 + 2 + 1 + 2);
  CHECK(rows[0].group == "App" && rows[0].label == "Preferences");
  CHECK(rows[0].keys == "Ctrl+," && !rows[0].rebindable());
  CHECK(rows[1].label == "Quit Phi" && !rows[1].rebindable());

  // File's rebindable rows, in table order, carrying their names.
  CHECK(rows[2].group == "File" && rows[2].name == "newTab");
  CHECK(rows[2].rebindable() && !rows[2].customized);
  CHECK(rows[3].name == "newWindow" && rows[3].customized);

  // History: the rebindable row FIRST, then the two fixed ones.
  CHECK(rows[4].name == "showFullHistory");
  CHECK(rows[5].group == "History" && rows[5].label == "Back");
  CHECK(rows[5].keys == "Alt+Left" && !rows[5].rebindable());
  CHECK(rows[6].label == "Forward" && rows[6].keys == "Alt+Right");

  // Tab, same shape.
  CHECK(rows[7].name == "pinTab");
  CHECK(rows[8].label == "Select Next Tab" && !rows[8].rebindable());
  CHECK(rows[9].label == "Select Previous Tab");

  // A fixed row is never offered as customised, whatever the table says.
  for (const PhiShortcutRow& row : rows) {
    CHECK(row.rebindable() || !row.customized);
  }

  // Every group the pane names appears in the declared order; nothing the
  // table did not fill leaves a row behind.
  size_t at = 0;
  for (const PhiShortcutRow& row : rows) {
    while (at < kPhiShortcutGroups.size() &&
           kPhiShortcutGroups[at] != row.group) {
      ++at;
    }
    CHECK(at < kPhiShortcutGroups.size());
  }
  CHECK(PhiBuildShortcutRows([](std::string_view, std::vector<PhiShortcutRow>*) {
        }).size() == 6);  // The fixed rows alone.
}

}  // namespace

int main() {
  TestSplit();
  TestSearchEngineWire();
  TestUrlRuleWire();
  TestCredentialAuditWire();
  TestShortcutRowOrder();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("settings_rows_test: all passed\n");
  return EXIT_SUCCESS;
}
