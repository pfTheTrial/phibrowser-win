// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SHELL_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SHELL_WINDOW_H_

#include <windows.h>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_drag_image.h"
#include "phibrowser-win/client/phi_feedback_window.h"
#include "phibrowser-win/client/phi_browser_host.h"
#include "phibrowser-win/client/phi_caption_band.h"
#include "phibrowser-win/client/phi_menu_bar.h"
#include "phibrowser-win/client/phi_native_pages_host.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_omnibox_session.h"
#include "phibrowser-win/client/phi_sidebar_actions.h"
#include "phibrowser-win/client/phi_sidebar_view.h"
#include "phibrowser-win/client/phi_shell_menus.h"
#include "phibrowser-win/client/phi_shell_layout.h"
#include "phibrowser-win/client/phi_spaces_strip.h"
#include "phibrowser-win/client/phi_sidebar_model.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_model.h"

namespace phi_client {

// PhiLayoutMode and its wire names live in phi_shell_layout.h (the pure
// layout needs them on the host); every includer of this header sees them.

// A stored row (pinned tab or bookmark) as the chrome consumes it — the
// favicon already decoded to raw premultiplied BGRA.
struct PhiStoredRowView {
  std::string guid;
  std::string title;  // UTF-8.
  std::string url;
  bool folder = false;
  std::vector<unsigned char> favicon_bgra;
  int favicon_width = 0;
  int favicon_height = 0;
  // Split cell (pin or bookmark): ONE row, the second pane riding here.
  std::string url_b;
  std::string title_b;
  std::vector<unsigned char> favicon_b_bgra;
  int favicon_b_width = 0;
  int favicon_b_height = 0;
  // Folders only: the catalog symbol id (the Mac's folderIconName).
  std::string icon;
  // Split rows only: "vertical" (side-by-side) or "horizontal" (stacked).
  std::string layout;
};

// A Space as the chips row and menus consume it.
struct PhiSpaceView {
  std::string id;
  std::string name;       // UTF-8.
  std::string profile_id;
  std::string icon;       // Storage form ("emoji:<id>" / "phi:<id>").
  std::string icon_text;  // Resolved display glyph, UTF-8 (may be PUA).
};

// What the create-Space form commits — CreateSpacePanel.create's arguments.
struct PhiNewSpaceSpec {
  std::string name;        // Already defaulted ("Space N") by the form.
  std::string profile_id;
  std::string icon;        // Storage form ("phi:<id>" / "emoji:<id>").
  std::string theme_id;    // A built-in theme, pinned to the new Space.
  int saturation = -1;     // The slider once dragged; -1 = follow the app.
  std::string color_hex;   // The theme overlay, "#RRGGBB" (the Mac's colorHex).
};

// A live agent Space as a pip in the switcher — the Mac's robot Space pip.
// The user clicks it to watch the (normally hidden) agent Space live.
struct PhiAgentPipView {
  std::string task_id;   // The skill's taskId; also the pip's identity.
  std::string space_id;  // "agentspace-N": what a slot hosts it under.
  std::string caption;   // The agent's current status line (may be empty).
  std::string agent_name;  // "Claude Code", "Codex", … or empty.
  int number = 0;        // The Mac's task ordinal ("R1").
  // What the corner badge shows (the Mac's agentBadge, in precedence):
  // error (failed, or an error the user has not seen yet), completed,
  // the user holding control, idle, else running.
  enum class Badge { kRunning, kIdle, kUserHolds, kCompleted, kError };
  Badge badge = Badge::kRunning;
  bool watched = false;  // Its window is currently surfaced on-screen.
};

// What a slot needs from its owner (the coordinator), as five narrow
// interfaces (docs/architecture-review.md §3.5): the full Space list to draw,
// and requests that only the owner can service (switching to a Space that
// has no browser yet must create one in that Space's profile). The shell
// takes the five as a ShellServices at construction and hands each
// component only the ones it calls — there is no union to reach a service
// through any more. ChromePrefsHost is PhiPreferences already; the
// coordinator implements the other four by forwarding to the service that
// owns each.
// Every `int64_t slot` parameter carries SlotId::value (the controller's
// slot()) and becomes SlotId in phase 2; a `browser` parameter carries
// BrowserId::value.
class PhiTabPreview;
class PhiBookmarkManager;
class PhiNewTabPage;
class PhiCreateSpacePanel;

// Stored items, rows, surfaces, Reader View and THE sidebar mutator.
class SidebarHost {
 public:
  virtual ~SidebarHost() = default;
  // Reader View: upstream says this tab has an article. The shell shows its
  // reader button only for such tabs.
  virtual void NoteTabDistillable(int64_t window_id,
                                  int tab_id,
                                  bool distillable) = 0;
  // Reader View proper is the Mirage extension's — it probes every page,
  // extracts in-page and renders on its own surface, and the client relays
  // intent. True when the extension's probe (or a site rule) says this tab's
  // CURRENT document has an article; the shell ORs it with upstream's
  // distillability verdict, which is the Mac's rule for offering the button.
  virtual bool IsReaderOfferable(int tab_id, const std::string& url) const = 0;
  // True while the extension's reading surface stands for this tab — either
  // the tab IS that surface, or the surface is open on its behalf.
  virtual bool IsReaderActive(int tab_id, const std::string& url) const = 0;
  // Enters Reader View for the tab, or leaves it when the tab already shows
  // the reading surface. `url` is what tells those two apart.
  virtual void RequestToggleReaderView(int tab_id, const std::string& url) = 0;
  // The page underneath the tab's attached surfaces (Reader View, Peek) is
  // about to change or move — an address-bar navigation or Back/Forward on
  // the origin — so every surface attached to it closes first. A surface
  // cannot be left over a page that changed beneath it.
  virtual void RequestCloseSurfaces(int tab_id) = 0;
  // The content card moved or resized: re-fit whatever is hosted over it.
  virtual void NoteContentLayoutChanged(int64_t slot) = 0;
  // Raise this slot's surface panels above the browser child, so the corner
  // caps can then be raised above THEM (see RaiseCornerCaps).
  virtual void RaiseReaderPanels(int64_t slot) = 0;
  // The slot's pinned tabs, in sibling order.
  virtual std::vector<PhiStoredRowView> PinnedRows(int64_t slot) const = 0;
  // Complete scoped identity inventory, independent of folder expansion,
  // layout, and favicon loading. The sidebar model owns live reconciliation.
  virtual std::vector<SidebarStoredItem> SidebarStoredItems(int64_t slot) const = 0;
  // THE mutator for sidebar state (docs/sidebar-architecture.md §5): every
  // gesture, menu item and shortcut is a SidebarCommand applied against the
  // slot's current tree. False when the command was refused or was a no-op.
  virtual bool Apply(int64_t slot,
                     const SidebarCommand& command,
                     const SidebarTree& tree) = 0;
  // Peek/Reader surfaces attached to tabs of `browser`: origin tab id ->
  // kind. The model puts the indicator on whichever row owns the origin.
  virtual std::map<int, SidebarSurface> AttachedSurfaces(
      int64_t browser) const = 0;
  // Bookmark rows under `parent_guid` (empty = the root), so the menu bar can
  // descend into folders the way the Mac's Bookmarks menu does.
  virtual std::vector<PhiStoredRowView> BookmarkRows(
      int64_t slot,
      const std::string& parent_guid) const = 0;
  // Opens `url` in the slot (a pinned tab or bookmark was clicked).
  virtual void RequestOpen(int64_t slot, const std::string& url) = 0;
  // Appends one bookmark row under `parent_guid` ("" = root) in `slot`'s
  // Space — Bookmark All Tabs' per-tab fill. Returns the new row's guid.
  virtual std::string AddBookmarkRow(int64_t slot,
                                     const std::string& parent_guid,
                                     const std::string& title,
                                     const std::string& url) = 0;
  // Bookmarks the slot's active tab — the menu bar's Bookmark This Tab.
  virtual void RequestBookmarkActiveTab(int64_t slot) = 0;
  // Creates a bookmark folder under `parent_guid` (empty = the root) in the
  // slot's Space — the sidebar's New Folder entries. Returns the new guid
  // (empty on refusal).
  virtual std::string AddBookmarkFolder(int64_t slot,
                                        const std::string& parent_guid,
                                        const std::string& title) = 0;
  // Removes a stored row (a pinned tab or bookmark) by its guid.
  virtual void RemoveStoredRow(const std::string& guid) = 0;
  // Retitles a stored row — the context menus' Rename….
  // Sets a bookmark folder's catalog symbol (the Mac's "Change Icon...").
  virtual bool SetStoredRowIcon(const std::string& guid,
                                const std::string& icon) = 0;
  // Retargets a bookmark (the manager's Address cell edit).
  virtual bool SetStoredRowUrl(const std::string& guid,
                               const std::string& url) = 0;
  // Remembers a split row's divider axis so it reopens stacked or
  // side-by-side (the Mac's persistent split layout).
  virtual bool SetStoredRowLayout(const std::string& guid,
                                  const std::string& layout) = 0;
  virtual bool RenameStoredRow(const std::string& guid,
                               const std::string& name) = 0;
};

// The Space list, switching, creation, per-Space theme and the default role.
class SpacesHost {
 public:
  virtual ~SpacesHost() = default;
  // Every Space, in display order.
  virtual std::vector<PhiSpaceView> Spaces() const = 0;
  virtual void RequestSwitchSpace(int64_t slot, const std::string& space_id) = 0;
  // Creates a Space in this slot's current profile (`new_profile` = false) or
  // in a freshly created profile (true), then switches to it.
  virtual void RequestCreateSpace(int64_t slot, bool new_profile) = 0;
  // A Space's browser is live in this slot. The coordinator remembers the
  // (Space, window id) pair so a later run can name that window when it asks
  // the framework to park or rebuild it (lazy Space restore).
  virtual void NoteSpaceWindow(const std::string& space_id,
                               int64_t browser_window_id) = 0;
  // The user is now looking at `space_id` in this slot — every switch, not
  // only the ones the coordinator drove: a chip click on a Space whose
  // browser is already attached never leaves the controller. The
  // coordinator persists it as the Space a cold start should reopen
  // (docs/architecture-review.md §4 item 21). Agent shells never call it.
  virtual void NoteSlotActiveSpace(const std::string& space_id) = 0;
  // Moves a Space to `new_index` in the Space list (chip drag).
  virtual void ReorderSpace(const std::string& space_id, int new_index) = 0;
  // The theme THIS SLOT paints with: a Space may pin its own (the Mac's
  // per-Space theme override), otherwise the app-wide one.
  virtual const PhiPalette& PaletteForSlot(int64_t slot) const = 0;
  // A Space's pinned theme id (empty = follows the app-wide one), and setting
  // it — the sidebar's Space menu writes through here.
  virtual std::string SpaceThemeId(const std::string& space_id) const = 0;
  virtual void SetSpaceTheme(const std::string& space_id,
                             const std::string& theme_id) = 0;
  // A Space's own overlay saturation, 0..100 (-1 = follow the app-wide
  // slider), and setting it — the Set Theme panel's second row. The setter
  // is the Settings host's spelling, so one coordinator override serves
  // both.
  virtual int SpaceSaturation(const std::string& space_id) const = 0;
  virtual void SetSpaceSaturation(const std::string& space_id, int value) = 0;
  // Space management (the menu bar and the chip context menu). Deleting the
  // default Space is refused; deletion takes the Space's bookmarks and
  // space-scoped pins with it, as on Mac.
  virtual bool RenameSpace(const std::string& space_id,
                           const std::string& name) = 0;
  virtual bool DeleteSpace(const std::string& space_id) = 0;
  // The Space holding the default role — what a fresh shell binds its first
  // browser to (see the coordinator's DefaultSpaceId).
  virtual std::string DefaultSpaceId() const = 0;
  // Whether the Space menu may offer Delete (never for the last Space).
  virtual bool CanDeleteSpace(const std::string& space_id) const = 0;
  // Re-binds a Space to another profile — the Space menu's "Change Profile",
  // the Mac's SpaceManager.changeProfile. A window's profile is baked in at
  // spawn, so the Space's browser is closed and respawned with its tabs
  // reopened there; the default Space keeps its binding. False when nothing
  // changed.
  virtual bool SetSpaceProfile(const std::string& space_id,
                               const std::string& profile_id) = 0;
  // Profile management. Deleting Default, or a profile still bound to a
  // Space, is refused (re-bind or delete those Spaces first).
  virtual bool DeleteProfileById(const std::string& profile_id) = 0;
  // Opens Settings > Spaces with this Space selected — its detail column is
  // the icon picker (the "Change Icon…" destination).
  virtual void RequestSpaceIconPicker(const std::string& space_id) = 0;
  // The create-Space form (PhiCreateSpacePanel). ProfileSummaries is the
  // Settings host's spelling, so one coordinator override serves both.
  virtual std::vector<std::string> ProfileSummaries() const = 0;  // "id\tname"
  // Creates a profile for the form; the slot's controller hears
  // OnSpaceFormProfileCreated once it exists (creation is asynchronous).
  virtual void RequestCreateProfileForSpaceForm(int64_t slot,
                                                const std::string& name) = 0;
  // Creates the Space the form described, pins its theme, and switches
  // `slot` to it. Returns the new Space's id, or empty on failure.
  virtual std::string RequestCreateSpaceWithOptions(
      int64_t slot,
      const PhiNewSpaceSpec& spec) = 0;
};

// The client-wide chrome preferences: layout, palette, previews, the
// bookmark-bar flags, the sidebar width. PhiPreferences implements it.
class ChromePrefsHost {
 public:
  virtual ~ChromePrefsHost() = default;
  // The layout every slot renders in — one setting for the whole client, as on
  // Mac, so a new window never disagrees with the one beside it.
  virtual PhiLayoutMode LayoutMode() const = 0;
  virtual void RequestLayoutMode(PhiLayoutMode mode) = 0;
  // The Mac's GeneralSettings.showTabPreviews: whether a settled hover on a
  // tab, pin or bookmark row raises its page-snapshot card.
  virtual bool ShowTabPreviews() const = 0;
  // The Mac's GeneralSettings.showOpenTabIndicators: a dot on an inactive
  // pinned tab or bookmark whose tab is open. Off by default.
  virtual bool ShowOpenTabIndicators() const = 0;
  // Developer ▸ "Allow agents to control Phi": gates the View menu's agent
  // items, as the Mac's cdpAgentAccessEnabled does.
  virtual bool AllowAgentControl() const = 0;
  // View ▸ Agent Autoview (the Mac's autoViewEnabled).
  virtual bool AgentAutoView() const = 0;
  virtual void RequestAgentAutoView(bool enabled) = 0;
  // "New tab behavior = Omnibox": a new tab focuses the address field
  // instead of opening a page (the tab is born on commit).
  virtual bool NewTabOpensOmnibox() const = 0;
  // The sidebar width in DIP — one app-wide value like the layout mode,
  // clamped to the Mac split range (193–500).
  virtual int SidebarWidth() const = 0;
  virtual void RequestSidebarWidth(int dip) = 0;
  // The app-wide resolved theme.
  virtual const PhiPalette& Palette() const = 0;
  // The bookmark bar's two visibility switches (the Mac View menu's
  // "Always Show Bookmark Bar" / "Show Bookmark Bar on New Tab").
  virtual bool AlwaysShowBookmarkBar() const = 0;
  virtual void SetAlwaysShowBookmarkBar(bool enabled) = 0;
  virtual bool BookmarkBarOnNewTab() const = 0;
  virtual void SetBookmarkBarOnNewTab(bool enabled) = 0;
  // The create-Space form's theme defaults. The three are the Settings
  // host's spellings, so one coordinator override serves both.
  virtual std::string ThemeId() const = 0;  // The app-wide theme.
  virtual PhiAppearance Appearance() const = 0;
  virtual int Saturation() const = 0;  // 0..100.
};

// Agent Spaces as pips, watching, and the operating mask over the user's
// own tabs.
class AgentHost {
 public:
  virtual ~AgentHost() = default;
  // Live agent Spaces to show as pips (empty when none). The user watches one
  // by clicking its pip: the agent's hidden window takes the frame of the
  // window the pip was clicked in (`watcher_slot`) and that window is
  // concealed until the watch ends — the Mac's slot.activate, ONE window on
  // screen, not the agent's beside the user's. Clicking the watched pip
  // again ends the watch.
  virtual std::vector<PhiAgentPipView> AgentSpaces() const = 0;
  virtual void RequestWatchAgentSpace(const std::string& task_id,
                                      int64_t watcher_slot) = 0;
  // A user-Space pip clicked while the agent Space is up: the watch ends
  // and the window shows `space_id` (empty: the Space it came from) — in
  // the slot hosting the agent Space this IS the normal Space swap back.
  virtual void RequestLeaveWatchedAgentSpace(const std::string& space_id) = 0;
  // The slot hosting an agent Space for a watch has let its browser go
  // (the swap back landed, or the window is closing): `child` goes back
  // to its own hidden agent shell.
  virtual void OnAgentBrowserReturned(const std::string& task_id,
                                      HWND child) = 0;
  // Watch-mode control pill (C3): ownership transfers ONLY through these —
  // the fog barrier is what makes that true. Finish ends the task.
  virtual void RequestAgentTakeControl(const std::string& task_id) = 0;
  virtual void RequestAgentHandBack(const std::string& task_id) = 0;
  virtual void RequestAgentFinish(const std::string& task_id) = 0;
  // Opens the task's transcript console (the agent pip context menu's
  // "Show Transcript", the Mac showTranscriptAction).
  virtual void RequestShowTranscript(const std::string& task_id) = 0;
  // --- Agent operating mask (the user's own Spaces) -----------------------
  // True while an agent holds `tab_id`; the row wears the mask badge.
  virtual bool IsTabAgentDriven(int tab_id) const = 0;
  // True once the user took `tab_id` back — the badge reads as "you have
  // control" and the browser refuses that tab's drive commands.
  virtual bool IsTabReclaimed(int tab_id) const = 0;
  // "Take control" / "Hand back" on a driven tab.
  virtual void RequestReclaimTab(int tab_id) = 0;
  virtual void RequestReleaseTab(int tab_id) = 0;
};

// Windows as a set: new windows, cross-window drops, tear-off placement,
// the feedback form and the settings window.
class WindowHost {
 public:
  virtual ~WindowHost() = default;
  // File > New Window: a user window in the profile the slot is showing.
  // Goes through the coordinator so the request is token-matched like every
  // other creation (contract rule 3) instead of the legacy NewWindow.
  virtual void RequestNewWindow(int64_t slot) = 0;
  // Surfaces the shell hosting `browser_window_id` (foregrounding it and
  // switching it to that browser's Space when it is hidden) — the search
  // palette's cross-window jump.
  virtual void FocusWindowForBrowser(int64_t browser_window_id) = 0;
  // Opens the settings window (theme, appearance, layout).
  virtual void RequestSettings() = 0;
  // Opens the universal URL Rules editor (Spaces ▸ URL Rules…), centred
  // over `anchor`.
  virtual void RequestUrlRulesEditor(HWND anchor) = 0;
  // Opens the feedback form for `slot`, about `context` (its focused
  // tab) — Help ▸ Report an Issue, the sidebar's Feedback capsule, and
  // Chromium's own feedback command all land here.
  virtual void RequestFeedback(int64_t slot,
                               const PhiFeedbackContext& context) = 0;
  // Cross-window drag (the Mac's window-to-window tab drop): the slot whose
  // shell is under `screen` (topmost wins; 0 = none but `exclude_slot`), and
  // the move itself — the dragged tab (plus its split partner) joins the
  // target window's strip. False leaves the drop to the tear-off path.
  virtual int64_t SlotAtScreenPoint(POINT screen,
                                    int64_t exclude_slot) const = 0;
  virtual bool RequestDropTabOnWindow(int64_t source_slot,
                                      int tab_a,
                                      int tab_b,
                                      int64_t target_slot) = 0;
  // Tear-off placement (the Mac's TabDraggingSession
  // resolvedTearOffWindowFrame): the screen point a tab was dropped at
  // outside every window, noted by the dragging controller just before it
  // asks the framework for the new window, and taken by that window's
  // constructor so it opens AROUND the drop instead of at the saved
  // placement. Stale after a few seconds, in case no window ever came.
  virtual void NoteTearOffDrop(POINT screen) = 0;
  virtual bool TakeTearOffDrop(POINT* screen) = 0;
};

// The five, as the shell receives them at construction. Non-owning; every
// pointer outlives the shell.
struct ShellServices {
  RAW_PTR_EXCLUSION SidebarHost* sidebar = nullptr;
  RAW_PTR_EXCLUSION SpacesHost* spaces = nullptr;
  RAW_PTR_EXCLUSION ChromePrefsHost* prefs = nullptr;
  RAW_PTR_EXCLUSION AgentHost* agent = nullptr;
  RAW_PTR_EXCLUSION WindowHost* window = nullptr;
};

// The Mac sidebar's default and drag range (MainSplitViewController).
inline constexpr int kSidebarWidthDefault = 240;
inline constexpr int kSidebarWidthMin = 193;
inline constexpr int kSidebarWidthMax = 500;

class PhiMenuBar;

// Owns the client-drawn shell window for one browser (the Windows twin of the
// Mac MainBrowserWindowController), using the Option-B model
// (windows-framework-design.md §2.4): the CLIENT owns a top-level window and
// its own painting, and Chromium's browser window is reparented in as a
// WS_CHILD filling the content rect. The client's sidebar is drawn into the
// client's own window (no DirectComposition conflict, unlike the falsified
// overlay approach), while web content renders via DComp inside the reparented
// child's own HWND — two non-overlapping regions, both visible.
// Shell-window app messages posted by the coordinator's keyboard hook —
// the Ctrl+Tab MRU switcher gestures (open/advance on the shell's thread).
enum : UINT {
  kMsgPhiSwitcherNext = WM_APP + 20,
  kMsgPhiSwitcherPrev,
  kMsgPhiSwitcherCommit,
  // Posted when a rebuild notices the active tab crossing into or out of a
  // NATIVE-PAGE route (the bookmark manager, the new-tab page), so the
  // mount happens on its own turn rather than inside the rebuild.
  kMsgPhiSyncNativePages,
  // The coalesced row rebuild (ScheduleRefresh): WM_PAINT and input never
  // rebuild, so the rows rebuild from the window model on this posted turn,
  // the bands whose rows changed are invalidated, and the paint recomposes.
  kMsgPhiRebuildRows,
  // Alt pressed and released on its own: summon the caption band's hidden
  // menu titles (and hide them again on a second Alt), the keyboard route
  // to the bar the pointer reveals by hovering.
  kMsgPhiToggleMenuReveal,
  // A close that arrived while a nested message loop of this window was on
  // the stack (PhiModalDepth() > 0): the handler retries on this posted
  // turn, and keeps retrying by timer until the loop has unwound. See
  // RequestClose.
  kMsgPhiDeferredClose,
};

// The shell window (docs/architecture-review.md §3.5): a window procedure,
// the layout cache, the DPI and fonts, focus routing, and dispatch to the
// components that own the rest. It keeps the name PhiShellWindow while
// the components are extracted one at a time — the rename to PhiShellWindow
// would touch every other file and lands with the last of them.
//
// Each component's needs reach it through a narrow interface implemented
// PRIVATELY here, so none of them widen the shell's public surface.
class PhiShellWindow : public PhiHitSource,
                            private PhiOmniboxShell,
                            private PhiBrowserHostShell,
                            private PhiCaptionShell,
                            private PhiMenuBarShell,
                            private PhiNativePagesShell,
                            private PhiSidebarShell,
                            private PhiSpacesStripShell {
 public:
  // `agent` hosts the window as a HIDDEN agent Space: the shell is created
  // off-screen and off the taskbar (WS_EX_TOOLWINDOW), shown without
  // activation so its content still composites and renders for CDP screen
  // capture, but the user never sees it. Everything else is identical, so a
  // future "watch" gesture can move it on-screen.
  // `incognito` hosts an off-the-record browser: fixed-dark chrome, no
  // Spaces strip / pinned grid / bookmarks (nothing persists), and the
  // window stays OUT of the Space registry so URL routing never lands here.
  // `slot` is this shell's identity; `browser` is the first browser it
  // adopts (the one it was opened with, which backs the default Space).
  // Today the two carry the same number — the types are what keep the slot
  // from ever being handed to the framework as a browser (phi_ids.h).
  // `model` is the coordinator's window model (docs/architecture-review.md
  // §3.3): every row, nav state and split this shell shows is read from
  // it, never parsed from the framework's JSON. Owned by the coordinator,
  // which outlives every controller.
  PhiShellWindow(SlotId slot,
                      BrowserId browser,
                      HWND browser_hwnd,
                      phi_framework::PhiFramework* framework,
                      const ShellServices& services,
                      const PhiWindowModel* model,
                      bool agent = false,
                      bool incognito = false);
  // Virtual because the shell now implements its components' interfaces:
  // the class has virtual functions, and PhiWindowRegistry deletes it
  // through a PhiShellWindow* (its test stand-ins subclass it).
  ~PhiShellWindow() override;

  PhiShellWindow(const PhiShellWindow&) = delete;
  PhiShellWindow& operator=(const PhiShellWindow&) = delete;

  // The slot's identity — what every host interface's `slot` takes.
  // Never a browser: see active_browser_id() for the one the slot shows.
  SlotId slot() const { return slot_; }
  // Upstream's distillability verdict for one of this window's tabs.
  void NoteTabDistillable(TabId tab_id, bool distillable);
  // True when `tab_id` is in the strip this window is currently drawing.
  bool ShowsTab(TabId tab_id) const;
  // Which corners of the content card are rounded right now. The reader panel
  // covers that card and has to wear the same shape: the TOP corners belong
  // to the bar band in the layouts that have one (only performance rounds
  // them), and content fullscreen rounds nothing — the same rule the corner
  // caps are shown by.
  void CardRoundedCorners(bool* top, bool* bottom) const;
  // Puts the rounded-card corner caps back on top. They are sibling windows
  // ABOVE the page (DirectComposition ignores a region on the browser child),
  // so anything else raised over the content card — the reader panel — buries
  // them and squares the card's corners until this runs.
  void RaiseCornerCaps();
  // Reader View for the ACTIVE tab: enter, or leave when the tab already
  // shows the extension's reading surface. The reader button, the View menu
  // item, its shortcut and the page context menu all land here.
  void ToggleReaderView();
  // Back/Forward or an address-bar commit on a tab the reader is covering:
  // leaves the reader and returns true, so the caller skips the navigation
  // (Back) or lets it land on the page underneath (the address bar). False
  // when no reader is up and the command is an ordinary one.
  bool LeaveReaderViewForNavigation();
  // Whether the active tab is in Reader View right now (the item's tick, and
  // what makes the toggle mean "leave").
  bool ActiveTabShowsReader() const;
  // Whether the View menu's Reader View item is reachable. DELIBERATELY not
  // the button's offerability test: the menu and its shortcut offer the
  // reader on any real page, so a page the probe misjudges still opens
  // through them (the Mac's rule). Only the native new-tab page is refused —
  // it has no web contents to extract from.
  bool ActiveTabCanUseReader() const;
  // The URL gate both of the above sit behind — the Mac's
  // ReaderExtractionService.canOfferReader.
  static bool CanOfferReaderForUrl(const std::string& url);
  bool is_agent() const { return agent_; }
  bool is_incognito() const { return incognito_; }
  // For satellite surfaces (search palette, popovers) that act through the
  // same seams the controller does — each takes the ONE interface its own
  // job needs, never the set (docs/architecture-review.md §3.5).
  SidebarHost* sidebar_host() const { return services_.sidebar; }
  SpacesHost* spaces_host() const { return services_.spaces; }
  ChromePrefsHost* prefs_host() const { return services_.prefs; }
  AgentHost* agent_host() const { return services_.agent; }
  WindowHost* window_host() const { return services_.window; }
  phi_framework::PhiFramework* framework() const { return framework_; }
  // The window model the rows are built from — what a satellite reads
  // instead of parsing the framework's tab JSON.
  const PhiWindowModel* model() const { return model_; }
  // The feedback form about this window's focused tab (the Mac's
  // showFeedbackWindow).
  void OpenFeedback();
  PhiPalette CurrentPalette() const;  // Ramped while a switch is in flight.
  // Tab ids most-recently-used first — the Ctrl+Tab switcher's order.
  const std::vector<TabId>& TabMruOrder() const;
  // The content card's client-coordinate rect (the group overview covers it).
  RECT ContentRect();
  // The create-Space form — the Mac's CreateSpacePanel hosted in the
  // sidebar (SidebarViewController.showCreateSpaceOverlay). Every "New
  // Space" entry point (the strip's "+", the switcher menus, the menu bar)
  // routes through ShowCreateSpaceForm; where a docked vertical sidebar
  // cannot host the form it creates at once instead (the Mac opens a
  // standalone window there).
  void ShowCreateSpaceForm();
  void CommitCreateSpaceForm(const PhiNewSpaceSpec& spec);
  void CancelCreateSpaceForm();
  // The form's live theme preview: the chrome paints with this palette
  // until the created Space presents, or the form is cancelled.
  void PreviewCreateSpaceTheme(const std::string& theme_id, int saturation);
  // The form's icon, mirrored on the strip's dashed placeholder pip.
  void SetCreateSpaceIcon(const std::string& storage_value);
  void OnSpaceFormProfileCreated(const std::string& profile_id);
  bool creating_space() const { return spaces_->creating(); }
  // Whether the address EDIT — a real child window — must stay hidden:
  // fullscreen content, a collapsed performance sidebar, or the create-Space
  // form covering the sidebar's address row.
  bool OmniboxFieldHidden() const;
  // Surfaces (visible=true) or parks off-screen (visible=false) an agent
  // window so the user can watch it live. No-op on a non-agent window.
  // `frame` (screen px) is the watching window's frame, which the agent
  // window takes over exactly, and `owner` that window: the agent window
  // becomes its OWNED window for the watch — kept above it by the system,
  // minimized with it, no taskbar button of its own — so the user window
  // never has to be hidden and nothing blinks when the watch ends. Null
  // frame centres it on the work area, unowned.
  void SetAgentVisible(bool visible,
                       const RECT* frame = nullptr,
                       HWND owner = nullptr);
  // The USER window's side of a watch. It stays shown and painted under
  // the agent window (hiding it made the desktop flash through when the
  // watch ended, then a cold repaint); `concealed=false` only takes the
  // focus back. No-op on an agent window.
  void SetConcealedForWatch(bool concealed);
  // Whether the agent holds this (agent) window: the owner tells it on
  // every ownership change. While it does, the window refuses what the
  // Mac's refuses under agent ownership — New Tab, closing tabs, the tab
  // menus, Back/Forward — so a stray keystroke never moves the agent's
  // page; the pill's Take control is the one way in.
  void SetAgentOwned(bool agent_owned);
  // The agent holds the page on screen: this is an agent window, or a user
  // window whose ACTIVE Space is the agent Space it hosts for a watch.
  bool AgentHoldsInput() const;

  // --- An agent Space hosted in a USER slot for a watch ---------------------
  // The Mac's slot.activate for an agent Space: its browser is attached to
  // THIS slot under `space_id` and shown with the slot's own Space swap
  // (band slide, pill slide, content reveal), exactly as any Space. The
  // agent shell keeps the browser's record and gets the child back when
  // the watch ends (LeaveHostedAgentSpace: the swap back to a user Space,
  // then, once it lands, OnAgentBrowserReturned on the agent host).
  void HostAgentSpace(const std::string& space_id,
                      BrowserId browser,
                      HWND browser_hwnd,
                      const std::string& task_id);
  bool HostsAgentSpace() const { return !hosted_agent_task_.empty(); }
  const std::string& hosted_agent_task() const { return hosted_agent_task_; }
  // `space_id` empty: the user Space the slot showed before.
  void LeaveHostedAgentSpace(const std::string& space_id);
  // The agent shell's two sides of the hand-over.
  HWND ReleaseAgentBrowserForHosting();
  void ReadoptAgentBrowser(HWND browser_hwnd);

  // Click on a pin or a sidebar bookmark row (cell click, context-menu
  // Open, or the pinned.open verb): focus the bound live tab if it still
  // exists, otherwise open the row's url as a new bound tab — the Mac's
  // BrowserState.openOrFocusPinnedTab / bookmarkGuidBacking model.
  void OpenOrFocusPinned(const std::string& guid, const std::string& url);

  // The pointered link target changed in `browser_id`'s page (empty url =
  // pointer left the link). Shows/hides the status bubble when that browser
  // is the one this shell displays.
  void OnTargetURLChanged(BrowserId browser_id, const std::wstring& url);

  // Autocomplete results for `browser_id`'s running omnibox query — feeds
  // the address field's suggestion dropdown (the Mac OmniBox panel).
  void OnOmniboxResults(BrowserId browser_id, const std::string& json);

  // `browser_id`'s last tab closed but the browser SURVIVES in placeholder
  // mode (entered) or a new tab ended that mode. The content child hides
  // while placeholding — the dead tab's final frame would linger otherwise
  // — and a field commit births the first tab.
  void OnPlaceholderModeChanged(BrowserId browser_id, bool entered);
  // A tab's content (video/element) entered or left fullscreen: the hosted
  // child resizes itself to the MONITOR, so the shell must go borderless
  // fullscreen for the duration or the render is clipped to the window.
  void OnContentFullscreenChanged(BrowserId browser_id, bool fullscreen);
  // Chromium moved a pane of this window's split (the framework's
  // OnContentBoundsChanged): the native pages over the panes follow.
  void OnContentBoundsChanged(BrowserId browser_id);

  // THE refresh entry point (docs/architecture-review.md §3.1 principle 3):
  // marks the rows dirty and posts ONE coalesced rebuild (kMsgPhiRebuildRows)
  // for this turn's burst of changes — the model's push, a pref, a Space
  // switch. The rebuild runs on that posted turn, invalidates the bands whose
  // rows changed, and the paint recomposes. Nothing rebuilds inline.
  void ScheduleRefresh();
  // A verb that READS rows (the agent dialect's window.layout, a capture)
  // right after a change: runs the pending rebuild now, on the caller's
  // turn, so it sees the rows the next paint will. A no-op when nothing is
  // pending.
  void FlushPendingRebuild();
  // Rebuilds the Space chips row from the owner's full Space list.
  void RebuildChips();
  // Re-derives the geometry and repositions the browser child + corner caps
  // (public so the owner can push app-wide layout changes, e.g. the sidebar
  // width, into every window).
  void LayoutContents();

  // --- Spaces (client-owned, as on Mac) ------------------------------------
  // A slot shows ONE Space at a time. Each Space has its own Browser; the
  // controller swaps which browser child is visible in the shell window.

  // Adopts an additional browser as `space_id`'s content for this slot and
  // shows it (hiding the previous). Called when the framework reports a
  // browser created for this slot.
  void AttachSpaceBrowser(const std::string& space_id,
                          BrowserId browser,
                          HWND browser_hwnd);
  // Shows the Space's browser, hiding the current one. False if unknown.
  bool ShowSpace(const std::string& space_id);
  bool HasSpace(const std::string& space_id) const;
  // Recreates the menu bar from the CURRENT language. The bar caches every
  // title and item string as wide text inside its HMENUs, so a language
  // change cannot be repainted — the menus have to be built again, and the
  // caption band's measured title rects re-measured with them.
  void RebuildMenuBar();
  // The Space whose browser is `browser_id` in this slot, or empty — the
  // reverse of BrowserIdForSpace, for a skill payload that names a WINDOW
  // and wants the Space it shows.
  std::string SpaceForBrowser(BrowserId browser_id) const {
    return browsers_->spaces().SpaceOf(browser_id);
  }
  const std::string& active_space() const {
    return browsers_->spaces().active_space();
  }
  // The browser this slot is SHOWING (for tab/nav commands): the active one,
  // else the first Space browser still attached, else invalid — a slot
  // between its last browser going and its own teardown, or one whose
  // adoption failed. NEVER the slot number: that names the browser the slot
  // was OPENED with, which may be long closed (docs/architecture-review.md
  // §3.5). Callers skip the framework call when this is invalid.
  BrowserId active_browser_id() const { return browsers_->spaces().active(); }
  // Legacy spelling of active_browser_id() for the coordinator's int64_t
  // paths; 0 when no browser is attached. Goes with the phase 2 signatures.
  int64_t active_browser() const { return active_browser_id().value; }
  // The browser this slot holds for `space_id`, or invalid — feeds the page
  // context menu's Space list (its current-Space exclusion).
  BrowserId BrowserIdForSpace(const std::string& space_id) const {
    return browsers_->spaces().ForSpace(space_id);
  }
  // Spaces this slot has a browser for, in creation order.
  const std::vector<std::string>& space_order() const {
    return browsers_->spaces().order();
  }
  // Drops a browser that went away; returns true if the slot still has one.
  bool ForgetBrowser(BrowserId browser);
  // Closes the browser this slot holds for `space_id` (a Space being
  // deleted), switching to another open Space first so the window never
  // shows a dead one. No-op when the Space is not open here. If it was the
  // slot's ONLY browser, the slot closes with it.
  void CloseSpaceBrowser(const std::string& space_id);
  // Starts closing the whole slot (its Space browsers, one per Space). MAY
  // DESTROY `this` before returning — the last browser going away tears the
  // slot down — so touch nothing on this object afterwards.
  //
  // INVARIANT: the controller is never destroyed while one of its own nested
  // message loops (a context menu, a prompt, the icon picker — anything
  // under a PhiModalScope) is on the stack. Both close entry points check
  // PhiModalDepth() and, while it is non-zero, park the request and post
  // kMsgPhiDeferredClose to the shell instead; the handler re-arms itself
  // (kDeferredCloseTimer) until the loop has unwound and only then runs the
  // close for real. Callers therefore may not assume the close happened
  // synchronously — only that it will.
  void RequestClose();
  // Gives the currently shown browser Win32 focus and Chromium activation.
  void FocusActiveBrowser();
  // --- Native new-tab page (phi_new_tab_page.cc) ---------------------------
  // The page's omnibox joins this window's ONE omnibox session: `edit` gets
  // the address field's subclass (Enter / Escape / arrows, the suggestion
  // dropdown) and reports its EN_CHANGE through OnOmniboxFieldChanged.
  void AttachOmniboxField(HWND edit);
  void OnOmniboxFieldChanged(HWND field);
  // A Space URL rule re-homed a navigation that began on this window's new
  // tab (the framework's OnRefreshNewTab): the native page returns to its
  // clean state, as the Mac's resetToInitialState does.
  void ResetNativeNtp();
  // A pane's native new-tab page was clicked (or its field took focus):
  // that pane's tab becomes the active one, as a click on the pane's web
  // content would make it — the omnibox session commits into the active
  // tab, so it must be the pane the user is typing in.
  void ActivatePaneTab(int tab_id);

  // The shell window's menu bar (null before the shell exists). The control
  // socket reads and clicks it through here.
  PhiMenuBar* menu_bar() {
    return caption_band_ ? caption_band_->menu_bar() : nullptr;
  }

  // Re-lays the chrome for `mode` and repaints. Cheap: all geometry is derived
  // from the client rect on demand, so nothing is cached across the switch.
  void ApplyLayoutMode(PhiLayoutMode mode);
  // Repaints for a palette the host just changed.
  void ApplyPalette();
  // The content region (client coords, physical pixels) the browser child
  // occupies.
  RECT content_rect();
  // The whole page CARD: the content region, raised to include the top bar
  // where one exists, because that bar is the card's first band (the Mac's
  // WebContentHeader is the first item inside `splitViewContainer`, and the
  // card's rounded top corners are the bar's). What a full-pane overlay has
  // to cover to BE the card rather than sit inside it. Comfortable has no
  // card — its page runs edge to edge under a toolbar that belongs to the
  // window — so there it is just the content.
  RECT card_rect();
  // The DPI the chrome is currently laid out for (96 = 100%).
  UINT dpi() const { return dpi_; }
  // Chrome geometry, published so tools and tests can locate what they are
  // asserting about instead of mirroring constants that then drift.
  RECT url_field_rect();
  // The reader button's slot inside the address capsule, empty when the
  // reader is not being offered for the active tab. Published for the same
  // reason as the field: a test asserting the button appeared should read the
  // layout, not count pixels.
  RECT reader_button_rect();
  // The two signals behind that offer, separately — a test needs to know
  // WHICH one lit the button (upstream's distiller, or the reader
  // extension's own in-page probe and site rules).
  bool ActiveTabExtensionOfferable() const;
  bool ActiveTabDistillable() const;
  int tabs_top();
  int tab_row_height() const;
  // The placed sidebar rows (keys + client rects) for the agent dialect's
  // window.layout, so a box probe can aim a real drag without pixels.
  base::ListValue LayoutRowsJson() const;
  // The gap between rows (the Mac's 2pt intercell spacing); row pitch is
  // tab_row_height() + tab_row_gap().
  int tab_row_gap() const;
  int sidebar_width() const;
  // The caption band's height — the row holding the menu titles and the
  // caption buttons. Everything else starts below it.
  int title_bar_height() const;
  // The tinted margin around the content card.
  int content_margin() const;
  // Client origin minus window origin. PrintWindow(PW_CLIENTONLY) renders from
  // the WINDOW origin, so a capture is offset by exactly this — a caller
  // comparing against client coordinates has to add it.
  POINT frame_offset() const;

  // Shell window geometry (client-owned; no framework round-trip).
  bool GetBounds(int* x, int* y, int* w, int* h) const;
  bool SetBounds(int x, int y, int w, int h);
  HWND shell_hwnd() const { return shell_hwnd_; }

  // PhiHitSource — THE rect list assistive technology reads (phi_window.h).
  // The sidebar publishes its rows into it on every rebuild (a stable key
  // per row, the row's title as the name, and a role of list item, button
  // or group), and the shell answers WM_GETOBJECT from it. The shell is
  // hand-registered rather than a PhiWindow, which is exactly why the
  // provider takes this interface instead of a window.
  HWND hwnd() const override;
  const std::vector<PhiHit>& hits() const override;
  uint32_t hits_version() const override;
  int HitTest(POINT client_pt) const override;
  bool InvokeHit(int index) override;

  // Window procedure for the client shell window.
  static LRESULT CALLBACK ShellWndProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wparam,
                                       LPARAM lparam);

