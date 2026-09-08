// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_

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
#include "phibrowser-win/client/phi_split_drop.h"
#include "phibrowser-win/client/phi_sidebar_actions.h"
#include "phibrowser-win/client/phi_sidebar_drop.h"
#include "phibrowser-win/client/phi_sidebar_layout.h"
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
  std::string caption;   // The agent's current status line (may be empty).
  bool watched = false;  // Its window is currently surfaced on-screen.
};

// What a slot needs from its owner (the coordinator), as five narrow
// interfaces (docs/architecture-review.md §3.5): the full Space list to draw,
// and requests that only the owner can service (switching to a Space that
// has no browser yet must create one in that Space's profile). PhiSlotHost
// is the union the controller still receives; ShellServices is the struct
// of the five it moves to next (phase 4), at which point each interface is
// handed to the service that owns it — ChromePrefsHost is PhiPreferences
// already.
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
  // by clicking its pip, which toggles its hidden window on/off screen.
  virtual std::vector<PhiAgentPipView> AgentSpaces() const = 0;
  virtual void RequestWatchAgentSpace(const std::string& task_id) = 0;
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

// The five, as the shell will receive them once the controller stops taking
// one object for all (the next extraction step). Non-owning; every pointer
// outlives the shell.
struct ShellServices {
  RAW_PTR_EXCLUSION SidebarHost* sidebar = nullptr;
  RAW_PTR_EXCLUSION SpacesHost* spaces = nullptr;
  RAW_PTR_EXCLUSION ChromePrefsHost* prefs = nullptr;
  RAW_PTR_EXCLUSION AgentHost* agent = nullptr;
  RAW_PTR_EXCLUSION WindowHost* window = nullptr;
};

