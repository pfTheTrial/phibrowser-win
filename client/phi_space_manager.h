// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SPACE_MANAGER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SPACE_MANAGER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_window_model.h"
#include "phibrowser-win/client/phi_window_registry.h"

// The Space list service (docs/architecture-review.md §3.4, the
// PhiSpaceManager row): the Spaces, the default role, the per-Space
// palettes and PhiSpaces.json (the URL rules share the file; PhiUrlRouter
// is their writer, through the same model).
//
// HOST-CLEAN ON PURPOSE: this header is STL plus the model, the ids and the
// registry/window-model headers the host tests already compile — no
// windows.h, and of base/ only the RAW_PTR_EXCLUSION macro (the host build
// shims it, test/model/stubs) — so the pure helpers below are host-tested
// (test/model/space_manager_test.cc). The store, the palettes and the
// framework are reached through forward declarations and the .cc.

namespace phi_framework {
class PhiFramework;
}  // namespace phi_framework

namespace phi_client {

class PhiPreferences;
struct PhiPalette;
struct PhiSpaceView;
template <typename T>
class PhiStore;

// --- Pure helpers (host-tested) ---------------------------------------------

inline const model::Space* FindSpaceIn(const std::vector<model::Space>& spaces,
                                       std::string_view id) {
  for (const model::Space& space : spaces) {
    if (space.id == id) {
      return &space;
    }
  }
  return nullptr;
}

// The Space holding the DEFAULT role at run time: the persisted holder when
// it still exists, "1" (the built-in default) when the list is empty, else
// the first Space. (The decoder's ResolveDefaultSpaceId is the load-time
// rule; this is what DefaultSpaceId() has always answered.)
inline std::string DefaultSpaceIdOf(const std::vector<model::Space>& spaces,
                                    const std::string& persisted) {
  if (FindSpaceIn(spaces, persisted)) {
    return persisted;
  }
  return spaces.empty() ? std::string("1") : spaces.front().id;
}

// Who inherits the default role when `deleted_id` (its holder) goes: the
// first REMAINING Space in strip order; empty when none remains.
inline std::string DefaultRoleSuccessor(const std::vector<model::Space>& spaces,
                                        std::string_view deleted_id) {
  for (const model::Space& space : spaces) {
    if (space.id != deleted_id) {
      return space.id;
    }
  }
  return std::string();
}

// Everything is deletable except the LAST remaining Space: retreat targets
// and new-window fallbacks need one persisted Space to land on.
inline bool CanDeleteSpaceIn(const std::vector<model::Space>& spaces,
                             std::string_view id) {
  return FindSpaceIn(spaces, id) != nullptr && spaces.size() > 1;
}

// Where a Space re-enters a list of `count` (after its own removal) when
// asked for `new_index`: clamped to [0, count].
inline size_t ClampSpaceInsertIndex(size_t count, int new_index) {
  return static_cast<size_t>(
      std::max(0, std::min(new_index, static_cast<int>(count))));
}

// --- The service --------------------------------------------------------------

class PhiSpaceManager {
 public:
  // Which fan-out a list change needs — one value per shape the
  // coordinator used to hand-write after each mutation.
  enum class SpacesChange {
    kList,     // Name, icon, membership: every slot repaints, Settings too.
    kOrder,    // A reorder: every slot repaints (Settings keeps its own).
    kTheme,    // A per-Space theme or saturation: palettes re-applied.
    kProfile,  // A re-binding: repaint, and the wire's Space list moves.
  };
  class Observer {
   public:
    virtual void OnSpacesChanged(SpacesChange what) = 0;

   protected:
    virtual ~Observer() = default;
  };

  // What DeleteSpace and SetSpaceProfile need from the rest of the app
  // (the coordinator implements it): the slot retreat/respawn, and the data
  // a Space owns outside this list — its rows, its rules, its ghost record.
  class Host {
   public:
    // SwitchSlotToSpace: surfaces `space_id` in `slot`, requesting its
    // browser when the slot has none; `reopen_urls` (may be null) open in
    // that browser once it attaches.
    virtual void SurfaceSpaceInSlot(
        SlotId slot,
        const std::string& space_id,
        const std::vector<std::string>* reopen_urls) = 0;
    // A Space is being deleted: its bookmarks and space-scoped pins, its
    // URL rules, and its ghost/parked window go with it.
    virtual void ForgetSpaceData(const std::string& space_id) = 0;
    // A Space was re-bound to `profile_id`: the rows it owns move with it,
    // and any parked window from the OLD profile is dropped.
    virtual void RehomeSpaceData(const std::string& space_id,
                                 const std::string& profile_id) = 0;

   protected:
    virtual ~Host() = default;
  };

  // A mutation writes nothing itself: it calls `dirty`, and the owner folds
  // the write into its coalesced-save timer (then calls Save()).
  using DirtyCallback = std::function<void()>;
  // Every reference outlives the service (the coordinator owns them all).
  PhiSpaceManager(PhiWindowRegistry& registry,
                  const PhiWindowModel& window_model,
                  const PhiPreferences& preferences,
                  phi_framework::PhiFramework& framework,
                  Host& host,
                  DirtyCallback dirty);
  ~PhiSpaceManager();

