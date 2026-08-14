// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_extensions_popover.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_dwrite.h"

#pragma comment(lib, "dwmapi.lib")

namespace phi_client {

namespace {

constexpr int kPanelWidth = 300;
constexpr int kPanelMaxHeight = 380;
constexpr int kHeaderHeight = 40;
constexpr int kRowHeight = 40;
constexpr int kIconSlot = 20;
constexpr UINT kPollTimer = 1;

void FillRect(HDC dc, const RECT& r, COLORREF color) {
  HBRUSH brush = ::CreateSolidBrush(color);
  ::FillRect(dc, &r, brush);
  ::DeleteObject(brush);
}

void FillRounded(HDC dc, const RECT& r, COLORREF color, int radius) {
  HBRUSH brush = ::CreateSolidBrush(color);
  HPEN pen = ::CreatePen(PS_SOLID, 1, color);
  HGDIOBJ old_brush = ::SelectObject(dc, brush);
  HGDIOBJ old_pen = ::SelectObject(dc, pen);
  ::RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  ::DeleteObject(pen);
  ::DeleteObject(brush);
}

// Box-averages a premultiplied BGRA buffer down to `out_px` square — the
// manifest icons arrive at 48 or 128 and the row slot is 20.
std::vector<unsigned char> ScaleBgra(const unsigned char* bgra,
                                     int width,
                                     int height,
                                     int out_px) {
  std::vector<unsigned char> out(
      static_cast<size_t>(out_px) * static_cast<size_t>(out_px) * 4, 0);
  if (!bgra || width <= 0 || height <= 0 || out_px <= 0) {
    return out;
  }
  const auto source = UNSAFE_BUFFERS(base::span<const unsigned char>(
      bgra, static_cast<size_t>(width) * static_cast<size_t>(height) * 4));
  for (int y = 0; y < out_px; ++y) {
    const int sy0 = y * height / out_px;
    const int sy1 = std::max(sy0 + 1, (y + 1) * height / out_px);
    for (int x = 0; x < out_px; ++x) {
      const int sx0 = x * width / out_px;
      const int sx1 = std::max(sx0 + 1, (x + 1) * width / out_px);
      std::array<unsigned int, 4> sum = {0, 0, 0, 0};
      for (int sy = sy0; sy < sy1; ++sy) {
        for (int sx = sx0; sx < sx1; ++sx) {
          const size_t at =
              (static_cast<size_t>(sy) * static_cast<size_t>(width) +
               static_cast<size_t>(sx)) *
              4;
          for (int c = 0; c < 4; ++c) {
            sum[c] += source[at + static_cast<size_t>(c)];
          }
        }
      }
      const unsigned int n =
          static_cast<unsigned int>((sy1 - sy0) * (sx1 - sx0));
      const size_t to =
          (static_cast<size_t>(y) * static_cast<size_t>(out_px) +
           static_cast<size_t>(x)) *
          4;
      for (int c = 0; c < 4; ++c) {
        out[to + static_cast<size_t>(c)] =
            static_cast<unsigned char>(sum[c] / n);
      }
    }
  }
  return out;
}

struct ExtensionRow {
  std::string id;
  std::wstring name;
  bool pinned = false;
  RECT bounds{};
  RECT pin_box{};
};

class Popover {
 public:
  static Popover* instance;
  static HWND last_owner;
  static ULONGLONG closed_tick;

