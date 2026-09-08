// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_app_model.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_file_io.h"
#include "phibrowser-win/client/phi_reader.h"
#include "phibrowser-win/client/phi_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"

// The on-disk codecs for the four model files and the one-shot import of
// the files they replace. The legacy shapes here are EXACTLY what the
// coordinator's Load*/Save* pairs and PhiGhostSpaces wrote up to client
// 3447dd7 (field names, defaults, the phase-0 "schema" stamp); a field
// that is absent keeps the model default, as those loaders did.

namespace phi_client {

using model::PhiConsentModel;
using model::PhiRestoreModel;
using model::PhiSettings;
using model::PhiSpacesModel;

namespace {

// Legacy file names (pre-phase-3), imported once and then renamed.
constexpr const wchar_t* kLegacyPrefs = L"PhiPrefs.json";
constexpr const wchar_t* kLegacyLayout = L"PhiLayout.json";
constexpr const wchar_t* kLegacyTheme = L"PhiTheme.json";
constexpr const wchar_t* kLegacyUrlRules = L"PhiUrlRules.json";
constexpr const wchar_t* kLegacyGhosts = L"PhiGhostSpaces.json";
constexpr const wchar_t* kLegacyCredentialGrants = L"PhiCredentialGrants.json";
// Read for one field (pinScope), never renamed: the tab store still owns it.
constexpr const wchar_t* kTabStoreFile = L"PhiTabData.json";

std::wstring Join(const std::wstring& dir, const wchar_t* name) {
  return dir + L"\\" + name;
}

// A legacy file is read through the same quarantining reader as the live
// ones, so a truncated pre-phase-3 file ends up as evidence, not defaults.
PhiJsonFileStatus ReadLegacy(const std::wstring& dir,
                             const wchar_t* name,
                             base::DictValue* dict) {
  return PhiReadJsonFile(Join(dir, name), dict);
}

// The legacy file has been folded into its new home: keep it, renamed, so
// a rollback (or a doubting engineer) still has the original. Never
// deleted.
void MarkImported(const std::wstring& dir, const wchar_t* name) {
  const std::wstring from = Join(dir, name);
  const std::wstring to = from + L".imported";
  if (::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    LOG(INFO) << "[PhiStore] imported " << base::WideToUTF8(name) << " -> "
              << base::WideToUTF8(name) << ".imported";
  } else {
    LOG(ERROR) << "[PhiStore] imported " << base::WideToUTF8(name)
               << " but could not rename it, err=" << ::GetLastError();
  }
}

// --- Settings ---------------------------------------------------------------

// The three legacy files' fields, each in its own reader so Import and
// Decode share one spelling per field.
void DecodeLayoutFields(const base::DictValue& dict, PhiSettings* out) {
  if (const std::string* name = dict.FindString("layoutMode")) {
    PhiLayoutMode parsed = PhiLayoutMode::kPerformance;
    if (PhiLayoutModeFromName(*name, &parsed)) {
      out->layout_mode = parsed;
    }
  }
  if (std::optional<int> width = dict.FindInt("sidebarWidth")) {
    out->sidebar_width =
        std::max(kSidebarWidthMin, std::min(*width, kSidebarWidthMax));
  }
}

void DecodeThemeFields(const base::DictValue& dict, PhiSettings* out) {
  if (const std::string* id = dict.FindString("themeId")) {
    out->theme_id = PhiThemeById(*id).id;  // Unknown ids normalise to Pure.
  }
  if (const std::string* name = dict.FindString("appearance")) {
    model::AppearanceFromName(*name, &out->appearance);
  }
}

void DecodePrefsFields(const base::DictValue& dict, PhiSettings* out) {
  out->saturation = dict.FindInt("saturation").value_or(out->saturation);
  out->always_show_full_url =
      dict.FindBool("alwaysShowFullUrl").value_or(out->always_show_full_url);
  out->show_tab_previews =
      dict.FindBool("showTabPreviews").value_or(out->show_tab_previews);
  out->peek_view_enabled =
      dict.FindBool("peekViewEnabled").value_or(out->peek_view_enabled);
  out->auto_peek_view_enabled = dict.FindBool("autoPeekViewEnabled")
                                    .value_or(out->auto_peek_view_enabled);
  out->restore_last_session =
      dict.FindBool("restoreLastSession").value_or(out->restore_last_session);
  out->always_show_bookmark_bar = dict.FindBool("alwaysShowBookmarkBar")
                                      .value_or(out->always_show_bookmark_bar);
  out->bookmark_bar_on_new_tab = dict.FindBool("bookmarkBarOnNewTab")
                                     .value_or(out->bookmark_bar_on_new_tab);
  out->developer_mode =
      dict.FindBool("developerMode").value_or(out->developer_mode);
  out->allow_agent_control =
      dict.FindBool("allowAgentControl").value_or(out->allow_agent_control);
  out->allow_agent_user_spaces = dict.FindBool("allowAgentUserSpaces")
                                     .value_or(out->allow_agent_user_spaces);
  if (const base::ListValue* blocked =
          dict.FindList("disallowedAgentProfileIds")) {
    out->disallowed_agent_profile_ids.clear();
    for (const base::Value& entry : *blocked) {
      if (const std::string* id = entry.GetIfString()) {
        out->disallowed_agent_profile_ids.push_back(*id);
      }
    }
  }
  out->test_rig = dict.FindBool("testRig").value_or(out->test_rig);
  if (const std::string* language = dict.FindString("language")) {
    out->language = *language;
  }
  if (const std::string* behavior = dict.FindString("newTabBehavior")) {
    // The coordinator read anything but "omnibox" as the new tab page.
    out->new_tab_behavior = *behavior == "omnibox"
                                ? model::NewTabBehavior::kOmnibox
                                : model::NewTabBehavior::kNewTabPage;
  }
  if (const base::DictValue* reader = dict.FindDict("reader")) {
    // Through PhiReaderStyle so the range checks live in one place.
    PhiReaderStyle style;
    style.font_size = out->reader.font_size;
    style.width = out->reader.width;
    style.theme = out->reader.theme;
    style.typeface = out->reader.typeface;
    style.line_height = out->reader.line_height;
    style.letter_spacing = out->reader.letter_spacing;
    style.shows_images = out->reader.shows_images;
    style.highlights_links = out->reader.highlights_links;
    style.MergeFrom(*reader);
    out->reader.font_size = style.font_size;
    out->reader.width = style.width;
    out->reader.theme = style.theme;
    out->reader.typeface = style.typeface;
    out->reader.line_height = style.line_height;
    out->reader.letter_spacing = style.letter_spacing;
    out->reader.shows_images = style.shows_images;
    out->reader.highlights_links = style.highlights_links;
  }
  if (const base::DictValue* shortcuts = dict.FindDict("shortcuts")) {
    for (const auto [name, value] : *shortcuts) {
      const base::DictValue* chord = value.GetIfDict();
      if (!chord) {
        continue;
      }
      out->shortcut_overrides[name] = {
          static_cast<unsigned>(chord->FindInt("vk").value_or(0)),
          chord->FindBool("ctrl").value_or(false),
          chord->FindBool("shift").value_or(false),
          chord->FindBool("alt").value_or(false)};
    }
  }
}

base::DictValue EncodeReader(const model::ReaderStyle& reader) {
  PhiReaderStyle style;
  style.font_size = reader.font_size;
  style.width = reader.width;
  style.theme = reader.theme;
  style.typeface = reader.typeface;
  style.line_height = reader.line_height;
  style.letter_spacing = reader.letter_spacing;
  style.shows_images = reader.shows_images;
  style.highlights_links = reader.highlights_links;
  return style.ToDict();
}

// --- Spaces -----------------------------------------------------------------

void DecodeSpaceList(const base::ListValue& list, PhiSpacesModel* out) {
  std::vector<model::Space> loaded;
  for (const base::Value& v : list) {
    const base::DictValue* d = v.GetIfDict();
    if (!d) {
      continue;
    }
    const std::string* id = d->FindString("id");
    if (!id || id->empty()) {
      continue;
    }
    const std::string* name = d->FindString("name");
    const std::string* profile = d->FindString("profileId");
    const std::string* icon = d->FindString("icon");
    const std::string* theme = d->FindString("themeId");
    const std::string* color = d->FindString("colorHex");
    model::Space space;
    space.id = *id;
    space.name = name ? *name : ("Space " + *id);
    space.profile_id = profile && !profile->empty() ? *profile : "Default";
    space.icon =
        icon && PhiIconIsValid(*icon) ? *icon : std::string(kPhiDefaultSpaceIcon);
    space.theme_id = theme ? *theme : std::string();
    space.saturation = d->FindInt("saturation").value_or(-1);
    space.color_hex = color ? *color : std::string();
    loaded.push_back(std::move(space));
  }
  // An empty or all-invalid list keeps the built-in default Space, as the
  // coordinator's loader did — a Space list with nothing in it is not a
  // state the UI can show.
  if (!loaded.empty()) {
    out->spaces = std::move(loaded);
  }
}

base::ListValue EncodeSpaceList(const std::vector<model::Space>& spaces) {
  base::ListValue list;
  for (const model::Space& space : spaces) {
    base::DictValue d;
    d.Set("id", space.id);
    d.Set("name", space.name);
    d.Set("profileId", space.profile_id);
    d.Set("icon", space.icon);
    d.Set("themeId", space.theme_id);
    d.Set("saturation", space.saturation);
    d.Set("colorHex", space.color_hex);
    list.Append(std::move(d));
  }
  return list;
}

void DecodeRuleList(const base::ListValue& list,
                    std::vector<model::UrlRule>* out) {
  out->clear();
  for (const base::Value& v : list) {
    const base::DictValue* d = v.GetIfDict();
    if (!d) {
      continue;
    }
    const std::string* space_id = d->FindString("spaceId");
    const std::string* host = d->FindString("host");
    if (!space_id || space_id->empty() || !host || host->empty()) {
      continue;
    }
    model::UrlRule rule;
    rule.space_id = *space_id;
    rule.host = *host;
    if (const std::string* prefix = d->FindString("pathPrefix")) {
      rule.path_prefix = *prefix;
    }
    rule.ask = d->FindBool("ask").value_or(false);
    out->push_back(std::move(rule));
  }
}

base::ListValue EncodeRuleList(const std::vector<model::UrlRule>& rules) {
  base::ListValue list;
  for (const model::UrlRule& rule : rules) {
    base::DictValue d;
    d.Set("spaceId", rule.space_id);
    d.Set("host", rule.host);
    if (!rule.path_prefix.empty()) {
      d.Set("pathPrefix", rule.path_prefix);
    }
    d.Set("ask", rule.ask);
    list.Append(std::move(d));
  }
  return list;
}

// --- Restore ----------------------------------------------------------------

// {spaceId, profile, windowId} rows — the ghost store's shape, kept for the
// new file's "remembered" list so the two read alike.
void DecodeRememberedList(const base::ListValue& list,
                          std::vector<model::RememberedWindow>* out) {
  for (const base::Value& v : list) {
    const base::DictValue* d = v.GetIfDict();
    if (!d) {
      continue;
    }
    const std::string* space = d->FindString("spaceId");
    const std::string* profile = d->FindString("profile");
    const std::optional<int> window_id = d->FindInt("windowId");
    if (!space || space->empty() || !profile || !window_id) {
      continue;
    }
    out->push_back({*space, *profile, *window_id});
  }
}

// Which legacy prefs file still holds lastActiveSpaceId: the live one if
// the settings store has not imported it yet, else its .imported copy.
// Neither is renamed here — that is the settings store's move.
std::optional<std::string> LegacyLastActiveSpaceId(const std::wstring& dir) {
  for (const std::wstring& name :
       {std::wstring(kLegacyPrefs), std::wstring(kLegacyPrefs) + L".imported"}) {
    base::DictValue prefs;
    if (PhiReadJsonFile(Join(dir, name.c_str()), &prefs) !=
        PhiJsonFileStatus::kOk) {
      continue;
    }
    if (const std::string* last = prefs.FindString("lastActiveSpaceId")) {
      return *last;
    }
    return std::nullopt;  // The file is there but never stored one.
  }
  return std::nullopt;
}

// --- Consent ----------------------------------------------------------------

void DecodeAudit(const base::ListValue& list, std::vector<std::string>* out) {
  for (const base::Value& v : list) {
    if (v.is_string()) {
      out->push_back(v.GetString());
    }
  }
}

}  // namespace

// =============================================================================
// PhiSettings.json (schema 1)
// =============================================================================

// static
base::DictValue PhiStoreTraits<PhiSettings>::Encode(const PhiSettings& value) {
  base::DictValue root;
  root.Set("layoutMode", PhiLayoutModeName(value.layout_mode));
  root.Set("sidebarWidth", value.sidebar_width);
  root.Set("themeId", value.theme_id);
  root.Set("appearance", model::AppearanceName(value.appearance));
  root.Set("saturation", value.saturation);
  root.Set("newTabBehavior", model::NewTabBehaviorName(value.new_tab_behavior));
  root.Set("alwaysShowFullUrl", value.always_show_full_url);
  root.Set("showTabPreviews", value.show_tab_previews);
  root.Set("peekViewEnabled", value.peek_view_enabled);
  root.Set("autoPeekViewEnabled", value.auto_peek_view_enabled);
  root.Set("restoreLastSession", value.restore_last_session);
  root.Set("alwaysShowBookmarkBar", value.always_show_bookmark_bar);
  root.Set("bookmarkBarOnNewTab", value.bookmark_bar_on_new_tab);
  if (!value.shortcut_overrides.empty()) {
    base::DictValue shortcuts;
    for (const auto& [name, chord] : value.shortcut_overrides) {
      base::DictValue entry;
      entry.Set("vk", static_cast<int>(chord.vk));
      entry.Set("ctrl", chord.ctrl);
      entry.Set("shift", chord.shift);
      entry.Set("alt", chord.alt);
      shortcuts.Set(name, std::move(entry));
    }
    root.Set("shortcuts", std::move(shortcuts));
  }
  root.Set("reader", EncodeReader(value.reader));
  root.Set("language", value.language);
  root.Set("developerMode", value.developer_mode);
  root.Set("allowAgentControl", value.allow_agent_control);
  root.Set("allowAgentUserSpaces", value.allow_agent_user_spaces);
  base::ListValue blocked;
  for (const std::string& id : value.disallowed_agent_profile_ids) {
    blocked.Append(id);
  }
  root.Set("disallowedAgentProfileIds", std::move(blocked));
  root.Set("testRig", value.test_rig);
  root.Set("pinScope", PhiPinScopeName(value.pin_scope));
  root.Set("analyticsEnabled", value.analytics_enabled);
  return root;
}

// static
bool PhiStoreTraits<PhiSettings>::Decode(const base::DictValue& dict,
                                         int /*schema*/,
                                         PhiSettings* out) {
  // Field-tolerant like PhiPrefs.json always was: a hand-edited or partial
  // file keeps the defaults for what it does not name, so there is no
  // shape that fails to decode.
  DecodeLayoutFields(dict, out);
  DecodeThemeFields(dict, out);
  DecodePrefsFields(dict, out);
  if (const std::string* scope = dict.FindString("pinScope")) {
    PhiPinScopeFromName(*scope, &out->pin_scope);
  }
  out->analytics_enabled =
      dict.FindBool("analyticsEnabled").value_or(out->analytics_enabled);
  return true;
}

// static
bool PhiStoreTraits<PhiSettings>::Import(const std::wstring& dir,
                                         PhiSettings* out) {
  bool imported = false;
  base::DictValue dict;
  if (ReadLegacy(dir, kLegacyLayout, &dict) == PhiJsonFileStatus::kOk) {
    DecodeLayoutFields(dict, out);
    MarkImported(dir, kLegacyLayout);
    imported = true;
  }
  dict = base::DictValue();
  if (ReadLegacy(dir, kLegacyTheme, &dict) == PhiJsonFileStatus::kOk) {
    DecodeThemeFields(dict, out);
    MarkImported(dir, kLegacyTheme);
    imported = true;
  }
  dict = base::DictValue();
  if (ReadLegacy(dir, kLegacyPrefs, &dict) == PhiJsonFileStatus::kOk) {
    // lastActiveSpaceId is NOT ours: PhiRestore imports it (from this
    // file or its .imported copy, whichever the order of loads leaves).
    DecodePrefsFields(dict, out);
    MarkImported(dir, kLegacyPrefs);
    imported = true;
  }
  dict = base::DictValue();
  // The pin scope's writer stays PhiTabStore for now; this only carries the
  // user's choice over so the setting does not flip on upgrade.
  if (ReadLegacy(dir, kTabStoreFile, &dict) == PhiJsonFileStatus::kOk) {
    if (const std::string* scope = dict.FindString("pinScope")) {
      if (PhiPinScopeFromName(*scope, &out->pin_scope)) {
        imported = true;
      }
    }
  }
  return imported;
}

// =============================================================================
// PhiSpaces.json (schema 2; schema 1 is the pre-phase-3 file)
// =============================================================================

// static
base::DictValue PhiStoreTraits<PhiSpacesModel>::Encode(
    const PhiSpacesModel& value) {
  base::DictValue root;
  root.Set("spaces", EncodeSpaceList(value.spaces));
  root.Set("defaultSpaceId", value.default_space_id);
  root.Set("nextSpaceId", value.next_space_id);
  root.Set("rules", EncodeRuleList(value.url_rules));
  root.Set("urlRulesGeneration", value.url_rules_generation);
  return root;
}

// static
bool PhiStoreTraits<PhiSpacesModel>::Decode(const base::DictValue& dict,
                                            int schema,
                                            PhiSpacesModel* out) {
  if (const base::ListValue* list = dict.FindList("spaces")) {
    DecodeSpaceList(*list, out);
  }
  const std::string* persisted = dict.FindString("defaultSpaceId");
  out->default_space_id = model::ResolveDefaultSpaceId(
      out->spaces, persisted ? *persisted : std::string());
  // Never below what the list implies, whatever the stamp says: a restored
  // backup must not hand out an id a listed Space already holds.
  out->next_space_id = std::max(dict.FindInt("nextSpaceId").value_or(0),
                                model::NextSpaceIdAfter(out->spaces));
  if (schema >= 2) {
    if (const base::ListValue* rules = dict.FindList("rules")) {
      DecodeRuleList(*rules, &out->url_rules);
    }
    out->url_rules_generation = std::max(
        1, dict.FindInt("urlRulesGeneration").value_or(1));
  }
  // Schema 1 (and the unstamped file before phase 0) carried no rules:
  // PhiStore's migration path calls Import for PhiUrlRules.json next.
  return true;
}

// static
bool PhiStoreTraits<PhiSpacesModel>::Import(const std::wstring& dir,
                                            PhiSpacesModel* out) {
  // Only the rules file: the Space list has kept its name, so a pre-phase-3
  // PhiSpaces.json reaches Decode as schema 1, not Import. The rules file
  // was a bare list before phase 0 and {"schema":1,"rules":[...]} after;
  // both are read, anything else is quarantined (as LoadUrlRules did).
  const std::wstring path = Join(dir, kLegacyUrlRules);
  std::string json;
  if (!PhiReadFileBytes(path, &json)) {
    return false;
  }
  std::optional<base::Value> root =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  const base::ListValue* rules = nullptr;
  if (root && root->is_list()) {
    rules = &root->GetList();
  } else if (root && root->is_dict()) {
    rules = root->GetDict().FindList("rules");
  }
  if (!rules) {
    PhiQuarantineFile(path, "not a rule list");
    return false;
  }
  DecodeRuleList(*rules, &out->url_rules);
  MarkImported(dir, kLegacyUrlRules);
  return true;
}

// =============================================================================
// PhiRestore.json (schema 1)
// =============================================================================

// static
base::DictValue PhiStoreTraits<PhiRestoreModel>::Encode(
    const PhiRestoreModel& value) {
  base::DictValue root;
  base::ListValue remembered;
  for (const model::RememberedWindow& row : value.remembered) {
    base::DictValue d;
    d.Set("spaceId", row.space_id);
    d.Set("profile", row.profile);
    // SessionIDs are 32-bit on the framework side; the ghost store wrote
    // them as int too.
    d.Set("windowId", static_cast<int>(row.window_id));
    remembered.Append(std::move(d));
  }
  root.Set("remembered", std::move(remembered));
  base::DictValue parked;
  for (const auto& [profile, ids] : value.parked) {
    base::ListValue list;
    for (int64_t id : ids) {
      list.Append(static_cast<int>(id));
    }
    parked.Set(profile, std::move(list));
  }
  root.Set("parked", std::move(parked));
  root.Set("lastActiveSpaceId", value.last_active_space_id);
  return root;
}

// static
bool PhiStoreTraits<PhiRestoreModel>::Decode(const base::DictValue& dict,
                                             int /*schema*/,
                                             PhiRestoreModel* out) {
  if (const base::ListValue* list = dict.FindList("remembered")) {
    DecodeRememberedList(*list, &out->remembered);
  }
  if (const base::DictValue* parked = dict.FindDict("parked")) {
    for (const auto [profile, value] : *parked) {
      const base::ListValue* ids = value.GetIfList();
      if (!ids) {
        continue;
      }
      std::vector<int64_t>& rows = out->parked[profile];
      for (const base::Value& id : *ids) {
        if (id.is_int() && id.GetInt() > 0) {
          rows.push_back(id.GetInt());
        }
      }
    }
  }
  if (const std::string* last = dict.FindString("lastActiveSpaceId")) {
    out->last_active_space_id = *last;
  }
  return true;
}

// static
bool PhiStoreTraits<PhiRestoreModel>::Import(const std::wstring& dir,
                                             PhiRestoreModel* out) {
  bool imported = false;
  base::DictValue dict;
  if (ReadLegacy(dir, kLegacyGhosts, &dict) == PhiJsonFileStatus::kOk) {
    if (const base::ListValue* rows = dict.FindList("spaces")) {
      DecodeRememberedList(*rows, &out->remembered);
    }
    MarkImported(dir, kLegacyGhosts);
    imported = true;
  }
  if (std::optional<std::string> last = LegacyLastActiveSpaceId(dir)) {
    out->last_active_space_id = *last;
    LOG(INFO) << "[PhiStore] imported lastActiveSpaceId from "
              << base::WideToUTF8(kLegacyPrefs);
    imported = true;
  }
  return imported;
}

// =============================================================================
// PhiConsent.json (schema 1, sealed)
// =============================================================================

// static
base::DictValue PhiStoreTraits<PhiConsentModel>::Encode(
    const PhiConsentModel& value) {
  base::DictValue root;
  base::ListValue audit;
  for (const std::string& line : value.credential_audit) {
    audit.Append(line);
  }
  root.Set("credentialAudit", std::move(audit));
  return root;
}

// static
bool PhiStoreTraits<PhiConsentModel>::Decode(const base::DictValue& dict,
                                             int /*schema*/,
                                             PhiConsentModel* out) {
  // "agentAlways" (per-image agent grants) and "credentialGrants" (standing
  // credential approvals) are ignored: neither kind of standing grant
  // exists any more — agent admission is the allowAgentControl setting.
  if (const base::ListValue* audit = dict.FindList("credentialAudit")) {
    DecodeAudit(*audit, &out->credential_audit);
  }
  return true;
}

// static
bool PhiStoreTraits<PhiConsentModel>::Import(const std::wstring& dir,
                                             PhiConsentModel* out) {
  bool imported = false;
  base::DictValue dict;
  if (ReadLegacy(dir, kLegacyCredentialGrants, &dict) ==
      PhiJsonFileStatus::kOk) {
    if (const base::ListValue* audit = dict.FindList("audit")) {
      DecodeAudit(*audit, &out->credential_audit);
    }
    MarkImported(dir, kLegacyCredentialGrants);
    imported = true;
  }
  return imported;
}

// Compile every member of PhiStore for the four files here, so a codec or
// seal bug surfaces in this TU's build rather than in whichever TU first
// touches that store.
template class PhiStore<PhiSettings>;
template class PhiStore<PhiSpacesModel>;
template class PhiStore<PhiRestoreModel>;
template class PhiStore<PhiConsentModel>;

}  // namespace phi_client
