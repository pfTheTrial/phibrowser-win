// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_

#include <windows.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_popup_chrome.h"
#include "phibrowser-win/client/phi_secondary_window.h"
#include "phibrowser-win/client/phi_settings_types.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

class PhiAccountAndFeedback;
class PhiCredentialService;
class PhiPreferences;
class PhiProfileService;
class PhiSpaceManager;
class PhiUrlRouter;

// What the New Tab shortcut opens — the Mac's NewTabShortcutBehavior. The
// windows-free model header carries the enum (phi_app_model.h); this is the
// UI side's spelling of the SAME type, not a second one to cast between.
using PhiNewTabBehavior = model::NewTabBehavior;

// What changed behind the host, for a window showing it. The view
// subscribes (PhiSettingsHost::AddSettingsObserver) and repaints on a
// notification; the model side no longer reaches it through the static
// PhiSettingsWindow::Refresh() (lane 1 D9).
enum class PhiSettingsChange {
  kPreferences,  // Any PhiPreferences domain (theme, prefs, language...).
  kSpaces,       // The Space list, a Space's theme, icon or profile.
  kProfiles,     // A profile was created, renamed, deleted or imported.
  kAccount,      // The Auth0 session settled or was signed out.
  kStoredRows,   // Pinned tabs / bookmarks changed.
  kConsent,      // The credential audit trail grew.
};
class PhiSettingsObserver {
 public:
  virtual void OnSettingsChanged(PhiSettingsChange what) = 0;

 protected:
  virtual ~PhiSettingsObserver() = default;
};

// What the settings window asks the app to DO, plus the doors to the
// services that own the values it shows.
//
// It used to be 75 virtuals, most of them one-line forwards from the
// coordinator to a service, several of them handing rows across as
// TAB-SEPARATED STRINGS that both sides split apart again by position
// (docs/architecture-review.md §3.5; lane 1 §1.3 and §5 D7). Phase 6 splits
// it in two:
//
//   READS go to the service that owns the domain. PhiPreferences,
//   PhiSpaceManager, PhiProfileService, PhiCredentialService and
//   PhiAccountAndFeedback are handed to the window CONST — it can see the
//   values, it cannot write behind the coordinator's back — and every row
//   they answer is a struct (phi_settings_types.h).
//
//   ACTIONS stay here, because each one needs something the service cannot
//   reach on its own: a framework push, a window, another store, a Chromium
//   profile. The exception is PhiUrlRouter, handed over MUTABLE: it is the
//   sole owner of the rules half of PhiSpaces.json and persists and
//   re-pushes the routing table itself, so the coordinator added nothing but
//   an index-shaped seam between the pane and it.
class PhiSettingsHost {
 public:
  virtual ~PhiSettingsHost() = default;

  // The window's subscription to changes behind the host (above). The
  // window adds itself when it opens and removes itself when it goes.
  virtual void AddSettingsObserver(PhiSettingsObserver* observer) = 0;
  virtual void RemoveSettingsObserver(PhiSettingsObserver* observer) = 0;

  // --- The services the window reads directly -------------------------------
  // All outlive the window (the coordinator owns them for the app's life).
  virtual const PhiPreferences& Preferences() const = 0;
  virtual const PhiSpaceManager& SpaceList() const = 0;
  virtual const PhiProfileService& ProfileList() const = 0;
  virtual const PhiCredentialService& Credentials() const = 0;
  virtual const PhiAccountAndFeedback& Account() const = 0;
  // Mutable: see the class comment — the URL Rules editor IS this service's
  // editor, and rules are addressed by wire id, never by row position.
  virtual PhiUrlRouter& UrlRules() = 0;

  // --- Theme ----------------------------------------------------------------
  virtual void SetThemeId(const std::string& id) = 0;
  virtual void SetAppearance(PhiAppearance appearance) = 0;
  virtual void SetSaturation(int value) = 0;  // 0..100, the Mac's range.

