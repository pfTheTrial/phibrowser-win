// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

// What the New Tab shortcut opens — the Mac's NewTabShortcutBehavior.
enum class PhiNewTabBehavior { kNewTabPage, kOmnibox };

// Everything the settings window reads and writes. Implemented by the
// coordinator, which owns the persisted values and pushes changes to the slots.
class PhiSettingsHost {
 public:
  virtual ~PhiSettingsHost() = default;

  // Theme
  virtual std::string ThemeId() const = 0;
  virtual PhiAppearance Appearance() const = 0;
  virtual int Saturation() const = 0;  // 0..100, the Mac's slider range.
  virtual void SetThemeId(const std::string& id) = 0;
  virtual void SetAppearance(PhiAppearance appearance) = 0;
  virtual void SetSaturation(int value) = 0;
  virtual const PhiPalette& Palette() const = 0;

  // Account (the Auth0 session; stage 1 of bucket D's auth work).
  // False when signed out; fills the display identity otherwise.
  virtual bool AccountInfo(std::string* name, std::string* email) const = 0;
  virtual bool AccountSigningIn() const = 0;
  virtual void RequestSignIn() = 0;
  virtual void RequestSignOut() = 0;
  // Registers Phi with Windows and opens the system Default apps page. The
  // choice itself is the user's to make there — Windows 10 and later allow
  // no other way.
  virtual void RequestDefaultBrowser() = 0;

  // Appearance
  virtual PhiLayoutMode LayoutMode() const = 0;
  virtual void RequestLayoutMode(PhiLayoutMode mode) = 0;

  // Browsing
  virtual PhiNewTabBehavior NewTabBehavior() const = 0;
  virtual void SetNewTabBehavior(PhiNewTabBehavior behavior) = 0;
  virtual bool AlwaysShowFullUrl() const = 0;
  virtual void SetAlwaysShowFullUrl(bool enabled) = 0;
  // GeneralSettings.showTabPreviews — the hover cards on tab, pin and
  // bookmark rows. On by default, as on Mac.
  virtual bool ShowTabPreviews() const = 0;
  virtual void SetShowTabPreviews(bool enabled) = 0;
  // The UI language. An EMPTY tag means "follow the operating system", which
  // is the default and the only value that tracks a later OS change.
  virtual std::string LanguagePreference() const = 0;
  virtual void SetLanguagePreference(const std::string& tag) = 0;
  // "Restore last session": app-wide choice, pushed into every loaded
  // profile's Chromium startup pref (SessionStartupPref LAST).
  virtual bool RestoreLastSession() const = 0;
  virtual void SetRestoreLastSession(bool enabled) = 0;

  // Spaces / Profiles
  virtual PhiPinScope PinScope() const = 0;
  virtual void SetPinScope(PhiPinScope scope) = 0;
  // "name\ticonText\tprofileId\tspaceId\tthemeId\tsaturation" per Space.
  virtual std::vector<std::string> SpaceSummaries() const = 0;
  // A Space's pinned theme id, or empty when it follows the app-wide one.
  virtual std::string SpaceThemeId(const std::string& space_id) const = 0;
  virtual void SetSpaceTheme(const std::string& space_id,
                             const std::string& theme_id) = 0;
  // A Space's own icon and saturation override (the Mac's per-Space settings;
  // saturation -1 follows the app-wide slider).
  virtual void SetSpaceIcon(const std::string& space_id,
                            const std::string& icon) = 0;
  virtual void SetSpaceSaturation(const std::string& space_id, int value) = 0;
  // Space / profile management, with the Mac's guards (see PhiSlotHost).
  virtual bool RenameSpace(const std::string& space_id,
                           const std::string& name) = 0;
  virtual bool DeleteSpace(const std::string& space_id) = 0;
  // The Space wearing the default badge, and whether Remove is offered for
  // a Space (refused for the last one) — see the coordinator's twins.
  virtual std::string DefaultSpaceId() const = 0;
  virtual bool CanDeleteSpace(const std::string& space_id) const = 0;
  virtual bool DeleteProfileById(const std::string& profile_id) = 0;
  virtual bool RenameProfileById(const std::string& profile_id,
                                 const std::string& name) = 0;
  virtual std::vector<std::string> ProfileSummaries() const = 0;
  virtual void CreateProfileNamed(const std::string& name) = 0;
  // Per-profile browsing settings (the Mac profile pane's two controls).
  // Engines come back as "keyword\tname\tdefault(0|1)" rows; the list is
  // empty — and the row disabled — while the profile is not loaded.
  virtual std::vector<std::string> SearchEngines(
      const std::string& profile_id) const = 0;
  virtual bool SetSearchEngine(const std::string& profile_id,
                               const std::string& keyword) = 0;
  virtual std::string DownloadDirectory(
      const std::string& profile_id) const = 0;
  virtual bool SetDownloadDirectory(const std::string& profile_id,
                                    const std::string& dir) = 0;
  // Creates a Space (Default profile) — the list's "+" button.
  virtual void CreateSpaceNamed(const std::string& name) = 0;

  // Space URL routing rules (the Mac's universal URL Rules editor).
  // Rows come back as "spaceId\thost\tpathPrefix\task(0|1)"; `host` is the
  // wire form ("github.com" exact, "*.foo.com" suffix, "*needle*"
  // contains). Mutations persist and re-push the routing table at once.
  virtual std::vector<std::string> URLRuleRows() const = 0;
  virtual void AddURLRule(const std::string& space_id,
                          const std::string& host,
                          const std::string& path_prefix,
                          bool ask) = 0;
  virtual void SetURLRulePattern(int index,
                                 const std::string& host,
                                 const std::string& path_prefix) = 0;
  virtual void SetURLRuleTarget(int index,
                                const std::string& space_id,
                                bool ask) = 0;
  virtual void RemoveURLRule(int index) = 0;

