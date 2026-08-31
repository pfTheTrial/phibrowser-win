// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_PEEK_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_PEEK_PANEL_H_

#include <windows.h>

#include <cstdint>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

class PhiCoordinator;
class PhiWindowController;

// The Mac's floating Peek panel (UserInterface/Peek/PeekPanelController): a
// link opened from a bound tab previews in a card floating over the page
// pane — the sidebar stays usable — instead of taking a tab. Closing (X, Esc,
// a click on the page around it) or expanding ends the peek.
//
// Geometry follows the Mac's spec: the card insets from the content card by
// 4% of its width (at least 16 DIP) horizontally and 14 DIP vertically, with
// a 44-DIP control gutter kept to its right, over the page. The card itself
// carries no chrome — the page fills it edge to edge (Arc's peek layout).
//
// WHAT THE CARD HOSTS DIVERGES FROM THE MAC, deliberately: there a peek is a
// live tab in the opener's own strip whose view is re-parented into the
// panel, which works because the Mac client hosts WebContents views directly.
// This client hosts a whole BrowserView per window and a non-active tab is
// not independently rendered, so the peek is its own browser (see the fork's
// phi_peek_window.h) whose window is parented into this panel. The panel is a
// WS_CHILD of the opener's shell — the bookmark manager's trick — so it
// clips into the content card and travels with the window.
class PhiPeekPanel {
 public:
  PhiPeekPanel(int64_t window_id,
               HWND browser_hwnd,
               int64_t opener_window_id,
               int64_t opener_tab_id,
               PhiWindowController* opener,
               phi_framework::PhiFramework* framework,
               PhiCoordinator* coordinator);
  ~PhiPeekPanel();

  PhiPeekPanel(const PhiPeekPanel&) = delete;
  PhiPeekPanel& operator=(const PhiPeekPanel&) = delete;

  int64_t window_id() const { return window_id_; }
  int64_t opener_window_id() const { return opener_window_id_; }
  int64_t opener_tab_id() const { return opener_tab_id_; }

  // The opener window moved or resized: re-fit the card to its content rect.
  void Reposition();

  // The opener switched tabs: a peek belongs to the tab that opened it, so it
  // shows only while that tab is active.
  void SetVisibleForActiveTab(int64_t active_tab_id);

  // The framework says this peek's page moved: re-read the url for the title
  // strip and repaint.
  void RefreshState();

  static LRESULT CALLBACK WndProc(HWND hwnd,
                                  UINT msg,
                                  WPARAM wparam,
                                  LPARAM lparam);

 private:
  LRESULT Handle(UINT msg, WPARAM wparam, LPARAM lparam);

  struct Layout {
    RECT card;     // The page, edge to edge.
    RECT gutter;   // The control strip kept to the card's right.
    RECT close;
    RECT expand;
  };
  Layout ComputeLayout() const;

  void Paint();
  void OnMouseMove(POINT pt);
  void OnClick(POINT pt);
  void PositionContent();
  void Dismiss();

  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }

  int64_t window_id_ = 0;
  HWND hwnd_ = nullptr;
  HWND browser_hwnd_ = nullptr;
  int64_t opener_window_id_ = 0;
  int64_t opener_tab_id_ = 0;
  RAW_PTR_EXCLUSION PhiWindowController* opener_ = nullptr;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  RAW_PTR_EXCLUSION PhiCoordinator* coordinator_ = nullptr;
  int dpi_ = 96;

  std::wstring url_;
  int hot_ = -1;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_PEEK_PANEL_H_
