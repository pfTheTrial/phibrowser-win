// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_KIOSK_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_KIOSK_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_theme.h"

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
// The chrome is ONE band, the Mac's KioskBrowserToolbar: the profile chip,
// the page's domain and the extensions/copy actions ride inside a single
// address capsule, with "Open in <Space>" trailing it. The Mac spends its
// leading 78 points clearing the traffic lights; here that room is at the
// TRAILING edge instead, because the window controls are Windows' own —
// minimize / maximize / close, drawn by this class after WM_NCCALCSIZE
// retires the OS caption, so the band is title bar and toolbar at once.
//
// Not ported from the Mac (deliberately, and not needed for the window to
// work): the cursor-zoom open animation, the global shortcut, frame autosave,
// the omnibox behind the address capsule (this client has none outside the
// shell, so the capsule shows the domain and takes no clicks), and switching
// the window's profile from the chip (the Mac rebuilds the Kiosk in the
// chosen profile; no framework call opens a Kiosk, so the chip names the
// profile without offering the menu). See windows-framework-status.md.
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
    RECT band;  // Title bar and toolbar in one row.
    RECT caption_min;
    RECT caption_max;
    RECT caption_close;
    RECT back;
    RECT address;  // The capsule holding the three below.
    RECT profile;  // The profile chip inside it.
    RECT url;
    RECT extensions;
    RECT copy;
    RECT space_menu;     // "Open in <Space>", the whole capsule.
    RECT space_primary;  // Its opening half.
    RECT space_chevron;  // Its Space-list half.
    RECT content;
  };
  Layout ComputeLayout();

  void EnsureFonts();
  int TextWidth(HFONT font, const std::wstring& text);
  // Re-measures the band's variable-width labels. Measuring in
  // ComputeLayout instead would put four text extents behind every
  // WM_NCHITTEST, and the window sends one per mouse move.
  void MeasureLabels();
  // The Mac's URLProcessor.displayName: the www.-stripped host, and nothing
  // at all for about:blank, the new-tab page or an extension page.
  std::wstring DisplayUrl() const;
  const PhiPalette& Palette() const;

  void Paint();
  void OnMouseMove(POINT pt);
  void OnClick(POINT pt);
  void PositionContent();
  void RefreshProfile();
  void RefreshSpaces();
  void CopyUrl();
  // Hands the page to `space_id` and lets this ephemeral window go — the
  // Mac's Kiosk space menu, and what the old expand button did.
  void OpenInSpace(const std::string& space_id);
  void ShowSpaceList(const RECT& anchor);
  // HTCLOSE / HTMAXBUTTON / HTMINBUTTON for the window controls, HTCLIENT for
  // the band's own controls, HTCAPTION for the rest of it; 0 outside the band.
  int CaptionHitTest(POINT client_pt);
  void SetCaptionHot(int code);

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

  // The profile this window's browser belongs to, for the chip.
  std::wstring profile_name_;

  // The Space list behind the trailing menu, and the one it offers by
  // default (the Mac's KioskSpaceMenuTargetResolver).
  struct SpaceRow {
    std::string id;
    std::wstring name;
    std::wstring icon;  // Resolved display glyph: catalog stand-in or emoji.
  };
  std::vector<SpaceRow> spaces_;
  std::string target_space_id_;
  const SpaceRow* TargetSpace() const;

  // The copy button wears a checkmark for a second after a copy.
  bool copy_flash_ = false;

  // Which toolbar control the cursor is over, for the hover wash. Indexes
  // the same order Layout declares them; -1 for none.
  int hot_ = -1;
  // The window control under the pointer, and the one a press is holding —
  // both HT* codes, 0 for none. They arrive as NON-client messages, so they
  // cannot ride `hot_`.
  int hot_caption_ = 0;
  int pressed_caption_ = 0;

  // Label widths, from MeasureLabels.
  int profile_text_w_ = 0;
  int space_prefix_w_ = 0;
  int space_name_w_ = 0;

  // Rebuilt whenever the DPI moves; owned here.
  HFONT ui_font_ = nullptr;
  HFONT ui_strong_font_ = nullptr;
  HFONT glyph_font_ = nullptr;
  HFONT caption_font_ = nullptr;
  int font_dpi_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_KIOSK_WINDOW_H_
