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
#include "phibrowser-win/client/phi_split_drop.h"
#include "phibrowser-win/client/phi_sidebar_actions.h"
#include "phibrowser-win/client/phi_sidebar_drop.h"
#include "phibrowser-win/client/phi_sidebar_layout.h"
#include "phibrowser-win/client/phi_sidebar_model.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

// Where the browser's chrome lives. The Mac client's LayoutMode, same names
// and same meanings (PhiPreferences.swift), so a user moving between platforms
// gets the same three choices:
//
//   kPerformance  vertical tabs, address bar IN the sidebar (the default)
//   kBalanced     vertical tabs, address bar across the TOP of the web content
//   kComfortable  horizontal tabs — the traditional stacked layout, no sidebar
enum class PhiLayoutMode { kPerformance, kBalanced, kComfortable };

// Wire names, matching the Mac enum's raw values so the two clients can share
// a stored setting: "performance", "balanced", "comfortable".
const char* PhiLayoutModeName(PhiLayoutMode mode);
bool PhiLayoutModeFromName(const std::string& name, PhiLayoutMode* out);

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

// What a slot needs from its owner (the coordinator): the full Space list to
// draw, and requests that only the owner can service (switching to a Space
// that has no browser yet must create one in that Space's profile).
class PhiTabPreview;
class PhiBookmarkManager;
class PhiNewTabPage;
class PhiCreateSpacePanel;

class PhiSlotHost {
 public:
  virtual ~PhiSlotHost() = default;
  // Every Space, in display order.
  virtual std::vector<PhiSpaceView> Spaces() const = 0;
  virtual void RequestSwitchSpace(int64_t slot, const std::string& space_id) = 0;
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
  // Creates a Space in this slot's current profile (`new_profile` = false) or
  // in a freshly created profile (true), then switches to it.
  virtual void RequestCreateSpace(int64_t slot, bool new_profile) = 0;
  // The layout every slot renders in — one setting for the whole client, as on
  // Mac, so a new window never disagrees with the one beside it.
  virtual PhiLayoutMode LayoutMode() const = 0;
  virtual void RequestLayoutMode(PhiLayoutMode mode) = 0;
  // The Mac's GeneralSettings.showTabPreviews: whether a settled hover on a
  // tab, pin or bookmark row raises its page-snapshot card.
  virtual bool ShowTabPreviews() const = 0;
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
  // A Space's browser is live in this slot. The coordinator remembers the
  // (Space, window id) pair so a later run can name that window when it asks
  // the framework to park or rebuild it (lazy Space restore).
  virtual void NoteSpaceWindow(const std::string& space_id,
                               int64_t browser_window_id) = 0;
  // --- Agent operating mask (the user's own Spaces) -----------------------
  // True while an agent holds `tab_id`; the row wears the mask badge.
  virtual bool IsTabAgentDriven(int tab_id) const = 0;
  // True once the user took `tab_id` back — the badge reads as "you have
  // control" and the browser refuses that tab's drive commands.
  virtual bool IsTabReclaimed(int tab_id) const = 0;
  // "Take control" / "Hand back" on a driven tab.
  virtual void RequestReclaimTab(int tab_id) = 0;
  virtual void RequestReleaseTab(int tab_id) = 0;
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
  // Surfaces the shell hosting `browser_window_id` (foregrounding it and
  // switching it to that browser's Space when it is hidden) — the search
  // palette's cross-window jump.
  virtual void FocusWindowForBrowser(int64_t browser_window_id) = 0;
  // "New tab behavior = Omnibox": a new tab focuses the address field
  // instead of opening a page (the tab is born on commit).
  virtual bool NewTabOpensOmnibox() const = 0;
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
  // Moves a Space to `new_index` in the Space list (chip drag).
  virtual void ReorderSpace(const std::string& space_id, int new_index) = 0;
  // The sidebar width in DIP — one app-wide value like the layout mode,
  // clamped to the Mac split range (193–500).
  virtual int SidebarWidth() const = 0;
  virtual void RequestSidebarWidth(int dip) = 0;
  // The app-wide resolved theme.
  virtual const PhiPalette& Palette() const = 0;
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
  // The bookmark bar's two visibility switches (the Mac View menu's
  // "Always Show Bookmark Bar" / "Show Bookmark Bar on New Tab").
  virtual bool AlwaysShowBookmarkBar() const = 0;
  virtual void SetAlwaysShowBookmarkBar(bool enabled) = 0;
  virtual bool BookmarkBarOnNewTab() const = 0;
  virtual void SetBookmarkBarOnNewTab(bool enabled) = 0;
  // Opens Settings > Spaces with this Space selected — its detail column is
  // the icon picker (the "Change Icon…" destination).
  virtual void RequestSpaceIconPicker(const std::string& space_id) = 0;
  // The create-Space form (PhiCreateSpacePanel). The first four are the
  // Settings host's spellings, so one coordinator override serves both.
  virtual std::vector<std::string> ProfileSummaries() const = 0;  // "id\tname"
  virtual std::string ThemeId() const = 0;  // The app-wide theme.
  virtual PhiAppearance Appearance() const = 0;
  virtual int Saturation() const = 0;  // 0..100.
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
  // The coalesced row rebuild (ScheduleRefresh): framework notifications
  // arrive DURING the change they describe and WM_PAINT must never rebuild,
  // so the model rebuilds on this posted turn and the paint recomposes.
  kMsgPhiRebuildRows,
  // Alt pressed and released on its own: summon the caption band's hidden
  // menu titles (and hide them again on a second Alt), the keyboard route
  // to the bar the pointer reveals by hovering.
  kMsgPhiToggleMenuReveal,
};

