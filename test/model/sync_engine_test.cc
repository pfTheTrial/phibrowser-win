// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Phi native sync's engine (docs/user-data-parity.md S5), settings path,
// device to device through a fake sync server that keeps the real server's
// rules: rows keyed by client_tag_hash with a version per write, a paged
// GetUpdates walking a version watermark, CONFLICT for a stale base version
// or a create over a live row, INVALID_MESSAGE for an unknown id,
// NOT_MY_BIRTHDAY for a wrong store birthday. The messages go through the
// real protocol codec both ways.

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "phi_key_crypto.h"
#include "phi_sync_engine.h"
#include "phi_sync_cleanup.h"
#include "phi_sync_bookmarks.h"
#include "phi_sync_merge.h"
#include "phi_sync_owned.h"
#include "phi_sync_pins.h"
#include "phi_sync_rules.h"
#include "phi_sync_proto.h"
#include "phi_sync_protocol.h"
#include "phi_sync_spaces.h"

namespace psync = phi_client::sync;
namespace keys = phi_client::keys;
using psync::wire::Reader;
using psync::wire::WireType;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    ++g_failures;
  }
}

// --- The fake server --------------------------------------------------------------------

struct Row {
  std::string id;
  int64_t version = 0;
  std::string ciphertext;
  bool deleted = false;
};

struct FakeServer : psync::SyncTransport {
  std::map<std::string, Row> rows;  // by client_tag_hash
  int64_t clock = 0;
  int next_id = 1;
  std::string birthday = "birthday-1";
  size_t page_size = 100;
  int64_t date_ms = 0;  // the Date header; 0 = absent
  int commits = 0;
  bool offline = false;  // every request fails as a transport error

  psync::ProtocolStatus Post(const std::string&, const std::string& body,
                             int* status, std::string* response,
                             std::optional<int64_t>* server_date_ms) override {
    if (offline) {
      psync::ProtocolStatus failed;
      failed.error = psync::ProtocolError::kTransport;
      return failed;
    }
    *status = 200;
    *server_date_ms =
        date_ms ? std::optional<int64_t>(date_ms) : std::nullopt;
    Reader reader(body);
    int contents = 0;
    std::string payload;
    std::string request_birthday;
    while (!reader.done()) {
      int field = 0;
      WireType type = psync::wire::kVarint;
      reader.Next(&field, &type);
      std::string_view bytes;
      uint64_t number = 0;
      if (type == psync::wire::kVarint) {
        reader.Varint(&number);
        if (field == 3) {
          contents = static_cast<int>(number);
        }
      } else if (type == psync::wire::kLengthDelimited) {
        reader.Bytes(&bytes);
        if (field == 4 || field == 5) {
          payload = std::string(bytes);
        } else if (field == 7) {
          request_birthday = std::string(bytes);
        }
      }
    }
    std::string out;
    if (!request_birthday.empty() && request_birthday != birthday) {
      psync::wire::PutVarintField(&out, 4, 2);  // NOT_MY_BIRTHDAY
      *response = out;
      return psync::ProtocolStatus();
    }
    if (contents == 2) {
      *response = GetUpdates(payload);
    } else {
      ++commits;
      *response = Commit(payload);
    }
    return psync::ProtocolStatus();
  }

  static std::string EntityBytes(const std::string& tag_hash, const Row& row) {
    std::string entity;
    psync::wire::PutBytesField(&entity, 1, row.id);
    psync::wire::PutVarintField(&entity, 4, static_cast<uint64_t>(row.version));
    if (row.deleted) {
      psync::wire::PutVarintField(&entity, 18, 1);
    }
    std::string phi;
    if (!row.ciphertext.empty()) {
      psync::wire::PutBytesField(&phi, 1, row.ciphertext);
    }
    std::string specifics;
    psync::wire::PutBytesField(&specifics, 2000, phi);
    psync::wire::PutBytesField(&entity, 21, specifics);
    psync::wire::PutBytesField(&entity, 23, tag_hash);
    return entity;
  }

  std::string GetUpdates(const std::string& payload) {
    // from_progress_marker(6) → token(2)
    int64_t since = 0;
    Reader reader(payload);
    while (!reader.done()) {
      int field = 0;
      WireType type = psync::wire::kVarint;
      reader.Next(&field, &type);
      if (type == psync::wire::kLengthDelimited) {
        std::string_view bytes;
        reader.Bytes(&bytes);
        if (field == 6) {
          Reader marker(bytes);
          while (!marker.done()) {
            int mf = 0;
            WireType mt = psync::wire::kVarint;
            marker.Next(&mf, &mt);
            if (mt == psync::wire::kLengthDelimited) {
              std::string_view token;
              marker.Bytes(&token);
              if (mf == 2 && !token.empty()) {
                since = std::stoll(std::string(token));
              }
            } else {
              uint64_t ignored = 0;
              marker.Varint(&ignored);
            }
          }
        }
      } else {
        reader.Skip(type, nullptr);
      }
    }
    std::vector<std::pair<int64_t, std::string>> newer;
    for (const auto& [hash, row] : rows) {
      if (row.version > since) {
        newer.push_back({row.version, hash});
      }
    }
    std::sort(newer.begin(), newer.end());
    std::string updates;
    int64_t watermark = since;
    size_t sent = 0;
    for (; sent < newer.size() && sent < page_size; ++sent) {
      const auto& [version, hash] = newer[sent];
      psync::wire::PutBytesField(&updates, 1, EntityBytes(hash, rows[hash]));
      watermark = version;
    }
    psync::wire::PutVarintField(&updates, 4, newer.size() - sent);
    std::string marker;
    psync::wire::PutVarintField(&marker, 1, 2000);
    psync::wire::PutBytesField(&marker, 2, std::to_string(watermark));
    psync::wire::PutBytesField(&updates, 5, marker);
    std::string out;
    psync::wire::PutBytesField(&out, 2, updates);
    psync::wire::PutBytesField(&out, 6, birthday);
    return out;
  }

  std::string Commit(const std::string& payload) {
    std::string commit_response;
    Reader reader(payload);
    while (!reader.done()) {
      int field = 0;
      WireType type = psync::wire::kVarint;
      reader.Next(&field, &type);
      std::string_view bytes;
      reader.Bytes(&bytes);
      if (field != 1) {
        continue;
      }
      std::string id;
      bool has_id = false;
      int64_t base = 0;
      bool deleted = false;
      std::string ciphertext;
      std::string hash;
      Reader entity(bytes);
      while (!entity.done()) {
        int ef = 0;
        WireType et = psync::wire::kVarint;
        entity.Next(&ef, &et);
        if (et == psync::wire::kVarint) {
          uint64_t number = 0;
          entity.Varint(&number);
          if (ef == 4) {
            base = static_cast<int64_t>(number);
          } else if (ef == 18) {
            deleted = number != 0;
          }
          continue;
        }
        std::string_view value;
        entity.Bytes(&value);
        if (ef == 1) {
          id = std::string(value);
          has_id = true;
        } else if (ef == 23) {
          hash = std::string(value);
        } else if (ef == 21) {
          Reader specifics(value);
          int sf = 0;
          WireType st = psync::wire::kVarint;
          while (!specifics.done() && specifics.Next(&sf, &st)) {
            std::string_view phi;
            specifics.Bytes(&phi);
            Reader inner(phi);
            int pf = 0;
            WireType pt = psync::wire::kVarint;
            while (!inner.done() && inner.Next(&pf, &pt)) {
              std::string_view c;
              inner.Bytes(&c);
              ciphertext = std::string(c);
            }
          }
        }
      }
      int response_type = 1;
      std::string out_id;
      int64_t out_version = 0;
      auto it = rows.find(hash);
      if (!has_id) {
        if (it != rows.end() && !it->second.deleted) {
          response_type = 2;  // CONFLICT: a live row holds this tag
          out_id = it->second.id;
          out_version = it->second.version;
        } else {
          Row& row = rows[hash];
          if (row.id.empty()) {
            row.id = "id-" + std::to_string(next_id++);
          }
          row.version = ++clock;
          row.ciphertext = ciphertext;
          row.deleted = deleted;
          out_id = row.id;
          out_version = row.version;
        }
      } else if (it == rows.end() || it->second.id != id) {
        response_type = 4;  // INVALID_MESSAGE
      } else if (it->second.version != base) {
        response_type = 2;
        out_version = it->second.version;
      } else {
        it->second.version = ++clock;
        it->second.ciphertext = ciphertext;
        it->second.deleted = deleted;
        out_id = it->second.id;
        out_version = it->second.version;
      }
      // EntryResponse is a proto2 GROUP (field 1).
      psync::wire::PutTag(&commit_response, 1, psync::wire::kStartGroup);
      psync::wire::PutVarintField(&commit_response, 2, response_type);
      if (!out_id.empty()) {
        psync::wire::PutBytesField(&commit_response, 3, out_id);
      }
      if (out_version) {
        psync::wire::PutVarintField(&commit_response, 6,
                                    static_cast<uint64_t>(out_version));
      }
      psync::wire::PutTag(&commit_response, 1, psync::wire::kEndGroup);
    }
    std::string out;
    psync::wire::PutBytesField(&out, 1, commit_response);
    psync::wire::PutBytesField(&out, 6, birthday);
    return out;
  }
};