 private:
  // Every constant in this class is a DESIGN UNIT — a pixel at 100% scaling.
  // Win32 places windows and paints in PHYSICAL pixels, and the client is
  // per-monitor DPI aware (it inherits Chrome's manifest), so nothing reaches
  // the screen without going through here first. Without it the whole chrome
  // renders 20% small at 125% and half-size at 200%, while the system font —
  // which IS scaled — outgrows the boxes drawn for it.
  int Scaled(int design_units) const;
  RECT Scaled(const RECT& design_units) const;
  // Rebuilds `ui_font_` for `dpi_`. Idempotent for an unchanged DPI.
  void EnsureFont();

  // Every rect the chrome needs, for one layout mode — ShellLayout
  // (phi_shell_layout.h) converted to RECTs. ComputeLayout() answers from a
  // cache keyed by ShellLayoutInputs, so the eight reads per pointer move
  // cost a struct compare; the geometry itself is the pure, host-tested
  // ComputeShellLayout.
  struct Layout {
    PhiLayoutMode mode = PhiLayoutMode::kPerformance;
    RECT client{};
    // The caption band: menu titles leading, then drag space, then the three
    // caption buttons. The OS caption is gone (WM_NCCALCSIZE) — this row IS
    // the title bar, so the menu shares it with minimize / maximize / close.
    RECT title_bar{};
    RECT caption_min{}, caption_max{}, caption_close{};
    // The x the menu titles may not cross: the caption band's trailing
    // control cluster (back / forward / reload and search-tabs in the
    // vertical layouts), or the caption buttons where there is no cluster.
    // A narrow window drops the titles that would collide rather than
    // painting them under the controls.
    int band_limit = 0;
    RECT sidebar{};  // Empty in Comfortable.
    RECT top_bar{};  // The band above the content; empty in Performance.
    RECT content{};  // Where the reparented browser child goes.
    // The bookmark bar band, carved off the content's top when visible
    // (the Mac's 32pt BookmarkBar under the header).
    RECT bookmark_bar{};
    // Navigation. In the vertical layouts back / forward / reload and
    // search-tabs ride the caption band's trailing cluster; comfortable and
    // balanced keep the trio on the top bar's leading edge.
    RECT search_tabs{};
    // The hairline closing the band's trailing cluster, in the gap between
    // search-tabs and the caption buttons. Every layout has one.
    RECT band_divider{};
    RECT back{}, forward{}, reload{};
    RECT spaces_strip{};   // The band the Space chips live in.
    RECT url_field{};
    RECT favorites{};      // Pinned-tab band; empty until the feature lands.
    // The 16pt separator row between the bookmark outline and "+ New Tab"
    // (the Mac's SeparatorItem); empty without bookmarks.
    RECT bookmark_separator{};
    RECT new_tab_row{};    // The sidebar's "+ New Tab" row.
    RECT bottom_bar{};
    // Bottom-bar buttons, the Mac's order: downloads and memory leading, the
    // Feedback and Chat capsules trailing. Settings has NO bottom-bar button —
    // it opens from the menu bar, as on Mac. All placeholders except through
    // the menu.
    RECT downloads{}, memory{}, feedback{}, chat{};
    RECT extensions{};     // The puzzle button (in-field / bottom bar / header).
    // Comfortable's overflow menu (Chrome's ⋮): the toolbar's trailing
    // button. It is that layout's ONLY way to the menu bar — the caption
    // band has no titles there — and the home of the Feedback, Chat and
    // memory entries the other layouts keep in bands. Empty elsewhere.
    RECT overflow{};
    // Comfortable's Phi button: the logo alone at the caption band's leading
    // edge, the corner the Mac's menu bar gives the Phi menu and where the
    // vertical layouts start their titles. It drops the bar's FIRST popup;
    // the rest of the bar stays behind the overflow. Empty in the other
    // layouts, which draw the real "Phi" title there instead.
    RECT phi_menu{};
    // The globe leading comfortable's omnibox pill, where Chrome shows the
    // site chip. Empty in the other layouts, whose fields carry no icon.
    RECT page_icon{};
    // SideAddressBar's in-field copy-link button (performance layout only) —
    // the leading half of the field's trailing stack, the puzzle its other.
    RECT copy_link{};
    // Reader View's entry, leading the copy-link button. Empty unless the
    // ACTIVE tab is distillable — the Mac shows the reader button only where
    // Reader View has something to offer.
    RECT reader{};
    // The pinned-extension icon strip leading the copy-link button inside
    // the capsule. Empty when nothing is pinned OR the icons would squeeze
    // the text below the Mac's 84pt minimum (they hide, as on Mac).
    RECT field_ext_strip{};
    RECT add_space{};      // The strip's trailing "+" chip.
    // The pip's edge in points, the trailing slot's with it: the vertical
    // layouts fill the strip band with it, comfortable keeps the Mac's
    // compact caption pip.
    int chip_size = 0;
    int chips_left = 0, chips_top = 0;
    int chips_limit = 0;  // Right edge chips may not cross.
    int tabs_left = 0, tabs_top = 0;
    int tabs_limit = 0;   // Bottom (vertical) or right (horizontal) edge.
    bool horizontal_tabs = false;
  };
  Layout ComputeLayout();
  // The cache behind ComputeLayout: the inputs the last layout was derived
  // from and that layout. Re-derived whenever the inputs differ — a resize,
  // a DPI move, a mode/pref change, a count from the rebuild, the floating
  // panel's override — so no explicit invalidation can be forgotten.
  ShellLayoutInputs CurrentLayoutInputs() const;
  static Layout ToLayout(const ShellLayout& shell);
  ShellLayoutInputs layout_inputs_;
  Layout layout_cache_;
  bool layout_cache_valid_ = false;
  // The two inputs that cost host round-trips (BookmarkBarVisible reads
  // two prefs; ActiveTabReaderOfferable asks the reader bridge), captured
  // by RefreshLayoutInputs at the points they can change — the rebuild,
  // LayoutContents, a distillability report — rather than per layout read.
  bool layout_bookmark_bar_ = false;
  bool layout_reader_ = false;
  void RefreshLayoutInputs();

