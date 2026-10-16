// Host tests for the account's local store (client/phi_local_store.h), the
// Windows twin of the Mac's SwiftData LocalStore. Run with test/model/run.sh.
//
// What is pinned here is the Mac's WRITE RULES, because the sync engine
// reads them: which operation stamps contentUpdatedDate/locationUpdatedDate,
// which renumbers, what a pin-scope change keeps as a backup and how copies
// merge back, what a Space delete does to its rules, how the URL rule edit
// set assigns ids and pending flags. Plus persistence: every mutation is on
// disk when it returns, a reopen reads it back, and a newer store format is
// refused.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

// A deterministic clock and uuid source; the clock advances only on Tick().
struct FakeEnv {
  TimeMs now = 1'700'000'000'000;
  int next_uuid = 0;
  Env Make() {
    return Env{[this] { return now; },
               [this] {
                 char text[40];
                 std::snprintf(text, sizeof(text),
                               "00000000-0000-4000-8000-%012d", ++next_uuid);
                 return std::string(text);
               }};
  }
  void Tick(TimeMs by = 1000) { now += by; }
};

std::string TempDir(const char* name) {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("phi_local_store_test_" +
                                     std::to_string(::getpid()) + "_" + name);
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir.string();
}

std::vector<std::string> Titles(const std::vector<const Tab*>& rows) {
  std::vector<std::string> out;
  for (const Tab* t : rows) {
    out.push_back(t->title);
  }
  return out;
}

void TestOpenAndManifest() {
  FakeEnv env;
  const std::string dir = TempDir("open");
  {
    LocalStore store(env.Make());
    CHECK(store.Open(dir) == LocalStore::OpenResult::kOk);
    CHECK(store.pin_scope() == PinScope::kProfile);  // The Mac's fallback.
  }
  std::ifstream manifest(dir + "/LocalStoreCompatibility.json");
  std::string text((std::istreambuf_iterator<char>(manifest)),
                   std::istreambuf_iterator<char>());
  CHECK(text.find("\"activeStoreFormatVersion\" : 13") != std::string::npos);
  CHECK(text.find("\"manifestVersion\" : 1") != std::string::npos);
  // A newer app's store is refused, and left untouched.
  {
    std::ofstream(dir + "/LocalStoreCompatibility.json")
        << "{\"activeStoreFormatVersion\" : 14}";
    LocalStore store(env.Make());
    CHECK(store.Open(dir) == LocalStore::OpenResult::kRequiresNewerApp);
    CHECK(!store.is_open());
  }
}

void TestDefaultSpaceAndRoots() {
  FakeEnv env;
  const std::string dir = TempDir("spaces");
  LocalStore store(env.Make());
  CHECK(store.Open(dir) == LocalStore::OpenResult::kOk);
  CHECK(store.EnsureDefaultSpace("Default"));
  const Space* def = store.FindSpace("default-space");
  CHECK(def && def->name == "Default" && def->color_hex == "#3A6FF8" &&
        def->icon_name == "phi:phi-icon-view-grid-add" && def->sort_order == 0);
  // The default Space's first root is mirrored onto the Profile.
  const std::string root = store.BookmarkRoot("Default", "default-space", true);
  CHECK(!root.empty());
  CHECK(store.IsBookmarkRoot(root));
  CHECK(store.FindProfile("Default")->bookmark_root == root);
  CHECK(store.FindTab(root)->url == "https://bookmark.phi/folder");
  // A second Space: appended to the global order, root materialised.
  CHECK(store.CreateSpace("S2", "Profile 1", "Work", "#112233", "emoji:1"));
  CHECK(store.FindSpace("S2")->sort_order == 1);
  const std::string root2 = store.ExistingBookmarkRoot("Profile 1", "S2");
  CHECK(!root2.empty() && root2 != root);
  // Not the Profile's pointer: only the default Space's root is.
  CHECK(!store.FindProfile("Profile 1")->bookmark_root);
  // ensureDefaultSpace never resurrects a deleted default Space.
  CHECK(store.DeleteSpaceCascade("default-space", true));
  CHECK(!store.FindTab(root));
  CHECK(!store.FindProfile("Default")->bookmark_root);
  CHECK(store.EnsureDefaultSpace("Default"));
  CHECK(!store.FindSpace("default-space"));
  // Reorder: position, no updatedDate.
  CHECK(store.CreateSpace("S3", "Default", "Play", "#445566", "emoji:2"));
  const TimeMs s3_updated = store.FindSpace("S3")->updated;
  env.Tick();
  CHECK(store.ReorderSpaces({"S3", "S2"}));
  CHECK(store.FindSpace("S3")->sort_order == 0);
  CHECK(store.FindSpace("S2")->sort_order == 1);
  CHECK(store.FindSpace("S3")->updated == s3_updated);
  CHECK(store.Spaces().front().space_id == "S3");
}

