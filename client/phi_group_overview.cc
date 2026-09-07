// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_group_overview.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"
#include "phibrowser-win/client/phi_window_model.h"

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
  TabId tab_id;
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
    hwnd_ = ::CreateWindowExW(0, L"PhiGroupOverview", L"",
                              // Keeps the card's corner caps visible.
                              WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0,
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

  PhiWindowController* controller() const { return controller_; }

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

  // The group's members from the window model — the same snapshot the
  // sidebar's rows come from, never a parse of the framework's JSON.
  void BuildCards() {
    cards_.clear();
    phi_framework::PhiFramework* framework = controller_->framework();
    const PhiWindowModel* model = controller_->model();
    const BrowserId browser = controller_->active_browser_id();
    const phi_framework::WindowSnapshot* snapshot =
        framework && model && browser.valid() ? model->Get(browser) : nullptr;
    if (snapshot) {
      for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
        if (tab.group_id != group_id_) {
          continue;
        }
        Card card;
        card.tab_id = tab.id;
        card.title = base::UTF8ToWide(tab.title);
        int w = 0, h = 0;
        if (const unsigned char* pixels = framework->TabThumbnailBgra(
                browser.value, tab.id.value, &w, &h)) {
          const auto copy = UNSAFE_BUFFERS(base::span<const unsigned char>(
              pixels, static_cast<size_t>(w) * h * 4));
          card.thumbnail.assign(copy.begin(), copy.end());
          card.thumb_w = w;
          card.thumb_h = h;
        }
        if (const unsigned char* pixels = framework->TabFaviconBgraById(
                browser.value, tab.id.value, &w, &h)) {
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
        PhiFillRoundRectAA(dc, card.bounds, Scaled(10), palette.hover);
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
    const BrowserId browser = controller_->active_browser_id();
    if (!browser.valid()) {
      return;  // No browser attached: nothing to activate or close.
    }
    for (size_t i = 0; i < cards_.size(); ++i) {
      const Card& card = cards_[i];
      if (!::PtInRect(&card.bounds, pt)) {
        continue;
      }
      if (card.add_card) {
        if (framework) {
          // OpenTab answers with the stable id the group call wants.
          const TabId fresh = framework->OpenTab(browser, "");
          if (fresh.valid()) {
            const std::string ids =
                "[" + base::NumberToString(fresh.value) + "]";
            framework->AddTabsToGroup(browser.value, group_id_.c_str(),
                                      ids.c_str());
          }
        }
        PhiGroupOverview::Close();
        return;
      }
      if (::PtInRect(&card.close_box, pt)) {
        if (framework && card.tab_id.valid()) {
          framework->CloseTabById(browser.value, card.tab_id.value);
        }
        // The card leaves in place: the model hears of the close on its
        // push, but the overview is the user's view of it right now.
        cards_.erase(cards_.begin() + static_cast<std::ptrdiff_t>(i));
        LayoutCards();
        ::InvalidateRect(hwnd_, nullptr, TRUE);
        return;
      }
      if (framework && card.tab_id.valid()) {
        framework->ActivateTabById(browser.value, card.tab_id.value);
      }
      PhiGroupOverview::Close();
      return;
    }
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
void PhiGroupOverview::CloseIfBoundTo(const PhiWindowController* controller) {
  if (Overview::instance && Overview::instance->controller() == controller) {
    Close();
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
