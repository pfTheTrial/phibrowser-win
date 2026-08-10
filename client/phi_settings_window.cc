// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_settings_window.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <vector>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiClientSettings";

// Design units (points at 100%), the same convention as the shell chrome.
// Card geometry follows the Mac: 7pt continuous corners, a hairline border,
// 12pt horizontal padding inside, 12pt vertical padding per row, 16pt between
// a section's title and its card (GeneralSectionView / GeneralContainerView /
// GeneralRowView).
constexpr int kWidth = 680;
constexpr int kHeight = 620;
constexpr int kToolbarHeight = 68;
constexpr int kToolbarItemWidth = 78;
constexpr int kToolbarIcon = 22;
constexpr int kMargin = 22;
constexpr int kCardPadding = 12;   // Horizontal padding inside a card.
constexpr int kRowPadding = 12;    // Vertical padding around a row's content.
constexpr int kSectionTitleHeight = 20;
constexpr int kSectionTitleGap = 8;
constexpr int kSectionGap = 24;
constexpr int kHintHeight = 22;
constexpr int kChipHeight = 26;
constexpr int kChipGap = 6;
constexpr int kSwatch = 40;
constexpr int kSwatchGap = 8;
constexpr int kToggleWidth = 40;
constexpr int kToggleHeight = 22;
constexpr int kSliderHeight = 20;
constexpr int kSliderWidth = 200;
constexpr int kCardRadius = 7;
constexpr int kRadius = 6;
constexpr int kListRowHeight = 30;

