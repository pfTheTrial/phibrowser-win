// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_PREFERENCES_H_
#define PHIBROWSER_WIN_CLIENT_PHI_PREFERENCES_H_

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "phibrowser-win/client/phi_app_model.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_settings_types.h"
#include "phibrowser-win/client/phi_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_shell_window.h"

namespace phi_client {

// The client's preferences service (docs/architecture-review.md §3.4, the
// PhiPreferences row): layout, theme, the general prefs, shortcuts, restore,
// peek, language and the developer gates — one model::PhiSettings, one
// PhiSettings.json, and the palette derived from the theme.
//
// It is the ChromePrefsHost of §3.5: what a shell reads for its layout,
// palette and bookmark-bar flags is answered here directly.
//
// RULES. Every setter is idempotent (an unchanged value is a no-op), marks
// the store dirty through the callback taken at construction (the
// coordinator's ScheduleSave, so coalescing stays in one place) and then
// tells the observers WHICH domain moved. The service pushes nothing
// itself: the windows, the framework mirrors (restore, peek, language) and
// telemetry are the coordinator's observer, and the settings window's
// repaint rides the same notification — the 36 static
// PhiSettingsWindow::Refresh() calls the setters used to make are gone.
class PhiPreferences : public ChromePrefsHost {
 public:
  // Which preference domain a notification is about. One value per
  // fan-out the coordinator used to hand-write after each setter.
  enum class PreferenceChange {
    kAccess,         // Guest choice / onboarding phase (the gate re-reads).
    kLayoutMode,     // Every slot re-lays out (ApplyLayoutMode).
    kSidebarWidth,   // Every slot re-fits its content.
    kTheme,          // Theme id or appearance: the palette was re-resolved.
    kSaturation,     // The slider: the palette was re-resolved.
    kNewTab,
    kFullUrl,        // The address fields repaint.
    kPreviews,
    kOpenIndicators,  // The sidebar's open-tab dots: every sidebar repaints.
    kDimUnloaded,     // The reclaimed-tab dimming: every sidebar repaints.
    kPeek,           // Both peek switches: the framework mirror is due.
    kRestore,        // The per-profile framework mirror is due.
    kBookmarkBar,    // Either bar flag: every slot re-fits its content.
    kLanguage,       // The preference: Chromium's locale mirror is due.
    kLocaleStrings,  // The ACTIVE catalog changed: rebuild every string.
    kShortcuts,      // The override table (the hook reads chords live).
    kDeveloper,
    kAgentControl,    // The master agent switch (Developer ▸ Agent control).
    kAgentAutoView,   // View ▸ Agent Autoview: the coordinator re-evaluates.
    kAgentUserSpaces,
    kAnalytics,      // The telemetry consent.
    kPinScope,       // The write-through mirror of the tab store's scope.
  };
  class Observer {
   public:
    virtual void OnPreferencesChanged(PreferenceChange what) = 0;

   protected:
    virtual ~Observer() = default;
  };

  // A mutation writes nothing itself: it calls `dirty`, and the owner folds
  // the write into its coalesced-save timer (then calls Save()).
  using DirtyCallback = std::function<void()>;
  explicit PhiPreferences(DirtyCallback dirty);
  ~PhiPreferences() override;

  PhiPreferences(const PhiPreferences&) = delete;
  PhiPreferences& operator=(const PhiPreferences&) = delete;

  void AddObserver(Observer* observer);
  void RemoveObserver(Observer* observer);

  // --- The store ----------------------------------------------------------
  // Binds the store to `user_data_dir` and loads it (a missing file imports
  // the pre-phase-3 files, a corrupt one is quarantined and defaults stand),
  // then applies what the loaded values drive here: the client catalog's
  // language and the palette. No observer hears a load.
  void Load(const std::wstring& user_data_dir);
  // Writes the model. The owner's flush is the only caller.
  void Save();
  // The whole model, read-only. Every field the coordinator or the verb
  // ladder reads comes from here; every write goes through a setter.
  const model::PhiSettings& settings() const { return settings_; }

  // --- ChromePrefsHost ------------------------------------------------------
  PhiLayoutMode LayoutMode() const override;
  void RequestLayoutMode(PhiLayoutMode mode) override;
  bool ShowTabPreviews() const override;
  bool ShowOpenTabIndicators() const override;
  bool DimUnloadedTabIcons() const override;
  bool NewTabOpensOmnibox() const override;
  int SidebarWidth() const override;
  void RequestSidebarWidth(int dip) override;
  const PhiPalette& Palette() const override;
  bool AlwaysShowBookmarkBar() const override;
  void SetAlwaysShowBookmarkBar(bool enabled) override;
  bool BookmarkBarOnNewTab() const override;
  void SetBookmarkBarOnNewTab(bool enabled) override;
  std::string ThemeId() const override;
  PhiAppearance Appearance() const override;
  int Saturation() const override;

  // --- Theme ---------------------------------------------------------------
  // An unknown id normalises to the default theme (PhiThemeById).
  void SetThemeId(const std::string& id);
  void SetAppearance(PhiAppearance appearance);
  void SetSaturation(int value);  // Clamped to 0..100.
  // The app-wide palette, resolved from the theme id, the appearance and
  // the saturation — derived, never persisted. Its address is stable for
  // the life of the service (the popup chrome keeps a pointer to it).
  const PhiPalette& palette() const { return palette_; }