class PhiWindowController {
 public:
  // `agent` hosts the window as a HIDDEN agent Space: the shell is created
  // off-screen and off the taskbar (WS_EX_TOOLWINDOW), shown without
  // activation so its content still composites and renders for CDP screen
  // capture, but the user never sees it. Everything else is identical, so a
  // future "watch" gesture can move it on-screen.
  // `incognito` hosts an off-the-record browser: fixed-dark chrome, no
  // Spaces strip / pinned grid / bookmarks (nothing persists), and the
  // window stays OUT of the Space registry so URL routing never lands here.
  PhiWindowController(int64_t window_id,
                      HWND browser_hwnd,
                      phi_framework::PhiFramework* framework,
                      PhiSlotHost* host,
                      bool agent = false,
                      bool incognito = false);
  ~PhiWindowController();

  PhiWindowController(const PhiWindowController&) = delete;
  PhiWindowController& operator=(const PhiWindowController&) = delete;

  int64_t window_id() const { return window_id_; }
  // Upstream's distillability verdict for one of this window's tabs.
  void NoteTabDistillable(int tab_id, bool distillable);
  // True when `tab_id` is in the strip this window is currently drawing.
  bool ShowsTab(int tab_id) const;
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
  // The feedback form about this window's focused tab (the Mac's
  // showFeedbackWindow).
  void OpenFeedback();
  PhiPalette CurrentPalette() const;  // Ramped while a switch is in flight.
  // Tab ids most-recently-used first — the Ctrl+Tab switcher's order.
  const std::vector<int>& TabMruOrder() const { return tab_mru_; }
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
  bool creating_space() const { return creating_space_; }
  // Where the form mounts: the sidebar below the Spaces strip.
  RECT CreateSpaceFormRect();
  // Surfaces (visible=true) or parks off-screen (visible=false) an agent
  // window so the user can watch it live. No-op on a non-agent window.
  void SetAgentVisible(bool visible);

  // Re-reads tab/nav state from the framework and repaints the sidebar NOW.
  // For changes the client itself just made, where the browser state has
  // already settled.
  void RefreshTabs();
  // Click on a pin or a sidebar bookmark row (cell click, context-menu
  // Open, or the pinned.open verb): focus the bound live tab if it still
  // exists, otherwise open the row's url as a new bound tab — the Mac's
  // BrowserState.openOrFocusPinnedTab / bookmarkGuidBacking model.
  void OpenOrFocusPinned(const std::string& guid, const std::string& url);

  // Paints one rounded-corner cap (called by its window proc).
  void PaintCornerCap(HWND cap, HDC dc);
  // Commits cap `index` as a per-pixel-alpha layer (an antialiased arc);
  // false when the layer cannot be built, and the cap keeps its region cut.
  bool CommitCornerCap(int index);
  void RefreshCornerCaps();  // Re-commits (or repaints) all four.

