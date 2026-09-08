// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_preferences.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_menu_bar.h"

namespace phi_client {

namespace {

// The customisable shortcuts (the Mac Shortcuts pane's editable rows), in the
// pane's display order. `name` is the stable prefs key; the chord here is the
// default the menu bar advertises. Everything NOT in this table (Quit, zoom,
// devtools, Ctrl+Tab's MRU switcher, Ctrl+1..9 Spaces...) stays fixed.
struct RebindableShortcut {
  const char* name;
  const wchar_t* group;
  const wchar_t* label;
  UINT command;
  PhiShortcutChord def;
};
constexpr RebindableShortcut kRebindableShortcuts[] = {
      {"newTab", L"File", L"New Tab", kCmdNewTab, {'T', true, false, false}},
      {"newWindow", L"File", L"New Window", kCmdNewWindow,
       {'N', true, false, false}},
      {"newIncognitoWindow", L"File", L"New Incognito Window",
       kCmdNewIncognitoWindow, {'N', true, true, false}},
      {"reopenClosedTab", L"File", L"Reopen Closed Tab", kCmdReopenClosedTab,
       {'T', true, true, false}},
      {"openLocation", L"File", L"Open Location", kCmdOpenLocation,
       {'L', true, false, false}},
      {"closeTab", L"File", L"Close Tab", kCmdCloseTab,
       {'W', true, false, false}},
      {"closeWindow", L"File", L"Close Window", kCmdCloseWindow,
       {'W', true, true, false}},
      {"copyUrl", L"Edit", L"Copy URL", kCmdCopyUrl,
       {'C', true, true, false}},
      {"reload", L"View", L"Reload This Page", kCmdReload,
       {'R', true, false, false}},
      {"forceReload", L"View", L"Force Reload This Page", kCmdForceReload,
       {'R', true, true, false}},
      {"toggleSidebar", L"View", L"Toggle Sidebar", kCmdToggleSidebar,
       {'S', true, false, false}},
      // Ctrl+Alt+R, the Mac's Cmd-Opt-R: Safari's reader chord (Cmd-Shift-R)
      // is Chromium's hard reload on both platforms.
      {"toggleReader", L"View", L"Toggle Reader View", kCmdToggleReader,
       {'R', true, false, true}},
      {"toggleBookmarkBar", L"View", L"Always Show Bookmark Bar",
       kCmdAlwaysShowBookmarkBar, {'B', true, true, false}},
      {"showFullHistory", L"History", L"Show Full History",
       kCmdShowFullHistory, {'H', true, false, false}},
      {"bookmarkThisTab", L"Bookmarks", L"Bookmark This Tab",
       kCmdBookmarkThisTab, {'D', true, false, false}},
      {"bookmarkAllTabs", L"Bookmarks", L"Bookmark All Tabs",
       kCmdBookmarkAllTabs, {'D', true, true, false}},
      {"downloads", L"Window", L"Downloads", kCmdDownloads,
       {'J', true, false, false}},
      {"searchTabs", L"Window", L"Search Tabs", kCmdSearchTabs,
       {'A', true, true, false}},
      {"nextSpace", L"Spaces", L"Next Space", kCmdNextSpace,
       {VK_RIGHT, true, false, true}},
      {"previousSpace", L"Spaces", L"Previous Space", kCmdPreviousSpace,
       {VK_LEFT, true, false, true}},
};

}  // namespace

PhiPreferences::PhiPreferences(DirtyCallback dirty)
    : palette_(PhiResolvePalette(PhiThemeById(settings_.theme_id),
                                 static_cast<PhiAppearance>(
                                     settings_.appearance),
                                 settings_.saturation)),
      dirty_(std::move(dirty)) {}

PhiPreferences::~PhiPreferences() = default;

void PhiPreferences::AddObserver(Observer* observer) {
  observers_.push_back(observer);
}

void PhiPreferences::RemoveObserver(Observer* observer) {
  std::erase(observers_, observer);
}

void PhiPreferences::Load(const std::wstring& user_data_dir) {
  store_.Init(user_data_dir);
  // The load leaves the model usable: decoded, imported from the
  // pre-phase-3 files on a first run over an old profile, or defaults over
  // a quarantined file (the truth-owner rules are in phi_app_model.h).
  store_.Load(&settings_);
  // The language first and before any chrome is built, so the first window
  // comes up in the chosen language rather than flashing English; an empty
  // preference follows the system.
  PhiSetLanguage(settings_.language);
  // The palette depends on the theme AND the saturation, so it is resolved
  // once both are known.
  ResolvePalette();
}

void PhiPreferences::Save() {
  store_.Save(settings_);
}

// --- ChromePrefsHost --------------------------------------------------------

PhiLayoutMode PhiPreferences::LayoutMode() const {
  return settings_.layout_mode;
}

void PhiPreferences::RequestLayoutMode(PhiLayoutMode mode) {
  if (settings_.layout_mode == mode) {
    return;
  }
  settings_.layout_mode = mode;
  MarkDirty();
  Notify(PreferenceChange::kLayoutMode);
}

bool PhiPreferences::ShowTabPreviews() const {
  return settings_.show_tab_previews;
}

bool PhiPreferences::NewTabOpensOmnibox() const {
  return settings_.new_tab_behavior == model::NewTabBehavior::kOmnibox;
}

int PhiPreferences::SidebarWidth() const {
  return settings_.sidebar_width;
}

void PhiPreferences::RequestSidebarWidth(int dip) {
  const int clamped =
      std::max(kSidebarWidthMin, std::min(dip, kSidebarWidthMax));
  if (clamped == settings_.sidebar_width) {
    return;
  }
  settings_.sidebar_width = clamped;
  MarkDirty();
  Notify(PreferenceChange::kSidebarWidth);
}

const PhiPalette& PhiPreferences::Palette() const {
  return palette_;
}

bool PhiPreferences::AlwaysShowBookmarkBar() const {
  return settings_.always_show_bookmark_bar;
}

void PhiPreferences::SetAlwaysShowBookmarkBar(bool enabled) {
  if (enabled == settings_.always_show_bookmark_bar) {
    return;
  }
  settings_.always_show_bookmark_bar = enabled;
  MarkDirty();
  Notify(PreferenceChange::kBookmarkBar);
}

bool PhiPreferences::BookmarkBarOnNewTab() const {
  return settings_.bookmark_bar_on_new_tab;
}

void PhiPreferences::SetBookmarkBarOnNewTab(bool enabled) {
  if (enabled == settings_.bookmark_bar_on_new_tab) {
    return;
  }
  settings_.bookmark_bar_on_new_tab = enabled;
  MarkDirty();
  Notify(PreferenceChange::kBookmarkBar);
}

std::string PhiPreferences::ThemeId() const {
  return settings_.theme_id;
}

PhiAppearance PhiPreferences::Appearance() const {
  // Same enumerators in the same order (phi_app_model.h): the model's twin
  // exists only to keep that header off windows.h.
  return static_cast<PhiAppearance>(settings_.appearance);
}

int PhiPreferences::Saturation() const {
  return settings_.saturation;
}

// --- Theme ------------------------------------------------------------------

void PhiPreferences::SetThemeId(const std::string& id) {
  const std::string normalised = PhiThemeById(id).id;
  if (normalised == settings_.theme_id) {
    return;
  }
  settings_.theme_id = normalised;
  ResolvePalette();
  MarkDirty();
  Notify(PreferenceChange::kTheme);
}

void PhiPreferences::SetAppearance(PhiAppearance appearance) {
  if (appearance == Appearance()) {
    return;
  }
  settings_.appearance = static_cast<model::Appearance>(appearance);
  ResolvePalette();
  MarkDirty();
  Notify(PreferenceChange::kTheme);
}

void PhiPreferences::SetSaturation(int value) {
  value = value < 0 ? 0 : (value > 100 ? 100 : value);
  if (value == settings_.saturation) {
    return;
  }
  settings_.saturation = value;
  ResolvePalette();
  MarkDirty();
  Notify(PreferenceChange::kSaturation);
}

void PhiPreferences::ResolvePalette() {
  palette_ = PhiResolvePalette(PhiThemeById(settings_.theme_id), Appearance(),
                               settings_.saturation);
}

// --- General ----------------------------------------------------------------

void PhiPreferences::SetNewTabBehavior(model::NewTabBehavior behavior) {
  if (behavior == settings_.new_tab_behavior) {
    return;
  }
  settings_.new_tab_behavior = behavior;
  MarkDirty();
  Notify(PreferenceChange::kNewTab);
}

void PhiPreferences::SetAlwaysShowFullUrl(bool enabled) {
  if (enabled == settings_.always_show_full_url) {
    return;
  }
  settings_.always_show_full_url = enabled;
  MarkDirty();
  Notify(PreferenceChange::kFullUrl);
}

void PhiPreferences::SetShowTabPreviews(bool enabled) {
  if (enabled == settings_.show_tab_previews) {
    return;
  }
  settings_.show_tab_previews = enabled;
  MarkDirty();
  Notify(PreferenceChange::kPreviews);
}

void PhiPreferences::SetPeekViewEnabled(bool enabled) {
  if (enabled == settings_.peek_view_enabled) {
    return;
  }
  settings_.peek_view_enabled = enabled;
  MarkDirty();
  Notify(PreferenceChange::kPeek);
}

void PhiPreferences::SetAutoPeekViewEnabled(bool enabled) {
  if (enabled == settings_.auto_peek_view_enabled) {
    return;
  }
  settings_.auto_peek_view_enabled = enabled;
  MarkDirty();
  Notify(PreferenceChange::kPeek);
}

void PhiPreferences::SetRestoreLastSession(bool enabled) {
  if (enabled == settings_.restore_last_session) {
    return;
  }
  settings_.restore_last_session = enabled;
  MarkDirty();
  Notify(PreferenceChange::kRestore);
}

void PhiPreferences::SetDeveloperMode(bool enabled) {
  if (settings_.developer_mode == enabled) {
    return;
  }
  settings_.developer_mode = enabled;
  MarkDirty();
  Notify(PreferenceChange::kDeveloper);
}

void PhiPreferences::SetAllowAgentUserSpaces(bool enabled) {
  // Only the Settings window reaches here — never a verb (see
  // agentSpace.settings.set), so a connected agent cannot widen its own
  // reach.
  if (settings_.allow_agent_user_spaces == enabled) {
    return;
  }
  settings_.allow_agent_user_spaces = enabled;
  LOG(INFO) << "[PhiAgent] agent access to user Spaces "
            << (enabled ? "enabled" : "disabled");
  MarkDirty();
  Notify(PreferenceChange::kAgentUserSpaces);
}

void PhiPreferences::SetAnalyticsEnabled(bool enabled) {
  if (enabled == settings_.analytics_enabled) {
    return;
  }
  settings_.analytics_enabled = enabled;
  MarkDirty();
  Notify(PreferenceChange::kAnalytics);
}

// --- Language ---------------------------------------------------------------

std::string PhiPreferences::LanguagePreference() const {
  return PhiLanguagePreference();
}

void PhiPreferences::SetLanguage(const std::string& tag) {
  const bool active_changed = PhiSetLanguage(tag);
  // The catalog's normalised preference is what the file carries.
  settings_.language = PhiLanguagePreference();
  MarkDirty();
  Notify(PreferenceChange::kLanguage);
  // Every piece of chrome caches the strings it was built with, so the whole
  // UI is rebuilt — only when the ACTIVE locale moved: picking Japanese on a
  // Japanese machine changes the preference without changing a single
  // string.
  if (active_changed) {
    Notify(PreferenceChange::kLocaleStrings);
  }
}

// --- Shortcuts --------------------------------------------------------------

std::vector<std::string> PhiPreferences::ShortcutRows() const {
  std::vector<std::string> rows;
  const auto fixed = [&](const char* group, const char* label,
                         const char* keys) {
    rows.push_back(std::string(group) + "\t" + label + "\t" + keys + "\t\t0");
  };
  const auto rebindable = [&](const RebindableShortcut& def) {
    rows.push_back(base::WideToUTF8(def.group) + "\t" +
                   base::WideToUTF8(def.label) + "\t" +
                   base::WideToUTF8(PhiFormatChord(EffectiveChord(def.name))) +
                   "\t" + def.name + "\t" +
                   (settings_.shortcut_overrides.count(def.name) ? "1" : "0"));
  };
  // The pane's groups in Mac order; a group's rebindable rows first, then its
  // fixed, informational ones (chords the hook special-cases or that other
  // machinery advertises — shown but not editable).
  fixed("App", "Preferences", "Ctrl+,");
  fixed("App", "Quit Phi", "Ctrl+Shift+Q");
  const std::array<const wchar_t*, 8> kGroups = {
      L"File", L"Edit", L"View", L"History", L"Bookmarks",
      L"Window", L"Tab", L"Spaces"};
  for (const wchar_t* group : kGroups) {
    for (const RebindableShortcut& def : kRebindableShortcuts) {
      if (std::wstring(def.group) == group) {
        rebindable(def);
      }
    }
    if (std::wstring(group) == L"History") {
      fixed("History", "Back", "Alt+Left");
      fixed("History", "Forward", "Alt+Right");
    } else if (std::wstring(group) == L"Tab") {
      fixed("Tab", "Select Next Tab", "Ctrl+Tab");
      fixed("Tab", "Select Previous Tab", "Ctrl+Shift+Tab");
    }
  }
  return rows;
}

void PhiPreferences::RebindShortcut(const std::string& name,
                                    UINT vk,
                                    bool ctrl,
                                    bool shift,
                                    bool alt) {
  const RebindableShortcut* target = nullptr;
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    if (name == def.name) {
      target = &def;
      break;
    }
  }
  if (!target || !vk) {
    return;
  }
  const PhiShortcutChord chord{vk, ctrl, shift, alt};
  // Steal the chord from whichever rebindable currently answers to it — the
  // stolen shortcut goes UNBOUND (vk 0) until given a new chord or restored.
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    if (name != def.name && EffectiveChord(def.name) == chord) {
      settings_.shortcut_overrides[def.name] = {};
    }
  }
  // Rebinding back to the default is a restore, not an override.
  if (chord == target->def) {
    settings_.shortcut_overrides.erase(name);
  } else {
    settings_.shortcut_overrides[name] = {chord.vk, chord.ctrl, chord.shift,
                                          chord.alt};
  }
  MarkDirty();
  Notify(PreferenceChange::kShortcuts);
}