// --- One device ---------------------------------------------------------------------------

struct MemoryMarker : psync::MarkerStore {
  psync::MarkerFile file;
  bool fail_save = false;
  psync::MarkerFile Load() override { return file; }
  bool Save(const psync::MarkerFile& f) override {
    if (fail_save) {
      return false;
    }
    file = f;
    return true;
  }
};

struct MemoryState : psync::EngineStateStore {
  std::map<std::string, std::string> values;
  std::optional<std::string> Get(const std::string& key) const override {
    const auto it = values.find(key);
    return it == values.end() ? std::nullopt
                              : std::optional<std::string>(it->second);
  }
  bool Set(const std::string& key,
           const std::optional<std::string>& value) override {
    if (value) {
      values[key] = *value;
    } else {
      values.erase(key);
    }
    return true;
  }
};

struct MemorySidecars : psync::SettingsSidecars {
  std::map<std::string, std::pair<int64_t, std::string>> values;
  std::optional<int64_t> Timestamp(const std::string& key) const override {
    const auto it = values.find(key);
    return it == values.end() ? std::nullopt
                              : std::optional<int64_t>(it->second.first);
  }
  std::optional<std::string> Value(const std::string& key) const override {
    const auto it = values.find(key);
    return it == values.end() ? std::nullopt
                              : std::optional<std::string>(it->second.second);
  }
  bool Set(const std::string& key, int64_t stamp,
           const std::string& value) override {
    values[key] = {stamp, value};
    return true;
  }
};

struct StaticKey : psync::DomainKeyProvider {
  keys::Bytes key;
  bool DomainKey(keys::Bytes* out) override {
    *out = key;
    return true;
  }
};

struct MemorySpaceTable : psync::SpaceTableStore {
  psync::SpaceSyncTable table;
  psync::SpaceSyncTable Load() override { return table; }
  bool Save(const psync::SpaceSyncTable& t) override {
    table = t;
    return true;
  }
};

// A device's Spaces the way LocalStore + the mapping layers hold them: the
// default Space is the local id "default-space" and maps to itself by a
// constant branch; other Spaces map through `mappings`.
struct FakeSpaces : psync::SpaceLocalAccess {
  std::vector<psync::LocalSpace> spaces;              // strip order
  std::map<std::string, std::string> profile_uuids;   // local id → account uuid
  std::map<std::string, std::string> mappings;        // space id → sync uuid
  std::set<std::string> hidden;
  int next_uuid = 1;
  std::string device;
  // The Space list cannot be asked (a stalled UI hop on Windows), or — with
  // `store_closed` — the account's store cannot open at all.
  bool unreachable = false;
  bool store_closed = false;

  psync::LocalSpace* Find(const std::string& id) {
    for (psync::LocalSpace& space : spaces) {
      if (space.space_id == id) {
        return &space;
      }
    }
    return nullptr;
  }
  std::vector<psync::LocalSpace> Visible() {
    std::vector<psync::LocalSpace> out;
    for (const psync::LocalSpace& space : spaces) {
      if (!hidden.count(space.space_id)) {
        out.push_back(space);
      }
    }
    return out;
  }
  std::vector<std::string> Names() {
    std::vector<std::string> out;
    for (const psync::LocalSpace& space : Visible()) {
      out.push_back(space.name);
    }
    return out;
  }

  std::optional<std::vector<psync::LocalSpace>> CurrentSpaces() override {
    if (unreachable) {
      return std::nullopt;
    }
    std::vector<psync::LocalSpace> out;
    for (const psync::LocalSpace& space : Visible()) {
      if (space.space_id == psync::kDefaultSpaceUuid ||
          profile_uuids.count(space.profile_id)) {
        out.push_back(space);
      }
    }
    return out;
  }
  std::optional<std::vector<psync::LocalSpace>> AllSpacesForOrdering() override {
    if (unreachable) {
      return std::nullopt;
    }
    return spaces;
  }
  std::optional<std::vector<psync::LocalSpace>> PairableSpaces() override {
    if (unreachable) {
      return std::nullopt;
    }
    return Visible();
  }
  bool SpaceStoreUnavailable() override { return unreachable && store_closed; }
  std::optional<std::string> GlobalUuid(const std::string& profile_id) override {
    const auto it = profile_uuids.find(profile_id);
    return it == profile_uuids.end() ? std::nullopt
                                     : std::optional<std::string>(it->second);
  }
  std::optional<std::string> LocalProfileId(const std::string& uuid) override {
    for (const auto& [id, global] : profile_uuids) {
      if (global == uuid) {
        return id;
      }
    }
    return std::nullopt;
  }
  psync::LocalPresence IsKnownLocalProfile(
      const std::string& profile_id) override {
    if (unreachable) {
      return psync::LocalPresence::kUnreachable;
    }
    return profile_id == "Default" || profile_uuids.count(profile_id)
               ? psync::LocalPresence::kPresent
               : psync::LocalPresence::kAbsent;
  }
  void DropProfileMapping(const std::string& profile_id) override {
    profile_uuids.erase(profile_id);
  }
  std::string DefaultProfileId() override { return "Default"; }
  std::optional<std::string> SyncUuid(const std::string& space_id) override {
    if (space_id == psync::kDefaultSpaceUuid) {
      return std::string(psync::kDefaultSpaceUuid);
    }
    const auto it = mappings.find(space_id);
    return it == mappings.end() ? std::nullopt
                                : std::optional<std::string>(it->second);
  }
  std::optional<std::string> LocalSpaceId(const std::string& uuid) override {
    if (uuid == psync::kDefaultSpaceUuid) {
      return std::string(psync::kDefaultSpaceUuid);
    }
    for (const auto& [id, mapped] : mappings) {
      if (mapped == uuid) {
        return id;
      }
    }
    return std::nullopt;
  }
  std::optional<std::string> EnsureMapped(const std::string& space_id) override {
    if (std::optional<std::string> uuid = SyncUuid(space_id)) {
      return uuid;
    }
    const std::string uuid = device + "-uuid-" + std::to_string(next_uuid++);
    mappings[space_id] = uuid;
    return uuid;
  }
  bool MapSpace(const std::string& space_id, const std::string& uuid) override {
    if (mappings.count(space_id)) {
      return false;
    }
    mappings[space_id] = uuid;
    return true;
  }
  void DropSpaceMapping(const std::string& space_id) override {
    mappings.erase(space_id);
  }
  psync::LocalPresence IsKnownLocalSpace(const std::string& space_id) override {
    if (unreachable) {
      return psync::LocalPresence::kUnreachable;
    }
    return Find(space_id) ? psync::LocalPresence::kPresent
                          : psync::LocalPresence::kAbsent;
  }
  std::map<std::string, std::string> AllSpaceMappings() override {
    return mappings;
  }
  bool IsImporting(const std::string&) override { return false; }
  psync::ProfileRefreshOutcome RefreshAccountProfiles() override {
    return psync::ProfileRefreshOutcome::kUnchanged;
  }
  bool Create(const psync::LocalSpace& space) override {
    if (Find(space.space_id)) {
      return false;
    }
    spaces.push_back(space);
    return true;
  }
  bool Update(const std::string& space_id,
              const std::optional<std::string>& name,
              const std::optional<std::string>& color_hex,
              const std::optional<std::string>& icon_name,
              std::optional<int64_t> created_ms) override {
    psync::LocalSpace* space = Find(space_id);
    if (!space) {
      return false;
    }
    if (name) space->name = *name;
    if (color_hex) space->color_hex = *color_hex;
    if (icon_name) space->icon_name = *icon_name;
    if (created_ms) space->created_ms = *created_ms;
    return true;
  }
  bool Rebind(const std::string& space_id,
              const std::string& profile_id) override {
    psync::LocalSpace* space = Find(space_id);
    if (!space) {
      return false;
    }
    space->profile_id = profile_id;
    return true;
  }
  bool ApplyThemeState(const std::string& space_id,
                       const std::optional<std::string>& theme_id,
                       std::optional<double> light,
                       std::optional<double> dark) override {
    psync::LocalSpace* space = Find(space_id);
    if (!space) {
      return false;
    }
    space->theme_id = theme_id;
    space->opacity_light = light;
    space->opacity_dark = dark;
    return true;
  }
  bool ApplyOrder(const std::vector<std::string>& ordered) override {
    std::vector<psync::LocalSpace> out;
    for (const std::string& id : ordered) {
      if (psync::LocalSpace* space = Find(id)) {
        out.push_back(*space);
      }
    }
    if (out.size() != spaces.size()) {
      return false;
    }
    spaces = std::move(out);
    return true;
  }
  bool Hide(const std::string& space_id) override {
    hidden.insert(space_id);
    return true;
  }
  bool Purge(const std::string& space_id) override {
    spaces.erase(std::remove_if(spaces.begin(), spaces.end(),
                                [&](const psync::LocalSpace& s) {
                                  return s.space_id == space_id;
                                }),
                 spaces.end());
    hidden.erase(space_id);
    return true;
  }

