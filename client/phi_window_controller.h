// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"

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

// What a slot needs from its owner (the coordinator): the full Space list to
// draw, and requests that only the owner can service (switching to a Space
// that has no browser yet must create one in that Space's profile).
class PhiSlotHost {
 public:
  virtual ~PhiSlotHost() = default;
  // All Spaces as a JSON array string: [{"id","name","profileId"},…]
  virtual std::string AllSpacesJson() const = 0;
  virtual void RequestSwitchSpace(int64_t slot, const std::string& space_id) = 0;
  // Creates a Space in this slot's current profile (`new_profile` = false) or
  // in a freshly created profile (true), then switches to it.
  virtual void RequestCreateSpace(int64_t slot, bool new_profile) = 0;
  // The layout every slot renders in — one setting for the whole client, as on
  // Mac, so a new window never disagrees with the one beside it.
  virtual PhiLayoutMode LayoutMode() const = 0;
  virtual void RequestLayoutMode(PhiLayoutMode mode) = 0;
};

// Owns the client-drawn shell window for one browser (the Windows twin of the
// Mac MainBrowserWindowController), using the Option-B model
// (windows-framework-design.md §2.4): the CLIENT owns a top-level window and
// its own painting, and Chromium's browser window is reparented in as a
// WS_CHILD filling the content rect. The client's sidebar is drawn into the
// client's own window (no DirectComposition conflict, unlike the falsified
// overlay approach), while web content renders via DComp inside the reparented
// child's own HWND — two non-overlapping regions, both visible.
class PhiWindowController {
 public:
  PhiWindowController(int64_t window_id,
                      HWND browser_hwnd,
                      phi_framework::PhiFramework* framework,
                      PhiSlotHost* host);
  ~PhiWindowController();

  PhiWindowController(const PhiWindowController&) = delete;
  PhiWindowController& operator=(const PhiWindowController&) = delete;

  int64_t window_id() const { return window_id_; }

  // Re-reads tab/nav state from the framework and repaints the sidebar NOW.
  // For changes the client itself just made, where the browser state has
  // already settled.
  void RefreshTabs();

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
  const std::string& active_space() const { return active_space_; }
  // The browser window id currently displayed (for tab/nav commands).
  int64_t active_browser() const { return active_browser_; }
  // Spaces this slot has a browser for, in creation order.
  const std::vector<std::string>& space_order() const { return space_order_; }
  // Drops a browser that went away; returns true if the slot still has one.
  bool ForgetBrowser(int64_t browser_window_id);
  // Starts closing the whole slot (its Space browsers, one per Space). MAY
  // DESTROY `this` before returning — the last browser going away tears the
  // slot down — so touch nothing on this object afterwards.
  void RequestClose();
  // Gives the currently shown browser Win32 focus and Chromium activation.
  void FocusActiveBrowser();

  // Re-lays the chrome for `mode` and repaints. Cheap: all geometry is derived
  // from the client rect on demand, so nothing is cached across the switch.
  void ApplyLayoutMode(PhiLayoutMode mode);
  // The content region (client coords) the browser child occupies.
  RECT content_rect() const;

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
  // Every rect the chrome needs, for one layout mode. Derived from the client
  // rect on demand rather than stored, so a resize or a mode switch needs no
  // invalidation of its own.
  struct Layout {
    PhiLayoutMode mode = PhiLayoutMode::kPerformance;
    RECT client{};
    RECT sidebar{};  // Empty in Comfortable.
    RECT top_bar{};  // The band above the content; empty in Performance.
    RECT content{};  // Where the reparented browser child goes.
    RECT url_field{};
    RECT back{}, forward{}, reload{};
    RECT new_window{};
    RECT layout_button{};  // Cycles the mode.
    RECT add_space{}, add_profile_space{};
    int chips_left = 0, chips_top = 0;
    int chips_limit = 0;  // Right edge chips may not cross.
    int tabs_left = 0, tabs_top = 0;
    int tabs_limit = 0;   // Bottom (vertical) or right (horizontal) edge.
    bool horizontal_tabs = false;
  };
  Layout ComputeLayout() const;

  void AdoptBrowserAsChild();
  void LayoutContents();
  // Moves the address field's EDIT child into `layout`'s slot for it.
  void PositionUrlField(const Layout& layout);
  // `include_child_controls` draws what child windows would draw themselves —
  // set only on the WM_PRINTCLIENT (PrintWindow) path, which renders the
  // parent alone. In WM_PAINT the URL field's EDIT child paints that region.
  void PaintChrome(HDC dc, const Layout& layout, bool include_child_controls);
  // Handles a click in the chrome (tab rows, close boxes, buttons, toolbar).
  void OnChromeClick(int x, int y);
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
  // Navigates to whatever is typed, then hands focus back to the page.
  void CommitUrlField();
  // Subclass proc: Enter commits, Escape reverts, focus selects all.
  static LRESULT CALLBACK UrlFieldProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wparam,
                                       LPARAM lparam);
  HWND url_field_ = nullptr;
  HFONT url_font_ = nullptr;

  // One rendered tab row: what to draw and what a click at that row does.
  struct TabRow {
    int index = 0;
    bool active = false;
    std::wstring title;
    RECT bounds{};       // Row rect in client coords.
    RECT close_box{};    // The row's close affordance.
  };
  std::vector<TabRow> tabs_;

  // Navigation state for the client toolbar, refreshed with the tab list.
  std::wstring nav_url_;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
  bool nav_loading_ = false;

  const int64_t window_id_;
  HWND browser_hwnd_;  // The VISIBLE Chromium window, reparented as our child.
  // Space id -> its browser window id / HWND, plus creation order.
  std::map<std::string, int64_t> space_browsers_;
  std::map<int64_t, HWND> browser_hwnds_;
  std::vector<std::string> space_order_;
  std::string active_space_;
  int64_t active_browser_ = 0;
  HWND shell_hwnd_ = nullptr;  // Our client-owned top-level.
  // Owned by the framework (a singleton in chrome.dll); non-owning, and across
  // a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* const framework_;
  RAW_PTR_EXCLUSION PhiSlotHost* const host_;  // Owns this controller.
  // The Space list as drawn (id + whether this slot has it open).
  struct SpaceChip {
    std::string id;
    std::wstring label;
    bool open = false;
    RECT bounds{};
  };
  std::vector<SpaceChip> chips_;
  RECT add_space_button_{};
  RECT add_profile_space_button_{};
  RECT layout_button_{};
  PhiLayoutMode layout_mode_ = PhiLayoutMode::kPerformance;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_
