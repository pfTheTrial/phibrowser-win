// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_tab_store.h"

#include <windows.h>

#include <algorithm>
#include <limits>
#include <map>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "base/values.h"

namespace phi_client {

namespace {

std::string NewGuid() {
  return base::Uuid::GenerateRandomV4().AsLowercaseString();
}

// What makes two pinned rows "the same pin" after a scope change — the Mac's
// PinnedTabContentSignature: title, url and favicon.
std::string ContentSignature(const PhiRow& row) {
  return row.title + "\n" + row.url + "\n" + row.favicon_base64;
}

}  // namespace

const char* PhiPinScopeName(PhiPinScope scope) {
  switch (scope) {
    case PhiPinScope::kSpace:
      return "space";
    case PhiPinScope::kApp:
      return "app";
    case PhiPinScope::kProfile:
      break;
  }
  return "profile";
}

bool PhiPinScopeFromName(const std::string& name, PhiPinScope* out) {
  if (name == "space") {
    *out = PhiPinScope::kSpace;
  } else if (name == "profile") {
    *out = PhiPinScope::kProfile;
  } else if (name == "app") {
    *out = PhiPinScope::kApp;
  } else {
    return false;
  }
  return true;
}

PhiTabStore::PhiTabStore() = default;
PhiTabStore::~PhiTabStore() = default;

void PhiTabStore::Load(const std::wstring& path) {
  path_ = path;
  HANDLE file = ::CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;  // First run.
  }
  std::string json;
  char buffer[8192];
  DWORD read = 0;
  while (::ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    json.append(buffer, read);
  }
  ::CloseHandle(file);

  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  if (const std::string* scope = root->FindString("pinScope")) {
    PhiPinScope parsed = PhiPinScope::kProfile;
    if (PhiPinScopeFromName(*scope, &parsed)) {
      pin_scope_ = parsed;
    }
  }
  // Absent = schema 1, the pre-2026-09-07 file.
  const int schema = root->FindInt("schema").value_or(1);
  std::map<std::string, std::string> legacy_partner;  // guid -> partner guid
  const base::ListValue* rows = root->FindList("rows");
  if (!rows) {
    return;
  }
  for (const base::Value& value : *rows) {
    if (!value.is_dict()) {
      continue;
    }
    const base::DictValue& dict = value.GetDict();
    const std::string* guid = dict.FindString("guid");
    if (!guid || guid->empty()) {
      continue;
    }
    PhiRow row;
    row.guid = *guid;
    const int type = dict.FindInt("type").value_or(0);
    if (type < static_cast<int>(PhiRowType::kPinnedTab) ||
        type > static_cast<int>(PhiRowType::kBookmarkFolder)) {
      LOG(WARNING) << "[PhiClient] tab store: dropping row of unknown type "
                   << type;
      continue;
    }
    row.type = static_cast<PhiRowType>(type);
    if (const std::string* title = dict.FindString("title")) {
      row.title = *title;
    }
    if (const std::string* url = dict.FindString("url")) {
      row.url = *url;
    }
    row.index = dict.FindInt("index").value_or(0);
    if (const std::string* profile = dict.FindString("profileId")) {
      row.profile_id = *profile;
    }
    if (const std::string* space = dict.FindString("spaceId")) {
      row.space_id = *space;
    }
    if (const std::string* parent = dict.FindString("parentGuid")) {
      row.parent_guid = *parent;
    }
    if (const std::string* lineage = dict.FindString("lineageId")) {
      row.lineage_id = *lineage;
    }
    if (const std::string* favicon = dict.FindString("favicon")) {
      row.favicon_base64 = *favicon;
    }
    row.favicon_width = dict.FindInt("faviconWidth").value_or(0);
    row.favicon_height = dict.FindInt("faviconHeight").value_or(0);
    if (const std::string* url_b = dict.FindString("secondaryUrl")) {
      row.url_b = *url_b;
    }
    if (const std::string* title_b = dict.FindString("secondaryTitle")) {
      row.title_b = *title_b;
    }
    if (const std::string* favicon_b = dict.FindString("secondaryFavicon")) {
      row.favicon_b_base64 = *favicon_b;
    }
    row.favicon_b_width = dict.FindInt("secondaryFaviconWidth").value_or(0);
    row.favicon_b_height = dict.FindInt("secondaryFaviconHeight").value_or(0);
    if (const std::string* icon = dict.FindString("icon")) {
      row.icon = *icon;
    }
    if (const std::string* layout = dict.FindString("layout")) {
      row.layout = *layout;
    }
    if (schema < 2) {
      if (const std::string* partner = dict.FindString("splitPartnerGuid")) {
        legacy_partner[row.guid] = *partner;
      }
    }
    rows_.push_back(std::move(row));
  }
  if (const base::DictValue* aliases = root->FindDict("legacyTagAliases")) {
    for (const auto [old_guid, tag] : *aliases) {
      if (tag.is_string() && !tag.GetString().empty()) {
        legacy_tag_aliases_[old_guid] = tag.GetString();
      }
    }
  }
  if (schema < 2) {
    FoldLegacySplitPairs(legacy_partner);
    Save();  // Rewrite at the current schema at once.
  }
  LOG(INFO) << "[PhiClient] tab store: " << rows_.size() << " rows, pin scope "
            << PhiPinScopeName(pin_scope_);
}

