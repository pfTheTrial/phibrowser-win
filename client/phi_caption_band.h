// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CAPTION_BAND_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CAPTION_BAND_H_

#include <windows.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_shell_layout.h"
#include "phibrowser-win/client/phi_theme.h"

// The shell window's CAPTION BAND (docs/architecture-review.md §3.5, lane 1
// cluster C): the row that replaced the OS caption. It owns
//
//   * the menu-bar host — `PhiMenuBar` lives here, and nowhere else,
//   * the measured menu-title rects and which title is hot,
//   * the reveal state machine (the vertical layouts hide the titles at rest
//     and slide them in on hover or Alt; the Alt reveal is sticky),
//   * which caption button is hot and which is pressed,
//   * the custom frame itself: WM_NCCALCSIZE, WM_NCHITTEST and the
//     WM_NC*BUTTON* / WM_NCMOUSE* family.
//
// The shell forwards messages here and asks the band to paint its rect; it
// keeps the window, the fonts, the palette and the layout, which the band
// reads back through `PhiCaptionShell`. There is no `friend` in either
// direction.

namespace phi_client {

class PhiMenuBar;
class PhiMenuBarShell;

// The band's geometry for one turn, taken from the shell's Layout. Passing a
// value (rather than the shell's whole layout type) keeps the band out of
// the controller's header and makes what it actually reads explicit.
struct PhiCaptionRects {
  PhiLayoutMode mode = PhiLayoutMode::kPerformance;
  RECT title_bar{};
  RECT caption_min{}, caption_max{}, caption_close{};
  // The hairline closing the band's trailing cluster.
  RECT band_divider{};
  // The band's own controls: they take their own clicks, so the drag space
  // must not swallow them.
  RECT back{}, forward{}, reload{}, search_tabs{};
  // Comfortable's leading Phi button (the logo alone). Empty elsewhere.
  RECT phi_menu{};
  // The x the menu titles may not cross.
  int band_limit = 0;
  // Comfortable drops its caption glyphs onto the tab strip's line.
  int tabs_top = 0;
  bool horizontal_tabs = false;
};

// What the band asks of the shell that owns it.
class PhiCaptionShell {
 public:
  virtual HWND CaptionShellWindow() const = 0;
  virtual int CaptionScaled(int design_units) const = 0;
  virtual UINT CaptionDpi() const = 0;
  // The shell's UI face, built for the current DPI (the band measures its
  // titles with it and draws them in it).
  virtual HFONT CaptionTextFont() = 0;
  // Segoe MDL2 Assets, for the three caption glyphs.
  virtual HFONT CaptionGlyphFont() const = 0;
  virtual PhiPalette CaptionPalette() const = 0;
  virtual PhiCaptionRects CaptionRects() = 0;
  // Comfortable's band IS the tab strip: everything the strip drew there
  // takes its own clicks, and only the bare run past it drags the window.
  virtual bool CaptionStripTakesPoint(POINT client_pt) = 0;
  // The Phi button's hover wash comes from the shell's own hover state.
  virtual bool CaptionPhiMenuHovered() const = 0;
  // The Phi mark, drawn by the shell (it owns the icon cache).
  virtual void CaptionDrawLogo(HDC dc, int x, int y, int size) = 0;
  // The pointer entered the band: nothing the sidebar was washing is under
  // it any more.
  virtual void CaptionClearShellHover() = 0;
  // Fullscreen claims every pixel as client, borders included.
  virtual bool CaptionContentFullscreen() const = 0;
  // The narrow shell interface `PhiMenuBar` runs on (phi_menu_bar.h).
  virtual PhiMenuBarShell* CaptionMenuServices() = 0;

 protected:
  // The shell owns the band, never the reverse.
  ~PhiCaptionShell() = default;
};

class PhiCaptionBand {
 public:
  explicit PhiCaptionBand(PhiCaptionShell* shell);
  ~PhiCaptionBand();

  PhiCaptionBand(const PhiCaptionBand&) = delete;
  PhiCaptionBand& operator=(const PhiCaptionBand&) = delete;

  // Builds the menu bar on the shell window. Called once at creation.
  void Install(HWND shell_hwnd);
  // Recreates the bar from the CURRENT language: the HMENUs cache every
  // string as wide text, so a language change cannot be repainted. The
  // measured title rects go with it.
  void Rebuild();
  PhiMenuBar* menu_bar() { return menu_bar_.get(); }

  // Paints the band's own contents over the ground the shell already laid:
  // the titles, the trailing hairline, and the three caption buttons —
  // those only when `with_buttons`.
  void Paint(HDC dc, bool with_buttons = true);
  // The three caption buttons alone (their hot fill and glyphs, no ground):
  // the comfortable Space slide paints them last, standing still over the
  // chrome sliding under them, the way the Mac's traffic lights do.
  void PaintButtons(HDC dc);

  // What a CLIENT point in the band hits: HTCLOSE / HTMAXBUTTON /
  // HTMINBUTTON, HTCLIENT over a menu title or a control, HTCAPTION anywhere
  // else in the band, 0 below it.
  int HitTest(POINT client_pt);