void TestBookmarkWriteRules() {
  FakeEnv env;
  LocalStore store(env.Make());
  CHECK(store.Open(TempDir("bookmarks")) == LocalStore::OpenResult::kOk);
  store.EnsureDefaultSpace("Default");
  LocalStore::BookmarkCreate a;
  a.profile_id = "Default";
  a.title = "";  // An empty title shows the url.
  a.url = "https://a.example/";
  const std::string ga = store.CreateBookmark(a);
  CHECK(store.FindTab(ga)->title == "https://a.example/");
  CHECK(!store.FindTab(ga)->content_updated);  // New is not edited.
  const std::string folder = store.CreateFolder(
      "Default", "default-space", std::nullopt, std::nullopt, "F");
  LocalStore::BookmarkCreate b = a;
  b.title = "B";
  b.url = "https://b.example/";
  const std::string gb = store.CreateBookmark(b);
  const std::string root = store.ExistingBookmarkRoot("Default",
                                                      "default-space");
  CHECK(Titles(store.Children(root)) ==
        (std::vector<std::string>{"https://a.example/", "F", "B"}));

  // A reorder under the same parent is rank: no location stamp.
  env.Tick();
  CHECK(store.MoveBookmark(gb, "Default", root, 0));
  CHECK(!store.FindTab(gb)->location_updated);
  CHECK(store.Children(root).front()->guid == gb);
  // A real move records the location edit.
  env.Tick();
  CHECK(store.MoveBookmark(gb, "Default", folder, 0));
  CHECK(store.FindTab(gb)->location_updated == env.now);
  CHECK(store.FindTab(gb)->parent_guid == folder);
  // The old siblings closed the gap.
  CHECK(store.Children(root)[0]->index == 0 && store.Children(root)[1]->index == 1);
  // A folder never moves into itself.
  CHECK(!store.MoveBookmark(folder, "Default", folder, 0));

  // Content stamps only on a real change; saving the same title does not.
  env.Tick();
  LocalStore::BookmarkUpdate same;
  same.title = "B";
  CHECK(store.UpdateBookmark(gb, same));
  CHECK(!store.FindTab(gb)->content_updated);
  env.Tick();
  LocalStore::BookmarkUpdate rename;
  rename.title = "B2";
  CHECK(store.UpdateBookmark(gb, rename));
  CHECK(store.FindTab(gb)->content_updated == env.now);
  // The folder icon is content too.
  env.Tick();
  LocalStore::BookmarkUpdate icon;
  icon.icon = "github";
  CHECK(store.UpdateBookmark(folder, icon));
  CHECK(store.FindTab(folder)->content_updated == env.now);
  // A split layout is not synced: no stamp.
  LocalStore::BookmarkUpdate split;
  split.secondary_url = std::optional<std::string>("https://c.example/");
  split.secondary_title = std::optional<std::string>("C");
  CHECK(store.UpdateBookmark(ga, split));
  const TimeMs stamped = *store.FindTab(ga)->content_updated;
  env.Tick();
  CHECK(store.SetBookmarkSplitLayout(ga, "horizontal"));
  CHECK(store.FindTab(ga)->content_updated == stamped);
  // Clearing the second pane clears its title and layout.
  LocalStore::BookmarkUpdate unsplit;
  unsplit.secondary_url = std::optional<std::string>();
  CHECK(store.UpdateBookmark(ga, unsplit));
  CHECK(!store.FindTab(ga)->secondary_url && !store.FindTab(ga)->secondary_title &&
        !store.FindTab(ga)->layout);

  // The root cannot be moved or deleted; a folder takes its subtree.
  CHECK(!store.DeleteBookmark(root));
  CHECK(store.DeleteBookmark(folder));
  CHECK(!store.FindTab(gb));
  CHECK(store.Children(root).size() == 1);
  CHECK(store.Children(root)[0]->index == 0);
}