// Schema 1 -> 2: a pinned split was two pinned rows cross-linked by
// splitPartnerGuid (either direction may be missing — the Mac's two writes
// were async and one could be lost on quit). Fold each pair into ONE cell:
// the lower-index row keeps its place and gains pane B from the partner,
// which is deleted; the survivors are renumbered densely. A link whose
// target is gone is simply dropped.
void PhiTabStore::FoldLegacySplitPairs(
    const std::map<std::string, std::string>& partner_of) {
  // Mirror every link so a half-persisted pair still folds.
  std::map<std::string, std::string> links = partner_of;
  for (const auto& [guid, partner] : partner_of) {
    if (!partner.empty() && !links.count(partner)) {
      links[partner] = guid;
    }
  }
  std::vector<std::string> doomed;
  int folded = 0;
  for (const auto& [guid, partner] : links) {
    if (partner.empty() || guid == partner ||
        std::find(doomed.begin(), doomed.end(), guid) != doomed.end() ||
        std::find(doomed.begin(), doomed.end(), partner) != doomed.end()) {
      continue;
    }
    PhiRow* a = nullptr;
    PhiRow* b = nullptr;
    for (PhiRow& row : rows_) {
      if (row.guid == guid) {
        a = &row;
      } else if (row.guid == partner) {
        b = &row;
      }
    }
    if (!a || !b || a->type != PhiRowType::kPinnedTab ||
        b->type != PhiRowType::kPinnedTab || !a->url_b.empty() ||
        !b->url_b.empty()) {
      continue;  // Dangling or already folded from the other side.
    }
    if (b->index < a->index) {
      std::swap(a, b);
    }
    a->url_b = b->url;
    a->title_b = b->title;
    a->favicon_b_base64 = b->favicon_base64;
    a->favicon_b_width = b->favicon_width;
    a->favicon_b_height = b->favicon_height;
    if (a->layout.empty()) {
      a->layout = b->layout;
    }
    legacy_tag_aliases_[b->guid] = a->guid + "#b";  // TagFor(a, true)
    doomed.push_back(b->guid);
    ++folded;
  }
  if (doomed.empty()) {
    return;
  }
  std::erase_if(rows_, [&](const PhiRow& row) {
    return std::find(doomed.begin(), doomed.end(), row.guid) != doomed.end();
  });
  // Renumber every pinned sibling set densely.
  std::map<std::pair<std::string, std::string>, std::vector<PhiRow*>> groups;
  for (PhiRow& row : rows_) {
    if (row.type == PhiRowType::kPinnedTab) {
      groups[{row.profile_id, row.space_id}].push_back(&row);
    }
  }
  for (auto& [owner, rows] : groups) {
    std::sort(rows.begin(), rows.end(), [](const PhiRow* x, const PhiRow* y) {
      return x->index != y->index ? x->index < y->index : x->guid < y->guid;
    });
    for (size_t i = 0; i < rows.size(); ++i) {
      rows[i]->index = static_cast<int>(i);
    }
  }
  LOG(INFO) << "[PhiClient] tab store: folded " << folded
            << " legacy pinned split pair(s) into single cells";
}

