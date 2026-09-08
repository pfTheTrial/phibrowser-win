// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_space_manager.h"

#include <utility>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_preferences.h"
#include "phibrowser-win/client/phi_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

PhiSpaceManager::PhiSpaceManager(PhiWindowRegistry& registry,
                                 const PhiWindowModel& window_model,
                                 const PhiPreferences& preferences,
                                 phi_framework::PhiFramework& framework,
                                 Host& host,
                                 DirtyCallback dirty)
    : store_(std::make_unique<PhiStore<model::PhiSpacesModel>>()),
      registry_(registry),
      window_model_(window_model),
      preferences_(preferences),
      framework_(framework),
      host_(host),
      dirty_(std::move(dirty)) {}

PhiSpaceManager::~PhiSpaceManager() = default;

void PhiSpaceManager::AddObserver(Observer* observer) {
  observers_.push_back(observer);
}

void PhiSpaceManager::RemoveObserver(Observer* observer) {
  std::erase(observers_, observer);
}

void PhiSpaceManager::Load(const std::wstring& user_data_dir) {
  store_->Init(user_data_dir);
  // The load leaves the model usable: decoded, imported from the
  // pre-phase-3 files on a first run over an old profile, or defaults over
  // a quarantined file (the truth-owner rules are in phi_app_model.h).
  store_->Load(&model_);
}

void PhiSpaceManager::Save() {
  store_->Save(model_);
}

// --- Reads --------------------------------------------------------------------

std::vector<PhiSpaceView> PhiSpaceManager::Spaces() const {
  std::vector<PhiSpaceView> out;
  out.reserve(model_.spaces.size());
  for (const model::Space& space : model_.spaces) {
    PhiSpaceView view;
    view.id = space.id;
    view.name = space.name;
    view.profile_id = space.profile_id;
    view.icon = space.icon;
    view.icon_text = PhiIconDisplayText(space.icon);
    out.push_back(std::move(view));
  }
  return out;
}

std::vector<PhiSpaceSummary> PhiSpaceManager::Summaries() const {
  std::vector<PhiSpaceSummary> out;
  out.reserve(model_.spaces.size());
  for (const model::Space& space : model_.spaces) {
    out.push_back({space.id, space.name, PhiIconDisplayText(space.icon),
                   space.profile_id, space.theme_id, space.saturation});
  }
  return out;
}

std::string PhiSpaceManager::DefaultSpaceId() const {
  return DefaultSpaceIdOf(model_.spaces, model_.default_space_id);
}

bool PhiSpaceManager::CanDeleteSpace(const std::string& space_id) const {
  return CanDeleteSpaceIn(model_.spaces, space_id);
}

std::string PhiSpaceManager::KioskTargetSpaceId() const {
  if (PhiShellWindow* main = registry_.FirstUserController()) {
    const std::string& active = main->active_space();
    if (!active.empty()) {
      return active;
    }
  }
  // DefaultSpaceId itself falls back to the first Space, so this always
  // names one as long as any exists.
  return DefaultSpaceId();
}

std::string PhiSpaceManager::SpaceThemeId(const std::string& space_id) const {
  const model::Space* space = Find(space_id);
  return space ? space->theme_id : std::string();
}

int PhiSpaceManager::SpaceSaturation(const std::string& space_id) const {
  const model::Space* space = Find(space_id);
  return space ? space->saturation : -1;  // -1 follows the app-wide slider.
}

// --- Writes -------------------------------------------------------------------

bool PhiSpaceManager::RenameSpace(const std::string& space_id,
                                  const std::string& name) {
  if (name.empty()) {
    return false;
  }
  for (model::Space& space : model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    space.name = name;
    MarkDirty();
    Notify(SpacesChange::kList);
    return true;
  }
  return false;
}

