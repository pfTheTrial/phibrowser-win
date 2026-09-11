// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_caption_band.h"

#include <windowsx.h>

#include <cstdlib>

#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_menu_bar.h"

namespace phi_client {

namespace {

// Each title's horizontal padding.
constexpr int kMenuTitlePad = 10;
// The Phi title's leading pad: its rect starts kPhiTitlePad short of the
// sidebar's edge inset, so the logo lands ON kEdgeInset — the left edge the
// Spaces pips, the pinned card and the tab rows below all share.
constexpr int kPhiTitlePad = 4;
// The logo's drawn edge, and the gap between it and the "Phi" label.
constexpr int kLogoSize = 18;
constexpr int kLogoTextGap = 6;

void FillRounded(HDC dc, const RECT& r, COLORREF fill, int radius) {
  // Every rounded shape goes through phi_dwrite: antialiased, and the TRUE
  // corner radius rather than GDI's corner ellipse.
  PhiFillRoundRectAA(dc, r, radius, fill);
}

void DrawGlyph(HDC dc, const RECT& r, const wchar_t* glyph, COLORREF ink) {
  if (r.right <= r.left) {
    return;
  }
  ::SetTextColor(dc, ink);
  PhiDrawTextAA(dc, glyph, -1, const_cast<RECT*>(&r),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

}  // namespace

PhiCaptionBand::PhiCaptionBand(PhiCaptionShell* shell) : shell_(shell) {}

PhiCaptionBand::~PhiCaptionBand() = default;

void PhiCaptionBand::Install(HWND shell_hwnd) {
  shell_hwnd_ = shell_hwnd;
  menu_bar_ = std::make_unique<PhiMenuBar>(shell_->CaptionMenuServices());
  menu_bar_->Install(shell_hwnd_);
}

void PhiCaptionBand::Rebuild() {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return;
  }
  menu_bar_ = std::make_unique<PhiMenuBar>(  // The old bar's HMENUs go with
      shell_->CaptionMenuServices());        // its destructor.
  menu_bar_->Install(shell_hwnd_);
  titles_.clear();  // Re-measured on the next paint, in the new language.
  titles_dpi_ = 0;
  ::InvalidateRect(shell_hwnd_, nullptr, TRUE);
}

void PhiCaptionBand::InvalidateBand() {
  if (!shell_hwnd_) {
    return;
  }
  const RECT band = shell_->CaptionRects().title_bar;
  ::InvalidateRect(shell_hwnd_, &band, FALSE);
}

// --- Titles ----------------------------------------------------------------

void PhiCaptionBand::EnsureTitleRects() {
  const UINT dpi = shell_->CaptionDpi();
  if (titles_dpi_ == dpi && !titles_.empty()) {
    return;
  }
  if (!menu_bar_ || !shell_hwnd_) {
    return;
  }
  const std::vector<std::wstring> titles = menu_bar_->Titles();
  if (titles.empty()) {
    return;
  }
  HFONT font = shell_->CaptionTextFont();
  HDC dc = ::GetDC(shell_hwnd_);
  if (!dc) {
    return;
  }
  HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
  titles_.clear();
  int x = shell_->CaptionScaled(shell_metrics::kEdgeInset) -
          shell_->CaptionScaled(kPhiTitlePad);
  const int band = shell_->CaptionScaled(shell_metrics::kTitleBarHeight);
  bool first = true;
  for (const std::wstring& title : titles) {
    SIZE extent = {};
    ::GetTextExtentPoint32W(dc, title.c_str(),
                            static_cast<int>(title.size()), &extent);
    int width = extent.cx + 2 * shell_->CaptionScaled(kMenuTitlePad);
    if (first) {
      // The logo and "Phi" are ONE button: the first title's rect carries
      // the icon (18) and its gap (6) ahead of the text, behind the shorter
      // leading pad that aligns the logo with the sidebar content.
      width += shell_->CaptionScaled(kPhiTitlePad) -
               shell_->CaptionScaled(kMenuTitlePad) +
               shell_->CaptionScaled(kLogoSize) +
               shell_->CaptionScaled(kLogoTextGap);
      first = false;
    }
    titles_.emplace_back(title, RECT{x, 0, x + width, band});
    x += width;
  }
  titles_dpi_ = dpi;
  if (previous) {
    ::SelectObject(dc, previous);
  }
  ::ReleaseDC(shell_hwnd_, dc);
}

size_t PhiCaptionBand::VisibleTitles(const PhiCaptionRects& rects) {
  EnsureTitleRects();
  if (titles_.empty()) {
    return 0;
  }
  // Comfortable gives the whole band to the tab strip and has no titles at
  // all — its band_limit is 0, so the clip below drops every one of them.
  // The vertical layouts show the Phi button alone until the band reveals.
  const bool all = revealed_ || alt_revealed_;
  const size_t shown = all ? titles_.size() : 1;
  size_t fits = 0;
  while (fits < shown && titles_[fits].second.right <= rects.band_limit) {
    ++fits;
  }
  return fits;
}

void PhiCaptionBand::SetRevealed(bool revealed) {
  if (!revealed && menu_open_) {
    return;  // A dropped menu holds the bar open until it closes.
  }
  if (revealed_ == revealed) {
    return;
  }
  revealed_ = revealed;
  if (!revealed) {
    hot_title_ = -1;  // Nothing under the pointer once the titles are gone.
  }
  InvalidateBand();
}

void PhiCaptionBand::UpdateRevealFromCursor() {
  POINT pt = {};
  if (!shell_hwnd_ || !::GetCursorPos(&pt)) {
    SetRevealed(false);
    return;
  }
  ::ScreenToClient(shell_hwnd_, &pt);
  const RECT band = shell_->CaptionRects().title_bar;
  SetRevealed(::PtInRect(&band, pt) != 0);
}

void PhiCaptionBand::UpdateHotTitleFromCursor() {
  POINT pt = {};
  if (!shell_hwnd_ || !::GetCursorPos(&pt)) {
    SetHotTitle(-1);
    return;
  }
  ::ScreenToClient(shell_hwnd_, &pt);
  const PhiCaptionRects rects = shell_->CaptionRects();
  int hot = -1;
  if (::PtInRect(&rects.title_bar, pt)) {
    const size_t titles = VisibleTitles(rects);
    for (size_t i = 0; i < titles; ++i) {
      if (::PtInRect(&titles_[i].second, pt)) {
        hot = static_cast<int>(i);
        break;
      }
    }
  }
  SetHotTitle(hot);
}

void PhiCaptionBand::SetCaptionHot(int code) {
  if (hot_caption_ == code) {
    return;
  }
  hot_caption_ = code;
  InvalidateBand();
}

void PhiCaptionBand::SetHotTitle(int index) {
  if (menu_open_ && index != hot_title_) {
    // A dropped menu owns the highlight until it closes. Its popup is modal,
    // so the shell sees no pointer motion meanwhile — anything trying to
    // move the highlight now is stale by definition.
    return;
  }
  if (hot_title_ == index) {
    return;
  }
  hot_title_ = index;
  InvalidateBand();
}

// --- Hit-testing -----------------------------------------------------------

int PhiCaptionBand::HitTest(POINT client_pt) {
  const PhiCaptionRects rects = shell_->CaptionRects();
  if (!::PtInRect(&rects.title_bar, client_pt)) {
    return 0;
  }
  if (::PtInRect(&rects.caption_close, client_pt)) {
    return HTCLOSE;
  }
  if (::PtInRect(&rects.caption_max, client_pt)) {
    return HTMAXBUTTON;
  }
  if (::PtInRect(&rects.caption_min, client_pt)) {
    return HTMINBUTTON;
  }
  // The band's own controls take their own clicks — the trailing cluster,
  // and comfortable's leading Phi button — without this the band would drag
  // the window out from under the pointer instead.
  if (::PtInRect(&rects.back, client_pt) ||
      ::PtInRect(&rects.forward, client_pt) ||
      ::PtInRect(&rects.reload, client_pt) ||
      ::PtInRect(&rects.search_tabs, client_pt) ||
      ::PtInRect(&rects.phi_menu, client_pt)) {
    return HTCLIENT;
  }
  if (rects.horizontal_tabs) {
    // Comfortable's band IS the tab strip. Everything drawn in it takes its
    // own clicks; the bare run past the last tab still drags the window,
    // which is how Chrome's strip behaves. The strip is the shell's, so the
    // shell answers for it.
    if (shell_->CaptionStripTakesPoint(client_pt)) {
      return HTCLIENT;
    }
  }
  const size_t titles = VisibleTitles(rects);
  for (size_t i = 0; i < titles; ++i) {
    if (::PtInRect(&titles_[i].second, client_pt)) {
      return HTCLIENT;  // A menu title takes the click itself.
    }
  }
  return HTCAPTION;  // Everything else in the band drags the window.
}

// --- The custom frame ------------------------------------------------------

bool PhiCaptionBand::OnNcCalcSize(HWND hwnd,
                                  WPARAM wparam,
                                  LPARAM lparam,
                                  LRESULT* result) {
  // Claim the caption for the client area: only the resize borders stay
  // non-client. The top edge keeps NO border — the band goes flush to it
  // (WM_NCHITTEST serves HTTOP from the band's first pixels). BOTH wparam
  // forms must be handled: the FIRST calc at creation comes with wparam
  // FALSE (a bare RECT), and skipping it leaves the OS caption alive — its
  // buttons then sit doubled above the band's.
  *result = 0;
  if (shell_->CaptionContentFullscreen()) {
    return true;  // Fullscreen: every pixel is client, no borders.
  }
  RECT* rect = wparam == TRUE
                   ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                   : reinterpret_cast<RECT*>(lparam);
  const UINT dpi = ::GetDpiForWindow(hwnd);
  const int border_x = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
  const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
  rect->left += border_x;
  rect->right -= border_x;
  rect->bottom -= border_y;
  if (::IsZoomed(hwnd)) {
    rect->top += border_y;  // Maximized frames overhang the monitor.
  }
  return true;
}

bool PhiCaptionBand::OnNcHitTest(HWND hwnd, LPARAM lparam, LRESULT* result) {
  const LRESULT hit = ::DefWindowProcW(hwnd, WM_NCHITTEST, 0, lparam);
  if (hit != HTCLIENT) {
    *result = hit;  // The real resize borders.
    return true;
  }
  POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  ::ScreenToClient(hwnd, &pt);
  const UINT dpi = ::GetDpiForWindow(hwnd);
  const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
  if (!::IsZoomed(hwnd) && pt.y < border_y) {
    *result = HTTOP;  // The band's first pixels still resize.
    return true;
  }
  const int band_hit = HitTest(pt);
  *result = band_hit ? band_hit : HTCLIENT;
  return true;
}

bool PhiCaptionBand::OnNcLButtonDown(WPARAM hit) {
  // Swallow presses on our drawn caption buttons; the action fires on
  // release over the same button, like the OS ones.
  if (hit != HTMINBUTTON && hit != HTMAXBUTTON && hit != HTCLOSE) {
    return false;
  }
  pressed_caption_ = static_cast<int>(hit);
  return true;
}

bool PhiCaptionBand::OnNcLButtonUp(HWND hwnd, WPARAM hit) {
  if (pressed_caption_ && static_cast<int>(hit) == pressed_caption_) {
    const int pressed = pressed_caption_;
    pressed_caption_ = 0;
    if (pressed == HTMINBUTTON) {
      ::ShowWindow(hwnd, SW_MINIMIZE);
    } else if (pressed == HTMAXBUTTON) {
      ::ShowWindow(hwnd, ::IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
    } else {
      ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return true;
  }
  pressed_caption_ = 0;
  return false;
}

void PhiCaptionBand::OnNcMouseMove(WPARAM hit, POINT client_pt) {
  SetCaptionHot(hit == HTMINBUTTON || hit == HTMAXBUTTON || hit == HTCLOSE
                    ? static_cast<int>(hit)
                    : 0);
  // Most of the band hit-tests as HTCAPTION, so the pointer crossing it
  // arrives here and not as WM_MOUSEMOVE — this is where the hidden titles
  // come out.
  const RECT band = shell_->CaptionRects().title_bar;
  const bool in_band = ::PtInRect(&band, client_pt) != 0;
  SetRevealed(in_band);
  if (in_band) {
    // A non-client point is drag space, never a title: no title is under the
    // pointer, and nothing the band's controls washed is either.
    SetHotTitle(-1);
    shell_->CaptionClearShellHover();
  }
}

void PhiCaptionBand::OnNcMouseLeave() {
  // NOT "the pointer left the window". Revealing the titles turns the band
  // under the pointer from HTCAPTION into HTCLIENT, so this arrives with the
  // pointer standing perfectly still — and hiding on it put the reveal in a
  // blink loop: reveal, nc-leave, hide, nc-move, reveal. Only where the
  // cursor actually is decides.
  SetCaptionHot(0);
  UpdateRevealFromCursor();
  if (!revealed_) {
    SetHotTitle(-1);
  }
}

void PhiCaptionBand::OnMouseMove(POINT client_pt) {
  SetCaptionHot(0);  // Client moves mean we left the buttons.
  const PhiCaptionRects rects = shell_->CaptionRects();
  const bool in_band = ::PtInRect(&rects.title_bar, client_pt) != 0;
  SetRevealed(in_band);
  int hot = -1;
  if (in_band) {
    const size_t titles = VisibleTitles(rects);
    for (size_t i = 0; i < titles; ++i) {
      if (::PtInRect(&titles_[i].second, client_pt)) {
        hot = static_cast<int>(i);
        break;
      }
    }
  }
  SetHotTitle(hot);
}

void PhiCaptionBand::OnMouseLeave() {
  // The mirror of OnNcMouseLeave: stepping off a menu title onto the band's
  // own drag space leaves the CLIENT area without leaving the band, and the
  // titles must stay put.
  SetCaptionHot(0);
  UpdateRevealFromCursor();
  if (!revealed_) {
    SetHotTitle(-1);
  }
}

void PhiCaptionBand::OnWindowDeactivated() {
  if (!alt_revealed_) {
    return;
  }
  alt_revealed_ = false;
  InvalidateBand();
}

void PhiCaptionBand::ToggleAltReveal() {
  alt_revealed_ = !alt_revealed_;
  InvalidateBand();
}

// --- Clicks ----------------------------------------------------------------

bool PhiCaptionBand::OnPhiButton(const PhiCaptionRects& rects,
                                 POINT client_pt) {
  // Comfortable's leading logo, and the vertical layouts' first title (the
  // logo and "Phi" as one button) — the same corner button either way.
  if (::PtInRect(&rects.phi_menu, client_pt)) {
    return true;
  }
  if (rects.horizontal_tabs || !::PtInRect(&rects.title_bar, client_pt)) {
    return false;
  }
  return VisibleTitles(rects) > 0 &&
         ::PtInRect(&titles_[0].second, client_pt) != 0;
}

bool PhiCaptionBand::OnClick(POINT client_pt) {
  if (!menu_bar_ || !shell_hwnd_) {
    return false;
  }
  const PhiCaptionRects rects = shell_->CaptionRects();
  // The corner button is a menu AND a grip on the window: the press is only
  // HELD here, and what it becomes is decided later — the menu when it comes
  // up in place, a window move when the pointer walks off it first
  // (OnPressMove / OnPressUp). Dropping the menu on the press itself made
  // the window's leading corner the one spot in the band that could not
  // drag the window.
  if (OnPhiButton(rects, client_pt)) {
    phi_pressed_ = true;
    phi_press_pt_ = client_pt;
    ::SetCapture(shell_hwnd_);
    return true;
  }
  return OpenMenuAt(rects, client_pt);
}

bool PhiCaptionBand::OnPressMove(POINT client_pt) {
  if (!phi_pressed_) {
    return false;
  }
  if (::GetKeyState(VK_LBUTTON) >= 0) {
    // The button came up somewhere the shell never saw it: drop the press
    // rather than hand the frame a move loop with nothing held down, which
    // would glue the window to the cursor until the next click.
    CancelPress();
    if (::GetCapture() == shell_hwnd_) {
      ::ReleaseCapture();
    }
    return false;
  }
  if (std::abs(client_pt.x - phi_press_pt_.x) <
          ::GetSystemMetrics(SM_CXDRAG) &&
      std::abs(client_pt.y - phi_press_pt_.y) <
          ::GetSystemMetrics(SM_CYDRAG)) {
    return true;  // Still a click; nothing else may see the travel either.
  }
  phi_pressed_ = false;
  if (::GetCapture() == shell_hwnd_) {
    ::ReleaseCapture();  // The frame's own move loop takes the mouse now.
  }
  POINT screen = client_pt;
  ::ClientToScreen(shell_hwnd_, &screen);
  // Hand the still-held press to the frame exactly as if it had landed on
  // the caption: DefWindowProc drags the window from this point until the
  // button comes up (and unsnaps a maximized frame on the way, as a caption
  // drag does). POSTED, not sent — the move loop must not run nested inside
  // the mouse message that started it.
  ::PostMessageW(shell_hwnd_, WM_NCLBUTTONDOWN, HTCAPTION,
                 MAKELPARAM(screen.x, screen.y));
  return true;
}

bool PhiCaptionBand::OnPressUp() {
  if (!phi_pressed_) {
    return false;
  }
  phi_pressed_ = false;
  if (::GetCapture() == shell_hwnd_) {
    ::ReleaseCapture();  // Before the popup: TrackPopupMenu wants the mouse.
  }
  if (!menu_bar_ || !shell_hwnd_) {
    return true;
  }
  // The press never travelled far enough to be a drag, so it is still on the
  // button: the menu drops from where it LANDED, not from the release point.
  OpenMenuAt(shell_->CaptionRects(), phi_press_pt_);
  return true;
}

void PhiCaptionBand::CancelPress() {
  phi_pressed_ = false;
}

bool PhiCaptionBand::OpenMenuAt(const PhiCaptionRects& rects,
                                POINT client_pt) {
  // Comfortable's Phi button. That band carries no titles, so the leading
  // logo drops the bar's first menu itself — the rest of the bar hangs off
  // the toolbar's overflow.
  if (::PtInRect(&rects.phi_menu, client_pt)) {
    POINT anchor = {rects.phi_menu.left, rects.title_bar.bottom};
    ::ClientToScreen(shell_hwnd_, &anchor);
    menu_open_ = true;
    menu_bar_->ShowAt(0, anchor.x, anchor.y);
    menu_open_ = false;
    return true;
  }
  // A menu title in the caption band drops its menu, aligned under the
  // title. The band's trailing controls are checked by the SHELL after this
  // returns, so the drag-space early-out must not swallow them.
  if (!::PtInRect(&rects.title_bar, client_pt) || rects.horizontal_tabs ||
      ::PtInRect(&rects.back, client_pt) ||
      ::PtInRect(&rects.forward, client_pt) ||
      ::PtInRect(&rects.reload, client_pt) ||
      ::PtInRect(&rects.search_tabs, client_pt)) {
    return false;
  }
  const size_t titles = VisibleTitles(rects);
  for (size_t i = 0; i < titles; ++i) {
    if (!::PtInRect(&titles_[i].second, client_pt)) {
      continue;
    }
    POINT anchor = {titles_[i].second.left, rects.title_bar.bottom};
    ::ClientToScreen(shell_hwnd_, &anchor);
    // The highlight belongs to the menu that is open, not to wherever the
    // pointer was last SEEN: clicking a second title while the first menu is
    // up dismisses that popup and opens this one WITHOUT the shell ever
    // receiving a move over the new title, so the old title kept the wash
    // while the new title's menu hung under it.
    SetHotTitle(static_cast<int>(i));
    // The popup takes the pointer, so the leave messages it triggers must
    // not collapse the bar behind the menu being read.
    menu_open_ = true;
    menu_bar_->ShowAt(i, anchor.x, anchor.y);
    menu_open_ = false;
    // Alt's sticky reveal has done its job by the time the menu closes; the
    // pointer decides from here.
    alt_revealed_ = false;
    UpdateRevealFromCursor();
    UpdateHotTitleFromCursor();
    return true;
  }
  return true;  // The rest of the band is drag space; no chrome lives here.
}

void PhiCaptionBand::ShowOverflow(POINT client_anchor) {
  if (!menu_bar_ || !shell_hwnd_) {
    return;
  }
  POINT anchor = client_anchor;
  ::ClientToScreen(shell_hwnd_, &anchor);
  menu_open_ = true;
  menu_bar_->ShowOverflow(anchor.x, anchor.y);
  menu_open_ = false;
}

void PhiCaptionBand::OnMenuOpening(HMENU menu) {
  if (menu_bar_) {
    menu_bar_->OnMenuOpening(menu);
  }
}

bool PhiCaptionBand::OnCommand(UINT id) {
  return menu_bar_ && menu_bar_->OnCommand(id);
}

// --- Paint -----------------------------------------------------------------

void PhiCaptionBand::Paint(HDC dc, bool with_buttons) {
  const PhiCaptionRects rects = shell_->CaptionRects();
  if (rects.title_bar.right <= rects.title_bar.left) {
    return;
  }
  const PhiPalette palette = shell_->CaptionPalette();
  const COLORREF kLabel = palette.label;
  const int logo_size = shell_->CaptionScaled(kLogoSize);
  HFONT text_font = shell_->CaptionTextFont();
  HGDIOBJ previous_font = text_font ? ::SelectObject(dc, text_font) : nullptr;
  // Comfortable draws no titles — the tab strip owns the band — so the Phi
  // menu shows as the logo alone in the leading slot, with the same hover
  // wash the band's other buttons take.
  if (rects.phi_menu.right > rects.phi_menu.left) {
    if (shell_->CaptionPhiMenuHovered()) {
      FillRounded(dc, rects.phi_menu, palette.hover,
                  shell_->CaptionScaled(4));
    }
    shell_->CaptionDrawLogo(
        dc, (rects.phi_menu.left + rects.phi_menu.right - logo_size) / 2,
        (rects.phi_menu.top + rects.phi_menu.bottom - logo_size) / 2,
        logo_size);
  }
  const size_t titles = VisibleTitles(rects);
  for (size_t i = 0; i < titles; ++i) {
    const RECT& title_rc = titles_[i].second;
    if (static_cast<int>(i) == hot_title_) {
      RECT hot_rc = title_rc;
      ::InflateRect(&hot_rc, 0, -shell_->CaptionScaled(4));
      FillRounded(dc, hot_rc, palette.hover, shell_->CaptionScaled(4));
    }
    ::SetTextColor(dc, kLabel);
    RECT text_rc = title_rc;
    if (i == 0) {
      // The Phi button: icon leading, label after — one hit target.
      shell_->CaptionDrawLogo(
          dc, title_rc.left + shell_->CaptionScaled(kPhiTitlePad),
          (title_rc.bottom + title_rc.top - logo_size) / 2, logo_size);
      text_rc.left += shell_->CaptionScaled(kPhiTitlePad) + logo_size +
                      shell_->CaptionScaled(kLogoTextGap);
      PhiDrawTextAA(dc, titles_[i].first.c_str(), -1, &text_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      continue;
    }
    PhiDrawTextAA(dc, titles_[i].first.c_str(), -1, &text_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  // The seam between the app's band controls and the window's. It draws in
  // the TERTIARY ink (30%), not the 6% separator the band's own bottom edge
  // uses: a hairline that short, standing alone in a gap rather than running
  // the window's width, disappears at 6%.
  if (rects.band_divider.right > rects.band_divider.left) {
    HBRUSH divider_brush = ::CreateSolidBrush(palette.label_tertiary);
    ::FillRect(dc, &rects.band_divider, divider_brush);
    ::DeleteObject(divider_brush);
  }
  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
  if (with_buttons) {
    PaintButtons(dc);
  }
}

void PhiCaptionBand::PaintButtons(HDC dc) {
  const PhiCaptionRects rects = shell_->CaptionRects();
  if (rects.title_bar.right <= rects.title_bar.left) {
    return;
  }
  const PhiPalette palette = shell_->CaptionPalette();
  const COLORREF kLabel = palette.label;
  // Hot fills first (close turns the Win11 red), then the MDL2 glyphs.
  if (hot_caption_ == HTMINBUTTON || hot_caption_ == HTMAXBUTTON) {
    const RECT& hot_rc =
        hot_caption_ == HTMINBUTTON ? rects.caption_min : rects.caption_max;
    HBRUSH hover_brush = ::CreateSolidBrush(palette.hover);
    ::FillRect(dc, &hot_rc, hover_brush);
    ::DeleteObject(hover_brush);
  } else if (hot_caption_ == HTCLOSE) {
    HBRUSH close_brush = ::CreateSolidBrush(RGB(0xC4, 0x2B, 0x1C));
    ::FillRect(dc, &rects.caption_close, close_brush);
    ::DeleteObject(close_brush);
  }
  HFONT glyph_font = shell_->CaptionGlyphFont();
  HGDIOBJ previous_caption =
      glyph_font ? ::SelectObject(dc, glyph_font) : nullptr;
  const bool zoomed = shell_hwnd_ && ::IsZoomed(shell_hwnd_);
  // Comfortable's band carries the tab strip, and everything on it sits on
  // the STRIP's line rather than the band's — the caption glyphs follow, so
  // the trio reads level with search-tabs next to it. Only the glyphs move:
  // the buttons keep the full band height, which is the Win11 hit target
  // (and the hover wash) whatever the glyph inside is doing.
  const int glyph_drop =
      rects.mode == PhiLayoutMode::kComfortable
          ? (rects.tabs_top + rects.title_bar.bottom) / 2 -
                (rects.title_bar.top + rects.title_bar.bottom) / 2
          : 0;
  const auto glyph_rc = [&](const RECT& rc) {
    return RECT{rc.left, rc.top + glyph_drop, rc.right,
                rc.bottom + glyph_drop};
  };
  DrawGlyph(dc, glyph_rc(rects.caption_min), L"\uE921", kLabel);
  DrawGlyph(dc, glyph_rc(rects.caption_max), zoomed ? L"\uE923" : L"\uE922",
            kLabel);
  DrawGlyph(dc, glyph_rc(rects.caption_close), L"\uE8BB",
            hot_caption_ == HTCLOSE ? RGB(0xFF, 0xFF, 0xFF) : kLabel);
  if (previous_caption) {
    ::SelectObject(dc, previous_caption);
  }
}

}  // namespace phi_client