void TestMoveBookmarksToSpace() {
  FakeEnv env;
  LocalStore store(env.Make());
  CHECK(store.Open(TempDir("move_space")) == LocalStore::OpenResult::kOk);
  store.EnsureDefaultSpace("Default");
  CHECK(store.CreateSpace("S2", "Profile 1", "Work", "#112233", "emoji:1"));
  const std::string folder = store.CreateFolder(
      "Default", "default-space", std::nullopt, std::nullopt, "F");
  LocalStore::BookmarkCreate child;
  child.profile_id = "Default";
  child.parent_guid = folder;
  child.title = "Child";
  child.url = "https://child.example/";
  const std::string gchild = store.CreateBookmark(child);
  env.Tick();
  CHECK(store.MoveBookmarksToSpace({folder}, "Default", "Profile 1", "S2",
                                   std::nullopt, std::nullopt));
  // The moved row records the move; the descendant is only retagged.
  CHECK(store.FindTab(folder)->location_updated == env.now);
  CHECK(!store.FindTab(gchild)->location_updated);
  CHECK(store.FindTab(gchild)->space_id == "S2");
  CHECK(store.FindTab(gchild)->profile_id == "Profile 1");
  CHECK(store.FindTab(folder)->parent_guid ==
        store.ExistingBookmarkRoot("Profile 1", "S2"));
}