void PhiPreferences::RestoreAllShortcuts() {
  if (settings_.shortcut_overrides.empty()) {
    return;
  }
  settings_.shortcut_overrides.clear();
  MarkDirty();
  Notify(PreferenceChange::kShortcuts);
}

bool PhiPreferences::AnyShortcutCustomized() const {
  return !settings_.shortcut_overrides.empty();
}

PhiShortcutChord PhiPreferences::EffectiveChord(const std::string& name) const {
  auto it = settings_.shortcut_overrides.find(name);
  if (it != settings_.shortcut_overrides.end()) {
    const model::ShortcutChord& chord = it->second;
    return {chord.vk, chord.ctrl, chord.shift, chord.alt};
  }
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    if (name == def.name) {
      return def.def;
    }
  }
  return {};
}

UINT PhiPreferences::MatchRebindableChord(UINT vk,
                                          bool ctrl,
                                          bool shift,
                                          bool alt) const {
  const PhiShortcutChord pressed{vk, ctrl, shift, alt};
  for (const RebindableShortcut& def : kRebindableShortcuts) {
    const PhiShortcutChord chord = EffectiveChord(def.name);
    if (chord.vk && chord == pressed) {
      return def.command;
    }
  }
  return 0;
}

// --- Mirrors ----------------------------------------------------------------

void PhiPreferences::SetReaderStyle(const model::ReaderStyle& style) {
  settings_.reader = style;
  MarkDirty();
}

void PhiPreferences::SetPinScopeMirror(PhiPinScope scope) {
  if (settings_.pin_scope == scope) {
    return;
  }
  settings_.pin_scope = scope;
  MarkDirty();
  Notify(PreferenceChange::kPinScope);
}

void PhiPreferences::MarkDirty() {
  if (dirty_) {
    dirty_();
  }
}

void PhiPreferences::Notify(PreferenceChange what) {
  // Over a copy: an observer may unsubscribe (or open a window that
  // subscribes) from inside the callback.
  const std::vector<Observer*> observers = observers_;
  for (Observer* observer : observers) {
    observer->OnPreferencesChanged(what);
  }
}

}  // namespace phi_client
