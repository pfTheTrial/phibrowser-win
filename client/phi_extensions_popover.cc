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
#include "phibrowser-win/client/phi_popover.h"

#pragma comment(lib, "dwmapi.lib")

namespace phi_client {

namespace {

constexpr int kPanelWidth = 300;
constexpr int kPanelMaxHeight = 380;
constexpr int kHeaderHeight = 40;
constexpr int kRowHeight = 40;
constexpr int kIconSlot = 20;
constexpr UINT kPollIntervalMs = 700;
// hits_ keys: row i is i * kKeyStride, its pin toggle is + 1. The base
// resolves hover and clicks over the list.
constexpr int kKeyStride = 2;

void FillRect(HDC dc, const RECT& r, COLORREF color) {
  HBRUSH brush = ::CreateSolidBrush(color);
  ::FillRect(dc, &r, brush);
  ::DeleteObject(brush);
}

void FillRounded(HDC dc, const RECT& r, COLORREF color, int radius) {
  PhiFillRoundRectAA(dc, r, radius, color);
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

// The popover keeps its extension rows, its Paint() and its pin/action
// handling. The window, the placement, the back buffer, hover, the poll
// timer, dismissal and lifetime belong to PhiPopover (lane 3 §6.2).
class Popover : public PhiPopover {
 public:
  static Popover* instance;

  Popover(phi_framework::PhiFramework* framework, BrowserId browser)
      : framework_(framework), browser_id_(browser.value) {}

  ~Popover() override = default;

  bool Open(HWND owner, const RECT& anchor, const PhiPalette& colors) {
    ClassSpec spec;
    spec.name = L"PhiExtensionsPopover";

    Options options;  // Activating, DWM-rounded, 500 ms opening grace.

    // Below the anchor when it sits in the owner's top half (the address
    // capsule's in-field button — the Mac popover drops downward there);
    // above it for the bottom bar. Clamped to the work area either way.
    RECT owner_rect = {};
    ::GetWindowRect(owner, &owner_rect);
    const bool anchored_high =
        anchor.top < (owner_rect.top + owner_rect.bottom) / 2;
    placement_.anchor = anchor;
    placement_.side = anchored_high ? PhiPopoverSide::kBelow
                                    : PhiPopoverSide::kAbove;
    placement_.gap_dip = 8;
    placement_.monitor_from_owner = true;

    SIZE probe = {1, 1};
    if (!CreatePopover(spec, owner, probe, placement_, options)) {
      return false;
    }
    instance = this;  // Before Show(): activation churn can destroy us.
    SetPalette(colors);
    Refresh();
    SIZE size = {Scaled(kPanelWidth), PanelHeight()};
    Reposition(placement_, size);
    BuildHits();  // Refresh laid out against the pre-placement size.
    Show();
    // The list loads asynchronously on first use; poll cheaply while open
    // so the panel fills even if the delegate event races the open.
    poll_ = NewTimer();
    poll_.Start(kPollIntervalMs);
    return true;
  }

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
    if (alive()) {
      // The row count changes the panel height (grown downward is wrong for
      // a bottom-bar popover — keep the BOTTOM edge pinned).
      RECT rect = {};
      ::GetWindowRect(hwnd(), &rect);
      const int h = PanelHeight();
      ::SetWindowPos(hwnd(), nullptr, rect.left, rect.bottom - h,
                     Scaled(kPanelWidth), h, SWP_NOZORDER | SWP_NOACTIVATE);
      BuildHits();
      Invalidate();
    }
  }

 protected:
  void Paint(HDC dc, const RECT&) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    PaintPanel(dc, client);
  }

  void OnTimerFired(UINT_PTR id) override {
    if (id == poll_.id()) {
      Refresh();
    }
  }

  void OnHover(int hit, int) override {
    hover_row_ = -1;
    hover_pin_ = false;
    if (hit < 0) {
      return;
    }
    const int key = hits_[static_cast<size_t>(hit)].key;
    hover_row_ = key / kKeyStride;
    hover_pin_ = (key % kKeyStride) != 0;
  }

  void OnClick(int hit, POINT) override {
    OnHover(hit, -1);
    RunHoveredAction();
  }

  void OnDpiChanged(UINT) override {
    RECT rect = {};
    ::GetWindowRect(hwnd(), &rect);
    const int h = PanelHeight();
    ::SetWindowPos(hwnd(), nullptr, rect.left, rect.bottom - h,
                   Scaled(kPanelWidth), h, SWP_NOZORDER | SWP_NOACTIVATE);
    BuildHits();
  }

  void OnNcDestroy() override {
    poll_.Stop();
    PhiPopover::RecordClose(owner());
    if (instance == this) {
      instance = nullptr;
    }
  }

 private:
  HFONT NameFont() { return fonts().Get(13, FW_NORMAL); }
  HFONT TitleFont() { return fonts().Get(13, FW_SEMIBOLD); }
  HFONT GlyphFont() {
    PhiFontDesc desc;
    desc.size = 12;
    desc.face = L"Segoe MDL2 Assets";
    return fonts().Get(desc);
  }
  HFONT EmptyFont() {
    PhiFontDesc desc;
    desc.size = 28;
    desc.face = L"Segoe MDL2 Assets";
    return fonts().Get(desc);
  }

