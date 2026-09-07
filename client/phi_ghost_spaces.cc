// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_ghost_spaces.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/values.h"

namespace phi_client {

PhiGhostSpaces::PhiGhostSpaces(model::PhiRestoreModel* model)
    : model_(model) {}

PhiGhostSpaces::~PhiGhostSpaces() = default;

model::RememberedWindow* PhiGhostSpaces::FindRemembered(
    const std::string& space_id) {
  for (model::RememberedWindow& row : model_->remembered) {
    if (row.space_id == space_id) {
      return &row;
    }
  }
  return nullptr;
}

const model::RememberedWindow* PhiGhostSpaces::FindRemembered(
    const std::string& space_id) const {
  for (const model::RememberedWindow& row : model_->remembered) {
    if (row.space_id == space_id) {
      return &row;
    }
  }
  return nullptr;
}

void PhiGhostSpaces::NoteLiveWindow(const std::string& space_id,
                                    const std::string& profile,
                                    BrowserId window_id) {
  if (space_id.empty() || !window_id.valid()) {
    return;
  }
  bool changed = false;
  if (model::RememberedWindow* row = FindRemembered(space_id)) {
    changed = row->profile != profile || row->window_id != window_id.value;
    row->profile = profile;
    row->window_id = window_id.value;
  } else {
    model_->remembered.push_back({space_id, profile, window_id.value});
    changed = true;
  }
  // A live window is by definition not parked.
  parked_.erase(space_id);
  if (changed) {
    MarkDirty();
  }
}

void PhiGhostSpaces::MarkDirty() const {
  if (dirty_callback_) {
    dirty_callback_();
  }
}

std::vector<int> PhiGhostSpaces::EagerWindowIds(
    const std::string& active_space_id) const {
  std::vector<int> eager;
  const model::RememberedWindow* row = FindRemembered(active_space_id);
  if (row && BrowserId{row->window_id}.valid()) {
    eager.push_back(static_cast<int>(row->window_id));
  }
  return eager;
}

void PhiGhostSpaces::ReplaceFromReceipt(const std::string& receipt_json) {
  // REPLACE, never merge: the receipt describes the whole ghost store, so a
  // window missing from it is not parked any more (and one this reopen never
  // touched is still parked and must survive). The model's advisory copy is
  // replaced the same way — it records what the LAST receipt said.
  parked_.clear();
  orphans_.clear();
  model_->parked.clear();
  MarkDirty();
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
      const BrowserId window_id{id_value.GetInt()};
      model_->parked[*profile].push_back(window_id.value);
      // Map back to whichever Space remembers this window.
      bool claimed = false;
      for (const model::RememberedWindow& row : model_->remembered) {
        if (row.window_id == window_id.value && row.profile == *profile) {
          parked_[row.space_id] = Remembered{row.profile, window_id};
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

const std::string* PhiGhostSpaces::SpaceForWindowId(BrowserId window_id) const {
  if (!window_id.valid()) {
    return nullptr;
  }
  for (const model::RememberedWindow& row : model_->remembered) {
    if (row.window_id == window_id.value) {
      return &row.space_id;
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
  auto row = std::find_if(
      model_->remembered.begin(), model_->remembered.end(),
      [&](const model::RememberedWindow& r) { return r.space_id == space_id; });
  if (row != model_->remembered.end()) {
    if (!gone.window_id.valid()) {
      gone = Remembered{row->profile, BrowserId{row->window_id}};
    }
    model_->remembered.erase(row);
    MarkDirty();
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