void TestPins() {
  FakeEnv env;
  const std::string dir = TempDir("pins");
  LocalStore store(env.Make());
  CHECK(store.Open(dir) == LocalStore::OpenResult::kOk);
  store.EnsureDefaultSpace("Default");
  CHECK(store.CreateSpace("S2", "Default", "Work", "#112233", "emoji:1"));
  const auto pin = [&](const char* title, const char* space) {
    LocalStore::PinCreate in;
    in.url = std::string("https://") + title + ".example/";
    in.title = title;
    in.profile_id = "Default";
    in.space_id = space;
    return store.CreatePin(in);
  };
  // Profile scope: one collection per profile, shared by its Spaces.
  const std::string p1 = pin("one", "default-space");
  const std::string p2 = pin("two", "S2");
  CHECK(store.FindTab(p1)->pin_lineage_id == p1);  // Its own lineage.
  CHECK(store.FindTab(p1)->profile_id == "Default" &&
        !store.FindTab(p1)->space_id);
  CHECK(Titles(store.ActivePins("Default", "S2")) ==
        (std::vector<std::string>{"one", "two"}));
  CHECK(store.MovePin(p2, "Default", "S2", 0));
  CHECK(Titles(store.ActivePins("Default", "default-space")) ==
        (std::vector<std::string>{"two", "one"}));

  // A UI title edit stamps only when it changes the title.
  env.Tick();
  CHECK(store.UpdatePin(p1, std::nullopt, std::string("one")));
  CHECK(!store.FindTab(p1)->content_updated);
  CHECK(store.UpdatePin(p1, std::nullopt, std::string("uno")));
  CHECK(store.FindTab(p1)->content_updated == env.now);

  // Split pair: two rows linked both ways; swapping panes swaps order.
  CHECK(store.SetPinSplitPair(p2, p1, "vertical"));
  CHECK(store.FindTab(p1)->split_partner_guid == p2);
  CHECK(store.SwapPinSplitOrder(p2));
  CHECK(Titles(store.ActivePins("Default", "S2")) ==
        (std::vector<std::string>{"uno", "two"}));

  // The sidebar's cells: the pair is one unit, primary first; moving and
  // removing act on the whole unit.
  const std::string p3 = pin("three", "S2");
  auto units = store.PinUnits("Default", "S2");
  CHECK(units.size() == 2 && units[0].secondary && units[0].primary->title == "uno");
  CHECK(store.MovePinUnit(p3, 0));
  CHECK(Titles(store.ActivePins("Default", "S2")) ==
        (std::vector<std::string>{"three", "uno", "two"}));
  CHECK(store.MovePinUnit(p2, 0));  // By either half.
  CHECK(Titles(store.ActivePins("Default", "S2")) ==
        (std::vector<std::string>{"uno", "two", "three"}));
  CHECK(store.RemovePinUnit(p3));
  CHECK(store.ActivePins("Default", "S2").size() == 2);

  // Narrowing to Space scope: every Space gets a copy with NEW guids and
  // the SAME lineage; the Profile rows stay as inactive backups.
  env.Tick();
  CHECK(store.ChangePinScope(PinScope::kSpace, "Default", "default-space"));
  CHECK(store.pin_scope() == PinScope::kSpace);
  const auto in_default = store.ActivePins("Default", "default-space");
  const auto in_s2 = store.ActivePins("Default", "S2");
  CHECK(Titles(in_default) == (std::vector<std::string>{"uno", "two"}));
  CHECK(Titles(in_s2) == (std::vector<std::string>{"uno", "two"}));
  CHECK(in_default[0]->guid != p1 && in_s2[0]->guid != p1);
  CHECK(in_default[0]->pin_lineage_id == p1 && in_s2[0]->pin_lineage_id == p1);
  CHECK(in_default[0]->content_updated == store.FindTab(p1)->content_updated);
  // The split pair is re-linked among the copies.
  CHECK(in_default[0]->split_partner_guid == in_default[1]->guid);
  CHECK(store.FindTab(p1) != nullptr);  // The backup.
  // A stale guid from the backup resolves to this Space's copy.
  CHECK(store.ResolveActivePin(p1, "Default", "S2") == in_s2[0]);

  // Widening back merges identical copies by lineage into one; a copy
  // edited in one Space is a divergent variant and survives separately —
  // and so does its split partner, whose variant includes the partner's
  // content.
  CHECK(store.UpdatePin(in_s2[1]->guid, std::nullopt,
                        std::string("two-edited")));
  env.Tick();
  CHECK(store.ChangePinScope(PinScope::kProfile, "Default", "default-space"));
  const auto merged = store.ActivePins("Default", "default-space");
  CHECK(Titles(merged) ==
        (std::vector<std::string>{"uno", "two", "uno", "two-edited"}));
  // Unsplit copies with no divergence collapse to one.
  CHECK(store.ChangePinScope(PinScope::kSpace, "Default", "default-space"));
  for (const Tab* t : store.ActivePins("Default", "S2")) {
    CHECK(store.ClearPinSplitPartner(t->guid));
  }
  for (const Tab* t : store.ActivePins("Default", "default-space")) {
    CHECK(store.ClearPinSplitPartner(t->guid));
  }
  CHECK(store.ChangePinScope(PinScope::kProfile, "Default", "default-space"));
  CHECK(Titles(store.ActivePins("Default", "default-space")) ==
        (std::vector<std::string>{"uno", "two", "two-edited"}));
  // The old Profile backups were rebuilt, not resurrected.
  CHECK(!store.FindTab(p1));

  // Persistence: a reopen reads everything back.
  const size_t rows = store.tabs().size();
  store.Close();
  CHECK(store.Open(dir) == LocalStore::OpenResult::kOk);
  CHECK(store.tabs().size() == rows);
  CHECK(store.pin_scope() == PinScope::kProfile);
  CHECK(Titles(store.ActivePins("Default", "S2")) ==
        (std::vector<std::string>{"uno", "two", "two-edited"}));
}

