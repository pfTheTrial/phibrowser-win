// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_toast.h"

#include <algorithm>
#include <string>

#include "phibrowser-win/client/phi_animator.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_popover.h"

namespace phi_client {

namespace {

// The fade the toast used to hand-roll with a 30 ms SetTimer and a
// 32-per-step alpha ramp; the same total durations, now on the shared
// animator's one coalescable 16 ms clock (lane 3 §6.3).
constexpr int kFadeInMs = 240;
constexpr int kFadeOutMs = 210;

// OverlayToastCenter's metrics, in DIP.
constexpr int kCornerRadius = 12;
constexpr int kTextInset = 16;
constexpr int kSideInset = 48;
constexpr int kHeightOneLine = 40;
constexpr int kHeightTwoLine = 56;
constexpr int kTopOffset = 56;
constexpr int kTitleSize = 13;
constexpr int kMessageSize = 12;

// The Mac queues toasts; replacement is still the simpler v1, so exactly
// one lives at a time.
class Toast;
Toast* g_toast = nullptr;

class Toast : public PhiPopover {
 public:
  Toast(std::wstring title, std::wstring message, const PhiPalette& palette)
      : title_(std::move(title)),
        message_(std::move(message)),
        pending_palette_(palette) {}

  ~Toast() override = default;

  bool Open(HWND owner, int duration_ms) {
    ClassSpec spec;
    spec.name = L"PhiToast";
    spec.cursor = nullptr;

    Options options;
    options.activate = false;
    options.foreground = false;
    options.topmost = true;
    options.layered = true;
    options.dwm_round = false;  // PhiFillRoundRectAA draws the corners.
    options.dismiss_on_inactive = false;
    options.dismiss_on_escape = false;
    options.click_through = true;  // Clicks belong to whatever is under it.

    // Created off-screen at a provisional size: the text has to be
    // measured with fonts() at the window's own dpi, which only exists
    // once the window does. Nothing is visible until Show().
    Placement placement;
    placement.side = PhiPopoverSide::kExplicit;
    placement.clamp = false;
    SIZE probe = {1, 1};
    if (!CreatePopover(spec, owner, probe, placement, options)) {
      return false;
    }
    g_toast = this;  // Before Show(), so a teardown cannot leave us stale.
    SetPalette(pending_palette_);
    ::SetLayeredWindowAttributes(hwnd(), 0, 0, LWA_ALPHA);

    const SIZE size = Measure();
    RECT owner_rect = {};
    ::GetWindowRect(owner, &owner_rect);
    const LONG x = (owner_rect.left + owner_rect.right) / 2 - size.cx / 2;
    const LONG y = owner_rect.top + Scaled(kTopOffset);
    placement.anchor = {x, y, x, y};
    Reposition(placement, size);
    Show();

    PhiAnimator::ForThread().Animate(
        hwnd(), kFadeInMs, PhiEasing::kLinear,
        [this](double t, RECT*) { SetAlpha(static_cast<BYTE>(t * 255.0)); });
    dismiss_ = NewTimer();
    dismiss_.Start(static_cast<UINT>(std::max(500, duration_ms)));
    return true;
  }

 protected:
  void Paint(HDC dc, const RECT&) override {
    RECT rc = {};
    ::GetClientRect(hwnd(), &rc);
    const PhiPalette& colors = palette();
    PhiFillRoundRectAA(dc, rc, Scaled(kCornerRadius), colors.card);
    PhiStrokeRoundRectAA(dc, rc, Scaled(kCornerRadius), colors.border, 1);
    ::SetBkMode(dc, TRANSPARENT);
    RECT text = rc;
    text.left += Scaled(kTextInset);
    text.right -= Scaled(kTextInset);
    if (message_.empty()) {
      ::SelectObject(dc, TitleFont());
      ::SetTextColor(dc, colors.label);
      ::DrawTextW(dc, title_.c_str(), -1, &text,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      return;
    }
    RECT top = text;
    top.bottom = (rc.top + rc.bottom) / 2;
    ::SelectObject(dc, TitleFont());
    ::SetTextColor(dc, colors.label);
    ::DrawTextW(dc, title_.c_str(), -1, &top,
                DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    RECT bottom = text;
    bottom.top = top.bottom + Scaled(2);
    ::SelectObject(dc, MessageFont());
    ::SetTextColor(dc, colors.label_dim);
    ::DrawTextW(dc, message_.c_str(), -1, &bottom,
                DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
  }

  void OnTimerFired(UINT_PTR id) override {
    if (id != dismiss_.id() || fading_out_) {
      return;
    }
    fading_out_ = true;
    dismiss_.Stop();
    PhiAnimator::ForThread().Animate(
        hwnd(), kFadeOutMs, PhiEasing::kLinear,
        [this](double t, RECT*) {
          SetAlpha(static_cast<BYTE>((1.0 - t) * 255.0));
        },
        [this] { Destroy(); });
  }

  void OnDpiChanged(UINT) override {
    const SIZE size = Measure();
    ::SetWindowPos(hwnd(), nullptr, 0, 0, size.cx, size.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  void OnNcDestroy() override {
    if (g_toast == this) {
      g_toast = nullptr;
    }
  }

 private:
  HFONT TitleFont() { return fonts().Get(kTitleSize, FW_SEMIBOLD); }
  HFONT MessageFont() { return fonts().Get(kMessageSize, FW_NORMAL); }

  void SetAlpha(BYTE alpha) {
    if (alive()) {
      ::SetLayeredWindowAttributes(hwnd(), 0, alpha, LWA_ALPHA);
    }
  }

  // Size to the text, the Mac's topCenter slot.
  SIZE Measure() {
    HDC probe = ::GetDC(nullptr);
    SIZE title_size = {};
    SIZE message_size = {};
    if (probe) {
      HGDIOBJ prev = ::SelectObject(probe, TitleFont());
      ::GetTextExtentPoint32W(probe, title_.c_str(),
                              static_cast<int>(title_.size()), &title_size);
      if (!message_.empty()) {
        ::SelectObject(probe, MessageFont());
        ::GetTextExtentPoint32W(probe, message_.c_str(),
                                static_cast<int>(message_.size()),
                                &message_size);
      }
      ::SelectObject(probe, prev);
      ::ReleaseDC(nullptr, probe);
    }
    SIZE size = {};
    size.cx = std::max<LONG>(std::max(title_size.cx, message_size.cx), 1) +
              Scaled(kSideInset);
    size.cy = Scaled(message_.empty() ? kHeightOneLine : kHeightTwoLine);
    return size;
  }

  std::wstring title_;
  std::wstring message_;
  PhiPalette pending_palette_;
  Timer dismiss_;
  bool fading_out_ = false;
};

}  // namespace

void PhiShowToast(HWND owner,
                  const PhiPalette& palette,
                  const std::wstring& title,
                  const std::wstring& message,
                  int duration_ms) {
  if (!owner || !::IsWindow(owner)) {
    return;
  }
  if (g_toast) {
    g_toast->Destroy();  // Replacement, not queueing (v1).
    g_toast = nullptr;
  }
  Toast* toast = new Toast(title, message, palette);
  toast->DeleteWhenDestroyed();
  if (!toast->Open(owner, duration_ms)) {
    delete toast;
  }
}

}  // namespace phi_client