bool PhiSpaceManager::DeleteSpace(const std::string& space_id) {
  // Only the LAST Space is undeletable (the Mac's last-Space rule); the
  // default Space itself may go, handing its role on below.
  if (!CanDeleteSpace(space_id)) {
    return false;
  }
  auto it = std::find_if(
      model_.spaces.begin(), model_.spaces.end(),
      [&](const model::Space& s) { return s.id == space_id; });
  if (it == model_.spaces.end()) {
    return false;
  }
  if (space_id == model_.default_space_id) {
    // The default role passes to the first REMAINING Space (strip order),
    // before the windows retreat so anything landing on the default lands
    // on the successor; the Spaces save below persists the hand-off.
    const std::string successor =
        DefaultRoleSuccessor(model_.spaces, space_id);
    if (!successor.empty()) {
      model_.default_space_id = successor;
    }
    LOG(INFO) << "[PhiClient] default Space role handed to "
              << model_.default_space_id;
  }
  // A slot whose ONLY browser is the deleted Space's would be torn down by
  // the close below (a window closes with its last browser). Retreat it to
  // the default Space first — spawning that Space's browser into the slot
  // (synchronous for a loaded profile) — the Mac's retreat-then-close order.
  // Now reachable for the default Space itself, whose browser is usually the
  // slot's original one.
  const std::string retreat = DefaultSpaceId();
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->is_agent() || !controller->HasSpace(space_id) ||
        retreat == space_id) {
      continue;
    }
    bool has_other = false;
    for (const model::Space& space : model_.spaces) {
      if (space.id != space_id && controller->HasSpace(space.id)) {
        has_other = true;
        break;
      }
    }
    if (!has_other) {
      host_.SurfaceSpaceInSlot(controller->slot(), retreat, nullptr);
    }
  }
  // Close the Space's browser wherever it is open; each controller switches
  // away first so a live window never keeps showing a deleted Space.
  for (PhiShellWindow* controller : registry_.Controllers()) {
    controller->CloseSpaceBrowser(space_id);
  }
  // The Space's bookmarks and space-scoped pins go with it (the Mac's
  // delete-confirmation promise) — and its URL rules, which would otherwise
  // route into a Space that no longer exists, and its ghost record (a
  // parked window is dropped on the session side too).
  host_.ForgetSpaceData(space_id);
  model_.spaces.erase(it);
  // The list, the default role and the rules are one file: one save.
  MarkDirty();
  RebuildSpacePalettes();
  Notify(SpacesChange::kList);
  return true;
}

void PhiSpaceManager::ReorderSpace(const std::string& space_id,
                                   int new_index) {
  auto it = std::find_if(
      model_.spaces.begin(), model_.spaces.end(),
      [&](const model::Space& s) { return s.id == space_id; });
  if (it == model_.spaces.end()) {
    return;
  }
  model::Space moved = *it;
  model_.spaces.erase(it);
  const size_t clamped = ClampSpaceInsertIndex(model_.spaces.size(), new_index);
  model_.spaces.insert(model_.spaces.begin() + static_cast<ptrdiff_t>(clamped),
                       std::move(moved));
  MarkDirty();
  Notify(SpacesChange::kOrder);
}

void PhiSpaceManager::SetSpaceIcon(const std::string& space_id,
                                   const std::string& icon) {
  if (!PhiIconIsValid(icon)) {
    return;
  }
  for (model::Space& space : model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    space.icon = icon;
    MarkDirty();
    Notify(SpacesChange::kList);
    return;
  }
}

void PhiSpaceManager::SetSpaceTheme(const std::string& space_id,
                                    const std::string& theme_id) {
  for (model::Space& space : model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    // An empty id clears the override, putting the Space back on the app-wide
    // theme — the Mac's "no entry in spaceThemeIds" state.
    space.theme_id =
        theme_id.empty() ? std::string() : PhiThemeById(theme_id).id;
    MarkDirty();
    RebuildSpacePalettes();
    Notify(SpacesChange::kTheme);
    return;
  }
}

void PhiSpaceManager::SetSpaceSaturation(const std::string& space_id,
                                         int value) {
  for (model::Space& space : model_.spaces) {
    if (space.id != space_id) {
      continue;
    }
    // -1 follows the app-wide slider; anything else clamps to its range.
    space.saturation = value < 0 ? -1 : (value > 100 ? 100 : value);
    MarkDirty();
    RebuildSpacePalettes();
    Notify(SpacesChange::kTheme);
    return;
  }
}

void PhiSpaceManager::PinSpaceTheme(const std::string& space_id,
                                    const std::string& theme_id,
                                    int saturation) {
  for (model::Space& space : model_.spaces) {
    if (space.id == space_id) {
      space.theme_id = PhiThemeById(theme_id).id;
      space.saturation = saturation < 0 ? -1 : std::min(100, saturation);
      break;
    }
  }
  MarkDirty();
  RebuildSpacePalettes();
  Notify(SpacesChange::kTheme);
}

