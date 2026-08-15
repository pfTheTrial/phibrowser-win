// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_agent_overlay.h"

#include <windowsx.h>

#include <algorithm>
#include <memory>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

constexpr wchar_t kFogClass[] = L"PhiAgentFog";
constexpr wchar_t kPillClass[] = L"PhiAgentPill";
// The fog is a light wash, not a blackout: the user must still SEE what the
// agent is doing — that is the whole point of watch mode.
constexpr BYTE kFogAlpha = 46;
constexpr int kPillHeight = 44;
constexpr int kPillMaxWidth = 560;
constexpr int kPillBottomGap = 24;
constexpr int kButtonHeight = 26;

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

class Overlay {
 public:
  static Overlay* instance;

  Overlay(PhiWindowController* controller, const std::string& task_id)
      : controller_(controller), task_id_(task_id) {
    dpi_ = ::GetDpiForWindow(controller_->shell_hwnd());
    Register();
    fog_ = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_NOPARENTNOTIFY, kFogClass,
                             L"", WS_CHILD, 0, 0, 0, 0,
                             controller_->shell_hwnd(), nullptr,
                             ::GetModuleHandleW(nullptr), this);
    pill_ = ::CreateWindowExW(WS_EX_NOPARENTNOTIFY, kPillClass, L"", WS_CHILD,
                              0, 0, 0, 0, controller_->shell_hwnd(), nullptr,
                              ::GetModuleHandleW(nullptr), this);
    EnsureFonts();
  }

  ~Overlay() {
    for (HFONT font : {label_font_, caption_font_}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
    for (HWND window : {pill_, fog_}) {
      if (window && ::IsWindow(window)) {
        ::DestroyWindow(window);
      }
    }
  }

  const std::string& task_id() const { return task_id_; }
  PhiWindowController* controller() const { return controller_; }

  void Update(const std::wstring& agent_label,
              const std::wstring& caption,
              bool agent_owns) {
    agent_label_ = agent_label;
    caption_ = caption;
    agent_owns_ = agent_owns;
    Reposition();
    // The fog only exists while the AGENT owns the Space; once the user takes
    // control the page must be usable, so the barrier comes down entirely.
    ::ShowWindow(fog_, agent_owns_ ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (agent_owns_) {
      ::SetLayeredWindowAttributes(fog_, 0, kFogAlpha, LWA_ALPHA);
    }
    ::ShowWindow(pill_, SW_SHOWNOACTIVATE);
    ::InvalidateRect(pill_, nullptr, TRUE);
  }

  void Reposition() {
    if (!controller_ || !::IsWindow(controller_->shell_hwnd())) {
      return;
    }
    const RECT content = controller_->ContentRect();
    const int width = content.right - content.left;
    const int height = content.bottom - content.top;
    ::SetWindowPos(fog_, HWND_TOP, content.left, content.top, width, height,
                   SWP_NOACTIVATE);
    const int pill_w = std::min(Scaled(kPillMaxWidth), width - Scaled(32));
    const int pill_h = Scaled(kPillHeight);
    ::SetWindowPos(pill_, HWND_TOP,
                   content.left + (width - pill_w) / 2,
                   content.bottom - pill_h - Scaled(kPillBottomGap), pill_w,
                   pill_h, SWP_NOACTIVATE);
    // Shape the WINDOW to the pill: painting a rounded rect inside a square
    // window would leave its corners showing the unpainted background.
    if (HRGN region = ::CreateRoundRectRgn(0, 0, pill_w + 1, pill_h + 1,
                                           pill_h, pill_h)) {
      ::SetWindowRgn(pill_, region, TRUE);  // The window owns it now.
    }
    LayoutButtons(pill_w, pill_h);
  }

  bool agent_owns() const { return agent_owns_; }

 private:
  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }

  void Register() {
    static bool registered = [] {
      WNDCLASSEXW fog = {};
      fog.cbSize = sizeof(fog);
      fog.lpfnWndProc = &FogProc;
      fog.hInstance = ::GetModuleHandleW(nullptr);
      fog.hCursor = ::LoadCursorW(nullptr, IDC_NO);
      fog.lpszClassName = kFogClass;
      ::RegisterClassExW(&fog);
      WNDCLASSEXW pill = {};
      pill.cbSize = sizeof(pill);
      pill.lpfnWndProc = &PillProc;
      pill.hInstance = ::GetModuleHandleW(nullptr);
      pill.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
      pill.lpszClassName = kPillClass;
      ::RegisterClassExW(&pill);
      return true;
    }();
    (void)registered;
  }

  void EnsureFonts() {
    auto make = [&](int size, int weight) {
      return ::CreateFontW(-Scaled(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    };
    label_font_ = make(13, FW_SEMIBOLD);
    caption_font_ = make(12, FW_NORMAL);
  }

  void LayoutButtons(int pill_w, int pill_h) {
    const int mid = pill_h / 2;
    const int bh = Scaled(kButtonHeight);
    int right = pill_w - Scaled(10);
    if (!agent_owns_) {
      // User holds control: Finish trails, Hand back leads it.
      const int finish_w = Scaled(72);
      secondary_ = {right - finish_w, mid - bh / 2, right, mid + bh / 2};
      right = secondary_.left - Scaled(8);
    } else {
      secondary_ = RECT{};
    }
    const int primary_w = Scaled(agent_owns_ ? 104 : 92);
    primary_ = {right - primary_w, mid - bh / 2, right, mid + bh / 2};
  }

  void Paint(HDC dc) {
    RECT client = {};
    ::GetClientRect(pill_, &client);
    const PhiPalette& palette = controller_->CurrentPalette();
    const COLORREF ground = palette.dark ? RGB(0x2A, 0x2A, 0x2E)
                                         : RGB(0xFF, 0xFF, 0xFF);
    const COLORREF ink = palette.dark ? RGB(0xF2, 0xF2, 0xF4)
                                      : RGB(0x1A, 0x1A, 0x1C);
    const COLORREF dim = palette.dark ? RGB(0xA0, 0xA0, 0xA8)
                                      : RGB(0x6A, 0x6A, 0x72);
    FillRounded(dc, client, ground, (client.bottom - client.top) / 2);
    ::SetBkMode(dc, TRANSPARENT);

    // Leading: who is driving, then the status caption.
    RECT text = {Scaled(16), client.top, primary_.left - Scaled(12),
                 client.bottom};
    ::SelectObject(dc, label_font_);
    ::SetTextColor(dc, ink);
    RECT measured = text;
    ::DrawTextW(dc, agent_label_.c_str(), -1, &measured,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_CALCRECT);
    ::DrawTextW(dc, agent_label_.c_str(), -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT caption = {measured.right + Scaled(12), client.top,
                    primary_.left - Scaled(12), client.bottom};
    if (caption.right > caption.left) {
      ::SelectObject(dc, caption_font_);
      ::SetTextColor(dc, dim);
      ::DrawTextW(dc, caption_.c_str(), -1, &caption,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    }

    // The primary action carries the accent; the secondary stays quiet.
    FillRounded(dc, primary_, palette.accent, Scaled(6));
    ::SelectObject(dc, label_font_);
    ::SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    const wchar_t* primary_text = agent_owns_ ? L"Take control" : L"Hand back";
    ::DrawTextW(dc, primary_text, -1, &primary_,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (secondary_.right > secondary_.left) {
      FillRounded(dc, secondary_, palette.hover, Scaled(6));
      ::SetTextColor(dc, ink);
      ::DrawTextW(dc, L"Finish", -1, &secondary_,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }

  void OnPillClick(int x, int y) {
    const POINT pt = {x, y};
    PhiSlotHost* host = controller_ ? controller_->host() : nullptr;
    if (!host) {
      return;
    }
    // Copy what the handlers need: they can destroy this overlay.
    const std::string task = task_id_;
    if (::PtInRect(&primary_, pt)) {
      if (agent_owns_) {
        host->RequestAgentTakeControl(task);
      } else {
        host->RequestAgentHandBack(task);
      }
      return;
    }
    if (secondary_.right > secondary_.left && ::PtInRect(&secondary_, pt)) {
      host->RequestAgentFinish(task);
    }
  }

  static LRESULT CALLBACK FogProc(HWND hwnd, UINT message, WPARAM w,
                                  LPARAM l) {
    switch (message) {
      case WM_ERASEBKGND: {
        RECT client = {};
        ::GetClientRect(hwnd, &client);
        // A flat wash; WS_EX_LAYERED turns it into the fog.
        HBRUSH brush = ::CreateSolidBrush(RGB(0x10, 0x12, 0x18));
        ::FillRect(reinterpret_cast<HDC>(w), &client, brush);
        ::DeleteObject(brush);
        return 1;
      }
      // The barrier itself: content input dies here. MOUSEACTIVATE keeps the
      // click from even activating the browser child beneath.
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATEANDEAT;
      case WM_LBUTTONDOWN:
      case WM_LBUTTONUP:
      case WM_RBUTTONDOWN:
      case WM_RBUTTONUP:
      case WM_MBUTTONDOWN:
      case WM_MBUTTONUP:
      case WM_MOUSEWHEEL:
      case WM_MOUSEMOVE:
        return 0;
      case WM_SETCURSOR:
        ::SetCursor(::LoadCursorW(nullptr, IDC_NO));
        return TRUE;
    }
    return ::DefWindowProcW(hwnd, message, w, l);
  }

  static LRESULT CALLBACK PillProc(HWND hwnd, UINT message, WPARAM w,
                                   LPARAM l) {
    switch (message) {
      case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // Clickable without stealing page focus.
      case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = ::BeginPaint(hwnd, &paint);
        RECT client = {};
        ::GetClientRect(hwnd, &client);
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP bitmap =
            ::CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = ::SelectObject(mem, bitmap);
        if (Overlay::instance) {
          Overlay::instance->Paint(mem);
        }
        ::BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        ::SelectObject(mem, old);
        ::DeleteObject(bitmap);
        ::DeleteDC(mem);
        ::EndPaint(hwnd, &paint);
        return 0;
      }
      case WM_LBUTTONUP:
        if (Overlay::instance) {
          Overlay::instance->OnPillClick(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        }
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, w, l);
  }

  RAW_PTR_EXCLUSION PhiWindowController* controller_;
  std::string task_id_;
  std::wstring agent_label_;
  std::wstring caption_;
  bool agent_owns_ = true;
  HWND fog_ = nullptr;
  HWND pill_ = nullptr;
  UINT dpi_ = 96;
  RECT primary_{};
  RECT secondary_{};
  HFONT label_font_ = nullptr;
  HFONT caption_font_ = nullptr;
};

Overlay* Overlay::instance = nullptr;

}  // namespace

// static
void PhiAgentOverlay::Show(PhiWindowController* controller,
                           const std::string& task_id,
                           const std::wstring& agent_label,
                           const std::wstring& caption,
                           bool agent_owns) {
  if (!controller || !controller->shell_hwnd()) {
    return;
  }
  if (Overlay::instance && (Overlay::instance->controller() != controller ||
                            Overlay::instance->task_id() != task_id)) {
    Hide();  // Watching a different Space now.
  }
  if (!Overlay::instance) {
    Overlay::instance = new Overlay(controller, task_id);
  }
  Overlay::instance->Update(agent_label, caption, agent_owns);
}

// static
void PhiAgentOverlay::Hide() {
  Overlay* overlay = Overlay::instance;
  Overlay::instance = nullptr;
  delete overlay;
}

// static
bool PhiAgentOverlay::IsShown() {
  return Overlay::instance != nullptr;
}

// static
void PhiAgentOverlay::Reposition() {
  if (Overlay::instance) {
    Overlay::instance->Reposition();
  }
}

}  // namespace phi_client
