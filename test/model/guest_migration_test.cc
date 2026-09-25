// Host tests for the Guest → account migration (client/phi_guest_migration.h),
// the Mac's GuestDataMigration planner and import. Run with test/model/run.sh.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "phibrowser-win/client/phi_guest_migration.h"
#include "phibrowser-win/client/phi_local_store.h"

using namespace phi_client::store;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

struct FakeEnv {
  TimeMs now = 1'700'000'000'000;
  int next_uuid = 0;
  std::string prefix;
  Env Make() {
    return Env{[this] { return now; },
               [this] {
                 char text[48];
                 std::snprintf(text, sizeof(text), "%s-0000-4000-8000-%012d",
                               prefix.c_str(), ++next_uuid);
                 return std::string(text);
               }};
  }
};

std::string TempDir(const char* name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("phi_guest_migration_test_" + std::to_string(::getpid()) + "_" + name);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir.string();
}

UrlRuleDraft Rule(const std::string& id,
                  const std::string& space,
                  const std::string& host) {
  UrlRuleDraft draft;
  draft.id = id;
  draft.space_id = space;
  draft.content = UrlRuleDraft::Content{host, std::nullopt, false};
  return draft;
}

// A Guest with the default Space and "Work", bookmarks in both (a folder
// with a child in the default one), two profile pins and three rules.
void SeedGuest(LocalStore& guest) {
  CHECK(guest.EnsureDefaultSpace(kDefaultProfileId));
  CHECK(guest.CreateSpace("W", kDefaultProfileId, "Work", "#112233",
                          "phi:work"));
  LocalStore::BookmarkCreate top;
  top.profile_id = kDefaultProfileId;
  top.guid = "gb-top";
  top.title = "Top";
  top.url = "https://top.example/";
  CHECK(!guest.CreateBookmark(top).empty());
  const std::string folder = guest.CreateFolder(
      kDefaultProfileId, kDefaultSpaceId, std::nullopt, std::nullopt, "Dir",
      "gb-dir");
  CHECK(folder == "gb-dir");
  LocalStore::BookmarkCreate child;
  child.profile_id = kDefaultProfileId;
  child.parent_guid = folder;
  child.guid = "gb-child";
  child.title = "Child";
  child.url = "https://child.example/";
  CHECK(!guest.CreateBookmark(child).empty());
  LocalStore::BookmarkCreate work;
  work.profile_id = kDefaultProfileId;
  work.space_id = "W";
  work.guid = "gb-work";
  work.title = "Work link";
  work.url = "https://work.example/";
  CHECK(!guest.CreateBookmark(work).empty());
  LocalStore::PinCreate pin;
  pin.profile_id = kDefaultProfileId;
  pin.guid = "gp-1";
  pin.url = "https://mail.example/";
  pin.title = "Mail";
  CHECK(!guest.CreatePin(pin).empty());
  pin.guid = "gp-2";
  pin.url = "https://chat.example/";
  pin.title = "Chat";
  CHECK(!guest.CreatePin(pin).empty());
  CHECK(guest.ApplyUrlRuleEdits({Rule("r-default", kDefaultSpaceId,
                                      "news.example"),
                                 Rule("r-work", "W", "jira.example"),
                                 Rule("r-dup", "W", "shared.example")},
                                {}) == WriteError::kNone);
}