  // Paints the bottom-left link-target bubble (called by its window proc).
  void PaintStatusBubble(HWND bubble, HDC dc);
  // The pointered link target changed in `browser_id`'s page (empty url =
  // pointer left the link). Shows/hides the status bubble when that browser
  // is the one this shell displays.
  void OnTargetURLChanged(int64_t browser_id, const std::wstring& url);

  // Autocomplete results for `browser_id`'s running omnibox query — feeds
  // the address field's suggestion dropdown (the Mac OmniBox panel).
  void OnOmniboxResults(int64_t browser_id, const std::string& json);

  // `browser_id`'s last tab closed but the browser SURVIVES in placeholder
  // mode (entered) or a new tab ended that mode. The content child hides
  // while placeholding — the dead tab's final frame would linger otherwise
  // — and a field commit births the first tab.
  void OnPlaceholderModeChanged(int64_t browser_id, bool entered);
  // A tab's content (video/element) entered or left fullscreen: the hosted
  // child resizes itself to the MONITOR, so the shell must go borderless
  // fullscreen for the duration or the render is clipped to the window.
  void OnContentFullscreenChanged(int64_t browser_id, bool fullscreen);
  // Paints the dropdown (called by its window proc).
  void PaintOmniboxPopup(HWND popup, HDC dc);
  // Popup mouse plumbing (client-y within the popup).
  void OmniboxHover(int y);
  void OmniboxClick(int y);

  // Marks the sidebar dirty and lets the paint happen at the next idle. Used
  // for framework-driven notifications, which arrive DURING the change they
  // describe (a tab being removed is still in the strip) — painting inline
  // would render the pre-change state and then never correct it.
  void ScheduleRefresh();

  // Rebuilds `tabs_` from framework state WITHOUT invalidating (paint-time
  // use; RefreshTabs() would recurse through InvalidateRect).
  void RebuildTabs();
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
                          int64_t browser_window_id,
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
  std::string SpaceForBrowser(int64_t browser_id) const {
    for (const auto& [space, browser] : space_browsers_) {
      if (browser == browser_id) {
        return space;
      }
    }
    return std::string();
  }
  const std::string& active_space() const { return active_space_; }
  // The browser window id currently displayed (for tab/nav commands).
  int64_t active_browser() const { return active_browser_; }
  // The browser this slot holds for `space_id`, or 0 — feeds the page
  // context menu's Space list (its current-Space exclusion).
  int64_t BrowserIdForSpace(const std::string& space_id) const {
    auto it = space_browsers_.find(space_id);
    return it != space_browsers_.end() ? it->second : 0;
  }
  // Spaces this slot has a browser for, in creation order.
  const std::vector<std::string>& space_order() const { return space_order_; }
  // Drops a browser that went away; returns true if the slot still has one.
  bool ForgetBrowser(int64_t browser_window_id);
  // Closes the browser this slot holds for `space_id` (a Space being
  // deleted), switching to another open Space first so the window never
  // shows a dead one. No-op when the Space is not open here. If it was the
  // slot's ONLY browser, the slot closes with it.
  void CloseSpaceBrowser(const std::string& space_id);
  // Starts closing the whole slot (its Space browsers, one per Space). MAY
  // DESTROY `this` before returning — the last browser going away tears the
  // slot down — so touch nothing on this object afterwards.
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
  PhiMenuBar* menu_bar() { return menu_bar_.get(); }

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
  // The menu bar drives navigation, tabs and Spaces through the controller's
  // own internals — it is this class's UI, split into its own file for size.
  friend class PhiMenuBar;

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

  // Every rect the chrome needs, for one layout mode. Derived from the client
  // rect on demand rather than stored, so a resize or a mode switch needs no
  // invalidation of its own.
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

