// The sync conformance gate (docs/user-data-parity.md, phase S0): replays the
// vectors the Mac's own merge core wrote (test/sync-vectors, produced by
// phibrowser-mac Tests/SyncConvergence, see its VECTORS.md) against the C++
// port. Run with test/model/run.sh, or directly with the vector directory as
// the only argument.
//
// Every family in the manifest is accounted for: a family the port implements
// must agree on every case, and a family it does not implement yet is listed
// as PENDING with the phase that brings it, so the gap is visible in every
// run rather than silently skipped. A family the runner has never heard of
// fails the run — a newer vector set must not look green on an older port.

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_url_rule_normalize.h"
#include "phibrowser-win/client/phi_sync_clock.h"
#include "phibrowser-win/client/phi_sync_merge.h"
#include "phibrowser-win/client/phi_sync_proto.h"
#include "phibrowser-win/client/phi_sync_rank.h"
#include "json_lite.h"

using json_lite::Value;
namespace psync = phi_client::sync;

namespace {

int g_failures = 0;

// Reports the first few disagreements of a family in full, then counts.
struct FamilyResult {
  std::string family;
  int cases = 0;
  int failed = 0;

  void Fail(const std::string& detail) {
    if (failed < 5) {
      std::fprintf(stderr, "FAIL %s case %d: %s\n", family.c_str(), cases,
                   detail.c_str());
    }
    ++failed;
  }
};

std::optional<std::string> OptionalString(const Value& v) {
  if (v.is_null()) {
    return std::nullopt;
  }
  return v.str();
}

std::string Show(const std::optional<std::string>& s) {
  return s ? "\"" + *s + "\"" : "nil";
}

// --- Families the port implements -----------------------------------------

void RunRankBetween(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    const auto lower = OptionalString(c["lower"]);
    const auto upper = OptionalString(c["upper"]);
    const std::string got = psync::RankBetween(lower, upper);
    if (got != c["out"].str()) {
      r.Fail("RankBetween(" + Show(lower) + ", " + Show(upper) + ") = " + got +
             ", Mac " + c["out"].str());
    }
    ++r.cases;
  }
}

void RunRankIsLegal(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    const bool got = psync::IsLegalRank(c["rank"].str());
    if (got != c["legal"].boolean) {
      r.Fail("IsLegalRank(\"" + c["rank"].str() + "\")");
    }
    ++r.cases;
  }
}

void RunRankAssign(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    std::vector<psync::RankedItem> order;
    for (const Value& item : c["order"].array) {
      order.push_back({item["uuid"].str(), OptionalString(item["rank"])});
    }
    const std::map<std::string, std::string> got = psync::AssignRanks(order);
    std::map<std::string, std::string> want;
    for (const auto& [uuid, rank] : c["assigned"].object) {
      want[uuid] = rank.str();
    }
    if (got != want) {
      std::string detail = "AssignRanks([";
      for (const auto& item : order) {
        detail += item.uuid + ":" + Show(item.rank) + " ";
      }
      detail += "]) disagrees";
      r.Fail(detail);
    }
    ++r.cases;
  }
}

void RunClockSequence(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    std::optional<psync::HybridClock> clock;
    int step_no = 0;
    for (const Value& step : c["steps"].array) {
      const std::string& op = step["op"].str();
      if (op == "init") {
        clock.emplace(step["max_seen"].i64());
      } else if (op == "stamp") {
        const int64_t got = clock->Stamp(step["wall_ms"].i64());
        if (got != step["out"].i64()) {
          r.Fail("step " + std::to_string(step_no) + " stamp(" +
                 step["wall_ms"].text + ") = " + std::to_string(got));
        }
      } else if (op == "observe") {
        clock->Observe(step["stamp_ms"].i64());
      }
      if (clock->max_seen() != step["max_seen"].i64()) {
        r.Fail("step " + std::to_string(step_no) + " maxSeen " +
               std::to_string(clock->max_seen()) + ", Mac " +
               step["max_seen"].text);
      }
      ++step_no;
    }
    ++r.cases;
  }
}

void RunClockFunctions(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    const int64_t a = c["a"].i64();
    const int64_t b = c["b"].i64();
    if (psync::HybridClock::EditStamp(a, b) != c["edit_stamp"].i64()) {
      r.Fail("EditStamp(" + c["a"].text + ", " + c["b"].text + ")");
    }
    if (psync::HybridClock::WallClockCorrection(a, b) !=
        c["wall_clock_correction"].i64()) {
      r.Fail("WallClockCorrection(" + c["a"].text + ", " + c["b"].text + ")");
    }
    if (psync::HybridClock::Corrected(a, b) != c["corrected"].i64()) {
      r.Fail("Corrected(" + c["a"].text + ", " + c["b"].text + ")");
    }
    ++r.cases;
  }
}

