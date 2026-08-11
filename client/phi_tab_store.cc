// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_tab_store.h"

#include <windows.h>

#include <algorithm>
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
    row.type = static_cast<PhiRowType>(dict.FindInt("type").value_or(0));
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
    rows_.push_back(std::move(row));
  }
  LOG(INFO) << "[PhiClient] tab store: " << rows_.size() << " rows, pin scope "
            << PhiPinScopeName(pin_scope_);
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
    rows.Append(std::move(dict));
  }
  base::DictValue root;
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
  int next = 0;
  for (const PhiRow& row : rows_) {
    if (row.type == type && row.profile_id == profile_id &&
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
  // A folder takes its subtree with it.
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