  // --- Account (the Auth0 session; stage 1 of bucket D's auth work) ---------
  virtual void RequestSignIn() = 0;
  virtual void RequestSignOut() = 0;
  // Registers Phi with Windows and opens the system Default apps page. The
  // choice itself is the user's to make there — Windows 10 and later allow
  // no other way.
  virtual void RequestDefaultBrowser() = 0;

  // --- Appearance -----------------------------------------------------------
  virtual void RequestLayoutMode(PhiLayoutMode mode) = 0;

  // --- Browsing -------------------------------------------------------------
  virtual void SetNewTabBehavior(PhiNewTabBehavior behavior) = 0;
  virtual void SetAlwaysShowFullUrl(bool enabled) = 0;
  // GeneralSettings.showTabPreviews — the hover cards on tab, pin and
  // bookmark rows. On by default, as on Mac.
  virtual void SetShowTabPreviews(bool enabled) = 0;
  // The UI language. An EMPTY tag means "follow the operating system", which
  // is the default and the only value that tracks a later OS change.
  virtual void SetLanguagePreference(const std::string& tag) = 0;
  // "Restore last session": app-wide choice, pushed into every loaded
  // profile's Chromium startup pref (SessionStartupPref LAST).
  virtual void SetRestoreLastSession(bool enabled) = 0;
  // "Share anonymous usage analytics": the telemetry consent. Off by
  // default; only this pane can turn it on (never the control channel).
  virtual void SetAnalyticsEnabled(bool enabled) = 0;

  // --- Spaces / Profiles ----------------------------------------------------
  // The pin scope's truth is the TAB STORE, not the preferences (the
  // settings file only mirrors it), and setting it re-owns every stored
  // row — so it stays an action here rather than a service read.
  virtual PhiPinScope PinScope() const = 0;
  virtual void SetPinScope(PhiPinScope scope) = 0;
  // A Space's own theme, icon and saturation override (the Mac's per-Space
  // settings; saturation -1 follows the app-wide slider).
  virtual void SetSpaceTheme(const std::string& space_id,
                             const std::string& theme_id) = 0;
  virtual void SetSpaceIcon(const std::string& space_id,
                            const std::string& icon) = 0;
  virtual void SetSpaceSaturation(const std::string& space_id, int value) = 0;
  // Space / profile management, with the Mac's guards (see PhiSlotHost).
  virtual bool RenameSpace(const std::string& space_id,
                           const std::string& name) = 0;
  virtual bool DeleteSpace(const std::string& space_id) = 0;
  virtual bool DeleteProfileById(const std::string& profile_id) = 0;
  virtual bool RenameProfileById(const std::string& profile_id,
                                 const std::string& name) = 0;
  virtual void CreateProfileNamed(const std::string& name) = 0;
  // Per-profile browsing settings (the Mac profile pane's two controls).
  // The engine LIST is read off PhiProfileService; setting one goes through
  // the framework's keyword service, so it comes back here.
  virtual bool SetSearchEngine(const std::string& profile_id,
                               const std::string& keyword) = 0;
  virtual bool SetDownloadDirectory(const std::string& profile_id,
                                    const std::string& dir) = 0;
  // Creates a Space (Default profile) — the list's "+" button.
  virtual void CreateSpaceNamed(const std::string& name) = 0;

  // --- Navigation -----------------------------------------------------------
  // Peek View, the Mac's peekViewEnabled / autoPeekViewEnabled pair. The
  // master gate governs the "Open Link in Peek View" context-menu item and,
  // under it, the automatic diversion; the sub-gate governs that diversion
  // alone — a cross-site link leaving a bookmark- or pinned-bound tab
  // previews in a Peek instead of taking a new tab. Both are pushed into the
  // framework, which is where both decisions are actually made.
  virtual void SetPeekViewEnabled(bool enabled) = 0;
  virtual void SetAutoPeekViewEnabled(bool enabled) = 0;

