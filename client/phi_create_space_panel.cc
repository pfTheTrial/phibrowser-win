// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_create_space_panel.h"

#include <commctrl.h>  // EM_SETCUEBANNER
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

#include "base/no_destructor.h"
#include "base/rand_util.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiCreateSpacePanel";
constexpr UINT_PTR kFieldId = 1;

// CreateSpacePanel's sidebar metrics (DIP).
constexpr int kContentWidth = 320;  // sidebarContentWidth
constexpr int kColumnPad = 10;      // .padding(.horizontal, 10)
constexpr int kColumnVPad = 20;     // .padding(.vertical, 20)
constexpr int kSectionGap = 28;     // formStack VStack(spacing: 28)
constexpr int kHeaderGap = 12;      // header VStack(spacing: 12)
constexpr int kTitleSize = 18;      // .medium
constexpr int kSubtitleSize = 14;
constexpr int kBodySize = 13;
constexpr int kPillSize = 11;
constexpr int kFormGap = 4;         // form VStack(spacing: 4)
constexpr int kRowHeight = 32;
constexpr int kRowRadius = 8;
constexpr int kRowLead = 8;
constexpr int kRowTrail = 6;
constexpr int kPillHeight = 20;
constexpr int kPillLead = 12;
constexpr int kPillTrail = 8;
constexpr int kPillChevronGap = 4;
constexpr int kPillChevron = 9;
// IconPickerMetrics — the Mac picker at its preferredHeight. The Mac's
// search row is not built here (nor in the Settings pane's picker).
constexpr int kPickerHeight = 280;
constexpr int kPickerVPad = 2;      // iconBlock .padding(.vertical, 2)
constexpr int kPickerTop = 8;       // topPadding
constexpr int kSegmentHeight = 26;
constexpr int kSegmentWidth = 128;
constexpr int kSegmentToGrid = 8;
constexpr int kGridHPad = 8;
constexpr int kGridVPad = 8;
constexpr int kItem = 26;           // itemSize
constexpr int kItemIcon = 20;       // iconSize
constexpr int kEmojiFont = 14;      // emojiFontSize
constexpr int kItemRadius = 10;     // itemCornerRadius
constexpr int kRowSpacing = 4;
constexpr int kColumnSpacing = 2;   // reflowColumnSpacing
// colorBlock: 16pt dots in 20pt rings, a divider, the 20pt slider.
constexpr int kDot = 16;
constexpr int kRing = 20;
constexpr int kColorHPad = 8;
constexpr int kColorVPad = 12;
constexpr int kColorGap = 12;
constexpr int kSliderHeight = 20;
constexpr int kSliderKnob = 18;
// actions: the Create capsule (13 medium, 16/6 padding) over the Cancel text.
constexpr int kActionGap = 12;
constexpr int kButtonHPad = 16;
constexpr int kButtonVPad = 6;
constexpr int kButtonTextHeight = 16;
// The create-form swipe shares the Space-switch push-in's clock.
constexpr int kSlideMs = 150;
constexpr int kWheelStep = 3;  // Rows per notch.

COLORREF Blend(COLORREF over, COLORREF base, int alpha) {
  const auto mix = [alpha](int a, int b) {
    return (a * alpha + b * (255 - alpha)) / 255;
  };
  return RGB(mix(GetRValue(over), GetRValue(base)),
             mix(GetGValue(over), GetGValue(base)),
             mix(GetBValue(over), GetBValue(base)));
}

// Anti-aliased where Direct2D is available, GDI's RoundRect otherwise.
// Antialiased, true-radius shapes (phi_dwrite.h); they never need a GDI
// fallback of their own.
void FillRound(HDC dc, const RECT& r, int radius, COLORREF fill) {
  PhiFillRoundRectAA(dc, r, radius, fill);
}

void FillCircle(HDC dc, const RECT& r, COLORREF fill) {
  PhiFillEllipseAA(dc, r, fill);
}

void StrokeCircle(HDC dc, const RECT& r, COLORREF ink, int width) {
  PhiStrokeEllipseAA(dc, r, ink, width);
}

void RgbToHsv(COLORREF c, double* h, double* s, double* v) {
  const double r = GetRValue(c) / 255.0;
  const double g = GetGValue(c) / 255.0;
  const double b = GetBValue(c) / 255.0;
  const double mx = std::max(r, std::max(g, b));
  const double mn = std::min(r, std::min(g, b));
  const double d = mx - mn;
  *v = mx;
  *s = mx > 0 ? d / mx : 0;
  if (d <= 0) {
    *h = 0;
  } else if (mx == r) {
    *h = 60.0 * std::fmod((g - b) / d, 6.0);
  } else if (mx == g) {
    *h = 60.0 * ((b - r) / d + 2.0);
  } else {
    *h = 60.0 * ((r - g) / d + 4.0);
  }
  if (*h < 0) {
    *h += 360.0;
  }
}

COLORREF HsvToRgb(double h, double s, double v) {
  const double c = v * s;
  const double x = c * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1));
  const double m = v - c;
  double r = 0, g = 0, b = 0;
  if (h < 60) {
    r = c, g = x;
  } else if (h < 120) {
    r = x, g = c;
  } else if (h < 180) {
    g = c, b = x;
  } else if (h < 240) {
    g = x, b = c;
  } else if (h < 300) {
    r = x, b = c;
  } else {
    r = c, b = x;
  }
  const auto to = [m](double q) {
    return static_cast<int>(std::lround((q + m) * 255.0));
  };
  return RGB(to(r), to(g), to(b));
}

std::string HexOf(COLORREF c) {
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", GetRValue(c), GetGValue(c),
                GetBValue(c));
  return buf;
}

int MeasureTextWidth(HDC dc, HFONT font, const std::wstring& text) {
  HGDIOBJ prev = font ? ::SelectObject(dc, font) : nullptr;
  RECT rc = {0, 0, 0, 0};
  PhiDrawTextAA(dc, text.c_str(), -1, &rc,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
  if (prev) {
    ::SelectObject(dc, prev);
  }
  return rc.right - rc.left;
}

}  // namespace

