// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_PEEK_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_PEEK_PANEL_H_

#include <windows.h>

#include <cstdint>
#include <string>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_surface_registry.h"

namespace phi_framework {
class PhiFramework;
}

namespace phi_client {

class PhiCoordinator;
class PhiWindowController;
class PhiWindowModel;

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
class PhiPeekPanel : public PhiSurfacePanel {
 public:
  // `model` is the coordinator's window model: the peek's own url is read
  // from its browser's snapshot there (docs/architecture-review.md §3.3).
  PhiPeekPanel(int64_t window_id,
               HWND browser_hwnd,
               int64_t opener_window_id,
               int64_t opener_tab_id,
               PhiWindowController* opener,
               phi_framework::PhiFramework* framework,
               const PhiWindowModel* model,
               PhiCoordinator* coordinator);
  ~PhiPeekPanel() override;

  PhiPeekPanel(const PhiPeekPanel&) = delete;
  PhiPeekPanel& operator=(const PhiPeekPanel&) = delete;

  // PhiSurfacePanel. The "opener" is the surface's origin.
  SidebarSurface kind() const override;
  int64_t window_id() const override;
  int64_t origin_window_id() const override;
  int64_t origin_tab_id() const override;
  int64_t opener_window_id() const { return opener_window_id_.value; }
  int64_t opener_tab_id() const { return opener_tab_id_; }
  // The card's own window (the peek's browser is reparented inside it) —
  // what the click-outside test compares against.
  HWND hwnd() const override;

  // The opener window moved or resized: re-fit the card to its content rect.
  void Reposition() override;

  // The opener switched tabs: a peek belongs to the tab that opened it, so it
  // shows only while that tab is active.
  void SetVisibleForActiveTab(int64_t active_tab_id) override;

  // The framework says this peek's page moved: re-read the url for the title
  // strip and repaint.
  void RefreshState() override;

  // Shadow first, then the card over it — both above the opener's page.
  void Raise() override;

  static LRESULT CALLBACK WndProc(HWND hwnd,
                                  UINT msg,
                                  WPARAM wparam,
                                  LPARAM lparam);
  static LRESULT CALLBACK ControlsWndProc(HWND hwnd,
                                          UINT msg,
                                          WPARAM wparam,
                                          LPARAM lparam);

 private:
  LRESULT Handle(UINT msg, WPARAM wparam, LPARAM lparam);

  struct Layout {
    RECT card;     // The page, edge to edge.
    RECT gutter;   // The control column floating over the page's top-right.
    RECT close;
    RECT expand;   // Promote the preview into a tab of the opener window.
    RECT split;    // ...and put it beside the tab that opened it.
  };
  Layout ComputeLayout() const;

  void Paint();
  void OnMouseMove(POINT pt);
  void OnClick(POINT pt);
  void PositionContent();
  void Dismiss();
  // The control chips live in a window of their OWN, above the hosted page:
  // a child window always paints over its parent, so chips drawn by the panel
  // would be buried under the page they belong to.
  void PositionControls();
  void PaintControls();
  LRESULT HandleControls(UINT msg, WPARAM wparam, LPARAM lparam);
  // Rounds the card. The hosted browser is a child of this window, so the
  // region that clips it is this one's.
  void ApplyCardShape();
  // Re-renders and repositions the drop shadow under the card.
  void UpdateShadow();

  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }

  BrowserId window_id_;  // The peek's own browser.
  HWND hwnd_ = nullptr;
  HWND browser_hwnd_ = nullptr;
  // The card's drop shadow: a LAYERED sibling beneath the panel. A plain
  // child window is opaque and could only shade its own background, so the
  // blur would never reach the opener's page.
  HWND shadow_ = nullptr;
  // The close / new-tab / split chips, a child of this window stacked above
  // the hosted browser.
  HWND controls_ = nullptr;
  BrowserId opener_window_id_;  // The browser the peek was opened FROM.
  int64_t opener_tab_id_ = 0;
  RAW_PTR_EXCLUSION PhiWindowController* opener_ = nullptr;
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_ = nullptr;
  RAW_PTR_EXCLUSION const PhiWindowModel* model_ = nullptr;
  RAW_PTR_EXCLUSION PhiCoordinator* coordinator_ = nullptr;
  int dpi_ = 96;

  std::wstring url_;
  int hot_ = -1;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_PEEK_PANEL_H_