  Popover(HWND owner, const RECT& anchor, const PhiPalette& palette,
          phi_framework::PhiFramework* framework, int64_t browser_id)
      : owner_(owner), palette_(palette), framework_(framework),
        browser_id_(browser_id) {
    dpi_ = ::GetDpiForWindow(owner);
    Register();
    Refresh();
    const int w = Scaled(kPanelWidth);
    const int h = PanelHeight();
    // Above the anchor when it is a bottom-bar button; clamped to the work
    // area either way.
    int x = anchor.left;
    int y = anchor.top - h - Scaled(8);
    HMONITOR monitor = ::MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof(info)};
    if (::GetMonitorInfoW(monitor, &info)) {
      x = std::max(static_cast<int>(info.rcWork.left),
                   std::min(x, static_cast<int>(info.rcWork.right) - w));
      if (y < info.rcWork.top) {
        y = anchor.bottom + Scaled(8);  // Header buttons open DOWNWARD.
      }
    }
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW, L"PhiExtensionsPopover", L"", WS_POPUP, x, y, w, h,
        owner, nullptr, ::GetModuleHandleW(nullptr), this);
    ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &kCorner,
                            sizeof(kCorner));
    EnsureFonts();
    created_tick_ = ::GetTickCount64();
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    // The list loads asynchronously on first use; poll cheaply while open so
    // the panel fills even if the delegate event races the open.
    ::SetTimer(hwnd_, kPollTimer, 700, nullptr);
  }

  ~Popover() {
    for (HFONT font : {name_font_, title_font_, glyph_font_, empty_font_}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
  }

  HWND hwnd() const { return hwnd_; }

  void Refresh() {
    const char* json =
        framework_ ? framework_->ListExtensionsJson(browser_id_) : "[]";
    if (json == last_json_ && !rows_.empty()) {
      return;
    }
    last_json_ = json;
    rows_.clear();
    std::optional<base::Value> parsed =
        base::JSONReader::Read(last_json_, base::JSON_PARSE_RFC);
    if (parsed && parsed->is_list()) {
      for (const base::Value& value : parsed->GetList()) {
        const base::DictValue* dict = value.GetIfDict();
        if (!dict) {
          continue;
        }
        ExtensionRow row;
        if (const std::string* id = dict->FindString("id")) {
          row.id = *id;
        }
        if (const std::string* name = dict->FindString("name")) {
          row.name = base::UTF8ToWide(*name);
        }
        row.pinned = dict->FindBool("pinned").value_or(false);
        rows_.push_back(std::move(row));
      }
    }
    if (hwnd_) {
      // The row count changes the panel height (grown downward is wrong for
      // a bottom-bar popover — keep the BOTTOM edge pinned).
      RECT rect = {};
      ::GetWindowRect(hwnd_, &rect);
      const int h = PanelHeight();
      ::SetWindowPos(hwnd_, nullptr, rect.left, rect.bottom - h,
                     Scaled(kPanelWidth), h, SWP_NOZORDER | SWP_NOACTIVATE);
      ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
  }

 private:
  static inline const DWM_WINDOW_CORNER_PREFERENCE kCorner = DWMWCP_ROUND;

  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }

  int PanelHeight() const {
    const int rows = std::max(1, static_cast<int>(rows_.size()));
    return std::min(Scaled(kPanelMaxHeight),
                    Scaled(kHeaderHeight) + rows * Scaled(kRowHeight) +
                        Scaled(8));
  }

  static void Register() {
    static bool done = false;
    if (done) {
      return;
    }
    done = true;
    WNDCLASSW cls = {};
    cls.lpfnWndProc = &Popover::WndProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"PhiExtensionsPopover";
    ::RegisterClassW(&cls);
  }

  void EnsureFonts() {
    auto make = [&](int size, int weight, const wchar_t* face) {
      return ::CreateFontW(-Scaled(size), 0, 0, 0, weight, FALSE, FALSE,
                           FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, face);
    };
    name_font_ = make(13, FW_NORMAL, L"Segoe UI");
    title_font_ = make(13, FW_SEMIBOLD, L"Segoe UI");
    glyph_font_ = make(12, FW_NORMAL, L"Segoe MDL2 Assets");
    empty_font_ = make(28, FW_NORMAL, L"Segoe MDL2 Assets");
  }

  void Paint(HDC dc, const RECT& client) {
    const COLORREF bg = palette_.ground_bottom;
    const COLORREF label = palette_.label;
    const COLORREF dim = palette_.label_dim;
    FillRect(dc, client, bg);
    ::SetBkMode(dc, TRANSPARENT);

    RECT title = {Scaled(16), 0, client.right - Scaled(16),
                  Scaled(kHeaderHeight)};
    ::SelectObject(dc, title_font_);
    ::SetTextColor(dc, label);
    ::DrawTextW(dc, L"Extensions", -1, &title,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (rows_.empty()) {
      RECT glyph = {0, Scaled(kHeaderHeight), client.right,
                    Scaled(kHeaderHeight + 48)};
      ::SelectObject(dc, empty_font_);
      ::SetTextColor(dc, dim);
      ::DrawTextW(dc, L"", -1, &glyph,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      RECT hint = {0, glyph.bottom, client.right,
                   glyph.bottom + Scaled(20)};
      ::SelectObject(dc, name_font_);
      ::DrawTextW(dc, L"No extensions installed", -1, &hint,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      return;
    }

    int y = Scaled(kHeaderHeight);
    for (ExtensionRow& row : rows_) {
      row.bounds = {Scaled(6), y, client.right - Scaled(6),
                    y + Scaled(kRowHeight)};
      const int mid = (row.bounds.top + row.bounds.bottom) / 2;
      const bool hot = hover_row_ == static_cast<int>(&row - rows_.data());
      if (hot) {
        FillRounded(dc, row.bounds, palette_.hover, Scaled(6));
      }
      // Icon.
      const int icon_px = Scaled(kIconSlot);
      RECT icon = {row.bounds.left + Scaled(10), mid - icon_px / 2,
                   row.bounds.left + Scaled(10) + icon_px, mid + icon_px / 2};
      int iw = 0;
      int ih = 0;
      const unsigned char* bgra =
          framework_ ? framework_->ExtensionIconBgra(row.id.c_str(), &iw, &ih)
                     : nullptr;
      if (bgra && iw > 0 && ih > 0) {
        const std::vector<unsigned char> scaled =
            ScaleBgra(bgra, iw, ih, icon_px);
        PhiBlendBgra(dc, icon, scaled.data(), icon_px, icon_px);
      } else {
        ::SelectObject(dc, glyph_font_);
        ::SetTextColor(dc, dim);
        ::DrawTextW(dc, L"", -1, &icon,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      // Pin toggle, trailing (the Mac popover's pin column).
      const int pin_px = Scaled(22);
      row.pin_box = {row.bounds.right - Scaled(10) - pin_px, mid - pin_px / 2,
                     row.bounds.right - Scaled(10), mid + pin_px / 2};
      const bool pin_hot =
          hot && hover_pin_ &&
          hover_row_ == static_cast<int>(&row - rows_.data());
      if (pin_hot) {
        FillRounded(dc, row.pin_box, palette_.selected, Scaled(4));
      }
      ::SelectObject(dc, glyph_font_);
      ::SetTextColor(dc, row.pinned ? palette_.accent : dim);
      ::DrawTextW(dc, row.pinned ? L"" : L"", -1, &row.pin_box,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      // Name.
      RECT text = {icon.right + Scaled(10), row.bounds.top,
                   row.pin_box.left - Scaled(8), row.bounds.bottom};
      ::SelectObject(dc, name_font_);
      ::SetTextColor(dc, label);
      ::DrawTextW(dc, row.name.c_str(), -1, &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      y += Scaled(kRowHeight);
    }
  }

  void OnMouseMove(int x, int y) {
    int row = -1;
    bool pin = false;
    const POINT pt = {x, y};
    for (const ExtensionRow& candidate : rows_) {
      if (::PtInRect(&candidate.bounds, pt)) {
        row = static_cast<int>(&candidate - rows_.data());
        pin = ::PtInRect(&candidate.pin_box, pt);
        break;
      }
    }
    if (row != hover_row_ || pin != hover_pin_) {
      hover_row_ = row;
      hover_pin_ = pin;
      ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd_, 0};
    ::TrackMouseEvent(&track);
  }

  void OnClick() {
    if (hover_row_ < 0 || hover_row_ >= static_cast<int>(rows_.size()) ||
        !framework_) {
      return;
    }
    const ExtensionRow row = rows_[static_cast<size_t>(hover_row_)];
    if (hover_pin_) {
      framework_->SetExtensionPinned(browser_id_, row.id.c_str(),
                                     !row.pinned);
      // The change notification refreshes the list; flip optimistically so
      // the toggle reads instant.
      rows_[static_cast<size_t>(hover_row_)].pinned = !row.pinned;
      ::InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
    // Run the action anchored to the ROW (screen coords) and leave — the
    // extension's own popup takes the stage.
    RECT anchor = row.bounds;
    ::MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    framework_->TriggerExtensionAction(
        browser_id_, row.id.c_str(), anchor.left, anchor.top,
        anchor.right - anchor.left, anchor.bottom - anchor.top);
    ::DestroyWindow(hwnd_);
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM w,
                                  LPARAM l) {
    Popover* self = reinterpret_cast<Popover*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
      case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<Popover*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        break;
      }
      case WM_ACTIVATE:
        if (self && LOWORD(w) == WA_INACTIVE) {
          // The downloads popover's opening grace against activation churn.
          if (::GetTickCount64() - self->created_tick_ < 500) {
            break;
          }
          ::DestroyWindow(hwnd);
          return 0;
        }
        break;
      case WM_TIMER:
        if (self && w == kPollTimer) {
          self->Refresh();
          return 0;
        }
        break;
      case WM_MOUSEMOVE:
        if (self) {
          self->OnMouseMove(GET_X_LPARAM(l), GET_Y_LPARAM(l));
          return 0;
        }
        break;
      case WM_MOUSELEAVE:
        if (self) {
          self->hover_row_ = -1;
          self->hover_pin_ = false;
          ::InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        break;
      case WM_LBUTTONUP:
        if (self) {
          self->OnClick();
          return 0;
        }
        break;
      case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = ::BeginPaint(hwnd, &paint);
        RECT client;
        ::GetClientRect(hwnd, &client);
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP bitmap =
            ::CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = ::SelectObject(mem, bitmap);
        if (self) {
          self->Paint(mem, client);
        }
        ::BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        ::SelectObject(mem, old);
        ::DeleteObject(bitmap);
        ::DeleteDC(mem);
        ::EndPaint(hwnd, &paint);
        return 0;
      }
      case WM_DESTROY:
        if (self) {
          ::KillTimer(hwnd, kPollTimer);
          Popover::closed_tick = ::GetTickCount64();
          Popover::last_owner = self->owner_;
          Popover::instance = nullptr;
          ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
          delete self;
        }
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, w, l);
  }

  HWND owner_;
  HWND hwnd_ = nullptr;
  ULONGLONG created_tick_ = 0;
  PhiPalette palette_;
  // Not rewritten to raw_ptr: the popover lives strictly inside the
  // framework's lifetime (owned popup of the shell window).
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_;
  int64_t browser_id_;
  UINT dpi_ = 96;
  std::vector<ExtensionRow> rows_;
  std::string last_json_;
  int hover_row_ = -1;
  bool hover_pin_ = false;
  HFONT name_font_ = nullptr;
  HFONT title_font_ = nullptr;
  HFONT glyph_font_ = nullptr;
  HFONT empty_font_ = nullptr;
};

Popover* Popover::instance = nullptr;
HWND Popover::last_owner = nullptr;
ULONGLONG Popover::closed_tick = 0;

}  // namespace

void PhiExtensionsPopover::Toggle(HWND owner,
                                  const RECT& anchor_screen,
                                  const PhiPalette& palette,
                                  phi_framework::PhiFramework* framework,
                                  int64_t browser_id) {
  if (Popover::instance) {
    ::DestroyWindow(Popover::instance->hwnd());
    return;
  }
  if (Popover::last_owner == owner &&
      ::GetTickCount64() - Popover::closed_tick < 300) {
    return;
  }
  Popover::instance =
      new Popover(owner, anchor_screen, palette, framework, browser_id);
}

void PhiExtensionsPopover::RefreshOpen() {
  if (Popover::instance) {
    Popover::instance->Refresh();
  }
}

}  // namespace phi_client