struct PhiCreateSpacePanel::Geometry {
  RECT column{};
  RECT title{};
  RECT subtitle{};
  RECT name_row{};
  RECT profile_row{};
  RECT pill{};
  RECT icon_block{};
  RECT segment{};
  RECT seg_icon{};
  RECT seg_emoji{};
  RECT grid{};
  RECT color_block{};
  RECT divider{};
  RECT slider{};
  std::array<RECT, 8> dots{};
  size_t dot_count = 0;
  RECT create_button{};
  RECT cancel_button{};
  int content_height = 0;       // The column's full height (form scroll).
  int columns = 1;              // Grid columns that fit this width.
  int column_stride = 0;        // Cell-to-cell x step, spread across the grid.
  int grid_content_height = 0;  // Every cell row, for the grid's scroll.
};

PhiCreateSpacePanel::PhiCreateSpacePanel(PhiWindowController* controller)
    : controller_(controller) {}

PhiCreateSpacePanel::~PhiCreateSpacePanel() {
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::DestroyWindow(hwnd_);  // Takes the field with it.
  }
  for (HFONT font : {title_font_, subtitle_font_, body_font_,
                     body_medium_font_, pill_font_, emoji_font_, mdl2_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  if (field_brush_) {
    ::DeleteObject(field_brush_);
  }
}

int PhiCreateSpacePanel::Scaled(int design) const {
  return ::MulDiv(design, static_cast<int>(dpi_), 96);
}

bool PhiCreateSpacePanel::visible() const {
  return hwnd_ && ::IsWindow(hwnd_) && ::IsWindowVisible(hwnd_);
}

void PhiCreateSpacePanel::EnsureWindow() {
  if (hwnd_ && ::IsWindow(hwnd_)) {
    return;
  }
  HWND owner = controller_ ? controller_->shell_hwnd() : nullptr;
  if (!owner) {
    return;
  }
  static const bool registered = [] {
    WNDCLASSW cls = {};
    cls.lpfnWndProc = &PhiCreateSpacePanel::PanelProc;
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
  hwnd_ = ::CreateWindowExW(0, kClassName, L"", WS_CHILD | WS_CLIPCHILDREN, 0,
                            0, 0, 0, owner, nullptr,
                            ::GetModuleHandleW(nullptr), nullptr);
  if (!hwnd_) {
    return;
  }
  ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  EnsureFonts();
  EnsureField();
}

void PhiCreateSpacePanel::EnsureFonts() {
  if (title_font_ && fonts_dpi_ == dpi_) {
    return;
  }
  for (HFONT font : {title_font_, subtitle_font_, body_font_,
                     body_medium_font_, pill_font_, emoji_font_, mdl2_font_}) {
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
  title_font_ = make(kTitleSize, FW_MEDIUM, L"Segoe UI");
  subtitle_font_ = make(kSubtitleSize, FW_NORMAL, L"Segoe UI");
  body_font_ = make(kBodySize, FW_NORMAL, L"Segoe UI");
  body_medium_font_ = make(kBodySize, FW_MEDIUM, L"Segoe UI");
  pill_font_ = make(kPillSize, FW_NORMAL, L"Segoe UI");
  emoji_font_ = make(kEmojiFont, FW_NORMAL, L"Segoe UI Emoji");
  mdl2_font_ = make(kEmojiFont, FW_NORMAL, L"Segoe MDL2 Assets");
  fonts_dpi_ = dpi_;
  if (field_ && body_font_) {
    ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_),
                   TRUE);
  }
}

void PhiCreateSpacePanel::EnsureField() {
  if (field_ || !hwnd_) {
    return;
  }
  field_ = ::CreateWindowExW(
      0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0,
      10, 10, hwnd_, reinterpret_cast<HMENU>(kFieldId),
      ::GetModuleHandleW(nullptr), nullptr);
  if (!field_) {
    return;
  }
  if (body_font_) {
    ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(body_font_),
                   TRUE);
  }
  ::SendMessageW(field_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(
                     PhiStr("sidebar.createSpace.nameField.placeholder")));
  // Enter creates (the field's onSubmit), Escape cancels (.cancelAction).
  field_base_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
      field_, GWLP_WNDPROC,
      reinterpret_cast<LONG_PTR>(&PhiCreateSpacePanel::FieldProc)));
}

void PhiCreateSpacePanel::ReloadProfiles() {
  profiles_.clear();
  if (!controller_ || !controller_->host()) {
    return;
  }
  for (const std::string& row : controller_->host()->ProfileSummaries()) {
    const size_t tab = row.find('\t');
    const std::string id = row.substr(0, tab);
    const std::string name =
        tab == std::string::npos ? id : row.substr(tab + 1);
    profiles_.emplace_back(id, base::UTF8ToWide(name));
  }
}

std::wstring PhiCreateSpacePanel::SelectedProfileName() const {
  for (const auto& [id, name] : profiles_) {
    if (id == profile_id_) {
      return name;
    }
  }
  return base::UTF8ToWide(profile_id_);
}

const std::vector<std::pair<std::wstring, std::string>>&
PhiCreateSpacePanel::Cells() const {
  // One flat list per tab: the Phi catalog, or every emoji group in the
  // Mac's order (the Settings pane's picker shows the same lists).
  using CellList = std::vector<std::pair<std::wstring, std::string>>;
  static const base::NoDestructor<CellList> icons([] {
    CellList out;
    for (const PhiCatalogIcon& icon : PhiIconCatalogAll()) {
      out.emplace_back(std::wstring(1, icon.glyph),
                       std::string("phi:") + icon.id);
    }
    return out;
  }());
  static const base::NoDestructor<CellList> emoji([] {
    CellList out;
    for (const PhiEmojiGroup& group : PhiEmojiGroups()) {
      for (const PhiEmoji& item : group.items) {
        out.emplace_back(base::UTF8ToWide(item.text), "emoji:" + item.id);
      }
    }
    return out;
  }());
  return tab_ == Tab::kIcon ? *icons : *emoji;
}