  // The user's edits.
  void Add(const std::string& id, const std::string& name,
           const std::string& profile, int64_t created) {
    psync::LocalSpace space;
    space.space_id = id;
    space.profile_id = profile;
    space.name = name;
    space.color_hex = "#336699";
    space.icon_name = "star";
    space.created_ms = created;
    spaces.push_back(space);
  }
};

struct MemoryOwnedTable : psync::OwnedTableStore {
  psync::OwnedTable table;
  bool exists = false;
  psync::OwnedTable Load(bool had_records, bool* reported_loss) override {
    *reported_loss = (!exists || table.cursors.empty()) && had_records;
    return exists ? table : psync::OwnedTable();
  }
  bool Save(const psync::OwnedTable& t) override {
    table = t;
    exists = true;
    return true;
  }
  bool Remove() override {
    table = psync::OwnedTable();
    exists = false;
    return true;
  }
};

// The store's bookmark rows: a Space root's children carry parent nullopt.
struct FakeBookmarks : psync::BookmarkLocalAccess {
  std::vector<psync::LocalBookmark> rows;
  int next_guid = 1;
  std::string device;

  psync::LocalBookmark* Find(const std::string& guid) {
    for (auto& row : rows) {
      if (row.guid == guid) {
        return &row;
      }
    }
    return nullptr;
  }
  psync::LocalBookmark* ByTitle(const std::string& title) {
    for (auto& row : rows) {
      if (row.title == title) {
        return &row;
      }
    }
    return nullptr;
  }
  void Sort() {
    std::sort(rows.begin(), rows.end(),
              [](const psync::LocalBookmark& a, const psync::LocalBookmark& b) {
                return std::make_tuple(a.space_id, a.parent_guid.value_or(""),
                                       a.index, a.guid) <
                       std::make_tuple(b.space_id, b.parent_guid.value_or(""),
                                       b.index, b.guid);
              });
  }
  void Normalize() {
    std::map<std::pair<std::string, std::string>,
             std::vector<psync::LocalBookmark*>> groups;
    for (auto& row : rows) {
      groups[{row.space_id, row.parent_guid.value_or("")}].push_back(&row);
    }
    for (auto& [key, members] : groups) {
      std::stable_sort(members.begin(), members.end(),
                       [](auto* a, auto* b) {
                         return a->index == b->index ? a->guid < b->guid
                                                     : a->index < b->index;
                       });
      for (size_t i = 0; i < members.size(); ++i) {
        members[i]->index = static_cast<int>(i);
      }
    }
    Sort();
  }
  // The user's edits.
  std::string Add(const std::string& title, const std::string& url,
                  std::optional<std::string> parent, bool folder,
                  int64_t now, const std::string& space = "default-space") {
    psync::LocalBookmark row;
    row.guid = device + "-G" + std::to_string(next_guid++);
    row.space_id = space;
    row.profile_id = "Default";
    row.parent_guid = parent;
    row.index = 1000;
    row.is_folder = folder;
    row.title = title;
    row.url = folder ? "https://bookmark.phi/folder" : url;
    row.created_ms = now;
    row.content_updated_ms = now;
    rows.push_back(row);
    Normalize();
    return row.guid;
  }
  std::vector<std::string> Titles(std::optional<std::string> parent,
                                  const std::string& space = "default-space") {
    Sort();
    std::vector<std::string> out;
    for (const auto& row : rows) {
      if (row.space_id == space && row.parent_guid == parent) {
        out.push_back(row.title);
      }
    }
    return out;
  }

  bool AllBookmarks(std::vector<psync::LocalBookmark>* out) override {
    Sort();
    *out = rows;
    return true;
  }
  bool AllSyncIds(std::set<std::string>* out) override {
    for (const auto& row : rows) {
      if (row.sync_id) {
        out->insert(*row.sync_id);
      }
    }
    return true;
  }
  bool CachedBookmarks(std::vector<psync::LocalBookmark>* out) override {
    return AllBookmarks(out);
  }
  std::vector<psync::LocalBookmark> Siblings(
      const std::optional<std::string>& parent,
      const std::string& space) override {
    std::vector<psync::LocalBookmark> out;
    for (const auto& row : rows) {
      if (row.space_id == space && row.parent_guid == parent) {
        out.push_back(row);
      }
    }
    return out;
  }
  bool IsKnownLocalBookmark(const std::string& guid) override {
    return Find(guid) != nullptr;
  }
  std::optional<bool> LocalIsFolder(const std::string& guid) override {
    const auto* row = Find(guid);
    return row ? std::optional<bool>(row->is_folder) : std::nullopt;
  }
  psync::BookmarkApplyResult Apply(
      const std::vector<psync::BookmarkApplyOp>& ops) override {
    const std::vector<psync::LocalBookmark> before = rows;
    auto fail = [&](psync::BookmarkApplyResult result) {
      rows = before;
      return result;
    };
    using Kind = psync::BookmarkApplyOp::Kind;
    for (const auto& op : ops) {
      switch (op.kind) {
        case Kind::kClaim: {
          auto* row = Find(op.guid);
          if (!row) {
            return fail(psync::BookmarkApplyResult::kFailed);
          }
          if (row->sync_id && *row->sync_id != op.sync_id) {
            return fail(psync::BookmarkApplyResult::kRowAlreadyMapped);
          }
          row->sync_id = op.sync_id;
          break;
        }
        case Kind::kCreate:
          if (Find(op.row.guid)) {
            return fail(psync::BookmarkApplyResult::kRowAlreadyMapped);
          }
          rows.push_back(op.row);
          break;
        case Kind::kMove: {
          auto* row = Find(op.guid);
          if (!row) {
            return fail(psync::BookmarkApplyResult::kFailed);
          }
          row->parent_guid = op.parent_guid;
          row->space_id = op.space_id;
          row->index = op.index;
          break;
        }
        case Kind::kUpdate: {
          auto* row = Find(op.guid);
          if (!row) {
            return fail(psync::BookmarkApplyResult::kFailed);
          }
          if (op.fields.title) row->title = *op.fields.title;
          if (op.fields.url) row->url = *op.fields.url;
          if (op.fields.secondary_url) row->secondary_url = *op.fields.secondary_url;
          if (op.fields.secondary_title)
            row->secondary_title = *op.fields.secondary_title;
          break;
        }
        case Kind::kDelete: {
          if (!Find(op.guid)) {
            return fail(psync::BookmarkApplyResult::kFailed);
          }
          for (const auto& row : rows) {
            if (row.parent_guid == op.guid) {
              return fail(psync::BookmarkApplyResult::kFolderNotEmpty);
            }
          }
          std::erase_if(rows, [&](const auto& r) { return r.guid == op.guid; });
          break;
        }
      }
    }
    Normalize();
    return psync::BookmarkApplyResult::kOk;
  }
};

// The store's pins under one scope.
struct FakePins : psync::PinLocalAccess {
  std::vector<psync::LocalPin> rows;
  std::string scope = "app";
  std::optional<std::string> account_scope = std::string("app");
  int next_guid = 1;
  std::string device;

  psync::LocalPin* Find(const std::string& guid) {
    for (auto& row : rows) {
      if (row.guid == guid) {
        return &row;
      }
    }
    return nullptr;
  }
  void Normalize() {
    std::map<std::string, std::vector<psync::LocalPin*>> groups;
    for (auto& row : rows) {
      groups[psync::PinKind::LocalOwnerKey(row)].push_back(&row);
    }
    for (auto& [key, members] : groups) {
      std::stable_sort(members.begin(), members.end(), [](auto* a, auto* b) {
        return a->index == b->index ? a->guid < b->guid : a->index < b->index;
      });
      for (size_t i = 0; i < members.size(); ++i) {
        members[i]->index = static_cast<int>(i);
      }
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
      return std::make_tuple(psync::PinKind::LocalOwnerKey(a), a.index, a.guid) <
             std::make_tuple(psync::PinKind::LocalOwnerKey(b), b.index, b.guid);
    });
  }
  std::string Add(const std::string& title, int64_t now) {
    psync::LocalPin row;
    row.guid = device + "-P" + std::to_string(next_guid++);
    row.lineage_id = device + "-lineage-" + std::to_string(next_guid);
    row.index = 1000;
    row.title = title;
    row.url = "https://" + title + ".example/";
    row.created_ms = now;
    row.content_updated_ms = now;
    rows.push_back(row);
    Normalize();
    return row.guid;
  }
  std::vector<std::string> Titles() {
    Normalize();
    std::vector<std::string> out;
    for (const auto& row : rows) {
      out.push_back(row.title);
    }
    return out;
  }

  std::string CurrentScope() override { return scope; }
  std::optional<std::string> AccountScope() override { return account_scope; }
  bool AllPins(std::vector<psync::LocalPin>* out) override {
    Normalize();
    *out = rows;
    return true;
  }
  bool AllPinRows(std::vector<psync::LocalPin>* out) override {
    return AllPins(out);
  }
  bool CachedPins(std::vector<psync::LocalPin>* out) override {
    return AllPins(out);
  }
  bool IsKnownLocalPin(const std::string& lineage,
                       const std::optional<std::string>& owner_key) override {
    if (!owner_key) {
      return false;
    }
    for (const auto& row : rows) {
      if (psync::PinKind::LineageKey(row.lineage_id) ==
              psync::PinKind::LineageKey(lineage) &&
          psync::PinKind::LocalOwnerKey(row) == *owner_key) {
        return true;
      }
    }
    return false;
  }
  psync::PinApplyResult Apply(const std::vector<psync::PinApplyOp>& ops) override {
    const std::vector<psync::LocalPin> before = rows;
    using Kind = psync::PinApplyOp::Kind;
    for (const auto& op : ops) {
      psync::LocalPin* row = op.kind == Kind::kCreate ? nullptr : Find(op.guid);
      if (op.kind != Kind::kCreate && !row) {
        rows = before;
        return psync::PinApplyResult::kRefused;
      }
      switch (op.kind) {
        case Kind::kCreate:
          rows.push_back(op.row);
          break;
        case Kind::kRelineage:
          row->lineage_id = op.new_lineage_id;
          break;
        case Kind::kMove:
          row->index = op.index;
          break;
        case Kind::kUpdate:
          if (op.fields.title) row->title = *op.fields.title;
          if (op.fields.url) row->url = *op.fields.url;
          if (op.fields.split_partner_lineage_id)
            row->split_partner_lineage_id = *op.fields.split_partner_lineage_id;
          break;
        case Kind::kDelete: {
          const std::string guid = op.guid;
          std::erase_if(rows, [&](const auto& r) { return r.guid == guid; });
          break;
        }
      }
    }
    Normalize();
    return psync::PinApplyResult::kOk;
  }
};

