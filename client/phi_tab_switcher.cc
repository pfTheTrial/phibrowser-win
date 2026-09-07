// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_tab_switcher.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"
#include "phibrowser-win/client/phi_window_model.h"

namespace phi_client {

namespace {

// Card metrics (DIP) — the Mac TabSwitchMetrics' shape.
constexpr int kCardWidth = 192;
constexpr int kCardHeight = 150;
constexpr int kCardGap = 12;
constexpr int kPanelPad = 16;
constexpr int kFooterHeight = 28;
constexpr size_t kMaxCards = 7;

struct Card {
  TabId tab_id;
  std::wstring title;
  std::vector<unsigned char> thumbnail;
  int thumb_w = 0;
  int thumb_h = 0;
  std::vector<unsigned char> favicon;
  int fav_w = 0;
  int fav_h = 0;
};

class Hud {
 public:
  explicit Hud(PhiWindowController* controller) : controller_(controller) {
    dpi_ = ::GetDpiForWindow(controller_->shell_hwnd());
    Register();
    BuildCards();
    const int count =
        static_cast<int>(std::min(cards_.size(), kMaxCards));
    const int w = Scaled(kPanelPad) * 2 + count * Scaled(kCardWidth) +
                  std::max(0, count - 1) * Scaled(kCardGap);
    const int h = Scaled(kPanelPad) * 2 + Scaled(kCardHeight);
    RECT owner_rect = {};
    ::GetWindowRect(controller_->shell_hwnd(), &owner_rect);
    const int x = (owner_rect.left + owner_rect.right) / 2 - w / 2;
    const int y = (owner_rect.top + owner_rect.bottom) / 2 - h / 2;
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"PhiTabSwitch",
        L"", WS_POPUP, x, y, w, h, controller_->shell_hwnd(), nullptr,
        ::GetModuleHandleW(nullptr), nullptr);
    font_ = ::CreateFontW(-Scaled(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                          FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Segoe UI");
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }

  ~Hud() {
    if (font_) {
      ::DeleteObject(font_);
    }
    if (hwnd_ && ::IsWindow(hwnd_)) {
      ::DestroyWindow(hwnd_);
    }
  }

  void Advance(int direction) {
    if (cards_.empty()) {
      return;
    }
    const int count = static_cast<int>(cards_.size());
    selected_ = ((selected_ + direction) % count + count) % count;
    ::InvalidateRect(hwnd_, nullptr, TRUE);
  }

  void Commit() {
    if (selected_ >= 0 && selected_ < static_cast<int>(cards_.size())) {
      const Card& card = cards_[static_cast<size_t>(selected_)];
      phi_framework::PhiFramework* framework = controller_->framework();
      const BrowserId browser = controller_->active_browser_id();
      if (framework && browser.valid() && card.tab_id.valid()) {
        framework->ActivateTabById(browser.value, card.tab_id.value);
        // The framework's push refreshes the sidebar; nothing to do here.
      }
    }
  }

  static Hud* instance;

  PhiWindowController* controller() const { return controller_; }

 private:
  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }

  void Register() {
    static bool registered = [] {
      WNDCLASSEXW wc = {};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = &HudProc;
      wc.hInstance = ::GetModuleHandleW(nullptr);
      wc.lpszClassName = L"PhiTabSwitch";
      return ::RegisterClassExW(&wc) != 0;
    }();
    (void)registered;
  }

  // MRU-ordered cards: the controller's MRU list first, then any tabs it
  // has not seen active yet, in strip order — the strip from the window
  // model, never a parse of the framework's JSON.
  void BuildCards() {
    phi_framework::PhiFramework* framework = controller_->framework();
    const PhiWindowModel* model = controller_->model();
    const BrowserId browser = controller_->active_browser_id();
    if (!framework || !model || !browser.valid()) {
      return;  // No browser attached: an empty switcher.
    }
    struct Entry {
      TabId id;
      std::wstring title;
    };
    std::vector<Entry> entries;
    if (const phi_framework::WindowSnapshot* snapshot = model->Get(browser)) {
      for (const phi_framework::TabSnapshot& tab : snapshot->tabs) {
        entries.push_back({tab.id, base::UTF8ToWide(tab.title)});
      }
    }
    std::vector<Entry> ordered;
    for (const TabId id : controller_->TabMruOrder()) {
      for (const Entry& entry : entries) {
        if (entry.id == id) {
          ordered.push_back(entry);
          break;
        }
      }
    }
    for (const Entry& entry : entries) {
      if (std::none_of(ordered.begin(), ordered.end(),
                       [&](const Entry& seen) {
                         return seen.id == entry.id;
                       })) {
        ordered.push_back(entry);
      }
    }
    for (const Entry& entry : ordered) {
      if (cards_.size() >= kMaxCards) {
        break;
      }
      Card card;
      card.tab_id = entry.id;
      card.title = entry.title;
      int w = 0, h = 0;
      if (const unsigned char* pixels = framework->TabThumbnailBgra(
              browser.value, entry.id.value, &w, &h)) {
        const auto copy = UNSAFE_BUFFERS(base::span<const unsigned char>(
            pixels, static_cast<size_t>(w) * h * 4));
        card.thumbnail.assign(copy.begin(), copy.end());
        card.thumb_w = w;
        card.thumb_h = h;
      }
      if (const unsigned char* pixels = framework->TabFaviconBgraById(
              browser.value, entry.id.value, &w, &h)) {
        const auto copy = UNSAFE_BUFFERS(base::span<const unsigned char>(
            pixels, static_cast<size_t>(w) * h * 4));
        card.favicon.assign(copy.begin(), copy.end());
        card.fav_w = w;
        card.fav_h = h;
      }
      cards_.push_back(std::move(card));
    }
    // The gesture means "back to the previous tab": start on entry #2.
    selected_ = cards_.size() > 1 ? 1 : 0;
  }