  // `include_child_controls` draws what child windows would draw themselves —
  // set only on the WM_PRINTCLIENT (PrintWindow) path, which renders the
  // parent alone. In WM_PAINT the URL field's EDIT child paints that region.
  void PaintChrome(HDC dc, const Layout& layout, bool include_child_controls);
  // Handles a click in the chrome (tab rows, close boxes, buttons, toolbar).
  void OnChromeClick(int x, int y);
  // Middle-click in the chrome: closes the tab row under the pointer (the
  // Mac tab list's otherMouseDown). Pins/bookmarks deliberately ignore it.
  void OnChromeMiddleClick(int x, int y);
  // Right-click in the chrome. On a Space chip this raises the Mac's Space
  // menu (activate / rename / change icon / set theme / delete); elsewhere it
  // does nothing, as on Mac.
  void OnChromeContextMenu(int x, int y);

  // --- Context menus (phi_shell_menus.h) ----------------------------------
  // A menu's nested loop dispatches whatever is queued, kMsgPhiRebuildRows
  // included, so tabs_ / pinned_ / bookmarks_ / group_headers_ / chips_ can
  // be cleared and refilled while one is up. Nothing below takes a reference
  // into them: the hit-test copies the row into a MenuRow, a pure builder
  // turns that into a spec, and the action comes back by key
  // (docs/architecture-review.md §3.1 principle 4, lane 1 §3.2).
  struct MenuRow {
    SidebarKey key;
    TabId id;
    TabId b_id;
    std::string guid;      // Store rows (pins, bookmarks).
    std::string split_id;  // Tab rows: the split this row already belongs to.
    std::string group_id;
    std::wstring title;
    std::string url;    // UTF-8, as the framework and the store want it.
    std::string url_b;  // Pane B of a split row / cell.
    std::string layout;  // A split's remembered divider axis.
    std::string icon;    // A bookmark folder's catalog symbol.
    bool merged = false;
    bool folder = false;
    bool open = false;  // A pinned cell with a live bound tab.
    RECT bounds{};      // Where a popover the action opens hangs off.
  };
  // Screen placement for a popup, resolved from a CLIENT point.
  PhiPopupMenuRequest PopupAt(POINT client_pt, UINT align) const;
  // The dynamic rows the builders take: every Space but this slot's (for the
  // transfer submenus), the full list (for the switchers), this Space's
  // bookmark folders in outline order, and the tab groups other than one.
  std::vector<PhiMenuChoice> SpaceMenuChoices() const;
  std::vector<PhiMenuChoice> SpaceSwitcherChoices() const;
  std::vector<PhiMenuChoice> BookmarkFolderChoices() const;
  std::vector<PhiMenuChoice> GroupMenuChoices(const std::string& exclude) const;
  // The store's bookmark outline as a value, for the bar's dropdown.
  std::vector<PhiMenuBookmarkNode> BookmarkTreeSnapshot(
      const std::string& parent_guid,
      int depth) const;
  // One menu each, from a snapshot. The by-value parameters are the point:
  // the copy happens at the call, before the loop.
  void ShowGroupHeaderMenu(POINT pt,
                           std::string group_id,
                           std::wstring title,
                           COLORREF color);
  void ShowTabRowMenu(POINT pt, const MenuRow& row);
  void ShowPinnedCellMenu(POINT pt, const MenuRow& row);
  void ShowBookmarkRowMenu(POINT pt, const MenuRow& row);
  void ShowTabAreaMenu(POINT pt);
  void ShowAgentChipMenu(POINT pt, std::string task);
  // The compact pip's switcher (with_new_space false) and the overflow "…"
  // button's (true, which adds the two creation rows).
  void ShowSpaceSwitcherMenu(POINT pt, bool with_new_space);