void TestPlanImportVerify() {
  FakeEnv guest_env{.prefix = "aaaaaaaa"};
  FakeEnv target_env{.prefix = "bbbbbbbb"};
  LocalStore guest(guest_env.Make());
  LocalStore target(target_env.Make());
  CHECK(guest.Open(TempDir("guest")) == LocalStore::OpenResult::kOk);
  CHECK(target.Open(TempDir("target")) == LocalStore::OpenResult::kOk);
  SeedGuest(guest);
  // The target already has its default Space, one bookmark, one pin, a
  // Space "W2" and the rule "shared.example" (under the guest's W id, which
  // it also holds — so the guest's W is renamed).
  CHECK(target.EnsureDefaultSpace(kDefaultProfileId));
  CHECK(target.CreateSpace("W", kDefaultProfileId, "Theirs", "#000000",
                           "phi:x"));
  LocalStore::BookmarkCreate existing;
  existing.profile_id = kDefaultProfileId;
  existing.guid = "tb-1";
  existing.title = "Existing";
  existing.url = "https://existing.example/";
  CHECK(!target.CreateBookmark(existing).empty());
  LocalStore::PinCreate tpin;
  tpin.profile_id = kDefaultProfileId;
  tpin.guid = "tp-1";
  tpin.url = "https://theirs.example/";
  tpin.title = "Theirs";
  CHECK(!target.CreatePin(tpin).empty());

  GuestThemes guest_themes;
  guest_themes.theme_ids = {{"W", "mint"}, {kDefaultSpaceId, "iris"},
                            {"gone", "coral"}};
  guest_themes.saturations = {{"W", 40}};
  GuestSnapshot snapshot;
  std::string error;
  CHECK(MakeGuestSnapshot(guest, "default-account-id", guest_themes, "snap-1",
                          target_env.now, &snapshot, &error));
  CHECK(snapshot.spaces.size() == 2);
  CHECK(snapshot.bookmarks.size() == 4);  // top, dir, child, work
  CHECK(snapshot.pins.size() == 2);
  CHECK(snapshot.rules.size() == 3);
  CHECK(snapshot.themes.theme_ids.size() == 2);  // "gone" is not referenced
  for (const Tab& row : snapshot.bookmarks) {
    CHECK(!row.sync_id);
    if (row.guid == "gb-top" || row.guid == "gb-dir" || row.guid == "gb-work") {
      CHECK(!row.parent_guid);  // the hidden root is cut
    }
  }

  // The target holds "shared.example" under ITS W; the guest's W maps to a
  // new id, so the guest rule is not a duplicate there. Add one under the
  // default Space that is.
  CHECK(target.ApplyUrlRuleEdits({Rule("t-news", kDefaultSpaceId,
                                       "NEWS.example")},
                                 {}) == WriteError::kNone);

  GuestMappings plan;
  CHECK(PlanGuestImport(snapshot, target, "op1", &plan, &error));
  CHECK(plan.space_ids.at(kDefaultSpaceId) == kDefaultSpaceId);
  const std::string new_w = plan.space_ids.at("W");
  CHECK(new_w == "guest-import-op1-space-1");
  CHECK(plan.inserted_space_ids == std::vector<std::string>{new_w});
  CHECK(plan.bookmark_guids.at("gb-top") == "gb-top");
  CHECK(plan.default_import_folder_guid ==
        std::optional<std::string>("guest-import-op1-default-bookmarks-0"));
  CHECK(plan.skipped_rules.size() == 1);
  CHECK(plan.skipped_rules[0].source_id == "r-default");
  CHECK(plan.rule_targets.size() == 2);
  CHECK(plan.pin_targets.size() == 2);
  for (const GuestPinTarget& pin : plan.pin_targets) {
    CHECK(pin.should_insert);
    CHECK(pin.target_index >= 1);  // after the target's own pin
  }
  CHECK(GuestImportPresence(target, plan, nullptr) == GuestPresence::kAbsent);

  CHECK(target.ImportGuestData(snapshot, plan, "Imported from Guest", &error));
  CHECK(GuestImportPresence(target, plan, nullptr) == GuestPresence::kComplete);
  CHECK(VerifyGuestImport(target, snapshot, plan, &error));
  if (!error.empty()) {
    std::fprintf(stderr, "verify: %s\n", error.c_str());
  }
  // Default-Space bookmarks sit in the import folder, after the target's
  // own bookmark; the folder keeps its child.
  const Tab* folder = target.FindTab(*plan.default_import_folder_guid);
  CHECK(folder && folder->title == "Imported from Guest");
  CHECK(folder && folder->index == 1);
  CHECK(target.FindTab("gb-top")->parent_guid == folder->guid);
  CHECK(target.FindTab("gb-child")->parent_guid ==
        std::optional<std::string>("gb-dir"));
  // The renamed Space: appended, with its root and its bookmark.
  const Space* w = target.FindSpace(new_w);
  CHECK(w && w->name == "Work" && w->sort_order == 2);
  CHECK(w && target.FindTab("gb-work")->parent_guid == w->bookmark_root);
  CHECK(target.FindTab("gb-work")->space_id == new_w);
  // Rules: the skipped one stays the target's; the others land with ids.
  CHECK(target.UrlRules().size() == 3);
  for (const UrlRule& rule : target.UrlRules()) {
    CHECK(rule.sync_id.has_value());
  }
  // A second import of the same plan is a no-op.
  const size_t rows = target.tabs().size();
  CHECK(target.ImportGuestData(snapshot, plan, "Imported from Guest", &error));
  CHECK(target.tabs().size() == rows);

  // Themes: target wins, the renamed Space carries the guest's.
  GuestThemes target_themes;
  target_themes.theme_ids = {{kDefaultSpaceId, "pure"}};
  const GuestThemes merged =
      MergeGuestThemes(target_themes, snapshot.themes, plan.space_ids);
  CHECK(merged.theme_ids.at(kDefaultSpaceId) == "pure");
  CHECK(merged.theme_ids.at(new_w) == "mint");
  CHECK(merged.saturations.at(new_w) == 40);

  // A re-snapshot of the untouched Guest has the same content.
  GuestSnapshot again;
  CHECK(MakeGuestSnapshot(guest, "default-account-id", guest_themes, "snap-2",
                          target_env.now + 5, &again, &error));
  CHECK(again.SameContent(snapshot));
}