// The store's URL rules (soft-deleted ones included), written the way the
// store's own editor writes them: a new rule mints a lowercase sync id and
// carries the pending-edit flag.
struct FakeRules : psync::UrlRuleLocalAccess {
  std::vector<psync::LocalUrlRule> rows;
  int next_id = 1;
  std::string device;
  const int64_t* clock = nullptr;

  psync::LocalUrlRule* ByHost(const std::string& host) {
    for (auto& row : rows) {
      if (row.host == host && !row.deleted_ms) {
        return &row;
      }
    }
    return nullptr;
  }
  std::vector<std::string> LiveHosts() {
    std::vector<std::string> out;
    for (const auto& row : rows) {
      if (!row.deleted_ms) {
        out.push_back(row.host);
      }
    }
    std::sort(out.begin(), out.end());
    return out;
  }
  void Add(const std::string& host, const std::string& space = "default-space") {
    psync::LocalUrlRule row;
    row.id = device + "-R" + std::to_string(next_id++);
    row.sync_id = device + "-rule-" + std::to_string(next_id++);
    row.space_id = space;
    row.host = host;
    row.sort_order = static_cast<int>(rows.size());
    row.created_ms = *clock;
    row.content_updated_ms = *clock;
    row.target_updated_ms = *clock;
    row.pending_local_edit = true;
    rows.push_back(row);
  }
  void Edit(const std::string& from, const std::string& to) {
    psync::LocalUrlRule* row = ByHost(from);
    row->host = to;
    row->content_updated_ms = *clock;
    row->pending_local_edit = true;
  }
  void Remove(const std::string& host) {
    psync::LocalUrlRule* row = ByHost(host);
    row->deleted_ms = *clock;  // the editor soft-deletes
  }

  bool AllRules(std::vector<psync::LocalUrlRule>* out) override {
    *out = rows;
    return true;
  }
  bool CachedRules(std::vector<psync::LocalUrlRule>* out) override {
    *out = rows;
    return true;
  }
  psync::UrlRuleApplyError Apply(const std::vector<psync::UrlRuleSyncOp>& ops,
                                 const psync::UrlRuleMergeTail& tail,
                                 bool* failed,
                                 psync::UrlRuleBatchOutcome* outcome) override {
    *failed = false;
    return psync::ApplyUrlRuleOps(
        &rows, ops, tail,
        [this] { return device + "-R" + std::to_string(next_id++); }, *clock,
        outcome);
  }
  void HardDelete(const std::string& sync_id) override {
    std::erase_if(rows, [&](const auto& r) { return r.sync_id == sync_id; });
  }
  int PurgeSoftDeleted(int64_t cutoff) override {
    const size_t before = rows.size();
    std::erase_if(rows, [&](const auto& r) {
      return r.deleted_ms && *r.deleted_ms < cutoff;
    });
    return static_cast<int>(before - rows.size());
  }
  void ClearPendingLocalEdits(
      const std::map<std::string, psync::RuleProjection>& entries,
      bool clears_partner) override {
    for (const auto& [sync_id, confirmed] : entries) {
      psync::ClearRulePendingLocalEdit(&rows, sync_id, confirmed, clears_partner);
    }
  }
};

// A device's preferences: a theme (string), a layout (string, only three
// values fit) and a bool.
struct Device {
  Device(FakeServer& server, const keys::Bytes& key, int64_t* clock,
         const std::string& name = std::string(), bool sync_default_space = false)
      : client(server, [] { return std::string("device"); }) {
    domain.key = key;
    prefs["PhiCurrentThemeId"] = "default";
    prefs["layoutMode"] = "balanced";
    prefs["alwaysShowURLPath"] = "0";
    psync::EngineDeps deps;
    deps.client = &client;
    deps.domain_keys = &domain;
    deps.marker_store = &marker;
    deps.state = &state;
    deps.sidecars = &sidecars;
    deps.now = [clock] { return *clock; };
    deps.build_identity = "test/1";
    deps.settings = {StringSetting("PhiCurrentThemeId", {}),
                     StringSetting("layoutMode",
                                   {"balanced", "comfortable", "performance"}),
                     BoolSetting("alwaysShowURLPath")};
    if (sync_default_space) {
      prefs["PhiDefaultSpaceUuid"] = psync::kDefaultSpaceUuid;
      auto setting = StringSetting("PhiDefaultSpaceUuid", {});
      setting.reconcile = [this] {
        const auto id = local.LocalSpaceId(prefs["PhiDefaultSpaceUuid"]);
        if (id && local.IsKnownLocalSpace(*id) ==
                      psync::LocalPresence::kPresent) {
          applied_default_space = *id;
        }
      };
      deps.settings.push_back(std::move(setting));
    }
    if (!name.empty()) {
      // The Space section: every device has the default Space in its
      // default profile, which maps to the account's "profile-default".
      local.device = name;
      local.profile_uuids["Default"] = "profile-default";
      local.Add(psync::kDefaultSpaceUuid, "Default", "Default", 1);
      deps.space_access = &local;
      deps.space_store = &spaces;
      deps.new_space_id = [this, name] {
        return name + "-local-" + std::to_string(next_local_id++);
      };
      bookmarks.device = name;
      pins.device = name;
      bookmark_kind = std::make_unique<psync::BookmarkOwnedKind>(
          bookmarks, bookmark_table,
          [this, name] { return name + "-uuid-b" + std::to_string(next_local_id++); },
          [this, name] { return name + "-G" + std::to_string(next_local_id++); });
      pin_kind = std::make_unique<psync::PinOwnedKind>(
          pins, pin_table,
          [this, name] { return name + "-P" + std::to_string(next_local_id++); });
      rules.device = name;
      rules.clock = clock;
      rule_kind = std::make_unique<psync::UrlRuleOwnedKind>(rules, rule_table);
      deps.owned_kinds = {bookmark_kind.get(), pin_kind.get(), rule_kind.get()};
    }
    engine = std::make_unique<psync::PhiSyncEngine>(std::move(deps));
  }

  psync::SyncableSetting StringSetting(const std::string& key,
                                       std::vector<std::string> allowed) {
    return {key,
            [this, key] {
              psync::SettingValue v;
              v.SetString(prefs[key]);
              return std::optional<psync::SettingValue>(v);
            },
            [this, key, allowed](const psync::SettingValue& v) {
              if (v.kind != psync::SettingValue::Kind::kString ||
                  v.string_value.empty() ||
                  (!allowed.empty() &&
                   std::find(allowed.begin(), allowed.end(), v.string_value) ==
                       allowed.end())) {
                return;  // refused
              }
              prefs[key] = v.string_value;
            }};
  }

  psync::SyncableSetting BoolSetting(const std::string& key) {
    return {key,
            [this, key] {
              psync::SettingValue v;
              v.SetBool(prefs[key] == "1");
              return std::optional<psync::SettingValue>(v);
            },
            [this, key](const psync::SettingValue& v) {
              if (v.kind == psync::SettingValue::Kind::kBool) {
                prefs[key] = v.bool_value ? "1" : "0";
              }
            }};
  }

