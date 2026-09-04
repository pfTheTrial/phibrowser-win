// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_transcript_panel.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_popup_chrome.h"

namespace phi_client {

namespace {

constexpr wchar_t kPanelClass[] = L"PhiTranscriptPanel";
// The Mac floating panel's shape: 460×560, no smaller than 320×300.
constexpr int kPanelWidth = 460;
constexpr int kPanelHeight = 560;
constexpr int kPanelMinWidth = 320;
constexpr int kPanelMinHeight = 300;
constexpr int kInputRowHeight = 44;
constexpr int kSendWidth = 72;
constexpr int kPad = 8;

constexpr UINT_PTR kFeedId = 1;
constexpr UINT_PTR kInputId = 2;
constexpr UINT_PTR kSendId = 3;

// The Mac AgentTranscriptPanel's own console palette — deliberately not
// the theme roles: a near-white / near-black ground, a slightly deeper
// chrome band, and text at 85% / 92%.
struct ConsolePalette {
  COLORREF background;
  COLORREF chrome;
  COLORREF text;
  COLORREF faint;
};

ConsolePalette ConsoleFor(bool dark) {
  if (dark) {
    return {RGB(19, 20, 26), RGB(31, 32, 40), RGB(235, 235, 235),
            RGB(89, 89, 89)};
  }
  return {RGB(246, 246, 249), RGB(233, 235, 238), RGB(38, 38, 38),
          RGB(173, 173, 173)};
}

class Panel {
 public:
  static Panel* instance;

  Panel(HWND owner,
        const PhiPalette& palette,
        const std::string& task_id,
        PhiTranscriptHost* host)
      : task_id_(task_id), host_(host) {
    // The window chrome follows the app appearance; the feed keeps the
    // console's own ground.
    colors_ = PhiPopupColorsFor(palette.dark, palette.accent);
    console_ = ConsoleFor(palette.dark);
    dpi_ = ::GetDpiForWindow(owner);
    Register();
    // Park beside the owner's top-right corner, clamped to the work area.
    RECT owner_rect = {};
    ::GetWindowRect(owner, &owner_rect);
    const int w = Scaled(kPanelWidth);
    const int h = Scaled(kPanelHeight);
    int x = owner_rect.right + Scaled(8);
    int y = owner_rect.top;
    HMONITOR monitor = ::MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof(info)};
    if (::GetMonitorInfoW(monitor, &info)) {
      if (x + w > info.rcWork.right) {
        x = std::max(static_cast<int>(info.rcWork.left),
                     static_cast<int>(owner_rect.right) - w);
        y = owner_rect.top + Scaled(40);
      }
      y = std::max(static_cast<int>(info.rcWork.top),
                   std::min(y, static_cast<int>(info.rcWork.bottom) - h));
    }
    const std::wstring title =
        L"Agent Transcript \u2014 " +
        base::UTF8ToWide(host_->TranscriptAgentName(task_id_));
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW, kPanelClass, title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX |
            WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        x, y, w, h, owner, nullptr, ::GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
      return;
    }
    // The Mac window is closable, miniaturizable and resizable: the full
    // Windows cluster.
    chrome_.Attach(hwnd_,
                   kPhiCaptionClose | kPhiCaptionMinimize |
                       kPhiCaptionMaximize,
                   colors_);
    EnsureFonts();
    CreateChildren();
    Refresh();
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }

  ~Panel() {
    for (HFONT font : {feed_font_, ui_font_}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
    for (HBRUSH brush : {bg_brush_, feed_brush_, card_brush_}) {
      if (brush) {
        ::DeleteObject(brush);
      }
    }
    if (hwnd_ && ::IsWindow(hwnd_)) {
      ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      ::DestroyWindow(hwnd_);
    }
  }

  const std::string& task_id() const { return task_id_; }

  void Refresh() {
    if (!feed_ || !host_) {
      return;
    }
    const std::vector<std::string> lines = host_->TranscriptLines(task_id_);
    if (lines.size() == last_line_count_) {
      return;  // Nothing new; leave the user's scroll/selection alone.
    }
    last_line_count_ = lines.size();
    std::wstring text;
    for (const std::string& line : lines) {
      text += base::UTF8ToWide(line);
      text += L"\r\n";
    }
    ::SetWindowTextW(feed_, text.c_str());
    // Follow the tail, the console way.
    const int length = ::GetWindowTextLengthW(feed_);
    ::SendMessageW(feed_, EM_SETSEL, length, length);
    ::SendMessageW(feed_, EM_SCROLLCARET, 0, 0);
  }

  void Send() {
    if (!input_ || !host_) {
      return;
    }
    const int length = ::GetWindowTextLengthW(input_);
    if (length <= 0) {
      return;
    }
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(input_, buffer.data(), length + 1);
    const std::string text = base::WideToUTF8(buffer.data());
    if (host_->PushTranscriptMessage(task_id_, text)) {
      ::SetWindowTextW(input_, L"");
      Refresh();
    }
  }

 private:
  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }

