// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_transcript_panel.h"

#include <commctrl.h>
#include <windowsx.h>

#include <memory>

#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"

namespace phi_client {

namespace {

constexpr wchar_t kPanelClass[] = L"PhiTranscriptPanel";
// The Mac floating panel's shape: 460×560, no smaller than 320×300.
constexpr int kPanelWidth = 460;
constexpr int kPanelHeight = 560;
constexpr int kPanelMinWidth = 320;
constexpr int kPanelMinHeight = 300;
constexpr int kInputRowHeight = 44;
constexpr int kSendWidth = 64;
constexpr int kPad = 8;

constexpr UINT_PTR kFeedId = 1;
constexpr UINT_PTR kInputId = 2;
constexpr UINT_PTR kSendId = 3;

class Panel {
 public:
  static Panel* instance;

  Panel(HWND owner,
        const PhiPalette& palette,
        const std::string& task_id,
        PhiTranscriptHost* host)
      : palette_(palette), task_id_(task_id), host_(host) {
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
        L"Transcript — " +
        base::UTF8ToWide(host_->TranscriptAgentName(task_id_));
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW, kPanelClass, title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, x, y, w, h,
        owner, nullptr, ::GetModuleHandleW(nullptr), this);
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
    if (bg_brush_) {
      ::DeleteObject(bg_brush_);
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
    // The feed reads best in a console face; the input row takes the UI one.
    feed_font_ = ::CreateFontW(-Scaled(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                               FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, L"Cascadia Mono");
    ui_font_ = ::CreateFontW(-Scaled(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                             FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Segoe UI");
    bg_brush_ = ::CreateSolidBrush(palette_.window_bg);
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
                   reinterpret_cast<LPARAM>(L"Message the agent…"));
    // Enter sends; the subclass forwards everything else to the EDIT.
    ::SetWindowLongPtrW(input_, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(this));
    input_base_proc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        input_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&InputProc)));
    Layout();
  }

  void Layout() {
    RECT client = {};
    ::GetClientRect(hwnd_, &client);
    const int pad = Scaled(kPad);
    const int row = Scaled(kInputRowHeight);
    const int send_w = Scaled(kSendWidth);
    ::MoveWindow(feed_, pad, pad, client.right - 2 * pad,
                 client.bottom - row - pad, TRUE);
    const int input_top = client.bottom - row + Scaled(6);
    const int input_h = row - Scaled(12);
    ::MoveWindow(input_, pad, input_top,
                 client.right - 3 * pad - send_w, input_h, TRUE);
    ::MoveWindow(send_, client.right - pad - send_w, input_top, send_w,
                 input_h, TRUE);
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
    switch (message) {
      case WM_SIZE:
        if (self) {
          self->Layout();
        }
        return 0;
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
      case WM_CTLCOLORSTATIC:  // The read-only feed EDIT arrives here.
      case WM_CTLCOLOREDIT:
        if (self) {
          HDC dc = reinterpret_cast<HDC>(w);
          ::SetTextColor(dc, self->palette_.label);
          ::SetBkColor(dc, self->palette_.window_bg);
          return reinterpret_cast<LRESULT>(self->bg_brush_);
        }
        break;
      case WM_ERASEBKGND:
        if (self) {
          RECT client = {};
          ::GetClientRect(hwnd, &client);
          ::FillRect(reinterpret_cast<HDC>(w), &client, self->bg_brush_);
          return 1;
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

  PhiPalette palette_;
  std::string task_id_;
  RAW_PTR_EXCLUSION PhiTranscriptHost* host_ = nullptr;
  HWND hwnd_ = nullptr;
  HWND feed_ = nullptr;
  HWND input_ = nullptr;
  HWND send_ = nullptr;
  WNDPROC input_base_proc_ = nullptr;
  HFONT feed_font_ = nullptr;
  HFONT ui_font_ = nullptr;
  HBRUSH bg_brush_ = nullptr;
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
