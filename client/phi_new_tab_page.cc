// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_new_tab_page.h"

#include <commctrl.h>  // EM_SETCUEBANNER
#include <windowsx.h>

#include <algorithm>
#include <vector>

#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_mac_icons.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiNewTabPage";
constexpr UINT_PTR kFieldId = 1;

// NewTabViewController's metrics (DIP).
constexpr int kIconSize = 64;          // iconSize
constexpr int kIconLabelGap = 6;       // label 6pt below the icon
constexpr int kLabelBoxGap = 48;       // omnibox 48pt below the label
constexpr int kLabelFontSize = 21;     // IvyPrestoHeadline-Light 21
constexpr int kBoxWidth = 680;         // OmniBoxViewController.boxWidth
constexpr int kBoxHeight = 57;         // collapsedOmniBoxHeight
constexpr int kBoxMinWidth = 240;      // updateContentLayout's floor
constexpr int kBoxHorizontalInset = 24;
constexpr int kBoxRadius = 14;
// Inside the box: the magnifier leads, the text follows.
constexpr int kGlyphLead = 18;
constexpr int kGlyphSize = 16;
constexpr int kGlyphTextGap = 12;
constexpr int kFieldTrail = 18;
constexpr int kFieldFontSize = 16;
constexpr int kFieldHeight = 22;

// Segoe MDL2 Assets "Search".
constexpr wchar_t kSearchGlyph[] = L"\xE721";

COLORREF BlendInk(COLORREF ink, COLORREF over, int alpha255) {
  const auto mix = [alpha255](int a, int b) {
    return (a * alpha255 + b * (255 - alpha255)) / 255;
  };
  return RGB(mix(GetRValue(ink), GetRValue(over)),
             mix(GetGValue(ink), GetGValue(over)),
             mix(GetBValue(ink), GetBValue(over)));
}

}  // namespace

PhiNewTabPage::PhiNewTabPage(PhiWindowController* controller)
    : controller_(controller) {}