bool PhiSpaceManager::SetSpaceProfile(const std::string& space_id,
                                      const std::string& profile_id) {
  if (space_id.empty() || profile_id.empty()) {
    return false;
  }
  auto it = std::find_if(
      model_.spaces.begin(), model_.spaces.end(),
      [&](const model::Space& s) { return s.id == space_id; });
  if (it == model_.spaces.end() || it->profile_id == profile_id) {
    return false;
  }
  // The default Space keeps its binding: it is what a fresh shell's FIRST
  // browser lands on, and that browser exists before any Space is chosen —
  // the Mac refuses the same Space for the same reason.
  const std::string retreat = DefaultSpaceId();
  if (space_id == retreat) {
    return false;
  }
  // A window's profile is baked in at spawn, so the Space's browser cannot
  // be re-pointed: it is closed and respawned in the new profile with its
  // open tabs reopened there (the Mac's changeProfile promise).
  std::vector<std::string> urls;
  SlotId respawn_slot;
  for (PhiShellWindow* controller : registry_.Controllers()) {
    if (controller->is_agent() || !controller->HasSpace(space_id)) {
      continue;
    }
    if (!respawn_slot.valid()) {
      respawn_slot = controller->slot();
      if (const phi_framework::WindowSnapshot* window =
              window_model_.Get(controller->BrowserIdForSpace(space_id))) {
        for (const phi_framework::TabSnapshot& tab : window->tabs) {
          if (!tab.url.empty()) {
            urls.push_back(tab.url);
          }
        }
      }
    }
    // A slot whose ONLY browser is this Space's would be torn down by the
    // close below (a window closes with its last browser) — retreat it to
    // the default Space first, the delete path's order.
    bool has_other = false;
    for (const model::Space& space : model_.spaces) {
      if (space.id != space_id && controller->HasSpace(space.id)) {
        has_other = true;
        break;
      }
    }
    if (!has_other) {
      host_.SurfaceSpaceInSlot(controller->slot(), retreat, nullptr);
    }
  }
  for (PhiShellWindow* controller : registry_.Controllers()) {
    controller->CloseSpaceBrowser(space_id);
  }
  it->profile_id = profile_id;
  MarkDirty();
  // The rows the Space owns move with it — the Mac's "bookmarks stay with
  // the Space" — and any parked window from the previous session belongs to
  // the OLD profile; materializing it now would reopen the Space where it
  // no longer lives.
  host_.RehomeSpaceData(space_id, profile_id);
  Notify(SpacesChange::kProfile);  // Repaint, Settings, the wire's list.
  if (respawn_slot.valid()) {
    // The Space has no browser any more, so this is a request; the tabs
    // ride on it and reopen once the replacement attaches.
    host_.SurfaceSpaceInSlot(respawn_slot, space_id, &urls);
  }
  LOG(INFO) << "[PhiClient] Space " << space_id << " re-bound to profile "
            << profile_id;
  return true;
}

// --- Palettes -----------------------------------------------------------------

void PhiSpaceManager::RebuildSpacePalettes() {
  const model::PhiSettings& settings = preferences_.settings();
  const PhiAppearance appearance = preferences_.Appearance();
  palette_by_space_.clear();
  for (const model::Space& space : model_.spaces) {
    COLORREF accent = 0;
    const bool has_hex = PhiParseColorHex(space.color_hex, &accent);
    if (space.theme_id.empty() && space.saturation < 0 && !has_hex) {
      continue;  // Follows the app-wide theme.
    }
    const int saturation =
        space.saturation < 0 ? settings.saturation : space.saturation;
    if (space.theme_id.empty() && has_hex) {
      // No pinned theme but a wire colorHex: the hex drives the Space's tint
      // (the Mac CreateSpacePanel's "colorHex drives the sidebar tint"
      // contract — Spaces created by the skill carry only the hex).
      PhiTheme custom = PhiThemeById(std::string());  // Pure's grounds.
      custom.light.overlay = accent;
      custom.light.accent = accent;
      custom.dark.overlay = accent;
      custom.dark.accent = accent;
      palette_by_space_[space.id] = std::make_unique<PhiPalette>(
          PhiResolvePalette(custom, appearance, saturation));
      continue;
    }
    const std::string& id =
        space.theme_id.empty() ? settings.theme_id : space.theme_id;
    palette_by_space_[space.id] = std::make_unique<PhiPalette>(
        PhiResolvePalette(PhiThemeById(id), appearance, saturation));
  }
}

const PhiPalette& PhiSpaceManager::PaletteForSpace(
    const std::string& space_id) const {
  auto found = palette_by_space_.find(space_id);
  if (found != palette_by_space_.end()) {
    return *found->second;
  }
  return preferences_.palette();
}

const PhiPalette& PhiSpaceManager::PaletteForSlot(int64_t slot) const {
  if (PhiShellWindow* window = registry_.Resolve(slot)) {
    return PaletteForSpace(window->active_space());
  }
  return preferences_.palette();
}

// --- The framework's copy -----------------------------------------------------

void PhiSpaceManager::PushSpaceMenuNow() {
  base::ListValue list;
  for (const model::Space& space : model_.spaces) {
    base::DictValue d;
    d.Set("id", space.id);
    d.Set("name", space.name);
    // The session id of the Space's open browser (any slot), so the menu
    // can exclude the Space the user is already looking at.
    BrowserId browser_id;
    for (PhiShellWindow* controller : registry_.Controllers()) {
      browser_id = controller->BrowserIdForSpace(space.id);
      if (browser_id.valid()) {
        break;
      }
    }
    d.Set("windowId", static_cast<double>(browser_id.value));
    list.Append(std::move(d));
  }
  std::string json;
  base::JSONWriter::Write(list, &json);
  framework_.SetSpaceMenu(json.c_str());
}

void PhiSpaceManager::MarkDirty() {
  if (dirty_) {
    dirty_();
  }
}

void PhiSpaceManager::Notify(SpacesChange what) {
  // Over a copy: an observer may unsubscribe from inside the callback.
  const std::vector<Observer*> observers = observers_;
  for (Observer* observer : observers) {
    observer->OnSpacesChanged(what);
  }
}

}  // namespace phi_client