  void Paint(HDC dc) {
    RECT rc = {};
    ::GetClientRect(hwnd_, &rc);
    const PhiPalette palette = controller_->CurrentPalette();
    PhiFillRoundRectAA(dc, rc, Scaled(16), palette.card);
    PhiStrokeRoundRectAA(dc, rc, Scaled(16), palette.border, 1);
    ::SetBkMode(dc, TRANSPARENT);
    int x = rc.left + Scaled(kPanelPad);
    for (size_t i = 0; i < cards_.size() && i < kMaxCards; ++i) {
      const Card& card = cards_[i];
      RECT card_rc = {x, rc.top + Scaled(kPanelPad),
                      x + Scaled(kCardWidth),
                      rc.top + Scaled(kPanelPad) + Scaled(kCardHeight)};
      if (static_cast<int>(i) == selected_) {
        RECT halo = card_rc;
        ::InflateRect(&halo, Scaled(4), Scaled(4));
        PhiFillRoundRectAA(dc, halo, Scaled(10), palette.hover_strong);
        PhiStrokeRoundRectAA(dc, halo, Scaled(10), palette.accent,
                             Scaled(2));
      }
      // Snapshot area (16:10-ish) with the footer strip below it.
      RECT shot = card_rc;
      shot.bottom -= Scaled(kFooterHeight);
      if (!card.thumbnail.empty()) {
        PhiDrawBgra(dc, shot, card.thumbnail.data(), card.thumb_w,
                    card.thumb_h);
      } else {
        HBRUSH ground = ::CreateSolidBrush(palette.hover);
        ::FillRect(dc, &shot, ground);
        ::DeleteObject(ground);
      }
      RECT footer = card_rc;
      footer.top = shot.bottom + Scaled(4);
      RECT icon_rc = {footer.left + Scaled(6), footer.top,
                      footer.left + Scaled(22), footer.bottom - Scaled(4)};
      if (!card.favicon.empty()) {
        PhiBlendBgra(dc, icon_rc, card.favicon.data(), card.fav_w,
                     card.fav_h);
      }
      RECT title_rc = {icon_rc.right + Scaled(6), footer.top,
                       footer.right - Scaled(6), footer.bottom};
      HGDIOBJ prev_font = font_ ? ::SelectObject(dc, font_) : nullptr;
      ::SetTextColor(dc, palette.label);
      ::DrawTextW(dc, card.title.c_str(), -1, &title_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      if (prev_font) {
        ::SelectObject(dc, prev_font);
      }
      x += Scaled(kCardWidth) + Scaled(kCardGap);
    }
  }

  static LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    Hud* self = instance && instance->hwnd_ == hwnd ? instance : nullptr;
    switch (msg) {
      case WM_NCHITTEST:
        return HTTRANSPARENT;
      case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = ::BeginPaint(hwnd, &ps);
        if (self) {
          self->Paint(dc);
        }
        ::EndPaint(hwnd, &ps);
        return 0;
      }
    }
    return ::DefWindowProcW(hwnd, msg, w, l);
  }

  RAW_PTR_EXCLUSION PhiWindowController* const controller_;
  UINT dpi_ = 96;
  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;
  std::vector<Card> cards_;
  int selected_ = 0;
};

Hud* Hud::instance = nullptr;

}  // namespace

// static
bool PhiTabSwitcher::IsActive() {
  return Hud::instance != nullptr;
}

// static
void PhiTabSwitcher::Cycle(PhiWindowController* controller, int direction) {
  if (!Hud::instance) {
    if (!controller) {
      return;
    }
    Hud::instance = new Hud(controller);
    return;  // Opening already selects the previous tab.
  }
  Hud::instance->Advance(direction);
}

// static
void PhiTabSwitcher::Commit() {
  if (!Hud::instance) {
    return;
  }
  Hud::instance->Commit();
  delete Hud::instance;
  Hud::instance = nullptr;
}

// static
void PhiTabSwitcher::Cancel() {
  delete Hud::instance;
  Hud::instance = nullptr;
}

// static
void PhiTabSwitcher::CancelIfBoundTo(const PhiWindowController* controller) {
  if (Hud::instance && Hud::instance->controller() == controller) {
    Cancel();
  }
}

}  // namespace phi_client
