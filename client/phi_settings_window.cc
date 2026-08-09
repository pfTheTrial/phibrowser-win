// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_settings_window.h"

#include <windowsx.h>

#include <array>

#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiClientSettings";

// Design units (points at 100%), same convention as the shell chrome.
constexpr int kWidth = 460;
// Sum of the sections rather than a guess: margin + title + gap
// + (label + chips + gap) + (label + 2 swatch rows + gap) + (label + chips)
// + margin.
constexpr int kHeight = 24 + 30 + 28 + (22 + 32 + 28) +
                        (22 + 2 * (56 + 10 + 22) + 28) + (22 + 32) + 24;
constexpr int kMargin = 24;
constexpr int kSectionGap = 28;
constexpr int kTitleHeight = 30;
constexpr int kLabelHeight = 22;
constexpr int kChipHeight = 32;
constexpr int kChipGap = 8;
constexpr int kSwatch = 56;
constexpr int kSwatchGap = 10;
constexpr int kRadius = 8;

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

}  // namespace

PhiSettingsWindow::~PhiSettingsWindow() {
  if (font_) {
    ::DeleteObject(font_);
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
  // Size to the design once the real DPI is known.
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
  if (g_instance && g_instance->hwnd_ && ::IsWindow(g_instance->hwnd_)) {
    ::InvalidateRect(g_instance->hwnd_, nullptr, TRUE);
  }
}

void PhiSettingsWindow::Rebuild() {
  choices_.clear();
  if (!host_) {
    return;
  }
  const int left = Scaled(kMargin);
  const int right = Scaled(kWidth - kMargin);
  int y = Scaled(kMargin + kTitleHeight + kSectionGap);

  // Appearance — the Mac's UserAppearanceChoice, same three options.
  y += Scaled(kLabelHeight);
  static constexpr std::array<const wchar_t*, 3> kAppearances = {
      L"System", L"Light", L"Dark"};
  const int chip_w = (right - left - 2 * Scaled(kChipGap)) / 3;
  for (int i = 0; i < 3; ++i) {
    Choice choice;
    choice.bounds = {left + i * (chip_w + Scaled(kChipGap)), y,
                     left + i * (chip_w + Scaled(kChipGap)) + chip_w,
                     y + Scaled(kChipHeight)};
    choice.label = kAppearances[i];
    choice.selected = static_cast<int>(host_->Appearance()) == i;
    choice.kind = 0;
    choice.index = i;
    choices_.push_back(std::move(choice));
  }
  y += Scaled(kChipHeight + kSectionGap);

  // Theme — the eight built-ins, drawn as their own tint.
  y += Scaled(kLabelHeight);
  const auto& themes = PhiBuiltInThemes();
  const int per_row = 4;
  for (size_t i = 0; i < themes.size(); ++i) {
    const int col = static_cast<int>(i) % per_row;
    const int row = static_cast<int>(i) / per_row;
    Choice choice;
    choice.bounds = {left + col * Scaled(kSwatch + kSwatchGap),
                     y + row * Scaled(kSwatch + kSwatchGap + kLabelHeight),
                     left + col * Scaled(kSwatch + kSwatchGap) + Scaled(kSwatch),
                     y + row * Scaled(kSwatch + kSwatchGap + kLabelHeight) +
                         Scaled(kSwatch)};
    choice.label = base::UTF8ToWide(themes[i].name);
    choice.id = themes[i].id;
    choice.selected = host_->ThemeId() == themes[i].id;
    choice.swatch =
        PhiResolvePalette(themes[i], host_->Appearance()).ground_bottom;
    choice.kind = 1;
    choice.index = static_cast<int>(i);
    choices_.push_back(std::move(choice));
  }
  y += 2 * Scaled(kSwatch + kSwatchGap + kLabelHeight) + Scaled(kSectionGap);

  // Layout — the same three modes the sidebar used to cycle through.
  y += Scaled(kLabelHeight);
  static constexpr std::array<const wchar_t*, 3> kLayouts = {
      L"Performance", L"Balanced", L"Comfortable"};
  for (int i = 0; i < 3; ++i) {
    Choice choice;
    choice.bounds = {left + i * (chip_w + Scaled(kChipGap)), y,
                     left + i * (chip_w + Scaled(kChipGap)) + chip_w,
                     y + Scaled(kChipHeight)};
    choice.label = kLayouts[i];
    choice.selected = static_cast<int>(host_->LayoutMode()) == i;
    choice.kind = 2;
    choice.index = i;
    choices_.push_back(std::move(choice));
  }
}

void PhiSettingsWindow::Paint(HDC dc) {
  if (!host_) {
    return;
  }
  const PhiPalette& palette = host_->Palette();
  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  HBRUSH ground = ::CreateSolidBrush(palette.ground_top);
  ::FillRect(dc, &client, ground);
  ::DeleteObject(ground);

  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = font_ ? ::SelectObject(dc, font_) : nullptr;

  const int left = Scaled(kMargin);
  const int right = Scaled(kWidth - kMargin);
  ::SetTextColor(dc, palette.label);
  RECT title = {left, Scaled(kMargin), right, Scaled(kMargin + kTitleHeight)};
  ::DrawTextW(dc, L"Settings", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

  auto section = [&](const wchar_t* text, int top) {
    RECT r = {left, top, right, top + Scaled(kLabelHeight)};
    ::SetTextColor(dc, palette.label_dim);
    ::DrawTextW(dc, text, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  };
  int y = Scaled(kMargin + kTitleHeight + kSectionGap);
  section(L"Appearance", y);
  y += Scaled(kLabelHeight + kChipHeight + kSectionGap);
  section(L"Theme", y);
  y += Scaled(kLabelHeight) + 2 * Scaled(kSwatch + kSwatchGap + kLabelHeight) +
       Scaled(kSectionGap);
  section(L"Layout", y);

  for (const Choice& choice : choices_) {
    if (choice.kind == 1) {
      // Theme swatch: its own tint, ringed in the accent when selected.
      FillRounded(dc, choice.bounds, choice.swatch, Scaled(kRadius));
      if (choice.selected) {
        StrokeRounded(dc, choice.bounds, choice.swatch, palette.accent,
                      Scaled(kRadius), Scaled(3));
      } else {
        StrokeRounded(dc, choice.bounds, choice.swatch, palette.border,
                      Scaled(kRadius), 1);
      }
      RECT label = {choice.bounds.left, choice.bounds.bottom,
                    choice.bounds.right,
                    choice.bounds.bottom + Scaled(kLabelHeight)};
      ::SetTextColor(dc, choice.selected ? palette.label : palette.label_dim);
      ::DrawTextW(dc, choice.label.c_str(), -1, &label,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      continue;
    }
    if (choice.selected) {
      FillRounded(dc, choice.bounds, palette.card, Scaled(kRadius));
    } else {
      StrokeRounded(dc, choice.bounds, palette.ground_bottom, palette.border,
                    Scaled(kRadius), 1);
    }
    ::SetTextColor(dc, choice.selected ? palette.label : palette.label_dim);
    ::DrawTextW(dc, choice.label.c_str(), -1, const_cast<RECT*>(&choice.bounds),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }

  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

void PhiSettingsWindow::OnClick(int x, int y) {
  if (!host_) {
    return;
  }
  POINT pt = {x, y};
  for (const Choice& choice : choices_) {
    if (!::PtInRect(&choice.bounds, pt)) {
      continue;
    }
    switch (choice.kind) {
      case 0:
        host_->SetAppearance(static_cast<PhiAppearance>(choice.index));
        break;
      case 1:
        host_->SetThemeId(choice.id);
        break;
      default:
        host_->RequestLayoutMode(static_cast<PhiLayoutMode>(choice.index));
        break;
    }
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
