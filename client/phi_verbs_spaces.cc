// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <windows.h>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/rand_util.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_agent_spaces.h"
#include "phibrowser-win/client/phi_control_dispatcher.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"
#include "phibrowser-win/client/phi_window_registry.h"

// The user's own state: Spaces, profiles, url routing rules, pins,
// bookmarks and the emoji catalog. One handler per registry row.

namespace phi_client {

namespace {

// The slot a payload names, taken from the ALREADY RESOLVED scope. A wire
// windowId may be a slot id, a browser bound to one (what spaces.list
// reports in windowIds) or 0 for "the window", and only ResolveScope knows
// which (docs/architecture-review.md §3.7 item 5). Building a SlotId from
// the raw number instead made a browser id name no slot at all, so a pin
// list aimed at a Space's own window came back empty.
SlotId SlotOf(const PhiVerbContext& ctx) {
  return ctx.scope.controller ? ctx.scope.controller->slot()
                              : SlotId{ctx.params.Int64("windowId", 0)};
}

// Splits a tab-separated row (the coordinator's URLRuleRows() encoding).
// Keeps empty fields so positional indices stay stable.
std::vector<std::string> SplitTabRow(const std::string& value) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    size_t tab = value.find('\t', start);
    if (tab == std::string::npos) {
      parts.push_back(value.substr(start));
      break;
    }
    parts.push_back(value.substr(start, tab - start));
    start = tab + 1;
  }
  return parts;
}

}  // namespace

