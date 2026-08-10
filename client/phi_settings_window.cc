// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_settings_window.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_prompt.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiSettingsWindow";

// The Mac window: content is a FIXED 680x561 in every pane
// (each pane pins view.snp to that size); the toolbar rides above it.
constexpr int kWidth = 680;
constexpr int kHeight = 561;
constexpr int kToolbarHeight = 68;
constexpr int kToolbarItemWidth = 78;
constexpr int kToolbarIcon = 22;
// Page insets 36 on every side; 24 between sections; 16 title -> card.
constexpr int kMargin = 36;
constexpr int kSectionGap = 24;
constexpr int kTitleToCard = 16;
constexpr int kCardPadding = 12;   // Inner horizontal padding of a card.
constexpr int kRowPadding = 12;    // Vertical padding around a row's content.
constexpr int kSectionTitleHeight = 15;  // 12pt section titles.
constexpr int kHintHeight = 15;          // 11pt hints.
constexpr int kRowLabelHeight = 16;      // 13pt labels.
constexpr int kCardRadius = 7;
constexpr int kRadius = 6;
// Theme swatches: a 22pt dot in a 26pt ring frame, 2pt ring when selected,
// caption (11pt) below only while selected (ThemeSwatchView).
constexpr int kSwatchDot = 22;
constexpr int kSwatchRing = 26;
constexpr int kSwatchItemGeneral = 30;   // Per-item width in General…
constexpr int kSwatchGapGeneral = 13;    // …with 13pt spacing.
constexpr int kSwatchItemSpaces = 26;    // Tighter in the Spaces detail:
constexpr int kSwatchGapSpaces = 3;      // width 26, spacing 3.
constexpr int kSwatchBlock = 26 + 8 + 15;  // ring + gap + caption line.
// The saturation slider: 10pt-tall full-width rounded bar with a gradient,
// an 18pt white knob (ThemeOpacitySliderView).
constexpr int kSliderHeight = 20;
constexpr int kSliderGeneral = 324;
constexpr int kSliderSpaces = 220;
constexpr int kSliderKnob = 18;
// Layout / appearance / new-tab picture cards: 122x72 image, r5, 2pt accent
// ring when selected, 11pt caption 10 below, 16 apart (GeneralSttingCardView).
constexpr int kImageCardW = 122;
constexpr int kImageCardH = 72;
constexpr int kImageCardGap = 16;
constexpr int kImageCardBlock = 72 + 10 + 15;
// Switches: the Mac's .mini switch is about 26x15.
constexpr int kToggleWidth = 26;
constexpr int kToggleHeight = 15;
// Master-detail lists (Profiles 240 wide, Spaces 280) with a 34pt tool strip.
constexpr int kProfilesListWidth = 240;
constexpr int kSpacesListWidth = 280;
constexpr int kMasterDetailGap = 16;
constexpr int kListHeaderHeight = 34;
constexpr int kListRowHeight = 30;
constexpr int kToolStripHeight = 34;
constexpr int kSpacesBlockHeight = 431;  // The Mac's fixed master-detail band.
// The Profiles trailing pill: inner width 110, H10/V5, primary@0.06, r6.
constexpr int kPillWidth = 130;
constexpr int kPillHeight = 24;
// Icon picker grid (IconPicker): 26pt cells, 4pt gaps, r10 cells.
constexpr int kEmojiCell = 26;
constexpr int kEmojiGap = 4;

// The Mac's panes(), in order. Labels are the toolbar captions — the Account
// pane is titled with the user's name ("You" when signed out); pane 5 is
// "Phi & AI" and pane 6 "Phi Link". Ids stay the wire names.
struct SectionDef {
  const char* id;
  const wchar_t* label;
  const wchar_t* glyph;
};
constexpr std::array<SectionDef, 8> kSections = {{
    {"account", L"You", L"○"},
    {"general", L"General", L"⚙"},
    {"profiles", L"Profiles", L"◒"},
    {"spaces", L"Spaces", L"▦"},
    {"ai", L"Phi & AI", L"✦"},
    {"phiLink", L"Phi Link", L"⚭"},
    {"shortcuts", L"Shortcuts", L"⌘"},
    {"developer", L"Developer", L"⚒"},
}};

PhiSettingsWindow* g_instance = nullptr;

void FillRounded(HDC dc, const RECT& r, COLORREF fill, int radius) {
  if (r.right <= r.left || r.bottom <= r.top) {
    return;
  }
  HBRUSH brush = ::CreateSolidBrush(fill);
  HGDIOBJ old_brush = ::SelectObject(dc, brush);
  HGDIOBJ old_pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
  ::RoundRect(dc, r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  ::DeleteObject(brush);
}

void StrokeRounded(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
                   int radius, int width) {
  if (r.right <= r.left || r.bottom <= r.top) {
    return;
  }
  HBRUSH brush = ::CreateSolidBrush(fill);
  HPEN pen = ::CreatePen(PS_SOLID, width, border);
  HGDIOBJ old_brush = ::SelectObject(dc, brush);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  ::RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  ::DeleteObject(pen);
  ::DeleteObject(brush);
}

void Ellipse(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
             int border_width) {
  HBRUSH brush = ::CreateSolidBrush(fill);
  HPEN pen = border_width > 0 ? ::CreatePen(PS_SOLID, border_width, border)
                              : nullptr;
  HGDIOBJ old_brush = ::SelectObject(dc, brush);
  HGDIOBJ old_pen =
      ::SelectObject(dc, pen ? static_cast<HGDIOBJ>(pen)
                             : ::GetStockObject(NULL_PEN));
  ::Ellipse(dc, r.left, r.top, r.right, r.bottom);
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  if (pen) {
    ::DeleteObject(pen);
  }
  ::DeleteObject(brush);
}

// `over` at alpha/255 on `base` — the Mac's percentage overlays, opaque.
COLORREF Blend(COLORREF over, COLORREF base, int alpha) {
  auto mix = [&](int a, int b) { return (a * alpha + b * (255 - alpha)) / 255; };
  return RGB(mix(GetRValue(over), GetRValue(base)),
             mix(GetGValue(over), GetGValue(base)),
             mix(GetBValue(over), GetBValue(base)));
}

// "a\tb\tc" -> {"a","b","c"}
std::vector<std::string> SplitTabs(const std::string& value) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    const size_t tab = value.find('\t', start);
    if (tab == std::string::npos) {
      parts.push_back(value.substr(start));
      return parts;
    }
    parts.push_back(value.substr(start, tab - start));
    start = tab + 1;
  }
}

}  // namespace

PhiSettingsWindow::~PhiSettingsWindow() {
  if (font_) {
    ::DeleteObject(font_);
  }
  if (title_font_) {
    ::DeleteObject(title_font_);
  }
  if (small_font_) {
    ::DeleteObject(small_font_);
  }
  if (hint_font_) {
    ::DeleteObject(hint_font_);
  }
  if (emoji_font_) {
    ::DeleteObject(emoji_font_);
  }
  if (mdl2_font_) {
    ::DeleteObject(mdl2_font_);
  }
}

PhiSettingsWindow::PhiSettingsWindow(PhiSettingsHost* host) : host_(host) {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PhiSettingsWindow::WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc);
  }();
  (void)atom;

  hwnd_ = ::CreateWindowExW(0, kClassName, L"General",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr,
                            nullptr, ::GetModuleHandleW(nullptr), this);
  if (!hwnd_) {
    LOG(ERROR) << "[PhiClient] settings CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }
  if (const UINT dpi = ::GetDpiForWindow(hwnd_)) {
    dpi_ = dpi;
  }
  // Content 680x561, the toolbar band above it — fixed, non-resizable.
  RECT frame = {0, 0, Scaled(kWidth), Scaled(kToolbarHeight + kHeight)};
  ::AdjustWindowRectExForDpi(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             FALSE, 0, dpi_);
  ::SetWindowPos(hwnd_, nullptr, 0, 0, frame.right - frame.left,
                 frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER);
  EnsureFont();
  SyncTitle();
}

int PhiSettingsWindow::Scaled(int design_units) const {
  return ::MulDiv(design_units, static_cast<int>(dpi_), 96);
}

void PhiSettingsWindow::EnsureFont() {
  if (font_ && font_dpi_ == dpi_) {
    return;
  }
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                    &metrics, 0, dpi_)) {
    return;
  }
  // The Mac's ladder: 13pt rows, 12pt section titles, 11pt hints.
  auto make = [&](int size, int weight) {
    LOGFONTW lf = metrics.lfMessageFont;
    lf.lfHeight = -Scaled(size);
    lf.lfWeight = weight;
    return ::CreateFontIndirectW(&lf);
  };
  auto swap = [](HFONT* slot, HFONT built) {
    if (!built) {
      return;
    }
    if (*slot) {
      ::DeleteObject(*slot);
    }
    *slot = built;
  };
  swap(&font_, make(13, FW_NORMAL));
  swap(&title_font_, make(13, FW_SEMIBOLD));
  swap(&small_font_, make(12, FW_NORMAL));
  swap(&hint_font_, make(11, FW_NORMAL));
  LOGFONTW emoji = metrics.lfMessageFont;
  const std::wstring face = L"Segoe UI Emoji";
  std::fill(std::begin(emoji.lfFaceName), std::end(emoji.lfFaceName), L'\0');
  std::copy_n(face.begin(),
              std::min(face.size(), std::size(emoji.lfFaceName) - 1),
              std::begin(emoji.lfFaceName));
  emoji.lfHeight = -Scaled(14);
  swap(&emoji_font_, ::CreateFontIndirectW(&emoji));
  LOGFONTW mdl2 = metrics.lfMessageFont;
  const std::wstring mdl2_face = L"Segoe MDL2 Assets";
  std::fill(std::begin(mdl2.lfFaceName), std::end(mdl2.lfFaceName), L'\0');
  std::copy_n(mdl2_face.begin(),
              std::min(mdl2_face.size(), std::size(mdl2.lfFaceName) - 1),
              std::begin(mdl2.lfFaceName));
  mdl2.lfHeight = -Scaled(14);
  swap(&mdl2_font_, ::CreateFontIndirectW(&mdl2));
  font_dpi_ = dpi_;
}