void PhiCreateSpacePanel::Show(const RECT& bounds,
                               const std::string& initial_profile_id,
                               const std::string& source_theme_id,
                               int saturation) {
  EnsureWindow();
  if (!hwnd_) {
    return;
  }
  // CreateSpacePanel.onAppear: the profile the strip's "+" was pressed
  // from; a random Phi icon; a random theme distinct from the source
  // Space's so the new Space arrives looking different.
  ReloadProfiles();
  profile_id_.clear();
  for (const auto& [id, name] : profiles_) {
    if (id == initial_profile_id) {
      profile_id_ = id;
      break;
    }
  }
  if (profile_id_.empty()) {
    profile_id_ = profiles_.empty() ? std::string("Default")
                                    : profiles_.front().first;
  }
  const auto& catalog = PhiIconCatalogAll();
  icon_ = catalog.empty()
              ? std::string(kPhiDefaultSpaceIcon)
              : "phi:" + std::string(
                             catalog[base::RandGenerator(catalog.size())].id);
  const auto& themes = PhiBuiltInThemes();
  std::vector<size_t> fresh;
  for (size_t i = 0; i < themes.size(); ++i) {
    if (themes[i].id != source_theme_id) {
      fresh.push_back(i);
    }
  }
  theme_id_ = fresh.empty() ? source_theme_id
                            : themes[fresh[base::RandGenerator(fresh.size())]].id;
  slider_ = std::max(0, std::min(100, saturation));
  user_adjusted_slider_ = false;
  tab_ = Tab::kIcon;
  grid_scroll_ = 0;
  form_scroll_ = 0;
  hover_ = Hit::kNone;
  hover_index_ = -1;
  dragging_slider_ = false;
  if (field_) {
    ::SetWindowTextW(field_, L"");
  }
  Reposition(bounds);
  // Preview at once, so the arriving surface already carries the new
  // Space's colour and the strip shows its icon.
  if (controller_) {
    controller_->SetCreateSpaceIcon(icon_);
  }
  PushPreview();
  ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  // The Space-switch push-in: the form arrives from the right.
  if (!::AnimateWindow(hwnd_, kSlideMs, AW_SLIDE | AW_HOR_NEGATIVE)) {
    ::ShowWindow(hwnd_, SW_SHOWNA);
  }
  ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhiCreateSpacePanel::Hide(bool animated) {
  if (!hwnd_ || !::IsWindow(hwnd_)) {
    return;
  }
  if (dragging_slider_) {
    dragging_slider_ = false;
    ::ReleaseCapture();
  }
  if (::IsWindowVisible(hwnd_)) {
    if (!animated ||
        !::AnimateWindow(hwnd_, kSlideMs,
                         AW_HIDE | AW_SLIDE | AW_HOR_POSITIVE)) {
      ::ShowWindow(hwnd_, SW_HIDE);
    }
  }
}

void PhiCreateSpacePanel::Reposition(const RECT& bounds) {
  if (!hwnd_ || !controller_) {
    return;
  }
  const UINT dpi = ::GetDpiForWindow(controller_->shell_hwnd());
  if (dpi && dpi != dpi_) {
    dpi_ = dpi;
    EnsureFonts();
  }
  ::SetWindowPos(hwnd_, HWND_TOP, bounds.left, bounds.top,
                 std::max<LONG>(1, bounds.right - bounds.left),
                 std::max<LONG>(1, bounds.bottom - bounds.top),
                 SWP_NOACTIVATE);
  LayoutField();
  ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhiCreateSpacePanel::Refresh() {
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void PhiCreateSpacePanel::FocusField() {
  if (visible() && field_ && ::IsWindow(field_)) {
    ::SetFocus(field_);
  }
}

void PhiCreateSpacePanel::OnProfileCreated(const std::string& profile_id) {
  ReloadProfiles();
  for (const auto& [id, name] : profiles_) {
    if (id == profile_id) {
      profile_id_ = id;
      break;
    }
  }
  Refresh();
}

PhiCreateSpacePanel::Geometry PhiCreateSpacePanel::ComputeGeometry() const {
  Geometry g;
  if (!hwnd_) {
    return g;
  }
  RECT rc = {};
  ::GetClientRect(hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  // One bounded, centred column: the sidebar's width minus the slim
  // padding, up to 320.
  const int column_w =
      std::max(1, std::min(Scaled(kContentWidth), w - 2 * Scaled(kColumnPad)));
  const int left = (w - column_w) / 2;
  const int right = left + column_w;

  HDC dc = ::GetDC(hwnd_);
  int title_h = Scaled(24), subtitle_h = Scaled(18);
  int pill_w = Scaled(80), create_w = Scaled(110), cancel_w = Scaled(48);
  if (dc) {
    HGDIOBJ prev = ::SelectObject(dc, title_font_);
    RECT m = {0, 0, column_w, 0};
    PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.title"), -1, &m,
                DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    title_h = m.bottom - m.top;
    ::SelectObject(dc, subtitle_font_);
    m = {0, 0, column_w, 0};
    PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.subtitle"), -1, &m,
                DT_CALCRECT | DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
    subtitle_h = m.bottom - m.top;
    ::SelectObject(dc, prev);
    pill_w = Scaled(kPillLead) +
             MeasureTextWidth(dc, pill_font_, SelectedProfileName()) +
             Scaled(kPillChevronGap) + Scaled(kPillChevron) +
             Scaled(kPillTrail);
    create_w = MeasureTextWidth(dc, body_medium_font_,
                                PhiStr("sidebar.createSpace.createButton")) +
               2 * Scaled(kButtonHPad);
    cancel_w = MeasureTextWidth(dc, body_medium_font_,
                                PhiStr("sidebar.createSpace.cancelButton")) +
               Scaled(8);
    ::ReleaseDC(hwnd_, dc);
  }

  int y = Scaled(kColumnVPad);
  g.title = {left, y, right, y + title_h};
  y += title_h + Scaled(kHeaderGap);
  g.subtitle = {left, y, right, y + subtitle_h};
  y += subtitle_h + Scaled(kSectionGap);

  g.name_row = {left, y, right, y + Scaled(kRowHeight)};
  y += Scaled(kRowHeight) + Scaled(kFormGap);
  g.profile_row = {left, y, right, y + Scaled(kRowHeight)};
  {
    const int pill_h = Scaled(kPillHeight);
    const int pill_top = y + (Scaled(kRowHeight) - pill_h) / 2;
    const int pill_right = right - Scaled(kRowTrail);
    g.pill = {std::max(left + Scaled(kRowLead), pill_right - pill_w), pill_top,
              pill_right, pill_top + pill_h};
  }
  y += Scaled(kRowHeight) + Scaled(kFormGap);

  const int block_h = Scaled(kPickerHeight) + 2 * Scaled(kPickerVPad);
  g.icon_block = {left, y, right, y + block_h};
  {
    const int seg_w = std::min(Scaled(kSegmentWidth), column_w);
    const int seg_left = (left + right) / 2 - seg_w / 2;
    const int seg_top = y + Scaled(kPickerVPad) + Scaled(kPickerTop);
    g.segment = {seg_left, seg_top, seg_left + seg_w,
                 seg_top + Scaled(kSegmentHeight)};
    g.seg_icon = {g.segment.left, g.segment.top,
                  (g.segment.left + g.segment.right) / 2, g.segment.bottom};
    g.seg_emoji = {g.seg_icon.right, g.segment.top, g.segment.right,
                   g.segment.bottom};
    g.grid = {left + Scaled(kGridHPad), g.segment.bottom + Scaled(kSegmentToGrid),
              right - Scaled(kGridHPad),
              y + block_h - Scaled(kPickerVPad) - Scaled(kGridVPad)};
    const int grid_w = std::max(1, static_cast<int>(g.grid.right - g.grid.left));
    const int item = Scaled(kItem);
    g.columns = std::max(1, (grid_w + Scaled(kColumnSpacing)) /
                                (item + Scaled(kColumnSpacing)));
    // Flexible columns: the leftover width spreads between the cells.
    g.column_stride =
        g.columns > 1 ? (grid_w - item) / (g.columns - 1) : 0;
    const size_t count = Cells().size();
    const int rows = static_cast<int>((count + g.columns - 1) / g.columns);
    g.grid_content_height =
        rows > 0 ? rows * (item + Scaled(kRowSpacing)) - Scaled(kRowSpacing) +
                       2 * Scaled(kGridVPad)
                 : 0;
  }
  y += block_h + Scaled(kFormGap);

  {
    const int ring = Scaled(kRing);
    const int color_h = Scaled(kColorVPad) + ring + Scaled(kColorGap) + 1 +
                        Scaled(kColorGap) + Scaled(kSliderHeight) +
                        Scaled(kColorVPad);
    g.color_block = {left, y, right, y + color_h};
    const auto& themes = PhiBuiltInThemes();
    g.dot_count = std::min<size_t>(themes.size(), 8);
    const int inner_left = left + Scaled(kColorHPad);
    const int inner_right = right - Scaled(kColorHPad);
    const int cell_w = g.dot_count > 0
                           ? (inner_right - inner_left) /
                                 static_cast<int>(g.dot_count)
                           : 0;
    const int dots_top = y + Scaled(kColorVPad);
    for (size_t i = 0; i < g.dot_count; ++i) {
      const int cx = inner_left + cell_w * static_cast<int>(i) + cell_w / 2;
      g.dots[i] = {cx - ring / 2, dots_top, cx - ring / 2 + ring,
                   dots_top + ring};
    }
    const int divider_y = dots_top + ring + Scaled(kColorGap);
    g.divider = {inner_left, divider_y, inner_right, divider_y + 1};
    const int slider_top = divider_y + 1 + Scaled(kColorGap);
    g.slider = {inner_left, slider_top, inner_right,
                slider_top + Scaled(kSliderHeight)};
    y += color_h + Scaled(kSectionGap);
  }

  {
    const int button_h = Scaled(kButtonTextHeight) + 2 * Scaled(kButtonVPad);
    const int cx = (left + right) / 2;
    g.create_button = {cx - create_w / 2, y, cx - create_w / 2 + create_w,
                       y + button_h};
    y += button_h + Scaled(kActionGap);
    g.cancel_button = {cx - cancel_w / 2, y, cx - cancel_w / 2 + cancel_w,
                       y + Scaled(kButtonTextHeight)};
    y += Scaled(kButtonTextHeight) + Scaled(kColumnVPad);
  }
  g.content_height = y;
  g.column = {left, 0, right, y};

  // Vertically centred when shorter than the sidebar; scrolled otherwise so
  // the create button stays reachable (the Mac's ScrollView).
  const int offset = g.content_height <= h
                         ? (h - g.content_height) / 2
                         : -std::max(0, std::min(form_scroll_,
                                                 g.content_height - h));
  const auto shift = [offset](RECT* r) {
    r->top += offset;
    r->bottom += offset;
  };
  for (RECT* r : {&g.column, &g.title, &g.subtitle, &g.name_row,
                  &g.profile_row, &g.pill, &g.icon_block, &g.segment,
                  &g.seg_icon, &g.seg_emoji, &g.grid, &g.color_block,
                  &g.divider, &g.slider, &g.create_button,
                  &g.cancel_button}) {
    shift(r);
  }
  for (size_t i = 0; i < g.dot_count; ++i) {
    shift(&g.dots[i]);
  }
  return g;
}

RECT PhiCreateSpacePanel::CellRect(const Geometry& g, size_t index) const {
  const int item = Scaled(kItem);
  const int col = static_cast<int>(index % static_cast<size_t>(g.columns));
  const int row = static_cast<int>(index / static_cast<size_t>(g.columns));
  const int x = g.grid.left + col * g.column_stride;
  const int y = g.grid.top + Scaled(kGridVPad) +
                row * (item + Scaled(kRowSpacing)) - grid_scroll_;
  return {x, y, x + item, y + item};
}

void PhiCreateSpacePanel::ClampScroll(const Geometry& g) {
  const int grid_h = g.grid.bottom - g.grid.top;
  grid_scroll_ = std::max(0, std::min(grid_scroll_,
                                      g.grid_content_height - grid_h));
  RECT rc = {};
  if (hwnd_) {
    ::GetClientRect(hwnd_, &rc);
  }
  form_scroll_ = std::max(
      0, std::min(form_scroll_,
                  g.content_height - static_cast<int>(rc.bottom - rc.top)));
}

void PhiCreateSpacePanel::LayoutField() {
  if (!field_) {
    return;
  }
  const Geometry g = ComputeGeometry();
  const int text_h = Scaled(17);
  const int left = g.name_row.left + Scaled(kRowLead);
  const int right = g.name_row.right - Scaled(kRowLead);
  ::SetWindowPos(field_, nullptr, left,
                 g.name_row.top + (Scaled(kRowHeight) - text_h) / 2,
                 std::max(1, right - left), text_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

PhiCreateSpacePanel::Hit PhiCreateSpacePanel::HitTest(const Geometry& g,
                                                      POINT pt,
                                                      int* index) const {
  *index = -1;
  if (::PtInRect(&g.pill, pt)) {
    return Hit::kProfilePill;
  }
  if (::PtInRect(&g.seg_icon, pt)) {
    return Hit::kTabIcon;
  }
  if (::PtInRect(&g.seg_emoji, pt)) {
    return Hit::kTabEmoji;
  }
  if (::PtInRect(&g.grid, pt)) {
    const size_t count = Cells().size();
    for (size_t i = 0; i < count; ++i) {
      const RECT cell = CellRect(g, i);
      if (cell.bottom < g.grid.top) {
        continue;
      }
      if (cell.top > g.grid.bottom) {
        break;
      }
      if (::PtInRect(&cell, pt)) {
        *index = static_cast<int>(i);
        return Hit::kCell;
      }
    }
    return Hit::kNone;
  }
  for (size_t i = 0; i < g.dot_count; ++i) {
    RECT hit = g.dots[i];
    ::InflateRect(&hit, Scaled(4), Scaled(4));
    if (::PtInRect(&hit, pt)) {
      *index = static_cast<int>(i);
      return Hit::kDot;
    }
  }
  if (::PtInRect(&g.slider, pt)) {
    return Hit::kSlider;
  }
  if (::PtInRect(&g.create_button, pt)) {
    return Hit::kCreate;
  }
  RECT cancel = g.cancel_button;
  ::InflateRect(&cancel, Scaled(6), Scaled(4));
  if (::PtInRect(&cancel, pt)) {
    return Hit::kCancel;
  }
  return Hit::kNone;
}

void PhiCreateSpacePanel::SelectTheme(size_t index) {
  const auto& themes = PhiBuiltInThemes();
  if (index >= themes.size() || theme_id_ == themes[index].id) {
    return;
  }
  theme_id_ = themes[index].id;
  PushPreview();
  Refresh();
}

void PhiCreateSpacePanel::SelectIcon(size_t index) {
  const auto& cells = Cells();
  if (index >= cells.size() || icon_ == cells[index].second) {
    return;
  }
  icon_ = cells[index].second;
  if (controller_) {
    controller_->SetCreateSpaceIcon(icon_);  // The placeholder pip follows.
  }
  Refresh();
}

void PhiCreateSpacePanel::SetSliderFromPoint(const Geometry& g, int x) {
  const int knob = Scaled(kSliderKnob);
  const int span = std::max(1, static_cast<int>(g.slider.right - g.slider.left) - knob);
  const int value = std::max(
      0, std::min(100, (x - static_cast<int>(g.slider.left) - knob / 2) * 100 / span));
  if (value == slider_ && user_adjusted_slider_) {
    return;
  }
  slider_ = value;
  user_adjusted_slider_ = true;
  PushPreview();
  Refresh();
}

void PhiCreateSpacePanel::PushPreview() {
  if (controller_) {
    controller_->PreviewCreateSpaceTheme(theme_id_, slider_);
  }
}

void PhiCreateSpacePanel::ShowProfileMenu(const Geometry& g) {
  if (!controller_ || !controller_->host()) {
    return;
  }
  ReloadProfiles();
  HMENU menu = ::CreatePopupMenu();
  if (!menu) {
    return;
  }
  for (size_t i = 0; i < profiles_.size(); ++i) {
    ::AppendMenuW(menu,
                  MF_STRING | (profiles_[i].first == profile_id_ ? MF_CHECKED
                                                                 : 0),
                  static_cast<UINT>(i) + 1, profiles_[i].second.c_str());
  }
  const UINT kNewProfile = static_cast<UINT>(profiles_.size()) + 1;
  ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  ::AppendMenuW(menu, MF_STRING, kNewProfile,
                PhiStr("sidebar.createSpace.profilePicker.newProfileAction"));
  POINT screen = {g.pill.left, g.pill.bottom};
  ::ClientToScreen(hwnd_, &screen);
  const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y,
      0, hwnd_, nullptr));
  ::DestroyMenu(menu);
  if (chosen == kNewProfile) {
    // The Mac's promptCreateProfile: a name, then the profile; the picker
    // selects it once the host reports it (OnProfileCreated).
    std::wstring name;
    if (PhiPromptText(controller_->shell_hwnd(),
                      PhiStr("settings.profiles.createDialog.title"),
                      PhiStr("settings.profiles.createDialog.message"),
                      &name)) {
      controller_->host()->RequestCreateProfileForSpaceForm(
          controller_->window_id(), base::WideToUTF8(name));
    }
  } else if (chosen >= 1 && chosen <= profiles_.size()) {
    profile_id_ = profiles_[chosen - 1].first;
  }
  Refresh();
}

void PhiCreateSpacePanel::Create() {
  if (!controller_ || !controller_->host()) {
    return;
  }
  PhiNewSpaceSpec spec;
  if (field_) {
    wchar_t buffer[256] = {};
    ::GetWindowTextW(field_, buffer, 255);
    std::wstring name(buffer);
    const auto is_space = [](wchar_t c) {
      return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
    };
    while (!name.empty() && is_space(name.back())) {
      name.pop_back();
    }
    size_t start = 0;
    while (start < name.size() && is_space(name[start])) {
      ++start;
    }
    name.erase(0, start);
    if (name.empty()) {
      // "Space N" for the N-th Space — the Mac's default.
      name = PhiStrF("sidebar.createSpace.defaultName",
                     {std::to_wstring(controller_->host()->Spaces().size() +
                                      1)});
    }
    spec.name = base::WideToUTF8(name);
  }
  spec.profile_id = profile_id_;
  spec.icon = icon_;
  spec.theme_id = theme_id_;
  spec.saturation = user_adjusted_slider_ ? slider_ : -1;
  // The Mac derives the stored colorHex from the theme's overlay colour, so
  // the sidebar tint matches the pinned theme on both clients.
  const PhiTheme& theme = PhiThemeById(theme_id_);
  const bool dark = controller_->CurrentPalette().dark;
  spec.color_hex = HexOf(dark ? theme.dark.overlay : theme.light.overlay);
  controller_->CommitCreateSpaceForm(spec);
}

void PhiCreateSpacePanel::Cancel() {
  if (controller_) {
    controller_->CancelCreateSpaceForm();
  }
}

void PhiCreateSpacePanel::Paint(HDC dc) {
  if (!controller_) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd_, &rc);
  const PhiPalette palette = controller_->CurrentPalette();
  // The sidebar's own tint under the form (the Mac's themed backdrop): the
  // same top-to-bottom ramp the shell paints, continued at this window's
  // offset so the two never seam.
  {
    RECT shell = {};
    POINT origin = {0, 0};
    if (HWND owner = controller_->shell_hwnd()) {
      ::GetClientRect(owner, &shell);
      ::MapWindowPoints(hwnd_, owner, &origin, 1);
    }
    const int total = std::max(1, static_cast<int>(shell.bottom));
    const int bands = 24;
    const int h = rc.bottom - rc.top;
    for (int i = 0; i < bands; ++i) {
      RECT band = {rc.left, rc.top + h * i / bands, rc.right,
                   rc.top + h * (i + 1) / bands};
      const int at = std::max(0, std::min(total, static_cast<int>(
                                                   origin.y + band.top)));
      const COLORREF tint = Blend(palette.ground_bottom, palette.ground_top,
                                  at * 255 / total);
      HBRUSH brush = ::CreateSolidBrush(tint);
      ::FillRect(dc, &band, brush);
      ::DeleteObject(brush);
    }
  }
  const Geometry g = ComputeGeometry();
  ::SetBkMode(dc, TRANSPARENT);
  const COLORREF ink = palette.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0);
  const COLORREF ground = palette.ground_top;
  const COLORREF fill4 = Blend(ink, ground, 10);    // primary 4%
  const COLORREF fill6 = Blend(ink, ground, 15);    // primary 6%
  const COLORREF fill10 = Blend(ink, ground, 25);   // hover / selected
  const COLORREF label = palette.label;             // primary 85%
  const COLORREF label50 = Blend(ink, ground, 128); // primary 50%
  const COLORREF divider = Blend(ink, ground, 31);

  // Header.
  HGDIOBJ prev = ::SelectObject(dc, title_font_);
  ::SetTextColor(dc, label);
  RECT title = g.title;
  PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.title"), -1, &title,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SelectObject(dc, subtitle_font_);
  ::SetTextColor(dc, label50);
  RECT subtitle = g.subtitle;
  PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.subtitle"), -1, &subtitle,
              DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);

  // Name field (the EDIT paints its own text over this row).
  FillRound(dc, g.name_row, Scaled(kRowRadius), fill4);

  // Profile row: label left, the picker pill right.
  FillRound(dc, g.profile_row, Scaled(kRowRadius), fill4);
  ::SelectObject(dc, body_font_);
  ::SetTextColor(dc, label);
  RECT profile_label = {g.profile_row.left + Scaled(kRowLead),
                        g.profile_row.top, g.pill.left - Scaled(8),
                        g.profile_row.bottom};
  PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.profilePicker.label"), -1,
              &profile_label,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (!PhiFillCapsuleAA(dc, g.pill,
                        hover_ == Hit::kProfilePill ? fill10 : fill6)) {
    FillRound(dc, g.pill, (g.pill.bottom - g.pill.top) / 2,
              hover_ == Hit::kProfilePill ? fill10 : fill6);
  }
  ::SelectObject(dc, pill_font_);
  ::SetTextColor(dc, label);
  RECT pill_text = {g.pill.left + Scaled(kPillLead), g.pill.top,
                    g.pill.right - Scaled(kPillTrail) - Scaled(kPillChevron) -
                        Scaled(kPillChevronGap),
                    g.pill.bottom};
  const std::wstring profile_name = SelectedProfileName();
  PhiDrawTextAA(dc, profile_name.c_str(), -1, &pill_text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);
  ::SetTextColor(dc, label50);
  RECT chevron = {g.pill.right - Scaled(kPillTrail) - Scaled(kPillChevron),
                  g.pill.top, g.pill.right - Scaled(kPillTrail), g.pill.bottom};
  PhiDrawTextAA(dc, L"↕", -1, &chevron,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // Icon picker: the segmented control over the scrolling grid.
  FillRound(dc, g.icon_block, Scaled(kRowRadius), fill4);
  FillRound(dc, g.segment, Scaled(6), fill6);
  {
    const RECT& selected = tab_ == Tab::kIcon ? g.seg_icon : g.seg_emoji;
    RECT knob = selected;
    ::InflateRect(&knob, -Scaled(2), -Scaled(2));
    FillRound(dc, knob, Scaled(5), palette.selected);
    ::SelectObject(dc, body_font_);
    ::SetTextColor(dc, tab_ == Tab::kIcon ? label : label50);
    RECT seg = g.seg_icon;
    PhiDrawTextAA(dc, PhiStr("common.iconPicker.iconTabTitle"), -1, &seg,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SetTextColor(dc, tab_ == Tab::kEmoji ? label : label50);
    seg = g.seg_emoji;
    PhiDrawTextAA(dc, PhiStr("common.iconPicker.emojiTabTitle"), -1, &seg,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  PaintCells(dc, g, palette);

  // Colour: the theme dots, a divider, the saturation slider.
  FillRound(dc, g.color_block, Scaled(kRowRadius), fill4);
  {
    const auto& themes = PhiBuiltInThemes();
    const PhiAppearance appearance =
        controller_->host() ? controller_->host()->Appearance()
                            : PhiAppearance::kSystem;
    for (size_t i = 0; i < g.dot_count; ++i) {
      const PhiPalette resolved =
          PhiResolvePalette(themes[i], appearance, slider_);
      const bool pure = i == 0;
      const COLORREF fill = pure ? RGB(0xFF, 0xFF, 0xFF) : resolved.accent;
      if (theme_id_ == themes[i].id) {
        StrokeCircle(dc, g.dots[i], resolved.accent, Scaled(2));
      }
      RECT dot = g.dots[i];
      const int inset = (Scaled(kRing) - Scaled(kDot)) / 2;
      ::InflateRect(&dot, -inset, -inset);
      FillCircle(dc, dot, fill);
      if (pure) {
        StrokeCircle(dc, dot, divider, 1);  // showsContrastBorder
      }
    }
    HBRUSH line = ::CreateSolidBrush(divider);
    ::FillRect(dc, &g.divider, line);
    ::DeleteObject(line);
    PaintSlider(dc, g.slider, palette);
  }

  // Actions.
  {
    const PhiPalette selected_palette = PhiResolvePalette(
        PhiThemeById(theme_id_),
        controller_->host() ? controller_->host()->Appearance()
                            : PhiAppearance::kSystem,
        slider_);
    COLORREF accent = selected_palette.accent;
    if (hover_ == Hit::kCreate) {
      accent = Blend(RGB(0, 0, 0), accent, 30);
    }
    if (!PhiFillCapsuleAA(dc, g.create_button, accent)) {
      FillRound(dc, g.create_button,
                (g.create_button.bottom - g.create_button.top) / 2, accent);
    }
    ::SelectObject(dc, body_medium_font_);
    ::SetTextColor(dc, RGB(0xF7, 0xF7, 0xF7));  // white 95%
    RECT create = g.create_button;
    PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.createButton"), -1, &create,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SetTextColor(dc, hover_ == Hit::kCancel ? label : Blend(ink, ground, 200));
    RECT cancel = g.cancel_button;
    PhiDrawTextAA(dc, PhiStr("sidebar.createSpace.cancelButton"), -1, &cancel,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  ::SelectObject(dc, prev);
}

void PhiCreateSpacePanel::PaintCells(HDC dc,
                                     const Geometry& g,
                                     const PhiPalette& palette) {
  const COLORREF ink = palette.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0);
  const COLORREF ground = palette.ground_top;
  const auto& cells = Cells();
  const int save = ::SaveDC(dc);
  ::IntersectClipRect(dc, g.grid.left, g.grid.top, g.grid.right,
                      g.grid.bottom);
  for (size_t i = 0; i < cells.size(); ++i) {
    const RECT cell = CellRect(g, i);
    if (cell.bottom < g.grid.top) {
      continue;
    }
    if (cell.top > g.grid.bottom) {
      break;
    }
    const bool selected = cells[i].second == icon_;
    const bool hovered =
        hover_ == Hit::kCell && hover_index_ == static_cast<int>(i);
    if (selected || hovered) {
      FillRound(dc, cell, Scaled(kItemRadius),
                Blend(ink, ground, selected ? 25 : 15));
    }
    // Inset: the colour pass repaints its whole rect from a sampled corner,
    // and a selected cell's rounded fill must survive at the corners.
    RECT glyph = cell;
    ::InflateRect(&glyph, -Scaled(3), -Scaled(3));
    const std::wstring& text = cells[i].first;
    if (PhiDrawCatalogArtwork(dc, glyph, text, Scaled(kItemIcon),
                              palette.label)) {
      continue;
    }
    if (!PhiIsPrivateGlyph(text) &&
        PhiDrawColorText(dc, glyph, text, Scaled(kEmojiFont), palette.label)) {
      continue;
    }
    HFONT font = PhiIsPrivateGlyph(text) ? mdl2_font_ : emoji_font_;
    HGDIOBJ prev = font ? ::SelectObject(dc, font) : nullptr;
    ::SetTextColor(dc, palette.label);
    RECT text_rc = cell;
    PhiDrawTextAA(dc, text.c_str(), -1, &text_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (prev) {
      ::SelectObject(dc, prev);
    }
  }
  ::RestoreDC(dc, save);
}

void PhiCreateSpacePanel::PaintSlider(HDC dc,
                                      const RECT& bounds,
                                      const PhiPalette& palette) {
  // ThemeOpacitySliderView, as the Settings pane draws it: a 10pt rounded
  // bar ramping the theme overlay's saturation 0.1 → 0.9 (Pure ramps
  // brightness, brightest at the left), a 1pt border, an 18pt white knob.
  const PhiTheme& theme = PhiThemeById(theme_id_);
  const COLORREF overlay = palette.dark ? theme.dark.overlay : theme.light.overlay;
  const bool pure = theme_id_ == "pure";
  const int mid = (bounds.top + bounds.bottom) / 2;
  RECT track = {bounds.left, mid - Scaled(5), bounds.right, mid + Scaled(5)};
  const int span = track.right - track.left;
  double hue = 0, sat = 0, val = 0;
  RgbToHsv(overlay, &hue, &sat, &val);
  // Painted square, then the corners are knocked back to the ground
  // (antialiased) — a region clip would cut the rounded track on the
  // pixel grid.
  const int bands = 32;
  for (int i = 0; i < bands; ++i) {
    RECT band = {track.left + span * i / bands, track.top,
                 track.left + span * (i + 1) / bands, track.bottom};
    const double t = (i + 0.5) / bands;
    COLORREF tint;
    if (pure) {
      const double hi = palette.dark ? 0.40 : 0.94;
      const double lo = palette.dark ? 0.0 : 0.84;
      tint = HsvToRgb(hue, sat, hi + (lo - hi) * t);
    } else {
      tint = HsvToRgb(hue, 0.1 + 0.8 * t, val);
    }
    HBRUSH brush = ::CreateSolidBrush(tint);
    ::FillRect(dc, &band, brush);
    ::DeleteObject(brush);
  }
  PhiKnockOutRoundRectAA(dc, track, Scaled(5), palette.ground_top);
  PhiStrokeRoundRectAA(dc, track, Scaled(5), palette.border, 1);
  const int knob = Scaled(kSliderKnob);
  const int at = track.left + (span - knob) * slider_ / 100;
  RECT dot = {at, mid - knob / 2, at + knob, mid + knob / 2};
  FillCircle(dc, dot, RGB(0xFF, 0xFF, 0xFF));
  StrokeCircle(dc, dot, palette.border, 1);
}

// static
LRESULT CALLBACK PhiCreateSpacePanel::PanelProc(HWND hwnd,
                                                UINT msg,
                                                WPARAM w,
                                                LPARAM l) {
  auto* self = reinterpret_cast<PhiCreateSpacePanel*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
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
      // AnimateWindow's slide and PrintWindow captures render through this.
      if (self) {
        self->Paint(reinterpret_cast<HDC>(w));
      }
      return 0;
    case WM_SIZE:
      if (self) {
        self->LayoutField();
      }
      return 0;
    case WM_MOUSEMOVE:
      if (self) {
        const POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const Geometry g = self->ComputeGeometry();
        if (self->dragging_slider_) {
          self->SetSliderFromPoint(g, pt.x);
          return 0;
        }
        int index = -1;
        const Hit hit = self->HitTest(g, pt, &index);
        if (hit != self->hover_ || index != self->hover_index_) {
          self->hover_ = hit;
          self->hover_index_ = index;
          ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!self->tracking_leave_) {
          TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
          ::TrackMouseEvent(&track);
          self->tracking_leave_ = true;
        }
      }
      return 0;
    case WM_MOUSELEAVE:
      if (self) {
        self->tracking_leave_ = false;
        if (self->hover_ != Hit::kNone) {
          self->hover_ = Hit::kNone;
          self->hover_index_ = -1;
          ::InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      return 0;
    case WM_LBUTTONDOWN:
      if (self) {
        const POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const Geometry g = self->ComputeGeometry();
        int index = -1;
        switch (self->HitTest(g, pt, &index)) {
          case Hit::kProfilePill:
            self->ShowProfileMenu(g);
            break;
          case Hit::kTabIcon:
          case Hit::kTabEmoji: {
            const Tab tab =
                ::PtInRect(&g.seg_emoji, pt) ? Tab::kEmoji : Tab::kIcon;
            if (tab != self->tab_) {
              self->tab_ = tab;
              self->grid_scroll_ = 0;
              ::InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
          }
          case Hit::kCell:
            self->SelectIcon(static_cast<size_t>(index));
            break;
          case Hit::kDot:
            self->SelectTheme(static_cast<size_t>(index));
            break;
          case Hit::kSlider:
            self->dragging_slider_ = true;
            ::SetCapture(hwnd);
            self->SetSliderFromPoint(g, pt.x);
            break;
          case Hit::kCreate:
            self->Create();
            return 0;
          case Hit::kCancel:
            self->Cancel();
            return 0;
          case Hit::kNone:
            break;
        }
        // The ground takes focus, so Escape / Enter still reach the form.
        if (!self->dragging_slider_ && ::PtInRect(&g.name_row, pt) &&
            self->field_) {
          ::SetFocus(self->field_);
        } else {
          ::SetFocus(hwnd);
        }
      }
      return 0;
    case WM_LBUTTONUP:
      if (self && self->dragging_slider_) {
        self->dragging_slider_ = false;
        ::ReleaseCapture();
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (self) {
        self->dragging_slider_ = false;
      }
      return 0;
    case WM_MOUSEWHEEL:
      if (self) {
        POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ::ScreenToClient(hwnd, &pt);
        Geometry g = self->ComputeGeometry();
        const int notches = GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        const int step =
            self->Scaled(kItem + kRowSpacing) * kWheelStep * notches;
        if (::PtInRect(&g.grid, pt) &&
            g.grid_content_height > g.grid.bottom - g.grid.top) {
          self->grid_scroll_ -= step;
        } else {
          self->form_scroll_ -= step;
        }
        self->ClampScroll(g);
        self->LayoutField();
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_KEYDOWN:
      if (self) {
        if (w == VK_ESCAPE) {
          self->Cancel();
          return 0;
        }
        if (w == VK_RETURN) {
          self->Create();
          return 0;
        }
      }
      break;
    case WM_CTLCOLOREDIT:
      if (self && self->controller_ &&
          reinterpret_cast<HWND>(l) == self->field_) {
        const PhiPalette palette = self->controller_->CurrentPalette();
        const COLORREF ink =
            palette.dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0);
        const COLORREF fill = Blend(ink, palette.ground_top, 10);
        HDC edit_dc = reinterpret_cast<HDC>(w);
        ::SetTextColor(edit_dc, palette.label);
        ::SetBkColor(edit_dc, fill);  // Seamless with the row's wash.
        if (!self->field_brush_ || self->field_brush_color_ != fill) {
          if (self->field_brush_) {
            ::DeleteObject(self->field_brush_);
          }
          self->field_brush_ = ::CreateSolidBrush(fill);
          self->field_brush_color_ = fill;
        }
        return reinterpret_cast<LRESULT>(self->field_brush_);
      }
      break;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

// static
LRESULT CALLBACK PhiCreateSpacePanel::FieldProc(HWND hwnd,
                                                UINT msg,
                                                WPARAM w,
                                                LPARAM l) {
  auto* self = reinterpret_cast<PhiCreateSpacePanel*>(
      ::GetWindowLongPtrW(::GetParent(hwnd), GWLP_USERDATA));
  const WNDPROC base = self && self->field_base_proc_
                           ? self->field_base_proc_
                           : ::DefWindowProcW;
  switch (msg) {
    case WM_KEYDOWN:
      if (self && w == VK_ESCAPE) {
        self->Cancel();
        return 0;
      }
      if (self && w == VK_RETURN) {
        self->Create();
        return 0;
      }
      break;
    case WM_CHAR:
      if (w == VK_RETURN || w == VK_ESCAPE) {
        return 0;  // No bell for the keys the form consumed.
      }
      break;
    default:
      break;
  }
  return ::CallWindowProcW(base, hwnd, msg, w, l);
}

}  // namespace phi_client