void TestDormantPins() {
  FakeEnv env;
  LocalStore store(env.Make());
  CHECK(store.Open(TempDir("dormant")) == LocalStore::OpenResult::kOk);
  store.EnsureDefaultSpace("Default");
  // A pin in a profile that has no Space.
  LocalStore::PinCreate in;
  in.url = "https://lonely.example/";
  in.title = "lonely";
  in.profile_id = "Profile 9";
  in.space_id = "nowhere";
  const std::string lonely = store.CreatePin(in);
  CHECK(store.ChangePinScope(PinScope::kSpace, "", ""));
  CHECK(store.FindTab(lonely)->pin_dormant);
  // Leaving Space scope, the dormant collection is live input again.
  CHECK(store.ChangePinScope(PinScope::kApp, "", ""));
  const auto app = store.ActivePins("Default", "default-space");
  CHECK(Titles(app) == (std::vector<std::string>{"lonely"}));
  CHECK(!store.FindTab(lonely)->pin_dormant);
}

// Space -> Profile with a dormant Profile row: the row is live input (it is
// rebuilt into the Profile collection) AND a target-scope backup (it goes).
// The port read the deleted row's guid to tell the two apart and crashed the
// client on every such change (use-after-free, 2026-10-16); the survivors are
// resolved before the deletion now.
void TestDormantPinSpaceToProfile() {
  FakeEnv env;
  LocalStore store(env.Make());
  CHECK(store.Open(TempDir("dormant_profile")) == LocalStore::OpenResult::kOk);
  store.EnsureDefaultSpace("Default");
  LocalStore::PinCreate in;
  in.url = "https://lonely.example/";
  in.title = "lonely";
  in.profile_id = "Profile 9";
  in.space_id = "nowhere";
  const std::string lonely = store.CreatePin(in);
  CHECK(store.ChangePinScope(PinScope::kSpace, "", ""));
  CHECK(store.FindTab(lonely)->pin_dormant);
  CHECK(store.ChangePinScope(PinScope::kProfile, "", ""));
  CHECK(store.pin_scope() == PinScope::kProfile);
  // The old row was the Profile collection's backup and is gone; its copy
  // carries the lineage, active and awake.
  CHECK(store.FindTab(lonely) == nullptr);
  const auto rebuilt = store.ActivePins("Profile 9", "nowhere");
  CHECK(Titles(rebuilt) == (std::vector<std::string>{"lonely"}));
  CHECK(rebuilt[0]->pin_lineage_id == lonely && !rebuilt[0]->pin_dormant);
}

void TestTransferPin() {
  FakeEnv env;
  LocalStore store(env.Make());
  CHECK(store.Open(TempDir("transfer")) == LocalStore::OpenResult::kOk);
  store.EnsureDefaultSpace("Default");
  CHECK(store.CreateSpace("S2", "Default", "Work", "#112233", "emoji:1"));
  CHECK(store.ChangePinScope(PinScope::kSpace, "", ""));
  LocalStore::PinCreate in;
  in.url = "https://x.example/";
  in.title = "x";
  in.profile_id = "Default";
  in.space_id = "default-space";
  const std::string x = store.CreatePin(in);
  const std::string moved =
      store.TransferPin(x, "Default", "default-space", "Default", "S2", 0);
  CHECK(!moved.empty() && moved != x);
  CHECK(!store.FindTab(x));
  CHECK(store.FindTab(moved)->space_id == "S2");
  CHECK(store.FindTab(moved)->pin_lineage_id == x);  // Lineage travels.
  CHECK(store.ActivePins("Default", "default-space").empty());
}

void TestSpaceCascadeAndProfileChange() {
  FakeEnv env;
  LocalStore store(env.Make());
  CHECK(store.Open(TempDir("cascade")) == LocalStore::OpenResult::kOk);
  store.EnsureDefaultSpace("Default");
  CHECK(store.CreateSpace("S2", "Default", "Work", "#112233", "emoji:1"));
  LocalStore::BookmarkCreate b;
  b.profile_id = "Default";
  b.space_id = "S2";
  b.title = "B";
  b.url = "https://b.example/";
  const std::string gb = store.CreateBookmark(b);
  UrlRuleDraft rule;
  rule.content = UrlRuleDraft::Content{"GitHub.com.", std::nullopt, false};
  rule.space_id = "S2";
  CHECK(store.ApplyUrlRuleEdits({rule}, {}) == WriteError::kNone);
  // Re-binding moves the Space's bookmarks with it; default-space refuses.
  CHECK(store.ChangeSpaceProfile("S2", "Profile 1"));
  CHECK(store.FindTab(gb)->profile_id == "Profile 1");
  CHECK(!store.ChangeSpaceProfile("default-space", "Profile 1"));
  // A user delete hard-deletes rows but SOFT-deletes the rules.
  CHECK(store.DeleteSpaceCascade("S2", /*user_intent=*/true));
  CHECK(!store.FindTab(gb));
  CHECK(store.UrlRules().empty());
  CHECK(store.UrlRules(true).size() == 1 && store.UrlRules(true)[0].deleted);
}