void PhiSettingsWindow::SyncTitle() {
  // The Mac library titles the window with the selected pane's title.
  for (const SectionDef& section : kSections) {
    if (section_ == section.id) {
      ::SetWindowTextW(hwnd_, section.label);
      return;
    }
  }
}

// static
void PhiSettingsWindow::Open(PhiSettingsHost* host) {
  if (!g_instance || !g_instance->hwnd_ || !::IsWindow(g_instance->hwnd_)) {
    delete g_instance;
    g_instance = new PhiSettingsWindow(host);
  }
  if (g_instance && g_instance->hwnd_) {
    ::ShowWindow(g_instance->hwnd_, SW_SHOW);
    ::SetForegroundWindow(g_instance->hwnd_);
    ::InvalidateRect(g_instance->hwnd_, nullptr, TRUE);
  }
}

// static
HWND PhiSettingsWindow::Handle() {
  if (g_instance && g_instance->hwnd_ && ::IsWindow(g_instance->hwnd_)) {
    return g_instance->hwnd_;
  }
  return nullptr;
}

// static
void PhiSettingsWindow::Refresh() {
  if (Handle()) {
    ::InvalidateRect(g_instance->hwnd_, nullptr, TRUE);
  }
}

// static
std::string PhiSettingsWindow::CurrentSection() {
  return g_instance ? g_instance->section_ : std::string();
}

// static
bool PhiSettingsWindow::ShowSection(const std::string& name) {
  for (const SectionDef& section : kSections) {
    if (name != section.id) {
      continue;
    }
    // Developer exists only in developer mode, exactly as on Mac.
    if (name == "developer" && g_instance && g_instance->host_ &&
        !g_instance->host_->DeveloperMode()) {
      return false;
    }
    if (g_instance) {
      g_instance->section_ = name;
      g_instance->scroll_ = 0;
      g_instance->SyncTitle();
      Refresh();
    }
    return true;
  }
  return false;
}

// --- Section plumbing --------------------------------------------------------