  psync::SyncProtocolClient client;
  StaticKey domain;
  MemoryMarker marker;
  MemoryState state;
  MemorySidecars sidecars;
  MemorySpaceTable spaces;
  FakeSpaces local;
  MemoryOwnedTable bookmark_table;
  MemoryOwnedTable pin_table;
  FakeBookmarks bookmarks;
  FakePins pins;
  std::unique_ptr<psync::BookmarkOwnedKind> bookmark_kind;
  std::unique_ptr<psync::PinOwnedKind> pin_kind;
  MemoryOwnedTable rule_table;
  FakeRules rules;
  std::unique_ptr<psync::UrlRuleOwnedKind> rule_kind;
  int next_local_id = 1;
  std::string applied_default_space = psync::kDefaultSpaceUuid;
  std::map<std::string, std::string> prefs;
  std::unique_ptr<psync::PhiSyncEngine> engine;
};

std::string ServerSetting(FakeServer& server, const keys::Bytes& key,
                          const std::string& name) {
  const auto it = server.rows.find(psync::SettingsClientTagHash());
  if (it == server.rows.end() || it->second.deleted) {
    return "<none>";
  }
  const std::optional<keys::Bytes> plain = keys::OpenWithSymmetric(
      keys::Bytes(it->second.ciphertext.begin(), it->second.ciphertext.end()),
      key);
  psync::PhiEntity entity;
  if (!plain || !entity.ParseFrom(std::string(plain->begin(), plain->end()))) {
    return "<unreadable>";
  }
  const auto value = entity.setting.values.find(name);
  if (value == entity.setting.values.end()) {
    return "<absent>";
  }
  return value->second.kind == psync::SettingValue::Kind::kBool
             ? (value->second.bool_value ? "1" : "0")
             : value->second.string_value;
}

// --- Scenarios --------------------------------------------------------------------------

void TestFirstDevicePublishesSecondAdopts() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock);
  a.prefs["PhiCurrentThemeId"] = "ocean";
  a.engine->PullOnce();
  Check(ServerSetting(server, key, "PhiCurrentThemeId") == "ocean" &&
            a.engine->has_adopted(),
        "the first device publishes its settings");

  // A fresh device with different defaults adopts the account wholesale:
  // its own never-edited values must not win by being stamped now.
  clock += 10'000;
  Device b(server, key, &clock);
  b.prefs["PhiCurrentThemeId"] = "forest";
  b.engine->PullOnce();
  Check(b.prefs["PhiCurrentThemeId"] == "ocean" && b.engine->has_adopted() &&
            ServerSetting(server, key, "PhiCurrentThemeId") == "ocean",
        "a fresh device adopts the account instead of overwriting it");
  Check(b.engine->last_outcome() == psync::RoundOutcome::kOk,
        "the round ended ok");
}

void TestStatusPhases() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock);
  Check(a.engine->status().phase == psync::SyncPhase::kChecking &&
            !a.engine->status().last_success_ms,
        "status starts as checking");
  a.engine->PullOnce();
  const psync::SyncStatus done = a.engine->status();
  Check(done.phase == psync::SyncPhase::kUpToDate && done.last_success_ms,
        "a drained round is up to date, with its success time");
  server.offline = true;
  clock += 1000;
  a.engine->PullOnce();
  Check(a.engine->status().phase == psync::SyncPhase::kOffline &&
            a.engine->status().last_success_ms == done.last_success_ms,
        "a transport failure is offline and keeps the last success");
  server.offline = false;
  clock += 1000;
  a.engine->PullOnce();
  Check(a.engine->status().phase == psync::SyncPhase::kUpToDate &&
            a.engine->status().revision > done.revision,
        "the next good round is up to date again");
}

void TestEditsFlowBothWays() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock);
  Device b(server, key, &clock);
  a.engine->PullOnce();
  b.engine->PullOnce();

  clock += 1'000;
  b.prefs["layoutMode"] = "comfortable";
  b.engine->HandleLocalSettingsChange();
  a.engine->PullOnce();
  Check(a.prefs["layoutMode"] == "comfortable",
        "a local change reaches the other device");

  // Different keys edited on both sides both survive.
  clock += 1'000;
  a.prefs["alwaysShowURLPath"] = "1";
  a.engine->HandleLocalSettingsChange();
  clock += 1'000;
  b.prefs["PhiCurrentThemeId"] = "sunset";
  b.engine->HandleLocalSettingsChange();
  a.engine->PullOnce();
  b.engine->PullOnce();
  Check(a.prefs["alwaysShowURLPath"] == "1" &&
            b.prefs["alwaysShowURLPath"] == "1" &&
            a.prefs["PhiCurrentThemeId"] == "sunset" &&
            b.prefs["PhiCurrentThemeId"] == "sunset",
        "concurrent edits of different keys both survive");

  // The same key edited on both: the later edit wins everywhere.
  clock += 1'000;
  a.prefs["layoutMode"] = "performance";
  a.engine->HandleLocalSettingsChange();
  clock += 1'000;
  b.prefs["layoutMode"] = "balanced";
  b.engine->HandleLocalSettingsChange();
  a.engine->PullOnce();
  b.engine->PullOnce();
  Check(a.prefs["layoutMode"] == "balanced" &&
            b.prefs["layoutMode"] == "balanced" &&
            ServerSetting(server, key, "layoutMode") == "balanced",
        "the later edit of one key wins on every device");

  // Steady state: nothing to publish, no commit.
  const int commits = server.commits;
  a.engine->PullOnce();
  b.engine->PullOnce();
  Check(server.commits == commits, "a converged pull publishes nothing");
}

void TestConflictRetry() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock);
  Device b(server, key, &clock);
  a.engine->PullOnce();
  b.engine->PullOnce();
  // A publishes; B's cursor is now behind. B's push pulls first, so to force
  // a CONFLICT the server version moves between B's pull and commit: make A
  // commit, then rewind B's view by editing the row version out of band.
  clock += 1'000;
  a.prefs["PhiCurrentThemeId"] = "night";
  a.engine->HandleLocalSettingsChange();
  clock += 1'000;
  b.prefs["alwaysShowURLPath"] = "1";
  b.engine->HandleLocalSettingsChange();
  Check(ServerSetting(server, key, "PhiCurrentThemeId") == "night" &&
            ServerSetting(server, key, "alwaysShowURLPath") == "1",
        "a push behind the server merges and publishes both edits");
}

void TestRefusedValueIsNotEchoed() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock);
  Device b(server, key, &clock);
  a.engine->PullOnce();
  b.engine->PullOnce();
  // A newer peer publishes a layout this build does not know.
  clock += 1'000;
  a.prefs["layoutMode"] = "futuristic";
  a.engine->HandleLocalSettingsChange();
  const int commits = server.commits;
  b.engine->PullOnce();
  Check(b.prefs["layoutMode"] == "balanced" &&
            ServerSetting(server, key, "layoutMode") == "futuristic" &&
            server.commits == commits,
        "a value that does not fit is refused, and not pushed back over");
}

void TestTombstoneHeal() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock);
  a.engine->PullOnce();
  // Another device deletes the settings row.
  Row& row = server.rows[psync::SettingsClientTagHash()];
  row.deleted = true;
  row.ciphertext.clear();
  row.version = ++server.clock;
  const int commits = server.commits;
  a.engine->PullOnce();
  Check(server.commits == commits &&
            a.engine->last_outcome() == psync::RoundOutcome::kUnusableSettings,
        "a tombstone is not published over");
  a.engine->PullOnce();
  a.engine->PullOnce();
  clock += 1'000;
  a.prefs["PhiCurrentThemeId"] = "reborn";
  a.engine->HandleLocalSettingsChange();
  Check(ServerSetting(server, key, "PhiCurrentThemeId") == "reborn",
        "after the heal a local change re-creates the row");
}

void TestUndecryptableIsNeverOverwritten() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock);
  a.engine->PullOnce();
  // A device holding another key (a re-mint this one has not caught up with).
  Device b(server, keys::GenerateArk(), &clock);
  b.engine->PullOnce();
  clock += 1'000;
  b.prefs["PhiCurrentThemeId"] = "clobber";
  b.engine->HandleLocalSettingsChange();
  // The refusal is durable (the record and the dropped baseline), not the
  // round outcome: the marker already walked past the entity (review A3).
  Check(ServerSetting(server, key, "PhiCurrentThemeId") == "default" &&
            b.prefs["PhiCurrentThemeId"] == "clobber",
        "an unreadable entity is never overwritten");
  Check(b.marker.file.marker.has_value(),
        "a refusal on record does not rewind the marker every round");
  // The key catches up: one replay re-reads it.
  b.domain.key = key;
  b.engine->PullOnce();
  Check(b.engine->last_outcome() == psync::RoundOutcome::kOk,
        "a new key replays and reads the entity");
}

