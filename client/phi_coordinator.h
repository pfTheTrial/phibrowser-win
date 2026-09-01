// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_
#define PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_

#include <windows.h>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_agent_spaces.h"
#include "phibrowser-win/client/phi_control_server.h"
#include "phibrowser-win/client/phi_credentials.h"
#include "phibrowser-win/client/phi_kiosk_window.h"
#include "phibrowser-win/client/phi_peek_panel.h"
#include "phibrowser-win/client/phi_vault_helper.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_auth.h"
#include "phibrowser-win/client/phi_settings_window.h"
#include "phibrowser-win/client/phi_transcript_panel.h"
#include "phibrowser-win/client/phi_agent_drive_registry.h"
#include "phibrowser-win/client/phi_ghost_spaces.h"
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
                       public PhiSettingsHost,
                       public PhiTranscriptHost {
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
  void OnOpenLinkInSpace(int64_t source_window_id,
                         const char* space_id,
                         const char* url) override;
  void OnAskSpaceForURL(const char* url,
                        const char* default_space_id,
                        int64_t source_window_id,
                        bool source_is_new_tab) override;
  void OnRouteURLToSpace(const char* space_id,
                         const char* url,
                         int64_t source_window_id) override;
  void OnTargetURLChanged(int64_t window_id, const char* url) override;
  void OnOmniboxResults(int64_t window_id, const char* json) override;
  void OnImportCompleted(const char* source, bool success) override;
  void OnPlaceholderModeChanged(int64_t window_id, bool entered) override;
  void OnContentFullscreenChanged(int64_t window_id, bool fullscreen) override;
  void OnExtensionsChanged(int64_t window_id) override;
  void OnProfileCreated(const char* profile_id) override;
  void OnAgentDroveUserSpaceTab(int64_t tab_id,
                                int64_t window_id,
                                int64_t session_id) override;
  void OnAgentObservedUserSpaceTab(int64_t tab_id,
                                   int64_t window_id,
                                   int64_t session_id) override;
  void OnAgentStoppedUserSpaceTab(int64_t tab_id,
                                  int64_t session_id) override;
  void OnTabDistillable(int64_t window_id, int tab_id,
                        bool distillable) override;
  void OnSessionRestoreSettled(const char* receipt_json) override;
  void OnAnalyticsEventCaptured(const char* module,
                                const char* event_name,
                                const char* properties_json) override;
  void OnColdStartRestorePlan(std::vector<int>* eager_window_ids,
                              bool* armed) override;
  void OnColdStartParkedWindows(const char* parked_json,
                                const char* reporting_profile) override;

  // PhiControlDelegate (called on the control server's I/O thread):
  std::string HandleControlCommand(const std::string& type,
                                   const std::string& payload_json) override;
  bool AuthorizeAgentPeer(const PhiAgentPeer& peer) override;

  // PhiSlotHost (called on the UI thread by a slot's sidebar):
  std::vector<PhiSpaceView> Spaces() const override;
  void RequestSwitchSpace(int64_t slot, const std::string& space_id) override;
  std::vector<PhiAgentPipView> AgentSpaces() const override;
  void RequestWatchAgentSpace(const std::string& task_id) override;
  void RequestAgentTakeControl(const std::string& task_id) override;
  void RequestAgentHandBack(const std::string& task_id) override;
  void RequestAgentFinish(const std::string& task_id) override;
  void RequestShowTranscript(const std::string& task_id) override;

  // PhiTranscriptHost:
  std::vector<std::string> TranscriptLines(
      const std::string& task_id) override;
  std::string TranscriptAgentName(const std::string& task_id) override;
  bool PushTranscriptMessage(const std::string& task_id,
                             const std::string& text) override;
  void RequestCreateSpace(int64_t slot, bool new_profile) override;
  bool AccountInfo(std::string* name, std::string* email) const override;
  bool AccountSigningIn() const override;
  void RequestSignIn() override;
  void RequestSignOut() override;
  PhiLayoutMode LayoutMode() const override;
  void RequestLayoutMode(PhiLayoutMode mode) override;
  const PhiPalette& Palette() const override;
  const PhiPalette& PaletteForSlot(int64_t slot) const override;

  // Opens `url` as a foreground tab in the first shell window and raises it —
  // how a Kiosk window hands its page over when the user expands it.
  void OpenUrlInMainWindow(const std::string& url);
  void RequestSettings() override;
  void RequestSpaceIconPicker(const std::string& space_id) override;
  int64_t SlotAtScreenPoint(POINT screen, int64_t exclude_slot) const override;
  bool RequestDropTabOnWindow(int64_t source_slot,
                              int index,
                              int b_index,
                              int64_t target_slot) override;
  bool AlwaysShowBookmarkBar() const override;
  void SetAlwaysShowBookmarkBar(bool enabled) override;
  bool BookmarkBarOnNewTab() const override;
  void SetBookmarkBarOnNewTab(bool enabled) override;
  std::vector<PhiStoredRowView> PinnedRows(int64_t slot) const override;
  std::vector<PhiStoredRowView> BookmarkRows(
      int64_t slot,
      const std::string& parent_guid) const override;
  void RequestOpen(int64_t slot, const std::string& url) override;
  void RequestBookmarkActiveTab(int64_t slot) override;
  void RequestStoreSplit(int64_t slot, int index_a, int index_b,
                         bool as_bookmark) override;
  void RequestDropTab(int64_t slot, int index, bool as_bookmark,
                      int insert_index) override;
  void RequestDropSplit(int64_t slot, int index_a, int index_b,
                        bool as_bookmark, int insert_index) override;
  void RequestStoreTab(int64_t slot, int index,
                       bool as_bookmark) override;
  void MoveStoredRow(const std::string& guid,
                     const std::string& new_parent_guid,
                     int new_index) override;
  std::string AddBookmarkFolder(int64_t slot,
                                const std::string& parent_guid,
                                const std::string& title) override;
  void RemoveStoredRow(const std::string& guid) override;
  bool SetStoredRowIcon(const std::string& guid,
                        const std::string& icon) override;
  bool SetStoredRowUrl(const std::string& guid,
                       const std::string& url) override;
  bool SetStoredRowLayout(const std::string& guid,
                          const std::string& layout) override;
  // The divider axis of the live split holding a tab, for stored rows.
  std::string LiveSplitLayout(int64_t browser, int index);
  bool RenameStoredRow(const std::string& guid,
                       const std::string& name) override;
  void ReorderStoredRow(const std::string& guid, int new_index) override;
  void ReorderSpace(const std::string& space_id, int new_index) override;
  int SidebarWidth() const override;
  void RequestSidebarWidth(int dip) override;

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
  bool ShowTabPreviews() const override;
  void NoteSpaceWindow(const std::string& space_id,
                       int64_t browser_window_id) override;
  void NoteTabDistillable(int64_t window_id, int tab_id,
                          bool distillable) override;
  bool IsTabAgentDriven(int tab_id) const override;
  bool IsTabReclaimed(int tab_id) const override;
  void RequestReclaimTab(int tab_id) override;
  void RequestReleaseTab(int tab_id) override;
  void SetShowTabPreviews(bool enabled) override;
  bool RestoreLastSession() const override;
  void SetRestoreLastSession(bool enabled) override;
  void FocusWindowForBrowser(int64_t browser_window_id) override;
  bool NewTabOpensOmnibox() const override;
  std::string AddBookmarkRow(int64_t slot,
                             const std::string& parent_guid,
                             const std::string& title,
                             const std::string& url) override;
  void RequestTabToSpace(int64_t slot,
                         int index,
                         const std::string& space_id,
                         bool move) override;
  // Pushes the persisted restore choice into every loaded profile — run
  // when the framework comes up and after a profile loads.
  void PushRestorePreference();
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
  std::vector<std::string> ShortcutRows() const override;
  void RebindShortcut(const std::string& name,
                      UINT vk,
                      bool ctrl,
                      bool shift,
                      bool alt) override;
  void RestoreAllShortcuts() override;
  bool AnyShortcutCustomized() const override;
  std::vector<std::string> AgentGrantRows() const override;
  void RemoveAgentGrant(int index) override;
  std::vector<std::string> CredentialGrantRows() const override;
  void RemoveCredentialGrant(int index) override;
  std::vector<std::string> CredentialAuditRows() const override;
  bool DeveloperMode() const override;
  void SetDeveloperMode(bool enabled) override;
  void OpenBrowserSettings() override;
  void OpenBrowserPage(const std::string& url) override;
  void SetSpaceTheme(const std::string& space_id,
                     const std::string& theme_id) override;
  std::vector<std::string> ProfileSummaries() const override;
  void CreateProfileNamed(const std::string& name) override;
  std::vector<std::string> SearchEngines(
      const std::string& profile_id) const override;
  bool SetSearchEngine(const std::string& profile_id,
                       const std::string& keyword) override;
  std::string DownloadDirectory(const std::string& profile_id) const override;
  bool SetDownloadDirectory(const std::string& profile_id,
                            const std::string& dir) override;
  void CreateSpaceNamed(const std::string& name) override;
  std::vector<std::string> URLRuleRows() const override;
  void AddURLRule(const std::string& space_id,
                  const std::string& host,
                  const std::string& path_prefix,
                  bool ask) override;
  void SetURLRulePattern(int index,
                         const std::string& host,
                         const std::string& path_prefix) override;
  void SetURLRuleTarget(int index,
                        const std::string& space_id,
                        bool ask) override;
  void RemoveURLRule(int index) override;

 private:
  // One phi_analytics event as it crossed the framework boundary. The client
  // has no analytics pipeline yet, so the browser's events are kept here —
  // bounded, newest last — and read back over the control socket. Without
  // this the component's sink stayed unattached on Windows and every event
  // it produced (the launch-state snapshots, renderer health) fell out of its
  // 64-entry pre-attach queue and was discarded.
  struct AnalyticsEvent {
    std::string module;
    std::string name;
    std::string properties_json;
  };
  static constexpr size_t kAnalyticsRingSize = 256;
  std::deque<AnalyticsEvent> analytics_events_;

  // Agent credential approval (C2) — the Mac's CredentialGrantStore model.
  // `kind` is the EXPOSURE the user is consenting to: kFill keeps the secret
  // inside Phi (vault -> framework -> page DOM, never to the agent); kReveal
  // hands it to the agent outright. A REVEAL grant covers a fill request; a
  // fill grant NEVER covers a reveal (the Mac's CredentialAccessKind.covers)
  // — they are different capabilities, not degrees of one.
  enum class CredentialAccessKind { kFill, kReveal };

  // Runs a control command on the UI thread; returns the JSON response. Called
  // via SendMessage to msg_hwnd_ from HandleControlCommand.
  std::string ExecuteControlOnUi(const std::string& type,
                                 const std::string& payload_json);
  // Maps a slot id (what callers pass as windowId) to the browser currently
  // shown in it — the Space's browser, not necessarily the slot's original.
  int64_t ResolveBrowser(int64_t slot_or_browser) const;
  // Swaps in the url-favicon service's larger icon when it beats the row's
  // stored 16px capture.
  void UpgradeFavicon(int64_t slot,
                      const std::string& url,
                      PhiStoredRowView* view) const;
  // The slot's controller; slot 0 falls back to the first window (the verb
  // convention for "the window"). Null when none exists.
  PhiWindowController* ControllerForSlot(int64_t slot);
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
  // Alt is down with nothing pressed since — a bare tap summons the caption
  // band's hidden menu titles on release. Cleared by any other key-down, so
  // Alt+Tab and the Alt+arrow history chords never trip it.
  bool alt_solo_ = false;
  static LRESULT CALLBACK KeyboardHookProc(int code,
                                           WPARAM wparam,
                                           LPARAM lparam);
  // The shell window owning the focus HWND, or null (focus is elsewhere).
  HWND ShellForFocus() const;
  // Agent-peer consent (C1): the UI-thread half of AuthorizeAgentPeer —
  // grant store first, then the session's remembered choices, then the
  // consent dialog. Grants persist in PhiAgentGrants.json by IMAGE PATH
  // (lowercased for comparison).
  bool AuthorizeAgentPeerOnUi(const PhiAgentPeer& peer);
  void LoadAgentGrants();
  void SaveAgentGrants() const;
  // The chord a rebindable shortcut currently answers to: its override when
  // one exists, its default otherwise.
  PhiShortcutChord EffectiveChord(const std::string& name) const;
  // The command bound to this chord among the REBINDABLE shortcuts, or 0.
  // Checked by the hook before the fixed table, so a custom chord wins.
  UINT MatchRebindableChord(UINT vk, bool ctrl, bool shift, bool alt) const;

  // One controller per SLOT, keyed by the slot's primary windowId.
  std::map<int64_t, std::unique_ptr<PhiWindowController>> windows_;
  // Kiosk windows, keyed like windows_ but held apart: they are not shell
  // slots, take no Space, and never appear in the slot/space bookkeeping.
  std::map<int64_t, std::unique_ptr<PhiKioskWindow>> kiosks_;
  // Peek panels, keyed by the peek's own window id. Like kiosks these take no
  // slot; unlike them they are bound to an opener tab and float inside that
  // window's content card.
  std::map<int64_t, std::unique_ptr<PhiPeekPanel>> peeks_;
  int sidebar_width_ = kSidebarWidthDefault;
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
    // The Mac wire's per-Space accent ("#RRGGBB"). Cosmetic data the Windows
    // chrome does not paint yet — stored and echoed so the skill's
    // create/update/list round-trip agrees across clients.
    std::string color_hex;
  };
  std::vector<Space> spaces_{{"1", "Space 1", "Default", kPhiDefaultSpaceIcon}};
  int next_space_id_ = 2;
  // Where the Space list is persisted (user data dir).
  std::wstring spaces_path_;
  // A Space URL routing rule, the Mac's SpaceURLRule. `host` is stored in
  // the wire form ("github.com" exact, "*.foo.com" suffix, "*needle*"
  // contains); `path_prefix` empty means any path; `ask` prompts instead of
  // routing silently. Order in the vector is the sortOrder tiebreak.
  struct UrlRule {
    std::string space_id;
    std::string host;
    std::string path_prefix;
    bool ask = false;
  };
  std::vector<UrlRule> url_rules_;
  // Bumped on every rule write; the wire rule ids ("g<gen>-<index>") carry
  // it, so ids from before a write stop resolving — the Mac contract that
  // callers re-list before update/delete.
  int url_rules_generation_ = 1;
  std::wstring url_rules_path_;
  void LoadUrlRules();
  void SaveUrlRules() const;
  // Compiles url_rules_ plus the Space -> VISIBLE browser map into the
  // framework's PhiURLRouter table. Called (via PushSpaceMenu) on every
  // rules edit, browser attach/close and Space switch.
  void PushRoutingTable() const;
  // Surfaces `space_id` (cold-spawning its browser if needed) and opens
  // `url` there with a one-shot routing bypass — the shared tail of the
  // context menu's "Open Link in Space", the silent auto-route into a
  // hidden/cold Space, and the "ask first" prompt's pick.
  void RouteUrlToSpace(const std::string& space_id,
                       const std::string& url,
                       int64_t source_window_id);
  // The shared store-tab/split bodies behind the menu (append, bookmark
  // unbound) and drag (placed + bound) entry points.
  void StoreTabInternal(int64_t slot, int index, bool as_bookmark,
                        int insert_index, bool bind);
  void StoreSplitInternal(int64_t slot, int index_a, int index_b,
                          bool as_bookmark, int insert_index,
                          bool bind_bookmark);

  void LoadSpaces();
  void SaveSpaces() const;
  // Publishes the Space list (with each Space's open browser id) into the
  // framework for the page context menu's "Open Link in Space" submenu.
  void PushSpaceMenu() const;
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
  bool show_tab_previews_ = true;  // The Mac's defaultValue.
  // Which of the user's own tabs an agent currently holds (the operating
  // mask). Fed by the three delegate reports above.
  PhiAgentDriveRegistry agent_drives_;
  // The last lazy-restore receipt, verbatim. Phase 2 keeps it only so the
  // harness can read it back; the park/materialize state machine (phase 3)
  // will REPLACE its records from this, never merge.
  std::string last_restore_receipt_;
  // Which Spaces exist only as a parked window, and the window each Space
  // had last run (the key the ghost store names them by).
  PhiGhostSpaces ghost_spaces_;
  // Which Space the cold start should rebuild eagerly: the one that was
  // active when the last run ended.
  std::string LastActiveSpaceId() const;
  // Message-window timer ids: 1 profile-space finish, 2 agent-space
  // keep-alive, this one the operating mask's idle/takeover sweep.
  static constexpr UINT_PTR kAgentDriveSweepTimer = 3;
  // "Restore last session" — persisted app-wide, mirrored into each loaded
  // profile's SessionStartupPref (LAST/DEFAULT).
  bool restore_last_session_ = false;
  // Persistent "Always Allow" agent grants (lowercased image paths) and the
  // session-scoped consent memory (allow-once / deny-for-now per path).
  std::set<std::wstring> agent_grants_;
  std::set<std::wstring> session_allowed_peers_;
  std::set<std::wstring> session_denied_peers_;
  std::wstring agent_grants_path_;
  // Credential approvals. Only NEVER-EXPIRING grants persist; the timed ones
  // are session-scoped by design (the Mac's in-memory 10-minute cache), so
  // a restart is always a clean slate for them.
  std::map<std::string, CredentialAccessKind> credential_grants_;
  struct TimedCredentialGrant {
    CredentialAccessKind kind = CredentialAccessKind::kFill;
    ULONGLONG expires_tick = 0;
  };
  std::map<std::string, TimedCredentialGrant> credential_timed_grants_;
  std::vector<std::string> credential_audit_;
  std::wstring credential_grants_path_;
  // profiles.create is ASYNC in Chromium (the basename exists only once the
  // profile is initialised), but the wire call is synchronous. The UI thread
  // arms this event before kicking creation and OnProfileCreated signals it;
  // the I/O thread waits on it AFTER its SendMessage returns, so the UI
  // thread stays free to actually finish the work. The SetEvent/Wait pair is
  // the happens-before for `created_profile_id_`.
  HANDLE profile_create_done_ = nullptr;
  std::string created_profile_id_;
  volatile bool profile_create_pending_ = false;
  // The bookmark bar's two visibility switches (the Mac View menu's pair).
  bool always_show_bookmark_bar_ = false;
  bool bookmark_bar_on_new_tab_ = false;
  // Shortcut overrides, keyed by the shortcut's STABLE NAME (never the
  // command int, which is free to be renumbered). vk 0 = explicitly unbound.
  std::map<std::string, PhiShortcutChord> shortcut_overrides_;
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
  // "Open Link in Space" into a cold Space: opened when its browser attaches.
  std::string pending_open_url_;
  // A "+ Space & profile" request waiting for its profile to appear (profile
  // creation is asynchronous). Polled from a WM_TIMER on msg_hwnd_.
  int64_t pending_profile_slot_ = 0;
  std::set<std::string> known_profiles_before_;
  int pending_profile_ticks_ = 0;
  // Returns true when finished (created the Space, or gave up).
  bool TryFinishProfileSpace();

  // --- Agent Spaces (the phi-browser skill's hidden windows) ---------------
  PhiAgentSpaces agent_spaces_;
  // The agent Space whose window is currently surfaced for the user to watch
  // (empty = none). At most one is watched at a time.
  std::string watched_agent_task_;
  // Repaints every USER window's sidebar so agent pips appear/disappear and
  // reflect the watched state.
  void RefreshUserWindows();
  // Set for the duration of an agentSpace.create so the re-entrant
  // OnBrowserWindowCreated adopts the new browser as a hidden agent window
  // (rather than a normal visible slot) and records its id here.
  std::string pending_agent_task_;
  int64_t last_agent_window_id_ = 0;
  // Handles one agentSpace.* control verb; returns true when `type` was an
  // agent-space verb (its result is in `out`), false to fall through to the
  // client-UI verbs. `now_ms` is GetTickCount64().
  bool HandleAgentSpaceVerb(const std::string& type,
                            const base::DictValue* payload,
                            uint64_t now_ms,
                            base::DictValue* out);
  // Handles one verb of the Mac skill's dialect — tabGroups.*, splitView.*,
  // downloads.*, pinnedTabs.*, the bookmark-tree forms, the spaces.listTabs/
  // openTab/focus/activate/update family and resolveTabIds; true when `type`
  // was one (result in `out`). Verbs whose names collide with the client-UI
  // dialect (bookmarks.list/add) claim the call only for skill-shaped
  // payloads (no windowId) and return false otherwise.
  bool HandleSkillDialectVerb(const std::string& type,
                              const base::DictValue* payload,
                              base::DictValue* out);
  // Resolves the skill's layout scope — {taskId}: the agent window,
  // {spaceId}: a user Space's open window — to the browser bound to it.
  // 0 with `error` set (unknown_task / space_not_open / invalid_payload)
  // when the scope names nothing live.
  int64_t ResolveScopeBrowser(const base::DictValue* payload,
                              std::string* error);
  // The browser hosting `space_id`'s tabs (possibly hidden behind a sibling
  // Space), or 0 when the Space has no open window.
  int64_t BrowserForSpace(const std::string& space_id) const;
  // The USER window the user is looking at: the foreground shell when it is
  // one of ours, the first user window otherwise. Null without windows.
  PhiWindowController* FocusedUserController();
  // Surfaces `space_id` in `slot` (slot 0: the first window), cold-spawning
  // its browser in the Space's profile when the slot has none — the
  // spaces.switch body, shared with the skill's spaces.activate. `cold`
  // reports the cold-create path; `pending` that the attach is still armed.
  bool SwitchSlotToSpace(int64_t slot, const std::string& space_id,
                         bool* pending, bool* cold);
  // The (profile, space) the skill's pinned/bookmark dialect verbs target:
  // an explicit profile id wins, else the focused window's active Space.
  bool DialectOwner(const std::string& profile_id_or_empty,
                    std::string* profile_id, std::string* space_id);
  // Tears down an agent Space: closes its hidden window and drops the record.
  void CloseAgentSpace(const std::string& task_id);
  // Keep-alive sweep (WM_TIMER id 2): completes and closes expired spaces.
  void SweepAgentSpaces();

  // --- Credentials (the skill's agent password surface) --------------------
  // The REAL provider: the out-of-process Bitwarden helper (the user's own
  // vault, same engine as macOS). `credentials_` is a local DPAPI dev/test
  // store used ONLY when no helper binary is installed.
  PhiVaultHelper vault_helper_;
  PhiCredentialVault credentials_;
  // The Auth0 session (bucket D auth, stage 1).
  PhiAuth auth_;
  std::wstring credentials_dir_;
  // Looks a query up through the helper. Returns the reply's `result` dict, or
  // nullopt with `error` set (`ambiguous` carries a candidates list).
  std::optional<base::DictValue> HelperLookup(const std::string& domain,
                                              const std::string& id,
                                              const std::string& search,
                                              const std::string& username,
                                              std::string* error);
  // The helper's vault status wire string, or empty when it is not running.
  std::string HelperStatus();
  // Resolves a query through whichever provider is active (helper first, dev
  // vault otherwise) to exactly ONE item. True fills `item` with the vault
  // item; false fills `out` with the wire error body (not_found / ambiguous +
  // non-secret candidates / not_ready), so both credential verbs share one
  // matching and one refusal policy.
  bool ResolveCredentialItem(const std::string& domain,
                             const std::string& id,
                             const std::string& search,
                             const std::string& username,
                             base::DictValue* item,
                             base::DictValue* out);
  // Handles one credentials.* verb; true when `type` was one (result in
  // `out`). The per-request approve/deny prompt and origin binding live here.
  bool HandleCredentialsVerb(const std::string& type,
                             const base::DictValue* payload,
                             base::DictValue* out);
  // The native approve/deny prompt for a credential request. `detail` names
  // the item and purpose. Returns true when the user approves.
  // Consults live grants first; prompts only when none covers the request.
  // `scope` identifies the credential (its site/name), not the agent: grants
  // are all-agents, as on Mac.
  bool PromptCredentialApproval(const std::wstring& detail,
                                CredentialAccessKind kind,
                                const std::string& scope);
  // Pushes agentSpace.ownershipChanged to connected drivers — the Mac's
  // broadcastToTaskDriver. Called after every ownership transition; a task
  // whose record is already gone is deliberately NOT announced (drivers
  // learn of an ended task from its absence in agentSpace.list).
  void PushOwnershipChanged(const std::string& task_id);
  // Mounts/updates/tears down the watch-mode overlay for the watched task.
  void RefreshAgentOverlay();
  bool CredentialGrantCovers(const std::string& scope,
                             CredentialAccessKind kind) const;
  void LoadCredentialGrants();
  void SaveCredentialGrants() const;
  // Appends one line to the credential audit trail (newest first, capped)
  // and persists it — every approval decision is recorded, allowed or not.
  void AppendCredentialAudit(const std::string& scope,
                             CredentialAccessKind kind,
                             const char* decision);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_COORDINATOR_H_