  // Collapses/restores the sidebar (View > Toggle Sidebar, Ctrl+S). The
  // content card widens to the left margin; the margin becomes the hover
  // trigger for the floating sidebar. No-op in the comfortable layout.
  void ToggleSidebar();
  // The New Tab entry points (Ctrl+T, the sidebar row, the menu): honors
  // the "New tab behavior" setting — page mode opens the NTP, omnibox mode
  // focuses the address field and the tab is born on commit.
  void StartNewTab();
  // Opens/closes the downloads popover, anchored to the sidebar's download
  // button — the Window menu's and Ctrl+J's path to the button's toggle.
  void ToggleDownloadsPopover();
  // Re-reads navigation state (url/title/canGoBack/…) for the toolbar.
  void RebuildNavState();

  // The ACTIVE tab, as the sidebar's rebuild resolved it (surfaces
  // redirected to the page they cover), and its URL. The omnibox, the
  // reader affordance, the native-page route and the feedback context all
  // read the shell's spelling of the sidebar's answer.
  TabId active_tab_id() const;
  const std::string& active_tab_url() const;

  // The shell's UI font at `ui_font_dpi_`, used both for the address field and
  // for every label PaintChrome draws, so text and boxes scale together.
  HFONT ui_font_ = nullptr;
  UINT ui_font_dpi_ = 0;
  // Space icons are emoji, which the UI font has no glyphs for; Segoe UI Emoji
  // does. GDI renders it MONOCHROME — colour emoji need DirectWrite, which is
  // a much larger change than the icons are worth right now.
  HFONT emoji_font_ = nullptr;
  UINT emoji_font_dpi_ = 0;
  // Segoe MDL2 Assets for the caption glyphs (minimize / maximize / restore /
  // close) — the same glyphs the OS buttons use.
  HFONT caption_font_ = nullptr;
  UINT caption_font_dpi_ = 0;
  // MDL2 at the emoji size, for Space-pip catalog glyphs.
  HFONT chip_icon_font_ = nullptr;
  // The same two faces at the vertical layouts' larger pip size. Built with
  // the caption font, so `caption_font_dpi_` covers them.
  HFONT pip_icon_font_ = nullptr;
  HFONT pip_emoji_font_ = nullptr;
  // The UI face at the strip's text size, for the big pip's initial-letter
  // stand-in and the trailing slot's "+" / "…".
  HFONT pip_text_font_ = nullptr;
  // 11pt labels — the bottom bar's Feedback/Chat capsules (the Mac's 11pt).
  HFONT small_font_ = nullptr;
  // The row face at semibold — bookmark folder titles (the Mac's .medium)
  // and tab-group headers (.semibold). Built with ui_font_.
  HFONT ui_semibold_font_ = nullptr;
  // The caption band's Phi mark when the ICO's 256px entry cannot be read
  // (see PhiLogoBgra256): loaded from the ICO at the pixel size it draws,
  // reloaded when a DPI change moves that size, destroyed with the shell.
  HICON logo_icon_ = nullptr;
  int logo_icon_size_ = 0;
  UINT dpi_ = 96;


