// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"

namespace phi_client {

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
                      phi_framework::PhiFramework* framework);
  ~PhiWindowController();

  PhiWindowController(const PhiWindowController&) = delete;
  PhiWindowController& operator=(const PhiWindowController&) = delete;

  int64_t window_id() const { return window_id_; }

  // Re-reads tab/nav state from the framework and repaints the sidebar. Called
  // on the UI thread when the framework reports a change.
  void RefreshTabs();

  // Rebuilds `tabs_` from framework state WITHOUT invalidating (paint-time
  // use; RefreshTabs() would recurse through InvalidateRect).
  void RebuildTabs();

  // Window procedure for the client shell window.
  static LRESULT CALLBACK ShellWndProc(HWND hwnd,
                                       UINT msg,
                                       WPARAM wparam,
                                       LPARAM lparam);

 private:
  void AdoptBrowserAsChild();
  void LayoutContents();
  void PaintSidebar(HDC dc, const RECT& client);
  // Handles a click in the sidebar (tab rows, close boxes, buttons).
  void OnSidebarClick(int x, int y);

  // One rendered tab row: what to draw and what a click at that row does.
  struct TabRow {
    int index = 0;
    bool active = false;
    std::wstring title;
    RECT bounds{};       // Row rect in client coords.
    RECT close_box{};    // The row's close affordance.
  };
  std::vector<TabRow> tabs_;

  const int64_t window_id_;
  HWND browser_hwnd_;  // Chromium's window, reparented as our child.
  HWND shell_hwnd_ = nullptr;  // Our client-owned top-level.
  // Owned by the framework (a singleton in chrome.dll); non-owning, and across
  // a module boundary, so not a raw_ptr.
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* const framework_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_WINDOW_CONTROLLER_H_