  // --- Shortcuts (the Mac Shortcuts pane's customising) ---------------------
  // Rebinding takes the chord's raw parts; a chord already held by another
  // rebindable shortcut is STOLEN (that one goes unbound), as the Mac pane
  // does. The keyboard hook reads the table live, so this is a push, not a
  // plain preference write.
  virtual void RebindShortcut(const std::string& name,
                              UINT vk,
                              bool ctrl,
                              bool shift,
                              bool alt) = 0;
  virtual void RestoreAllShortcuts() = 0;

  // --- Consent --------------------------------------------------------------
  // The persisted "Always Allow" agent grants (C1 consent) as displayable
  // UTF-8 image paths. This half of the consent file belongs to the control
  // plane's ladder, not to PhiCredentialService, so it is read and revoked
  // here. Revoking is by PATH — the identity — never by row position: the
  // pane re-reads the list between paint and click (lane 1 §5 D7). Future
  // connections are affected; live sessions keep their allow-once.
  virtual std::vector<std::string> AgentGrantPaths() const = 0;
  virtual bool RemoveAgentGrant(const std::string& path) = 0;
  // Revokes the standing credential approval held under `scope`, and with
  // it any live timed grant beneath it. Also by identity, for the same
  // reason.
  virtual bool RemoveCredentialGrant(const std::string& scope) = 0;

  // --- Developer ------------------------------------------------------------
  // Developer mode gates the Developer pane, as on Mac (default off).
  virtual void SetDeveloperMode(bool enabled) = 0;
  // Developer ▸ "Allow agents to operate your Spaces": lets a connected
  // agent read and change the user's OWN Spaces, tabs, bookmarks, URL rules
  // and profiles. Default off; agent Spaces are never affected, and the
  // agent cannot flip it over the control channel.
  virtual void SetAllowAgentUserSpaces(bool enabled) = 0;
  // Developer ▸ "Profiles agents can use": which profiles an agent may
  // bind a NEW agent Space to. Turning one off stops the next
  // agentSpace.create against it; Spaces already open are untouched.
  virtual void SetAgentProfileAllowed(const std::string& profile_id,
                                      bool allowed) = 0;
  // "Additional browser settings" — opens chrome://settings in a tab.
  virtual void OpenBrowserSettings() = 0;
  // Opens any browser page in a tab — the Mac profile pane's data links
  // (chrome://settings/privacy and friends).
  virtual void OpenBrowserPage(const std::string& url) = 0;
};