  // Model freshness: rebuilds happen on the posted turn ScheduleRefresh
  // asks for, never per paint, never from input. A capture verb that needs
  // current rows calls FlushPendingRebuild first.
  bool rows_dirty_ = true;
  bool rebuild_posted_ = false;  // A kMsgPhiRebuildRows is in the queue.
  // Cached paint resources, recreated on resize / palette change instead of
  // per WM_PAINT: the full-window backbuffer bitmap and the ground-gradient
  // band brushes.
  HBITMAP back_buffer_ = nullptr;
  SIZE back_buffer_size_{};
  std::vector<HBRUSH> band_brushes_;
  COLORREF band_top_ = 0;
  COLORREF band_bottom_ = 0;
  // The slot palette's window ground — what uncovered backbuffer pixels show.
  COLORREF BackgroundFill() const;


  // Sidebar edge resize (the Mac's split drag): a 4-DIP grab band in the
  // sidebar's trailing gutter; live while dragging, persisted on release.
  int sidebar_dip_ = kSidebarWidthDefault;
  bool sidebar_resizing_ = false;
  bool InSidebarResizeBand(POINT client_pt);


  // Hover feedback (the Mac leans on hover: row washes, close affordances).
  // Which chrome element the cursor rests on, as element-kind + position in
  // its vector; repaints fire only when it changes.
  // kNav: the toolbar's back/forward/reload/search buttons — hover_index_
  // is 0..3 in that order.
  // kAddSpace: the Spaces strip's trailing slot — the "+" or, on overflow,
  // the "…" switcher standing in for it.
  using HoverKind = PhiChromeHover;
  HoverKind hover_kind_ = HoverKind::kNone;
  int hover_index_ = -1;
  void OnChromeMouseMove(POINT client_pt);
  void ClearHover();
  // The client rect a hovered element's wash covers, empty for none — so a
  // hover change invalidates the old and new element, not the whole shell
  // (lane 3 §7 item 3).
  RECT HoverRect(HoverKind kind, int index);
  void SetHover(HoverKind kind, int index);
  // The chrome buttons a kNav hover indexes, in PaintChrome's order.
  std::array<RECT, 14> NavHoverTable(const Layout& layout) const;