void TestPagingAndBirthday() {
  FakeServer server;
  server.page_size = 1;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  // Unrelated rows (other kinds) spread over several pages.
  for (int i = 0; i < 5; ++i) {
    Row& row = server.rows["other-" + std::to_string(i)];
    row.id = "o" + std::to_string(i);
    row.version = ++server.clock;
    row.ciphertext = "x";
  }
  Device a(server, key, &clock);
  a.engine->PullOnce();
  Check(a.engine->last_pages() >= 5 &&
            a.engine->last_outcome() == psync::RoundOutcome::kOk &&
            ServerSetting(server, key, "layoutMode") == "balanced",
        "a paged drain completes and then publishes");

  server.birthday = "birthday-2";  // the store was reset
  a.engine->PullOnce();
  Check(a.engine->requires_reconfiguration() &&
            a.engine->last_outcome() == psync::RoundOutcome::kNotMyBirthday,
        "NOT_MY_BIRTHDAY pauses the engine for reconfiguration");
  const int commits = server.commits;
  a.engine->PullOnce();
  Check(server.commits == commits, "a paused engine does nothing");

  a.engine->Stop();
  psync::CleanupActions cleanup{
      [] { return true; }, [] { return true; }, [] { return true; },
      [&] { a.state.values.clear(); a.sidecars.values.clear(); return true; },
      [] { return true; }, [&] { a.marker.file = {}; return true; }};
  Check(psync::CleanupSyncState(a.marker, false, cleanup),
        "confirmed server-reset cleanup completes without device removal");
  psync::EngineDeps deps;
  deps.client = &a.client;
  deps.domain_keys = &a.domain;
  deps.marker_store = &a.marker;
  deps.state = &a.state;
  deps.sidecars = &a.sidecars;
  deps.now = [&] { return clock; };
  deps.settings = {a.StringSetting("layoutMode", {})};
  a.engine = std::make_unique<psync::PhiSyncEngine>(std::move(deps));
  a.engine->PullOnce();
  Check(!a.engine->requires_reconfiguration() &&
            a.engine->last_outcome() == psync::RoundOutcome::kOk &&
            a.marker.file.store_birthday == "birthday-2",
        "setup after cleanup accepts the new server birthday and resumes syncing");
}

void TestClockCorrection() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000'000;
  server.date_ms = clock + 3'600'000;  // this device runs an hour behind
  Device a(server, key, &clock);
  a.engine->PullOnce();
  Check(a.engine->wall_clock_offset_ms() == 3'600'000,
        "the Date header corrects a clock an hour behind");
}

// --- Spaces -------------------------------------------------------------------------------

std::optional<psync::SpaceEntity> ServerSpace(FakeServer& server,
                                              const keys::Bytes& key,
                                              const std::string& uuid) {
  const auto it =
      server.rows.find(psync::ClientTagHash(psync::SpaceClientTag(uuid)));
  if (it == server.rows.end() || it->second.deleted) {
    return std::nullopt;
  }
  const std::optional<keys::Bytes> plain = keys::OpenWithSymmetric(
      keys::Bytes(it->second.ciphertext.begin(), it->second.ciphertext.end()),
      key);
  psync::PhiEntity entity;
  if (!plain || !entity.ParseFrom(std::string(plain->begin(), plain->end())) ||
      entity.kind != psync::PhiEntity::Kind::kSpace) {
    return std::nullopt;
  }
  return entity.space;
}

bool Deleted(FakeServer& server, const std::string& uuid) {
  const auto it =
      server.rows.find(psync::ClientTagHash(psync::SpaceClientTag(uuid)));
  return it != server.rows.end() && it->second.deleted;
}

using Names = std::vector<std::string>;

void TestSpacesGateAndFirstPublish() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.local.Add("A-WORK", "Work", "Default", 2);
  a.engine->PullOnce();
  Check(a.engine->last_outcome() == psync::RoundOutcome::kGated &&
            a.local.mappings.empty() && server.rows.size() == 1,
        "a shut gate publishes no Space and mints no mapping");
  Check(a.spaces.table.marker_moved_while_gate_shut,
        "a shut gate records that the marker walked on");

  a.engine->SetSpaceSyncEnabled(true);
  Check(a.spaces.table.drain_in_progress && !a.marker.file.marker,
        "opening the gate arms a replay, marker first");
  a.engine->PullOnce();
  const std::string work_uuid = a.local.mappings["A-WORK"];
  const auto work = ServerSpace(server, key, work_uuid);
  const auto def = ServerSpace(server, key, psync::kDefaultSpaceUuid);
  Check(a.engine->last_outcome() == psync::RoundOutcome::kOk &&
            a.spaces.table.has_drained_full_replay && work && def &&
            work->name.get().str() == "Work" &&
            work->profile_uuid.get().str() == "profile-default" &&
            def->name.get().str() == "Default",
        "after the replay the device publishes its Spaces");
  Check(work && def && def->rank.get().str() < work->rank.get().str(),
        "the published ranks keep the strip order");

  const int commits = server.commits;
  a.engine->PullOnce();
  Check(server.commits == commits, "a converged Space pull publishes nothing");
}

void TestSpacesReachAFreshDevice() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.local.Add("A-WORK", "Work", "Default", 2);
  a.local.spaces[0].name = "Home";
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();

  clock += 10'000;
  Device b(server, key, &clock, "b");
  b.local.Add("B-FACTORY", "Scratch", "Default", 3);
  b.engine->SetSpaceSyncEnabled(true);
  const int commits_before = server.commits;
  b.engine->PullOnce();
  // Its own unsynced Space keeps its slot; its derived rank lands between
  // its neighbours, so every device ends in the same order.
  Check(b.local.Names() == Names({"Home", "Scratch", "Work"}),
        "a fresh device lands the account's Spaces around its own");
  Check(b.local.mappings.count("B-FACTORY") &&
            ServerSpace(server, key, b.local.mappings["B-FACTORY"]) &&
            ServerSpace(server, key, psync::kDefaultSpaceUuid)->name.get().str() ==
                "Home",
        "its own Space is added; the account's default Space is not overwritten");
  Check(server.commits > commits_before, "the new Space was committed");

  a.engine->PullOnce();
  Check(a.local.Names() == Names({"Home", "Scratch", "Work"}),
        "the first device lands the new Space in the account's order");
  const int commits = server.commits;
  a.engine->PullOnce();
  b.engine->PullOnce();
  Check(server.commits == commits, "two converged devices publish nothing");
}

void TestSpaceEditsFlowBothWays() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.local.Add("A-WORK", "Work", "Default", 2);
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();
  Device b(server, key, &clock, "b");
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  const std::string b_work = *b.local.LocalSpaceId(a.local.mappings["A-WORK"]);

  // A rename on B reaches A.
  clock += 1'000;
  b.local.Find(b_work)->name = "Office";
  b.engine->HandleLocalSpaceChange();
  a.engine->PullOnce();
  Check(a.local.Find("A-WORK")->name == "Office",
        "a Space rename reaches the other device");

  // Different fields edited on both sides both survive.
  clock += 1'000;
  a.local.Find("A-WORK")->color_hex = "#ff0000";
  a.engine->HandleLocalSpaceChange();
  clock += 1'000;
  b.local.Find(b_work)->icon_name = "briefcase";
  b.engine->HandleLocalSpaceChange();
  a.engine->PullOnce();
  b.engine->PullOnce();
  Check(a.local.Find("A-WORK")->color_hex == "#ff0000" &&
            a.local.Find("A-WORK")->icon_name == "briefcase" &&
            b.local.Find(b_work)->color_hex == "#ff0000" &&
            b.local.Find(b_work)->icon_name == "briefcase",
        "concurrent edits of different Space fields both survive");

  // A reorder on A moves the Space on B.
  clock += 1'000;
  std::swap(a.local.spaces[0], a.local.spaces[1]);
  a.engine->HandleLocalSpaceChange();
  b.engine->PullOnce();
  Check(b.local.Names() == Names({"Office", "Default"}),
        "a reorder reaches the other device");

  const int commits = server.commits;
  a.engine->PullOnce();
  b.engine->PullOnce();
  Check(server.commits == commits, "edits converge");
}

void TestSpaceDeletionAndRetention() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.local.Add("A-WORK", "Work", "Default", 2);
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();
  Device b(server, key, &clock, "b");
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  const std::string uuid = a.local.mappings["A-WORK"];

  // B deletes it.
  const std::string b_work = *b.local.LocalSpaceId(uuid);
  b.engine->RecordLocalSpaceDeletion(b_work);
  b.local.Purge(b_work);
  b.engine->HandleLocalSpaceChange();
  Check(Deleted(server, uuid), "a local deletion commits a tombstone");
  a.engine->PullOnce();
  Check(a.local.Names() == Names({"Default"}) && a.local.Find("A-WORK"),
        "a remote deletion hides the Space, kept for retention");

  // The default Space is never hidden when it is the last one.
  Row& row = server.rows[psync::ClientTagHash(
      psync::SpaceClientTag(psync::kDefaultSpaceUuid))];
  row.deleted = true;
  row.ciphertext.clear();
  row.version = ++server.clock;
  a.engine->PullOnce();
  Check(a.local.Names() == Names({"Default"}) &&
            a.spaces.table.cursors[psync::kDefaultSpaceUuid].pending_tombstone,
        "the last live Space is never hidden by a tombstone");

  clock += psync::kSpaceRetentionMs + 1;
  a.engine->RetentionSweep();
  Check(!a.local.Find("A-WORK") && !a.local.mappings.count("A-WORK") &&
            a.spaces.table.cursors[uuid].purged_at_ms,
        "retention purges the hidden Space and keeps a bare tombstone");
}

