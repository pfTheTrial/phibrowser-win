// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_session_restore.h"

#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_store.h"

namespace phi_client {

PhiSessionRestore::PhiSessionRestore(phi_framework::PhiFramework& framework,
                                     Host& host,
                                     DirtyCallback dirty)
    : framework_(framework),
      host_(host),
      dirty_(std::move(dirty)),
      store_(std::make_unique<PhiStore<model::PhiRestoreModel>>()) {
  // The ghost view's mutations ride the coalesced-save timer like every
  // other client store.
  ghosts_.set_dirty_callback([this] { dirty_(); });
}

PhiSessionRestore::~PhiSessionRestore() = default;

void PhiSessionRestore::Load(const std::wstring& user_data_dir) {
  store_->Init(user_data_dir);
  store_->Load(&model_);
}

void PhiSessionRestore::Save() {
  store_->Save(model_);
}

void PhiSessionRestore::PrepareForShutdown() {
  model_.last_active_space_id = LastActiveSpaceId();  // The live window's.
}

std::string PhiSessionRestore::LastActiveSpaceId() const {
  // A live slot's current Space wins when there is one (a reopen mid-run);
  // at cold start there is no window yet, so answer what PhiRestore.json
  // remembers from the last run — falling back to the default Space when
  // that id was deleted since.
  const std::string live = host_.ActiveSpaceOfLiveWindow();
  return ResolveLastActiveSpace(live, model_.last_active_space_id,
                                !model_.last_active_space_id.empty() &&
                                    host_.HasSpace(model_.last_active_space_id),
                                host_.DefaultSpaceId());
}

void PhiSessionRestore::NoteActiveSpace(const std::string& space_id) {
  // Coalesced: a switch is one keypress, and the store rewrites the whole
  // file. The save timer folds a burst into one write; a shutdown flushes
  // it early (PrepareForShutdown).
  if (space_id.empty() || space_id == model_.last_active_space_id) {
    return;
  }
  model_.last_active_space_id = space_id;
  dirty_();
}

void PhiSessionRestore::OnColdStartRestorePlan(
    std::vector<int>* eager_window_ids,
    bool* armed) {
  // Refuse to gate until the Space records are actually loaded. Arming with
  // an empty set here would park every window the user expected to see; an
  // unfiltered replay is the correct fallback, and `*armed` arrives false.
  if (!eager_window_ids || !armed || ghosts_.Empty()) {
    LOG(INFO) << "[PhiClient] ColdStartRestorePlan: NOT armed (records "
              << (ghosts_.Empty() ? "not loaded" : "loaded") << ")";
    host_.NoteRestorePlanned("unfiltered");
    return;
  }
  host_.NoteRestorePlanned("armed");
  // The active Space's window is rebuilt now; every other saved window parks
  // and materializes when the user switches to it.
  *eager_window_ids = ghosts_.EagerWindowIds(LastActiveSpaceId());
  *armed = true;
  LOG(INFO) << "[PhiClient] ColdStartRestorePlan: armed space="
            << LastActiveSpaceId() << " eager="
            << (eager_window_ids->empty() ? 0 : eager_window_ids->front());
  // A cold start has no settlement callback, so the "eager set matched
  // nothing" case (the ids named windows the session file no longer holds:
  // every window parks, none appears) is caught by a timer: once the
  // replay has had its say and no user window exists, open a default
  // shell. A user window arriving disarms it (NoteUserWindowUp).
  cold_start_armed_ = true;
  host_.ArmColdStartFallback(kColdStartFallbackMs);
}

void PhiSessionRestore::OnColdStartParkedWindows(
    const char* parked_json,
    const char* reporting_profile) {
  // Same shape as the reopen receipt's `parked`, so it feeds the one path.
  // Wrapped to match, since the cold start reports the list bare.
  base::DictValue receipt;
  std::optional<base::Value> parked = base::JSONReader::Read(
      parked_json ? parked_json : "[]", base::JSON_PARSE_RFC);
  if (parked && parked->is_list()) {
    receipt.Set("parked", std::move(*parked));
  } else {
    receipt.Set("parked", base::ListValue());
  }
  std::string json;
  base::JSONWriter::Write(receipt, &json);
  LOG(INFO) << "[PhiClient] ColdStartParked profile=" << reporting_profile
            << " parked=" << (parked_json ? parked_json : "(null)");
  ghosts_.ReplaceFromReceipt(json);
  host_.PushSpaceMenu();
  host_.NoteRestoring();
  if (host_.RestoreSettled()) {
    // The diversion landed AFTER the restore settled (a user shell's first
    // snapshot beat it): the settle-time orphan drop found nothing, so run
    // it for this receipt — the parked windows no Space claims must not be
    // replayed as loose windows by the next unfiltered restore.
    DropOrphanGhosts();
  }
  if (cold_start_armed_) {
    // The diversion landed; give the eager window (if any) a moment to be
    // built before deciding nothing came.
    host_.ArmColdStartFallback(kColdStartSettleMs);
  }
}

void PhiSessionRestore::OnSessionRestoreSettled(const char* receipt_json) {
  last_receipt_ = receipt_json ? receipt_json : "";
  // REPLACE, never merge — the receipt describes the whole ghost store.
  ghosts_.ReplaceFromReceipt(last_receipt_);
  host_.PushSpaceMenu();
  // The receipt's verdict names the settle reason: an armed eager set that
  // matched nothing parks the whole session with no window up, and a replay
  // that never started restores nothing either. Settle rescues both (and is
  // a no-op rescue when a window is up).
  std::optional<base::DictValue> receipt =
      base::JSONReader::ReadDict(last_receipt_, base::JSON_PARSE_RFC);
  const bool matched_nothing =
      receipt && receipt->FindBool("eagerFilterMatchedNothing").value_or(false);
  const bool restored_any =
      receipt && receipt->FindBool("restoredAny").value_or(true);
  Settle(SettleReasonFor(matched_nothing, restored_any));
}

void PhiSessionRestore::Settle(const char* reason) {
  // Parked windows no Space claims (the Space was deleted while its window
  // sat parked) would be replayed as loose windows by the next unfiltered
  // restore; drop them once the receipt is in, whichever path brought it.
  DropOrphanGhosts();
  // The rescue and the phase move are the coordinator's.
  host_.OnRestoreSettled(reason);
}

void PhiSessionRestore::OnColdStartFallbackTimer() {
  // The replay had its say and no user window disarmed this: the cold
  // start is settled either way (the rescue opens a shell when the eager
  // set matched nothing).
  host_.DisarmColdStartFallback();
  cold_start_armed_ = false;
  Settle("fallback");
}

void PhiSessionRestore::NoteUserWindowUp() {
  if (!cold_start_armed_) {
    return;
  }
  cold_start_armed_ = false;
  host_.DisarmColdStartFallback();
}

const std::string* PhiSessionRestore::SpaceForWindowId(
    BrowserId window_id) const {
  return ghosts_.SpaceForWindowId(window_id);
}

const PhiGhostSpaces::Remembered* PhiSessionRestore::Parked(
    const std::string& space_id) const {
  return ghosts_.Parked(space_id);
}

void PhiSessionRestore::NoteLiveWindow(const std::string& space_id,
                                       const std::string& profile,
                                       BrowserId window_id) {
  ghosts_.NoteLiveWindow(space_id, profile, window_id);
}

void PhiSessionRestore::ClearParked(const std::string& space_id) {
  ghosts_.ClearParked(space_id);
}

void PhiSessionRestore::ForgetSpace(const std::string& space_id) {
  const bool was_parked = ghosts_.Parked(space_id) != nullptr;
  const PhiGhostSpaces::Remembered ghost = ghosts_.Forget(space_id);
  if (was_parked && ghost.window_id.valid()) {
    framework_.DropGhostWindow(ghost.profile.c_str(),
                               static_cast<int>(ghost.window_id.value));
  }
}

void PhiSessionRestore::DropOrphanGhosts() {
  for (const PhiGhostSpaces::Remembered& orphan : ghosts_.Orphans()) {
    framework_.DropGhostWindow(orphan.profile.c_str(),
                               static_cast<int>(orphan.window_id.value));
  }
  ghosts_.ClearOrphans();
}

}  // namespace phi_client
