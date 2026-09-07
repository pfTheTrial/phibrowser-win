// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_toast.h"

#include "phibrowser-win/client/phi_dwrite.h"

#include <algorithm>

namespace phi_client {

namespace {

constexpr UINT_PTR kDismissTimer = 1;
constexpr UINT_PTR kFadeTimer = 2;
constexpr int kFadeStepMs = 30;
constexpr BYTE kFadeStep = 32;

struct Toast {
  HWND hwnd = nullptr;
  HFONT title_font = nullptr;
  HFONT message_font = nullptr;
  std::wstring title;
  std::wstring message;
  PhiPalette palette;
  BYTE alpha = 0;
  bool fading_out = false;
  UINT dpi = 96;

  int Scaled(int design) const { return ::MulDiv(design, dpi, 96); }
};

Toast* g_toast = nullptr;

void DestroyToast() {
  if (g_toast && ::IsWindow(g_toast->hwnd)) {
    ::DestroyWindow(g_toast->hwnd);
  }
}

LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  Toast* self = g_toast && g_toast->hwnd == hwnd ? g_toast : nullptr;
  switch (msg) {
    case WM_NCHITTEST:
      return HTTRANSPARENT;  // Clicks belong to whatever is under it.
    case WM_TIMER:
      if (!self) {
        break;
      }
      if (w == kDismissTimer) {
        ::KillTimer(hwnd, kDismissTimer);
        self->fading_out = true;
        ::SetTimer(hwnd, kFadeTimer, kFadeStepMs, nullptr);
        return 0;
      }
      if (w == kFadeTimer) {
        if (self->fading_out) {
          if (self->alpha <= kFadeStep) {
            ::KillTimer(hwnd, kFadeTimer);
            ::DestroyWindow(hwnd);
            return 0;
          }
          self->alpha = static_cast<BYTE>(self->alpha - kFadeStep);
        } else if (self->alpha >= 255 - kFadeStep) {
          self->alpha = 255;
          ::KillTimer(hwnd, kFadeTimer);
        } else {
          self->alpha = static_cast<BYTE>(self->alpha + kFadeStep);
        }
        ::SetLayeredWindowAttributes(hwnd, 0, self->alpha, LWA_ALPHA);
        return 0;
      }
      break;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        PhiFillRoundRectAA(dc, rc, self->Scaled(12), self->palette.card);
        PhiStrokeRoundRectAA(dc, rc, self->Scaled(12), self->palette.border,
                             1);
        ::SetBkMode(dc, TRANSPARENT);
        RECT text = rc;
        text.left += self->Scaled(16);
        text.right -= self->Scaled(16);
        if (self->message.empty()) {
          ::SelectObject(dc, self->title_font);
          ::SetTextColor(dc, self->palette.label);
          ::DrawTextW(dc, self->title.c_str(), -1, &text,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
          RECT top = text;
          top.bottom = (rc.top + rc.bottom) / 2;
          ::SelectObject(dc, self->title_font);
          ::SetTextColor(dc, self->palette.label);
          ::DrawTextW(dc, self->title.c_str(), -1, &top,
                      DT_CENTER | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
          RECT bottom = text;
          bottom.top = top.bottom + self->Scaled(2);
          ::SelectObject(dc, self->message_font);
          ::SetTextColor(dc, self->palette.label_dim);
          ::DrawTextW(dc, self->message.c_str(), -1, &bottom,
                      DT_CENTER | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        }
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      if (self) {
        if (self->title_font) {
          ::DeleteObject(self->title_font);
        }
        if (self->message_font) {
          ::DeleteObject(self->message_font);
        }
        delete self;
        g_toast = nullptr;
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

ATOM EnsureToastClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ToastProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhiToast";
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

}  // namespace

void PhiShowToast(HWND owner,
                  const PhiPalette& palette,
                  const std::wstring& title,
                  const std::wstring& message,
                  int duration_ms) {
  if (!owner || !::IsWindow(owner)) {
    return;
  }
  DestroyToast();  // Replacement, not queueing (v1).
  EnsureToastClass();
  Toast* toast = new Toast();
  toast->title = title;
  toast->message = message;
  toast->palette = palette;
  toast->dpi = ::GetDpiForWindow(owner);
  const auto make_font = [&](int size, int weight) {
    return ::CreateFontW(-toast->Scaled(size), 0, 0, 0, weight, FALSE, FALSE,
                         FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, L"Segoe UI");
  };
  toast->title_font = make_font(13, FW_SEMIBOLD);
  toast->message_font = make_font(12, FW_NORMAL);

  // Size to the text, top-center of the owner (the Mac's topCenter slot).
  HDC probe = ::GetDC(nullptr);
  HGDIOBJ prev = ::SelectObject(probe, toast->title_font);
  SIZE title_size = {};
  ::GetTextExtentPoint32W(probe, title.c_str(),
                          static_cast<int>(title.size()), &title_size);
  SIZE message_size = {};
  if (!message.empty()) {
    ::SelectObject(probe, toast->message_font);
    ::GetTextExtentPoint32W(probe, message.c_str(),
                            static_cast<int>(message.size()), &message_size);
  }
  ::SelectObject(probe, prev);
  ::ReleaseDC(nullptr, probe);

  RECT owner_rect = {};
  ::GetWindowRect(owner, &owner_rect);
  const int width =
      std::max<int>(std::max(title_size.cx, message_size.cx), 1) +
      toast->Scaled(48);
  const int height =
      toast->Scaled(message.empty() ? 40 : 56);
  const int x = (owner_rect.left + owner_rect.right) / 2 - width / 2;
  const int y = owner_rect.top + toast->Scaled(56);
  toast->hwnd = ::CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST,
      L"PhiToast", L"", WS_POPUP, x, y, width, height, owner, nullptr,
      ::GetModuleHandleW(nullptr), nullptr);
  if (!toast->hwnd) {
    delete toast;
    return;
  }
  g_toast = toast;
  ::SetLayeredWindowAttributes(toast->hwnd, 0, 0, LWA_ALPHA);
  ::ShowWindow(toast->hwnd, SW_SHOWNOACTIVATE);
  ::SetTimer(toast->hwnd, kFadeTimer, kFadeStepMs, nullptr);  // Fade in.
  ::SetTimer(toast->hwnd, kDismissTimer, std::max(500, duration_ms),
             nullptr);
}

}  // namespace phi_client