  // --- General -------------------------------------------------------------
  model::NewTabBehavior new_tab_behavior() const {
    return settings_.new_tab_behavior;
  }
  void SetNewTabBehavior(model::NewTabBehavior behavior);
  bool AlwaysShowFullUrl() const { return settings_.always_show_full_url; }
  void SetAlwaysShowFullUrl(bool enabled);
  void SetShowTabPreviews(bool enabled);
  void SetShowOpenTabIndicators(bool enabled);
  void SetDimUnloadedTabIcons(bool enabled);
  bool PeekViewEnabled() const { return settings_.peek_view_enabled; }
  void SetPeekViewEnabled(bool enabled);
  bool AutoPeekViewEnabled() const { return settings_.auto_peek_view_enabled; }
  void SetAutoPeekViewEnabled(bool enabled);
  bool RestoreLastSession() const { return settings_.restore_last_session; }
  void SetRestoreLastSession(bool enabled);
  bool DeveloperMode() const { return settings_.developer_mode; }
  void SetDeveloperMode(bool enabled);
  // --- Browser access (the onboarding gate, docs/login-port.md §8) ------
  bool GuestModeEnabled() const { return settings_.guest_mode_enabled; }
  void SetGuestModeEnabled(bool enabled);
  // The account's onboarding phase (0 = unknown / brand new).
  int OnboardingPhase(const std::string& sub) const;
  void SetOnboardingPhase(const std::string& sub, int phase);
  bool SkipOnboarding() const { return settings_.skip_onboarding; }
  bool AllowAgentControl() const override;
  void SetAllowAgentControl(bool enabled);
  bool AgentAutoView() const override;
  void SetAgentAutoView(bool enabled);
  void RequestAgentAutoView(bool enabled) override;
  bool AllowAgentUserSpaces() const {
    return settings_.allow_agent_user_spaces;
  }
  void SetAllowAgentUserSpaces(bool enabled);
  // May an agent bind a new agent Space to `profile_id`? Stored as the
  // refusals, so a profile made later starts allowed (the Mac's set).
  bool AgentProfileAllowed(const std::string& profile_id) const;
  void SetAgentProfileAllowed(const std::string& profile_id, bool allowed);
  bool AnalyticsEnabled() const { return settings_.analytics_enabled; }
  void SetAnalyticsEnabled(bool enabled);

  // --- Language ------------------------------------------------------------
  // The stored preference (the catalog's normalised form); empty follows
  // the operating system.
  std::string LanguagePreference() const;
  // Applies `tag` to the client catalog and stores the normalised
  // preference. NOT idempotent: the preference is saved and kLanguage sent
  // either way (the picker shows the preference, which may change without
  // a single string changing — Japanese on a Japanese machine);
  // kLocaleStrings follows only when the ACTIVE catalog moved.
  void SetLanguage(const std::string& tag);

  // --- Shortcuts (the Mac Shortcuts pane's customising) --------------------
  // The pane's rows in display order (PhiShortcutRow: group, label, the
  // EFFECTIVE chord, the stable rebind name and whether it is overridden).
  // A row with an empty name is fixed and informational.
  std::vector<PhiShortcutRow> ShortcutRows() const;
  // Rebinds the named rebindable shortcut to the chord's raw parts; a chord
  // already held by another rebindable shortcut is STOLEN (that one goes
  // unbound), as the Mac pane does. Rebinding back to the default is a
  // restore, not an override.
  void RebindShortcut(const std::string& name,
                      UINT vk,
                      bool ctrl,
                      bool shift,
                      bool alt);
  void RestoreAllShortcuts();
  bool AnyShortcutCustomized() const;
  // The chord a rebindable shortcut currently answers to: its override when
  // one exists, its default otherwise.
  PhiShortcutChord EffectiveChord(const std::string& name) const;
  // The command bound to this chord among the REBINDABLE shortcuts, or 0.
  // Checked by the keyboard hook before the fixed table, so a custom chord
  // wins.
  UINT MatchRebindableChord(UINT vk, bool ctrl, bool shift, bool alt) const;

  // --- Mirrors of state owned elsewhere ------------------------------------
  // The reader's live style is PhiReaderBridge's; the model carries a copy
  // so the store writes it. Marks dirty; no observer hears it.
  void SetReaderStyle(const model::ReaderStyle& style);
  // The pin scope's truth is the tab store (its rows re-own themselves on a
  // change); this is the write-through mirror so the two files never
  // disagree — called after every tab-store change and load. Marks dirty
  // and sends kPinScope only when the mirror actually moved.
  void SetPinScopeMirror(PhiPinScope scope);

 private:
  // Recomputes `palette_` from the theme id, appearance and saturation.
  void ResolvePalette();
  void MarkDirty();
  void Notify(PreferenceChange what);

  model::PhiSettings settings_;  // PhiSettings.json
  PhiStore<model::PhiSettings> store_;
  PhiPalette palette_;
  DirtyCallback dirty_;
  std::vector<Observer*> observers_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_PREFERENCES_H_