  // --- The custom frame ----------------------------------------------------
  // Each returns true when it handled the message (`result` then carries the
  // return value where one is needed).
  bool OnNcCalcSize(HWND hwnd, WPARAM wparam, LPARAM lparam, LRESULT* result);
  bool OnNcHitTest(HWND hwnd, LPARAM lparam, LRESULT* result);
  bool OnNcLButtonDown(WPARAM hit);
  bool OnNcLButtonUp(HWND hwnd, WPARAM hit);
  // The non-client move: the caption buttons' hover, and where the hidden
  // menu titles come out (most of the band hit-tests as HTCAPTION, so the
  // pointer crossing it never arrives as WM_MOUSEMOVE).
  void OnNcMouseMove(WPARAM hit, POINT client_pt);
  void OnNcMouseLeave();
  // The client-area mirrors of the two above.
  void OnMouseMove(POINT client_pt);
  void OnMouseLeave();
  // Alt's summoned titles go back down when the window deactivates, as
  // Windows' own Alt-revealed bar does.
  void OnWindowDeactivated();
  // The Alt tap (kMsgPhiToggleMenuReveal).
  void ToggleAltReveal();

  // A click in the chrome. True when the band took it: the Phi button, a
  // menu title, or the band's drag space in a vertical layout.
  bool OnClick(POINT client_pt);
  // The Phi button's press, in flight. The corner button is a menu AND a
  // grip: OnClick only HOLDS the press, the menu drops when it comes up in
  // place (OnPressUp), and the press becomes the frame's own window move as
  // soon as the pointer travels past the system drag threshold
  // (OnPressMove). Each returns true when the held press took the message;
  // CancelPress drops it when the capture goes somewhere else.
  bool OnPressMove(POINT client_pt);
  bool OnPressUp();
  void CancelPress();
  // Comfortable's overflow button (Chrome's ⋮), anchored under the toolbar.
  void ShowOverflow(POINT client_anchor);

  // WM_INITMENUPOPUP / WM_COMMAND, forwarded to the bar.
  void OnMenuOpening(HMENU menu);
  bool OnCommand(UINT id);

 private:
  // The titles, measured for the current DPI: one rect per top-level menu,
  // in bar order.
  void EnsureTitleRects();
  // How many of `titles_` are live right now, from the leading edge. The
  // vertical layouts show the Phi button alone at rest and the whole bar
  // once the band is revealed; the count is then clipped to `band_limit` so
  // a narrow window drops the titles that would run under the trailing
  // controls rather than painting them there.
  size_t VisibleTitles(const PhiCaptionRects& rects);
  // Pointer-driven reveal of the hidden titles. Repaints the band on a
  // change; Alt's own sticky reveal is untouched.
  void SetRevealed(bool revealed);
  // Re-derives the reveal from where the cursor ACTUALLY is. The two leave
  // messages do not mean "the pointer left the band" — a reveal turns the
  // band under the pointer from HTCAPTION into HTCLIENT, which fires
  // WM_NCMOUSELEAVE with the pointer standing still — so they ask this
  // instead of hiding outright.
  void UpdateRevealFromCursor();
  // Same idea for the hot title: after a dropped menu closes the pointer may
  // be somewhere the shell never saw it move to.
  void UpdateHotTitleFromCursor();
  // Is this point the leading Phi button — comfortable's logo, or the first
  // title (logo + "Phi") in the vertical layouts?
  bool OnPhiButton(const PhiCaptionRects& rects, POINT client_pt);
  // Drops the menu that lives under `client_pt`, if any. The click path and
  // the Phi button's release share it.
  bool OpenMenuAt(const PhiCaptionRects& rects, POINT client_pt);
  void SetCaptionHot(int code);
  void SetHotTitle(int index);
  void InvalidateBand();

  RAW_PTR_EXCLUSION PhiCaptionShell* const shell_;  // Owns this.
  HWND shell_hwnd_ = nullptr;
  std::unique_ptr<PhiMenuBar> menu_bar_;

  // Which caption button is hot / pressed (an HT* code, 0 = none) and which
  // menu title is hot (-1 = none). Purely visual except `pressed_caption_`,
  // which arms the button-up action.
  int hot_caption_ = 0;
  int pressed_caption_ = 0;
  // The vertical layouts keep the titles hidden at rest — the band's left
  // run is drag space — and slide them in when the pointer enters the band
  // or Alt asks for them. The Alt reveal is sticky: it survives the pointer
  // leaving, and drops on a menu opening or when the window loses focus, the
  // way Windows' own Alt-summoned menu bar does.
  bool revealed_ = false;
  bool alt_revealed_ = false;
  // A menu is dropped from the band right now. The popup takes the pointer
  // with it, so the leave messages that follow must not collapse the bar out
  // from under the menu the user is reading.
  bool menu_open_ = false;
  int hot_title_ = -1;
  // The Phi button is held right now, and the client point the press landed
  // on: what the press becomes is decided from there (OnPressMove).
  bool phi_pressed_ = false;
  POINT phi_press_pt_ = {};
  // Titles with their band rects, measured for `titles_dpi_`.
  std::vector<std::pair<std::wstring, RECT>> titles_;
  UINT titles_dpi_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CAPTION_BAND_H_
