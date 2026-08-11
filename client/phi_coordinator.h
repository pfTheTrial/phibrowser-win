// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_
#define PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_control_server.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_settings_window.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

// The client's implementation of the framework delegate — the Windows twin of
// the Mac PhiChromiumCoordinator. Registered with the framework before
// ChromeMain; the framework calls it back on the browser UI thread. F2 slice 1
// only observes window lifecycle and logs it; adopting the HWND and drawing
// chrome comes next.
class PhiCoordinator : public phi_framework::PhiFrameworkDelegate,
                       public PhiControlDelegate,
                       public PhiSlotHost,
                       public PhiSettingsHost {
 public:
  PhiCoordinator();
  ~PhiCoordinator() override;

  PhiCoordinator(const PhiCoordinator&) = delete;
  PhiCoordinator& operator=(const PhiCoordinator&) = delete;

  // phi_framework::PhiFrameworkDelegate:
  void OnFrameworkReady(phi_framework::PhiFramework* framework,
                        const char* user_data_dir_utf8) override;
  void OnBrowserWindowCreated(int64_t window_id, void* hwnd) override;
  void OnBrowserWindowClosing(int64_t window_id) override;
  void OnTabsChanged(int64_t window_id) override;

  // PhiControlDelegate (called on the control server's I/O thread):
  std::string HandleControlCommand(const std::string& type,
                                   const std::string& payload_json) override;

  // PhiSlotHost (called on the UI thread by a slot's sidebar):
  std::string AllSpacesJson() const override;
  void RequestSwitchSpace(int64_t slot, const std::string& space_id) override;
  void RequestCreateSpace(int64_t slot, bool new_profile) override;
  PhiLayoutMode LayoutMode() const override;
  void RequestLayoutMode(PhiLayoutMode mode) override;
  const PhiPalette& Palette() const override;
  const PhiPalette& PaletteForSlot(int64_t slot) const override;
  void RequestSettings() override;
  std::string PinnedJson(int64_t slot) const override;
  std::string BookmarksJson(int64_t slot,
                            const std::string& parent_guid) const override;
  void RequestOpen(int64_t slot, const std::string& url) override;
  void RequestBookmarkActiveTab(int64_t slot) override;
  void RequestStoreTab(int64_t slot, int index,
                       bool as_bookmark) override;
  void RemoveStoredRow(const std::string& guid) override;

  // PhiSettingsHost:
  std::string ThemeId() const override;
  PhiAppearance Appearance() const override;
  int Saturation() const override;
  void SetThemeId(const std::string& id) override;
  void SetAppearance(PhiAppearance appearance) override;
  void SetSaturation(int value) override;
  PhiNewTabBehavior NewTabBehavior() const override;
  void SetNewTabBehavior(PhiNewTabBehavior behavior) override;
  bool AlwaysShowFullUrl() const override;
  void SetAlwaysShowFullUrl(bool enabled) override;
  PhiPinScope PinScope() const override;
  void SetPinScope(PhiPinScope scope) override;
  std::vector<std::string> SpaceSummaries() const override;
  std::string SpaceThemeId(const std::string& space_id) const override;
  void SetSpaceIcon(const std::string& space_id,
                    const std::string& icon) override;
  void SetSpaceSaturation(const std::string& space_id, int value) override;
  bool RenameSpace(const std::string& space_id,
                   const std::string& name) override;
  bool DeleteSpace(const std::string& space_id) override;
  bool DeleteProfileById(const std::string& profile_id) override;
  bool RenameProfileById(const std::string& profile_id,
                         const std::string& name) override;
  bool DeveloperMode() const override;
  void SetDeveloperMode(bool enabled) override;
  void OpenBrowserSettings() override;
  void SetSpaceTheme(const std::string& space_id,
                     const std::string& theme_id) override;
  std::vector<std::string> ProfileSummaries() const override;
  void CreateProfileNamed(const std::string& name) override;
  void CreateSpaceNamed(const std::string& name) override;

 private:
  // Runs a control command on the UI thread; returns the JSON response. Called
  // via SendMessage to msg_hwnd_ from HandleControlCommand.
  std::string ExecuteControlOnUi(const std::string& type,
                                 const std::string& payload_json);
  // Maps a slot id (what callers pass as windowId) to the browser currently
  // shown in it — the Space's browser, not necessarily the slot's original.
  int64_t ResolveBrowser(int64_t slot_or_browser) const;
  static LRESULT CALLBACK MsgWndProc(HWND hwnd,
                                     UINT msg,
                                     WPARAM wparam,
                                     LPARAM lparam);

  // The framework command interface (a singleton in chrome.dll); non-owning,
  // across a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  // The unified control/CDP socket.
  PhiControlServer control_server_;
  // Message-only window on the UI thread; control commands marshal here.
  HWND msg_hwnd_ = nullptr;
  // Keyboard accelerators: a UI-thread WH_KEYBOARD hook translates the menu
  // bar's shortcuts (Ctrl+T, Ctrl+W, ...) into WM_COMMAND on the focused
  // shell — keys normally land in the browser child, never in our chrome.
  HHOOK keyboard_hook_ = nullptr;
  static LRESULT CALLBACK KeyboardHookProc(int code,
                                           WPARAM wparam,
                                           LPARAM lparam);
  // The shell window owning the focus HWND, or null (focus is elsewhere).
  HWND ShellForFocus() const;

  // One controller per SLOT, keyed by the slot's primary windowId.
  std::map<int64_t, std::unique_ptr<PhiWindowController>> windows_;
  // Every browser window -> the slot hosting it (a slot has one per Space).
  std::map<int64_t, int64_t> browser_to_slot_;
  // Spaces are a client concept. Each binds EXACTLY ONE profile; several
  // Spaces may share a profile (the fork's model — the binding is immutable
  // once created).
  struct Space {
    std::string id;
    std::string name;
    std::string profile_id;  // On-disk basename, e.g. "Default".
    // Icon in the Mac's storage form ("emoji:<id>" / "phi:<id>"), so a Space
    // means the same thing on both clients.
    std::string icon = kPhiDefaultSpaceIcon;
    // Per-Space THEME OVERRIDE, the Mac's spaceThemeIds / spaceThemeSaturations
    // maps. Empty id and -1 saturation mean "follow the app-wide choice".
    std::string theme_id;
    int saturation = -1;
  };
  std::vector<Space> spaces_{{"1", "Space 1", "Default", kPhiDefaultSpaceIcon}};
  int next_space_id_ = 2;
  // Where the Space list is persisted (user data dir).
  std::wstring spaces_path_;
  void LoadSpaces();
  void SaveSpaces() const;
  // Where the chrome lives — one setting for every slot, as on Mac. Persisted
  // beside the Space list so a restart comes back in the same layout.
  PhiLayoutMode layout_mode_ = PhiLayoutMode::kPerformance;
  std::wstring layout_path_;
  void LoadLayoutMode();
  void SaveLayoutMode() const;
  // Theme + appearance, the Mac's ThemeSettings. Persisted beside the layout.
  std::string theme_id_ = "pure";
  PhiAppearance appearance_ = PhiAppearance::kSystem;
  PhiPalette palette_;
  // One resolved palette per Space that overrides the app-wide theme; a Space
  // with no override is not in here and uses `palette_`.
  std::map<std::string, PhiPalette> palette_by_space_;
  void RebuildSpacePalettes();
  std::wstring theme_path_;
  void LoadTheme();
  void SaveTheme() const;
  // The rest of the settings surface (the Mac's General pane).
  int saturation_ = 50;
  PhiNewTabBehavior new_tab_behavior_ = PhiNewTabBehavior::kNewTabPage;
  bool always_show_full_url_ = false;
  // Gates the Developer settings pane, as on Mac (default off).
  bool developer_mode_ = false;
  std::wstring prefs_path_;
  void LoadPrefs();
  void SavePrefs() const;
  // Pinned tabs and bookmarks, the Mac's LocalStore equivalent.
  PhiTabStore tab_store_;
  // The (profile, space) a slot's pinned/bookmark rows belong to.
  bool OwnerForSlot(int64_t slot, std::string* profile_id,
                    std::string* space_id) const;
  // The active tab's favicon as base64 BGRA, for storing on a pinned/bookmark
  // row. A pinned tab has no live tab to ask later.
  std::string CaptureFavicon(int64_t slot, int* width, int* height);
  // The url the slot is showing right now.
  std::string LiveUrl(int64_t slot);
  // Recomputes `palette_` and repaints every slot.
  void ApplyTheme();
  // Set while a Space browser is being created, so the next
  // OnBrowserWindowCreated attaches to that slot instead of opening a shell.
  int64_t pending_slot_ = 0;
  std::string pending_space_;
  // A "+ Space & profile" request waiting for its profile to appear (profile
  // creation is asynchronous). Polled from a WM_TIMER on msg_hwnd_.
  int64_t pending_profile_slot_ = 0;
  std::set<std::string> known_profiles_before_;
  int pending_profile_ticks_ = 0;
  // Returns true when finished (created the Space, or gave up).
  bool TryFinishProfileSpace();
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_