// --- Spaces ---------------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbSpacesList(const PhiVerbContext& ctx) {
  PhiWindowController* shell =
      coordinator_->registry().Resolve(ctx.params.Int64("windowId", 0));
  base::ListValue list;
  int sort_order = 0;
  for (const Space& space : coordinator_->spaces_model().spaces) {
    base::DictValue s;
    // The client-UI shape (id/icon/themeId) AND the skill's management shape
    // (spaceId/colorHex/iconName/sortOrder/isDefault/isActive) — one list
    // serves the box probes and the phi-browser skill both.
    s.Set("id", space.id);
    s.Set("spaceId", space.id);
    s.Set("name", space.name);
    s.Set("profileId", space.profile_id);
    s.Set("icon", space.icon);
    s.Set("iconName", space.icon);
    s.Set("colorHex", space.color_hex);
    s.Set("themeId", space.theme_id);
    s.Set("iconText", PhiIconDisplayText(space.icon));
    s.Set("sortOrder", sort_order++);
    s.Set("isDefault", space.id == coordinator_->DefaultSpaceId());
    const bool active = shell && shell->active_space() == space.id;
    s.Set("active", active);
    s.Set("isActive", active);
    s.Set("open", shell && shell->HasSpace(space.id));
    // Every user window showing the Space, as CDP windowIds (the Space's
    // browser in each slot) — what the skill pins a binding to
    // (enterContext({kind:'user', window})); empty when it is not open.
    base::ListValue window_ids;
    for (PhiWindowController* other :
         coordinator_->registry().Controllers()) {
      if (other->is_agent()) {
        continue;
      }
      const BrowserId browser = other->BrowserIdForSpace(space.id);
      if (browser.valid()) {
        window_ids.Append(static_cast<double>(browser.value));
      }
    }
    s.Set("windowIds", std::move(window_ids));
    list.Append(std::move(s));
  }
  base::DictValue out;
  out.Set("spaces", std::move(list));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesCreate(
    const PhiVerbContext& ctx) {
  const std::string id =
      std::to_string(coordinator_->spaces_model().next_space_id++);
  std::string name = ctx.params.String("name");
  if (name.empty()) {
    name = "Space " + id;
  }
  std::string profile_id = ctx.params.String("profileId");
  if (profile_id.empty()) {
    profile_id = "Default";  // A Space always binds exactly one profile.
  }
  // A named icon when the skill gave a valid one; a random catalog draw
  // otherwise, as the Mac's create panel does.
  const std::string icon_name = ctx.params.String("iconName");
  const auto& catalog = PhiIconCatalogAll();
  const std::string icon =
      PhiIconIsValid(icon_name)
          ? icon_name
          : "phi:" +
                std::string(catalog[base::RandGenerator(catalog.size())].id);
  Space space{id, name, profile_id, icon};
  space.color_hex = ctx.params.String("colorHex");
  coordinator_->spaces_model().spaces.push_back(std::move(space));
  coordinator_->ScheduleSave(PhiCoordinator::StoreDomain::kSpaces);
  // A wire colorHex tints the new Space.
  coordinator_->RebuildSpacePalettes();
  if (ctx.params.Bool("activate", false)) {
    // Surface it in the user's focused window — explicit opt-in only (the
    // skill's default create never yanks the user's window).
    PhiWindowController* focused = coordinator_->FocusedUserController();
    bool pending = false, cold = false;
    coordinator_->SwitchSlotToSpace(focused ? focused->slot() : SlotId(), id,
                                    &pending, &cold);
  }
  base::DictValue out;
  out.Set("id", id);
  out.Set("spaceId", id);
  out.Set("name", name);
  out.Set("profileId", profile_id);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesSwitch(
    const PhiVerbContext& ctx) {
  bool pending = false, cold = false;
  const bool ok = coordinator_->SwitchSlotToSpace(
      SlotOf(ctx), ctx.params.String("spaceId"),
      &pending, &cold);
  base::DictValue out;
  if (cold) {
    out.Set("pending", pending);  // Attaching when the browser arrives.
  }
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesRename(
    const PhiVerbContext& ctx) {
  return PhiVerbOk(coordinator_->RenameSpace(ctx.params.String("spaceId"),
                                             ctx.params.String("name")));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesDelete(
    const PhiVerbContext& ctx) {
  return PhiVerbOk(coordinator_->DeleteSpace(ctx.params.String("spaceId")));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesReorder(
    const PhiVerbContext& ctx) {
  const std::string space_id = ctx.params.String("spaceId");
  const int index = ctx.params.Int("index", -1);
  const bool known = index >= 0 && FindSpace(space_id) != nullptr;
  if (known) {
    coordinator_->ReorderSpace(space_id, index);
  }
  return PhiVerbOk(known);
}

PhiVerbResult PhiControlDispatcher::VerbSpacesSetTheme(
    const PhiVerbContext& ctx) {
  const std::string space_id = ctx.params.String("spaceId");
  const std::string theme_id = ctx.params.String("themeId");
  // An empty id is legal: it clears the override.
  const bool known = theme_id.empty() || theme_id == PhiThemeById(theme_id).id;
  bool ok = false;
  if (known && FindSpace(space_id)) {
    coordinator_->SetSpaceTheme(space_id, theme_id);
    ok = true;
  }
  base::DictValue out;
  out.Set("themeId", coordinator_->SpaceThemeId(space_id));
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesSetIcon(
    const PhiVerbContext& ctx) {
  const std::string icon = ctx.params.String("icon");
  bool ok = false;
  if (PhiIconIsValid(icon)) {
    if (Space* space = FindSpace(ctx.params.String("spaceId"))) {
      space->icon = icon;
      coordinator_->ScheduleSave(PhiCoordinator::StoreDomain::kSpaces);
      coordinator_->registry().InvalidateAll();
      ok = true;
    }
  }
  base::DictValue out;
  out.Set("icon", icon);
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesListTabs(
    const PhiVerbContext& ctx) {
  std::string space_id, error;
  const BrowserId browser = UserWindowOf(ctx.scope, &space_id, &error);
  if (!browser.valid() || !framework_) {
    return PhiVerbResult::Fail(error.empty() ? "space_not_open" : error);
  }
  base::ListValue rows;
  if (const phi_framework::WindowSnapshot* window =
          coordinator_->window_model().Get(browser)) {
    for (const phi_framework::TabSnapshot& tab : window->tabs) {
      base::DictValue row;
      row.Set("tabId", tab.id.value);
      row.Set("index", tab.index);
      row.Set("url", tab.url);
      row.Set("title", tab.title);
      row.Set("active", tab.active);
      // Present only when set: the stored-cell binding tag (TagFor) and
      // the live split, so a probe can assert bindings without pixels.
      if (!tab.tag.empty()) {
        row.Set("tag", tab.tag);
      }
      if (!tab.split_id.empty()) {
        row.Set("splitId", tab.split_id);
      }
      rows.Append(std::move(row));
    }
  }
  base::DictValue out;
  out.Set("tabs", std::move(rows));
  out.Set("spaceId", space_id);  // A window-only bind learns its Space.
  out.Set("windowId", static_cast<double>(browser.value));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesOpenTab(
    const PhiVerbContext& ctx) {
  std::string space_id, error;
  const BrowserId browser = UserWindowOf(ctx.scope, &space_id, &error);
  if (!browser.valid() || !framework_) {
    return PhiVerbResult::Fail(error.empty() ? "space_not_open" : error);
  }
  const bool activate = ctx.params.Bool("activate", true);
  const TabId previous_active =
      activate ? TabId() : coordinator_->ActiveTabId(browser);
  // Bypassed like every client-side deliberate open: a URL rule pointing
  // at another Space must not bounce a tab the skill placed HERE.
  framework_->OpenTabBypassingSpaceRouting(browser.value,
                                           ctx.params.String("url").c_str());
  if (previous_active.valid()) {
    // The new tab opened foreground; hand the selection back by id.
    framework_->ActivateTabById(browser.value, previous_active.value);
  }
  base::DictValue out;
  out.Set("windowId", static_cast<double>(browser.value));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesFocus(
    const PhiVerbContext& ctx) {
  // A surfaced agent window in the foreground answers as the agent Space.
  const HWND foreground = ::GetForegroundWindow();
  for (PhiWindowController* controller :
       coordinator_->registry().Controllers()) {
    if (!controller->is_agent() || !foreground ||
        controller->shell_hwnd() != foreground) {
      continue;
    }
    // An agent window's slot is the browser it opened with (it never
    // switches Space), so the slot number names its window.
    PhiAgentSpaces::Space* agent = coordinator_->agent_spaces().FindByWindow(
        BrowserId{controller->slot().value});
    if (!agent) {
      break;
    }
    base::DictValue out;
    out.Set("spaceId", agent->space_id);
    out.Set("spaceName", "Agent Space");
    out.Set("isAgentSpace", true);
    out.Set("isIncognito", false);
    out.Set("windowId",
            static_cast<double>(
                coordinator_->ResolveBrowser(controller->slot()).value));
    return PhiVerbResult::Ok(std::move(out));
  }
  PhiWindowController* controller = coordinator_->FocusedUserController();
  if (!controller) {
    return PhiVerbResult::Fail("no_window");
  }
  const std::string space_id = controller->active_space();
  std::string name = space_id;
  if (const Space* space = FindSpace(space_id)) {
    name = space->name;
  }
  base::DictValue out;
  out.Set("spaceId", space_id);
  out.Set("spaceName", name);
  out.Set("isAgentSpace", false);
  out.Set("isIncognito", controller->is_incognito());
  const BrowserId browser = controller->active_browser_id();
  if (browser.valid()) {
    out.Set("windowId", static_cast<double>(browser.value));
    if (coordinator_->window_model().ActiveTab(browser)) {
      out.Set("tab", ActiveTabRow(browser));
    }
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbSpacesActivate(
    const PhiVerbContext& ctx) {
  const std::string space_id = ctx.params.String("spaceId");
  // A slot already hosting the Space surfaces it there; otherwise the
  // user's focused window takes it (cold-spawning the browser).
  for (PhiWindowController* controller :
       coordinator_->registry().Controllers()) {
    if (controller->is_agent() || !controller->HasSpace(space_id)) {
      continue;
    }
    const bool ok = controller->ShowSpace(space_id);
    if (ok) {
      coordinator_->PushSpaceMenu();
      if (controller->shell_hwnd()) {
        ::SetForegroundWindow(controller->shell_hwnd());
      }
      controller->ScheduleRefresh();
    }
    return PhiVerbOk(ok);
  }
  PhiWindowController* focused = coordinator_->FocusedUserController();
  if (!focused) {
    return PhiVerbResult::Fail("no_window");
  }
  bool pending = false, cold = false;
  if (!coordinator_->SwitchSlotToSpace(focused->slot(), space_id, &pending,
                                       &cold)) {
    return PhiVerbResult::Fail("unknown_space");
  }
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbSpacesUpdate(
    const PhiVerbContext& ctx) {
  const std::string space_id = ctx.params.String("spaceId");
  Space* entry = FindSpace(space_id);
  if (!entry) {
    return PhiVerbResult::Fail("unknown_space");
  }
  bool ok = true;
  if (const std::string* name = ctx.params.FindString("name")) {
    ok &= coordinator_->RenameSpace(space_id, *name);
  }
  if (const std::string* icon = ctx.params.FindString("iconName")) {
    if (PhiIconIsValid(*icon)) {
      entry->icon = *icon;
      coordinator_->ScheduleSave(PhiCoordinator::StoreDomain::kSpaces);
      coordinator_->registry().InvalidateAll();
    } else {
      ok = false;
    }
  }
  if (const std::string* color = ctx.params.FindString("colorHex")) {
    entry->color_hex = *color;
    coordinator_->ScheduleSave(PhiCoordinator::StoreDomain::kSpaces);
    // The hex drives the Space's tint when no theme is pinned.
    coordinator_->RebuildSpacePalettes();
    for (PhiWindowController* controller :
         coordinator_->registry().Controllers()) {
      controller->ApplyPalette();
    }
  }
  base::DictValue out;
  out.Set("spaceId", space_id);
  return PhiVerbOk(ok, std::move(out));
}

// --- Profiles ---------------------------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbProfilesList(
    const PhiVerbContext& ctx) {
  base::DictValue out;
  if (framework_) {
    std::optional<base::Value> v = base::JSONReader::Read(
        framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
    if (v && v->is_list()) {
      out.Set("profiles", std::move(*v));
    }
  }
  if (!out.contains("profiles")) {
    out.Set("profiles", base::ListValue());
  }
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbProfilesCreate(
    const PhiVerbContext& ctx) {
  const std::string name = ctx.params.String("name");
  if (!framework_ || name.empty()) {
    // Nothing to ask and nothing to name: the bare refusal, as before.
    return PhiVerbResult::Refuse(PhiErrorCode::kInvalidPayload);
  }
  // Creation is fire-and-forget in the framework, so the duplicate
  // display-name refusal has to happen here — against the profiles that
  // exist AND the ones already on their way.
  std::optional<base::Value> profiles = base::JSONReader::Read(
      framework_->ListProfilesJson(), base::JSON_PARSE_RFC);
  if (profiles && profiles->is_list()) {
    for (const base::Value& v : profiles->GetList()) {
      const base::DictValue* d = v.GetIfDict();
      const std::string* existing = d ? d->FindString("name") : nullptr;
      if (existing && *existing == name) {
        return PhiVerbResult::Fail("duplicate_name");
      }
    }
  }
  for (const PhiCoordinator::PendingProfile& pending :
       coordinator_->pending_profiles()) {
    if (pending.display_name == name) {
      return PhiVerbResult::Fail("duplicate_name");
    }
  }
  // Deferred: the basename exists only once OnProfileCreated names it,
  // and the skill's `const {profileId} = await createProfile(...)` needs
  // it in the reply.
  PhiCoordinator::PendingProfile pending;
  pending.display_name = name;
  pending.then = PhiCoordinator::PendingProfile::Then::kReplyOnly;
  pending.pending_reply_id =
      RegisterPendingReply(/*document=*/false, /*include_html=*/false);
  const int reply_id = pending.pending_reply_id;
  coordinator_->RequestProfile(std::move(pending));
  return PhiVerbResult::Deferred(reply_id);
}

PhiVerbResult PhiControlDispatcher::VerbProfilesRename(
    const PhiVerbContext& ctx) {
  // Both key spellings ride the schema's aliases: the box probes'
  // {id,name} and the Mac skill's {profileId,displayName}.
  return PhiVerbOk(coordinator_->RenameProfileById(ctx.params.String("id"),
                                                   ctx.params.String("name")));
}

PhiVerbResult PhiControlDispatcher::VerbProfilesDelete(
    const PhiVerbContext& ctx) {
  // Deleting a profile erases its data on disk. The skill contract
  // (references/management.md) promises agents can create and rename
  // profiles but never delete them, so from the pipe this is a test-rig
  // verb only; and even the rig has to say `confirm:true`, the nod the
  // Settings pane asks the user for.
  if (ctx.caller == ControlCaller::kPipe &&
      !coordinator_->settings().test_rig) {
    return PhiVerbResult::Fail(PhiErrorCode::kTestRigOnly);
  }
  if (!ctx.params.Bool("confirm", false)) {
    return PhiVerbResult::Fail(PhiErrorCode::kConfirmRequired);
  }
  return PhiVerbOk(coordinator_->DeleteProfileById(ctx.params.String("id")));
}

// --- URL routing rules -------------------------------------------------------------
// Rule "id" is "g<generation>-<index>" — the generation bumps on every rule
// write, so ids REGENERATE across writes (the Mac contract; callers must
// re-list before update/delete).

PhiVerbResult PhiControlDispatcher::VerbUrlRulesList(
    const PhiVerbContext& ctx) {
  base::ListValue rules;
  const std::vector<std::string> rows = coordinator_->URLRuleRows();
  for (size_t i = 0; i < rows.size(); ++i) {
    const std::vector<std::string> parts = SplitTabRow(rows[i]);
    base::DictValue d;
    d.Set("id", model::UrlRuleWireId(
                    coordinator_->spaces_model().url_rules_generation, i));
    d.Set("spaceId", parts.size() > 0 ? parts[0] : "");
    d.Set("host", parts.size() > 1 ? parts[1] : "");
    if (parts.size() > 2 && !parts[2].empty()) {
      d.Set("pathPrefix", parts[2]);
    } else {
      d.Set("pathPrefix", base::Value());
    }
    d.Set("ask", parts.size() > 3 && parts[3] == "1");
    d.Set("sortOrder", static_cast<int>(i));
    rules.Append(std::move(d));
  }
  base::DictValue out;
  out.Set("rules", std::move(rules));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbUrlRulesAdd(
    const PhiVerbContext& ctx) {
  const std::string space_id = ctx.params.String("spaceId");
  if (!SpaceExists(space_id)) {
    return PhiVerbResult::Fail("unknown_space");
  }
  coordinator_->AddURLRule(space_id,
                           base::ToLowerASCII(ctx.params.String("host")),
                           ctx.params.String("pathPrefix"),
                           ctx.params.Bool("ask", false));
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::UrlRuleEdit(const PhiVerbContext& ctx,
                                                bool remove) {
  int index = -1;
  if (const std::string* id = ctx.params.FindString("id")) {
    size_t parsed = 0;
    if (model::ParseUrlRuleWireId(
            *id, coordinator_->spaces_model().url_rules_generation, &parsed)) {
      index = static_cast<int>(parsed);  // The parser saturates at INT_MAX.
    } else if (id->find('-') != std::string::npos) {
      // A prior generation's id (or one with no index): the list has been
      // rewritten since it was read, so it no longer names anything.
      return PhiVerbResult::Fail("unknown_rule");
    }
  }
  const std::vector<std::string> rows = coordinator_->URLRuleRows();
  if (index < 0 || index >= static_cast<int>(rows.size())) {
    return PhiVerbResult::Fail(index < 0 ? "invalid_payload" : "unknown_rule");
  }
  if (remove) {
    coordinator_->RemoveURLRule(index);
    return PhiVerbResult::Ok();
  }
  // Update: host/pathPrefix change the pattern; spaceId/ask change the
  // target. Absent fields keep their current value.
  const std::vector<std::string> parts = SplitTabRow(rows[index]);
  std::string space_id = parts.size() > 0 ? parts[0] : "";
  std::string host = parts.size() > 1 ? parts[1] : "";
  std::string prefix = parts.size() > 2 ? parts[2] : "";
  bool ask = parts.size() > 3 && parts[3] == "1";
  if (const std::string* h = ctx.params.FindString("host")) {
    host = base::ToLowerASCII(*h);
  }
  // pathPrefix: key presence is meaningful (present clears/sets, absent
  // keeps) — but our storage can't distinguish; treat present as set.
  if (ctx.params.Has("pathPrefix")) {
    prefix = ctx.params.String("pathPrefix");
  }
  if (const std::string* s = ctx.params.FindString("spaceId")) {
    if (!SpaceExists(*s)) {
      return PhiVerbResult::Fail("unknown_space");
    }
    space_id = *s;
  }
  if (ctx.params.Has("ask")) {
    ask = ctx.params.Bool("ask", ask);
  }
  coordinator_->SetURLRulePattern(index, host, prefix);
  coordinator_->SetURLRuleTarget(index, space_id, ask);
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbUrlRulesUpdate(
    const PhiVerbContext& ctx) {
  return UrlRuleEdit(ctx, /*remove=*/false);
}

PhiVerbResult PhiControlDispatcher::VerbUrlRulesDelete(
    const PhiVerbContext& ctx) {
  return UrlRuleEdit(ctx, /*remove=*/true);
}

// --- Pins and bookmarks, the client dialect (windowId-scoped, flat) ---------------

PhiVerbResult PhiControlDispatcher::VerbPinnedScopeGet(
    const PhiVerbContext& ctx) {
  base::DictValue out;
  out.Set("scope", PhiPinScopeName(coordinator_->tab_store().pin_scope()));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbPinnedScopeSet(
    const PhiVerbContext& ctx) {
  PhiPinScope scope = coordinator_->tab_store().pin_scope();
  bool ok = false;
  if (const std::string* name = ctx.params.FindString("scope")) {
    ok = PhiPinScopeFromName(*name, &scope);
  }
  if (ok) {
    coordinator_->tab_store().SetPinScope(scope);
    coordinator_->MirrorPinScope();
    coordinator_->registry().InvalidateAll();
  }
  base::DictValue out;
  out.Set("scope", PhiPinScopeName(coordinator_->tab_store().pin_scope()));
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbPinnedList(
    const PhiVerbContext& ctx) {
  const SlotId slot = SlotOf(ctx);
  std::string profile_id, space_id;
  base::ListValue list;
  if (coordinator_->OwnerForSlot(slot, &profile_id, &space_id)) {
    for (const PhiRow& row :
         coordinator_->tab_store().PinnedFor(profile_id, space_id)) {
      base::DictValue entry;
      entry.Set("guid", row.guid);
      entry.Set("title", row.title);
      entry.Set("url", row.url);
      entry.Set("index", row.index);
      entry.Set("favicon", row.favicon_base64);
      entry.Set("faviconWidth", row.favicon_width);
      entry.Set("faviconHeight", row.favicon_height);
      entry.Set("secondaryUrl", row.url_b);
      entry.Set("secondaryTitle", row.title_b);
      entry.Set("layout", row.layout);
      list.Append(std::move(entry));
    }
  }
  base::DictValue out;
  out.Set("pinned", std::move(list));
  out.Set("scope", PhiPinScopeName(coordinator_->tab_store().pin_scope()));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbPinnedOpen(
    const PhiVerbContext& ctx) {
  // The Mac's openOrFocusPinnedTab, the same path a sidebar pin click
  // takes: focus the pin's bound live tab, or open one bound to the pin.
  const std::string guid = ctx.params.String("guid");
  PhiWindowController* controller = ctx.scope.controller;
  std::string profile_id, space_id, url;
  bool known = false;
  if (controller &&
      coordinator_->OwnerForSlot(controller->slot(), &profile_id, &space_id)) {
    // Pins and sidebar bookmarks both bind (bookmarkGuidBacking) — resolve
    // the guid against either kind of stored row.
    for (const PhiRow& row : coordinator_->tab_store().rows()) {
      if (row.guid == guid && row.type != PhiRowType::kBookmarkFolder) {
        url = row.url;
        known = true;
        break;
      }
    }
  }
  if (!known) {
    return PhiVerbResult::Fail("unknown pin");
  }
  controller->OpenOrFocusPinned(guid, url);
  return PhiVerbResult::Ok();
}

PhiVerbResult PhiControlDispatcher::VerbPinnedAdd(const PhiVerbContext& ctx) {
  const SlotId slot = SlotOf(ctx);
  std::string title = ctx.params.String("title");
  std::string url = ctx.params.String("url");
  std::string profile_id, space_id, guid;
  base::DictValue out;
  if (coordinator_->OwnerForSlot(slot, &profile_id, &space_id)) {
    // No url given: pin what the slot is showing.
    if (url.empty()) {
      if (const phi_framework::WindowSnapshot* window =
              coordinator_->window_model().Get(
                  coordinator_->ResolveBrowser(slot.value))) {
        url = window->nav.url;
        if (title.empty()) {
          title = window->nav.title;
        }
      }
    }
    int fw = 0, fh = 0;
    // Only when we are pinning what the slot is SHOWING: the live favicon
    // belongs to the active tab, not to some other url the caller named.
    const std::string favicon = url == coordinator_->LiveUrl(slot)
                                    ? coordinator_->CaptureFavicon(slot, &fw,
                                                                   &fh)
                                    : std::string();
    guid = coordinator_->tab_store().AddPinned(profile_id, space_id, title, url,
                                               favicon, fw, fh);
    // Report what was captured: a caller (or a test) can then tell "no icon
    // yet" apart from "the pin failed".
    out.Set("faviconWidth", fw);
    out.Set("faviconHeight", fh);
    out.Set("liveUrl", coordinator_->LiveUrl(slot));
  }
  if (!guid.empty()) {
    coordinator_->registry().InvalidateAll();
  }
  out.Set("guid", guid);
  return PhiVerbOk(!guid.empty(), std::move(out));
}

PhiVerbResult PhiControlDispatcher::StoredRowRemove(
    const PhiVerbContext& ctx) {
  // Route through RemoveStoredRow: it also clears the identity tag from
  // any tab bound to the row (verb and UI paths must behave alike).
  const std::string guid = ctx.params.String("guid");
  const PhiRow* row =
      guid.empty() ? nullptr : coordinator_->tab_store().Find(guid);
  if (!row) {
    return PhiVerbResult::Refuse(PhiErrorCode::kInvalidPayload);
  }
  // The owner rides the response so the skill can settle against the
  // right Space's list after the row is gone.
  base::DictValue out;
  out.Set("spaceId", row->space_id);
  coordinator_->RemoveStoredRow(guid);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbPinnedRemove(
    const PhiVerbContext& ctx) {
  return StoredRowRemove(ctx);
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksRemove(
    const PhiVerbContext& ctx) {
  return StoredRowRemove(ctx);
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksListFlat(
    const PhiVerbContext& ctx) {
  const SlotId slot = SlotOf(ctx);
  const std::string parent = ctx.params.String("parentGuid");
  std::string profile_id, space_id;
  base::ListValue list;
  if (coordinator_->OwnerForSlot(slot, &profile_id, &space_id)) {
    for (const PhiRow& row : coordinator_->tab_store().BookmarksFor(
             profile_id, space_id, parent)) {
      base::DictValue entry;
      entry.Set("guid", row.guid);
      entry.Set("title", row.title);
      entry.Set("url", row.url);
      entry.Set("index", row.index);
      entry.Set("folder", row.type == PhiRowType::kBookmarkFolder);
      entry.Set("favicon", row.favicon_base64);
      entry.Set("faviconWidth", row.favicon_width);
      entry.Set("faviconHeight", row.favicon_height);
      entry.Set("secondaryUrl", row.url_b);
      entry.Set("secondaryTitle", row.title_b);
      list.Append(std::move(entry));
    }
  }
  base::DictValue out;
  out.Set("bookmarks", std::move(list));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksAddFlat(
    const PhiVerbContext& ctx) {
  const SlotId slot = SlotOf(ctx);
  const bool folder = ctx.params.Bool("folder", false);
  std::string title = ctx.params.String("title");
  std::string url = ctx.params.String("url");
  const std::string parent = ctx.params.String("parentGuid");
  std::string profile_id, space_id, guid;
  if (coordinator_->OwnerForSlot(slot, &profile_id, &space_id)) {
    if (!folder && url.empty()) {
      if (const phi_framework::WindowSnapshot* window =
              coordinator_->window_model().Get(
                  coordinator_->ResolveBrowser(slot.value))) {
        url = window->nav.url;
        if (title.empty()) {
          title = window->nav.title;
        }
      }
    }
    int fw = 0, fh = 0;
    const std::string favicon =
        (!folder && url == coordinator_->LiveUrl(slot))
            ? coordinator_->CaptureFavicon(slot, &fw, &fh)
            : std::string();
    guid = coordinator_->tab_store().AddBookmark(
        profile_id, space_id, parent, title, url, folder, favicon, fw, fh);
  }
  if (!guid.empty()) {
    coordinator_->registry().InvalidateAll();
  }
  base::DictValue out;
  out.Set("guid", guid);
  return PhiVerbOk(!guid.empty(), std::move(out));
}

// --- Bookmarks, the skill's Space-addressed tree forms ------------------------------

namespace {

// The Space a tree-shaped bookmark payload targets: an explicit spaceId,
// else the default Space (else the first).
const model::Space* TreeTargetSpace(PhiCoordinator* coordinator,
                                    const std::string& space_id,
                                    bool* unknown) {
  *unknown = false;
  const auto& spaces = coordinator->spaces_model().spaces;
  if (!space_id.empty()) {
    for (const model::Space& space : spaces) {
      if (space.id == space_id) {
        return &space;
      }
    }
    *unknown = true;
    return nullptr;
  }
  if (spaces.empty()) {
    return nullptr;
  }
  const model::Space* target = &spaces.front();
  for (const model::Space& space : spaces) {
    if (space.id == coordinator->DefaultSpaceId()) {  // The default role.
      target = &space;
      break;
    }
  }
  return target;
}

}  // namespace

PhiVerbResult PhiControlDispatcher::VerbBookmarksListTree(
    const PhiVerbContext& ctx) {
  bool unknown = false;
  const Space* target =
      TreeTargetSpace(coordinator_, ctx.params.String("spaceId"), &unknown);
  if (!target) {
    return PhiVerbResult::Fail("unknown_space");
  }
  (void)unknown;
  const std::string profile_id = target->profile_id;
  const std::string space_id = target->id;
  // Folders carry `children`, leaves carry `url` — the Mac tree shape.
  auto build = [&](auto&& self, const std::string& parent) -> base::ListValue {
    base::ListValue nodes;
    for (const PhiRow& row : coordinator_->tab_store().BookmarksFor(
             profile_id, space_id, parent)) {
      base::DictValue node;
      node.Set("guid", row.guid);
      node.Set("title", row.title);
      node.Set("index", row.index);
      if (row.type == PhiRowType::kBookmarkFolder) {
        node.Set("children", self(self, row.guid));
      } else {
        node.Set("url", row.url);
      }
      nodes.Append(std::move(node));
    }
    return nodes;
  };
  base::DictValue out;
  out.Set("bookmarks", build(build, ""));
  out.Set("spaceId", space_id);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::BookmarkTreeAdd(const PhiVerbContext& ctx,
                                                    bool folder) {
  bool unknown = false;
  const Space* target =
      TreeTargetSpace(coordinator_, ctx.params.String("spaceId"), &unknown);
  if (!target) {
    return PhiVerbResult::Fail("unknown_space");
  }
  (void)unknown;
  const std::string profile_id = target->profile_id;
  const std::string space_id = target->id;
  std::string parent;
  if (const std::string* p = ctx.params.FindString("parentGuid")) {
    const PhiRow* parent_row = coordinator_->tab_store().Find(*p);
    if (!parent_row || parent_row->type != PhiRowType::kBookmarkFolder ||
        parent_row->space_id != space_id) {
      return PhiVerbResult::Fail("unknown_folder");
    }
    parent = *p;
  }
  const std::string guid = coordinator_->tab_store().AddBookmark(
      profile_id, space_id, parent, ctx.params.String("title"),
      folder ? std::string() : ctx.params.String("url"), folder,
      /*favicon_base64=*/"", 0, 0);
  if (guid.empty()) {
    return PhiVerbResult::Fail("create_failed");
  }
  if (std::optional<int> index = ctx.params.FindInt("index")) {
    coordinator_->tab_store().Reorder(guid, *index);
  }
  coordinator_->registry().InvalidateAll();
  base::DictValue out;
  out.Set("guid", guid);
  out.Set("spaceId", space_id);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksAddTree(
    const PhiVerbContext& ctx) {
  return BookmarkTreeAdd(ctx, /*folder=*/false);
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksAddFolder(
    const PhiVerbContext& ctx) {
  return BookmarkTreeAdd(ctx, /*folder=*/true);
}

PhiVerbResult PhiControlDispatcher::BookmarkTreeOp(const PhiVerbContext& ctx,
                                                   std::string_view op) {
  const std::string guid = ctx.params.String("guid");
  const PhiRow* row = coordinator_->tab_store().Find(guid);
  if (!row || (row->type != PhiRowType::kBookmark &&
               row->type != PhiRowType::kBookmarkFolder)) {
    return PhiVerbResult::Fail("unknown_bookmark");
  }
  const std::string owner_space = row->space_id;
  bool ok = true;
  if (op == "update") {
    if (const std::string* title = ctx.params.FindString("title")) {
      ok &= coordinator_->RenameStoredRow(guid, *title);
    }
    if (const std::string* icon = ctx.params.FindString("icon")) {
      // Folders only, as on Mac; an unknown id resolves to "default".
      ok &= row->type == PhiRowType::kBookmarkFolder &&
            coordinator_->SetStoredRowIcon(guid, PhiResolveFolderIcon(*icon));
    }
    if (const std::string* layout = ctx.params.FindString("layout")) {
      // Split rows only; the divider axis the pair reopens with.
      ok &= (*layout == "vertical" || *layout == "horizontal") &&
            coordinator_->SetStoredRowLayout(guid, *layout);
    }
    if (const std::string* url = ctx.params.FindString("url")) {
      ok &= coordinator_->tab_store().SetRowUrl(guid, *url);
    }
  } else {
    ok = coordinator_->tab_store().Move(guid, ctx.params.String("parentGuid"),
                                        ctx.params.Int("index", -1));
  }
  coordinator_->registry().InvalidateAll();
  base::DictValue out;
  out.Set("spaceId", owner_space);
  return PhiVerbOk(ok, std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksUpdate(
    const PhiVerbContext& ctx) {
  return BookmarkTreeOp(ctx, "update");
}

PhiVerbResult PhiControlDispatcher::VerbBookmarksMove(
    const PhiVerbContext& ctx) {
  return BookmarkTreeOp(ctx, "move");
}

// --- Pinned tabs, the skill dialect (per-profile records) ---------------------------

PhiVerbResult PhiControlDispatcher::VerbPinnedTabsList(
    const PhiVerbContext& ctx) {
  std::string profile_id, space_id;
  if (!DialectOwner(ctx.params.String("profileId"), &profile_id, &space_id)) {
    return PhiVerbResult::Fail("no_window");
  }
  base::ListValue rows;
  for (const PhiRow& row :
       coordinator_->tab_store().PinnedFor(profile_id, space_id)) {
    base::DictValue entry;
    entry.Set("guid", row.guid);
    entry.Set("url", row.url);
    entry.Set("title", row.title);
    entry.Set("index", row.index);
    entry.Set("profileId",
              row.profile_id.empty() ? profile_id : row.profile_id);
    rows.Append(std::move(entry));
  }
  base::DictValue out;
  out.Set("pinnedTabs", std::move(rows));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbPinnedTabsAdd(
    const PhiVerbContext& ctx) {
  std::string profile_id, space_id;
  if (!DialectOwner(ctx.params.String("profileId"), &profile_id, &space_id)) {
    return PhiVerbResult::Fail("no_window");
  }
  const std::string guid = coordinator_->tab_store().AddPinned(
      profile_id, space_id, ctx.params.String("title"),
      ctx.params.String("url"), /*favicon_base64=*/"", 0, 0);
  if (guid.empty()) {
    return PhiVerbResult::Fail("create_failed");
  }
  if (std::optional<int> index = ctx.params.FindInt("index")) {
    coordinator_->tab_store().Reorder(guid, *index);
  }
  coordinator_->registry().InvalidateAll();
  const PhiRow* row = coordinator_->tab_store().Find(guid);
  base::DictValue out;
  out.Set("guid", guid);
  out.Set("profileId",
          row && !row->profile_id.empty() ? row->profile_id : profile_id);
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::PinnedTabsOp(const PhiVerbContext& ctx,
                                                 std::string_view op) {
  const std::string guid = ctx.params.String("guid");
  if (!coordinator_->tab_store().Contains(guid)) {
    return PhiVerbResult::Fail("unknown_pin");
  }
  if (op == "remove") {
    coordinator_->RemoveStoredRow(guid);
    return PhiVerbResult::Ok();
  }
  bool ok = true;
  if (const std::string* title = ctx.params.FindString("title")) {
    ok &= coordinator_->RenameStoredRow(guid, *title);
  }
  if (const std::string* url = ctx.params.FindString("url")) {
    ok &= coordinator_->tab_store().SetRowUrl(guid, *url);
  }
  coordinator_->registry().InvalidateAll();
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::VerbPinnedTabsUpdate(
    const PhiVerbContext& ctx) {
  return PinnedTabsOp(ctx, "update");
}

PhiVerbResult PhiControlDispatcher::VerbPinnedTabsRemove(
    const PhiVerbContext& ctx) {
  return PinnedTabsOp(ctx, "remove");
}

// --- Rows and the emoji catalog --------------------------------------------------

PhiVerbResult PhiControlDispatcher::VerbRowsReorder(
    const PhiVerbContext& ctx) {
  const std::string guid = ctx.params.String("guid");
  const int index = ctx.params.Int("index", -1);
  const bool ok =
      !guid.empty() && index >= 0 && coordinator_->tab_store().Contains(guid);
  if (ok && coordinator_->tab_store().Reorder(guid, index)) {
    coordinator_->registry().InvalidateAll();
  }
  return PhiVerbOk(ok);
}

PhiVerbResult PhiControlDispatcher::VerbEmojiGroups(
    const PhiVerbContext& ctx) {
  // The catalog the Mac bundles, offered verbatim.
  base::ListValue groups;
  for (const PhiEmojiGroup& group : PhiEmojiGroups()) {
    base::DictValue g;
    g.Set("name", group.name);
    g.Set("count", static_cast<int>(group.items.size()));
    groups.Append(std::move(g));
  }
  base::DictValue out;
  out.Set("groups", std::move(groups));
  return PhiVerbResult::Ok(std::move(out));
}

PhiVerbResult PhiControlDispatcher::VerbEmojiList(const PhiVerbContext& ctx) {
  const std::string want = ctx.params.String("group");
  const int limit = ctx.params.Int("limit", 64);
  base::ListValue items;
  for (const PhiEmojiGroup& group : PhiEmojiGroups()) {
    if (!want.empty() && group.name != want) {
      continue;
    }
    for (const PhiEmoji& emoji : group.items) {
      if (static_cast<int>(items.size()) >= limit) {
        break;
      }
      base::DictValue e;
      e.Set("id", emoji.id);
      e.Set("text", emoji.text);
      e.Set("name", emoji.name);
      items.Append(std::move(e));
    }
  }
  base::DictValue out;
  out.Set("emoji", std::move(items));
  return PhiVerbResult::Ok(std::move(out));
}

}  // namespace phi_client