  // The hovered row's page-snapshot card (the Mac's TabPreviewController).
  // Unlike the Space card above it floats in its own popup, so it can hang
  // over the Chromium child window.
  std::unique_ptr<PhiTabPreview> tab_preview_;
  // --- The client-drawn pages, as a component (phi_native_pages_host.h) ----
  // The bookmark manager and the native new-tab page, mounted over the
  // content card by the ACTIVE tab's URL (docs/architecture-review.md §3.5,
  // lane 1 cluster M). Nothing else creates, shows or hides either one.
  std::unique_ptr<PhiNativePagesHost> native_pages_;
  void UpdateTabPreview();  // Re-aims the card at the hovered row.

  // Whether to offer the reader control for the active tab: upstream's
  // distiller verdict OR the reader extension's own probe — either alone is
  // incomplete, since the distiller misses pages a site rule rescues and the
  // probe misses tabs whose content script never ran — and always while the
  // reader is already up, which is the way back out of it.
  bool ActiveTabReaderOfferable() const;
  // True when the split holding `tab_id` stacks its panes (the wire's
  // "horizontal"); false for side-by-side and for a tab in no split.
  bool SplitIsStacked(TabId tab_id) const;
  // The framework's CURRENT state for `browser`, pulled typed — for the few
  // gesture paths that just mutated the strip and act on the result inside
  // the same turn, before the coordinator's model has heard the push. Never
  // for rows or paint: those read model_.
  bool PullSnapshot(BrowserId browser,
                    phi_framework::WindowSnapshot* out) const;
  // The bookmark bar (the Mac's 32pt HorizontalBar/BookmarkBar): root-level
  // rows as favicon+title pills, the rest behind a trailing "›" menu.
  // Items are laid out by the paint pass and hit-tested from the same list.
  struct BarItem {
    RECT bounds{};
    int bookmark = -1;    // Index into bookmarks_ (a depth-0 row).
    bool overflow = false;
  };
  std::vector<BarItem> bar_items_;
  bool BookmarkBarVisible();
  void PaintBookmarkBar(HDC dc, const Layout& layout,
                        const PhiPalette& palette);
  // A dropdown of bookmark rows under `anchor`: a bar folder's children
  // (parent_guid set) or the root's overflow (skip_first shown pills).
  void ShowBookmarkBarMenu(RECT anchor,
                           std::string parent_guid,
                           size_t skip_first);
  // The tab whose native new-tab page blanked on an omnibox commit; the
  // controls return once another tab is active or that load has ended.
  TabId ntp_commit_tab_id_;
  // Counted before the layout is derived, because the bands they occupy push
  // everything below them down.
  int pinned_row_count_ = 0;
  int bookmark_row_count_ = 0;