  void AdoptBrowserAsChild();
  // Moves the address field's EDIT child into `layout`'s slot for it.
  void PositionUrlField(const Layout& layout);
  // `include_child_controls` draws what child windows would draw themselves —
  // set only on the WM_PRINTCLIENT (PrintWindow) path, which renders the
  // parent alone. In WM_PAINT the URL field's EDIT child paints that region.
  void PaintChrome(HDC dc, const Layout& layout, bool include_child_controls);
  // The caption band's menu titles, measured for the current DPI: one rect
  // per top-level menu, in bar order. Rebuilt when the DPI changes.
  void EnsureMenuTitleRects();
  // How many of `menu_titles_` are live right now, from the leading edge.
  // The vertical layouts hide every title but the Phi button at rest and
  // show the whole bar once the band is revealed; the count is then clipped
  // to `layout.band_limit` so a narrow window drops the titles that would
  // run under the trailing controls rather than painting them there.
  size_t VisibleMenuTitles(const Layout& layout);
  // Pointer-driven reveal of the hidden titles. Repaints the band on a
  // change; Alt's own sticky reveal (menu_alt_revealed_) is untouched.
  void SetMenuRevealed(bool revealed);
  // Re-derives the reveal from where the cursor ACTUALLY is. The two leave
  // messages do not mean "the pointer left the band" — see the WM_MOUSELEAVE
  // handler — so they ask this instead of hiding outright.
  void UpdateMenuRevealFromCursor();
  // Same idea for the hot title: after a dropped menu closes, the pointer may
  // be somewhere the shell never saw it move to, so the highlight is re-read
  // from the cursor rather than left where the last WM_MOUSEMOVE put it.
  void UpdateMenuHotFromCursor();
  // What a point in the caption band hits: HTCLOSE / HTMAXBUTTON /
  // HTMINBUTTON, HTCLIENT over a menu title, HTCAPTION anywhere else in the
  // band, 0 below it.
  int CaptionHitTest(POINT client_pt);
  // Hot-state setters; each repaints the band when the state changes.
  void SetCaptionHot(int code);
  void SetMenuHot(int index);
  // Handles a click in the chrome (tab rows, close boxes, buttons, toolbar).
  void OnChromeClick(int x, int y);
  // Middle-click in the chrome: closes the tab row under the pointer (the
  // Mac tab list's otherMouseDown). Pins/bookmarks deliberately ignore it.
  void OnChromeMiddleClick(int x, int y);
  // Right-click in the chrome. On a Space chip this raises the Mac's Space
  // menu (activate / rename / change icon / set theme / delete); elsewhere it
  // does nothing, as on Mac.
  void OnChromeContextMenu(int x, int y);
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