void TestUrlRuleEdits() {
  FakeEnv env;
  LocalStore store(env.Make());
  CHECK(store.Open(TempDir("rules")) == LocalStore::OpenResult::kOk);
  UrlRuleDraft a;
  a.id = "A";
  a.content = UrlRuleDraft::Content{" GitHub.COM. ", std::string("/"), false};
  a.space_id = "S";
  UrlRuleDraft b;
  b.id = "B";
  b.content = UrlRuleDraft::Content{"*.figma.com", std::string("docs/"), true};
  b.space_id = "S";
  CHECK(store.ApplyUrlRuleEdits({a, b}, {}) == WriteError::kNone);
  auto rules = store.UrlRules();
  CHECK(rules.size() == 2);
  // Normalised; "/" stays root-only; a new row mints a lowercase syncId, no
  // content stamp, and is pending.
  CHECK(rules[0].id == "A" && rules[0].host == "github.com" &&
        rules[0].path_prefix == "/" && rules[0].sort_order == 0);
  CHECK(rules[1].path_prefix == "/docs" && rules[1].sort_order == 1);
  CHECK(rules[0].sync_id && rules[0].sync_id->find_first_of("ABCDEF") ==
                                std::string::npos);
  CHECK(!rules[0].content_updated && rules[0].pending_local_edit);

  // An equal unit writes nothing; a changed one stamps it.
  env.Tick();
  UrlRuleDraft same;
  same.id = "A";
  same.content = UrlRuleDraft::Content{"github.com", std::string("/"), false};
  CHECK(store.ApplyUrlRuleEdits({same}, {}) == WriteError::kNone);
  CHECK(!store.UrlRules()[0].content_updated);
  UrlRuleDraft retarget;
  retarget.id = "A";
  retarget.space_id = "T";
  CHECK(store.ApplyUrlRuleEdits({retarget}, {}) == WriteError::kNone);
  rules = store.UrlRules();
  CHECK(rules.size() == 2);
  const UrlRule& moved = rules[1].id == "A" ? rules[1] : rules[0];
  CHECK(moved.space_id == "T" && moved.target_updated == env.now &&
        moved.sort_order == 0);
  // "B" closed the gap in its bucket.
  const UrlRule& b_row = rules[0].id == "B" ? rules[0] : rules[1];
  CHECK(b_row.sort_order == 0);

  // A delete is soft; the editor re-adding the same id gets a fresh row.
  CHECK(store.ApplyUrlRuleEdits({}, {"B"}) == WriteError::kNone);
  CHECK(store.UrlRules().size() == 1);
  CHECK(store.ApplyUrlRuleEdits({b}, {}) == WriteError::kNone);
  CHECK(store.UrlRules().size() == 2);
  CHECK(store.UrlRules(true).size() == 3);
  // An incomplete new draft is refused and changes nothing.
  UrlRuleDraft incomplete;
  incomplete.id = "C";
  incomplete.space_id = "S";
  CHECK(store.ApplyUrlRuleEdits({incomplete}, {}) ==
        WriteError::kNoCandidateSurvived);
  CHECK(store.UrlRules(true).size() == 3);
}

}  // namespace

int main() {
  TestOpenAndManifest();
  TestDefaultSpaceAndRoots();
  TestBookmarkWriteRules();
  TestMoveBookmarksToSpace();
  TestPins();
  TestDormantPins();
  TestDormantPinSpaceToProfile();
  TestTransferPin();
  TestSpaceCascadeAndProfileChange();
  TestUrlRuleEdits();
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("local_store_test: all passed\n");
  return EXIT_SUCCESS;
}