  // The page changes AFTER the band slide completes (the Mac reveals the
  // target on completion): the entering child stays hidden and the leaving
  // one visible until then.
  HWND swap_reveal_ = nullptr;   // Shown when the slide lands.
  HWND swap_conceal_ = nullptr;  // Hidden when the slide lands.
  void RevealSwappedContent();
  void ComposeFrame(HDC mem);  // Chrome + drag indicator.

  // Tabs upstream's distiller says carry an article. Positive-only reports,
  // so entries are added and cleared when the tab navigates away.
  std::set<TabId> distillable_tabs_;
  // Pinned-extension icons inside the address capsule (SideAddressBar's
  // right-stack lead): the pinned set with resolved 16px icons, refreshed
  // with the tab list and on OnExtensionsChanged.
  struct FieldExtension {
    std::string id;
    std::vector<unsigned char> icon;  // Premultiplied BGRA; may be empty.
    int icon_w = 0;
    int icon_h = 0;
  };
  std::vector<FieldExtension> field_extensions_;
  // The count ComputeLayout carves capsule slots for (it cannot call the
  // framework itself).
  int field_ext_count_ = 0;

 public:
  // Re-reads the pinned set + icons (delegate's OnExtensionsChanged, and
  // every tab-list rebuild).
  void RefreshFieldExtensions();

 private:
  // Slot `i` of the in-field icon strip, or an empty rect without one.
  RECT FieldExtRect(const Layout& layout, size_t i) const;

  // The slot's identity. Not a browser: the browsers this slot holds live
  // in space_browsers_ / browser_hwnds_, and "the original browser" is
  // simply whichever of them is still attached.
  const SlotId slot_;
  // --- The content area, as a component (phi_browser_host.h) ---------------
  // Chromium's HWND adopted as a child, the browser this slot holds for each
  // Space, placeholder and content-fullscreen mode, and the overlays that
  // ride above the page (docs/architecture-review.md §3.5, lane 1 clusters E
  // and G). It owns BrowserId -> HWND; SetParent and subclassing happen
  // there and nowhere else.
  std::unique_ptr<PhiBrowserHost> browsers_;
  HWND shell_hwnd_ = nullptr;  // Our client-owned top-level.
  // Set at the top of the destructor: the shell's DestroyWindow still
  // delivers WM_ACTIVATE / WM_SIZE / WM_CAPTURECHANGED to ShellWndProc on
  // the way down, and those handlers must not lay out, refresh or call the
  // host for an object whose destructor body is executing.
  bool destroying_ = false;
  // Close requests parked by RequestClose / CloseSpaceBrowser while a nested
  // loop was up (see the RequestClose invariant); drained by
  // RunDeferredClose on the posted kMsgPhiDeferredClose turn.
  bool deferred_slot_close_ = false;
  std::vector<std::string> deferred_space_closes_;
  void PostDeferredClose();
  void RunDeferredClose();
  // The caption band (phi_caption_band.h): the row that replaced the OS
  // caption. It owns the menu-bar host, the measured title rects, the
  // reveal state machine, the caption buttons and the WM_NC* frame; the
  // shell forwards it messages and asks it to paint its rect.
  std::unique_ptr<PhiCaptionBand> caption_band_;
  // --- The address field, as a component (phi_omnibox_session.h) -----------
  // The single owner of the EDIT, its subclass, the suggestion dropdown, the
  // results and the navigation state (docs/architecture-review.md §3.5,
  // lane 1 cluster F). The shell forwards the field's messages here and
  // reads back what it paints; nothing else touches any of it.
  //
  // NOT reset in the destructor body: the EDIT dies with the shell window a
  // few lines further down and its last messages must still find a live
  // session (they take the OmniboxShellDestroying path).
  std::unique_ptr<PhiOmniboxSession> omnibox_;
  // --- PhiCaptionShell: what the caption band asks of its shell ------------
  HWND CaptionShellWindow() const override;
  int CaptionScaled(int design_units) const override;
  UINT CaptionDpi() const override;
  HFONT CaptionTextFont() override;
  HFONT CaptionGlyphFont() const override;
  PhiPalette CaptionPalette() const override;
  PhiCaptionRects CaptionRects() override;
  bool CaptionStripTakesPoint(POINT client_pt) override;
  bool CaptionPhiMenuHovered() const override;
  void CaptionDrawLogo(HDC dc, int x, int y, int size) override;
  void CaptionClearShellHover() override;
  bool CaptionContentFullscreen() const override;
  PhiMenuBarShell* CaptionMenuServices() override;

  // --- PhiMenuBarShell: what the menu bar asks of its shell ----------------
  // This interface RETIRED `friend class PhiMenuBar` and the six const
  // reference views the shell used to publish so the bar's by-name reaches
  // kept compiling (lane 1 §2.1 item 3).
  HWND MenuShellWindow() const override;
  SlotId MenuSlot() const override;
  bool MenuIncognito() const override;
  SidebarHost* MenuSidebarHost() const override;
  SpacesHost* MenuSpacesHost() const override;
  ChromePrefsHost* MenuPrefsHost() const override;
  WindowHost* MenuWindowHost() const override;
  phi_framework::PhiFramework* MenuFramework() const override;
  const PhiWindowModel* MenuModel() const override;
  BrowserId MenuActiveBrowser() const override;
  TabId MenuActiveTab() const override;
  std::string MenuActiveSpace() const override;
  PhiPalette MenuPalette() const override;
  int MenuScaled(int design_units) const override;
  HWND MenuAddressField() const override;
  void MenuRefreshNavState() override;
  PhiMenuBarNavState MenuNavState() const override;
  std::vector<PhiMenuBarTab> MenuTabRows() const override;
  bool MenuReaderActive() const override;
  bool MenuReaderOfferable() const override;
  bool MenuLeaveReaderForNavigation() override;
  bool MenuHasSpace(const std::string& space_id) const override;
  bool MenuShowSpace(const std::string& space_id) override;
  void MenuNewTab() override;
  void MenuToggleSidebar() override;
  void MenuToggleReader() override;
  bool MenuAgentHoldsInput() const override;
  void MenuToggleDownloads() override;
  void MenuToggleSearchTabs() override;
  void MenuOpenFeedback() override;
  void MenuCreateSpace() override;
  void MenuEditSpaceTheme() override;

  // --- PhiBrowserHostShell: what the content host asks of its shell --------
  HWND BrowserHostShellWindow() const override;
  int BrowserHostScaled(int design_units) const override;
  PhiPalette BrowserHostPalette() const override;
  RECT BrowserHostContentRect() override;
  RECT BrowserHostClientRect() override;
  HFONT BrowserHostStatusFont() const override;
  phi_framework::PhiFramework* BrowserHostFramework() const override;
  bool BrowserHostIsAgent() const override;
  void BrowserHostRelayout() override;
  void BrowserHostRefreshTabs() override;
  void BrowserHostScheduleRefresh() override;
  void BrowserHostBeginSpaceSwap(const std::string& space_id) override;
  bool BrowserHostSwapRunning() const override;
  void BrowserHostNoteSwapChildren(HWND reveal, HWND conceal) override;
  void BrowserHostEndSpacePreview() override;
  void BrowserHostNoteSpaceWindow(const std::string& space_id,
                                  BrowserId browser) override;
  void BrowserHostNoteActiveSpace(const std::string& space_id) override;
  void BrowserHostAbandonTyping() override;
  void BrowserHostRetintChrome() override;
  void BrowserHostFocusAddressField() override;
  bool BrowserHostFocusNativePage() override;
  bool BrowserHostDeferSpaceClose(const std::string& space_id) override;

