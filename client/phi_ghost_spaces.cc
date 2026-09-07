// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_ghost_spaces.h"

#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_file_io.h"

namespace phi_client {

PhiGhostSpaces::PhiGhostSpaces() = default;
PhiGhostSpaces::~PhiGhostSpaces() = default;

void PhiGhostSpaces::Load(const std::wstring& path) {
  path_ = path;
  remembered_.clear();
  parked_.clear();
  orphans_.clear();
  base::DictValue root;
  switch (PhiReadJsonFile(path, &root)) {
    case PhiJsonFileStatus::kMissing:
      return;  // No file yet is the ordinary first-run case.
    case PhiJsonFileStatus::kCorrupt:
      // Quarantined by the reader; an empty store means "do not gate the
      // replay" (Empty()), which is the safe answer for a file we could not
      // read.
      LOG(ERROR) << "[PhiGhost] ghost store unreadable; replaying unfiltered";
      return;
    case PhiJsonFileStatus::kOk:
      break;
  }
  if (const base::ListValue* rows = root.FindList("spaces")) {
    for (const base::Value& v : *rows) {
      const base::DictValue* d = v.GetIfDict();
      if (!d) {
        continue;
      }
      const std::string* space = d->FindString("spaceId");
      const std::string* profile = d->FindString("profile");
      const std::optional<int> window_id = d->FindInt("windowId");
      if (!space || !profile || !window_id) {
        continue;
      }
      remembered_[*space] = Remembered{*profile, *window_id};
    }
  }
}

void PhiGhostSpaces::Save() const {
  if (path_.empty()) {
    return;
  }
  base::ListValue rows;
  for (const auto& [space_id, remembered] : remembered_) {
    base::DictValue d;
    d.Set("spaceId", space_id);
    d.Set("profile", remembered.profile);
    d.Set("windowId", remembered.window_id);
    rows.Append(std::move(d));
  }
  base::DictValue root;
  root.Set("spaces", std::move(rows));
  PhiWriteJsonFile(path_, std::move(root), /*schema=*/1);
}

void PhiGhostSpaces::NoteLiveWindow(const std::string& space_id,
                                    const std::string& profile,
                                    int window_id) {
  if (space_id.empty() || window_id == 0) {
    return;
  }
  Remembered& remembered = remembered_[space_id];
  const bool changed =
      remembered.profile != profile || remembered.window_id != window_id;
  remembered.profile = profile;
  remembered.window_id = window_id;
  // A live window is by definition not parked.
  parked_.erase(space_id);
  if (changed) {
    Save();
  }
}

std::vector<int> PhiGhostSpaces::EagerWindowIds(
    const std::string& active_space_id) const {
  std::vector<int> eager;
  auto it = remembered_.find(active_space_id);
  if (it != remembered_.end() && it->second.window_id != 0) {
    eager.push_back(it->second.window_id);
  }
  return eager;
}

void PhiGhostSpaces::ReplaceFromReceipt(const std::string& receipt_json) {
  // REPLACE, never merge: the receipt describes the whole ghost store, so a
  // window missing from it is not parked any more (and one this reopen never
  // touched is still parked and must survive).
  parked_.clear();
  orphans_.clear();
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(receipt_json, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  const base::ListValue* parked = root->FindList("parked");
  if (!parked) {
    return;  // An empty receipt means nothing is parked — a real answer.
  }
  for (const base::Value& v : *parked) {
    const base::DictValue* d = v.GetIfDict();
    const std::string* profile = d ? d->FindString("profile") : nullptr;
    const base::ListValue* ids = d ? d->FindList("windowIds") : nullptr;
    if (!profile || !ids) {
      continue;
    }
    for (const base::Value& id_value : *ids) {
      if (!id_value.is_int()) {
        continue;
      }
      const int window_id = id_value.GetInt();
      // Map back to whichever Space remembers this window.
      bool claimed = false;
      for (const auto& [space_id, remembered] : remembered_) {
        if (remembered.window_id == window_id &&
            remembered.profile == *profile) {
          parked_[space_id] = remembered;
          claimed = true;
          break;
        }
      }
      if (!claimed) {
        orphans_.push_back(Remembered{*profile, window_id});
      }
    }
  }
}

bool PhiGhostSpaces::IsParked(const std::string& space_id) const {
  return parked_.count(space_id) > 0;
}

const PhiGhostSpaces::Remembered* PhiGhostSpaces::Parked(
    const std::string& space_id) const {
  auto it = parked_.find(space_id);
  return it == parked_.end() ? nullptr : &it->second;
}

const std::string* PhiGhostSpaces::SpaceForWindowId(int window_id) const {
  if (window_id == 0) {
    return nullptr;
  }
  for (const auto& [space_id, remembered] : remembered_) {
    if (remembered.window_id == window_id) {
      return &space_id;
    }
  }
  return nullptr;
}

void PhiGhostSpaces::ClearParked(const std::string& space_id) {
  parked_.erase(space_id);
}

PhiGhostSpaces::Remembered PhiGhostSpaces::Forget(const std::string& space_id) {
  Remembered gone;
  auto parked = parked_.find(space_id);
  if (parked != parked_.end()) {
    gone = parked->second;  // Still a ghost: the caller drops the window.
    parked_.erase(parked);
  }
  auto remembered = remembered_.find(space_id);
  if (remembered != remembered_.end()) {
    if (gone.window_id == 0) {
      gone = remembered->second;
    }
    remembered_.erase(remembered);
    Save();
  }
  return gone;
}

std::vector<PhiGhostSpaces::Remembered> PhiGhostSpaces::Orphans() const {
  return orphans_;
}

void PhiGhostSpaces::ClearOrphans() {
  orphans_.clear();
}

}  // namespace phi_client