  PhiSpaceManager(const PhiSpaceManager&) = delete;
  PhiSpaceManager& operator=(const PhiSpaceManager&) = delete;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // --- The store ----------------------------------------------------------
  // Binds the store to `user_data_dir` and loads it. No observer hears a
  // load; the owner rebuilds the palettes once the preferences are in.
  void Load(const std::wstring& user_data_dir);
  // Writes the model (Spaces, default role AND the rules). The owner's
  // flush is the only caller.
  void Save();
  const model::PhiSpacesModel& model() const { return model_; }
  // THE exception to "this service is the only writer of the list": the
  // verb ladder's agentSpace.spaces.create (and the urlRules.* verbs, on
  // the router's half) still write the model directly until the typed verb
  // registry lands. Nothing else may.
  model::PhiSpacesModel& mutable_model() { return model_; }

  // --- Reads --------------------------------------------------------------
  const std::vector<model::Space>& spaces() const { return model_.spaces; }
  const model::Space* Find(const std::string& id) const {
    return FindSpaceIn(model_.spaces, id);
  }
  bool Has(const std::string& id) const { return Find(id) != nullptr; }
  int next_space_id() const { return model_.next_space_id; }
  // Every Space, in display order, as the chips row and menus consume it.
  std::vector<PhiSpaceView> Spaces() const;
  // "name\ticonText\tprofileId\tspaceId\tthemeId\tsaturation" per Space —
  // the Settings host's rows.
  std::vector<std::string> SpaceSummaries() const;
  // The Space holding the DEFAULT role (the Mac's currentDefaultSpaceId):
  // the landing target for retreats and windows opened without a Space, the
  // pre-selected bookmark target, the badge in Settings. Starts on "1" and,
  // when its holder is deleted, is handed to the first remaining Space —
  // persisted with the Space list so the hand-off survives relaunches.
  std::string DefaultSpaceId() const;
  bool CanDeleteSpace(const std::string& space_id) const;
  // The Space a Kiosk offers to hand its page to: the main window's ACTIVE
  // Space, the Space holding the default role when no window is up — the
  // Mac's KioskSpaceMenuTargetResolver.primarySpace.
  std::string KioskTargetSpaceId() const;
  std::string SpaceThemeId(const std::string& space_id) const;
  // -1 follows the app-wide slider.
  int SpaceSaturation(const std::string& space_id) const;

  // --- Writes (each persists and publishes SpacesChanged) -----------------
  bool RenameSpace(const std::string& space_id, const std::string& name);
  // Only the LAST Space is undeletable (the Mac's last-Space rule); the
  // default Space itself may go, handing its role on. Slots whose only
  // browser is the Space's retreat to the default Space first (through the
  // Host), the Space's browsers close, its data is forgotten (Host), then
  // the list entry goes.
  bool DeleteSpace(const std::string& space_id);
  // Moves a Space to `new_index` in the list (chip drag).
  void ReorderSpace(const std::string& space_id, int new_index);
  // Refused for an icon the catalogue does not know.
  void SetSpaceIcon(const std::string& space_id, const std::string& icon);
  // An empty id clears the override, putting the Space back on the app-wide
  // theme — the Mac's "no entry in spaceThemeIds" state.
  void SetSpaceTheme(const std::string& space_id, const std::string& theme_id);
  // -1 follows the app-wide slider; anything else clamps to 0..100.
  void SetSpaceSaturation(const std::string& space_id, int value);
  // The create-Space form's pin: theme and saturation in one write.
  void PinSpaceTheme(const std::string& space_id,
                     const std::string& theme_id,
                     int saturation);
  // Re-binds a Space to another profile (the Mac's changeProfile): its
  // browser is closed and respawned in the new profile with its open tabs,
  // and the rows it owns move with it. Refused for the default Space, whose
  // browser is the one a fresh shell binds to. False when nothing changed.
  bool SetSpaceProfile(const std::string& space_id,
                       const std::string& profile_id);

  // --- Palettes -------------------------------------------------------------
  // Re-resolves the per-Space overrides against the app-wide theme. Run
  // after a load, after any theme or saturation change (app-wide or
  // per-Space) and after a Space list change.
  void RebuildSpacePalettes();
  // The palette `space_id` paints with: its override, else the app-wide one.
  const PhiPalette& PaletteForSpace(const std::string& space_id) const;
  // The palette a SLOT paints with: its active Space's.
  const PhiPalette& PaletteForSlot(int64_t slot) const;

  // --- The framework's copy -------------------------------------------------
  // Publishes the Space list (with each Space's open browser id) into the
  // framework for the page context menu's "Open Link in Space" submenu —
  // the body; the coordinator coalesces the calls to one per turn.
  void PushSpaceMenuNow();

 private:
  void MarkDirty();
  void Notify(SpacesChange what);

  model::PhiSpacesModel model_;  // PhiSpaces.json (Spaces + rules)
  std::unique_ptr<PhiStore<model::PhiSpacesModel>> store_;
  // One resolved palette per Space that overrides the app-wide theme; a
  // Space with no override is not in here and uses the preferences' palette.
  std::map<std::string, std::unique_ptr<PhiPalette>> palette_by_space_;
  // Non-owning; all outlive the service (see the constructor). The
  // framework is a singleton across a module boundary.
  RAW_PTR_EXCLUSION PhiWindowRegistry& registry_;
  RAW_PTR_EXCLUSION const PhiWindowModel& window_model_;
  RAW_PTR_EXCLUSION const PhiPreferences& preferences_;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework& framework_;
  RAW_PTR_EXCLUSION Host& host_;
  DirtyCallback dirty_;
  std::vector<Observer*> observers_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SPACE_MANAGER_H_