void TestSpaceParksUntilItsProfileExists() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.local.profile_uuids["Profile 1"] = "profile-work";
  a.local.Add("A-WORK", "Work", "Profile 1", 2);
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();
  const std::string uuid = a.local.mappings["A-WORK"];

  Device b(server, key, &clock, "b");
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  Check(b.local.Names() == Names({"Default"}) &&
            b.spaces.table.cursors[uuid].pending_apply,
        "a Space whose profile is not here yet parks");
  const auto before = ServerSpace(server, key, uuid);
  Check(before && before->name.get().str() == "Work",
        "a parked Space is never published over");

  b.local.profile_uuids["Profile 7"] = "profile-work";
  b.engine->PullOnce();
  const std::optional<std::string> landed = b.local.LocalSpaceId(uuid);
  Check(landed && b.local.Find(*landed) &&
            b.local.Find(*landed)->profile_id == "Profile 7" &&
            !b.spaces.table.cursors[uuid].pending_apply,
        "the parked Space lands once its profile exists");
}

void TestUnreadableSpaceIsNeverOverwritten() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();
  const std::string row_before =
      server.rows[psync::ClientTagHash(
                      psync::SpaceClientTag(psync::kDefaultSpaceUuid))]
          .ciphertext;
  Device b(server, keys::GenerateArk(), &clock, "b");
  b.local.spaces[0].name = "Clobber";
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  Check(server.rows[psync::ClientTagHash(
                        psync::SpaceClientTag(psync::kDefaultSpaceUuid))]
                    .ciphertext == row_before &&
            !b.spaces.table.unreadable_tag_hashes.empty(),
        "an unreadable Space row is recorded and never committed over");
}

// --- Bookmarks and pins ------------------------------------------------------------

int CountKind(FakeServer& server, const keys::Bytes& key,
              psync::PhiEntity::Kind kind) {
  int count = 0;
  for (const auto& [hash, row] : server.rows) {
    if (row.deleted || row.ciphertext.empty()) {
      continue;
    }
    const std::optional<keys::Bytes> plain = keys::OpenWithSymmetric(
        keys::Bytes(row.ciphertext.begin(), row.ciphertext.end()), key);
    psync::PhiEntity entity;
    if (plain && entity.ParseFrom(std::string(plain->begin(), plain->end())) &&
        entity.kind == kind) {
      ++count;
    }
  }
  return count;
}

void TestBookmarksReachAFreshDevice() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  const std::string folder = a.bookmarks.Add("Folder", "", std::nullopt, true, clock);
  a.bookmarks.Add("Inside", "https://inside.example/", folder, false, clock);
  a.bookmarks.Add("Root", "https://root.example/", std::nullopt, false, clock);
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();
  Check(CountKind(server, key, psync::PhiEntity::Kind::kBookmark) == 3,
        "a device publishes its bookmark tree");

  Device b(server, key, &clock, "b");
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  const auto* landed_folder = b.bookmarks.ByTitle("Folder");
  Check(b.bookmarks.Titles(std::nullopt) == Names({"Folder", "Root"}) &&
            landed_folder &&
            b.bookmarks.Titles(landed_folder->guid) == Names({"Inside"}),
        "a fresh device lands the tree in order");
  const int commits = server.commits;
  a.engine->PullOnce();
  b.engine->PullOnce();
  Check(server.commits == commits, "converged bookmarks publish nothing");

  // A rename on B reaches A.
  clock += 1'000;
  b.bookmarks.ByTitle("Inside")->title = "Renamed";
  b.bookmarks.ByTitle("Renamed")->content_updated_ms = clock;
  b.engine->HandleLocalOwnedChange();
  a.engine->PullOnce();
  Check(a.bookmarks.ByTitle("Renamed") != nullptr,
        "a bookmark rename reaches the other device");

  // A deletes the folder (and its child); B follows.
  clock += 1'000;
  const std::string a_folder = a.bookmarks.ByTitle("Folder")->guid;
  std::erase_if(a.bookmarks.rows, [&](const auto& r) {
    return r.guid == a_folder || r.parent_guid == a_folder;
  });
  a.engine->HandleLocalOwnedChange();
  // The folder's own tombstone waits one round for its child's to be
  // accepted (an ancestor never goes before its descendants).
  a.engine->PullOnce();
  b.engine->PullOnce();
  Check(b.bookmarks.Titles(std::nullopt) == Names({"Root"}) &&
            !b.bookmarks.ByTitle("Renamed"),
        "a deleted folder goes with its child everywhere");
}

void TestBookmarkAdoptionClaimsATwin() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.bookmarks.Add("Shared", "https://shared.example/", std::nullopt, false, clock);
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();

  // B already has the same bookmark, unsynced.
  Device b(server, key, &clock, "b");
  b.bookmarks.Add("Shared", "https://shared.example/", std::nullopt, false, clock);
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  Check(b.bookmarks.rows.size() == 1 && b.bookmarks.rows[0].sync_id &&
            CountKind(server, key, psync::PhiEntity::Kind::kBookmark) == 1,
        "an arriving bookmark claims the local twin instead of duplicating");
}

void TestPinsSyncBothWays() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.pins.Add("one", clock);
  a.pins.Add("two", clock);
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();
  Check(CountKind(server, key, psync::PhiEntity::Kind::kPinTab) == 2,
        "a device publishes its pins");

  Device b(server, key, &clock, "b");
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  Check(b.pins.Titles() == Names({"one", "two"}),
        "a fresh device lands the pins in order");

  // A reorder on B reaches A.
  clock += 1'000;
  b.pins.rows[0].index = 5;
  b.pins.Normalize();
  b.engine->HandleLocalOwnedChange();
  a.engine->PullOnce();
  Check(a.pins.Titles() == Names({"two", "one"}), "a pin reorder reaches the other device");

  // An unpin on A reaches B.
  clock += 1'000;
  std::erase_if(a.pins.rows, [](const auto& r) { return r.title == "one"; });
  a.engine->HandleLocalOwnedChange();
  b.engine->PullOnce();
  Check(b.pins.Titles() == Names({"two"}), "an unpin reaches the other device");

  const int commits = server.commits;
  a.engine->PullOnce();
  b.engine->PullOnce();
  Check(server.commits == commits, "converged pins publish nothing");
}

void TestPinScopeMismatchParks() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.pins.Add("one", clock);
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();

  Device b(server, key, &clock, "b");
  b.pins.scope = "profile";  // the account says "app"
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  Check(b.pins.rows.empty() && !b.pin_table.table.cursors.empty() &&
            b.pin_table.table.cursors.begin()->second.pending_apply,
        "a scope mismatch parks the account's pins");
  b.pins.scope = "app";  // the scope migration ran
  b.engine->PullOnce();
  Check(b.pins.Titles() == Names({"one"}), "the parked pin lands once the scopes agree");
}

void TestANewKindReplaysOnce() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.bookmarks.Add("Old", "https://old.example/", std::nullopt, false, clock);
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();

  // B drained the account with a build that had no owned kinds: its marker
  // is past the bookmark, which nothing routed.
  Device b(server, key, &clock, "b");
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  b.bookmarks.rows.clear();
  b.bookmark_table.Remove();
  b.pin_table.Remove();
  b.spaces.table.replayed_kinds.clear();
  b.spaces.table.bookmarks_had_records = false;
  b.spaces.table.pins_had_records = false;
  b.engine->PullOnce();
  Check(b.bookmarks.ByTitle("Old") != nullptr &&
            b.spaces.table.replayed_kinds.count("bookmarks"),
        "a kind the last replay did not route replays the type once");
  const int pages = b.engine->last_pages();
  b.engine->PullOnce();
  Check(b.engine->last_pages() <= pages && b.spaces.table.has_drained_full_replay,
        "and only once");
}

int LiveRuleRows(FakeServer& server, const keys::Bytes& key) {
  return CountKind(server, key, psync::PhiEntity::Kind::kUrlRule);
}

void TestRulesSyncBothWays() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.rules.Add("github.com");
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();
  Check(LiveRuleRows(server, key) == 1 && !a.rules.ByHost("github.com")->pending_local_edit,
        "a device publishes its rule and clears the pending edit");

  Device b(server, key, &clock, "b");
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  Check(b.rules.LiveHosts() == Names({"github.com"}), "a fresh device lands the rule");

  clock += 1'000;
  b.rules.Edit("github.com", "gitlab.com");
  b.engine->HandleLocalOwnedChange();
  a.engine->PullOnce();
  Check(a.rules.LiveHosts() == Names({"gitlab.com"}), "a rule edit reaches the other device");

  clock += 1'000;
  a.rules.Remove("gitlab.com");
  a.engine->HandleLocalOwnedChange();
  Check(a.rules.rows.empty(), "a published rule tombstone hard-deletes the soft-deleted row");
  b.engine->PullOnce();
  Check(b.rules.LiveHosts().empty(), "a rule delete reaches the other device");

  const int commits = server.commits;
  a.engine->PullOnce();
  b.engine->PullOnce();
  Check(server.commits == commits, "converged rules publish nothing");
}