// The Mac's panes(), in its order, with its titles. Glyphs stand in for the
// custom icon assets (settingGeneralIcon and friends).
struct SectionDef {
  const char* id;
  const wchar_t* label;
  const wchar_t* glyph;
};
constexpr std::array<SectionDef, 8> kSections = {{
    {"account", L"Account", L"\u25CB"},
    {"general", L"General", L"\u2699"},
    {"profiles", L"Profiles", L"\u25D2"},
    {"spaces", L"Spaces", L"\u25A6"},
    {"ai", L"AI", L"\u2726"},
    {"phiLink", L"Phi Link", L"\u26AD"},
    {"shortcuts", L"Shortcuts", L"\u2318"},
    {"developer", L"Developer", L"\u2692"},
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

  hwnd_ = ::CreateWindowExW(0, kClassName, L"Phi Settings",
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
  RECT frame = {0, 0, Scaled(kWidth), Scaled(kHeight)};
  ::AdjustWindowRectExForDpi(&frame, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             FALSE, 0, dpi_);
  ::SetWindowPos(hwnd_, nullptr, 0, 0, frame.right - frame.left,
                 frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER);
  EnsureFont();
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
  HFONT built = ::CreateFontIndirectW(&metrics.lfMessageFont);
  if (!built) {
    return;
  }
  HFONT previous = font_;
  font_ = built;
  font_dpi_ = dpi_;
  if (previous) {
    ::DeleteObject(previous);
  }
  // A smaller face for section titles, toolbar captions and hints — the Mac
  // sets those at 12pt against 13pt row labels.
  LOGFONTW small = metrics.lfMessageFont;
  small.lfHeight = ::MulDiv(small.lfHeight, 11, 13);
  if (HFONT small_built = ::CreateFontIndirectW(&small)) {
    HFONT previous_small = small_font_;
    small_font_ = small_built;
    if (previous_small) {
      ::DeleteObject(previous_small);
    }
  }
  // A heavier face, kept for anything that wants emphasis.
  LOGFONTW title = metrics.lfMessageFont;
  title.lfWeight = FW_SEMIBOLD;
  if (HFONT title_built = ::CreateFontIndirectW(&title)) {
    HFONT previous_title = title_font_;
    title_font_ = title_built;
    if (previous_title) {
      ::DeleteObject(previous_title);
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
    if (name == section.id) {
      if (g_instance) {
        g_instance->section_ = name;
        Refresh();
      }
      return true;
    }
  }
  return false;
}

void PhiSettingsWindow::BeginSection(int* y, const wchar_t* title) {
  Item label;
  label.kind = ItemKind::kSectionTitle;
  label.text = title;
  label.bounds = {Scaled(kMargin), *y, Scaled(kWidth - kMargin),
                  *y + Scaled(kSectionTitleHeight)};
  items_.push_back(std::move(label));
  *y += Scaled(kSectionTitleHeight + kSectionTitleGap);

  // The card is emitted FIRST so it paints behind its rows; EndSection sizes
  // it once the rows have been laid out.
  Item card;
  card.kind = ItemKind::kCard;
  card.bounds = {Scaled(kMargin), *y, Scaled(kWidth - kMargin), *y};
  open_card_ = items_.size();
  items_.push_back(std::move(card));
}

void PhiSettingsWindow::EndSection(int* y) {
  if (open_card_ < items_.size()) {
    items_[open_card_].bounds.bottom = *y;
  }
  *y += Scaled(kSectionGap);
}

void PhiSettingsWindow::AddHint(int* y, const wchar_t* text) {
  Item item;
  item.kind = ItemKind::kHint;
  item.text = text;
  item.bounds = {Scaled(kMargin), *y, Scaled(kWidth - kMargin),
                 *y + Scaled(kHintHeight)};
  items_.push_back(std::move(item));
  *y += Scaled(kHintHeight);
}

RECT PhiSettingsWindow::AddRow(int* y, const wchar_t* title,
                               int control_height, int control_width) {
  const int height = control_height + 2 * kRowPadding;
  const int left = Scaled(kMargin + kCardPadding);
  const int right = Scaled(kWidth - kMargin - kCardPadding);
  Item label;
  label.kind = ItemKind::kRowLabel;
  label.text = title;
  label.bounds = {left, *y, right, *y + Scaled(height)};
  items_.push_back(std::move(label));
  // The control is TRAILING, as the Mac's Spacer(minLength:) puts it.
  const int control_left = right - Scaled(control_width);
  const RECT control = {control_left, *y + Scaled(kRowPadding), right,
                        *y + Scaled(kRowPadding + control_height)};
  *y += Scaled(height);
  return control;
}

void PhiSettingsWindow::AddChips(const RECT& area,
                                 const std::vector<std::wstring>& labels,
                                 int selected_index,
                                 const std::string& action) {
  if (labels.empty()) {
    return;
  }
  const int count = static_cast<int>(labels.size());
  const int width =
      (area.right - area.left - (count - 1) * Scaled(kChipGap)) / count;
  for (int i = 0; i < count; ++i) {
    Item chip;
    chip.kind = ItemKind::kChip;
    chip.text = labels[static_cast<size_t>(i)];
    chip.selected = i == selected_index;
    chip.value = i;
    chip.action = action;
    const int x = area.left + i * (width + Scaled(kChipGap));
    chip.bounds = {x, area.top, x + width, area.bottom};
    items_.push_back(std::move(chip));
  }
}

void PhiSettingsWindow::BuildGeneral(int* y) {
  BeginSection(y, L"Theme");
  {
    // The eight built-ins, wrapped to what the card fits.
    const int available = Scaled(kWidth - 2 * (kMargin + kCardPadding));
    const int per_row = std::max(
        1, (available + Scaled(kSwatchGap)) / Scaled(kSwatch + kSwatchGap));
    const auto& themes = PhiBuiltInThemes();
    const int rows = (static_cast<int>(themes.size()) + per_row - 1) / per_row;
    const int block = rows * kSwatch + (rows - 1) * kSwatchGap;
    const int width = per_row * kSwatch + (per_row - 1) * kSwatchGap;
    const RECT area = AddRow(y, L"Color", block, width);
    for (size_t i = 0; i < themes.size(); ++i) {
      const int col = static_cast<int>(i) % per_row;
      const int line = static_cast<int>(i) / per_row;
      Item swatch;
      swatch.kind = ItemKind::kSwatch;
      swatch.payload = themes[i].id;
      swatch.selected = host_->ThemeId() == themes[i].id;
      swatch.swatch = PhiResolvePalette(themes[i], host_->Appearance(),
                                        host_->Saturation()).ground_bottom;
      swatch.action = "theme";
      const int x = area.left + col * Scaled(kSwatch + kSwatchGap);
      const int top = area.top + line * Scaled(kSwatch + kSwatchGap);
      swatch.bounds = {x, top, x + Scaled(kSwatch), top + Scaled(kSwatch)};
      items_.push_back(std::move(swatch));
    }
  }
  {
    const RECT area = AddRow(y, L"Saturation", kSliderHeight, kSliderWidth);
    Item slider;
    slider.kind = ItemKind::kSliderTrack;
    slider.value = host_->Saturation();
    slider.action = "saturation";
    slider.bounds = area;
    items_.push_back(std::move(slider));
  }
  EndSection(y);

  BeginSection(y, L"Appearance");
  AddChips(AddRow(y, L"Layout mode", kChipHeight, 300),
           {L"Performance", L"Balanced", L"Comfortable"},
           static_cast<int>(host_->LayoutMode()), "layout");
  AddChips(AddRow(y, L"Color appearance", kChipHeight, 240),
           {L"System", L"Light", L"Dark"},
           static_cast<int>(host_->Appearance()), "appearance");
  EndSection(y);

  BeginSection(y, L"Browsing");
  AddChips(AddRow(y, L"New tab behavior", kChipHeight, 240),
           {L"New Tab Page", L"Omnibox"},
           static_cast<int>(host_->NewTabBehavior()), "newtab");
  {
    const RECT area = AddRow(y, L"Always show full URL", kToggleHeight,
                             kToggleWidth);
    Item toggle;
    toggle.kind = ItemKind::kToggle;
    toggle.on = host_->AlwaysShowFullUrl();
    toggle.action = "fullurl";
    toggle.bounds = area;
    items_.push_back(std::move(toggle));
  }
  EndSection(y);
}

void PhiSettingsWindow::BuildSpaces(int* y) {
  BeginSection(y, L"Pinned tabs");
  AddChips(AddRow(y, L"Pinned tabs live in", kChipHeight, 320),
           {L"This Space", L"The profile", L"Everywhere"},
           static_cast<int>(host_->PinScope()), "pinscope");
  EndSection(y);
  AddHint(y, L"Where a pinned tab is stored, and so where it shows up.");
  *y += Scaled(kSectionGap);

  BeginSection(y, L"Spaces");
  for (const std::string& summary : host_->SpaceSummaries()) {
    const std::vector<std::string> parts = SplitTabs(summary);
    const std::wstring icon =
        parts.size() > 1 ? base::UTF8ToWide(parts[1]) : std::wstring();
    const std::wstring profile =
        parts.size() > 2 ? base::UTF8ToWide(parts[2]) : std::wstring();
    Item row;
    row.kind = ItemKind::kListRow;
    row.text = (icon.empty() ? L"" : icon + L"   ") +
               base::UTF8ToWide(parts.empty() ? std::string() : parts[0]) +
               (profile.empty() ? L"" : L"   ·   " + profile);
    row.bounds = {Scaled(kMargin + kCardPadding), *y,
                  Scaled(kWidth - kMargin - kCardPadding),
                  *y + Scaled(kListRowHeight)};
    items_.push_back(std::move(row));
    *y += Scaled(kListRowHeight);
  }
  *y += Scaled(kRowPadding);
  EndSection(y);
}

void PhiSettingsWindow::BuildProfiles(int* y) {
  BeginSection(y, L"Profiles");
  for (const std::string& summary : host_->ProfileSummaries()) {
    const std::vector<std::string> parts = SplitTabs(summary);
    Item row;
    row.kind = ItemKind::kListRow;
    row.text = base::UTF8ToWide(parts.size() > 1 ? parts[1] : summary);
    if (parts.size() > 1) {
      row.text += L"   ·   " + base::UTF8ToWide(parts[0]);
    }
    row.bounds = {Scaled(kMargin + kCardPadding), *y,
                  Scaled(kWidth - kMargin - kCardPadding),
                  *y + Scaled(kListRowHeight)};
    items_.push_back(std::move(row));
    *y += Scaled(kListRowHeight);
  }
  {
    const RECT area = AddRow(y, L"Add a profile", kChipHeight, 120);
    Item button;
    button.kind = ItemKind::kButton;
    button.text = L"Add profile";
    button.action = "addprofile";
    button.bounds = area;
    items_.push_back(std::move(button));
  }
  EndSection(y);
}

void PhiSettingsWindow::BuildPlaceholder(int* y, const wchar_t* text) {
  AddHint(y, text);
}

void PhiSettingsWindow::Rebuild() {
  items_.clear();
  open_card_ = 0;
  if (!host_) {
    return;
  }
  // The pane toolbar, centred across the top.
  const int total = static_cast<int>(kSections.size()) * kToolbarItemWidth;
  int x = Scaled((kWidth - total) / 2);
  for (const SectionDef& section : kSections) {
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

  int y = Scaled(kToolbarHeight + kMargin);
  if (section_ == "general") {
    BuildGeneral(&y);
  } else if (section_ == "spaces") {
    BuildSpaces(&y);
  } else if (section_ == "profiles") {
    BuildProfiles(&y);
  } else if (section_ == "shortcuts") {
    BuildPlaceholder(&y, L"Keyboard shortcuts are not wired up yet.");
  } else if (section_ == "ai") {
    BuildPlaceholder(&y, L"Phi AI settings are not available on this build.");
  } else if (section_ == "account") {
    BuildPlaceholder(&y, L"Sign-in is not available on this build.");
  } else if (section_ == "phiLink") {
    BuildPlaceholder(&y, L"Phi Link is not available on this build.");
  } else {
    BuildPlaceholder(&y, L"Developer tools are not exposed on this build.");
  }
}

void PhiSettingsWindow::Paint(HDC dc) {
  if (!host_) {
    return;
  }
  const PhiPalette& palette = host_->Palette();
  RECT client = {};
  ::GetClientRect(hwnd_, &client);

  // Toolbar band in the tint, the pane below on the window ground.
  HBRUSH page = ::CreateSolidBrush(palette.ground_top);
  ::FillRect(dc, &client, page);
  ::DeleteObject(page);
  HBRUSH bar = ::CreateSolidBrush(palette.ground_bottom);
  RECT toolbar = {0, 0, client.right, Scaled(kToolbarHeight)};
  ::FillRect(dc, &toolbar, bar);
  ::DeleteObject(bar);
  HPEN edge = ::CreatePen(PS_SOLID, 1, palette.border);
  HGDIOBJ old_pen = ::SelectObject(dc, edge);
  ::MoveToEx(dc, 0, Scaled(kToolbarHeight), nullptr);
  ::LineTo(dc, client.right, Scaled(kToolbarHeight));
  ::SelectObject(dc, old_pen);
  ::DeleteObject(edge);

  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = font_ ? ::SelectObject(dc, font_) : nullptr;

  for (const Item& item : items_) {
    RECT bounds = item.bounds;
    switch (item.kind) {
      case ItemKind::kToolbarItem: {
        if (item.selected) {
          FillRounded(dc, bounds, palette.card, Scaled(kRadius));
        }
        ::SetTextColor(dc, item.selected ? palette.label : palette.label_dim);
        RECT glyph = bounds;
        glyph.bottom = glyph.top + Scaled(kToolbarIcon + 8);
        const wchar_t* mark = L"\u25CB";
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
        StrokeRounded(dc, bounds, palette.card, palette.border,
                      Scaled(kCardRadius), 1);
        break;
      case ItemKind::kSectionTitle:
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        ::SetTextColor(dc, palette.label_dim);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kRowLabel:
        ::SetTextColor(dc, palette.label);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
      case ItemKind::kHint:
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        ::SetTextColor(dc, palette.label_dim);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kListRow:
        ::SetTextColor(dc, palette.label);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        break;
      case ItemKind::kChip:
        if (item.selected) {
          FillRounded(dc, bounds, palette.accent, Scaled(kRadius));
        } else {
          StrokeRounded(dc, bounds, palette.card, palette.border,
                        Scaled(kRadius), 1);
        }
        ::SetTextColor(dc, item.selected ? RGB(0xFF, 0xFF, 0xFF)
                                         : palette.label_dim);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
      case ItemKind::kSwatch:
        StrokeRounded(dc, bounds, item.swatch,
                      item.selected ? palette.accent : palette.border,
                      Scaled(kRadius), item.selected ? Scaled(3) : 1);
        break;
      case ItemKind::kToggle: {
        const int height = bounds.bottom - bounds.top;
        FillRounded(dc, bounds, item.on ? palette.accent : palette.border,
                    height / 2);
        const int knob = height - Scaled(6);
        const int left = item.on ? bounds.right - knob - Scaled(3)
                                 : bounds.left + Scaled(3);
        RECT dot = {left, bounds.top + Scaled(3), left + knob,
                    bounds.top + Scaled(3) + knob};
        FillRounded(dc, dot, RGB(0xFF, 0xFF, 0xFF), knob / 2);
        break;
      }
      case ItemKind::kSliderTrack: {
        const int mid = (bounds.top + bounds.bottom) / 2;
        RECT track = {bounds.left, mid - Scaled(2), bounds.right,
                      mid + Scaled(2)};
        FillRounded(dc, track, palette.border, Scaled(2));
        const int span = bounds.right - bounds.left;
        const int at = bounds.left + span * item.value / 100;
        RECT filled = {bounds.left, track.top, at, track.bottom};
        FillRounded(dc, filled, palette.accent, Scaled(2));
        const int knob = Scaled(14);
        RECT dot = {at - knob / 2, mid - knob / 2, at + knob / 2,
                    mid + knob / 2};
        StrokeRounded(dc, dot, palette.card, palette.accent, knob / 2, 2);
        break;
      }
      case ItemKind::kButton:
        StrokeRounded(dc, bounds, palette.card, palette.border, Scaled(kRadius),
                      1);
        ::SetTextColor(dc, palette.label);
        ::DrawTextW(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
  }

  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

void PhiSettingsWindow::Apply(const Item& item, int x) {
  if (item.action == "section") {
    section_ = item.payload;
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
  } else if (item.action == "pinscope") {
    host_->SetPinScope(static_cast<PhiPinScope>(item.value));
  } else if (item.action == "saturation") {
    const int span = item.bounds.right - item.bounds.left;
    if (span > 0) {
      const int value = (x - item.bounds.left) * 100 / span;
      host_->SetSaturation(value < 0 ? 0 : (value > 100 ? 100 : value));
    }
  } else if (item.action == "addprofile") {
    host_->CreateProfileNamed("Profile");
  }
}

void PhiSettingsWindow::OnClick(int x, int y) {
  if (!host_) {
    return;
  }
  POINT pt = {x, y};
  for (const Item& item : items_) {
    if (item.action.empty() || !::PtInRect(&item.bounds, pt)) {
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
      // Dragging the saturation slider.
      if (self && (wparam & MK_LBUTTON)) {
        self->OnClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
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
