// Host tests for the application model (docs/architecture-review.md §3.4,
// phase 3). Run with test/model/run.sh. The header is STL-only, so what is
// covered here is the defaults every store falls back to and the pure id
// helpers; the codecs (phi_app_model.cc) and PhiStore (phi_store.h) are
// over base::Value and Win32 and are exercised only in the client build.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_app_model.h"

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

Space MakeSpace(const char* id, const char* name = "") {
  Space space;
  space.id = id;
  space.name = *name ? name : (std::string("Space ") + id);
  return space;
}

// The defaults are the coordinator's member initialisers at 3447dd7 (the
// Mac's defaultValues); a drift here is a settings flip on upgrade.
void TestSettingsDefaults() {
  const PhiSettings s;
  CHECK(s.layout_mode == PhiLayoutMode::kPerformance);
  CHECK(s.sidebar_width == 240);
  CHECK(s.theme_id == "pure");
  CHECK(s.appearance == Appearance::kSystem);
  CHECK(s.saturation == 50);
  CHECK(s.new_tab_behavior == NewTabBehavior::kNewTabPage);
  CHECK(!s.always_show_full_url);
  CHECK(s.show_tab_previews);
  CHECK(s.peek_view_enabled);
  CHECK(s.auto_peek_view_enabled);
  CHECK(!s.restore_last_session);
  CHECK(!s.always_show_bookmark_bar);
  CHECK(!s.bookmark_bar_on_new_tab);
  CHECK(s.shortcut_overrides.empty());
  CHECK(s.reader.font_size == 19);
  CHECK(s.reader.width == "normal");
  CHECK(s.reader.theme == "matchBrowser");
  CHECK(s.reader.typeface == "serif");
  CHECK(s.reader.line_height == "normal");
  CHECK(s.reader.letter_spacing == "normal");
  CHECK(s.reader.shows_images);
  CHECK(s.reader.highlights_links);
  CHECK(s.language.empty());
  CHECK(!s.developer_mode);
  CHECK(!s.allow_agent_control);
  CHECK(!s.allow_agent_user_spaces);
  CHECK(!s.test_rig);
  CHECK(s.pin_scope == PhiPinScope::kProfile);
  // New in phase 3 and OFF until the user opts in.
  CHECK(!s.analytics_enabled);
  CHECK(s == PhiSettings());
}

void TestSpacesDefaults() {
  const PhiSpacesModel m;
  CHECK(m.spaces.size() == 1);
  CHECK(m.spaces[0].id == "1");
  CHECK(m.spaces[0].name == "Space 1");
  CHECK(m.spaces[0].profile_id == "Default");
  CHECK(m.spaces[0].icon == "phi:phi-icon-view-grid-add");
  CHECK(m.spaces[0].theme_id.empty());
  CHECK(m.spaces[0].saturation == -1);
  CHECK(m.spaces[0].color_hex.empty());
  CHECK(m.default_space_id == "1");
  CHECK(m.next_space_id == 2);
  CHECK(m.url_rules.empty());
  CHECK(m.url_rules_generation == 1);
}

void TestRestoreAndConsentDefaults() {
  const PhiRestoreModel r;
  CHECK(r.remembered.empty());
  CHECK(r.parked.empty());
  CHECK(r.last_active_space_id.empty());
  const RememberedWindow w;
  CHECK(w.window_id == 0);
  const PhiConsentModel c;
  CHECK(c.credential_audit.empty());
}

void TestEnumNames() {
  CHECK(std::string(AppearanceName(Appearance::kSystem)) == "system");
  CHECK(std::string(AppearanceName(Appearance::kLight)) == "light");
  CHECK(std::string(AppearanceName(Appearance::kDark)) == "dark");
  Appearance a = Appearance::kLight;
  CHECK(AppearanceFromName("dark", &a) && a == Appearance::kDark);
  CHECK(!AppearanceFromName("neon", &a) && a == Appearance::kDark);
  CHECK(std::string(NewTabBehaviorName(NewTabBehavior::kNewTabPage)) ==
        "newTabPage");
  CHECK(std::string(NewTabBehaviorName(NewTabBehavior::kOmnibox)) ==
        "omnibox");
  NewTabBehavior b = NewTabBehavior::kNewTabPage;
  CHECK(NewTabBehaviorFromName("omnibox", &b) && b == NewTabBehavior::kOmnibox);
  CHECK(!NewTabBehaviorFromName("", &b) && b == NewTabBehavior::kOmnibox);
  // Round trips.
  for (Appearance each : {Appearance::kSystem, Appearance::kLight,
                          Appearance::kDark}) {
    Appearance back = Appearance::kSystem;
    CHECK(AppearanceFromName(AppearanceName(each), &back) && back == each);
  }
}