  void Register() {
    static bool registered = [] {
      WNDCLASSEXW panel = {};
      panel.cbSize = sizeof(panel);
      panel.lpfnWndProc = &PanelProc;
      panel.hInstance = ::GetModuleHandleW(nullptr);
      panel.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
      panel.lpszClassName = kPanelClass;
      ::RegisterClassExW(&panel);
      return true;
    }();
    (void)registered;
  }

  void EnsureFonts() {
    // The feed reads best in a console face (the Mac's monospaced 11.5);
    // the input row takes the UI one.
    feed_font_ = ::CreateFontW(-Scaled(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                               FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Cascadia Mono");
    ui_font_ = PhiPopupFont(dpi_, 13);
    bg_brush_ = ::CreateSolidBrush(colors_.bg);
    feed_brush_ = ::CreateSolidBrush(console_.background);
    card_brush_ = ::CreateSolidBrush(colors_.card);
  }

  void CreateChildren() {
    feed_ = ::CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(kFeedId),
        ::GetModuleHandleW(nullptr), nullptr);
    input_ = ::CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(kInputId),
        ::GetModuleHandleW(nullptr), nullptr);
    send_ = ::CreateWindowExW(
        0, L"BUTTON", L"Send", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0,
        10, 10, hwnd_, reinterpret_cast<HMENU>(kSendId),
        ::GetModuleHandleW(nullptr), nullptr);
    for (HWND child : {feed_, input_}) {
      ::SendMessageW(child, WM_SETFONT,
                     reinterpret_cast<WPARAM>(
                         child == feed_ ? feed_font_ : ui_font_),
                     TRUE);
    }
    ::SendMessageW(send_, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_),
                   TRUE);
    ::SendMessageW(input_, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"Message the agent\u2026"));
    PhiPopupStyleButton(send_, PhiPopupButtonRole::kPrimary);
    PhiPopupThemeControl(feed_, colors_.dark);
    // Enter sends; the subclass forwards everything else to the EDIT.
    ::SetWindowLongPtrW(input_, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(this));
    input_base_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        input_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&InputProc)));
    Layout();
  }

  // The prompt row's well (the input's drawn border) and the divider over
  // it, in client pixels.
  RECT InputWell() const {
    RECT client = {};
    ::GetClientRect(hwnd_, &client);
    const int pad = Scaled(kPad);
    const int row = Scaled(kInputRowHeight);
    const int send_w = Scaled(kSendWidth);
    const int top = client.bottom - row + Scaled(8);
    return {pad, top, client.right - 2 * pad - send_w, client.bottom - Scaled(8)};
  }

  void Layout() {
    RECT client = {};
    ::GetClientRect(hwnd_, &client);
    const int band = chrome_.CaptionHeight();
    const int pad = Scaled(kPad);
    const int row = Scaled(kInputRowHeight);
    const int send_w = Scaled(kSendWidth);
    // The feed fills from the band to the divider above the prompt row.
    ::MoveWindow(feed_, 0, band, client.right,
                 std::max<int>(0, client.bottom - row - band - 1), TRUE);
    const RECT well = InputWell();
    const int line = Scaled(20);
    ::MoveWindow(input_, well.left + Scaled(10),
                 (well.top + well.bottom - line) / 2,
                 std::max<int>(0, well.right - well.left - Scaled(20)), line,
                 TRUE);
    ::MoveWindow(send_, client.right - pad - send_w, well.top, send_w,
                 well.bottom - well.top, TRUE);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void Paint() {
    PAINTSTRUCT ps = {};
    HDC dc = ::BeginPaint(hwnd_, &ps);
    if (!dc) {
      return;
    }
    RECT client = {};
    ::GetClientRect(hwnd_, &client);
    ::FillRect(dc, &client, bg_brush_);
    chrome_.PaintCaption(dc, colors_, /*show_mark=*/true);
    // The feed's ground reaches the window edges; the divider (the Mac's
    // faint rule) closes it above the prompt row.
    const int row = Scaled(kInputRowHeight);
    RECT feed = {0, chrome_.CaptionHeight(), client.right,
                 client.bottom - row - 1};
    ::FillRect(dc, &feed, feed_brush_);
    RECT rule = {0, client.bottom - row - 1, client.right, client.bottom - row};
    HBRUSH rule_brush = ::CreateSolidBrush(console_.faint);
    ::FillRect(dc, &rule, rule_brush);
    ::DeleteObject(rule_brush);
    PhiPopupPaintWell(dc, InputWell(), colors_, dpi_);
    ::EndPaint(hwnd_, &ps);
  }

  static LRESULT CALLBACK InputProc(HWND hwnd, UINT message, WPARAM w,
                                    LPARAM l) {
    auto* self = reinterpret_cast<Panel*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
      if (message == WM_KEYDOWN && w == VK_RETURN) {
        self->Send();
        return 0;
      }
      if (message == WM_CHAR && w == VK_RETURN) {
        return 0;  // No beep for the handled key.
      }
      return ::CallWindowProcW(self->input_base_proc_, hwnd, message, w, l);
    }
    return ::DefWindowProcW(hwnd, message, w, l);
  }

  static LRESULT CALLBACK PanelProc(HWND hwnd, UINT message, WPARAM w,
                                    LPARAM l) {
    if (message == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      return ::DefWindowProcW(hwnd, message, w, l);
    }
    auto* self =
        reinterpret_cast<Panel*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    LRESULT chrome_result = 0;
    if (self && self->chrome_.HandleMessage(message, w, l, &chrome_result)) {
      return chrome_result;
    }
    switch (message) {
      case WM_SIZE:
        if (self) {
          self->Layout();
        }
        return 0;
      case WM_DPICHANGED:
        if (self) {
          self->dpi_ = HIWORD(w);
          const RECT* suggested = reinterpret_cast<const RECT*>(l);
          if (suggested) {
            ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                           suggested->right - suggested->left,
                           suggested->bottom - suggested->top,
                           SWP_NOZORDER | SWP_NOACTIVATE);
          }
          self->Layout();
          return 0;
        }
        break;
      case WM_GETMINMAXINFO:
        if (self) {
          auto* mmi = reinterpret_cast<MINMAXINFO*>(l);
          mmi->ptMinTrackSize.x = self->Scaled(kPanelMinWidth);
          mmi->ptMinTrackSize.y = self->Scaled(kPanelMinHeight);
          return 0;
        }
        break;
      case WM_COMMAND:
        if (self && LOWORD(w) == kSendId) {
          self->Send();
          return 0;
        }
        break;
      case WM_DRAWITEM:
        if (self &&
            PhiPopupDrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(l),
                               self->colors_, nullptr)) {
          return TRUE;
        }
        break;
      case WM_CTLCOLORSTATIC:  // The read-only feed EDIT arrives here.
        if (self) {
          HDC dc = reinterpret_cast<HDC>(w);
          ::SetTextColor(dc, self->console_.text);
          ::SetBkColor(dc, self->console_.background);
          return reinterpret_cast<LRESULT>(self->feed_brush_);
        }
        break;
      case WM_CTLCOLOREDIT:
        if (self) {
          HDC dc = reinterpret_cast<HDC>(w);
          ::SetTextColor(dc, self->colors_.label);
          ::SetBkColor(dc, self->colors_.card);
          return reinterpret_cast<LRESULT>(self->card_brush_);
        }
        break;
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
        if (self) {
          self->Paint();
          return 0;
        }
        break;
      case WM_CLOSE:
        if (self && Panel::instance == self) {
          Panel::instance = nullptr;
          delete self;
          return 0;
        }
        break;
    }
    return ::DefWindowProcW(hwnd, message, w, l);
  }

  PhiPopupColors colors_;
  ConsolePalette console_ = ConsoleFor(false);
  std::string task_id_;
  RAW_PTR_EXCLUSION PhiTranscriptHost* host_ = nullptr;
  HWND hwnd_ = nullptr;
  PhiPopupChrome chrome_;
  HWND feed_ = nullptr;
  HWND input_ = nullptr;
  HWND send_ = nullptr;
  WNDPROC input_base_proc_ = nullptr;
  HFONT feed_font_ = nullptr;
  HFONT ui_font_ = nullptr;
  HBRUSH bg_brush_ = nullptr;
  HBRUSH feed_brush_ = nullptr;
  HBRUSH card_brush_ = nullptr;
  size_t last_line_count_ = static_cast<size_t>(-1);
  UINT dpi_ = 96;
};

Panel* Panel::instance = nullptr;

}  // namespace

// static
void PhiTranscriptPanel::Toggle(HWND owner,
                                const PhiPalette& palette,
                                const std::string& task_id,
                                PhiTranscriptHost* host) {
  if (Panel::instance) {
    const bool same = Panel::instance->task_id() == task_id;
    Panel* panel = Panel::instance;
    Panel::instance = nullptr;
    delete panel;
    if (same) {
      return;  // The Mac toggle: reopening the shown task closes it.
    }
  }
  if (owner && host) {
    Panel::instance = new Panel(owner, palette, task_id, host);
  }
}

// static
void PhiTranscriptPanel::RefreshOpen() {
  if (Panel::instance) {
    Panel::instance->Refresh();
  }
}

// static
bool PhiTranscriptPanel::IsOpen() {
  return Panel::instance != nullptr;
}

// static
void PhiTranscriptPanel::CloseFor(const std::string& task_id) {
  if (Panel::instance && Panel::instance->task_id() == task_id) {
    Panel* panel = Panel::instance;
    Panel::instance = nullptr;
    delete panel;
  }
}

}  // namespace phi_client