void PhiSettingsWindow::BeginSection(int* y, const wchar_t* title) {
  if (title && title[0]) {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = title;
    heading.bounds = {Scaled(kMargin), *y, Scaled(kWidth - kMargin),
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    *y += Scaled(kSectionTitleHeight) + Scaled(kTitleToCard) -
          Scaled(kRowPadding);
  }
  // The card is emitted FIRST so it paints behind its rows; EndSection sizes
  // it around whatever was added.
  Item card;
  card.kind = ItemKind::kCard;
  card.bounds = {Scaled(kMargin), *y + Scaled(kRowPadding) - Scaled(4),
                 Scaled(kWidth - kMargin), 0};
  items_.push_back(std::move(card));
  open_card_ = items_.size() - 1;
  *y += Scaled(kRowPadding);
}

void PhiSettingsWindow::EndSection(int* y) {
  items_[open_card_].bounds.bottom = *y + Scaled(4);
  *y += Scaled(kSectionGap);
}

void PhiSettingsWindow::AddHint(int* y, const wchar_t* text) {
  Item hint;
  hint.kind = ItemKind::kHint;
  hint.text = text;
  hint.bounds = {Scaled(kMargin + kCardPadding), *y,
                 Scaled(kWidth - kMargin - kCardPadding),
                 *y + Scaled(kHintHeight)};
  items_.push_back(std::move(hint));
  *y += Scaled(kHintHeight) + Scaled(6);
}

RECT PhiSettingsWindow::AddRow(int* y, const wchar_t* title,
                               int control_height, int control_width) {
  const int row_height =
      std::max(Scaled(kRowLabelHeight), Scaled(control_height));
  Item label;
  label.kind = ItemKind::kRowLabel;
  label.text = title;
  label.bounds = {Scaled(kMargin + kCardPadding), *y,
                  Scaled(kWidth - kMargin - kCardPadding) -
                      Scaled(control_width) - Scaled(12),
                  *y + row_height};
  items_.push_back(std::move(label));
  // The control is TRAILING, as on Mac (label, Spacer, accessory).
  RECT control = {Scaled(kWidth - kMargin - kCardPadding) -
                      Scaled(control_width),
                  *y + (row_height - Scaled(control_height)) / 2,
                  Scaled(kWidth - kMargin - kCardPadding),
                  *y + (row_height + Scaled(control_height)) / 2};
  *y += row_height + Scaled(kRowPadding);
  return control;
}

void PhiSettingsWindow::AddChips(const RECT& area,
                                 const std::vector<std::wstring>& labels,
                                 int selected_index,
                                 const std::string& action) {
  // Retained for popup-style option groups: one popup pill showing the
  // current choice; clicking cycles to the next option (the Mac uses a menu).
  Item pill;
  pill.kind = ItemKind::kPopup;
  if (selected_index >= 0 &&
      selected_index < static_cast<int>(labels.size())) {
    pill.text = labels[static_cast<size_t>(selected_index)];
  }
  pill.value = selected_index;
  pill.action = action;
  pill.bounds = area;
  items_.push_back(std::move(pill));
}

// --- Panes -------------------------------------------------------------------

void PhiSettingsWindow::BuildGeneral(int* y) {
  const auto& themes = PhiBuiltInThemes();
  // With more than one Space, each Space owns colour and saturation; the Mac
  // locks these rows at 40% and adds the jump link.
  const bool locked = host_->SpaceSummaries().size() > 1;

  BeginSection(y, L"Theme");
  {
    // Row "Color": 22pt dots on 30pt centres, caption under the selected one.
    const int width = static_cast<int>(themes.size()) * kSwatchItemGeneral +
                      (static_cast<int>(themes.size()) - 1) * kSwatchGapGeneral;
    const RECT area = AddRow(y, L"Color", kSwatchBlock, width);
    for (size_t i = 0; i < themes.size(); ++i) {
      Item swatch;
      swatch.kind = ItemKind::kSwatch;
      swatch.text = base::UTF8ToWide(themes[i].name);
      swatch.payload = themes[i].id;
      swatch.selected = host_->ThemeId() == themes[i].id;
      swatch.enabled = !locked;
      const PhiPalette resolved = PhiResolvePalette(
          themes[i], host_->Appearance(), host_->Saturation());
      // The dot is the theme's own colour; Pure fills white (contrast ring).
      swatch.swatch = i == 0 ? RGB(0xFF, 0xFF, 0xFF) : resolved.accent;
      swatch.value = static_cast<int>(i);
      swatch.action = "theme";
      const int x = area.left + static_cast<int>(i) *
                                    Scaled(kSwatchItemGeneral +
                                           kSwatchGapGeneral);
      swatch.bounds = {x, area.top, x + Scaled(kSwatchItemGeneral),
                       area.top + Scaled(kSwatchBlock)};
      items_.push_back(std::move(swatch));
    }
  }
  AddDividerRow(y);
  {
    const RECT area = AddRow(y, L"Saturation", kSliderHeight, kSliderGeneral);
    Item slider;
    slider.kind = ItemKind::kSliderTrack;
    slider.value = host_->Saturation();
    slider.enabled = !locked;
    slider.action = "saturation";
    slider.swatch = host_->Palette().ground_bottom;
    slider.bounds = area;
    items_.push_back(std::move(slider));
  }
  if (locked) {
    AddDividerRow(y);
    // The Mac's exact lock text, with the jump link trailing.
    const RECT area = AddRow(y,
                             L"You have more than one Space, so each Space "
                             L"sets its own color and opacity.",
                             kRowLabelHeight, 160);
    Item link;
    link.kind = ItemKind::kLink;
    link.text = L"Edit in Spaces settings…";
    link.action = "gospaces";
    link.bounds = area;
    items_.push_back(std::move(link));
  }
  AddDividerRow(y);
  AddToggleRow(y, L"Apply theme to text selection on web pages",
               /*on=*/true, /*enabled=*/false, std::string());
  EndSection(y);

  BeginSection(y, L"Appearance");
  {
    // "Layout mode": three picture cards, in the Mac's allCases order —
    // Balanced, Performance, Comfortable.
    const RECT area = AddRow(y, L"Layout mode", kImageCardBlock,
                             3 * kImageCardW + 2 * kImageCardGap);
    const struct {
      const wchar_t* title;
      PhiLayoutMode mode;
      const char* sketch;
    } kCards[] = {
        {L"Balanced", PhiLayoutMode::kBalanced, "balanced"},
        {L"Performance", PhiLayoutMode::kPerformance, "performance"},
        {L"Comfortable", PhiLayoutMode::kComfortable, "comfortable"},
    };
    int i = 0;
    for (const auto& definition : kCards) {
      Item card;
      card.kind = ItemKind::kImageCard;
      card.text = definition.title;
      card.payload = definition.sketch;
      card.selected = host_->LayoutMode() == definition.mode;
      card.value = static_cast<int>(definition.mode);
      card.action = "layout";
      const int x = area.left + i * Scaled(kImageCardW + kImageCardGap);
      card.bounds = {x, area.top, x + Scaled(kImageCardW),
                     area.top + Scaled(kImageCardBlock)};
      items_.push_back(std::move(card));
      ++i;
    }
  }
  AddDividerRow(y);
  {
    const RECT area = AddRow(y, L"Color appearance", kImageCardBlock,
                             3 * kImageCardW + 2 * kImageCardGap);
    const struct {
      const wchar_t* title;
      PhiAppearance value;
      const char* sketch;
    } kCards[] = {
        {L"System", PhiAppearance::kSystem, "system"},
        {L"Light", PhiAppearance::kLight, "light"},
        {L"Dark", PhiAppearance::kDark, "dark"},
    };
    int i = 0;
    for (const auto& definition : kCards) {
      Item card;
      card.kind = ItemKind::kImageCard;
      card.text = definition.title;
      card.payload = definition.sketch;
      card.selected = host_->Appearance() == definition.value;
      card.value = static_cast<int>(definition.value);
      card.action = "appearance";
      const int x = area.left + i * Scaled(kImageCardW + kImageCardGap);
      card.bounds = {x, area.top, x + Scaled(kImageCardW),
                     area.top + Scaled(kImageCardBlock)};
      items_.push_back(std::move(card));
      ++i;
    }
  }
  EndSection(y);

  BeginSection(y, L"Language");
  AddChips(AddRow(y, L"Display language", kPillHeight, kPillWidth),
           {L"System Default"}, 0, std::string());
  EndSection(y);

  BeginSection(y, L"Browsing");
  {
    // "New tab behavior": two picture cards (New Tab Page requires AI on
    // Mac; there is no AI here yet, so it is drawn dim).
    const RECT area = AddRow(y, L"New tab behavior", kImageCardBlock,
                             2 * kImageCardW + kImageCardGap);
    const struct {
      const wchar_t* title;
      PhiNewTabBehavior value;
      const char* sketch;
      bool enabled;
    } kCards[] = {
        {L"New Tab Page", PhiNewTabBehavior::kNewTabPage, "ntp", true},
        {L"Omnibox", PhiNewTabBehavior::kOmnibox, "omnibox", true},
    };
    int i = 0;
    for (const auto& definition : kCards) {
      Item card;
      card.kind = ItemKind::kImageCard;
      card.text = definition.title;
      card.payload = definition.sketch;
      card.selected = host_->NewTabBehavior() == definition.value;
      card.enabled = definition.enabled;
      card.value = static_cast<int>(definition.value);
      card.action = "newtab";
      const int x = area.left + i * Scaled(kImageCardW + kImageCardGap);
      card.bounds = {x, area.top, x + Scaled(kImageCardW),
                     area.top + Scaled(kImageCardBlock)};
      items_.push_back(std::move(card));
      ++i;
    }
  }
  AddDividerRow(y);
  AddToggleRow(y, L"Always show full URL", host_->AlwaysShowFullUrl(),
               /*enabled=*/true, "fullurl");
  AddDividerRow(y);
  {
    const RECT area = AddRow(y, L"Auto picture-in-picture", kPillHeight,
                             kPillWidth);
    AddSubHint(y, L"Pop out playing video when you switch tabs or apps");
    Item pill;
    pill.kind = ItemKind::kPopup;
    pill.text = L"Normal";
    pill.enabled = false;
    pill.bounds = area;
    items_.push_back(std::move(pill));
  }
  AddDividerRow(y);
  {
    AddToggleRow(y, L"Restore last session", /*on=*/true, /*enabled=*/false,
                 std::string());
    AddSubHint(y, L"Reopen your windows and tabs the next time you open Phi.");
  }
  AddDividerRow(y);
  {
    const RECT area = AddRow(y, L"Additional browser settings",
                             kRowLabelHeight, 20);
    Item chevron;
    chevron.kind = ItemKind::kLink;
    chevron.text = L"›";
    chevron.action = "browsersettings";
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
    // The whole row opens chrome://settings, as on Mac.
    Item row;
    row.kind = ItemKind::kListRow;
    row.text.clear();
    row.action = "browsersettings";
    row.bounds = {Scaled(kMargin), area.top - Scaled(4),
                  Scaled(kWidth - kMargin), area.bottom + Scaled(4)};
    items_.push_back(std::move(row));
  }
  EndSection(y);

  BeginSection(y, L"Profile");
  if (host_->ProfileSummaries().size() > 1) {
    const RECT area = AddRow(y,
                             L"You have more than one profile, so each "
                             L"profile manages its own settings.",
                             kRowLabelHeight, 170);
    Item link;
    link.kind = ItemKind::kLink;
    link.text = L"Edit in Profiles settings…";
    link.action = "goprofiles";
    link.bounds = area;
    items_.push_back(std::move(link));
  } else {
    AddChips(AddRow(y, L"Search Engine", kPillHeight, kPillWidth),
             {L"Unavailable"}, 0, std::string());
    AddDividerRow(y);
    AddChips(AddRow(y, L"Download Location", kPillHeight, kPillWidth),
             {L"Choose…"}, 0, std::string());
  }
  EndSection(y);

  BeginSection(y, L"Developer");
  AddToggleRow(y, L"Developer mode", host_->DeveloperMode(), /*enabled=*/true,
               "devmode");
  AddDividerRow(y);
  AddSubHint(y, host_->DeveloperMode()
                    ? L"Agent access, permissions, and the password manager "
                      L"live in the Developer tab."
                    : L"Turning developer mode off turns off agent access "
                      L"and the agent password manager. An agent that "
                      L"connects later can ask you to turn it back on.");
  *y += Scaled(kRowPadding);
  EndSection(y);
}

void PhiSettingsWindow::AddDividerRow(int* y) {
  Item divider;
  divider.kind = ItemKind::kDivider;
  divider.bounds = {Scaled(kMargin + kCardPadding), *y - Scaled(6),
                    Scaled(kWidth - kMargin - kCardPadding),
                    *y - Scaled(6) + 1};
  items_.push_back(std::move(divider));
  *y += Scaled(2);
}

void PhiSettingsWindow::AddSubHint(int* y, const wchar_t* text) {
  Item hint;
  hint.kind = ItemKind::kHint;
  hint.text = text;
  hint.bounds = {Scaled(kMargin + kCardPadding), *y - Scaled(8),
                 Scaled(kWidth - kMargin - kCardPadding),
                 *y - Scaled(8) + Scaled(kHintHeight)};
  items_.push_back(std::move(hint));
  *y += Scaled(kHintHeight);
}

void PhiSettingsWindow::AddToggleRow(int* y, const wchar_t* title, bool on,
                                     bool enabled,
                                     const std::string& action) {
  const RECT area = AddRow(y, title, kToggleHeight + 4, kToggleWidth);
  Item toggle;
  toggle.kind = ItemKind::kToggle;
  toggle.on = on;
  toggle.enabled = enabled;
  toggle.action = action;
  toggle.bounds = area;
  items_.push_back(std::move(toggle));
}

void PhiSettingsWindow::BuildSpaces(int* y) {
  const std::vector<std::string> spaces = host_->SpaceSummaries();
  // Resolve (and default) the selection.
  bool found = false;
  for (const std::string& summary : spaces) {
    const std::vector<std::string> parts = SplitTabs(summary);
    if (parts.size() > 3 && parts[3] == selected_space_) {
      found = true;
      break;
    }
  }
  if (!found) {
    selected_space_.clear();
    if (!spaces.empty()) {
      const std::vector<std::string> parts = SplitTabs(spaces.front());
      if (parts.size() > 3) {
        selected_space_ = parts[3];
      }
    }
  }
  std::string selected_theme;
  std::wstring selected_name;
  int selected_saturation = -1;
  for (const std::string& summary : spaces) {
    const std::vector<std::string> parts = SplitTabs(summary);
    if (parts.size() > 3 && parts[3] == selected_space_) {
      selected_name = base::UTF8ToWide(parts[0]);
      selected_theme = parts.size() > 4 ? parts[4] : std::string();
      if (parts.size() > 5) {
        int parsed = -1;
        base::StringToInt(parts[5], &parsed);
        selected_saturation = parsed;
      }
      break;
    }
  }

  // The master-detail band: list 280 wide, detail beside, fixed height.
  const int band_top = *y;
  const int band_bottom = band_top + Scaled(kSpacesBlockHeight);
  const int list_left = Scaled(kMargin);
  const int list_right = list_left + Scaled(kSpacesListWidth);
  {
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {list_left, band_top, list_right, band_bottom};
    items_.push_back(std::move(card));
  }
  {
    Item header;
    header.kind = ItemKind::kSectionTitle;
    header.text = L"Your Spaces";
    header.bounds = {list_left + Scaled(kCardPadding), band_top + Scaled(10),
                     list_right - Scaled(kCardPadding),
                     band_top + Scaled(10) + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(header));
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {list_left, band_top + Scaled(kListHeaderHeight),
                      list_right, band_top + Scaled(kListHeaderHeight) + 1};
    items_.push_back(std::move(divider));
  }
  int row_y = band_top + Scaled(kListHeaderHeight) + Scaled(6);
  const int rows_bottom = band_bottom - Scaled(kToolStripHeight) - Scaled(6);
  for (const std::string& summary : spaces) {
    if (row_y + Scaled(kListRowHeight) > rows_bottom) {
      break;  // The Mac scrolls the inner list; ours clips for now.
    }
    const std::vector<std::string> parts = SplitTabs(summary);
    const std::string id = parts.size() > 3 ? parts[3] : std::string();
    Item row;
    row.kind = ItemKind::kListRow;
    row.text = base::UTF8ToWide(parts[0]);
    if (parts.size() > 1 && !parts[1].empty()) {
      const std::wstring glyph = base::UTF8ToWide(parts[1]);
      if (!PhiIsPrivateGlyph(glyph)) {
        row.text = glyph + L"  " + row.text;
      }
    }
    row.detail = base::UTF8ToWide(parts.size() > 2 ? parts[2] : "");
    row.selected = id == selected_space_;
    row.action = "selectspace";
    row.payload = id;
    row.bounds = {list_left + Scaled(6), row_y, list_right - Scaled(6),
                  row_y + Scaled(kListRowHeight)};
    items_.push_back(std::move(row));
    row_y += Scaled(kListRowHeight);
  }
  {
    // The 34pt tool strip: new / delete / rename (the latter two wait for
    // their surfaces, exactly like the sidebar menu's entries).
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {list_left, band_bottom - Scaled(kToolStripHeight),
                      list_right, band_bottom - Scaled(kToolStripHeight) + 1};
    items_.push_back(std::move(divider));
    const struct {
      const wchar_t* glyph;
      const char* action;
      bool enabled;
    } kTools[] = {
        {L"+", "newspace", true},
        // The default Space is permanent, as on Mac.
        {L"−", "delspace", !selected_space_.empty() && selected_space_ != "1"},
        {L"✎", "renspace", !selected_space_.empty()},
    };
    int x = list_left;
    for (const auto& tool : kTools) {
      Item button;
      button.kind = ItemKind::kToolButton;
      button.text = tool.glyph;
      button.enabled = tool.enabled;
      button.action = tool.action;
      button.bounds = {x, band_bottom - Scaled(kToolStripHeight),
                       x + Scaled(kToolStripHeight), band_bottom};
      items_.push_back(std::move(button));
      x += Scaled(kToolStripHeight);
    }
  }

  // Detail column: icon picker card, then colour + saturation card.
  const int detail_left = list_right + Scaled(kMasterDetailGap);
  const int detail_right = Scaled(kWidth - kMargin);
  if (!selected_space_.empty()) {
    // Card 1: the icon picker (emoji grid from the shared catalog).
    const int picker_bottom = band_top + Scaled(236);
    {
      Item card;
      card.kind = ItemKind::kCard;
      card.bounds = {detail_left, band_top, detail_right, picker_bottom};
      items_.push_back(std::move(card));
      Item header;
      header.kind = ItemKind::kSectionTitle;
      header.text = selected_name.empty() ? L"Icon"
                                          : (L"Icon — " + selected_name);
      header.bounds = {detail_left + Scaled(kCardPadding),
                       band_top + Scaled(10),
                       detail_right - Scaled(kCardPadding),
                       band_top + Scaled(10) + Scaled(kSectionTitleHeight)};
      items_.push_back(std::move(header));
    }
    // The Mac picker's two segments: the Phi icon catalog, then the emoji
    // library. The Mac scrolls in place; this pages the same content.
    {
      const struct {
        const wchar_t* label;
        const char* tab;
      } kTabs[] = {{L"Icon", "icon"}, {L"Emoji", "emoji"}};
      int seg_x = detail_left + Scaled(kCardPadding) + Scaled(80);
      for (const auto& tab : kTabs) {
        Item segment;
        segment.kind = ItemKind::kButton;
        segment.text = tab.label;
        segment.selected = picker_tab_ == tab.tab;
        segment.action = "pickertab";
        segment.payload = tab.tab;
        segment.bounds = {seg_x, band_top + Scaled(6), seg_x + Scaled(56),
                          band_top + Scaled(26)};
        items_.push_back(std::move(segment));
        seg_x += Scaled(60);
      }
      Item back;
      back.kind = ItemKind::kToolButton;
      back.text = L"\u2039";
      back.action = "pickerpage";
      back.value = -1;
      back.bounds = {detail_right - Scaled(56), band_top + Scaled(4),
                     detail_right - Scaled(32), band_top + Scaled(28)};
      items_.push_back(std::move(back));
      Item forward;
      forward.kind = ItemKind::kToolButton;
      forward.text = L"\u203A";
      forward.action = "pickerpage";
      forward.value = 1;
      forward.bounds = {detail_right - Scaled(32), band_top + Scaled(4),
                        detail_right - Scaled(8), band_top + Scaled(28)};
      items_.push_back(std::move(forward));
    }
    // One flat cell list: the catalog, or every emoji group in Mac order.
    std::vector<std::pair<std::wstring, std::string>> cells;
    if (picker_tab_ == "icon") {
      for (const PhiCatalogIcon& icon : PhiIconCatalogAll()) {
        cells.emplace_back(std::wstring(1, icon.glyph),
                           std::string("phi:") + icon.id);
      }
    } else {
      for (const PhiEmojiGroup& group : PhiEmojiGroups()) {
        for (const PhiEmoji& emoji : group.items) {
          cells.emplace_back(base::UTF8ToWide(emoji.text),
                             "emoji:" + emoji.id);
        }
      }
    }
    const int grid_left = detail_left + Scaled(10);
    const int grid_top = band_top + Scaled(34);
    const int per_row = std::max(
        1, (detail_right - Scaled(10) - grid_left + Scaled(kEmojiGap)) /
               Scaled(kEmojiCell + kEmojiGap));
    const int rows_fit = std::max(
        1, (picker_bottom - Scaled(8) - grid_top) /
               Scaled(kEmojiCell + kEmojiGap));
    const int per_page = per_row * rows_fit;
    const int pages =
        std::max(1, (static_cast<int>(cells.size()) + per_page - 1) /
                        per_page);
    picker_page_ = std::max(0, std::min(picker_page_, pages - 1));
    const size_t first = static_cast<size_t>(picker_page_) *
                         static_cast<size_t>(per_page);
    for (size_t i = first;
         i < cells.size() && i < first + static_cast<size_t>(per_page);
         ++i) {
      const int index = static_cast<int>(i - first);
      const int col = index % per_row;
      const int line = index / per_row;
      Item cell;
      cell.kind = ItemKind::kEmojiCell;
      cell.text = cells[i].first;
      cell.action = "spaceicon";
      cell.payload = selected_space_ + "\t" + cells[i].second;
      const int x = grid_left + col * Scaled(kEmojiCell + kEmojiGap);
      const int top = grid_top + line * Scaled(kEmojiCell + kEmojiGap);
      cell.bounds = {x, top, x + Scaled(kEmojiCell),
                     top + Scaled(kEmojiCell)};
      items_.push_back(std::move(cell));
    }

    // Card 2: Color (26pt centres, spacing 3) + Saturation (220pt slider),
    // applied to THIS Space.
    const int theme_top = picker_bottom + Scaled(12);
    {
      Item card;
      card.kind = ItemKind::kCard;
      card.bounds = {detail_left, theme_top, detail_right,
                     theme_top + Scaled(150)};
      items_.push_back(std::move(card));
    }
    {
      Item label;
      label.kind = ItemKind::kRowLabel;
      label.text = L"Color";
      label.bounds = {detail_left + Scaled(kCardPadding),
                      theme_top + Scaled(kRowPadding),
                      detail_left + Scaled(70),
                      theme_top + Scaled(kRowPadding) + Scaled(kSwatchBlock)};
      items_.push_back(std::move(label));
      const auto& themes = PhiBuiltInThemes();
      int x = detail_left + Scaled(64);
      // Leading "follow the app theme" dot, then the eight built-ins.
      Item follow;
      follow.kind = ItemKind::kSwatch;
      follow.text = L"App";
      follow.selected = selected_theme.empty();
      follow.swatch = host_->Palette().accent;
      follow.action = "spacetheme";
      follow.payload = selected_space_;
      follow.bounds = {x, theme_top + Scaled(kRowPadding),
                       x + Scaled(kSwatchItemSpaces),
                       theme_top + Scaled(kRowPadding) + Scaled(kSwatchBlock)};
      items_.push_back(std::move(follow));
      x += Scaled(kSwatchItemSpaces + kSwatchGapSpaces);
      for (size_t i = 0; i < themes.size(); ++i) {
        Item swatch;
        swatch.kind = ItemKind::kSwatch;
        swatch.text = base::UTF8ToWide(themes[i].name);
        swatch.selected = selected_theme == themes[i].id;
        const PhiPalette resolved = PhiResolvePalette(
            themes[i], host_->Appearance(), host_->Saturation());
        swatch.swatch = i == 0 ? RGB(0xFF, 0xFF, 0xFF) : resolved.accent;
        swatch.action = "spacetheme";
        swatch.payload = selected_space_ + "\t" + themes[i].id;
        swatch.bounds = {x, theme_top + Scaled(kRowPadding),
                         x + Scaled(kSwatchItemSpaces),
                         theme_top + Scaled(kRowPadding) +
                             Scaled(kSwatchBlock)};
        items_.push_back(std::move(swatch));
        x += Scaled(kSwatchItemSpaces + kSwatchGapSpaces);
      }
    }
    {
      const int slider_top = theme_top + Scaled(kRowPadding) +
                             Scaled(kSwatchBlock) + Scaled(14);
      Item label;
      label.kind = ItemKind::kRowLabel;
      label.text = L"Saturation";
      label.bounds = {detail_left + Scaled(kCardPadding), slider_top,
                      detail_left + Scaled(90),
                      slider_top + Scaled(kSliderHeight)};
      items_.push_back(std::move(label));
      Item slider;
      slider.kind = ItemKind::kSliderTrack;
      slider.value = selected_saturation < 0 ? host_->Saturation()
                                             : selected_saturation;
      slider.action = "spacesat";
      slider.payload = selected_space_;
      slider.swatch = host_->Palette().ground_bottom;
      slider.bounds = {detail_right - Scaled(kCardPadding) -
                           Scaled(kSliderSpaces),
                       slider_top,
                       detail_right - Scaled(kCardPadding),
                       slider_top + Scaled(kSliderHeight)};
      items_.push_back(std::move(slider));
    }
  } else {
    Item empty;
    empty.kind = ItemKind::kHint;
    empty.text = L"Select a Space to view its settings.";
    empty.bounds = {detail_left, band_top + Scaled(200), detail_right,
                    band_top + Scaled(200) + Scaled(kHintHeight)};
    items_.push_back(std::move(empty));
  }
  *y = band_bottom + Scaled(kMasterDetailGap);

  // Pane-wide card: pinned tab scope, with the live description.
  BeginSection(y, nullptr);
  {
    const RECT area = AddRow(y, L"Pinned tab scope", kPillHeight, kPillWidth);
    const wchar_t* description = L"";
    switch (host_->PinScope()) {
      case PhiPinScope::kSpace:
        description = L"Each Space has its own pinned tabs.";
        break;
      case PhiPinScope::kProfile:
        description = L"Spaces using the same profile share pinned tabs.";
        break;
      case PhiPinScope::kApp:
        description = L"Pinned tabs are shared across all profiles and "
                      L"Spaces.";
        break;
    }
    AddSubHint(y, description);
    Item pill;
    pill.kind = ItemKind::kPopup;
    pill.text = host_->PinScope() == PhiPinScope::kSpace     ? L"Space"
                : host_->PinScope() == PhiPinScope::kProfile ? L"Profile"
                                                             : L"App";
    pill.action = "pinscope";
    pill.bounds = area;
    items_.push_back(std::move(pill));
  }
  EndSection(y);

  // Pane-wide card: URL rules (its editor does not exist yet).
  BeginSection(y, nullptr);
  {
    const RECT area = AddRow(y, L"URL Rules…", kRowLabelHeight, 20);
    Item chevron;
    chevron.kind = ItemKind::kLink;
    chevron.text = L"›";
    chevron.enabled = false;
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
  }
  EndSection(y);
}

void PhiSettingsWindow::BuildProfiles(int* y) {
  const std::vector<std::string> profiles = host_->ProfileSummaries();
  const std::vector<std::string> spaces = host_->SpaceSummaries();
  bool found = false;
  for (const std::string& summary : profiles) {
    const std::vector<std::string> parts = SplitTabs(summary);
    if (!parts.empty() && parts[0] == selected_profile_) {
      found = true;
      break;
    }
  }
  if (!found) {
    selected_profile_.clear();
    if (!profiles.empty()) {
      selected_profile_ = SplitTabs(profiles.front())[0];
    }
  }

  const int band_top = *y;
  const int band_bottom = band_top + Scaled(kSpacesBlockHeight);
  const int list_left = Scaled(kMargin);
  const int list_right = list_left + Scaled(kProfilesListWidth);
  {
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {list_left, band_top, list_right, band_bottom};
    items_.push_back(std::move(card));
    Item header;
    header.kind = ItemKind::kSectionTitle;
    header.text = L"Your Profiles";
    header.bounds = {list_left + Scaled(kCardPadding), band_top + Scaled(10),
                     list_right - Scaled(kCardPadding),
                     band_top + Scaled(10) + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(header));
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {list_left, band_top + Scaled(kListHeaderHeight),
                      list_right, band_top + Scaled(kListHeaderHeight) + 1};
    items_.push_back(std::move(divider));
  }
  int row_y = band_top + Scaled(kListHeaderHeight) + Scaled(6);
  const int rows_bottom = band_bottom - Scaled(kToolStripHeight) - Scaled(6);
  for (const std::string& summary : profiles) {
    if (row_y + Scaled(kListRowHeight) > rows_bottom) {
      break;
    }
    const std::vector<std::string> parts = SplitTabs(summary);
    const std::string& id = parts[0];
    int bound = 0;
    for (const std::string& space : spaces) {
      const std::vector<std::string> space_parts = SplitTabs(space);
      if (space_parts.size() > 2 && space_parts[2] == id) {
        ++bound;
      }
    }
    Item row;
    row.kind = ItemKind::kListRow;
    row.text = base::UTF8ToWide(parts.size() > 1 ? parts[1] : id);
    row.detail =
        bound == 0
            ? L"Not used"
            : base::UTF8ToWide(base::NumberToString(bound) + " Spaces");
    row.selected = id == selected_profile_;
    row.action = "selectprofile";
    row.payload = id;
    row.bounds = {list_left + Scaled(6), row_y, list_right - Scaled(6),
                  row_y + Scaled(kListRowHeight)};
    items_.push_back(std::move(row));
    row_y += Scaled(kListRowHeight);
  }
  {
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {list_left, band_bottom - Scaled(kToolStripHeight),
                      list_right, band_bottom - Scaled(kToolStripHeight) + 1};
    items_.push_back(std::move(divider));
    const struct {
      const wchar_t* glyph;
      const char* action;
      bool enabled;
    } kTools[] = {
        {L"+", "addprofile", true},
        // Default is permanent; a profile still bound to a Space stays.
        {L"−", "delprofile", [&] {
           if (selected_profile_.empty() || selected_profile_ == "Default") {
             return false;
           }
           for (const std::string& space : spaces) {
             const std::vector<std::string> parts = SplitTabs(space);
             if (parts.size() > 2 && parts[2] == selected_profile_) {
               return false;
             }
           }
           return true;
         }()},
        {L"✎", "renprofile", !selected_profile_.empty()},
    };
    int x = list_left;
    for (const auto& tool : kTools) {
      Item button;
      button.kind = ItemKind::kToolButton;
      button.text = tool.glyph;
      button.enabled = tool.enabled;
      button.action = tool.action;
      button.bounds = {x, band_bottom - Scaled(kToolStripHeight),
                       x + Scaled(kToolStripHeight), band_bottom};
      items_.push_back(std::move(button));
      x += Scaled(kToolStripHeight);
    }
  }

  // Detail column: the profile's two cards, controls waiting on their
  // backing (search engines and download folders are not plumbed yet).
  const int detail_left = list_right + Scaled(kMasterDetailGap);
  const int detail_right = Scaled(kWidth - kMargin);
  if (!selected_profile_.empty()) {
    int detail_y = band_top;
    {
      Item card;
      card.kind = ItemKind::kCard;
      card.bounds = {detail_left, detail_y, detail_right,
                     detail_y + Scaled(96)};
      items_.push_back(std::move(card));
      Item label;
      label.kind = ItemKind::kRowLabel;
      label.text = L"Search Engine";
      label.bounds = {detail_left + Scaled(kCardPadding),
                      detail_y + Scaled(kRowPadding),
                      detail_left + Scaled(150),
                      detail_y + Scaled(kRowPadding) + Scaled(kPillHeight)};
      items_.push_back(std::move(label));
      Item pill;
      pill.kind = ItemKind::kPopup;
      pill.text = L"Unavailable";
      pill.enabled = false;
      pill.bounds = {detail_right - Scaled(kCardPadding) - Scaled(kPillWidth),
                     detail_y + Scaled(kRowPadding),
                     detail_right - Scaled(kCardPadding),
                     detail_y + Scaled(kRowPadding) + Scaled(kPillHeight)};
      items_.push_back(std::move(pill));
      Item divider;
      divider.kind = ItemKind::kDivider;
      divider.bounds = {detail_left + Scaled(kCardPadding),
                        detail_y + Scaled(48), detail_right -
                        Scaled(kCardPadding), detail_y + Scaled(48) + 1};
      items_.push_back(std::move(divider));
      Item label2;
      label2.kind = ItemKind::kRowLabel;
      label2.text = L"Download Location";
      label2.bounds = {detail_left + Scaled(kCardPadding),
                       detail_y + Scaled(58), detail_left + Scaled(160),
                       detail_y + Scaled(58) + Scaled(kPillHeight)};
      items_.push_back(std::move(label2));
      Item pill2;
      pill2.kind = ItemKind::kPopup;
      pill2.text = L"Choose…";
      pill2.enabled = false;
      pill2.bounds = {detail_right - Scaled(kCardPadding) -
                          Scaled(kPillWidth),
                      detail_y + Scaled(58),
                      detail_right - Scaled(kCardPadding),
                      detail_y + Scaled(58) + Scaled(kPillHeight)};
      items_.push_back(std::move(pill2));
      detail_y += Scaled(96) + Scaled(kMasterDetailGap);
    }
    {
      Item heading;
      heading.kind = ItemKind::kSectionTitle;
      heading.text = L"Your Data and Settings";
      heading.bounds = {detail_left + Scaled(2), detail_y,
                        detail_right,
                        detail_y + Scaled(kSectionTitleHeight)};
      items_.push_back(std::move(heading));
      detail_y += Scaled(kSectionTitleHeight) + Scaled(8);
      Item card;
      card.kind = ItemKind::kCard;
      card.bounds = {detail_left, detail_y, detail_right,
                     detail_y + Scaled(120)};
      items_.push_back(std::move(card));
      const wchar_t* kRows[] = {L"Privacy and Security", L"Notifications",
                                L"Clear Browsing Data"};
      int i = 0;
      for (const wchar_t* row_title : kRows) {
        Item row;
        row.kind = ItemKind::kRowLabel;
        row.text = row_title;
        row.enabled = false;
        row.bounds = {detail_left + Scaled(kCardPadding),
                      detail_y + Scaled(8 + 38 * i),
                      detail_right - Scaled(kCardPadding + 24),
                      detail_y + Scaled(8 + 38 * i) + Scaled(24)};
        items_.push_back(std::move(row));
        Item arrow;
        arrow.kind = ItemKind::kLink;
        arrow.text = L"→";
        arrow.enabled = false;
        arrow.bounds = {detail_right - Scaled(kCardPadding + 20),
                        detail_y + Scaled(8 + 38 * i),
                        detail_right - Scaled(kCardPadding),
                        detail_y + Scaled(8 + 38 * i) + Scaled(24)};
        items_.push_back(std::move(arrow));
        if (i < 2) {
          Item divider;
          divider.kind = ItemKind::kDivider;
          divider.bounds = {detail_left + Scaled(kCardPadding),
                            detail_y + Scaled(38 * (i + 1)),
                            detail_right - Scaled(kCardPadding),
                            detail_y + Scaled(38 * (i + 1)) + 1};
          items_.push_back(std::move(divider));
        }
        ++i;
      }
    }
  } else {
    Item empty;
    empty.kind = ItemKind::kHint;
    empty.text = L"Select a profile to view its settings.";
    empty.bounds = {detail_left, band_top + Scaled(200), detail_right,
                    band_top + Scaled(200) + Scaled(kHintHeight)};
    items_.push_back(std::move(empty));
  }
  *y = band_bottom + Scaled(kSectionGap);
}

void PhiSettingsWindow::BuildAccount(int* y) {
  // The structural twin of the Mac pane; sign-in does not exist on this
  // build, so everything is drawn and disabled.
  BeginSection(y, nullptr);
  {
    const RECT area = AddRow(y, L"You", 40, 90);
    AddSubHint(y, L"Sign-in is not available on this build.");
    Item button;
    button.kind = ItemKind::kButton;
    button.text = L"Logout";
    button.enabled = false;
    button.bounds = area;
    items_.push_back(std::move(button));
  }
  EndSection(y);
  BeginSection(y, nullptr);
  {
    const RECT area = AddRow(y, L"Phi is not your default browser", 24, 110);
    Item button;
    button.kind = ItemKind::kButton;
    button.text = L"Set as default";
    button.enabled = false;
    button.bounds = area;
    items_.push_back(std::move(button));
  }
  EndSection(y);
}

void PhiSettingsWindow::BuildAi(int* y) {
  AddToggleRow(y, L"Enable AI features in Phi Browser", /*on=*/true,
               /*enabled=*/false, std::string());
  *y += Scaled(8);
  BeginSection(y, L"Browser memory");
  {
    const RECT area = AddRow(y, L"View and manage your browser memory",
                             kRowLabelHeight, 20);
    Item chevron;
    chevron.kind = ItemKind::kLink;
    chevron.text = L"›";
    chevron.enabled = false;
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
  }
  EndSection(y);
  BeginSection(y, L"Phi Sentinel");
  AddToggleRow(y, L"Launch Phi Sentinel automatically at login", true, false,
               std::string());
  AddDividerRow(y);
  {
    const RECT area = AddRow(y, L"Private AI", kRowLabelHeight, 20);
    Item chevron;
    chevron.kind = ItemKind::kLink;
    chevron.text = L"›";
    chevron.enabled = false;
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
  }
  EndSection(y);
  BeginSection(y, L"New Tab Page");
  AddToggleRow(y, L"Show proactive suggestions on the new tab page", true,
               false, std::string());
  EndSection(y);
  BeginSection(y, L"AI Sidebar");
  AddToggleRow(y, L"Automatically add current tab as context to new "
                  L"conversation",
               true, false, std::string());
  EndSection(y);
  BeginSection(y, L"External Data Connectors");
  AddToggleRow(y, L"Enable External Data Connectors", true, false,
               std::string());
  AddDividerRow(y);
  AddSubHint(y, L"External Data Connectors help to provide additional "
                L"context for better AI experience");
  *y += Scaled(kRowPadding);
  EndSection(y);
}

void PhiSettingsWindow::BuildPhiLink(int* y) {
  BeginSection(y, L"Telegram");
  {
    Item row;
    row.kind = ItemKind::kDisclosure;
    row.text = L"Official Phi Link Telegram bot";
    row.detail = L"We provide a Telegram bot to relay your messages "
                 L"@philink_bot";
    row.enabled = false;
    row.bounds = {Scaled(kMargin + kCardPadding), *y,
                  Scaled(kWidth - kMargin - kCardPadding - 90),
                  *y + Scaled(48)};
    items_.push_back(std::move(row));
    Item badge;
    badge.kind = ItemKind::kBadge;
    badge.text = L"Not linked";
    badge.bounds = {Scaled(kWidth - kMargin - kCardPadding - 80),
                    *y + Scaled(16),
                    Scaled(kWidth - kMargin - kCardPadding),
                    *y + Scaled(34)};
    items_.push_back(std::move(badge));
    *y += Scaled(52);
  }
  AddDividerRow(y);
  {
    Item row;
    row.kind = ItemKind::kDisclosure;
    row.text = L"Custom Telegram bot";
    row.detail = L"Setting up your own Telegram bot using Telegram's "
                 L"@BotFather";
    row.enabled = false;
    row.bounds = {Scaled(kMargin + kCardPadding), *y,
                  Scaled(kWidth - kMargin - kCardPadding - 110),
                  *y + Scaled(48)};
    items_.push_back(std::move(row));
    Item badge;
    badge.kind = ItemKind::kBadge;
    badge.text = L"Not configured";
    badge.bounds = {Scaled(kWidth - kMargin - kCardPadding - 104),
                    *y + Scaled(16),
                    Scaled(kWidth - kMargin - kCardPadding),
                    *y + Scaled(34)};
    items_.push_back(std::move(badge));
    *y += Scaled(52);
  }
  EndSection(y);
}

void PhiSettingsWindow::BuildShortcuts(int* y) {
  BeginSection(y, L"Shortcuts");
  // The shortcut table, grouped as on Mac (App, File, Edit, View, History,
  // Window, Tab, Spaces), showing the Windows bindings the menu bar
  // displays. Recording/customising waits for its surface.
  const struct {
    const wchar_t* group;
    const wchar_t* name;
    const wchar_t* keys;
  } kRows[] = {
      {L"App", L"Preferences", L"Ctrl+,"},
      {L"App", L"Quit Phi", L"Ctrl+Shift+Q"},
      {L"File", L"New Tab", L"Ctrl+T"},
      {L"File", L"New Window", L"Ctrl+N"},
      {L"File", L"Open Location", L"Ctrl+L"},
      {L"File", L"Close Tab", L"Ctrl+W"},
      {L"File", L"Close Window", L"Ctrl+Shift+W"},
      {L"Edit", L"Copy URL", L"Ctrl+Shift+C"},
      {L"View", L"Reload This Page", L"Ctrl+R"},
      {L"View", L"Force Reload This Page", L"Ctrl+Shift+R"},
      {L"View", L"Toggle Sidebar", L"Ctrl+S"},
      {L"History", L"Back", L"Alt+Left"},
      {L"History", L"Forward", L"Alt+Right"},
      {L"Window", L"Downloads", L"Ctrl+J"},
      {L"Tab", L"Select Next Tab", L"Ctrl+Tab"},
      {L"Tab", L"Select Previous Tab", L"Ctrl+Shift+Tab"},
      {L"Spaces", L"Next Space", L"Ctrl+Alt+Right"},
      {L"Spaces", L"Previous Space", L"Ctrl+Alt+Left"},
  };
  std::wstring group;
  for (const auto& row : kRows) {
    if (group != row.group) {
      group = row.group;
      Item heading;
      heading.kind = ItemKind::kSectionTitle;
      heading.text = row.group;
      heading.bounds = {Scaled(kMargin + kCardPadding), *y,
                        Scaled(kWidth - kMargin - kCardPadding),
                        *y + Scaled(kSectionTitleHeight)};
      items_.push_back(std::move(heading));
      *y += Scaled(kSectionTitleHeight) + Scaled(6);
    }
    const RECT area = AddRow(y, row.name, 20, 110);
    Item keys;
    keys.kind = ItemKind::kBadge;
    keys.text = row.keys;
    keys.enabled = false;
    keys.bounds = area;
    items_.push_back(std::move(keys));
  }
  EndSection(y);
}

void PhiSettingsWindow::BuildDeveloper(int* y) {
  BeginSection(y, L"Agent control");
  AddToggleRow(y, L"Allow agents to control Phi (CDP)", /*on=*/true,
               /*enabled=*/false, std::string());
  AddDividerRow(y);
  AddSubHint(y, L"Agent tools drive Phi over the DevTools Protocol through "
                L"a private socket only this machine's processes can reach.");
  *y += Scaled(kRowPadding);
  EndSection(y);
  BeginSection(y, L"Agent Password Manager");
  AddToggleRow(y, L"Bitwarden password manager", /*on=*/false,
               /*enabled=*/false, std::string());
  AddDividerRow(y);
  AddSubHint(y, L"Logins for Phi and agent requests, held by a separate "
                L"helper — passwords never touch the browser process.");
  *y += Scaled(kRowPadding);
  EndSection(y);
}

void PhiSettingsWindow::Rebuild() {
  items_.clear();
  open_card_ = 0;
  if (!host_) {
    return;
  }
  // The pane toolbar, centred across the top; Developer only in dev mode.
  const bool developer = host_->DeveloperMode();
  const int shown = static_cast<int>(kSections.size()) - (developer ? 0 : 1);
  int x = Scaled((kWidth - shown * kToolbarItemWidth) / 2);
  for (const SectionDef& section : kSections) {
    if (!developer && std::string(section.id) == "developer") {
      continue;
    }
    Item entry;
    entry.kind = ItemKind::kToolbarItem;
    entry.text = section.label;
    entry.payload = section.id;
    entry.selected = section_ == section.id;
    entry.action = "section";
    entry.bounds = {x, Scaled(8), x + Scaled(kToolbarItemWidth),
                    Scaled(kToolbarHeight - 8)};
    items_.push_back(std::move(entry));
    x += Scaled(kToolbarItemWidth);
  }
  if (!developer && section_ == "developer") {
    section_ = "general";  // The pane vanished under us, as on Mac.
    SyncTitle();
  }

  int y = Scaled(kToolbarHeight + kMargin) - scroll_;
  const int top = y;
  if (section_ == "general") {
    BuildGeneral(&y);
  } else if (section_ == "spaces") {
    BuildSpaces(&y);
  } else if (section_ == "profiles") {
    BuildProfiles(&y);
  } else if (section_ == "shortcuts") {
    BuildShortcuts(&y);
  } else if (section_ == "ai") {
    BuildAi(&y);
  } else if (section_ == "account") {
    BuildAccount(&y);
  } else if (section_ == "phiLink") {
    BuildPhiLink(&y);
  } else {
    BuildDeveloper(&y);
  }
  content_height_ = y - top + Scaled(kMargin);
}

void PhiSettingsWindow::Paint(HDC dc) {
  if (!host_) {
    return;
  }
  // The Mac's fixedWindowBackground: flat, theme-INdependent — #EAEAEA
  // light / #252525 dark. Only the appearance choice matters here.
  const bool dark = host_->Palette().dark;
  const COLORREF bg = dark ? RGB(0x25, 0x25, 0x25) : RGB(0xEA, 0xEA, 0xEA);
  const COLORREF ink = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
  const COLORREF label = dark ? Blend(RGB(0xFF, 0xFF, 0xFF), bg, 204)
                              : RGB(0x26, 0x26, 0x26);
  const COLORREF secondary = dark ? RGB(0xB3, 0xB3, 0xB3) : RGB(0x66, 0x66, 0x66);
  const COLORREF tertiary = Blend(ink, bg, 77);
  const COLORREF card_bg = Blend(ink, bg, 5);        // settingItemBackground 2%
  const COLORREF card_border = Blend(ink, bg, 20);   // border 8%
  const COLORREF separator = Blend(ink, bg, 15);
  const COLORREF accent = host_->Palette().accent;

  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  HBRUSH page = ::CreateSolidBrush(bg);
  ::FillRect(dc, &client, page);
  ::DeleteObject(page);
  // The toolbar band shares the ground; a separator closes it.
  RECT bar_sep = {0, Scaled(kToolbarHeight), client.right,
                  Scaled(kToolbarHeight) + 1};
  HBRUSH sep_brush = ::CreateSolidBrush(separator);
  ::FillRect(dc, &bar_sep, sep_brush);
  ::DeleteObject(sep_brush);

  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = font_ ? ::SelectObject(dc, font_) : nullptr;
  bool clipped = false;

  for (const Item& item : items_) {
    if (item.kind != ItemKind::kToolbarItem && !clipped) {
      // Pane content scrolls; nothing of it may paint over the toolbar.
      ::IntersectClipRect(dc, 0, Scaled(kToolbarHeight) + 1, client.right,
                          client.bottom);
      clipped = true;
    }
    RECT bounds = item.bounds;
    const COLORREF row_ink = item.enabled ? label : Blend(label, bg, 120);
    switch (item.kind) {
      case ItemKind::kToolbarItem: {
        if (item.selected) {
          FillRounded(dc, bounds, Blend(ink, bg, 20), Scaled(kRadius));
        }
        ::SetTextColor(dc, item.selected ? accent : secondary);
        RECT glyph = bounds;
        glyph.bottom = glyph.top + Scaled(kToolbarIcon + 8);
        const wchar_t* mark = L"○";
        for (const SectionDef& section : kSections) {
          if (item.payload == section.id) {
            mark = section.glyph;
            break;
          }
        }
        ::DrawTextW(dc, mark, -1, &glyph,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT caption = bounds;
        caption.top = glyph.bottom;
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        ::DrawTextW(dc, item.text.c_str(), -1, &caption,
                    DT_CENTER | DT_TOP | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kCard:
        StrokeRounded(dc, bounds, card_bg, card_border, Scaled(kCardRadius),
                      1);
        break;
      case ItemKind::kSectionTitle:
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        ::SetTextColor(dc, secondary);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kSubtitle:
      case ItemKind::kHint:
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        ::SetTextColor(dc, tertiary);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kRowLabel:
        ::SetTextColor(dc, row_ink);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_VCENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
        break;
      case ItemKind::kLink:
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        ::SetTextColor(dc, item.enabled ? accent : tertiary);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kDivider: {
        HBRUSH line = ::CreateSolidBrush(separator);
        ::FillRect(dc, &bounds, line);
        ::DeleteObject(line);
        break;
      }
      case ItemKind::kSwatch: {
        // A 22pt dot centred in its item width; 2pt ring when selected;
        // the caption (11pt) shows only for the selected swatch.
        const int cx = (bounds.left + bounds.right) / 2;
        const int ring = Scaled(kSwatchRing);
        RECT ring_rc = {cx - ring / 2, bounds.top, cx + ring / 2,
                        bounds.top + ring};
        if (item.selected) {
          Ellipse(dc, ring_rc, card_bg, item.enabled ? accent : tertiary,
                  Scaled(2));
        }
        const int dot = Scaled(kSwatchDot);
        RECT dot_rc = {cx - dot / 2, bounds.top + (ring - dot) / 2,
                       cx + dot / 2, bounds.top + (ring + dot) / 2};
        COLORREF fill = item.swatch;
        if (!item.enabled) {
          fill = Blend(fill, bg, 100);
        }
        // Pure is white and needs the contrast hairline.
        Ellipse(dc, dot_rc, fill, Blend(ink, bg, 31), 1);
        if (item.selected && !item.text.empty()) {
          if (hint_font_) {
            ::SelectObject(dc, hint_font_);
          }
          RECT caption = {bounds.left - Scaled(6), bounds.top + ring +
                          Scaled(6), bounds.right + Scaled(6), bounds.bottom};
          ::SetTextColor(dc, row_ink);
          ::DrawTextW(dc, item.text.c_str(), -1, &caption,
                      DT_CENTER | DT_TOP | DT_SINGLELINE);
          if (font_) {
            ::SelectObject(dc, font_);
          }
        }
        break;
      }
      case ItemKind::kImageCard: {
        // 122x72 sketch, r5, 2pt accent ring when selected, caption below.
        RECT image = {bounds.left, bounds.top, bounds.right,
                      bounds.top + Scaled(kImageCardH)};
        const COLORREF page_bg =
            item.payload == "dark"
                ? RGB(0x25, 0x25, 0x25)
                : (item.payload == "system" ? bg : RGB(0xF7, 0xF7, 0xF7));
        StrokeRounded(dc, image, page_bg,
                      item.selected ? accent : card_border, Scaled(5),
                      item.selected ? Scaled(2) : 1);
        // The sketch: a miniature of what the option produces.
        RECT inner = image;
        ::InflateRect(&inner, -Scaled(8), -Scaled(8));
        const COLORREF mini = Blend(ink, page_bg, 46);
        if (item.payload == "performance" || item.payload == "balanced") {
          RECT side = {inner.left, inner.top, inner.left + Scaled(26),
                       inner.bottom};
          FillRounded(dc, side, mini, Scaled(3));
          RECT content = {side.right + Scaled(4),
                          inner.top + (item.payload == "balanced"
                                           ? Scaled(12)
                                           : 0),
                          inner.right, inner.bottom};
          if (item.payload == "balanced") {
            RECT top_bar = {side.right + Scaled(4), inner.top, inner.right,
                            inner.top + Scaled(8)};
            FillRounded(dc, top_bar, mini, Scaled(2));
          }
          StrokeRounded(dc, content, page_bg, mini, Scaled(3), 1);
        } else if (item.payload == "comfortable") {
          RECT strip = {inner.left, inner.top, inner.right,
                        inner.top + Scaled(8)};
          FillRounded(dc, strip, mini, Scaled(2));
          RECT content = {inner.left, strip.bottom + Scaled(4), inner.right,
                          inner.bottom};
          StrokeRounded(dc, content, page_bg, mini, Scaled(3), 1);
        } else if (item.payload == "system") {
          // Half light, half dark, the ring redrawn on top (border only:
          // a filled RoundRect would cover the halves).
          RECT halves = image;
          ::InflateRect(&halves, -Scaled(2), -Scaled(2));
          RECT left_half = {halves.left, halves.top,
                            (halves.left + halves.right) / 2, halves.bottom};
          RECT right_half = {(halves.left + halves.right) / 2, halves.top,
                             halves.right, halves.bottom};
          HBRUSH light_brush = ::CreateSolidBrush(RGB(0xF7, 0xF7, 0xF7));
          HBRUSH dark_brush = ::CreateSolidBrush(RGB(0x25, 0x25, 0x25));
          ::FillRect(dc, &left_half, light_brush);
          ::FillRect(dc, &right_half, dark_brush);
          ::DeleteObject(light_brush);
          ::DeleteObject(dark_brush);
          HPEN ring_pen = ::CreatePen(
              PS_SOLID, item.selected ? Scaled(2) : 1,
              item.selected ? accent : card_border);
          HGDIOBJ old_ring_pen = ::SelectObject(dc, ring_pen);
          HGDIOBJ old_ring_brush =
              ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
          ::RoundRect(dc, image.left, image.top, image.right, image.bottom,
                      Scaled(5), Scaled(5));
          ::SelectObject(dc, old_ring_brush);
          ::SelectObject(dc, old_ring_pen);
          ::DeleteObject(ring_pen);
        } else if (item.payload == "ntp") {
          RECT box = {inner.left + Scaled(24), inner.top + Scaled(22),
                      inner.right - Scaled(24), inner.top + Scaled(34)};
          FillRounded(dc, box, mini, Scaled(3));
        } else if (item.payload == "omnibox") {
          RECT field = {inner.left, inner.top, inner.right,
                        inner.top + Scaled(10)};
          FillRounded(dc, field, mini, Scaled(3));
        }
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        RECT caption = {bounds.left, image.bottom + Scaled(10) - Scaled(4),
                        bounds.right, bounds.bottom};
        ::SetTextColor(dc, row_ink);
        ::DrawTextW(dc, item.text.c_str(), -1, &caption,
                    DT_CENTER | DT_TOP | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kToggle: {
        const int height = bounds.bottom - bounds.top;
        COLORREF fill = item.on ? accent : Blend(ink, bg, 60);
        if (!item.enabled) {
          fill = Blend(fill, bg, 120);
        }
        FillRounded(dc, bounds, fill, height);
        const int knob = height - Scaled(4);
        const int left = item.on ? bounds.right - knob - Scaled(2)
                                 : bounds.left + Scaled(2);
        RECT dot = {left, bounds.top + Scaled(2), left + knob,
                    bounds.top + Scaled(2) + knob};
        Ellipse(dc, dot, RGB(0xFF, 0xFF, 0xFF), 0, 0);
        break;
      }
      case ItemKind::kSliderTrack: {
        // The Mac bar: 10pt tall, full width, rounded, a saturation
        // GRADIENT of the current theme; an 18pt white knob rides it.
        const int mid = (bounds.top + bounds.bottom) / 2;
        RECT track = {bounds.left, mid - Scaled(5), bounds.right,
                      mid + Scaled(5)};
        const int span = track.right - track.left;
        const int bands = 24;
        for (int i = 0; i < bands; ++i) {
          RECT band = {track.left + span * i / bands, track.top,
                       track.left + span * (i + 1) / bands, track.bottom};
          // From barely tinted to heavily tinted, like the Mac's ramp.
          const COLORREF tint = Blend(item.swatch, RGB(0xFF, 0xFF, 0xFF),
                                      40 + 200 * i / bands);
          HBRUSH brush = ::CreateSolidBrush(item.enabled
                                                ? tint
                                                : Blend(tint, bg, 140));
          ::FillRect(dc, &band, brush);
          ::DeleteObject(brush);
        }
        StrokeRounded(dc, track, 0, card_border, Scaled(5), 1);
        const int knob = Scaled(kSliderKnob);
        const int at = track.left + (span - knob) * item.value / 100;
        RECT dot = {at, mid - knob / 2, at + knob, mid + knob / 2};
        Ellipse(dc, dot, RGB(0xFF, 0xFF, 0xFF), card_border, 1);
        break;
      }
      case ItemKind::kPopup: {
        FillRounded(dc, bounds, Blend(ink, bg, 15), Scaled(kRadius));
        ::SetTextColor(dc, row_ink);
        RECT text = bounds;
        text.left += Scaled(10);
        text.right -= Scaled(18);
        ::DrawTextW(dc, item.text.c_str(), -1, &text,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT arrows = bounds;
        arrows.left = bounds.right - Scaled(16);
        ::SetTextColor(dc, secondary);
        ::DrawTextW(dc, L"↕", -1, &arrows,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
      }
      case ItemKind::kListRow: {
        if (item.selected) {
          // Spaces uses the solid accent; Profiles a 15% wash. One look —
          // the accent — keeps the two lists consistent here.
          FillRounded(dc, bounds, accent, Scaled(kRadius));
        }
        ::SetTextColor(dc, item.selected ? RGB(0xFF, 0xFF, 0xFF) : row_ink);
        RECT text = bounds;
        text.left += Scaled(10);
        text.right -= Scaled(70);
        ::DrawTextW(dc, item.text.c_str(), -1, &text,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!item.detail.empty()) {
          if (hint_font_) {
            ::SelectObject(dc, hint_font_);
          }
          RECT detail = bounds;
          detail.right -= Scaled(8);
          ::SetTextColor(dc, item.selected
                                 ? RGB(0xFF, 0xFF, 0xFF)
                                 : secondary);
          ::DrawTextW(dc, item.detail.c_str(), -1, &detail,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
          if (font_) {
            ::SelectObject(dc, font_);
          }
        }
        break;
      }
      case ItemKind::kToolButton:
        ::SetTextColor(dc, item.enabled ? row_ink : tertiary);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
      case ItemKind::kEmojiCell: {
        if (item.selected) {
          FillRounded(dc, bounds, Blend(ink, bg, 25), Scaled(10));
        }
        if (!PhiIsPrivateGlyph(item.text) &&
            PhiDrawColorText(dc, bounds, item.text, Scaled(14), row_ink)) {
          break;  // Colour emoji via DirectWrite; GDI below is the fallback.
        }
        {
          HFONT cell_font =
              PhiIsPrivateGlyph(item.text) ? mdl2_font_ : emoji_font_;
          if (cell_font) {
            ::SelectObject(dc, cell_font);
          }
        }
        ::SetTextColor(dc, row_ink);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kBadge: {
        FillRounded(dc, bounds, Blend(ink, bg, 25),
                    (bounds.bottom - bounds.top) / 2);
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        ::SetTextColor(dc, secondary);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kButton:
        if (item.selected) {
          FillRounded(dc, bounds, accent, Scaled(kRadius));
        } else {
          StrokeRounded(dc, bounds, Blend(ink, bg, 10), card_border,
                        Scaled(kRadius), 1);
        }
        ::SetTextColor(dc, item.selected ? RGB(0xFF, 0xFF, 0xFF) : row_ink);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
      case ItemKind::kDisclosure: {
        ::SetTextColor(dc, row_ink);
        RECT title = bounds;
        title.bottom = title.top + Scaled(20);
        ::DrawTextW(dc, item.text.c_str(), -1, &title,
                    DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        RECT subtitle = bounds;
        subtitle.top = title.bottom + Scaled(4);
        ::SetTextColor(dc, tertiary);
        ::DrawTextW(dc, item.detail.c_str(), -1, &subtitle,
                    DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
    }
  }

  ::SelectClipRgn(dc, nullptr);
  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

void PhiSettingsWindow::Apply(const Item& item, int x) {
  if (item.action == "section") {
    section_ = item.payload;
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "theme") {
    host_->SetThemeId(item.payload);
  } else if (item.action == "appearance") {
    host_->SetAppearance(static_cast<PhiAppearance>(item.value));
  } else if (item.action == "layout") {
    host_->RequestLayoutMode(static_cast<PhiLayoutMode>(item.value));
  } else if (item.action == "newtab") {
    host_->SetNewTabBehavior(static_cast<PhiNewTabBehavior>(item.value));
  } else if (item.action == "fullurl") {
    host_->SetAlwaysShowFullUrl(!item.on);
  } else if (item.action == "devmode") {
    host_->SetDeveloperMode(!item.on);
  } else if (item.action == "gospaces") {
    section_ = "spaces";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "goprofiles") {
    section_ = "profiles";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "browsersettings") {
    host_->OpenBrowserSettings();
  } else if (item.action == "pinscope") {
    // The Mac's popup: Space / Profile / App.
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    const PhiPinScope current = host_->PinScope();
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiPinScope::kSpace ? MF_CHECKED
                                                              : 0),
                  1, L"Space");
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiPinScope::kProfile ? MF_CHECKED
                                                                : 0),
                  2, L"Profile");
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiPinScope::kApp ? MF_CHECKED : 0),
                  3, L"App");
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd_, &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd_, nullptr));
    ::DestroyMenu(menu);
    if (chosen >= 1 && chosen <= 3) {
      host_->SetPinScope(chosen == 1   ? PhiPinScope::kSpace
                         : chosen == 2 ? PhiPinScope::kProfile
                                       : PhiPinScope::kApp);
    }
  } else if (item.action == "saturation") {
    const int span = item.bounds.right - item.bounds.left;
    if (span > 0) {
      const int value = (x - item.bounds.left) * 100 / span;
      host_->SetSaturation(value < 0 ? 0 : (value > 100 ? 100 : value));
    }
  } else if (item.action == "spacesat") {
    const int span = item.bounds.right - item.bounds.left;
    if (span > 0) {
      const int value = (x - item.bounds.left) * 100 / span;
      host_->SetSpaceSaturation(item.payload,
                                value < 0 ? 0 : (value > 100 ? 100 : value));
    }
  } else if (item.action == "spacetheme") {
    const std::vector<std::string> parts = SplitTabs(item.payload);
    if (!parts.empty()) {
      host_->SetSpaceTheme(parts[0],
                           parts.size() > 1 ? parts[1] : std::string());
    }
  } else if (item.action == "spaceicon") {
    const std::vector<std::string> parts = SplitTabs(item.payload);
    if (parts.size() > 1) {
      host_->SetSpaceIcon(parts[0], parts[1]);
    }
  } else if (item.action == "selectspace") {
    selected_space_ = item.payload;
  } else if (item.action == "selectprofile") {
    selected_profile_ = item.payload;
  } else if (item.action == "pickertab") {
    picker_tab_ = item.payload;
    picker_page_ = 0;
  } else if (item.action == "pickerpage") {
    picker_page_ += item.value;  // Clamped when the grid rebuilds.
    if (picker_page_ < 0) {
      picker_page_ = 0;
    }
  } else if (item.action == "newspace") {
    std::wstring name;
    if (PhiPromptText(hwnd_, L"New Space", L"Enter a name for the new Space.",
                      &name)) {
      host_->CreateSpaceNamed(base::WideToUTF8(name));
    }
  } else if (item.action == "renspace") {
    std::wstring name;
    for (const std::string& summary : host_->SpaceSummaries()) {
      const std::vector<std::string> parts = SplitTabs(summary);
      if (parts.size() > 3 && parts[3] == selected_space_) {
        name = base::UTF8ToWide(parts[0]);
        break;
      }
    }
    if (PhiPromptText(hwnd_, L"Rename Space",
                      L"Enter a new name for this Space.", &name)) {
      host_->RenameSpace(selected_space_, base::WideToUTF8(name));
    }
  } else if (item.action == "delspace") {
    if (PhiConfirm(hwnd_, L"Delete Space",
                   L"Bookmarks and pinned tabs belonging to this Space will "
                   L"also be removed. This action cannot be undone.") &&
        host_->DeleteSpace(selected_space_)) {
      selected_space_.clear();
    }
  } else if (item.action == "renprofile") {
    std::wstring name;
    for (const std::string& summary : host_->ProfileSummaries()) {
      const std::vector<std::string> parts = SplitTabs(summary);
      if (!parts.empty() && parts[0] == selected_profile_) {
        name = base::UTF8ToWide(parts.size() > 1 ? parts[1] : parts[0]);
        break;
      }
    }
    if (PhiPromptText(hwnd_, L"Rename Profile",
                      L"Enter a new name for this profile.", &name)) {
      host_->RenameProfileById(selected_profile_, base::WideToUTF8(name));
    }
  } else if (item.action == "delprofile") {
    if (PhiConfirm(hwnd_, L"Delete Profile",
                   L"All cookies, history, extensions, and saved data on "
                   L"this profile will be permanently removed. This cannot "
                   L"be undone.") &&
        host_->DeleteProfileById(selected_profile_)) {
      selected_profile_.clear();
    }
  } else if (item.action == "addprofile") {
    std::wstring name;
    if (PhiPromptText(hwnd_, L"New Profile",
                      L"Enter a name for the new profile. Each profile has "
                      L"its own cookies, history, and extensions.",
                      &name)) {
      host_->CreateProfileNamed(base::WideToUTF8(name));
    }
  }
}

void PhiSettingsWindow::OnClick(int x, int y) {
  if (!host_) {
    return;
  }
  POINT pt = {x, y};
  for (const Item& item : items_) {
    if (item.action.empty() || !item.enabled ||
        !::PtInRect(&item.bounds, pt)) {
      continue;
    }
    Apply(item, x);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    return;
  }
}

// static
LRESULT CALLBACK PhiSettingsWindow::WndProc(HWND hwnd,
                                            UINT msg,
                                            WPARAM wparam,
                                            LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiSettingsWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        self->Rebuild();
        self->Paint(dc);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN:
      if (self) {
        self->OnClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      }
      return 0;
    case WM_MOUSEMOVE:
      // Dragging a saturation slider.
      if (self && (wparam & MK_LBUTTON)) {
        self->OnClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      }
      return 0;
    case WM_MOUSEWHEEL:
      // Pane content can outgrow the fixed window; the Mac scrolls, so do we.
      if (self) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        RECT client = {};
        ::GetClientRect(hwnd, &client);
        const int visible = client.bottom - self->Scaled(kToolbarHeight);
        const int max_scroll =
            std::max(0, self->content_height_ - visible);
        self->scroll_ = std::max(
            0, std::min(max_scroll, self->scroll_ - delta / 2));
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    case WM_DPICHANGED:
      if (self) {
        if (const UINT dpi = ::GetDpiForWindow(hwnd)) {
          self->dpi_ = dpi;
        }
        self->EnsureFont();
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested) {
          ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    case WM_CLOSE:
      ::ShowWindow(hwnd, SW_HIDE);  // Keep it around; reopening is instant.
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace phi_client