void TestNextSpaceId() {
  CHECK(NextSpaceIdAfter({}) == 2);
  CHECK(NextSpaceIdAfter({MakeSpace("1")}) == 2);
  CHECK(NextSpaceIdAfter({MakeSpace("1"), MakeSpace("7"), MakeSpace("3")}) ==
        8);
  // Non-numeric ids (a Mac-created uuid) do not take part; a numeric
  // prefix counts as atoi did.
  CHECK(NextSpaceIdAfter({MakeSpace("abc"), MakeSpace("4")}) == 5);
  CHECK(NextSpaceIdAfter({MakeSpace("12x")}) == 13);
  // Never below 2, whatever the list says.
  CHECK(NextSpaceIdAfter({MakeSpace("0"), MakeSpace("-5")}) == 2);
}

void TestResolveDefaultSpaceId() {
  const std::vector<Space> spaces = {MakeSpace("1"), MakeSpace("2"),
                                     MakeSpace("9")};
  CHECK(ResolveDefaultSpaceId(spaces, "2") == "2");
  // A persisted holder that is gone falls back to "1" when that exists.
  CHECK(ResolveDefaultSpaceId(spaces, "4") == "1");
  CHECK(ResolveDefaultSpaceId(spaces, "") == "1");
  // ...else the first Space.
  const std::vector<Space> no_one = {MakeSpace("5"), MakeSpace("6")};
  CHECK(ResolveDefaultSpaceId(no_one, "6") == "6");
  CHECK(ResolveDefaultSpaceId(no_one, "7") == "5");
  // An empty list is the built-in default.
  CHECK(ResolveDefaultSpaceId({}, "3") == "1");
}

void TestUrlRuleWireIds() {
  CHECK(UrlRuleWireId(1, 0) == "g1-0");
  CHECK(UrlRuleWireId(17, 42) == "g17-42");
  size_t index = 99;
  CHECK(ParseUrlRuleWireId("g3-5", 3, &index) && index == 5);
  // A stale generation stops resolving: the caller must re-list.
  CHECK(!ParseUrlRuleWireId("g2-5", 3, &index));
  // The legacy bare-index probe still works.
  CHECK(ParseUrlRuleWireId("7", 3, &index) && index == 7);
  // Malformed.
  CHECK(!ParseUrlRuleWireId("g3-", 3, &index));
  CHECK(!ParseUrlRuleWireId("g3-x", 3, &index));
  CHECK(!ParseUrlRuleWireId("g3-5-1", 3, &index));
  CHECK(!ParseUrlRuleWireId("", 3, &index));
  CHECK(!ParseUrlRuleWireId("abc", 3, &index));
  // Round trip through the current generation.
  for (size_t i = 0; i < 5; ++i) {
    size_t back = 0;
    CHECK(ParseUrlRuleWireId(UrlRuleWireId(12, i), 12, &back) && back == i);
  }
}

void TestValueEquality() {
  PhiSpacesModel a;
  PhiSpacesModel b;
  CHECK(a == b);
  b.url_rules.push_back({"1", "github.com", "", false});
  CHECK(!(a == b));
  UrlRule r1{"1", "*.foo.com", "/x", true};
  UrlRule r2 = r1;
  CHECK(r1 == r2);
  r2.ask = false;
  CHECK(!(r1 == r2));
  ShortcutChord c1{65, true, false, false};
  ShortcutChord c2{65, true, false, false};
  CHECK(c1 == c2);
}

}  // namespace

int main() {
  TestSettingsDefaults();
  TestSpacesDefaults();
  TestRestoreAndConsentDefaults();
  TestEnumNames();
  TestNextSpaceId();
  TestResolveDefaultSpaceId();
  TestUrlRuleWireIds();
  TestValueEquality();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("app_model_test: all passed\n");
  return EXIT_SUCCESS;
}
