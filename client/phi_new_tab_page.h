// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_NEW_TAB_PAGE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_NEW_TAB_PAGE_H_

#include <windows.h>

#include <string>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_client {

class PhiWindowController;

// The Mac's native new-tab page (UserInterface/WebContent/NativeNTP/
// NewTabViewController.swift): the Phi mark, the "Incognito" caption in an
// off-the-record window, and an omnibox centred in the content card — drawn
// by the client instead of Chromium's web NTP.
//
// URL-DRIVEN like the bookmark manager: the page is a child window mounted
// over the content card while the active tab sits on a new-tab URL, and
// unmounted the moment it navigates away. The tab under it is an ordinary
// browser tab (chrome://newtab keeps loading behind the page, so CDP and the
// sidebar see the same tab they always did); only what the user SEES is the
// native page. The Mac shows this page for incognito windows and its AI web
// NTP elsewhere; this client has no AI NTP, so every new tab gets it.
//
// The omnibox is a real EDIT owned by this page but driven by the window
// controller's ONE omnibox session (query, suggestion dropdown, commit) —
// the controller subclasses the field exactly like its own address field.
class PhiNewTabPage {
 public:
  explicit PhiNewTabPage(PhiWindowController* controller);
  ~PhiNewTabPage();

  PhiNewTabPage(const PhiNewTabPage&) = delete;
  PhiNewTabPage& operator=(const PhiNewTabPage&) = delete;

  // True for the new-tab URLs (the Mac String.isNTP, plus the WebUI spelling
  // Chromium itself redirects to).
  static bool IsNewTabUrl(const std::string& url);

  // Mounts the page over the content card in its clean state — controls
  // visible, omnibox empty. Focus is the controller's call.
  void Show();
  void Hide();
  bool visible() const;
  HWND hwnd() const { return hwnd_; }
  HWND field() const { return field_; }
  // True while the page is up WITH its omnibox showing — the field is then
  // where keyboard focus belongs (the Mac focuses it in viewDidAppear).
  bool WantsFocus() const;
  // True after HideControls, until Show or Reset.
  bool controls_hidden() const { return controls_hidden_; }

  // Follows the content card (and the monitor's DPI) on relayout.
  void Reposition();
  // Repaints — a palette change.
  void Refresh();
  // The Mac's omniBoxDidClear: a navigation was committed from the omnibox,
  // so the controls blank out until the rebuild unmounts the page. Nothing
  // of the old page flashes over the arriving one.
  void HideControls();
  // The Mac's resetToInitialState: a Space URL rule re-homed the navigation
  // and this tab is a new tab again — controls back, omnibox cleared.
  void Reset();
  void FocusField();
  // The omnibox box in SCREEN coordinates — the suggestion dropdown anchors
  // flush under it, as wide as it is.
  RECT BoxScreenRect() const;

 private:
  struct Geometry {
    RECT icon{};
    RECT label{};  // Empty outside incognito.
    RECT box{};
  };

  int Scaled(int design) const;
  void EnsureWindow();
  void EnsureFonts();
  void EnsureField();
  Geometry ComputeGeometry() const;
  void LayoutField();
  void Paint(HDC dc);

  static LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);

  RAW_PTR_EXCLUSION PhiWindowController* const controller_;
  HWND hwnd_ = nullptr;
  HWND field_ = nullptr;
  HFONT label_font_ = nullptr;
  HFONT field_font_ = nullptr;
  HFONT glyph_font_ = nullptr;
  UINT fonts_dpi_ = 0;
  // The EDIT's ground (WM_CTLCOLOREDIT must return a REAL brush).
  HBRUSH field_brush_ = nullptr;
  COLORREF field_brush_color_ = CLR_INVALID;
  UINT dpi_ = 96;
  bool controls_hidden_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_NEW_TAB_PAGE_H_