void TestPartialImportRefused() {
  FakeEnv guest_env{.prefix = "cccccccc"};
  FakeEnv target_env{.prefix = "dddddddd"};
  LocalStore guest(guest_env.Make());
  LocalStore target(target_env.Make());
  CHECK(guest.Open(TempDir("guest2")) == LocalStore::OpenResult::kOk);
  CHECK(target.Open(TempDir("target2")) == LocalStore::OpenResult::kOk);
  SeedGuest(guest);
  GuestSnapshot snapshot;
  std::string error;
  CHECK(MakeGuestSnapshot(guest, "default-account-id", {}, "s", 1, &snapshot,
                          &error));
  GuestMappings plan;
  CHECK(PlanGuestImport(snapshot, target, "op2", &plan, &error));
  // An empty target: every Space is inserted, ids kept.
  CHECK(plan.inserted_space_ids.size() == 2);
  CHECK(plan.space_ids.at("W") == "W");
  // One planned id appears out of band: the import refuses.
  CHECK(target.CreateSpace("W", kDefaultProfileId, "X", "#000000", "phi:x"));
  CHECK(GuestImportPresence(target, plan, nullptr) == GuestPresence::kPartial);
  CHECK(!target.ImportGuestData(snapshot, plan, "Imported", &error));
  CHECK(!target.FindTab("gb-top"));
}

void TestMissingSpaceRefused() {
  FakeEnv env{.prefix = "eeeeeeee"};
  LocalStore guest(env.Make());
  CHECK(guest.Open(TempDir("guest3")) == LocalStore::OpenResult::kOk);
  CHECK(guest.EnsureDefaultSpace(kDefaultProfileId));
  CHECK(guest.ApplyUrlRuleEdits({Rule("r", "nowhere", "a.example")}, {}) ==
        WriteError::kNone);
  GuestSnapshot snapshot;
  std::string error;
  CHECK(!MakeGuestSnapshot(guest, "default-account-id", {}, "s", 1, &snapshot,
                           &error));
  CHECK(error.find("nowhere") != std::string::npos);
}

}  // namespace

int main() {
  TestPlanImportVerify();
  TestPartialImportRefused();
  TestMissingSpaceRefused();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("guest_migration_test: all passed\n");
  return EXIT_SUCCESS;
}