  // THE layout pass: one hit per row plus one for its pin toggle, in
  // paint order so the toggle (added second) takes the pointer.
  void BuildHits() {
    ClearHits();
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    int y = Scaled(kHeaderHeight);
    for (size_t i = 0; i < rows_.size(); ++i) {
      RECT bounds = {Scaled(6), y, client.right - Scaled(6),
                     y + Scaled(kRowHeight)};
      rows_[i].bounds = bounds;
      const int mid = (bounds.top + bounds.bottom) / 2;
      const int pin_px = Scaled(22);
      rows_[i].pin_box = {bounds.right - Scaled(10) - pin_px, mid - pin_px / 2,
                          bounds.right - Scaled(10), mid + pin_px / 2};
      AddHit(bounds, static_cast<int>(i) * kKeyStride, rows_[i].name,
             PhiHitRole::kListItem);
      AddHit(rows_[i].pin_box, static_cast<int>(i) * kKeyStride + 1,
             rows_[i].pinned ? L"Unpin" : L"Pin", PhiHitRole::kToggle,
             rows_[i].pinned);
      y += Scaled(kRowHeight);
    }
  }

  int PanelHeight() const {
    const int rows = std::max(1, static_cast<int>(rows_.size()));
    return std::min(Scaled(kPanelMaxHeight),
                    Scaled(kHeaderHeight) + rows * Scaled(kRowHeight) +
                        Scaled(8));
  }

  void PaintPanel(HDC dc, const RECT& client) {
    const PhiPalette& p = palette();
    const COLORREF bg = p.ground_bottom;
    const COLORREF label = p.label;
    const COLORREF dim = p.label_dim;
    FillRect(dc, client, bg);
    ::SetBkMode(dc, TRANSPARENT);

    RECT title = {Scaled(16), 0, client.right - Scaled(16),
                  Scaled(kHeaderHeight)};
    ::SelectObject(dc, TitleFont());
    ::SetTextColor(dc, label);
    PhiDrawTextAA(dc, L"Extensions", -1, &title,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (rows_.empty()) {
      RECT glyph = {0, Scaled(kHeaderHeight), client.right,
                    Scaled(kHeaderHeight + 48)};
      ::SelectObject(dc, EmptyFont());
      ::SetTextColor(dc, dim);
      PhiDrawTextAA(dc, L"", -1, &glyph,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      RECT hint = {0, glyph.bottom, client.right,
                   glyph.bottom + Scaled(20)};
      ::SelectObject(dc, NameFont());
      PhiDrawTextAA(dc, L"No extensions installed", -1, &hint,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      return;
    }

    for (ExtensionRow& row : rows_) {
      const int index = static_cast<int>(&row - rows_.data());
      const int mid = (row.bounds.top + row.bounds.bottom) / 2;
      const bool hot = hover_row_ == index;
      if (hot) {
        FillRounded(dc, row.bounds, p.hover, Scaled(6));
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
        ::SelectObject(dc, GlyphFont());
        ::SetTextColor(dc, dim);
        PhiDrawTextAA(dc, L"", -1, &icon,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      // Pin toggle, trailing (the Mac popover's pin column).
      const bool pin_hot = hot && hover_pin_;
      if (pin_hot) {
        FillRounded(dc, row.pin_box, p.selected, Scaled(4));
      }
      ::SelectObject(dc, GlyphFont());
      ::SetTextColor(dc, row.pinned ? p.accent : dim);
      PhiDrawTextAA(dc, row.pinned ? L"" : L"", -1, &row.pin_box,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      // Name.
      RECT text = {icon.right + Scaled(10), row.bounds.top,
                   row.pin_box.left - Scaled(8), row.bounds.bottom};
      ::SelectObject(dc, NameFont());
      ::SetTextColor(dc, label);
      PhiDrawTextAA(dc, row.name.c_str(), -1, &text,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    }
  }

  void RunHoveredAction() {
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
      Invalidate();
      return;
    }
    // Run the action anchored to the ROW (screen coords) and leave — the
    // extension's own popup takes the stage.
    RECT anchor = row.bounds;
    ::MapWindowPoints(hwnd(), nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    framework_->TriggerExtensionAction(
        browser_id_, row.id.c_str(), anchor.left, anchor.top,
        anchor.right - anchor.left, anchor.bottom - anchor.top);
    Dismiss();
  }

  // Not rewritten to raw_ptr: the popover lives strictly inside the
  // framework's lifetime (owned popup of the shell window).
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_;
  // The browser the popover was opened for (BrowserId::value; the
  // framework calls below are still int64_t until phase 2).
  int64_t browser_id_;
  Placement placement_;
  Timer poll_;
  std::vector<ExtensionRow> rows_;
  std::string last_json_;
  int hover_row_ = -1;
  bool hover_pin_ = false;
};

Popover* Popover::instance = nullptr;

}  // namespace

void PhiExtensionsPopover::Toggle(HWND owner,
                                  const RECT& anchor_screen,
                                  const PhiPalette& palette,
                                  phi_framework::PhiFramework* framework,
                                  BrowserId browser) {
  if (Popover::instance) {
    Popover::instance->Dismiss();
    return;
  }
  if (PhiPopover::ShouldSuppressReopen(owner)) {
    return;
  }
  Popover* popover = new Popover(framework, browser);
  popover->DeleteWhenDestroyed();
  if (!popover->Open(owner, anchor_screen, palette)) {
    delete popover;
  }
}

void PhiExtensionsPopover::RefreshOpen() {
  if (Popover::instance) {
    Popover::instance->Refresh();
  }
}

}  // namespace phi_client