void TestRuleClaimsATwin() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.rules.Add("docs.example");
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();

  Device b(server, key, &clock, "b");
  b.rules.Add("DOCS.example.");  // the same rule, not yet normalized
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  Check(b.rules.rows.size() == 1 && LiveRuleRows(server, key) == 1 &&
            b.rules.rows[0].sync_id == a.rules.rows[0].sync_id,
        "an arriving rule claims the local twin instead of duplicating");
}

void TestDuplicateRulesCollapse() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.rules.Add("one.example");
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();
  Device b(server, key, &clock, "b");
  b.rules.Add("two.example");
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  a.engine->PullOnce();

  // B edits its rule into a duplicate of A's.
  clock += 1'000;
  b.rules.Edit("two.example", "one.example");
  b.engine->HandleLocalOwnedChange();
  for (int round = 0; round < 3; ++round) {
    a.engine->PullOnce();
    b.engine->PullOnce();
  }
  Check(a.rules.LiveHosts() == Names({"one.example"}) &&
            b.rules.LiveHosts() == Names({"one.example"}) &&
            LiveRuleRows(server, key) == 1,
        "duplicate rules collapse into one everywhere");
}

void TestSameNamedSpacesRemainSeparate() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.local.Add("A-WORK", "Work", "Default", 2);
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();

  Device b(server, key, &clock, "b");
  b.local.Add("B-WORK", "Work", "Default", 3);
  b.local.Find("B-WORK")->color_hex = "#112233";
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  Check(b.local.spaces.size() == 3 &&
            b.local.Find("B-WORK")->color_hex == "#112233" &&
            b.local.mappings["B-WORK"] != a.local.mappings["A-WORK"],
        "same-named Spaces retain separate identities and local fields");

  Device c(server, key, &clock, "c");
  c.local.Add("C-WORK", "Work", "Default", 3);
  c.local.MapSpace("C-WORK", a.local.mappings["A-WORK"]);
  c.engine->SetSpaceSyncEnabled(true);
  c.engine->PullOnce();
  Check(c.local.spaces.size() == 3 &&
            c.local.mappings["C-WORK"] == a.local.mappings["A-WORK"],
        "an existing explicit Space mapping still adopts the account Space");
}

void TestUnreachableSpaceListNeverDuplicates() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a");
  a.local.Add("A-WORK", "Work", "Default", 2);
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();
  Device b(server, key, &clock, "b");
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  const std::string b_work = *b.local.LocalSpaceId(a.local.mappings["A-WORK"]);

  // A rename arrives while B's Space list cannot be asked (a UI stall): no
  // answer is never "gone", so nothing is dropped, created or published, and
  // the page waits behind its marker.
  clock += 1'000;
  a.local.Find("A-WORK")->name = "Office";
  a.engine->HandleLocalSpaceChange();
  const auto marker_before = b.marker.file.marker;
  const int commits = server.commits;
  b.local.unreachable = true;
  b.engine->PullOnce();
  b.local.unreachable = false;
  Check(b.local.spaces.size() == 2 && b.local.mappings.count(b_work) &&
            b.local.Find(b_work)->name == "Work" &&
            b.local.profile_uuids.count("Default"),
        "an unreachable Space list keeps every mapping and lands no Space");
  Check(b.engine->last_outcome() == psync::RoundOutcome::kLocalReadFailed &&
            b.marker.file.marker == marker_before &&
            server.commits == commits,
        "the round stops behind its marker and publishes nothing");

  // The next round replays the page into the same Space.
  b.engine->PullOnce();
  Check(b.local.spaces.size() == 2 && b.local.Find(b_work)->name == "Office" &&
            b.engine->last_outcome() == psync::RoundOutcome::kOk,
        "the next round lands the rename in place");

  // A store that cannot open is no stall: the arrival parks, nothing is
  // dropped or created, and the marker (with every other kind) moves on.
  clock += 1'000;
  a.local.Find("A-WORK")->name = "Studio";
  a.engine->HandleLocalSpaceChange();
  const auto parked_from = b.marker.file.marker;
  b.local.unreachable = true;
  b.local.store_closed = true;
  b.engine->PullOnce();
  b.local.unreachable = false;
  b.local.store_closed = false;
  const std::string work_uuid = a.local.mappings["A-WORK"];
  Check(b.local.spaces.size() == 2 && b.local.mappings.count(b_work) &&
            b.local.Find(b_work)->name == "Office" &&
            b.spaces.table.cursors[work_uuid].pending_apply.has_value(),
        "a store that cannot open parks the arrival and keeps every mapping");
  Check(b.engine->last_outcome() != psync::RoundOutcome::kLocalReadFailed &&
            b.marker.file.marker != parked_from,
        "a store that cannot open does not hold the marker");
  b.engine->PullOnce();
  Check(b.local.spaces.size() == 2 && b.local.Find(b_work)->name == "Studio" &&
            !b.spaces.table.cursors[work_uuid].pending_apply,
        "the parked arrival lands once the store opens");
}

void TestDefaultSpaceArrivesAfterSettings() {
  FakeServer server;
  const keys::Bytes key = keys::GenerateArk();
  int64_t clock = 1'000'000;
  Device a(server, key, &clock, "a", true);
  a.local.Add("A-WORK", "Work", "Default", 2);
  const std::string uuid = *a.local.EnsureMapped("A-WORK");
  a.prefs["PhiDefaultSpaceUuid"] = uuid;
  a.engine->SetSpaceSyncEnabled(true);
  a.engine->PullOnce();

  Device b(server, key, &clock, "b", true);
  // Settings can arrive while Space sync is gated (or before a later page).
  b.engine->PullOnce();
  Check(b.prefs["PhiDefaultSpaceUuid"] == uuid &&
            b.applied_default_space == psync::kDefaultSpaceUuid,
        "an unresolved default-space register is retained without fallback writes");
  const auto stamp = b.sidecars.Timestamp("PhiDefaultSpaceUuid");
  b.engine->SetSpaceSyncEnabled(true);
  b.engine->PullOnce();
  Check(b.local.LocalSpaceId(uuid) == b.applied_default_space &&
            b.sidecars.Timestamp("PhiDefaultSpaceUuid") == stamp,
        "the role is reconciled after Spaces land without restamping the register");
  // Simulate a missed local application with an unchanged incremental pull.
  b.applied_default_space = psync::kDefaultSpaceUuid;
  b.engine->PullOnce();
  Check(b.local.LocalSpaceId(uuid) == b.applied_default_space,
        "an incremental pull retries the stored register without new settings");
}

void TestCleanupResumesAfterEveryFailure() {
  for (int failure = 0; failure <= 6; ++failure) {
    int fail_at = failure;
    MemoryMarker marker;
    marker.file.marker = "progress";
    marker.file.store_birthday = "birthday";
    marker.fail_save = failure == 0;
    std::vector<int> steps;
    auto step = [&](int number) {
      steps.push_back(number);
      Check(marker.file.requires_reconfiguration && marker.file.removal_pending,
            "cleanup is journaled before any destructive step");
      return fail_at != number;
    };
    psync::CleanupActions actions{
        [&] { return step(1); }, [&] { return step(2); },
        [&] { return step(3); }, [&] { return step(4); },
        [&] { return step(5); }, [&] {
          if (!step(6)) {
            return false;
          }
          marker.file = {};
          return true;
        }};
    Check(!psync::CleanupSyncState(marker, true, actions),
          "a failed cleanup step never reports success");
    Check(static_cast<int>(steps.size()) == failure &&
              marker.file.marker == "progress" &&
              marker.file.store_birthday == "birthday",
          "failure stops subsequent steps and retains the old progress boundary");
    fail_at = -1;
    marker.fail_save = false;
    steps.clear();
    Check(psync::CleanupSyncState(marker, failure == 0, actions) &&
              steps == std::vector<int>({1, 2, 3, 4, 5, 6}) &&
              !marker.file.requires_reconfiguration && !marker.file.removal_pending,
          "retry preserves removal intent and deletes the journal last");
  }
}

}  // namespace

int main() {
  TestCleanupResumesAfterEveryFailure();
  TestDefaultSpaceArrivesAfterSettings();
  TestFirstDevicePublishesSecondAdopts();
  TestStatusPhases();
  TestEditsFlowBothWays();
  TestConflictRetry();
  TestRefusedValueIsNotEchoed();
  TestTombstoneHeal();
  TestUndecryptableIsNeverOverwritten();
  TestPagingAndBirthday();
  TestClockCorrection();
  TestSpacesGateAndFirstPublish();
  TestSpacesReachAFreshDevice();
  TestSpaceEditsFlowBothWays();
  TestSpaceDeletionAndRetention();
  TestSpaceParksUntilItsProfileExists();
  TestUnreadableSpaceIsNeverOverwritten();
  TestBookmarksReachAFreshDevice();
  TestBookmarkAdoptionClaimsATwin();
  TestPinsSyncBothWays();
  TestPinScopeMismatchParks();
  TestANewKindReplaysOnce();
  TestRulesSyncBothWays();
  TestRuleClaimsATwin();
  TestDuplicateRulesCollapse();
  TestSameNamedSpacesRemainSeparate();
  TestUnreachableSpaceListNeverDuplicates();
  if (g_failures) {
    std::printf("sync_engine_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("sync_engine_test: OK\n");
  return 0;
}
