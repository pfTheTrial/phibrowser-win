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
#include "base/synchronization/lock.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_agent_spaces.h"
#include "phibrowser-win/client/phi_control_server.h"
#include "phibrowser-win/client/phi_credentials.h"
#include "phibrowser-win/client/phi_kiosk_window.h"
#include "phibrowser-win/client/phi_peek_panel.h"
#include "phibrowser-win/client/phi_vault_helper.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_reader.h"
#include "phibrowser-win/client/phi_reader_panel.h"
#include "phibrowser-win/client/phi_surface_registry.h"
#include "phibrowser-win/client/phi_auth.h"
#include "phibrowser-win/client/phi_feedback_window.h"
#include "phibrowser-win/client/phi_settings_window.h"
#include "phibrowser-win/client/phi_transcript_panel.h"
#include "phibrowser-win/client/phi_agent_drive_registry.h"
#include "phibrowser-win/client/phi_ghost_spaces.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"
#include "phibrowser-win/client/phi_window_model.h"
#include "phibrowser-win/client/phi_window_registry.h"

namespace phi_client {

// One request marshalled from the control server's I/O thread to the UI
// thread (a control verb or a peer-consent decision). Defined in the .cc;
// the coordinator only queues pointers to it.
struct PhiUiCall;

// The client's implementation of the framework delegate — the Windows twin of
// the Mac PhiChromiumCoordinator. Registered with the framework before
// ChromeMain; the framework calls it back on the browser UI thread. F2 slice 1
// only observes window lifecycle and logs it; adopting the HWND and drawing
// chrome comes next.
class PhiCoordinator : public phi_framework::PhiFrameworkDelegate,
                       public PhiControlDelegate,
                       public PhiSlotHost,
                       public PhiSettingsHost,
                       public PhiTranscriptHost,
                       public PhiFeedbackHost {
 public:
  PhiCoordinator();
  ~PhiCoordinator() override;

  PhiCoordinator(const PhiCoordinator&) = delete;
  PhiCoordinator& operator=(const PhiCoordinator&) = delete;

  // phi_framework::PhiFrameworkDelegate:
  void OnFrameworkReady(phi_framework::PhiFramework* framework,
                        const char* user_data_dir_utf8) override;
  // Delivered INSIDE Browser::Create (contract rule 2): adopts the HWND by
  // `creation.kind`, matching a requested window to what RequestCreation
  // filed under its token. Creates and closes no browsers.
  void OnBrowserWindowCreated(
      const phi_framework::WindowCreation& creation) override;
  void OnBrowserWindowClosing(int64_t window_id) override;
  // The ONE strip/nav/split/group notification the client consumes (the
  // legacy OnTabsChanged that follows it is left unimplemented): stores the
  // snapshot in window_model_, settles the surfaces over the window and
  // posts the slot's rebuild. Delivered under contract rule 1, so the
  // framework may be called from inside it.
  void OnWindowChanged(const phi_framework::WindowSnapshot& snapshot,
                       phi_framework::WindowChangeReason reason) override;
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
  void OnRefreshNewTab(int64_t window_id) override;
  void OnContentFullscreenChanged(int64_t window_id, bool fullscreen) override;
  void OnExtensionsChanged(int64_t window_id) override;
  void OnProfileCreated(const char* profile_id) override;
  void OnReaderArticleExtracted(int request_id, const char* json) override;
  void OnShowFeedbackRequested() override;
  void OnFeedbackSystemLogs(int request_id, const char* text) override;
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
  void OnExtensionMessage(const char* request_id,
                          const char* sender_id,
                          const char* type,
                          const char* payload_json) override;
  void OnOpenReaderView(int64_t window_id, int64_t tab_id) override;
  void OnSessionRestoreSettled(const char* receipt_json) override;
  void OnAnalyticsEventCaptured(const char* module,
                                const char* event_name,
                                const char* properties_json) override;
  void OnColdStartRestorePlan(std::vector<int>* eager_window_ids,
                              bool* armed) override;
  void OnColdStartParkedWindows(const char* parked_json,
                                const char* reporting_profile) override;

  // --- Reader View ---------------------------------------------------------
  // Reader View lives in the Mirage extension; PhiReaderBridge is the app's
  // half of that conversation and these are the seams it acts through.
  PhiReaderBridge& reader() { return reader_; }
  // Answers one extension request (see PhiFramework::RespondToExtensionMessage).
  void RespondToExtension(const std::string& request_id,
                          const std::string& response_json);
  // App -> extensions. False when the broadcast reached nobody.
  bool BroadcastToExtensions(const std::string& type,
                             const std::string& payload_json);
  // The window whose ON-SCREEN browser holds `tab_id` (by the model, so a
  // tab hidden in a pin or bookmark cell still counts), or null when the
  // tab is not on screen anywhere (a background Space, or a tab already
  // gone).
  PhiWindowController* WindowShowingTab(TabId tab_id);
  // The reader extension's surface tab has appeared in the origin's strip:
  // move it into a browser of its own and host that in a panel over the
  // origin's page. False when there is no such tab to move.
  bool PanelizeReaderSurface(TabId origin_tab_id);
  // Closes the reader panel covering `origin_tab_id`, if any. The panel's
  // browser holds only the surface tab, so closing the window is the whole
  // teardown.
  void CloseReaderPanelForOrigin(TabId origin_tab_id);
  // Closes every surface of `kind` (kContent = all kinds) attached to the
  // tab: the panel browsers close, and OnBrowserWindowClosing forgets them.
  void CloseSurfacesForOrigin(TabId origin_tab_id,
                              SidebarSurface kind = SidebarSurface::kContent);
  // Whether a reader panel is currently up for this origin tab.
  bool HasReaderPanelForOrigin(TabId origin_tab_id) const;
  // A link inside the reading surface navigated it away from reader.html:
  // hand `url` back to the origin window as a normal tab and drop the panel.
  void ExpandReaderPanelIntoTab(int64_t panel_window_id,
                                const std::string& url);
  // The deferred half of the above (WM_PHI_READER_EXPAND).
  void FinishReaderPanelExpand(BrowserId panel_window);
  // A reader style axis moved: persist it.
  void OnReaderStyleChanged();
  // The reader affordance for `tab_id` may have changed — repaint the window
  // showing it.
  void OnReaderOfferabilityChanged(TabId tab_id);

  // PhiControlDelegate (called on the control server's I/O thread):
  std::string HandleControlCommand(const std::string& type,
                                   const std::string& payload_json) override;
  bool AuthorizeAgentPeer(const PhiAgentPeer& peer) override;

  // PhiSlotHost (called on the UI thread by a slot's sidebar):
  std::vector<PhiSpaceView> Spaces() const override;
  void RequestSwitchSpace(int64_t slot, const std::string& space_id) override;
  void RequestNewWindow(int64_t slot) override;
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
  void RequestDefaultBrowser() override;
  PhiLayoutMode LayoutMode() const override;
  void RequestLayoutMode(PhiLayoutMode mode) override;
  const PhiPalette& Palette() const override;
  const PhiPalette& PaletteForSlot(int64_t slot) const override;

  // Opens `url` as a foreground tab in the first shell window and raises it —
  // how a Kiosk window hands its page over when the user expands it.
  void OpenUrlInMainWindow(const std::string& url);
  // The Space a Kiosk offers to hand its page to: the main window's ACTIVE
  // Space, the Space holding the default role when no window is up — the
  // Mac's KioskSpaceMenuTargetResolver.primarySpace.
  std::string KioskTargetSpaceId() const;
  void RequestSettings() override;
  void RequestFeedback(int64_t slot,
                       const PhiFeedbackContext& context) override;
  void RequestSpaceIconPicker(const std::string& space_id) override;

  // PhiFeedbackHost:
  bool FeedbackAccount(std::string* email) override;
  void RequestFeedbackSignIn() override;
  void OpenFeedbackLink(int64_t window_id, const std::string& url) override;
  bool FetchFeedbackSystemLogs(int64_t window_id, int request_id) override;
  std::map<std::string, std::string> PhiExtensionVersions(
      int64_t window_id) override;
  void RequestCreateProfileForSpaceForm(int64_t slot,
                                        const std::string& name) override;
  std::string RequestCreateSpaceWithOptions(
      int64_t slot,
      const PhiNewSpaceSpec& spec) override;
  int64_t SlotAtScreenPoint(POINT screen, int64_t exclude_slot) const override;
  bool RequestDropTabOnWindow(int64_t source_slot,  // tab ids
                              int index,
                              int b_index,
                              int64_t target_slot) override;
  void NoteTearOffDrop(POINT screen) override;
  bool TakeTearOffDrop(POINT* screen) override;
  bool AlwaysShowBookmarkBar() const override;
  void SetAlwaysShowBookmarkBar(bool enabled) override;
  bool BookmarkBarOnNewTab() const override;
  void SetBookmarkBarOnNewTab(bool enabled) override;
  std::vector<PhiStoredRowView> PinnedRows(int64_t slot) const override;
  std::vector<SidebarStoredItem> SidebarStoredItems(int64_t slot) const override;
  bool Apply(int64_t slot,
             const SidebarCommand& command,
             const SidebarTree& tree) override;
  std::map<int, SidebarSurface> AttachedSurfaces(int64_t browser) const override;
  std::vector<PhiStoredRowView> BookmarkRows(
      int64_t slot,
      const std::string& parent_guid) const override;
  void RequestOpen(int64_t slot, const std::string& url) override;
  void RequestBookmarkActiveTab(int64_t slot) override;
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
  // The divider axis ("vertical" / "horizontal") of the live split holding
  // `tab`, for stored rows; empty when the tab is in no split.
  std::string LiveSplitLayout(BrowserId browser, TabId tab) const;
  bool RenameStoredRow(const std::string& guid,
                       const std::string& name) override;
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
  void NoteSlotActiveSpace(const std::string& space_id) override;
  void NoteSpaceWindow(const std::string& space_id,
                       int64_t browser_window_id) override;
  void NoteTabDistillable(int64_t window_id, int tab_id,
                          bool distillable) override;
  bool IsReaderOfferable(int tab_id, const std::string& url) const override;
  bool IsReaderActive(int tab_id, const std::string& url) const override;
  void RequestToggleReaderView(int tab_id, const std::string& url) override;
  void RequestCloseSurfaces(int tab_id) override;
  void NoteContentLayoutChanged(int64_t slot) override;
  void RaiseReaderPanels(int64_t slot) override;
  bool IsTabAgentDriven(int tab_id) const override;
  bool IsTabReclaimed(int tab_id) const override;
  void RequestReclaimTab(int tab_id) override;
  void RequestReleaseTab(int tab_id) override;
  void SetShowTabPreviews(bool enabled) override;
  std::string LanguagePreference() const override;
  void SetLanguagePreference(const std::string& tag) override;
  bool RestoreLastSession() const override;
  void SetRestoreLastSession(bool enabled) override;
  void FocusWindowForBrowser(int64_t browser_window_id) override;
  bool NewTabOpensOmnibox() const override;
  std::string AddBookmarkRow(int64_t slot,
                             const std::string& parent_guid,
                             const std::string& title,
                             const std::string& url) override;
  // Pushes the persisted restore choice into every loaded profile — run
  // when the framework comes up and after a profile loads.
  void PushRestorePreference();
  bool PeekViewEnabled() const override;
  void SetPeekViewEnabled(bool enabled) override;
  bool AutoPeekViewEnabled() const override;
  void SetAutoPeekViewEnabled(bool enabled) override;
  // Pushes both Peek switches into the framework, which owns the two
  // decisions they gate (the context-menu item and the cross-domain
  // throttle's diversion). Run once the framework is up and on every change.
  void PushPeekPreferences();
  PhiPinScope PinScope() const override;
  void SetPinScope(PhiPinScope scope) override;
  std::vector<std::string> SpaceSummaries() const override;
  std::string SpaceThemeId(const std::string& space_id) const override;
  void SetSpaceIcon(const std::string& space_id,
                    const std::string& icon) override;
  void SetSpaceSaturation(const std::string& space_id, int value) override;
  int SpaceSaturation(const std::string& space_id) const override;
  bool RenameSpace(const std::string& space_id,
                   const std::string& name) override;
  bool DeleteSpace(const std::string& space_id) override;
  // The Space holding the DEFAULT role (the Mac's currentDefaultSpaceId):
  // the landing target for retreats and windows opened without a Space, the
  // pre-selected bookmark target, the badge in Settings. Starts on "1" and,
  // when its holder is deleted, is handed to the first remaining Space —
  // persisted with the Space list so the hand-off survives relaunches.
  std::string DefaultSpaceId() const override;
  // Everything is deletable except the LAST remaining Space: retreat targets
  // and new-window fallbacks need one persisted Space to land on.
  bool CanDeleteSpace(const std::string& space_id) const override;
  // Re-binds a Space to another profile (the Mac's changeProfile): its
  // browser is closed and respawned in the new profile with its open tabs,
  // and the rows it owns move with it. Refused for the default Space, whose
  // browser is the one a fresh shell binds to.
  bool SetSpaceProfile(const std::string& space_id,
                       const std::string& profile_id) override;
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
  bool AllowAgentUserSpaces() const override;
  void SetAllowAgentUserSpaces(bool enabled) override;
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

  // Agent credential approval (C2). `kind` names the ROUTE the secret takes
  // — kFill: Phi types it into the page; kReveal: it is handed to the agent
  // — for the audit line and the prompt wording ONLY. For grant purposes
  // the two are the same exposure: the agent holds a CDP session on the
  // page it asked Phi to fill and can read the field back, so a fill is a
  // reveal with extra steps (docs/architecture-review.md §4 item 2).
  enum class CredentialAccessKind { kFill, kReveal };

  // Runs a control command on the UI thread; returns the JSON response.
  // Reached from HandleControlCommand through the posted WM_PHI_CONTROL and
  // the deferred-call queue, so it only ever runs from the main loop, never
  // inside a nested modal one.
  // Who is asking: the pipe (an agent — gated by IsUserSpaceRequest and the
  // test-rig switch) or the client's own UI, which calls the verb ladder as
  // its internal API and is never gated.
  enum class ControlCaller { kInternal, kPipe };
  std::string ExecuteControlOnUi(const std::string& type,
                                 const std::string& payload_json,
                                 ControlCaller caller);
  // The user-space gate (lane 5 F3). True when `type` is a verb that reads
  // or changes the USER's own Spaces/state AND the request's scope resolves
  // to a user window/Space — a {taskId} scope, or a windowId naming an
  // agent/shadow window, is agent territory and stays ungated. Resolves the
  // scope the way the handlers do (ResolveScopeBrowser / ControllerForSlot /
  // DialectOwner), so the answer matches what the verb would touch.
  bool IsUserSpaceRequest(const std::string& type,
                          const base::DictValue* payload);
  // Runs `call` now, or queues it while a nested modal loop is on the UI
  // thread (PhiModalDepth() > 0) and arms kDeferredUiCallTimer to drain
  // the queue once the loop unwinds. The UI side owns its reference to the
  // call and releases it after running it.
  void RunOrDeferUiCall(PhiUiCall* call);
  void RunUiCall(PhiUiCall* call);
  void DrainDeferredUiCalls();
  // Everything that must reach disk before the session ends: pending
  // coalesced saves, the telemetry spool and the feedback outbox worker.
  void FlushForShutdown();
  // The browser a wire `windowId` means: a slot number resolves to the
  // browser that slot is SHOWING (the Space's browser, never the slot's
  // original), anything else names a browser itself (an agent window, a
  // Space browser the caller learned from spaces.list).
  BrowserId ResolveBrowser(int64_t slot_or_browser) const;
  BrowserId ResolveBrowser(SlotId slot) const;
  // The session id of `browser`'s active tab (what the extensions API calls
  // a tab id), or invalid — from the model.
  TabId ActiveTabId(BrowserId browser) const;
  // A window's tab strip changed: re-fit, re-hide and retire the reader
  // panels bound to it. Reads the model, so it runs AFTER the snapshot that
  // reports the change has been stored.
  void UpdateReaderPanelsForWindow(BrowserId window_id);
  // Finishes the store's schema 1 -> 2 fold on LIVE tabs: a tab still tagged
  // with a folded-away pin guid (session restore brings them back) is retagged
  // to the survivor's pane-B tag. No-op once the store has no aliases.
  void ApplyLegacyTagAliases(BrowserId window_id);
  // Swaps in the url-favicon service's larger icon when it beats the row's
  // stored 16px capture.
  void UpgradeFavicon(SlotId slot,
                      const std::string& url,
                      PhiStoredRowView* view) const;
  // The slot's controller; an invalid slot (the wire's 0 / no windowId)
  // falls back to the first USER window — the verb convention for "the
  // window". Null when none exists.
  PhiWindowController* ControllerForSlot(SlotId slot);
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
  // Thread-scoped mouse hook: a click anywhere in a shell OUTSIDE its open
  // Peek card dismisses the card (the Mac's click-outside monitor). Clicks
  // reach the browser child's own HWNDs, never the shell, so a hook is the
  // only place that sees them first.
  HHOOK mouse_hook_ = nullptr;
  static LRESULT CALLBACK MouseHookProc(int code, WPARAM wparam, LPARAM lparam);
  // Dismisses the Peek card(s) floating over `shell`, skipping any whose
  // card contains `inside` (a click landing in the card itself). Returns
  // true when one was dismissed.
  bool DismissPeeksForShell(HWND shell, HWND inside);
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

  // The client's one copy of framework window state (docs/architecture-
  // review.md §3.3 "client side"): every controller reads it through a
  // const pointer, so it is declared BEFORE the registry that owns them and
  // outlives them. Fed by OnWindowChanged; pulled (Refresh) for a browser
  // adopted before its first push and after a mutation whose result is
  // needed on the spot.
  PhiWindowModel window_model_;
  // The shell slots, the browser -> slot map, the kiosks and the creations
  // in flight (docs/architecture-review.md §3.4). Every "which window"
  // question goes through here.
  PhiWindowRegistry registry_;
  // Peek and Reader View panels — browsers of their own, hosted over an
  // origin tab's page, one lifecycle for both kinds (see the registry).
  PhiSurfaceRegistry surfaces_;
  // Panels whose page left reader.html, with the url to re-open, until the
  // deferred expand runs.
  std::map<BrowserId, std::string> pending_reader_expands_;
  int sidebar_width_ = kSidebarWidthDefault;
  // Spaces are a client concept. Each binds EXACTLY ONE profile; several
  // Spaces may share a profile (the fork's model). The binding is fixed for
  // the life of a browser window — changing it goes through SetSpaceProfile,
  // which respawns the Space's window.
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
  // The default role's holder (see DefaultSpaceId); validated on load.
  std::string default_space_id_ = "1";
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
                       BrowserId source_window);
  // The shared store-tab/split bodies behind the menu (append, bookmark
  // unbound) and drag (placed + bound) entry points.
  // One cell from live tab(s), by STABLE id: a single (tab_b 0) or a split
  // (both panes), as a pin or a bookmark under `parent_guid`, at
  // `insert_index` among its siblings (INT_MAX = end), bound when `bind`.
  // Returns the new guid, empty on refusal.
  std::string StoreTabsById(SlotId slot,
                            TabId tab_a,
                            TabId tab_b,
                            bool as_bookmark,
                            const std::string& parent_guid,
                            int insert_index,
                            bool bind);
  // A live tab bound by tag to a stored row: which browser holds it, its
  // id, and whether it is the row's pane B.
  struct BoundTab {
    BrowserId browser;
    TabId tab;
    bool pane_b = false;
  };
  // Every live tab (any browser the model knows) bound to the row `guid`.
  std::vector<BoundTab> TabsBoundTo(const std::string& guid) const;
  // The Move transition table (SidebarMove).
  bool ApplyMove(SlotId slot, const SidebarMove& move, const SidebarTree& tree);
  // Opens a closed cell's pane(s) as live tab(s) bound to it; returns pane
  // A, invalid on refusal.
  TabId OpenCell(BrowserId browser, const std::string& guid);
  // SetTabTagById + a model pull, so the row rebuild this gesture posts
  // already sees the tag (see the definition for why the push is not enough).
  void TagTab(BrowserId browser, int tab_id, const char* tag);
  // Former host verbs, now internal to Apply and the agent dialect.
  bool ConvertStoredRow(SlotId slot,
                        const std::string& guid,
                        bool to_bookmark,
                        const std::string& parent_guid,
                        int index);
  bool MoveStoredRowToSpace(const std::string& guid,
                            const std::string& space_id,
                            bool clone);
  void RequestTabToSpace(SlotId slot,
                         TabId tab,
                         const std::string& space_id,
                         bool move);
  void StoreSplitInternal(SlotId slot, TabId tab_a, TabId tab_b,
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
  // Peek View: master gate and the automatic bound-opener diversion under
  // it. Both the Mac's defaultValue, and what the framework did before it
  // was asked.
  bool peek_view_enabled_ = true;
  bool auto_peek_view_enabled_ = true;
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
  // Message-window (msg_hwnd_) timer ids. (Id 1 was the profile-creation
  // poll; OnProfileCreated made it redundant.)
  // Agent-space / shadow-window keep-alive sweep (the Mac's 10 s tick) —
  // also sweeps the creations in flight (registry_ pending, pending_profiles_).
  static constexpr UINT_PTR kAgentSpaceSweepTimer = 2;
  static constexpr UINT kAgentSpaceSweepIntervalMs = 10 * 1000;
  // The operating mask's idle/takeover sweep.
  static constexpr UINT_PTR kAgentDriveSweepTimer = 3;
  // Drains deferred_ui_calls_ once no modal loop is up.
  static constexpr UINT_PTR kDeferredUiCallTimer = 4;
  static constexpr UINT kDeferredUiCallPollMs = 50;
  // Coalesces the last-active-Space write into one SavePrefs.
  static constexpr UINT_PTR kLastActiveSaveTimer = 5;
  static constexpr UINT kLastActiveSaveDelayMs = 1000;
  // Cold start armed an eager set: if no user window is up once the replay
  // settles, open a default shell (lane 4 §6 item 6). Armed for the replay
  // to have its say, re-armed shorter once the park list has landed.
  static constexpr UINT_PTR kColdStartFallbackTimer = 6;
  static constexpr UINT kColdStartFallbackMs = 4000;
  static constexpr UINT kColdStartSettleMs = 2000;
  // How long a RequestBrowser / CreateProfile request may stay unanswered
  // before the sweep presumes it lost and fails any waiting reply with
  // create_timeout. The framework has no failure callback for either.
  static constexpr uint64_t kCreateTimeoutMs = 20 * 1000;
  // Arms the sweep while anything needs sweeping (agent Spaces, shadow
  // windows, creations in flight) and stops it once nothing does.
  void UpdateSweepTimer();
  // Verbs and consent decisions that arrived while a modal loop was on the
  // UI thread, in arrival order (see RunOrDeferUiCall).
  std::deque<PhiUiCall*> deferred_ui_calls_;
  bool draining_ui_calls_ = false;
  // The Space the user was in when the run ended (PhiPrefs.json
  // lastActiveSpaceId): what LastActiveSpaceId() answers before any window
  // is live. Noted on every switch; written by kLastActiveSaveTimer.
  std::string last_active_space_id_;
  void NoteActiveSpace(const std::string& space_id);
  // True from OnColdStartRestorePlan arming a filter until a user window
  // exists — the window the fallback timer checks for.
  bool cold_start_armed_ = false;
  // Opens a shell on the default Space when a restore left no user window.
  void OpenDefaultShellIfNoUserWindow(const char* reason);
  // Drops (framework side) and forgets the parked windows no Space claims.
  void DropOrphanGhosts();
  // "Restore last session" — persisted app-wide, mirrored into each loaded
  // profile's SessionStartupPref (LAST/DEFAULT).
  bool restore_last_session_ = false;
  // Persistent "Always Allow" agent grants (lowercased image paths) and the
  // session-scoped consent memory (allow-once / deny-for-now per path).
  std::set<std::wstring> agent_grants_;
  std::set<std::wstring> session_allowed_peers_;
  std::set<std::wstring> session_denied_peers_;
  std::wstring agent_grants_path_;
  // Credential approvals. The persisted "always" grants are LOADED FOR THE
  // SETTINGS DISPLAY ONLY — they no longer cover anything (a standing
  // password grant plus an agent-chosen page was the exfil path, lane 5
  // F2); the timed ones are session-scoped by design (the Mac's in-memory
  // 10-minute cache), so a restart is always a clean slate for them.
  std::map<std::string, CredentialAccessKind> credential_grants_;
  struct TimedCredentialGrant {
    CredentialAccessKind kind = CredentialAccessKind::kFill;
    ULONGLONG expires_tick = 0;
  };
  std::map<std::string, TimedCredentialGrant> credential_timed_grants_;
  // Denial memory: grant key -> tick until which the request is refused
  // without a prompt (5 minutes), so a denied ask cannot re-prompt at once.
  std::map<std::string, ULONGLONG> credential_denials_;
  std::vector<std::string> credential_audit_;
  std::wstring credential_grants_path_;
  // --- Deferred control replies -------------------------------------------
  // A verb whose answer arrives asynchronously (reader extraction, a window
  // or profile creation) returns {"pendingReply":<id>} from the UI thread;
  // HandleControlCommand (the pipe I/O thread) then waits on the id's event
  // and returns the JSON the UI thread stored for it. The map is the only
  // coordinator state touched from two threads.
  struct PendingReply {
    HANDLE event = nullptr;  // Manual-reset; owned by the I/O waiter.
    std::string json;
    bool document = false;      // readerDocument (else readerArticle)
    bool include_html = true;   // readerArticle {includeHTML}
  };
  base::Lock pending_replies_lock_;
  std::map<int, PendingReply> pending_replies_;
  int next_pending_reply_ = 1;
  // UI thread: registers a slot and returns its id.
  int RegisterPendingReply(bool document, bool include_html);
  // UI thread: stores the answer and wakes the waiter (no-op if it gave up).
  void CompletePendingReply(int id, std::string json);
  // I/O thread: waits up to `timeout_ms` and returns the stored JSON (or a
  // timeout error), releasing the slot either way.
  std::string WaitPendingReply(int id, DWORD timeout_ms);
  // The JSON error a waiter gets when its creation never arrived.
  static constexpr char kCreateTimeoutReply[] =
      "{\"ok\":false,\"error\":\"create_timeout\"}";
  // The bookmark bar's two visibility switches (the Mac View menu's pair).
  bool always_show_bookmark_bar_ = false;
  bool bookmark_bar_on_new_tab_ = false;
  // Shortcut overrides, keyed by the shortcut's STABLE NAME (never the
  // command int, which is free to be renumbered). vk 0 = explicitly unbound.
  std::map<std::string, PhiShortcutChord> shortcut_overrides_;
  // Gates the Developer settings pane, as on Mac (default off).
  bool developer_mode_ = false;
  // Settings ▸ Developer ▸ "Allow agents to operate your Spaces"
  // (PhiPrefs.json allowAgentUserSpaces). Off: every user-space verb fails
  // user_space_operations_disabled (IsUserSpaceRequest). NEVER settable
  // over the control channel.
  bool allow_agent_user_spaces_ = false;
  // PhiPrefs.json testRig: the box runner seeds it; unlocks the
  // harness-only verbs (feedback.setAccount, analytics.list, window.handle,
  // captureWindow to a path). No UI, never agent-settable.
  bool test_rig_ = false;
  // Reader View's app half. Holds the reading style (one store for every
  // reader surface, persisted in PhiPrefs.json) and the per-tab offer/state
  // the extension reports.
  PhiReaderBridge reader_{this};
  std::wstring prefs_path_;
  void LoadPrefs();
  void SavePrefs() const;
  // Hands the display language to the framework, which owns Chromium's own
  // locale (context menus, error pages, Accept-Language). The client catalog
  // and Chromium keep separate copies of the choice, so this runs both when
  // it changes and once at startup, where it reconciles a preference file
  // that was edited while the browser was not running.
  void PushDisplayLanguageToChromium();
  // Pinned tabs and bookmarks, the Mac's LocalStore equivalent.
  PhiTabStore tab_store_;
  // The (profile, space) a slot's pinned/bookmark rows belong to.
  bool OwnerForSlot(SlotId slot, std::string* profile_id,
                    std::string* space_id) const;
  // The active tab's favicon as base64 BGRA, for storing on a pinned/bookmark
  // row. A pinned tab has no live tab to ask later.
  std::string CaptureFavicon(SlotId slot, int* width, int* height);
  // The url the slot is showing right now (the model's nav state).
  std::string LiveUrl(SlotId slot) const;
  // Recomputes `palette_` and repaints every slot.
  void ApplyTheme();
  // Asks the framework for a browser (PhiFramework::RequestBrowser) and files
  // `pending` under the token it returns, so OnBrowserWindowCreated can
  // finish the job when the window arrives — `kind`, the profile and the
  // deadline are stamped here. Invalid when the framework refused; then
  // nothing was filed and a waiting reply (pending.pending_reply_id) is
  // already answered create_failed.
  RequestToken RequestCreation(phi_framework::WindowKind kind,
                               const std::string& profile_id,
                               bool incognito,
                               BrowserId source_window,
                               PendingCreation pending);
  // The OnBrowserWindowCreated branches. Each adopts `hwnd` for `browser`.
  // A requested kind whose pending record names a slot that is gone, and a
  // peek/reader whose opener is gone, fall back to HostUserShell.
  void HostUserShell(BrowserId browser, HWND hwnd, bool incognito);
  bool AttachRequestedSpaceBrowser(BrowserId browser,
                                   HWND hwnd,
                                   const PendingCreation& pending);
  void HostAgentWindow(BrowserId browser,
                       HWND hwnd,
                       bool incognito,
                       const PendingCreation& pending);
  bool AttachRestoredGhost(BrowserId browser,
                           HWND hwnd,
                           BrowserId restored_from);
  bool HostSurfacePanel(const phi_framework::WindowCreation& creation);
  // A tear-off's drop point, waiting for the new window it asked for (the
  // Mac's pendingTearOffWindowPlacement): the next shell opens around it.
  // `tear_off_drop_at_` is the tick it was noted at; 0 = none pending.
  POINT tear_off_drop_{};
  ULONGLONG tear_off_drop_at_ = 0;
  // A CreateProfile in flight and what to do with the profile once
  // OnProfileCreated names it. Keyed by the DISPLAY NAME asked for — the
  // only thing the client knows about the profile before the framework
  // reports its basename; the oldest request is the fallback when the
  // name cannot be read back. Swept with kCreateTimeoutMs.
  struct PendingProfile {
    enum class Then {
      kOpenSpace,   // "+ Space & profile": create a Space on it, switch.
      kFillForm,    // The create-Space form's picker selects it.
      kReplyOnly,   // profiles.create: answer the waiting verb.
    };
    std::string display_name;
    SlotId slot;
    Then then = Then::kReplyOnly;
    int pending_reply_id = 0;
    uint64_t deadline_tick = 0;
  };
  std::deque<PendingProfile> pending_profiles_;
  // Files a CreateProfile and asks the framework for it.
  void RequestProfile(PendingProfile pending);
  // The display name the framework reports for `profile_id`, or empty.
  std::string ProfileDisplayName(const std::string& profile_id) const;

  // --- Agent Spaces (the phi-browser skill's hidden windows) ---------------
  PhiAgentSpaces agent_spaces_;
  // The agent Space whose window is currently surfaced for the user to watch
  // (empty = none). At most one is watched at a time.
  std::string watched_agent_task_;

  // --- Shadow windows (the skill's invisible background windows) ----------
  // A shadow window is an agent-style off-screen window WITHOUT a presence:
  // no pip, no watch, no transcript, no ownership — the Mac's
  // AgentSpaceManager+Shadow registry, keyed by the skill's taskId. Kept out
  // of agent_spaces_ so nothing that draws or surfaces agent Spaces sees it;
  // the window itself is adopted exactly like an agent Space's (hidden
  // controller, parked off-screen, composited for CDP).
  struct ShadowWindow {
    std::string task_id;
    BrowserId window_id;  // Its value is the CDP windowId.
    std::string profile_id;
    bool incognito = false;
    int64_t created_ms = 0;              // epoch ms (the list's createdAt)
    uint64_t keepalive_deadline_ms = 0;  // GetTickCount64 ms; swept when past
  };
  std::map<std::string, ShadowWindow> shadow_windows_;
  ShadowWindow* FindShadowByWindow(BrowserId window_id);
  // The Mac's keep-alive contract, shared with agent Spaces: extends the
  // deadline to max(existing, now + ttl); ttl <= 0 is the default round TTL.
  void TouchShadow(ShadowWindow* shadow, int ttl_seconds, uint64_t now_ms);
  void CloseShadowWindow(const std::string& task_id, const char* reason);
  // agentSpace.shadow.* (create / list / openTab / ping / close).
  bool HandleShadowVerb(const std::string& type,
                        const base::DictValue* payload,
                        uint64_t now_ms,
                        base::DictValue* out);
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
  // Invalid with `error` set (unknown_task / space_not_open /
  // invalid_payload) when the scope names nothing live.
  BrowserId ResolveScopeBrowser(const base::DictValue* payload,
                                std::string* error);
  // The browser hosting `space_id`'s tabs (possibly hidden behind a sibling
  // Space), or invalid when the Space has no open window.
  BrowserId BrowserForSpace(const std::string& space_id) const;
  // The USER window the user is looking at: the foreground shell when it is
  // one of ours, the first user window otherwise. Null without windows.
  PhiWindowController* FocusedUserController();
  // Surfaces `space_id` in `slot` (invalid: the first user window),
  // requesting its browser in the Space's profile when the slot has none —
  // the spaces.switch body, shared with the skill's spaces.activate.
  // `cold` reports the request path; `pending` that the browser is still
  // to arrive (a request never attaches before it returns). `reopen_urls`
  // are opened in the new browser once it attaches (a re-profiled Space's
  // tabs).
  bool SwitchSlotToSpace(SlotId slot, const std::string& space_id,
                         bool* pending, bool* cold,
                         const std::vector<std::string>* reopen_urls = nullptr);
  // The (profile, space) the skill's pinned/bookmark dialect verbs target:
  // an explicit profile id wins, else the focused window's active Space.
  bool DialectOwner(const std::string& profile_id_or_empty,
                    std::string* profile_id, std::string* space_id);
  // Tears down an agent Space: closes its hidden window and drops the record.
  void CloseAgentSpace(const std::string& task_id);
  // Keep-alive sweep (kAgentSpaceSweepTimer): completes and closes expired
  // spaces and shadows, and fails the creations that never arrived.
  void SweepAgentSpaces();

  // --- Credentials (the skill's agent password surface) --------------------
  // The REAL provider: the out-of-process Bitwarden helper (the user's own
  // vault, same engine as macOS). `credentials_` is a local DPAPI dev/test
  // store used ONLY when no helper binary is installed.
  PhiVaultHelper vault_helper_;
  PhiCredentialVault credentials_;
  // The Auth0 session (bucket D auth, stage 1).
  PhiAuth auth_;
  // Hands the signed-in account to the feedback outbox (whose jobs are
  // filed and uploaded per account) after every auth change.
  void SyncFeedbackAccount();
  // A harness-set stand-in account (agentSpace.feedback.setAccount) for a
  // box with no Auth0 session: reports file and prepare under it, and the
  // upload then fails for want of a token — which is the testable part.
  // Ignored while a real session is signed in.
  std::string test_feedback_user_id_;
  std::string test_feedback_email_;
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
  // Consults the timed grants and the denial memory first; prompts only
  // when neither answers. `grant_key` is "<item scope>|<page host>" for a
  // fill and the item scope for a reveal: grants are all-agents, as on Mac,
  // but never wider than one page host. `once_only` (a cross-origin fill)
  // offers no timed grant and consumes none.
  bool PromptCredentialApproval(const std::wstring& detail,
                                CredentialAccessKind kind,
                                const std::string& grant_key,
                                bool once_only);
  // Pushes agentSpace.ownershipChanged to connected drivers — the Mac's
  // broadcastToTaskDriver. Called after every ownership transition; a task
  // whose record is already gone is deliberately NOT announced (drivers
  // learn of an ended task from its absence in agentSpace.list).
  void PushOwnershipChanged(const std::string& task_id);
  // Mounts/updates/tears down the watch-mode overlay for the watched task.
  void RefreshAgentOverlay();
  // True while a live TIMED grant covers `grant_key`. The kind is not
  // consulted: fill and reveal are one exposure (see CredentialAccessKind).
  bool CredentialGrantCovers(const std::string& grant_key) const;
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
