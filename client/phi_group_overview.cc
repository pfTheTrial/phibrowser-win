// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_group_overview.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <optional>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

constexpr int kCardWidth = 240;
constexpr int kCardHeight = 190;
constexpr int kCardGap = 20;
constexpr int kGridPad = 32;
constexpr int kFooterHeight = 30;
constexpr int kCloseBox = 18;

struct Card {
  bool add_card = false;  // The trailing "+" card.
  int tab_id = 0;
  int index = -1;
  std::wstring title;
  std::vector<unsigned char> thumbnail;
  int thumb_w = 0;
  int thumb_h = 0;
  std::vector<unsigned char> favicon;
  int fav_w = 0;
  int fav_h = 0;
  RECT bounds{};
  RECT close_box{};
};

class Overview {
 public:
  Overview(PhiWindowController* controller, const std::string& group_id)
      : controller_(controller), group_id_(group_id) {
    dpi_ = ::GetDpiForWindow(controller_->shell_hwnd());
    Register();
    BuildCards();
    hwnd_ = ::CreateWindowExW(0, L"PhiGroupOverview", L"", WS_CHILD, 0, 0, 0,
                              0, controller_->shell_hwnd(), nullptr,
                              ::GetModuleHandleW(nullptr), nullptr);
    font_ = ::CreateFontW(-Scaled(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                          FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Segoe UI");
    Reposition();
    ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    ::SetFocus(hwnd_);
  }

  ~Overview() {
    if (font_) {
      ::DeleteObject(font_);
    }
    if (hwnd_ && ::IsWindow(hwnd_)) {
      ::DestroyWindow(hwnd_);
    }
  }

  static Overview* instance;

 private:
  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }

  void Register() {
    static bool registered = [] {
      WNDCLASSEXW wc = {};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = &OverviewProc;
      wc.hInstance = ::GetModuleHandleW(nullptr);
      wc.lpszClassName = L"PhiGroupOverview";
      return ::RegisterClassExW(&wc) != 0;
    }();
    (void)registered;
  }

  void Reposition() {
    // The overlay covers the CONTENT card (the page area).
    RECT content = controller_->ContentRect();
    ::SetWindowPos(hwnd_, HWND_TOP, content.left, content.top,
                   content.right - content.left, content.bottom - content.top,
                   SWP_NOACTIVATE);
    LayoutCards();
  }

  void BuildCards() {
    cards_.clear();
    phi_framework::PhiFramework* framework = controller_->framework();
    if (!framework) {
      return;
    }
    const int64_t browser = controller_->active_browser()
                                ? controller_->active_browser()
                                : controller_->window_id();
    std::optional<base::Value> tabs = base::JSONReader::Read(
        framework->ListTabsJson(browser), base::JSON_PARSE_RFC);
    if (tabs && tabs->is_list()) {
      for (const base::Value& v : tabs->GetList()) {
        const base::DictValue* d = v.GetIfDict();
        if (!d) {
          continue;
        }
        const std::string* group = d->FindString("groupId");
        if (!group || *group != group_id_) {
          continue;
        }
        Card card;
        card.tab_id = d->FindInt("id").value_or(0);
        card.index = d->FindInt("index").value_or(-1);
        if (const std::string* s = d->FindString("title")) {
          card.title = base::UTF8ToWide(*s);
        }
        int w = 0, h = 0;
        if (const unsigned char* pixels =
                framework->TabThumbnailBgra(browser, card.tab_id, &w, &h)) {
          const auto copy = UNSAFE_BUFFERS(base::span<const unsigned char>(
              pixels, static_cast<size_t>(w) * h * 4));
          card.thumbnail.assign(copy.begin(), copy.end());
          card.thumb_w = w;
          card.thumb_h = h;
        }
        if (const unsigned char* pixels =
                framework->TabFaviconBgra(browser, card.index, &w, &h)) {
          const auto copy = UNSAFE_BUFFERS(base::span<const unsigned char>(
              pixels, static_cast<size_t>(w) * h * 4));
          card.favicon.assign(copy.begin(), copy.end());
          card.fav_w = w;
          card.fav_h = h;
        }
        cards_.push_back(std::move(card));
      }
    }
    Card add;
    add.add_card = true;
    cards_.push_back(std::move(add));
  }

  void LayoutCards() {
    RECT rc = {};
    ::GetClientRect(hwnd_, &rc);
    const int usable = rc.right - rc.left - Scaled(kGridPad) * 2;
    const int per_row = std::max(
        1, (usable + Scaled(kCardGap)) /
               (Scaled(kCardWidth) + Scaled(kCardGap)));
    int x = rc.left + Scaled(kGridPad);
    int y = rc.top + Scaled(kGridPad);
    int in_row = 0;
    for (Card& card : cards_) {
      card.bounds = {x, y, x + Scaled(kCardWidth), y + Scaled(kCardHeight)};
      card.close_box = {card.bounds.right - Scaled(kCloseBox) - Scaled(6),
                        card.bounds.top + Scaled(6),
                        card.bounds.right - Scaled(6),
                        card.bounds.top + Scaled(6) + Scaled(kCloseBox)};
      if (++in_row >= per_row) {
        in_row = 0;
        x = rc.left + Scaled(kGridPad);
        y += Scaled(kCardHeight) + Scaled(kCardGap);
      } else {
        x += Scaled(kCardWidth) + Scaled(kCardGap);
      }
    }
  }

  void Paint(HDC dc) {
    RECT rc = {};
    ::GetClientRect(hwnd_, &rc);
    const PhiPalette palette = controller_->CurrentPalette();
    HBRUSH bg = ::CreateSolidBrush(palette.window_bg);
    ::FillRect(dc, &rc, bg);
    ::DeleteObject(bg);
    ::SetBkMode(dc, TRANSPARENT);
    for (const Card& card : cards_) {
      if (card.add_card) {
        HBRUSH wash = ::CreateSolidBrush(palette.hover);
        HGDIOBJ prev_brush = ::SelectObject(dc, wash);
        HGDIOBJ prev_pen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
        ::RoundRect(dc, card.bounds.left, card.bounds.top,
                    card.bounds.right + 1, card.bounds.bottom + 1,
                    Scaled(10), Scaled(10));
        ::SelectObject(dc, prev_pen);
        ::SelectObject(dc, prev_brush);
        ::DeleteObject(wash);
        RECT plus = card.bounds;
        HGDIOBJ prev_font = font_ ? ::SelectObject(dc, font_) : nullptr;
        ::SetTextColor(dc, palette.label_dim);
        ::DrawTextW(dc, L"+", -1, &plus,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (prev_font) {
          ::SelectObject(dc, prev_font);
        }
        continue;
      }
      RECT shot = card.bounds;
      shot.bottom -= Scaled(kFooterHeight);
      if (!card.thumbnail.empty()) {
        PhiDrawBgra(dc, shot, card.thumbnail.data(), card.thumb_w,
                    card.thumb_h);
      } else {
        HBRUSH ground = ::CreateSolidBrush(palette.card);
        ::FillRect(dc, &shot, ground);
        ::DeleteObject(ground);
      }
      HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
      HGDIOBJ prev_pen = ::SelectObject(dc, pen);
      HGDIOBJ prev_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
      ::Rectangle(dc, card.bounds.left, card.bounds.top, card.bounds.right,
                  card.bounds.bottom);
      ::SelectObject(dc, prev_brush);
      ::SelectObject(dc, prev_pen);
      ::DeleteObject(pen);
      // Close box.
      HGDIOBJ prev_font = font_ ? ::SelectObject(dc, font_) : nullptr;
      ::SetTextColor(dc, palette.label_dim);
      RECT close_rc = card.close_box;
      ::DrawTextW(dc, L"✕", -1, &close_rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      RECT footer = card.bounds;
      footer.top = shot.bottom + Scaled(4);
      RECT icon_rc = {footer.left + Scaled(8), footer.top,
                      footer.left + Scaled(24), footer.bottom - Scaled(6)};
      if (!card.favicon.empty()) {
        PhiBlendBgra(dc, icon_rc, card.favicon.data(), card.fav_w,
                     card.fav_h);
      }
      RECT title_rc = {icon_rc.right + Scaled(6), footer.top,
                       footer.right - Scaled(8), footer.bottom};
      ::SetTextColor(dc, palette.label);
      ::DrawTextW(dc, card.title.c_str(), -1, &title_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      if (prev_font) {
        ::SelectObject(dc, prev_font);
      }
    }
  }

  void Click(POINT pt) {
    phi_framework::PhiFramework* framework = controller_->framework();
    const int64_t browser = controller_->active_browser()
                                ? controller_->active_browser()
                                : controller_->window_id();
    for (const Card& card : cards_) {
      if (!::PtInRect(&card.bounds, pt)) {
        continue;
      }
      if (card.add_card) {
        if (framework) {
          const int fresh = framework->NewTab(browser, "");
          if (fresh >= 0) {
            const std::string ids =
                "[" +
                base::NumberToString(TabIdAtIndex(framework, browser,
                                                  fresh)) +
                "]";
            framework->AddTabsToGroup(browser, group_id_.c_str(),
                                      ids.c_str());
          }
        }
        PhiGroupOverview::Close();
        return;
      }
      if (::PtInRect(&card.close_box, pt)) {
        if (framework && card.index >= 0) {
          framework->CloseTab(browser, card.index);
        }
        // Rebuild in place: indices shifted and a card left.
        BuildCards();
        LayoutCards();
        ::InvalidateRect(hwnd_, nullptr, TRUE);
        controller_->RefreshTabs();
        return;
      }
      if (framework && card.index >= 0) {
        framework->ActivateTab(browser, card.index);
      }
      PhiGroupOverview::Close();
      return;
    }
  }

  static int TabIdAtIndex(phi_framework::PhiFramework* framework,
                          int64_t browser,
                          int index) {
    std::optional<base::Value> tabs = base::JSONReader::Read(
        framework->ListTabsJson(browser), base::JSON_PARSE_RFC);
    if (tabs && tabs->is_list()) {
      for (const base::Value& v : tabs->GetList()) {
        const base::DictValue* d = v.GetIfDict();
        if (d && d->FindInt("index").value_or(-1) == index) {
          return d->FindInt("id").value_or(0);
        }
      }
    }
    return 0;
  }

  static LRESULT CALLBACK OverviewProc(HWND hwnd, UINT msg, WPARAM w,
                                       LPARAM l) {
    Overview* self = instance && instance->hwnd_ == hwnd ? instance : nullptr;
    switch (msg) {
      case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = ::BeginPaint(hwnd, &ps);
        if (self) {
          self->Paint(dc);
        }
        ::EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_LBUTTONDOWN:
        if (self) {
          ::SetFocus(hwnd);  // Keep Escape working after a click.
          self->Click({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
        }
        return 0;
      case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
          PhiGroupOverview::Close();
          return 0;
        }
        break;
      case WM_KILLFOCUS:
        // Focus moved elsewhere (page click, another window): leave the
        // overview — matching the Mac's dismissal.
        PhiGroupOverview::Close();
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, w, l);
  }

  RAW_PTR_EXCLUSION PhiWindowController* const controller_;
  const std::string group_id_;
  UINT dpi_ = 96;
  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;
  std::vector<Card> cards_;
};

Overview* Overview::instance = nullptr;

}  // namespace

// static
bool PhiGroupOverview::IsOpen() {
  return Overview::instance != nullptr;
}

// static
void PhiGroupOverview::Open(PhiWindowController* controller,
                            const std::string& group_id) {
  Close();
  if (controller) {
    Overview::instance = new Overview(controller, group_id);
  }
}

// static
void PhiGroupOverview::Close() {
  if (Overview* gone = Overview::instance) {
    Overview::instance = nullptr;  // Cleared FIRST: ~Overview refires focus.
    delete gone;
  }
}

}  // namespace phi_client