void PhiTabStore::DropLegacyTagAlias(const std::string& old_guid) {
  if (legacy_tag_aliases_.erase(old_guid)) {
    Save();
  }
}

void PhiTabStore::Save() const {
  if (path_.empty()) {
    return;
  }
  base::ListValue rows;
  for (const PhiRow& row : rows_) {
    base::DictValue dict;
    dict.Set("guid", row.guid);
    dict.Set("type", static_cast<int>(row.type));
    dict.Set("title", row.title);
    dict.Set("url", row.url);
    dict.Set("index", row.index);
    dict.Set("profileId", row.profile_id);
    dict.Set("spaceId", row.space_id);
    dict.Set("parentGuid", row.parent_guid);
    dict.Set("lineageId", row.lineage_id);
    dict.Set("favicon", row.favicon_base64);
    dict.Set("faviconWidth", row.favicon_width);
    dict.Set("faviconHeight", row.favicon_height);
    dict.Set("secondaryUrl", row.url_b);
    dict.Set("secondaryTitle", row.title_b);
    dict.Set("secondaryFavicon", row.favicon_b_base64);
    dict.Set("secondaryFaviconWidth", row.favicon_b_width);
    dict.Set("secondaryFaviconHeight", row.favicon_b_height);
    dict.Set("icon", row.icon);
    dict.Set("layout", row.layout);
    rows.Append(std::move(dict));
  }
  base::DictValue root;
  root.Set("schema", kPhiTabStoreSchema);
  if (!legacy_tag_aliases_.empty()) {
    base::DictValue aliases;
    for (const auto& [old_guid, tag] : legacy_tag_aliases_) {
      aliases.Set(old_guid, tag);
    }
    root.Set("legacyTagAliases", std::move(aliases));
  }
  root.Set("pinScope", PhiPinScopeName(pin_scope_));
  root.Set("rows", std::move(rows));
  std::string json;
  base::JSONWriter::Write(root, &json);

  // Write-temp-then-replace: overwriting the live file in place would
  // truncate EVERY pin and bookmark on a crash or power cut mid-write.
  const std::wstring temp_path = path_ + L".tmp";
  HANDLE file = ::CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD wrote = 0;
  const BOOL written = ::WriteFile(
      file, json.data(), static_cast<DWORD>(json.size()), &wrote, nullptr);
  ::FlushFileBuffers(file);
  ::CloseHandle(file);
  if (!written || wrote != json.size()) {
    ::DeleteFileW(temp_path.c_str());
    return;
  }
  if (!::ReplaceFileW(path_.c_str(), temp_path.c_str(), nullptr, 0, nullptr,
                      nullptr)) {
    // First save: there is nothing to replace yet.
    ::MoveFileExW(temp_path.c_str(), path_.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  }
}

void PhiTabStore::OwnerForScope(const std::string& profile_id,
                                const std::string& space_id,
                                std::string* out_profile,
                                std::string* out_space) const {
  switch (pin_scope_) {
    case PhiPinScope::kSpace:
      *out_profile = profile_id;
      *out_space = space_id;
      return;
    case PhiPinScope::kProfile:
      *out_profile = profile_id;
      out_space->clear();
      return;
    case PhiPinScope::kApp:
      out_profile->clear();
      out_space->clear();
      return;
  }
}

int PhiTabStore::NextIndexFor(PhiRowType type,
                              const std::string& profile_id,
                              const std::string& space_id,
                              const std::string& parent_guid) const {
  // Folders and bookmarks share ONE sibling sequence (as Reorder treats
  // them) — separate sequences would hand a folder's mixed children
  // duplicate indices and an arbitrary guid-tiebreak order.
  const bool bookmark_kind = type == PhiRowType::kBookmark ||
                             type == PhiRowType::kBookmarkFolder;
  int next = 0;
  for (const PhiRow& row : rows_) {
    const bool same_kind =
        bookmark_kind ? (row.type == PhiRowType::kBookmark ||
                         row.type == PhiRowType::kBookmarkFolder)
                      : row.type == type;
    if (same_kind && row.profile_id == profile_id &&
        row.space_id == space_id && row.parent_guid == parent_guid) {
      next = std::max(next, row.index + 1);
    }
  }
  return next;
}

std::vector<PhiRow> PhiTabStore::PinnedFor(const std::string& profile_id,
                                           const std::string& space_id) const {
  std::string owner_profile, owner_space;
  OwnerForScope(profile_id, space_id, &owner_profile, &owner_space);
  std::vector<PhiRow> out;
  for (const PhiRow& row : rows_) {
    if (row.type == PhiRowType::kPinnedTab &&
        row.profile_id == owner_profile && row.space_id == owner_space) {
      out.push_back(row);
    }
  }
  std::sort(out.begin(), out.end(), [](const PhiRow& a, const PhiRow& b) {
    return a.index != b.index ? a.index < b.index : a.guid < b.guid;
  });
  return out;
}

std::string PhiTabStore::AddPinned(const std::string& profile_id,
                                   const std::string& space_id,
                                   const std::string& title,
                                   const std::string& url,
                                   const std::string& favicon_base64,
                                   int favicon_width,
                                   int favicon_height) {
  if (url.empty()) {
    return std::string();
  }
  PhiRow row;
  row.guid = NewGuid();
  row.lineage_id = row.guid;  // Its own lineage until a scope change merges it.
  row.type = PhiRowType::kPinnedTab;
  row.title = title;
  row.url = url;
  row.favicon_base64 = favicon_base64;
  row.favicon_width = favicon_width;
  row.favicon_height = favicon_height;
  OwnerForScope(profile_id, space_id, &row.profile_id, &row.space_id);
  row.index = NextIndexFor(row.type, row.profile_id, row.space_id, "");
  const std::string guid = row.guid;
  rows_.push_back(std::move(row));
  Save();
  return guid;
}

std::vector<PhiRow> PhiTabStore::BookmarksFor(
    const std::string& profile_id,
    const std::string& space_id,
    const std::string& parent_guid) const {
  std::vector<PhiRow> out;
  for (const PhiRow& row : rows_) {
    const bool is_bookmark = row.type == PhiRowType::kBookmark ||
                             row.type == PhiRowType::kBookmarkFolder;
    if (is_bookmark && row.profile_id == profile_id &&
        row.space_id == space_id && row.parent_guid == parent_guid) {
      out.push_back(row);
    }
  }
  std::sort(out.begin(), out.end(), [](const PhiRow& a, const PhiRow& b) {
    return a.index != b.index ? a.index < b.index : a.guid < b.guid;
  });
  return out;
}

std::string PhiTabStore::AddBookmark(const std::string& profile_id,
                                     const std::string& space_id,
                                     const std::string& parent_guid,
                                     const std::string& title,
                                     const std::string& url,
                                     bool folder,
                                     const std::string& favicon_base64,
                                     int favicon_width,
                                     int favicon_height) {
  if (!folder && url.empty()) {
    return std::string();
  }
  PhiRow row;
  row.guid = NewGuid();
  row.type = folder ? PhiRowType::kBookmarkFolder : PhiRowType::kBookmark;
  row.title = title;
  row.url = folder ? std::string() : url;
  row.favicon_base64 = favicon_base64;
  row.favicon_width = favicon_width;
  row.favicon_height = favicon_height;
  // Bookmarks are rooted per profile AND per Space, as on Mac.
  row.profile_id = profile_id;
  row.space_id = space_id;
  row.parent_guid = parent_guid;
  row.index = NextIndexFor(row.type, profile_id, space_id, parent_guid);
  const std::string guid = row.guid;
  rows_.push_back(std::move(row));
  Save();
  return guid;
}

bool PhiTabStore::Contains(const std::string& guid) const {
  for (const PhiRow& row : rows_) {
    if (row.guid == guid) {
      return true;
    }
  }
  return false;
}

bool PhiTabStore::Remove(const std::string& guid) {
  const size_t before = rows_.size();
  // A folder takes its subtree.
  std::vector<std::string> doomed{guid};
  for (size_t i = 0; i < doomed.size(); ++i) {
    for (const PhiRow& row : rows_) {
      if (row.parent_guid == doomed[i]) {
        doomed.push_back(row.guid);
      }
    }
  }
  std::erase_if(rows_, [&](const PhiRow& row) {
    return std::find(doomed.begin(), doomed.end(), row.guid) != doomed.end();
  });
  if (rows_.size() == before) {
    return false;
  }
  Save();
  return true;
}

bool PhiTabStore::SetRowSecondary(const std::string& guid,
                                  const std::string& url_b,
                                  const std::string& title_b,
                                  const std::string& favicon_b_base64,
                                  int favicon_b_width,
                                  int favicon_b_height) {
  for (PhiRow& row : rows_) {
    if (row.guid == guid) {
      row.url_b = url_b;
      // Clearing the second pane drops everything that described it.
      const bool split = !url_b.empty();
      row.title_b = split ? title_b : std::string();
      row.favicon_b_base64 = split ? favicon_b_base64 : std::string();
      row.favicon_b_width = split ? favicon_b_width : 0;
      row.favicon_b_height = split ? favicon_b_height : 0;
      if (!split) {
        row.layout.clear();
      }
      Save();
      return true;
    }
  }
  return false;
}

bool PhiTabStore::RenameRow(const std::string& guid,
                            const std::string& title) {
  for (PhiRow& row : rows_) {
    if (row.guid == guid) {
      row.title = title;
      Save();
      return true;
    }
  }
  return false;
}

bool PhiTabStore::SetRowIcon(const std::string& guid,
                             const std::string& icon) {
  for (PhiRow& row : rows_) {
    if (row.guid == guid) {
      if (row.icon == icon) {
        return false;
      }
      row.icon = icon;
      Save();
      return true;
    }
  }
  return false;
}

bool PhiTabStore::SetRowLayout(const std::string& guid,
                               const std::string& layout) {
  for (PhiRow& row : rows_) {
    if (row.guid == guid) {
      if (row.layout == layout) {
        return false;
      }
      row.layout = layout;
      Save();
      return true;
    }
  }
  return false;
}

bool PhiTabStore::SwapPanes(const std::string& guid) {
  for (PhiRow& row : rows_) {
    if (row.guid != guid || row.url_b.empty()) {
      continue;
    }
    std::swap(row.title, row.title_b);
    std::swap(row.url, row.url_b);
    std::swap(row.favicon_base64, row.favicon_b_base64);
    std::swap(row.favicon_width, row.favicon_b_width);
    std::swap(row.favicon_height, row.favicon_b_height);
    Save();
    return true;
  }
  return false;
}

int PhiTabStore::RemoveForSpace(const std::string& space_id) {
  if (space_id.empty()) {
    return 0;
  }
  const size_t before = rows_.size();
  std::erase_if(rows_,
                [&](const PhiRow& row) { return row.space_id == space_id; });
  const int removed = static_cast<int>(before - rows_.size());
  if (removed > 0) {
    Save();
  }
  return removed;
}

int PhiTabStore::RehomeSpace(const std::string& space_id,
                             const std::string& profile_id) {
  if (space_id.empty() || profile_id.empty()) {
    return 0;
  }
  int moved = 0;
  for (PhiRow& row : rows_) {
    if (row.space_id == space_id && row.profile_id != profile_id) {
      row.profile_id = profile_id;
      ++moved;
    }
  }
  if (moved > 0) {
    Save();
  }
  return moved;
}

bool PhiTabStore::Reorder(const std::string& guid, int new_index) {
  // A real sibling move: pull the row out of its ordered sibling list,
  // reinsert at `new_index`, and renumber densely — writing a bare index
  // would leave duplicates and an undefined sort.
  PhiRow* target = nullptr;
  for (PhiRow& row : rows_) {
    if (row.guid == guid) {
      target = &row;
      break;
    }
  }
  if (!target) {
    return false;
  }
  const bool pinned_kind = target->type == PhiRowType::kPinnedTab;
  std::vector<PhiRow*> siblings;
  for (PhiRow& row : rows_) {
    const bool same_kind = pinned_kind
                               ? row.type == PhiRowType::kPinnedTab
                               : (row.type == PhiRowType::kBookmark ||
                                  row.type == PhiRowType::kBookmarkFolder);
    if (same_kind && row.profile_id == target->profile_id &&
        row.space_id == target->space_id &&
        row.parent_guid == target->parent_guid) {
      siblings.push_back(&row);
    }
  }
  std::sort(siblings.begin(), siblings.end(),
            [](const PhiRow* a, const PhiRow* b) { return a->index < b->index; });
  siblings.erase(std::find(siblings.begin(), siblings.end(), target));
  const int clamped = std::max(
      0, std::min(new_index, static_cast<int>(siblings.size())));
  siblings.insert(siblings.begin() + clamped, target);
  for (size_t i = 0; i < siblings.size(); ++i) {
    siblings[i]->index = static_cast<int>(i);
  }
  Save();
  return true;
}

const PhiRow* PhiTabStore::Find(const std::string& guid) const {
  for (const PhiRow& row : rows_) {
    if (row.guid == guid) {
      return &row;
    }
  }
  return nullptr;
}

bool PhiTabStore::SetRowUrl(const std::string& guid, const std::string& url) {
  for (PhiRow& row : rows_) {
    if (row.guid != guid) {
      continue;
    }
    if (row.type == PhiRowType::kBookmarkFolder || url.empty()) {
      return false;
    }
    row.url = url;
    // The stored favicon belonged to the old site; drop it rather than show
    // the wrong icon next to the new url.
    row.favicon_base64.clear();
    row.favicon_width = 0;
    row.favicon_height = 0;
    Save();
    return true;
  }
  return false;
}

bool PhiTabStore::Move(const std::string& guid,
                       const std::string& new_parent,
                       int new_index) {
  PhiRow* target = nullptr;
  for (PhiRow& row : rows_) {
    if (row.guid == guid) {
      target = &row;
      break;
    }
  }
  if (!target || (target->type != PhiRowType::kBookmark &&
                  target->type != PhiRowType::kBookmarkFolder)) {
    return false;
  }
  if (!new_parent.empty()) {
    const PhiRow* parent = Find(new_parent);
    if (!parent || parent->type != PhiRowType::kBookmarkFolder ||
        parent->profile_id != target->profile_id ||
        parent->space_id != target->space_id) {
      return false;
    }
    // A folder must not become its own ancestor — walk up from the new
    // parent and refuse when the chain passes through the moving row.
    for (const PhiRow* p = parent; p;
         p = p->parent_guid.empty() ? nullptr : Find(p->parent_guid)) {
      if (p->guid == guid) {
        return false;
      }
    }
  }
  target->parent_guid = new_parent;
  // Reorder does the real placement: it clamps, renumbers the new sibling
  // list densely, and saves. INT_MAX (or a negative index) means append.
  target->index = std::numeric_limits<int>::max();
  return Reorder(guid,
                 new_index >= 0 ? new_index : std::numeric_limits<int>::max());
}

bool PhiTabStore::SetPinScope(PhiPinScope scope) {
  if (scope == pin_scope_) {
    return false;
  }
  pin_scope_ = scope;

  // Re-own every pinned row for the new scope, then merge rows that have become
  // indistinguishable — the Mac's scope migration. Widening (space -> profile
  // -> app) collapses per-Space copies of the same pin into one; the survivor
  // keeps the lowest index so order stays stable.
  std::map<std::string, size_t> keep_by_signature;
  std::vector<PhiRow> merged;
  for (PhiRow& row : rows_) {
    if (row.type != PhiRowType::kPinnedTab) {
      merged.push_back(row);
      continue;
    }
    std::string owner_profile, owner_space;
    OwnerForScope(row.profile_id, row.space_id, &owner_profile, &owner_space);
    row.profile_id = owner_profile;
    row.space_id = owner_space;
    const std::string key =
        owner_profile + "\n" + owner_space + "\n" + ContentSignature(row);
    auto it = keep_by_signature.find(key);
    if (it == keep_by_signature.end()) {
      keep_by_signature[key] = merged.size();
      merged.push_back(row);
    } else {
      // Same pin, arrived twice: keep the earlier position.
      PhiRow& survivor = merged[it->second];
      survivor.index = std::min(survivor.index, row.index);
    }
  }
  rows_ = std::move(merged);
  Save();
  return true;
}

}  // namespace phi_client