void RunNormalizedHost(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    const std::string got =
        phi_client::store::NormalizedRuleHost(c["raw"].str());
    if (got != c["out"].str()) {
      r.Fail("NormalizedRuleHost(\"" + c["raw"].str() + "\") = \"" + got +
             "\", Mac \"" + c["out"].str() + "\"");
    }
    ++r.cases;
  }
}

void RunNormalizedPathPrefix(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    const auto raw = OptionalString(c["raw"]);
    const auto want = OptionalString(c["out"]);
    const auto got = phi_client::store::NormalizedRulePathPrefix(raw);
    if (got != want) {
      r.Fail("NormalizedRulePathPrefix(" + Show(raw) + ") = " + Show(got) +
             ", Mac " + Show(want));
    }
    ++r.cases;
  }
}

// --- The merge layer (S4) -------------------------------------------------------

std::string FromHex(const std::string& hex) {
  std::string out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
  }
  return out;
}

std::string ToHex(const std::string& bytes) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (const unsigned char c : bytes) {
    out.push_back(kDigits[c >> 4]);
    out.push_back(kDigits[c & 15]);
  }
  return out;
}

template <typename Message>
bool ParseHex(const Value& v, Message* out) {
  return out->ParseFrom(FromHex(v.str()));
}

void RunLwwWinner(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    psync::SettingValue a, b;
    if (!ParseHex(c["a"], &a) || !ParseHex(c["b"], &b)) {
      r.Fail("unparseable value");
    } else {
      const std::string got = ToHex(psync::LwwWinner(a, b).Serialize());
      if (got != c["winner"].str()) {
        r.Fail("winner " + got + ", Mac " + c["winner"].str());
      }
    }
    ++r.cases;
  }
}

void RunSettingsMerge(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    psync::SettingEntity local, remote;
    if (!ParseHex(c["local"], &local) || !ParseHex(c["remote"], &remote)) {
      r.Fail("unparseable entity");
    } else {
      const std::string got =
          ToHex(psync::MergeSettings(local, remote).Serialize());
      if (got != c["merged"].str()) {
        r.Fail("merged " + got + ", Mac " + c["merged"].str());
      }
    }
    ++r.cases;
  }
}

void RunSpacesMerge(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    psync::SpaceEntity local, remote;
    if (!ParseHex(c["local"], &local) || !ParseHex(c["remote"], &remote)) {
      r.Fail("unparseable entity");
    } else {
      const psync::SpaceEntity merged = psync::MergeSpace(local, remote);
      const std::string got = ToHex(merged.Serialize());
      if (got != c["merged"].str()) {
        r.Fail("merged " + got + ", Mac " + c["merged"].str());
      } else if (psync::SpaceRefuses(merged) != c["merged_refuses"].boolean) {
        r.Fail("refuses disagrees");
      }
    }
    ++r.cases;
  }
}

// The three owned-item kinds share the signature / stamp checks.
template <typename Entity>
void CheckOwned(const Value& c, FamilyResult& r, const Entity& merged,
                const std::string& signature, int64_t location, int64_t content) {
  const std::string got = ToHex(merged.Serialize());
  if (got != c["merged"].str()) {
    r.Fail("merged " + got + ", Mac " + c["merged"].str());
  } else if (ToHex(signature) != c["merged_content_signature"].str()) {
    r.Fail("content signature " + ToHex(signature) + ", Mac " +
           c["merged_content_signature"].str());
  } else if (std::to_string(location) != c["merged_location_stamp"].str() ||
             std::to_string(content) != c["merged_content_stamp"].str()) {
    r.Fail("stamps " + std::to_string(location) + "/" +
           std::to_string(content) + ", Mac " +
           c["merged_location_stamp"].str() + "/" +
           c["merged_content_stamp"].str());
  }
}

void RunBookmarksMerge(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    psync::BookmarkEntity local, remote;
    if (!ParseHex(c["local"], &local) || !ParseHex(c["remote"], &remote)) {
      r.Fail("unparseable entity");
    } else {
      const psync::BookmarkEntity m = psync::MergeBookmark(local, remote);
      CheckOwned(c, r, m, psync::BookmarkContentSignature(m),
                 psync::BookmarkLocationStamp(m),
                 psync::BookmarkContentStamp(m));
    }
    ++r.cases;
  }
}

void RunPinsMerge(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    psync::PinTabEntity local, remote;
    if (!ParseHex(c["local"], &local) || !ParseHex(c["remote"], &remote)) {
      r.Fail("unparseable entity");
    } else {
      const psync::PinTabEntity m = psync::MergePin(local, remote);
      if (psync::PinIdentity(m) != c["merged_identity"].str()) {
        r.Fail("identity " + psync::PinIdentity(m) + ", Mac " +
               c["merged_identity"].str());
      } else {
        CheckOwned(c, r, m, psync::PinContentSignature(m),
                   psync::PinLocationStamp(m), psync::PinContentStamp(m));
      }
    }
    ++r.cases;
  }
}