// The union the controller (and its satellites' `host()`) still hold: one
// object implementing all five. The coordinator is it today, forwarding
// each interface's methods to the service that owns them.
class PhiSlotHost : public SidebarHost,
                    public SpacesHost,
                    public ChromePrefsHost,
                    public AgentHost,
                    public WindowHost {
 public:
  ~PhiSlotHost() override = default;
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
// components that own the rest. It keeps the name PhiWindowController while
// the components are extracted one at a time — the rename to PhiShellWindow
// would touch every other file and lands with the last of them.
//
// Each component's needs reach it through a narrow interface implemented
// PRIVATELY here, so none of them widen the shell's public surface.
class PhiWindowController : private PhiOmniboxShell,
                            private PhiBrowserHostShell,
                            private PhiCaptionShell,
                            private PhiMenuBarShell,
                            private PhiNativePagesShell,
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
  PhiWindowController(SlotId slot,
                      BrowserId browser,
                      HWND browser_hwnd,
                      phi_framework::PhiFramework* framework,
                      PhiSlotHost* host,
                      const PhiWindowModel* model,
                      bool agent = false,
                      bool incognito = false);
  // Virtual because the shell now implements its components' interfaces:
  // the class has virtual functions, and PhiWindowRegistry deletes it
  // through a PhiWindowController* (its test stand-ins subclass it).
  virtual ~PhiWindowController();

  PhiWindowController(const PhiWindowController&) = delete;
  PhiWindowController& operator=(const PhiWindowController&) = delete;

  // The slot's identity — what every PhiSlotHost `slot` parameter takes.
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
  // same seams the controller does.
  PhiSlotHost* host() const { return host_; }
  phi_framework::PhiFramework* framework() const { return framework_; }
  // The window model the rows are built from — what a satellite reads
  // instead of parsing the framework's tab JSON.
  const PhiWindowModel* model() const { return model_; }
  // The feedback form about this window's focused tab (the Mac's
  // showFeedbackWindow).
  void OpenFeedback();
  PhiPalette CurrentPalette() const;  // Ramped while a switch is in flight.
  // Tab ids most-recently-used first — the Ctrl+Tab switcher's order.
  const std::vector<TabId>& TabMruOrder() const { return tab_mru_; }
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
  // Where the form mounts: the sidebar below the Spaces strip.
  RECT CreateSpaceFormRect();
  // Surfaces (visible=true) or parks off-screen (visible=false) an agent
  // window so the user can watch it live. No-op on a non-agent window.
  void SetAgentVisible(bool visible);

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
  // The attached-surface indicator: a plate with the kind's glyph, or the
  // minus that closes it while its row is hovered.
  void DrawSurfaceBadge(HDC dc, const RECT& box, SidebarSurface kind,
                        bool hovered, COLORREF ground);
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

  // Draws a favicon's BGRA pixels into `dest`, composited over `background`.
  // GDI has no alpha blit without msimg32 (AlphaBlend), and we always know what
  // is behind the icon, so the blend is done in software and the result blitted
  // opaque.
  void DrawFavicon(HDC dc,
                   const RECT& dest,
                   const unsigned char* bgra,
                   int width,
                   int height,
                   COLORREF background);
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

  // One rendered tab row: what to draw and what a click at that row does.
  struct TabRow {
    SidebarKey key;  // The tree node this row paints (kTab or kSplit).
    int index = 0;
    TabId id;  // Session id — stable while indices shift; pins bind to it.
    // The tab's favicon, copied out of the framework's scratch buffer (which
    // only survives until its next call).
    std::vector<unsigned char> favicon;
    int favicon_width = 0;
    int favicon_height = 0;
    bool active = false;
    std::wstring title;
    // The page address, as the hover preview card's second line wants it.
    std::wstring url;
    // Chromium native side-by-side: members share a splitId. The leading
    // member renders the MERGED row and absorbs its partner as pane B; the
    // partner never becomes its own row.
    std::string split_id;
    // Tab-group membership (Chromium native model); members sit under their
    // group's header row and hide when it is collapsed.
    std::string group_id;
    // The client identity tag (PhiTabData, session-persisted): a tab tagged
    // with a live pin/bookmark guid IS that row's bound tab.
    std::string tag;
    // chrome:// pages show the Phi mark when no favicon exists.
    bool internal_page = false;
    // DORMANT: holds navigation history it has not loaded yet (restored,
    // duplicated, or discarded). Phi does not load restored background tabs,
    // so this is a steady state for most of them — the row is drawn dimmed.
    bool unloaded = false;
    int b_index = -1;  // -1: not a merged row.
    TabId b_id;        // Pane B's session id, for its hover snapshot.
    bool b_active = false;
    std::wstring b_title;
    std::wstring b_url;
    std::vector<unsigned char> b_favicon;
    int b_favicon_width = 0;
    int b_favicon_height = 0;
    // A Peek or Reader View attached to this row's tab (either pane): the
    // trailing indicator, whose minus closes it (docs/sidebar-architecture
    // §4.6). While one is attached the row's own close box is withheld —
    // the indicator is the one close affordance, as on the Mac.
    SidebarSurface attached_surface = SidebarSurface::kContent;
    RECT bounds{};       // Row rect in client coords.
    RECT pane_b{};       // Right half of a merged row (activates pane B).
    RECT close_box{};    // The row's close affordance.
    RECT surface_box{};  // The attached-surface indicator.
  };
  std::vector<TabRow> tabs_;
  // The model — what every live tab IS (docs/sidebar-architecture.md §4.5).
  // tabs_, pinned_ and bookmarks_ are paint/hit-test caches derived from it
  // in RebuildTabs; nothing decides bindings or split rows anywhere else.
  SidebarTree sidebar_tree_;
  // Comfortable's "+" in the caption band, trailing the last tab. Its place
  // depends on how wide the strip came out, so it is measured with the rows
  // rather than in ComputeLayout. Empty in the vertical layouts, which put
  // their New Tab entry in the sidebar list instead.
  RECT add_tab_button_{};

  // Sidebar tab-group headers (TabGroupHeaderView): chevron + color dot +
  // title on the group's 18% color tint, clicking collapses/expands.
  struct GroupHeader {
    std::string id;
    std::wstring name;   // The stored title; empty for an unnamed group.
    std::wstring title;  // What the row shows: the name, or "Grey · 2 tabs".
    COLORREF color = 0;
    bool collapsed = false;
    RECT bounds{};     // The header row (the collapse toggle).
    RECT container{};  // Header plus visible members — the tinted card.
  };
  std::vector<GroupHeader> group_headers_;

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

  // Sidebar tab-list scrolling (bookmark rows and the pinned band stay put;
  // the group/tab rows scroll). Offset in pixels, clamped to the overflow.
  int scroll_offset_ = 0;
  int tabs_scroll_max_ = 0;
  // Wheel over the sidebar: true when consumed (vertical layouts only).
  bool OnSidebarWheel(POINT client_pt, int wheel_delta);

  // Drag-to-reorder: armed on button-down over a draggable element, live
  // once the cursor passes the system drag threshold, committed on button-up
  // through MoveTab / ReorderStoredRow / ReorderSpace. The insertion slot is
  // painted as a 2px accent line while dragging.
  enum class DragKind { kNone, kTab, kPin, kBookmark, kChip };
  // Where the cursor currently sits during a tab/pin drag — same-band
  // reorder, a conversion target (the Mac's moveNormalTab toPinnd/
  // toBookmark), the PAGE (a split drop, the Mac's SplitTabDropContainer —
  // the one band bookmark drags also reach), or outside the window entirely
  // (tear-off to a new window). Chip drags stay in their own band.
  enum class DropBand { kNone, kTabs, kPins, kBookmarks, kOutside, kContent };
  DragKind drag_kind_ = DragKind::kNone;
  DropBand drag_band_ = DropBand::kNone;
  // WHAT is dragged: a key, never an index — rows rebuild under a drag
  // (a folder toggle, a notification) and a key survives that.
  SidebarKey drag_key_;
  // Its current position in the kind's own vector (-1 when it is gone),
  // looked up fresh each time for the paint/slide code that walks vectors.
  int DragIndex() const;
  POINT drag_origin_{};
  bool drag_active_ = false;
  int drag_insert_ = -1;  // The gap slot for the slide animation, -1 = none.
  // WHERE it lands (docs/sidebar-architecture.md §5), resolved by the pure
  // drop resolvers on every move; consumed by CommitDrag as a SidebarMove.
  SidebarPlace drag_place_;
  bool drag_place_valid_ = false;
  SidebarKey drag_folder_key_;  // A drop INTO this collapsed folder.
  DropBand DropBandAt(POINT client_pt);

  // DROP ON THE PAGE — the Mac's SplitTabDropContainer. A tab, pinned tab or
  // bookmark dragged over the content area forms a split with the focused
  // tab (create mode), or swaps into one pane of the focused split (replace
  // mode). Resolved on every pointer move while the cursor is over the page,
  // and consumed by CommitDrag.
  PhiSplitDropZone split_drop_zone_ = PhiSplitDropZone::kNone;
  // Whether this drag can form a split at all. Resolved ONCE when the pointer
  // enters the page (nothing it depends on can change mid-drag), so a pointer
  // move costs a zone hit-test rather than a framework round-trip.
  bool split_drop_valid_ = false;
  bool split_drop_replace_ = false;
  std::string split_drop_split_id_;  // Replace mode: the split being re-paned.
  bool split_drop_stacked_ = false;  // Its divider axis, for the pane cards.
  double split_drop_ratio_ = 0.5;    // The primary pane's share.
  // Resolves what a drop at `pt` would do and shows (or hides) the hint
  // cards. True while this drag is a valid split candidate over the page —
  // the zone may still be kNone, which is the dead middle band: the cards
  // stay up, but no drop lands.
  bool ResolveSplitDrop(POINT pt, bool rebuild);
  // Performs the drop ResolveSplitDrop described. Takes its inputs by value
  // because CommitDrag clears the drag state before acting.
  void CommitSplitDrop(DragKind kind,
                       const SidebarKey& source,
                       TabId focused_id,
                       PhiSplitDropZone zone,
                       bool replace,
                       const std::string& split_id);
  // The dragged row as the page drop sees it: `tab_id` its live tab (0 when
  // the row has none open), `url` the address to open in that case. False
  // when this drag can never form a split — a split row, a bookmark folder,
  // a row with nothing to open.
  // `guid` names the stored row a pinned/bookmark drag came from, so the
  // caller can re-resolve its LIVE tab against the strip: the press that
  // armed the drag has already opened that row, which the cached view row
  // may not know yet.
  bool SplitDragSource(DragKind kind,
                       const SidebarKey& key,
                       TabId* tab_id,
                       std::string* url,
                       std::string* guid) const;
  // Pairs two live tabs into a split with `primary_id` in the leading (left
  // or top) pane. Chromium normalizes the primary to the lower-indexed tab,
  // so this reverses the pair afterwards when that is not the one asked for —
  // the Mac's pendingSplitPrimaryByCreateId follow-up. Returns the split id.
  std::string FormSplitOrdered(BrowserId browser,
                               TabId primary_id,
                               TabId secondary_id,
                               const char* layout);
  // Drops a tab's pin/bookmark binding (the Mac's makeTabNormalOpened): the
  // stored row stays put as an unopened one, the tab joins the plain list.
  // Harmless on an untagged tab.
  void UnbindStoredTab(BrowserId browser, TabId tab_id);
  // Splits `primary` with `secondary`, after dropping any pin or bookmark
  // binding on EITHER pane. A SPLIT NEVER LIVES IN A STORED CELL: the Mac
  // runs makeTabNormalOpened over both panes in every split-formation path
  // (openNewTabAsSplit, formSplitFromBookmark, the page drop), leaving the
  // stored row behind as an unopened placeholder, so the pair always shows
  // as one merged row in the NORMAL tab list. Reopening a STORED split is
  // the other direction and stays bound — see OpenStoredSplit.
  bool SplitTabsNormalized(BrowserId browser, TabId primary, TabId secondary);
  // The framework's CURRENT state for `browser`, pulled typed — for the few
  // gesture paths that just mutated the strip and act on the result inside
  // the same turn, before the coordinator's model has heard the push. Never
  // for rows or paint: those read model_.
  bool PullSnapshot(BrowserId browser,
                    phi_framework::WindowSnapshot* out) const;

  // Drag-reorder animation, Chrome's vertical-tab model
  // (TabCollectionAnimatingLayoutManager): the dragged row FLOATS with the
  // cursor while its siblings glide to open the insertion gap — each leg
  // eases current → target over 200ms, retargeting when the gap moves.
  std::map<TabId, double> drag_slide_from_;  // Tab id -> leg start px.
  std::map<TabId, double> drag_slide_to_;    // Tab id -> leg target px.
  // Pinned cells glide in BOTH axes (a grid gap wraps rows); bookmark rows
  // glide vertically. Keyed by the stored row's guid.
  struct SlideVec {
    double dx = 0.0;
    double dy = 0.0;
  };
  std::map<std::string, SlideVec> row_slide_from_;
  std::map<std::string, SlideVec> row_slide_to_;
  ULONGLONG drag_slide_start_ = 0;         // Leg clock; 0 = idle.
  POINT drag_point_{};                     // Cursor, for the drop.
  // The tab that was FOCUSED when this drag was armed — the split drop's
  // other pane. It has to be captured at arming time: the same mouse-down
  // that arms a drag also activates the tab it grabbed (and OPENS the pinned
  // tab or bookmark it grabbed), so by the time the pointer reaches the page
  // active_tab_id_ is the dragged row itself and the page the user was
  // looking at is gone.
  TabId drag_focus_tab_id_;
  // The tab row a press armed WITHOUT activating it: a plain click
  // activates on release, a drag never does (the Mac's click-to-select).
  TabId drag_click_tab_id_;
  double DragSlideOffset(TabId tab_id) const;
  SlideVec RowSlideOffset(const std::string& guid) const;
  void RetargetDragSlide();  // New targets for the current gap slot.

  // THE DRAG IMAGE — the Mac's NSDraggingSession image with
  // TabDraggingSession's switching: the dragged row's snapshot while the
  // cursor is inside the sidebar, the page card once it leaves, and AppKit's
  // slide-back when a drag ends inside the sidebar without effect. It lives
  // in its own topmost layered window (PhiDragImage): the shell's DC cannot
  // paint over the reparented Chromium child or past the window's edge,
  // which is exactly where the old in-DC floating row vanished. While it is
  // up, the painters leave the dragged element OUT of the chrome (its gap
  // is what the siblings glide around).
  //
  // `drag_capture_` is the one exception: PaintChrome renders the dragged
  // element at home, in its lifted style, so BeginDragImage can lift its
  // pixels straight out of the frame — the Mac's createDraggingSnapshot.
  bool drag_capture_ = false;
  bool drag_image_inside_ = true;  // Last boundary verdict: row or card.
  bool drag_card_built_ = false;   // The card is composed once per drag.
  PhiDragCard drag_card_;
  RECT drag_home_screen_{};  // The element's rect at lift-off, for the slide.
  // The dragged element's own rect — the pill, the cell — in client coords.
  RECT DragSourceRect();
  // The Mac's drag-image boundary (containsTabDragBoundary): inside the
  // sidebar's list band, or over ANOTHER shell window, the row image shows;
  // elsewhere — the page, the header, off the window — the page card does.
  bool DragImageInside(POINT client_pt);
  void BeginDragImage(POINT client_pt);  // At drag activation.
  void SyncDragImage(POINT client_pt);   // Every pointer move.
  // After the drop: slides home when nothing happened and the pointer was
  // still inside the sidebar; otherwise just takes the image down.
  void EndDragImage(bool acted, bool inside);
  // An external cancel (the capture was taken away, a panel press): the
  // state goes and the image slides home or vanishes on the same rule.
  void AbortDrag();
  bool CommitDragImpl();  // True when the drop changed something.
  // The page card's model for this drag: the dragged row's live page as
  // seen on screen, else its cached thumbnail(s), else favicon + title.
  PhiDragCard BuildDragCard();
  // `client_rect` of the shell's chrome as it paints right now, opaque.
  PhiDragBitmap CaptureChromeRect(const RECT& client_rect);
  // The browser child's on-screen pixels, downscaled toward the card; empty
  // when the page has not painted (uniform) or the child is not visible.
  PhiDragBitmap CaptureContentFromScreen();
  // A pinned row or bookmark released outside the window: its live tab(s)
  // move to the shell under the cursor, else tear off into a new window —
  // the Mac's live-wrapper tear-off, the stored row left behind unopened.
  bool TearOffStoredRow(DragKind kind, const SidebarKey& key, POINT release);
  // The pinned grid's placement walk over arbitrary cell widths (1 or 2
  // slots) — layout uses it for the real cells, the drag preview with a
  // phantom cell spliced in at the insertion slot.
  std::vector<RECT> PinnedGridWalk(const std::vector<int>& units,
                                   const Layout& layout);

  // Sidebar edge resize (the Mac's split drag): a 4-DIP grab band in the
  // sidebar's trailing gutter; live while dragging, persisted on release.
  int sidebar_dip_ = kSidebarWidthDefault;
  bool sidebar_resizing_ = false;
  bool InSidebarResizeBand(POINT client_pt);

  void ArmDrag(const SidebarKey& key, POINT pt);
  void UpdateDrag(POINT pt);
  void CommitDrag();
  void CancelDrag();
  int DragInsertSlot(POINT pt);
  void DrawDragIndicator(HDC dc);

  // Hover feedback (the Mac leans on hover: row washes, close affordances).
  // Which chrome element the cursor rests on, as element-kind + position in
  // its vector; repaints fire only when it changes.
  // kNav: the toolbar's back/forward/reload/search buttons — hover_index_
  // is 0..3 in that order.
  // kAddSpace: the Spaces strip's trailing slot — the "+" or, on overflow,
  // the "…" switcher standing in for it.
  enum class HoverKind { kNone, kTab, kPin, kBookmark, kGroupHeader, kChip,
                         kNav, kNewTabRow, kFieldExt, kAddSpace };
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
  // Paints a bookmark folder's chosen catalog symbol into its icon slot.
  void DrawFolderIcon(HDC dc,
                      const RECT& slot,
                      const std::string& icon_id,
                      COLORREF ink,
                      bool expanded);
  // True when the split holding `tab_id` stacks its panes (the wire's
  // "horizontal"); false for side-by-side and for a tab in no split.
  bool SplitIsStacked(TabId tab_id) const;
  // Pinned tabs (the band under the header) and bookmarks (the section above
  // the open tabs) — both come from the client's own store, as on Mac.
  struct ShortcutRow {
    SidebarKey key;  // kCell or kFolder.
    std::string guid;  // The store row, so the context menu can remove it.
    std::string url;
    std::wstring title;
    bool folder = false;
    // Folders only: the catalog symbol drawn in the row's icon slot.
    std::string icon;
    // Split rows: the divider axis the pair reopens with.
    std::string layout;
    // Bookmark outline (the Mac's NSOutlineView): an expanded folder splices
    // its children in below itself, one indent step deeper. `parent_guid`
    // names the folder a row sits in (empty = the root).
    int depth = 0;
    bool expanded = false;
    std::string parent_guid;
    // A pinned tab bound to a LIVE tab (openOrFocusPinnedTab): the tab lives
    // in the pin's cell — never in the tab list — and the active one gets the
    // Mac's selected wash + 2pt accent ring.
    bool open = false;
    bool active = false;
    // A Peek or Reader View attached to a bound pane — the indicator (a
    // trailing slot on a bookmark row, a corner badge on a pinned cell).
    SidebarSurface attached_surface = SidebarSurface::kContent;
    RECT surface_box{};
    // The bound tab's session id while `open` — what the hover preview asks
    // the framework for a snapshot of. A pinned PAIR binds both: `tab_id` is
    // this row's own tab, `tab_id_b` its partner's.
    TabId tab_id;
    TabId tab_id_b;
    RECT bounds{};
    // The favicon stored with the row (a pinned tab has no live tab to ask).
    std::vector<unsigned char> favicon;
    int favicon_width = 0;
    int favicon_height = 0;
    // Split cell: one row whose second pane rides url_b/title_b/favicon_b
    // (a pinned split spans two grid slots; a bookmark split draws both
    // favicons and a "primary • secondary" title). Pane B's live tab is
    // tagged TagFor(guid, true) and lands in tab_id_b.
    std::string url_b;
    std::wstring title_b;
    std::vector<unsigned char> favicon_b;
    int favicon_b_width = 0;
    int favicon_b_height = 0;
  };
  std::vector<ShortcutRow> pinned_;
  std::vector<ShortcutRow> bookmarks_;
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
  // The active tab's URL (captured by RebuildTabs) — "show on new tab"
  // needs to know whether the page IS the new-tab page.
  std::string active_tab_url_;
  TabId active_tab_id_;  // Its session id; invalid with no active tab.
  // The tab whose native new-tab page blanked on an omnibox commit; the
  // controls return once another tab is active or that load has ended.
  TabId ntp_commit_tab_id_;
  // Counted before the layout is derived, because the bands they occupy push
  // everything below them down.
  int pinned_row_count_ = 0;
  int bookmark_row_count_ = 0;
  // Pinned cells and the bookmark outline as paint rows, from the tree's
  // cells (order, depth, bindings) and the store's views (titles, urls,
  // favicons), placed within `layout`. The tree must be built first.
  void RebuildShortcuts(const Layout& layout);
  // The bookmark cells an expanded chain reveals, in outline order.
  std::vector<const SidebarCell*> VisibleBookmarkCells() const;
  // The pinned grid's metrics for LayoutPinnedGrid, from the sidebar width
  // and `layout.favorites`.
  PinnedGridMetrics GridMetrics(const Layout& layout) const;
  // Folders the user has disclosed in the sidebar outline (session-local
  // view state, like the Mac outline's).
  std::set<std::string> expanded_bookmarks_;
  // Where a bookmark drag will land, resolved from the pointer on every
  // move (the Mac's SidebarBookmarkFolderDropResolver): the target parent
  // folder (empty = the root), the sibling index within it, and — for a
  // drop ON a collapsed folder — the visible row to highlight (-1 = an
  // ordinary gap insertion). Returns the visible slot the gap animation
  // should open at.
  int bookmark_drop_folder_row_ = -1;  // Visible row of drag_folder_key_.
  // The section's rows as the pure resolvers see them.
  std::vector<DropRow> TabDropRows() const;
  std::vector<DropRow> PinnedDropRows() const;
  std::vector<DropRow> BookmarkDropRows() const;
  // Sends a command to the host against the current tree; the host refreshes.
  bool Apply(const SidebarCommand& command);

  // Space switch: the PAGE swaps instantly; the SIDEBAR BAND slides — and
  // natively, no macOS snapshot compositing: the leaving Space's laid-out
  // rows are RETAINED and painted live, shifted out at their frozen source
  // palette, while the entering rows paint shifted in at the ramping
  // palette. The chrome's colors ramp source→target on their own clock.
  struct BandModel {
    std::vector<TabRow> tabs;
    std::vector<ShortcutRow> pinned;
    std::vector<ShortcutRow> bookmarks;
    std::vector<GroupHeader> headers;
  };
  BandModel leaving_band_;
  void DropLeavingBand() { leaving_band_ = BandModel(); }
  // The page changes AFTER the band slide completes (the Mac reveals the
  // target on completion): the entering child stays hidden and the leaving
  // one visible until then.
  HWND swap_reveal_ = nullptr;   // Shown when the slide lands.
  HWND swap_conceal_ = nullptr;  // Hidden when the slide lands.
  void RevealSwappedContent();
  void ComposeFrame(HDC mem);  // Chrome + drag indicator.
  // The sidebar band (pinned grid through tab list): once normally, or the
  // two-pass slide while a switch animates.
  void PaintSidebarBand(HDC dc, const Layout& layout,
                        const PhiPalette& palette);
  void PaintBandOrSwipe(HDC dc, const Layout& layout,
                        const PhiPalette& palette);
  void SwapBandModel();  // leaving_band_ ↔ the live row vectors.

  // Stored-row bindings are DERIVED, not stored: a tab whose identity tag
  // equals a live pin/bookmark guid is that row's bound tab (the Mac's
  // customGuid model). Tags ride PhiTabData through session restore, so
  // bindings survive a restart. Bookmark bindings hide their tab from the
  // list only in vertical layouts.
  void CloseStoredContent(const std::string& guid);
  // Reopens or refocuses a STORED split (a pinned pair or a split
  // bookmark): missing panes are created, tags applied, and the pair
  // married — the Mac's openPinnedSplit / openTwoURLsAsSplit.
  void OpenStoredSplit(const ShortcutRow& row);

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
  // Tab ids of the ACTIVE browser, most-recently-used first.
  std::vector<TabId> tab_mru_;
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
  PhiSlotHost* MenuHost() const override;
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
  PhiWindowController* NativePagesOwner() override;
  bool NativePagesIncognito() const override;
  bool NativePagesIsAgent() const override;
  bool NativePagesContentFullscreen() const override;
  bool NativePagesPlaceholding() const override;
  bool NativePagesSwapPending() const override;
  const std::string& NativePagesActiveUrl() const override;
  void NativePagesRaiseCaps() override;
  void NativePagesFocusContent() override;
  HWND NativePagesContentWindow() const override;
  bool NativePagesCommitPending() const override;

  // --- PhiSpacesStripShell: what the Spaces strip asks of its shell --------
  HWND SpacesShellWindow() const override;
  PhiWindowController* SpacesOwner() override;
  SlotId SpacesSlot() const override;
  PhiSlotHost* SpacesHost() const override;
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
  RAW_PTR_EXCLUSION PhiSlotHost* const host_;  // Owns this controller.
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

#endif  // PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_