  // --- Address field --------------------------------------------------------
  // A real Win32 EDIT child, so typing, selection, clipboard, IME and the
  // caret all behave natively (Chromium's own omnibox is hollowed away under
  // IS_WIN_PHI_CLIENT, and hand-rolling text editing would reproduce it badly).
  void EnsureUrlField();
  // Pushes the current page's URL into the field. Skipped while the user is
  // typing unless `force` (the Escape-reverts path).
  void SyncUrlField(bool force);
  // Navigates to whatever is typed in `field` — the address field or the
  // native new-tab page's omnibox — then hands focus back to the page.
  void CommitOmniboxField(HWND field);
  void CommitUrlField();  // CommitOmniboxField(url_field_).
  // The omnibox field holding keyboard focus (the address field or the
  // new-tab page's), or null: the session's query, dropdown and arrow fills
  // all act on this one.
  HWND FocusedOmniboxField() const;
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
  // Subclass proc: Enter commits, Escape reverts, focus selects all.
  static LRESULT CALLBACK UrlFieldProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wparam,
                                       LPARAM lparam);
  HWND url_field_ = nullptr;
  // The EDIT paints itself in SYSTEM colours, which clash under a dark theme.
  // WM_CTLCOLOREDIT hands it the palette instead; the brush must outlive the
  // message, so it is cached and rebuilt only when the colour changes.
  HBRUSH url_brush_ = nullptr;
  COLORREF url_brush_color_ = CLR_INVALID;
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
  UINT dpi_ = 96;

  // Caption-band state: which caption button is hot / pressed (an HT* code,
  // 0 = none) and which menu title is hot (-1 = none). Purely visual except
  // pressed_caption_, which arms the button-up action.
  int hot_caption_ = 0;
  int pressed_caption_ = 0;
  // The vertical layouts keep the menu titles hidden at rest — the band's
  // left run is drag space — and slide them in when the pointer enters the
  // band or Alt asks for them. The Alt reveal is sticky: it survives the
  // pointer leaving, and drops on Escape, on a menu opening, or when the
  // window loses focus, the way Windows' own Alt-summoned menu bar does.
  bool menu_revealed_ = false;
  bool menu_alt_revealed_ = false;
  // A menu is dropped from the band right now. The popup takes the pointer
  // with it, so the leave messages that follow must not collapse the bar out
  // from under the menu the user is reading.
  bool menu_open_ = false;
  int hot_menu_ = -1;
  // Menu titles with their band rects, measured for menu_titles_dpi_.
  std::vector<std::pair<std::wstring, RECT>> menu_titles_;
  UINT menu_titles_dpi_ = 0;

  // One rendered tab row: what to draw and what a click at that row does.
  struct TabRow {
    SidebarKey key;  // The tree node this row paints (kTab or kSplit).
    int index = 0;
    int id = 0;  // Session id — stable while indices shift; pins bind to it.
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
    int b_id = 0;      // Pane B's session id, for its hover snapshot.
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
    std::wstring title;
    COLORREF color = 0;
    bool collapsed = false;
    RECT bounds{};     // The header row (the collapse toggle).
    RECT container{};  // Header plus visible members — the tinted card.
  };
  std::vector<GroupHeader> group_headers_;

  // Model freshness: rebuilds happen on notifications and intents
  // (ScheduleRefresh/RefreshTabs/LayoutContents mark this), not per paint.
  // WM_PRINTCLIENT still forces a rebuild — captures must be current.
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
                       int focused_id,
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
                       int* tab_id,
                       std::string* url,
                       std::string* guid) const;
  // Pairs two live tabs into a split with `primary_id` in the leading (left
  // or top) pane. Chromium normalizes the primary to the lower-indexed tab,
  // so this reverses the pair afterwards when that is not the one asked for —
  // the Mac's pendingSplitPrimaryByCreateId follow-up. Returns the split id.
  std::string FormSplitOrdered(int64_t browser,
                               int primary_id,
                               int secondary_id,
                               const char* layout);
  // Drops a tab's pin/bookmark binding (the Mac's makeTabNormalOpened): the
  // stored row stays put as an unopened one, the tab joins the plain list.
  // No-op for an untagged tab.
  void UnbindStoredTab(int64_t browser, int tab_id);
  // Splits the ACTIVE tab with `tab_id`, after dropping any pin or bookmark
  // binding on EITHER pane. A SPLIT NEVER LIVES IN A STORED CELL: the Mac
  // runs makeTabNormalOpened over both panes in every split-formation path
  // (openNewTabAsSplit, formSplitFromBookmark, the page drop), leaving the
  // stored row behind as an unopened placeholder, so the pair always shows
  // as one merged row in the NORMAL tab list. Reopening a STORED split is
  // the other direction and stays bound — see OpenStoredSplit.
  bool SplitActiveWithNormalized(int64_t browser, int tab_id);

  // Drag-reorder animation, Chrome's vertical-tab model
  // (TabCollectionAnimatingLayoutManager): the dragged row FLOATS with the
  // cursor while its siblings glide to open the insertion gap — each leg
  // eases current → target over 200ms, retargeting when the gap moves.
  std::map<int, double> drag_slide_from_;  // Tab id -> leg start offset px.
  std::map<int, double> drag_slide_to_;    // Tab id -> leg target offset px.
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
  int drag_focus_tab_id_ = 0;
  // The tab row a press armed WITHOUT activating it: a plain click
  // activates on release, a drag never does (the Mac's click-to-select).
  int drag_click_tab_id_ = 0;
  double DragSlideOffset(int tab_id) const;
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

  // The Space pips' hover card (the Mac SpacesStripView's delayed tooltip):
  // the dim wash is immediate (the hover state above); the info card — the
  // Space as a tinted pill, its bound profile, its Ctrl+N switch shortcut —
  // appears only after the cursor SETTLES on a pip, retargets instantly
  // while already up, and a click keeps it down until the pointer leaves
  // the clicked pip (so the card never blinks back right after a switch).
  std::string hover_card_space_;       // Card owner (empty = no card).
  std::string pending_card_space_;     // Scheduled, awaiting the delay.
  std::string card_suppressed_space_;  // Clicked; no card until hover-off.
  // Card content, resolved when the card arms — the profile name needs a
  // framework JSON round-trip, not per-paint work.
  std::wstring card_profile_;
  COLORREF card_accent_ = 0;  // The owner Space's theme accent.
  int card_shortcut_ = 0;     // 1-based Ctrl+N digit; 0 = past the ninth.
  void UpdateHoverCard();  // Re-aims card state at the hovered pip.
  void PromoteHoverCard();  // Delay elapsed: pending becomes shown.
  void DismissHoverCard(const std::string& suppress_space_id);
  void ResolveHoverCard(const std::string& space_id);  // Fills card_*.
  // The Space pip's hover card. It is a floating POPUP (like PhiTabPreview),
  // not shell-DC chrome: it hangs below the strip, where the omnibox EDIT
  // child would paint over anything the shell drew and the content child
  // would clip it. PaintChrome measures it (MeasureHoverCard, with the
  // shell DC's font) and SyncHoverCardPopup shows / moves / hides the popup,
  // which paints the measured geometry at its own origin.
  struct HoverCardGeometry {
    RECT card{};
    int pill_h = 0, pill_w = 0, name_w = 0, profile_w = 0, keys_w = 0;
    int dot_w = 0, gap = 0;
    std::vector<int> key_widths;
    std::vector<std::wstring> keycaps;
    std::wstring icon, name, profile;
  };
  HoverCardGeometry hover_card_geom_;  // As last synced; card at (0, 0).
  HWND hover_card_popup_ = nullptr;
  RECT hover_card_popup_rect_{};  // Screen rect last applied to the popup.
  bool MeasureHoverCard(HDC dc, const Layout& layout, HoverCardGeometry* out);
  void PaintHoverCard(HDC dc,
                      const HoverCardGeometry& geometry,
                      const PhiPalette& palette);
  void SyncHoverCardPopup(HDC dc, const Layout& layout);
  void PaintHoverCardPopup(HWND popup, HDC dc);
  static LRESULT CALLBACK HoverCardPopupProc(HWND hwnd,
                                             UINT msg,
                                             WPARAM wparam,
                                             LPARAM lparam);
  // The hovered row's page-snapshot card (the Mac's TabPreviewController).
  // Unlike the Space card above it floats in its own popup, so it can hang
  // over the Chromium child window.
  std::unique_ptr<PhiTabPreview> tab_preview_;
  // The native bookmark manager page, mounted over the content card while
  // the active tab sits on the bookmark route (the Mac's URL-driven mount).
  std::unique_ptr<PhiBookmarkManager> bookmark_manager_;
  void SyncBookmarkManager();
  bool WantsBookmarkPage() const;
  // The native new-tab page, mounted over the content card while the active
  // tab sits on a new-tab URL (or the browser is in placeholder mode) — the
  // Mac's NewTabViewController, URL-driven like the bookmark manager.
  std::unique_ptr<PhiNewTabPage> new_tab_page_;
  std::unique_ptr<PhiCreateSpacePanel> create_space_panel_;
  void SyncNativeNtp();
  bool WantsNativeNtp() const;
  // Focuses the page's omnibox unless the user is busy in another control.
  void FocusNativeNtpIfIdle();
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
  bool SplitIsStacked(int tab_id) const;
  // The split id holding `tab_id`, or empty when it is in none.
  std::string SplitIdForTab(int tab_id) const;
  // Re-orients the split holding the tab at `index` (a no-op for the
  // default side-by-side), used right after a stored split is recreated.
  void ApplySplitLayout(int index, const std::string& layout);
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
    int tab_id = 0;
    int tab_id_b = 0;
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
  void ShowBookmarkBarMenu(const RECT& anchor,
                           const std::string& parent_guid,
                           size_t skip_first);
  // The active tab's URL (captured by RebuildTabs) — "show on new tab"
  // needs to know whether the page IS the new-tab page.
  std::string active_tab_url_;
  int active_tab_id_ = 0;  // Its session id; 0 with no active tab.
  // The tab whose native new-tab page blanked on an omnibox commit; the
  // controls return once another tab is active or that load has ended.
  int ntp_commit_tab_id_ = 0;
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
  // The page changes AFTER the band slide completes (the Mac reveals the
  // target on completion): the entering child stays hidden and the leaving
  // one visible until then.
  HWND swap_reveal_ = nullptr;   // Shown when the slide lands.
  HWND swap_conceal_ = nullptr;  // Hidden when the slide lands.
  void RevealSwappedContent();
  bool swap_forward_ = true;
  // The strip's selected pill slides from the leaving pip to the entering
  // one during a switch (the Mac's matchedGeometryEffect glass chip),
  // riding the band slide's clock. Empty when idle.
  RECT swap_chip_from_{};
  ULONGLONG swap_start_ = 0;  // Band slide clock; 0 = idle.
  PhiPalette swap_source_palette_{};
  ULONGLONG ramp_start_ = 0;  // Theme ramp clock; 0 = idle.
  double SwapEasedProgress() const;  // Slide progress; 1.0 when idle.
  double RampEasedProgress() const;  // Theme progress; 1.0 when idle.
  void ComposeFrame(HDC mem);        // Chrome + drag indicator.
  void BeginSpaceSwap(const std::string& entering_space_id);
  void CancelSpaceSwap();
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
  // The current index of the tab tagged `tag`, or -1 when none is.
  void CloseStoredContent(const std::string& guid);
  int IndexOfTagged(int64_t browser, const std::string& tag) const;
  // Reopens or refocuses a STORED split (a pinned pair or a split
  // bookmark): missing panes are created, tags applied, and the pair
  // married — the Mac's openPinnedSplit / openTwoURLsAsSplit.
  void OpenStoredSplit(const ShortcutRow& row);

  // Navigation state for the client toolbar, refreshed with the tab list.
  std::wstring nav_url_;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
  bool nav_loading_ = false;
  // Copy-link acknowledgement: the glyph swaps to a checkmark for a second
  // after a copy (the Mac's triggeredSystemName/triggeredRevertDelay).
  bool copy_link_flash_ = false;
  // Tabs upstream's distiller says carry an article. Positive-only reports,
  // so entries are added and cleared when the tab navigates away.
  std::set<int> distillable_tabs_;
  // The address capsule's hover brighten (SideAddressBar hoveredColor:
  // resting 4% wash → emphasized 8%). Fed from BOTH sides — the shell's
  // mouse moves for the capsule's rim, the EDIT subclass for the text area
  // the child covers.
  bool url_field_hover_ = false;
  void SetUrlFieldHover(bool hovered);
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

  const int64_t window_id_;
  HWND browser_hwnd_;  // The VISIBLE Chromium window, reparented as our child.
  // Space id -> its browser window id / HWND, plus creation order.
  std::map<std::string, int64_t> space_browsers_;
  std::map<int64_t, HWND> browser_hwnds_;
  std::vector<std::string> space_order_;
  std::string active_space_;
  int64_t active_browser_ = 0;
  HWND shell_hwnd_ = nullptr;  // Our client-owned top-level.
  std::unique_ptr<PhiMenuBar> menu_bar_;  // Attached to shell_hwnd_.
  // The four content-corner caps (TL, TR, BL, BR), siblings above the
  // browser child — the only rounding DComp content respects.
  std::array<HWND, 4> corner_caps_ = {};
  // The Chrome-style link-target bubble at the content's bottom-left (the
  // Mac StatusURLView): a shell-child sibling raised over the browser child,
  // like the corner caps. Created on first use; text empty = hidden.
  HWND status_bubble_ = nullptr;
  std::wstring status_url_;
  void SetStatusUrl(const std::wstring& url);

  // --- Omnibox suggestion dropdown (the Mac OmniBox panel) -----------------
  // Rows mirror the framework's OnOmniboxResults json; the popup is a
  // non-activating WS_POPUP under the address field, so the EDIT keeps
  // focus and its caret while the list is up.
  struct OmniboxRow {
    int line = 0;
    std::string type;
    std::wstring contents;
    std::wstring description;
    std::wstring fill;  // fillIntoEdit — what arrowing onto the row types.
    bool has_tab_match = false;
  };
  std::vector<OmniboxRow> omnibox_rows_;
  int omnibox_selected_ = -1;  // -1 = nothing armed; Enter commits the text.
  HWND omnibox_popup_ = nullptr;
  bool url_field_programmatic_ = false;  // Suppresses EN_CHANGE feedback.
  // Set by StartNewTab in omnibox mode: the next commit opens a NEW tab.
  bool omnibox_new_tab_pending_ = false;
  // Browsers currently in placeholder mode (empty strip, window alive).
  std::set<int64_t> placeholder_browsers_;
  // Content-fullscreen state: the shell parks its chrome and covers the
  // monitor while a tab's content is fullscreen; placement and style are
  // restored on exit.
  bool content_fullscreen_ = false;
  int64_t fullscreen_browser_ = 0;
  WINDOWPLACEMENT pre_fullscreen_placement_ = {};
  LONG_PTR pre_fullscreen_style_ = 0;
  // Tab ids of the ACTIVE browser, most-recently-used first.
  std::vector<int> tab_mru_;
  void ShowOmniboxPopup();   // Creates/positions the popup under the field.
  void CloseOmniboxPopup(bool stop_query);
  void MoveOmniboxSelection(int delta);
  void CommitOmniboxSelection();
  int OmniboxRowAtY(int y) const;
  // The EDIT only repaints on its own events, so a palette change (Space
  // swap ramp, instant switch) must invalidate it or it keeps the old wash.
  void RepaintUrlField();
  // Owned by the framework (a singleton in chrome.dll); non-owning, and across
  // a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* const framework_;
  RAW_PTR_EXCLUSION PhiSlotHost* const host_;  // Owns this controller.
  // The Space list as drawn (id + whether this slot has it open).
  struct SpaceChip {
    std::string id;
    std::wstring label;
    std::wstring icon;  // The Space's emoji, empty when it has none.
    bool open = false;
    bool is_agent = false;  // A live agent Space pip; `id` is its taskId.
    RECT bounds{};
  };
  std::vector<SpaceChip> chips_;
  // The Mac's SpacesStripView pip viewport: when the row overflows, a
  // clipped window of `visible` whole pips slides along the FULL row,
  // widened by 65% of a pip at any edge with pips hidden past it so the
  // neighbour peeks through — the cue that the row continues. `chips_`
  // holds the windowed pips plus those peeked ones; `chips_first_index_`
  // is the row index of chips_[0], and `chips_clip_` the viewport rect
  // (empty when nothing is hidden) that bounds both paint and hit tests.
  int chips_start_ = 0;         // stripStartIndex: first whole pip shown.
  int chips_first_index_ = 0;
  RECT chips_clip_{};
  // The window re-centres on the active pip when the active Space or the
  // pip count changes (ensureActivePipVisible); wheel steps move it freely
  // in between, so both are remembered to tell the two apart.
  std::string chips_anchor_space_;
  int chips_anchor_visible_ = -1;
  bool ChipHit(const SpaceChip& chip, POINT client_pt) const;
  void StepChipsViewport(int step);  // stepViewport: +1 toward the end.
  RECT add_space_button_{};
  // True when not every Space fit the strip: the trailing "+" becomes the
  // Mac's "…" (moreButton) and opens the full switcher menu instead.
  bool chips_overflow_ = false;
  // The Mac's SpacesStripView shows its "+" only while the pointer is over
  // the strip's ROW (slot.isStripRowHovered): the whole row is the hover
  // region, so the button is already up by the time the cursor could reach
  // its far-right slot, and the slot stays reserved so the pips never
  // reflow. The overflow "…" is never gated. A flip fades the button over
  // kAddSpaceFadeMs (the Mac's 0.15s easeInOut on the button's opacity).
  bool strip_row_hovered_ = false;
  ULONGLONG add_space_fade_start_ = 0;  // Fade clock; 0 = settled.
  void SetStripRowHovered(bool hovered);
  double AddSpaceRevealProgress() const;  // 0 = hidden … 1 = fully shown.
  // The create-Space form's host state — the Mac SpaceWindowSlot's
  // isCreatingSpace / creatingSpaceIconValue: the strip reserves a dashed
  // placeholder pip at its end, hides the active pill and turns read-only.
  // The palette override is the form's live theme preview, kept through the
  // created Space's arrival so the chrome never flashes the source theme.
  bool creating_space_ = false;
  std::wstring creating_space_icon_text_;
  RECT creating_pip_bounds_{};
  std::optional<PhiPalette> preview_palette_;
  bool preview_until_switch_ = false;
  void EndCreateSpacePreview();
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