  // --- PhiOmniboxShell: what the session asks of its shell ------------------
  HWND OmniboxShellWindow() const override;
  bool OmniboxShellDestroying() const override;
  int OmniboxScaled(int design_units) const override;
  HFONT OmniboxTextFont() const override;
  HFONT OmniboxIconFont() const override;
  PhiPalette OmniboxPalette() const override;
  PhiLayoutMode OmniboxLayoutMode() const override;
  PhiOmniboxCapsule OmniboxCapsule() override;
  phi_framework::PhiFramework* OmniboxFramework() const override;
  BrowserId OmniboxBrowser() const override;
  bool OmniboxBrowserPlaceholding() const override;
  bool OmniboxNewTabFocusesField() const override;
  void OmniboxFocusContent() override;
  void OmniboxLeaveReader() override;
  HWND OmniboxNativePageField() const override;
  bool OmniboxNativePageBox(RECT* out) const override;
  void OmniboxNativePageCommitting() override;

  // --- PhiNativePagesShell: what the native pages ask of their shell -------
  HWND NativePagesShellWindow() const override;
  PhiShellWindow* NativePagesOwner() override;
  bool NativePagesIncognito() const override;
  bool NativePagesIsAgent() const override;
  bool NativePagesContentFullscreen() const override;
  bool NativePagesPlaceholding() const override;
  bool NativePagesSwapPending() const override;
  const std::string& NativePagesActiveUrl() const override;
  int NativePagesActiveTabId() const override;
  std::vector<NativePane> NativePagesSplitPanes() const override;
  void NativePagesRaiseCaps() override;
  void NativePagesFocusContent() override;
  HWND NativePagesContentWindow() const override;
  bool NativePagesCommitPending() const override;

  // --- PhiSidebarShell: what the sidebar asks of its shell -----------------
  HWND SidebarShellWindow() const override;
  PhiShellWindow* SidebarOwner() override;
  int SidebarScaled(int design_units) const override;
  UINT SidebarDpi() const override;
  int SidebarWidthDip() const override;
  PhiSidebarRects SidebarRects() override;
  void SidebarSetRowCounts(int pinned_rows, int bookmark_rows) override;
  void SidebarRefreshLayoutInputs() override;
  PhiPalette SidebarPalette() const override;
  BrowserId SidebarActiveBrowser() const override;
  std::string SidebarActiveSpace() const override;
  bool SidebarPullSnapshot(
      BrowserId browser,
      phi_framework::WindowSnapshot* out) const override;
  bool SidebarSplitIsStacked(TabId tab_id) const override;
  HFONT SidebarGlyphFont() const override;
  HFONT SidebarSemiboldFont() const override;
  PhiChromeHover SidebarHoverKind() const override;
  int SidebarHoverIndex() const override;
  PhiSpacesStrip* SidebarSpacesStrip() override;
  void SidebarScheduleRefresh() override;
  void SidebarSyncNativePages() override;
  PhiDragBitmap SidebarCaptureChrome(const RECT& client_rect) override;
  HWND SidebarContentWindow() const override;
  void SidebarStartNewTab() override;
  void SidebarPublishHits(std::vector<PhiHit> hits) override;
  // The sidebar's slice of a per-turn Layout.
  PhiSidebarRects BandRects(const Layout& layout) const;
  // `client_rect` of the shell's chrome as it paints right now, opaque —
  // what the drag image lifts the dragged element's pixels out of.
  PhiDragBitmap CaptureChromeRect(const RECT& client_rect);
  // THE rect list the accessibility provider reads (phi_window.h): the
  // sidebar publishes its rows here on every rebuild. Not yet answered on
  // WM_GETOBJECT — the shell window is still hand-registered rather than a
  // PhiWindow, so this list is filled and ready but not yet reachable by a
  // screen reader (see the report).
  std::vector<PhiHit> hits_;
  uint32_t hits_version_ = 0;

  // --- PhiSpacesStripShell: what the Spaces strip asks of its shell --------
  HWND SpacesShellWindow() const override;
  PhiShellWindow* SpacesOwner() override;
  SlotId SpacesSlot() const override;
  bool SpacesIncognito() const override;
  bool SpacesIsAgent() const override;
  int SpacesScaled(int design_units) const override;
  PhiSpacesStripRects SpacesRects() override;
  const std::string& SpacesActiveSpace() const override;
  bool SpacesHasSpace(const std::string& space_id) const override;
  PhiPalette SpacesPalette() const override;
  bool SpacesDragActive() const override;
  int SpacesHoveredChip() const override;
  bool SpacesAddSlotHovered() const override;
  RECT SpacesAddSlotDirtyRect() override;
  void SpacesForgetHover() override;
  void SpacesRehoverAtCursor() override;
  void SpacesScheduleRefresh() override;
  void SpacesApplyPalette() override;
  void SpacesRefreshTinted() override;
  void SpacesRepaintOmnibox() override;
  void SpacesFocusContent() override;
  void SpacesSetCoveredContentSuppressed(bool suppressed) override;
  void SpacesRevealSwappedContent() override;
  void SpacesSwapBandModel() override;
  void SpacesDropLeavingBand() override;
  HFONT SpacesUiFont() const override;
  HFONT SpacesSmallFont() const override;
  HFONT SpacesEmojiFont() const override;
  HFONT SpacesChipIconFont() const override;
  HFONT SpacesPipIconFont() const override;
  HFONT SpacesPipEmojiFont() const override;
  HFONT SpacesPipTextFont() const override;
  bool SpacesSidebarCanHostForm(const RECT& form) const override;
  HWND SpacesFloatingPanel(int* offset_y) const override;
  // The strip's slice of a per-turn Layout.
  PhiSpacesStripRects StripRects(const Layout& layout) const;

  // Owned by the framework (a singleton in chrome.dll); non-owning, and across
  // a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* const framework_;
  // The five narrow services the shell was constructed with (§3.5). The
  // coordinator implements all five and owns this controller, so every
  // pointer outlives it; each component below is handed only the ones it
  // calls.
  const ShellServices services_;
  // The coordinator's window model (it owns the instance and outlives every
  // controller): the one source for rows, nav state, splits and groups.
  RAW_PTR_EXCLUSION const PhiWindowModel* const model_;
  // Rebuilds the paint caches (tabs_/pinned_/bookmarks_/group_headers_,
  // the nav state, the chips) from model_ and the host's stores WITHOUT
  // invalidating. Runs on the posted rebuild turn; a few structural view
  // changes (a Space switch, a layout mode change, the floating panel
  // opening) run it directly through RefreshTabs so the next frame is not a
  // frame of the old geometry.
  void RebuildTabs();
  // RebuildTabs + an invalidation of the whole shell. For the structural
  // view changes above only — a framework mutation never needs it, the
  // model's push schedules the rebuild.
  void RefreshTabs();
  // What the rebuild turn compares to invalidate only the bands whose rows
  // changed: a cheap signature of each band's rows.
  struct BandSignature {
    std::vector<std::pair<std::string, RECT>> rows;  // key + bounds.
    std::vector<std::pair<std::string, RECT>> chips;
    std::wstring nav_url;
    bool can_go_back = false;
    bool can_go_forward = false;
    bool nav_loading = false;
    ShellLayoutInputs layout;
  };
  BandSignature CurrentBandSignature() const;
  // Invalidates what changed between two signatures (everything when the
  // layout itself moved).
  void InvalidateChangedBands(const BandSignature& before);
  // --- The Spaces strip, as a component (phi_spaces_strip.h) --------------
  // The pips, their viewport and overflow, the hover card, the trailing "+"
  // and its fade, the create-Space form host and the Space-switch animation
  // (docs/architecture-review.md §3.5, lane 1 clusters K and O plus the chip
  // half of H). The shell keeps only the RETAINED sidebar rows the band
  // slide paints (leaving_band_ below) — they are the sidebar's, not the
  // strip's — and the parked browser child the slide reveals.
  std::unique_ptr<PhiSpacesStrip> spaces_;
  // --- The sidebar, as a component (phi_sidebar_view.h) --------------------
  // The row pipeline, the band's paint, its hit tests, its scrolling, the
  // expanded-bookmark set, the MRU order and the drag session
  // (docs/architecture-review.md §3.5, lane 1 clusters H, I and J plus the
  // sidebar half of Q). The shell keeps the geometry the rows are placed
  // in, the hover cursor they share with the rest of the chrome, and the
  // resize band that sets the width.
  std::unique_ptr<PhiSidebarView> sidebar_;
  PhiLayoutMode layout_mode_ = PhiLayoutMode::kPerformance;

  // The Mac floating sidebar (WebContentContainerViewController+
  // FloatingSidebar): with the sidebar collapsed, hovering the left margin
  // slides in a popup panel showing the REAL sidebar — PaintChrome runs
  // against the expanded layout (floating_layout_ override) and the panel
  // forwards its mouse events through the normal handlers with a y offset.
  bool sidebar_collapsed_ = false;
  // Hosted as a hidden agent Space (off-screen, off-taskbar). See the ctor.
  bool agent_ = false;
  // Off-the-record shell: fixed-dark, no Spaces/pins/bookmarks, unrouted.
  bool incognito_ = false;
  // For an agent window: whether it is currently surfaced (being watched).
  bool agent_visible_ = false;
  bool agent_owned_ = false;
  // The agent Space this USER slot hosts for a watch (see HostAgentSpace).
  std::string hosted_agent_task_;
  std::string hosted_agent_space_;
  std::string hosted_return_space_;  // The user Space to swap back to.
  bool unhost_at_landing_ = false;
  void FinishUnhost();
  bool floating_layout_ = false;
  HWND floating_sidebar_ = nullptr;
  int floating_offset_y_ = 0;
  ULONGLONG floating_shown_tick_ = 0;
  ULONGLONG floating_outside_tick_ = 0;
  void ShowFloatingSidebar();
  void HideFloatingSidebar(bool animated);
  void FloatingSidebarTick();
  void PaintFloatingSidebar(HDC dc);
  static LRESULT CALLBACK FloatingSidebarProc(HWND hwnd,
                                              UINT msg,
                                              WPARAM wparam,
                                              LPARAM lparam);
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SHELL_WINDOW_H_