void RunUrlRulesMerge(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    psync::UrlRuleEntity local, remote;
    if (!ParseHex(c["local"], &local) || !ParseHex(c["remote"], &remote)) {
      r.Fail("unparseable entity");
    } else {
      const psync::UrlRuleEntity m = psync::MergeUrlRule(local, remote);
      CheckOwned(c, r, m, psync::UrlRuleContentSignature(m),
                 psync::UrlRuleLocationStamp(m),
                 psync::UrlRuleContentStamp(m));
    }
    ++r.cases;
  }
}

void RunClientTagHash(const Value& cases, FamilyResult& r) {
  for (const Value& c : cases.array) {
    const std::string got = psync::ClientTagHash(c["tag"].str());
    if (got != c["hash"].str()) {
      r.Fail("hash(" + c["tag"].str() + ") = " + got + ", Mac " +
             c["hash"].str());
    }
    ++r.cases;
  }
}

// --- The registry ------------------------------------------------------------

struct Family {
  // Null while the port has not reached the family; `pending` then names the
  // phase of docs/user-data-parity.md §5 that brings it.
  std::function<void(const Value&, FamilyResult&)> run;
  const char* pending = nullptr;
};

const std::map<std::string, Family>& Registry() {
  static const std::map<std::string, Family> registry = {
      {"rank_between", {RunRankBetween}},
      {"rank_is_legal", {RunRankIsLegal}},
      {"rank_assign", {RunRankAssign}},
      {"clock_sequence", {RunClockSequence}},
      {"clock_functions", {RunClockFunctions}},
      {"lww_winner", {RunLwwWinner}},
      {"settings_merge", {RunSettingsMerge}},
      {"spaces_merge", {RunSpacesMerge}},
      {"bookmarks_merge", {RunBookmarksMerge}},
      {"pins_merge", {RunPinsMerge}},
      {"urlrules_merge", {RunUrlRulesMerge}},
      {"client_tag_hash", {RunClientTagHash}},
      {"urlrule_normalized_host", {RunNormalizedHost}},
      {"urlrule_normalized_path_prefix", {RunNormalizedPathPrefix}},
  };
  return registry;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "../sync-vectors";
  Value manifest;
  try {
    manifest = json_lite::ParseFile(dir + "/manifest.json");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "FAIL cannot read the vectors: %s\n", e.what());
    return EXIT_FAILURE;
  }
  if (manifest["format"].text != "1") {
    std::fprintf(stderr, "FAIL vector format %s, this runner reads 1\n",
                 manifest["format"].text.c_str());
    return EXIT_FAILURE;
  }
  std::printf("sync vectors from phibrowser-mac %s, seed %s\n",
              manifest["source_revision"].is_null()
                  ? "(unknown revision)"
                  : manifest["source_revision"].str().c_str(),
              manifest["seed"].str().c_str());

  int passed_cases = 0;
  std::vector<std::string> pending;
  for (const auto& [family, count] : manifest["families"].object) {
    const auto it = Registry().find(family);
    if (it == Registry().end()) {
      std::fprintf(stderr,
                   "FAIL unknown family %s: the vectors are newer than this "
                   "runner\n",
                   family.c_str());
      ++g_failures;
      continue;
    }
    if (!it->second.run) {
      pending.push_back(family + " (" + count.text + " cases) -> " +
                        it->second.pending);
      continue;
    }
    FamilyResult result{family};
    try {
      const Value body = json_lite::ParseFile(dir + "/" + family + ".json");
      it->second.run(body["cases"], result);
    } catch (const std::exception& e) {
      result.Fail(std::string("exception: ") + e.what());
    }
    if (std::to_string(result.cases) != count.text) {
      result.Fail("ran " + std::to_string(result.cases) +
                  " cases, the manifest lists " + count.text);
    }
    if (result.failed) {
      std::fprintf(stderr, "FAIL %s: %d of %d cases disagree with the Mac\n",
                   family.c_str(), result.failed, result.cases);
      ++g_failures;
    } else {
      std::printf("  ok       %-32s %d cases\n", family.c_str(), result.cases);
      passed_cases += result.cases;
    }
  }
  for (const std::string& p : pending) {
    std::printf("  PENDING  %s\n", p.c_str());
  }
  if (g_failures) {
    std::fprintf(stderr, "%d famil%s failed\n", g_failures,
                 g_failures == 1 ? "y" : "ies");
    return EXIT_FAILURE;
  }
  std::printf("sync_vectors_test: %d cases agree, %zu families pending\n",
              passed_cases, pending.size());
  return EXIT_SUCCESS;
}