// The settings window, shaped like the Mac's Preferences: a TOOLBAR of panes
// across the top (SettingsWindowController is created with style:
// .toolbarItems — not a sidebar), and below it sections, each a small title
// over a rounded CARD holding its rows (GeneralSectionView wrapping a
// GeneralContainerView; label left, control trailing). A real
// top-level window, NOT an overlay inside a shell: the content region belongs
// to Chromium's DirectComposition surface, which composites above anything GDI
// draws there (windows-framework-design.md — the falsified overlay approach).
class PhiSettingsWindow : public PhiSecondaryWindow,
                          public PhiSettingsObserver {
 public:
  static void Open(PhiSettingsHost* host);
  // Repaints the open window. The coordinator no longer calls this (it
  // notifies the observer instead); the verb ladder still does for the
  // profile settings it writes through the framework.
  static void Refresh();
  // The open window's HWND, or null. Reported over the control socket: a caller
  // in another session cannot enumerate our windows to find it.
  static HWND Handle();
  static std::string CurrentSection();
  static bool ShowSection(const std::string& name);
  // Lands on Spaces with `space_id` selected — the detail column IS the icon
  // picker, so this is the "Change Icon…" destination. Needs an open window
  // (callers Open() first).
  static void ShowSpacePicker(const std::string& space_id);

  ~PhiSettingsWindow() override;

  // PhiSettingsObserver: whatever changed behind the host, the pane
  // rebuilds NOW and repaints — never from inside WM_PAINT (lane 3 §3.1
  // and ranked finding 1; docs/sidebar-architecture.md §2.7).
  void OnSettingsChanged(PhiSettingsChange what) override;

 private:
  explicit PhiSettingsWindow(PhiSettingsHost* host);
  bool OpenWindow();

  // PhiSecondaryWindow. Paint() only paints: the item list it draws was
  // built by the last ScheduleRebuild().
  void Paint(HDC dc, const RECT& dirty) override;
  LRESULT OnWindowMessage(UINT msg,
                          WPARAM wparam,
                          LPARAM lparam,
                          bool* handled) override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnTimerFired(UINT_PTR id) override;
  void OnNcDestroy() override;

  // One drawn thing. Rows are built per section and hit-tested from the same
  // list, so what is drawn and what is clickable cannot drift apart.
  enum class ItemKind {
    kToolbarItem,    // A pane, in the toolbar across the top.
    kCard,           // The well a section's rows sit in.
    kSectionTitle,   // Small secondary label ABOVE a card.
    kSubtitle,       // 11pt tertiary line under a section title.
    kRowLabel,
    kHint,
    kLink,           // 12pt accent text ("Edit in … settings…").
    kDivider,        // The 1px separator between card rows.
    kSwatch,         // A theme dot: 22pt circle, 2pt ring when selected.
    kImageCard,      // 122x72 picture card (layout/appearance/new-tab).
    kToggle,
    kSliderTrack,
    kPopup,          // Trailing pill that opens a popup menu on click.
    kListRow,        // Master-list row (Spaces / Profiles).
    kToolButton,     // 34x34 glyph button in a list's bottom strip.
    kEmojiCell,      // One 26pt cell of the icon picker grid.
    kBadge,          // Small capsule ("Default", "Not linked", …).
    kButton,
    kIcon,           // A lone MDL2 glyph (row gutters, drag handles).
    kKeycap,         // Shortcuts' bordered key pill (min width 80, r6).
    kSearchField,    // Shortcuts' in-card filter field.
    kIconChip,       // The Mac's 24pt SettingsIconChip leading a row.
  };
  struct Item {
    ItemKind kind = ItemKind::kRowLabel;
    RECT bounds{};
    std::wstring text;
    std::wstring detail;  // Second line / trailing text, kind-dependent.
    std::wstring icon;    // A list row's leading Space icon (glyph text).
    bool selected = false;
    bool on = false;
    bool enabled = true;  // Drawn dim and inert when false.
    // Multi-line text, measured at build time and drawn DT_WORDBREAK. The
    // Mac wraps every label and description
    // (`fixedSize(horizontal: false, vertical: true)`); a single line with
    // DT_END_ELLIPSIS chopped the longer ones mid-sentence.
    bool wrap = false;
    COLORREF swatch = 0;
    COLORREF ring = 0;   // A swatch's selection-ring color (its own accent).
    int value = 0;       // Slider position, or an option index.
    std::string action;  // What a click does; empty = not clickable.
    // The action's ONE target id (a theme, a Space, a profile, a rule, a
    // shortcut name). `space` carries the Space a Space-SCOPED action (a
    // theme dot, an icon cell) applies to, so no item packs two ids into
    // one tab-separated payload the click handler has to split again.
    std::string payload;
    std::string space;
  };

  void Rebuild();
  // THE way the pane changes: rebuild the item list from the host, then
  // ask for a repaint. Everything that used to InvalidateRect(TRUE) and
  // let WM_PAINT rebuild calls this instead.
  void ScheduleRebuild();
  void BuildAccount(int* y);
  void BuildGeneral(int* y);
  void BuildSpaces(int* y);
  // The Mac's Navigations pane: Kiosk, Peek and URL Rules.
  void BuildNavigation(int* y);
  // The URL Rules editor, a drill-in of the Navigation pane (the Mac's
  // universal editor window).
  void BuildUrlRules(int* y);
  void BuildProfiles(int* y);
  void BuildAi(int* y);
  void BuildShortcuts(int* y);
  void BuildDeveloper(int* y);
  // The Mac's ProfileDetailSettingsView: the Search Engine / Download
  // Location card, then "Your Data and Settings" links. Shared by the
  // Profiles detail column and the General pane's single-profile case.
  void BuildProfileDetail(int* y, int left, int right,
                          const std::string& profile_id);
  // A section = a small title, then a card holding its rows. BeginSection
  // remembers where the card starts; EndSection sizes it around the rows that
  // were added, the way the Mac's GeneralSectionView wraps a
  // GeneralContainerView.
  void BeginSection(int* y, const wchar_t* title);
  void EndSection(int* y);
  void AddHint(int* y, const wchar_t* text);
  // A section title with the Mac's 11pt description line under it, for a
  // card BeginSection then opens title-less.
  void AddSectionHeading(int* y, const wchar_t* title,
                         const wchar_t* description);
  // The 1px separator between two rows of the open card.
  void AddDividerRow(int* y);
  // An 11pt tertiary line tucked under the row just added.
  void AddSubHint(int* y, const wchar_t* text);
  // Label + trailing switch, one call — most Mac rows are exactly this.
  // `hint` is the row's own 11pt description, part of the SAME row.
  void AddToggleRow(int* y, const wchar_t* title, bool on, bool enabled,
                    const std::string& action,
                    const wchar_t* hint = nullptr,
                    int text_indent = 0,
                    wchar_t chip_glyph = 0,
                    COLORREF chip_color = 0,
                    int row_padding = -1);
  // A row inside the open card; returns the rect its control occupies,
  // RIGHT-ALIGNED as on Mac (label left, Spacer, accessory trailing).
  //
  // `hint`, when given, is the Mac's 11pt description line 4pt under the
  // title and INSIDE the row (GeneralSettingView stacks them in one
  // VStack): the trailing control centres on the pair, and the divider a
  // caller adds next lands after the description rather than between a
  // title and the sentence explaining it.
  // `text_indent` moves the text block right of the card's padding — the
  // Mac's `.padding(.leading, 36)` on the rows under a chip-led header, so
  // they line up with that header's text rather than with its icon.
  // `chip_glyph` / `chip_color` draw the Mac's SettingsIconChip in the 24pt
  // gutter that indent leaves, top-aligned with the title.
  // `row_padding` is the Mac's `.padding(.vertical, …)`: the card default
  // for a header row, 6 for the tight rows grouped under one. -1 takes the
  // default (the metric itself lives with the other metrics, in the .cc).
  RECT AddRow(int* y, const wchar_t* title, int control_height,
              int control_width, const wchar_t* hint = nullptr,
              int text_indent = 0, wchar_t chip_glyph = 0,
              COLORREF chip_color = 0, int row_padding = -1);
  // Height of `text` wrapped into `width` with `font` — DT_CALCRECT
  // through the same DrawTextW the paint pass ends in, so what is measured
  // and what is drawn cannot disagree.
  int WrappedHeight(HFONT font, const wchar_t* text, int width) const;
  void AddChips(const RECT& area,
                const std::vector<std::wstring>& labels,
                int selected_index,
                const std::string& action);

  // Hover, press and the slider drag. Every control here was inert under
  // the pointer (no hover wash, no hand cursor) and FIRED on the press —
  // and again on every mouse-move while the button stayed down, so a drag
  // across the pane flipped every switch it crossed. The base's hit list
  // (phi_window.h: "hits_ is THE rect list") owns all of it now, which is
  // also what makes the window answer UI Automation.
  void OnMouseDown(int hit, POINT client_pt) override;
  void OnClick(int hit, POINT client_pt) override;
  void OnWheel(int delta, POINT client_pt) override;
  void OnEscape() override;
  // items_ index for a hit index, or -1. The hit's `key` IS that index.
  int ItemOfHit(int hit) const;
  // Publishes every actionable item into hits_ — the one rect list hover,
  // clicks, tooltips and UIA all read.
  void PublishHits();
  void Apply(const Item& item, int x);
  void EnsureFont();
  // A lone MDL2 glyph at a size the ladder does not carry (the Account
  // avatar). Cached by size and dropped with the rest on a DPI move; it
  // used to be a CreateFontIndirectW / DeleteObject pair per item PER
  // PAINT (lane 3 ranked finding 12).
  HFONT IconFont(int size);
  // The window title follows the selected pane, as the Mac library does.
  void SyncTitle();

  RAW_PTR_EXCLUSION PhiSettingsHost* host_;
  // The services the panes read straight out of (see PhiSettingsHost).
  // Taken once at construction: the coordinator owns them for the app's
  // life, so they outlive every window.
  RAW_PTR_EXCLUSION const PhiPreferences* prefs_;
  RAW_PTR_EXCLUSION const PhiSpaceManager* spaces_;
  RAW_PTR_EXCLUSION const PhiProfileService* profiles_;
  RAW_PTR_EXCLUSION const PhiCredentialService* credentials_;
  RAW_PTR_EXCLUSION const PhiAccountAndFeedback* account_;
  RAW_PTR_EXCLUSION PhiUrlRouter* rules_;
  HFONT font_ = nullptr;
  HFONT title_font_ = nullptr;
  HFONT small_font_ = nullptr;  // Section titles and toolbar labels.
  HFONT hint_font_ = nullptr;   // 11pt hints and captions.
  HFONT emoji_font_ = nullptr;  // The icon-picker grid's emoji.
  HFONT mdl2_font_ = nullptr;   // The grid's Phi-icon MDL2 stand-ins.
  // The caption strip is PhiPopupChrome's now, through
  // PhiSecondaryWindow: the OS caption is retired so the title can sit
  // CENTERED with the Phi mark leading, and the close button's hover,
  // press and red-when-hot live in the frame. Lane 3 §2 counted four
  // copies of that code; this was the third.
  std::vector<std::pair<int, HFONT>> icon_fonts_;
  // The icon picker's segment ("icon" | "emoji", Icon default as on Mac)
  // and its scroll position: the first visible cell row, clamped by the
  // Spaces build to `picker_max_line_`. `picker_grid_` is the grid's
  // viewport (client px) so the wheel over it scrolls the picker, not the
  // pane; empty on every other pane.
  std::string picker_tab_ = "icon";
  int picker_scroll_ = 0;
  int picker_max_line_ = 0;
  RECT picker_grid_{};
  UINT font_dpi_ = 0;
  std::string section_ = "general";
  std::vector<Item> items_;
  // Index of the card being filled, so EndSection can size it.
  size_t open_card_ = 0;
  // Pane content can outgrow the fixed window; the wheel scrolls it. Reset on
  // pane switch, clamped to the built content's height.
  int scroll_ = 0;
  int content_height_ = 0;
  // Master-list selections (the Mac remembers per pane).
  std::string selected_space_;
  std::string selected_profile_;
  // The Shortcuts pane's filter text ("Find shortcut keys"), fed by WM_CHAR
  // while that pane is up.
  std::wstring shortcut_search_;
  // Whether this exe is the system's http handler — a registry walk
  // (AssocQueryStringW). Resolved on demand, CACHED for the life of the
  // answer, and dropped only when the window is activated, which is when
  // the user comes back from the Windows Settings page that the "Set as
  // default" button opens. The Mac's twin re-checks in viewWillAppear for
  // the same reason. Before phase 5 this probe sat behind a Rebuild()
  // that ran inside WM_PAINT (lane 3 §3.1).
  std::optional<bool> is_default_browser_;
  // The item the press landed on, and the capture a slider drag holds so
  // the knob keeps following the pointer outside the window.
  int pressed_ = -1;
  bool dragging_ = false;
  // The slider the press landed on, replayed by every move of the drag —
  // re-hit-testing each move is what let a drag act on its neighbours.
  Item drag_item_;
  Capture drag_;
  // Rows added to the card BeginSection opened, so a lone hint knows
  // whether it tucks under a row or is the card's own first line.
  int card_rows_ = 0;
  // Screen-compatible DC kept for DT_CALCRECT measuring; owned.
  mutable HDC measure_dc_ = nullptr;
  // A profile whose data is still loading re-arms one short poll per
  // rebuild; the id comes from the window's own pool, not the hand-picked
  // 0x517 (lane 3 §4.1).
  Timer profile_poll_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_