PhiNewTabPage::~PhiNewTabPage() {
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::DestroyWindow(hwnd_);  // Takes the field with it.
  }
  for (HFONT font : {label_font_, field_font_, glyph_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  if (field_brush_) {
    ::DeleteObject(field_brush_);
  }
}

// static
bool PhiNewTabPage::IsNewTabUrl(const std::string& url) {
  return url.rfind("chrome://newtab", 0) == 0 ||
         url.rfind("chrome://new-tab-page", 0) == 0 ||
         url.rfind("phi://newtab", 0) == 0;
}

int PhiNewTabPage::Scaled(int design) const {
  return ::MulDiv(design, static_cast<int>(dpi_), 96);
}

bool PhiNewTabPage::visible() const {
  return hwnd_ && ::IsWindow(hwnd_) && ::IsWindowVisible(hwnd_);
}

bool PhiNewTabPage::WantsFocus() const {
  return visible() && !controls_hidden_ && field_ && ::IsWindow(field_);
}

void PhiNewTabPage::EnsureWindow() {
  if (hwnd_ && ::IsWindow(hwnd_)) {
    return;
  }
  HWND owner = controller_ ? controller_->shell_hwnd() : nullptr;
  if (!owner) {
    return;
  }
  static const bool registered = [] {
    WNDCLASSW cls = {};
    cls.lpfnWndProc = &PhiNewTabPage::PageProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kClassName;
    return ::RegisterClassW(&cls) != 0;
  }();
  (void)registered;
  dpi_ = ::GetDpiForWindow(owner);
  if (!dpi_) {
    dpi_ = 96;
  }
  hwnd_ = ::CreateWindowExW(
      0, kClassName, L"",
      // WS_CLIPSIBLINGS: this covers the content card, and without it the
      // page paints straight over the corner caps stacked above it —
      // squaring off the card's rounded corners.
      WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0,
                            0, 0, 0, owner, nullptr,
                            ::GetModuleHandleW(nullptr), nullptr);
  if (!hwnd_) {
    return;
  }
  ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  EnsureFonts();
  EnsureField();
}

void PhiNewTabPage::EnsureFonts() {
  if (label_font_ && fonts_dpi_ == dpi_) {
    return;
  }
  for (HFONT font : {label_font_, field_font_, glyph_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  const auto make = [&](int size, int weight, const wchar_t* face) {
    return ::CreateFontW(-Scaled(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, face);
  };
  // The Mac's brand display face (IvyPresto Headline Light) is not shipped
  // here; Segoe's light cut is the system's equivalent weight.
  label_font_ = make(kLabelFontSize, FW_LIGHT, L"Segoe UI Light");
  field_font_ = make(kFieldFontSize, FW_NORMAL, L"Segoe UI");
  glyph_font_ = make(kGlyphSize, FW_NORMAL, L"Segoe MDL2 Assets");
  fonts_dpi_ = dpi_;
  if (field_ && field_font_) {
    ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(field_font_),
                   TRUE);
  }
}

void PhiNewTabPage::EnsureField() {
  if (field_ || !hwnd_ || !controller_) {
    return;
  }
  field_ = ::CreateWindowExW(
      0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0,
      10, 10, hwnd_, reinterpret_cast<HMENU>(kFieldId),
      ::GetModuleHandleW(nullptr), nullptr);
  if (!field_) {
    return;
  }
  if (field_font_) {
    ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(field_font_),
                   TRUE);
  }
  // The Mac omnibox's placeholder, drawn by the EDIT itself while empty.
  ::SendMessageW(field_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(
                     PhiStr("addressBar.configuration.defaultPlaceholder")));
  // One omnibox session for the whole window: the controller's subclass
  // gives this field the address bar's Enter / Escape / arrow handling and
  // its suggestion dropdown.
  controller_->AttachOmniboxField(field_);
}

void PhiNewTabPage::Show() {
  EnsureWindow();
  if (!hwnd_) {
    return;
  }
  controls_hidden_ = false;
  if (field_) {
    ::SetWindowTextW(field_, L"");
  }
  Reposition();
  ::ShowWindow(hwnd_, SW_SHOWNA);
  ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void PhiNewTabPage::Hide() {
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::ShowWindow(hwnd_, SW_HIDE);
  }
}

void PhiNewTabPage::Reposition() {
  if (!hwnd_ || !controller_) {
    return;
  }
  const UINT dpi = ::GetDpiForWindow(controller_->shell_hwnd());
  if (dpi && dpi != dpi_) {
    dpi_ = dpi;
    EnsureFonts();
  }
  const RECT content = controller_->ContentRect();
  ::SetWindowPos(hwnd_, HWND_TOP, content.left, content.top,
                 content.right - content.left, content.bottom - content.top,
                 SWP_NOACTIVATE);
  LayoutField();
  ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhiNewTabPage::Refresh() {
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void PhiNewTabPage::HideControls() {
  if (controls_hidden_) {
    return;
  }
  controls_hidden_ = true;
  if (field_) {
    ::ShowWindow(field_, SW_HIDE);
  }
  Refresh();
}

void PhiNewTabPage::Reset() {
  controls_hidden_ = false;
  if (field_) {
    ::SetWindowTextW(field_, L"");
    ::ShowWindow(field_, SW_SHOW);
  }
  LayoutField();
  Refresh();
}

void PhiNewTabPage::FocusField() {
  if (WantsFocus()) {
    ::SetFocus(field_);
  }
}

RECT PhiNewTabPage::BoxScreenRect() const {
  RECT box = ComputeGeometry().box;
  if (hwnd_) {
    ::MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&box), 2);
  }
  return box;
}

PhiNewTabPage::Geometry PhiNewTabPage::ComputeGeometry() const {
  Geometry g;
  if (!hwnd_ || !controller_) {
    return g;
  }
  RECT rc = {};
  ::GetClientRect(hwnd_, &rc);
  const int clip_w = rc.right - rc.left;
  const int clip_h = rc.bottom - rc.top;
  const bool incognito = controller_->is_incognito();

  // updateContentLayout: the box fits the visible clip — its natural 680
  // overflows a narrow card (a split pane) — and the column stays centred.
  const int icon = Scaled(kIconSize);
  int label_w = 0, label_h = 0;
  if (incognito && label_font_) {
    HDC dc = ::GetDC(hwnd_);
    if (dc) {
      HGDIOBJ prev = ::SelectObject(dc, label_font_);
      RECT measure = {0, 0, 0, 0};
      PhiDrawTextAA(dc, PhiStr("browser.newTabPage.incognitoLabel"), -1,
                  &measure, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      ::SelectObject(dc, prev);
      ::ReleaseDC(hwnd_, dc);
      label_w = measure.right - measure.left + 2;  // The Mac's clip guard.
      label_h = measure.bottom - measure.top;
    }
  }
  const int box_w = std::min(
      std::min(Scaled(kBoxWidth),
               std::max(clip_w - Scaled(kBoxHorizontalInset) * 2,
                        Scaled(kBoxMinWidth))),
      std::max(clip_w - Scaled(8), 1));
  const int box_h = Scaled(kBoxHeight);
  const int column_h = icon +
                       (incognito ? Scaled(kIconLabelGap) + label_h : 0) +
                       Scaled(kLabelBoxGap) + box_h;
  const int cx = clip_w / 2;
  int y = std::max(0, (clip_h - column_h) / 2);
  g.icon = {cx - icon / 2, y, cx - icon / 2 + icon, y + icon};
  y += icon;
  if (incognito) {
    y += Scaled(kIconLabelGap);
    g.label = {cx - label_w / 2, y, cx - label_w / 2 + label_w, y + label_h};
    y += label_h;
  }
  y += Scaled(kLabelBoxGap);
  g.box = {cx - box_w / 2, y, cx - box_w / 2 + box_w, y + box_h};
  return g;
}

void PhiNewTabPage::LayoutField() {
  if (!field_) {
    return;
  }
  const RECT box = ComputeGeometry().box;
  const int text_h = Scaled(kFieldHeight);
  const int left =
      box.left + Scaled(kGlyphLead) + Scaled(kGlyphSize) + Scaled(kGlyphTextGap);
  const int right = box.right - Scaled(kFieldTrail);
  ::SetWindowPos(field_, nullptr, left,
                 box.top + (box.bottom - box.top - text_h) / 2,
                 std::max(1, right - left), text_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  ::ShowWindow(field_, controls_hidden_ ? SW_HIDE : SW_SHOW);
}

void PhiNewTabPage::Paint(HDC dc) {
  if (!controller_) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd_, &rc);
  const PhiPalette palette = controller_->CurrentPalette();
  // contentOverlayBackground: the card's own ground, white in light mode.
  HBRUSH ground = ::CreateSolidBrush(palette.card);
  ::FillRect(dc, &rc, ground);
  ::DeleteObject(ground);
  if (controls_hidden_) {
    return;  // omniBoxDidClear: a blank card until the page unmounts.
  }
  const Geometry g = ComputeGeometry();
  ::SetBkMode(dc, TRANSPARENT);

  // The mark: the imageset's light variant is pure black, its dark one
  // #F8F8F8 — tinted from the shared mask rather than shipped twice.
  const PhiMacIcon& art = controller_->is_incognito()
                              ? PhiNativeNtpIncognito()
                              : PhiNativeNtpLogo();
  const COLORREF ink =
      palette.dark ? RGB(0xF8, 0xF8, 0xF8) : RGB(0x00, 0x00, 0x00);
  const int icon_px = g.icon.right - g.icon.left;
  if (icon_px > 0) {
    const std::vector<unsigned char> bgra =
        PhiTintedIconBgra(art, ink, icon_px);
    PhiBlendBgra(dc, g.icon, bgra.data(), icon_px, icon_px);
  }

  if (controller_->is_incognito() && label_font_) {
    HGDIOBJ prev = ::SelectObject(dc, label_font_);
    ::SetTextColor(dc, palette.label);
    RECT label = g.label;
    PhiDrawTextAA(dc, PhiStr("browser.newTabPage.incognitoLabel"), -1, &label,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, prev);
  }

  // The omnibox box: the Mac's contentOverlayBackground fill under a 1pt
  // black/white 20% border, continuous corners of radius 14.
  const COLORREF border = BlendInk(
      palette.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00),
      palette.card, 51);
  PhiFillRoundRectAA(dc, g.box, Scaled(kBoxRadius), palette.card);
  PhiStrokeRoundRectAA(dc, g.box, Scaled(kBoxRadius), border, 1);
  if (glyph_font_) {
    RECT glyph = {g.box.left + Scaled(kGlyphLead), g.box.top,
                  g.box.left + Scaled(kGlyphLead) + Scaled(kGlyphSize),
                  g.box.bottom};
    HGDIOBJ prev = ::SelectObject(dc, glyph_font_);
    ::SetTextColor(dc, palette.label_dim);  // secondaryLabelColor
    PhiDrawTextAA(dc, kSearchGlyph, -1, &glyph,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, prev);
  }
}

// static
LRESULT CALLBACK PhiNewTabPage::PageProc(HWND hwnd,
                                         UINT msg,
                                         WPARAM w,
                                         LPARAM l) {
  auto* self = reinterpret_cast<PhiNewTabPage*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        // Back-buffered: the whole card repaints on every palette ramp tick.
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        const int page_w = rc.right - rc.left;
        const int page_h = rc.bottom - rc.top;
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP buffer =
            mem ? ::CreateCompatibleBitmap(dc, page_w, page_h) : nullptr;
        if (mem && buffer) {
          HGDIOBJ prev = ::SelectObject(mem, buffer);
          self->Paint(mem);
          ::BitBlt(dc, 0, 0, page_w, page_h, mem, 0, 0, SRCCOPY);
          ::SelectObject(mem, prev);
        } else {
          self->Paint(dc);
        }
        if (buffer) {
          ::DeleteObject(buffer);
        }
        if (mem) {
          ::DeleteDC(mem);
        }
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_PRINTCLIENT:
      // PrintWindow captures (the box's screenshot rig) render children
      // through this; without it the page would capture blank.
      if (self) {
        self->Paint(reinterpret_cast<HDC>(w));
      }
      return 0;
    case WM_SIZE:
      if (self) {
        self->LayoutField();
      }
      return 0;
    case WM_LBUTTONDOWN:
      if (self) {
        // The box's padding focuses the field like the Mac's box does; the
        // ground takes focus itself, which blurs the field and drops its
        // suggestion list.
        const POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const RECT box = self->ComputeGeometry().box;
        if (self->WantsFocus() && ::PtInRect(&box, pt)) {
          ::SetFocus(self->field_);
          ::SendMessageW(self->field_, EM_SETSEL, 0, -1);
        } else {
          ::SetFocus(hwnd);
        }
      }
      return 0;
    case WM_COMMAND:
      if (self && self->controller_ && self->field_ &&
          reinterpret_cast<HWND>(l) == self->field_ &&
          HIWORD(w) == EN_CHANGE) {
        self->controller_->OnOmniboxFieldChanged(self->field_);
        return 0;
      }
      break;
    case WM_CTLCOLOREDIT:
      if (self && self->controller_ &&
          reinterpret_cast<HWND>(l) == self->field_) {
        const PhiPalette palette = self->controller_->CurrentPalette();
        HDC edit_dc = reinterpret_cast<HDC>(w);
        ::SetTextColor(edit_dc, palette.label);
        ::SetBkColor(edit_dc, palette.card);  // Seamless with the box fill.
        if (!self->field_brush_ ||
            self->field_brush_color_ != palette.card) {
          if (self->field_brush_) {
            ::DeleteObject(self->field_brush_);
          }
          self->field_brush_ = ::CreateSolidBrush(palette.card);
          self->field_brush_color_ = palette.card;
        }
        return reinterpret_cast<LRESULT>(self->field_brush_);
      }
      break;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

}  // namespace phi_client
