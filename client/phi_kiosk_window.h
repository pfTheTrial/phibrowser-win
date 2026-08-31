// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_KIOSK_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_KIOSK_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

class PhiCoordinator;

// The Mac's Kiosk browser window (UserInterface/Kiosk): an ephemeral,
// single-tab window for a link opened outside the user's Spaces — an external
// link, a URL rule routed to Kiosk, or the "Open Link in Kiosk" context-menu
// item. Deliberately minimal: no sidebar, no tab list, no Spaces, just a
// compact toolbar over the page.
//
// The FRAMEWORK owns the browser (a popup-backed Browser created with
// is_kiosk, see phi_kiosk_window.cc in the fork); this class only owns the
// native window that hosts it. It is built the same way as the main shell —
// Chromium's HWND is restyled into a WS_CHILD and clipped into the content
// rect — but from a much smaller controller, because none of the shell's
// sidebar/tab machinery applies to a window that can only ever hold one tab.
//
// Not ported from the Mac (deliberately, and not needed for the window to
// work): the cursor-zoom open animation, the global shortcut, and frame
// autosave. See windows-framework-status.md.
class PhiKioskWindow {
 public:
  PhiKioskWindow(int64_t window_id,
                 HWND browser_hwnd,
                 phi_framework::PhiFramework* framework,
                 PhiCoordinator* coordinator);
  ~PhiKioskWindow();

  PhiKioskWindow(const PhiKioskWindow&) = delete;
  PhiKioskWindow& operator=(const PhiKioskWindow&) = delete;

  int64_t window_id() const { return window_id_; }
  HWND hwnd() const { return hwnd_; }

  // The framework says this window's tab or navigation state moved: re-read
  // the title/URL/can-go-back and repaint the toolbar.
  void RefreshState();

 
  static LRESULT CALLBACK WndProc(HWND hwnd,
                                  UINT msg,
                                  WPARAM wparam,
                                  LPARAM lparam);

 private:
  LRESULT Handle(UINT msg, WPARAM wparam, LPARAM lparam);

  // Toolbar geometry, all in physical pixels for the current DPI.
  struct Layout {
    RECT toolbar;
    RECT back;
    RECT forward;
    RECT reload;
    RECT url;
    RECT expand;  // Hand the page to a real tab in the main window.
    RECT close;
    RECT content;
  };
  Layout ComputeLayout() const;

  void Paint();
  void OnMouseMove(POINT pt);
  void OnClick(POINT pt);
  void PositionContent();

  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }

  int64_t window_id_ = 0;
  HWND hwnd_ = nullptr;
  HWND browser_hwnd_ = nullptr;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  RAW_PTR_EXCLUSION PhiCoordinator* coordinator_ = nullptr;
  int dpi_ = 96;

  std::wstring title_;
  std::wstring url_;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
  bool loading_ = false;

  // Which toolbar control the cursor is over, for the hover wash. Indexes
  // the same order Layout declares them; -1 for none.
  int hot_ = -1;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_KIOSK_WINDOW_H_