  // Rebindable keyboard shortcuts (the Mac Shortcuts pane's customising).
  // Rows come back as "group\tlabel\tkeys\tname\tcustomized(0|1)" in display
  // order; `name` is empty for fixed, informational rows. Rebinding takes the
  // chord's raw parts; a chord already held by another rebindable shortcut is
  // STOLEN (that one goes unbound), as the Mac pane does.
  virtual std::vector<std::string> ShortcutRows() const = 0;
  virtual void RebindShortcut(const std::string& name,
                              UINT vk,
                              bool ctrl,
                              bool shift,
                              bool alt) = 0;
  virtual void RestoreAllShortcuts() = 0;
  virtual bool AnyShortcutCustomized() const = 0;

  // The persisted "Always Allow" agent grants (C1 consent), as displayable
  // rows (the UTF-8 image path). Removing one revokes it for future
  // connections; live sessions keep their allow-once.
  virtual std::vector<std::string> AgentGrantRows() const = 0;
  virtual void RemoveAgentGrant(int index) = 0;
  // Standing credential approvals ("scope\tfill|reveal") and the approval
  // audit trail ("stamp\tkind\tscope\tdecision", newest first). Revoking a
  // grant only affects future requests.
  virtual std::vector<std::string> CredentialGrantRows() const = 0;
  virtual void RemoveCredentialGrant(int index) = 0;
  virtual std::vector<std::string> CredentialAuditRows() const = 0;

  // Developer mode gates the Developer pane, as on Mac (default off).
  virtual bool DeveloperMode() const = 0;
  virtual void SetDeveloperMode(bool enabled) = 0;
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
class PhiSettingsWindow {
 public:
  static void Open(PhiSettingsHost* host);
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

  ~PhiSettingsWindow();

  static LRESULT CALLBACK WndProc(HWND hwnd,
                                  UINT msg,
                                  WPARAM wparam,
                                  LPARAM lparam);

 private:
  explicit PhiSettingsWindow(PhiSettingsHost* host);

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
  };
  struct Item {
    ItemKind kind = ItemKind::kRowLabel;
    RECT bounds{};
    std::wstring text;
    std::wstring detail;  // Second line / trailing text, kind-dependent.
    bool selected = false;
    bool on = false;
    bool enabled = true;  // Drawn dim and inert when false.
    COLORREF swatch = 0;
    COLORREF ring = 0;   // A swatch's selection-ring color (its own accent).
    int value = 0;       // Slider position, or an option index.
    std::string action;  // What a click does; empty = not clickable.
    std::string payload;
  };

  void Rebuild();
  void BuildAccount(int* y);
  void BuildGeneral(int* y);
  void BuildSpaces(int* y);
  // The URL Rules editor, a drill-in of the Spaces pane (the Mac's
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
  // The 1px separator between two rows of the open card.
  void AddDividerRow(int* y);
  // An 11pt tertiary line tucked under the row just added.
  void AddSubHint(int* y, const wchar_t* text);
  // Label + trailing switch, one call — most Mac rows are exactly this.
  void AddToggleRow(int* y, const wchar_t* title, bool on, bool enabled,
                    const std::string& action);
  // A row inside the open card; returns the rect its control occupies,
  // RIGHT-ALIGNED as on Mac (label left, Spacer, accessory trailing).
  RECT AddRow(int* y, const wchar_t* title, int control_height,
              int control_width);
  void AddChips(const RECT& area,
                const std::vector<std::wstring>& labels,
                int selected_index,
                const std::string& action);

  void Paint(HDC dc);
  void OnClick(int x, int y);
  void Apply(const Item& item, int x);
  int Scaled(int design_units) const;
  void EnsureFont();
  // The window title follows the selected pane, as the Mac library does.
  void SyncTitle();

  RAW_PTR_EXCLUSION PhiSettingsHost* host_;
  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;
  HFONT title_font_ = nullptr;
  HFONT small_font_ = nullptr;  // Section titles and toolbar labels.
  HFONT hint_font_ = nullptr;   // 11pt hints and captions.
  HFONT emoji_font_ = nullptr;  // The icon-picker grid's emoji.
  HFONT mdl2_font_ = nullptr;   // The grid's Phi-icon MDL2 stand-ins.
  HFONT caption_font_ = nullptr;  // MDL2 at 10pt — the close glyph.
  // The custom caption strip (the OS caption is retired via WM_NCCALCSIZE
  // so the title can sit CENTERED, with the Phi mark leading): hover and
  // press state for its close button.
  bool close_hot_ = false;
  int pressed_caption_ = 0;
  // The icon picker's segment ("icon" | "emoji", Icon default as on Mac) and
  // page (the Mac scrolls; this pages through the same content).
  std::string picker_tab_ = "icon";
  int picker_page_ = 0;
  UINT font_dpi_ = 0;
  UINT dpi_ = 96;
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
  // Whether this exe is the system's http handler. Resolved on demand — the
  // Account pane rebuilds on every paint — and dropped when the window is
  // activated, which is when the user comes back from the Windows Settings
  // page that the "Set as default" button opens. The Mac's twin re-checks in
  // viewWillAppear for the same reason.
  std::optional<bool> is_default_browser_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_
