// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_prompt.h"

#include <algorithm>

namespace phi_client {

namespace {

constexpr wchar_t kPromptClass[] = L"PhiPrompt";

struct PromptState {
  HWND edit = nullptr;
  bool accepted = false;
  bool done = false;
};

LRESULT CALLBACK PromptProc(HWND hwnd, UINT msg, WPARAM wparam,
                            LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* state = reinterpret_cast<PromptState*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_COMMAND:
      // IDOK / IDCANCEL, so IsDialogMessage's Enter and Escape handling
      // drives the same path as the buttons. The WM_NULL post wakes the
      // modal loop: a WM_COMMAND delivered via SendMessage from outside the
      // thread is processed INSIDE GetMessageW without making it return, so
      // `done` alone would leave the loop parked until unrelated input.
      if (state && LOWORD(wparam) == IDOK) {
        state->accepted = true;
        state->done = true;
        ::PostMessageW(hwnd, WM_NULL, 0, 0);
        return 0;
      }
      if (state && LOWORD(wparam) == IDCANCEL) {
        state->done = true;
        ::PostMessageW(hwnd, WM_NULL, 0, 0);
        return 0;
      }
      break;
    case WM_CLOSE:
      if (state) {
        state->done = true;
        ::PostMessageW(hwnd, WM_NULL, 0, 0);
        return 0;
      }
      break;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

bool PhiPromptText(HWND owner,
                   const wchar_t* title,
                   const wchar_t* message,
                   std::wstring* value) {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PromptProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kPromptClass;
    return ::RegisterClassExW(&wc);
  }();
  (void)atom;

  const UINT dpi = owner ? ::GetDpiForWindow(owner) : 96;
  auto scaled = [&](int design) { return ::MulDiv(design, static_cast<int>(dpi), 96); };
  const int width = scaled(360);
  const int height = scaled(150);

  PromptState state;
  RECT anchor = {0, 0, 0, 0};
  if (owner) {
    ::GetWindowRect(owner, &anchor);
  }
  const int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
  const int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 3;
  HWND dialog = ::CreateWindowExW(
      WS_EX_DLGMODALFRAME, kPromptClass, title,
      WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr,
      ::GetModuleHandleW(nullptr), &state);
  if (!dialog) {
    return false;
  }

  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  HFONT font = nullptr;
  if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                   &metrics, 0, dpi)) {
    font = ::CreateFontIndirectW(&metrics.lfMessageFont);
  }
  auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                   int cx, int cy, int cw, int ch, UINT_PTR id) {
    HWND control = ::CreateWindowExW(
        0, cls, text, WS_CHILD | WS_VISIBLE | style, scaled(cx), scaled(cy),
        scaled(cw), scaled(ch), dialog, reinterpret_cast<HMENU>(id),
        ::GetModuleHandleW(nullptr), nullptr);
    if (control && font) {
      ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                     TRUE);
    }
    return control;
  };
  child(L"STATIC", message, 0, 16, 12, 320, 20, 0);
  state.edit = child(L"EDIT", value ? value->c_str() : L"",
                     WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 16, 40, 320, 24,
                     0);
  child(L"BUTTON", L"OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 176, 80, 76, 26,
        IDOK);
  child(L"BUTTON", L"Cancel", WS_TABSTOP, 260, 80, 76, 26, IDCANCEL);

  if (owner) {
    ::EnableWindow(owner, FALSE);  // Modal against its owner, as NSAlert is.
  }
  ::ShowWindow(dialog, SW_SHOW);
  ::SetFocus(state.edit);
  ::SendMessageW(state.edit, EM_SETSEL, 0, -1);

  MSG msg;
  while (!state.done && ::GetMessageW(&msg, nullptr, 0, 0)) {
    if (!::IsDialogMessageW(dialog, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }

  std::wstring entered;
  if (state.accepted && state.edit) {
    const int length = ::GetWindowTextLengthW(state.edit);
    if (length > 0) {
      entered.resize(static_cast<size_t>(length) + 1, L'\0');
      ::GetWindowTextW(state.edit, entered.data(), length + 1);
      entered.resize(static_cast<size_t>(length));
    }
  }
  if (owner) {
    ::EnableWindow(owner, TRUE);
    ::SetForegroundWindow(owner);
  }
  ::DestroyWindow(dialog);
  if (font) {
    ::DeleteObject(font);
  }
  if (!state.accepted || entered.empty()) {
    return false;
  }
  if (value) {
    *value = entered;
  }
  return true;
}

bool PhiConfirm(HWND owner, const wchar_t* title, const wchar_t* message) {
  return ::MessageBoxW(owner, message, title,
                       MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) == IDOK;
}

int PhiPromptChoose(HWND owner,
                    const wchar_t* title,
                    const wchar_t* message,
                    const std::vector<std::wstring>& options,
                    int default_index) {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PromptProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kPromptClass;
    return ::RegisterClassExW(&wc);
  }();
  (void)atom;

  const UINT dpi = owner ? ::GetDpiForWindow(owner) : 96;
  auto scaled = [&](int design) {
    return ::MulDiv(design, static_cast<int>(dpi), 96);
  };
  // The list grows with the option count, within reason.
  const int list_height =
      std::min(static_cast<int>(options.size()), 8) * 20 + 6;
  const int width = scaled(380);
  const int height = scaled(96 + list_height + 44);

  PromptState state;
  RECT anchor = {0, 0, 0, 0};
  if (owner) {
    ::GetWindowRect(owner, &anchor);
  }
  const int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
  const int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 3;
  HWND dialog = ::CreateWindowExW(
      WS_EX_DLGMODALFRAME, kPromptClass, title,
      WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr,
      ::GetModuleHandleW(nullptr), &state);
  if (!dialog) {
    return -1;
  }

  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  HFONT font = nullptr;
  if (::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                   &metrics, 0, dpi)) {
    font = ::CreateFontIndirectW(&metrics.lfMessageFont);
  }
  auto child = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                   int cx, int cy, int cw, int ch, UINT_PTR id) {
    HWND control = ::CreateWindowExW(
        0, cls, text, WS_CHILD | WS_VISIBLE | style, scaled(cx), scaled(cy),
        scaled(cw), scaled(ch), dialog, reinterpret_cast<HMENU>(id),
        ::GetModuleHandleW(nullptr), nullptr);
    if (control && font) {
      ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                     TRUE);
    }
    return control;
  };
  child(L"STATIC", message, SS_ENDELLIPSIS, 16, 12, 340, 36, 0);
  // The list reuses PromptState.edit as its "the control" slot; LBS_NOTIFY's
  // double-click lands on IDOK below.
  state.edit = child(L"LISTBOX", L"",
                     WS_BORDER | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY, 16, 52,
                     340, list_height, 100);
  for (const std::wstring& option : options) {
    ::SendMessageW(state.edit, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(option.c_str()));
  }
  if (default_index >= 0 &&
      default_index < static_cast<int>(options.size())) {
    ::SendMessageW(state.edit, LB_SETCURSEL,
                   static_cast<WPARAM>(default_index), 0);
  }
  const int buttons_y = 52 + list_height + 12;
  child(L"BUTTON", L"Open", BS_DEFPUSHBUTTON | WS_TABSTOP, 196, buttons_y, 76,
        26, IDOK);
  child(L"BUTTON", L"Keep Here", WS_TABSTOP, 280, buttons_y, 76, 26,
        IDCANCEL);

  if (owner) {
    ::EnableWindow(owner, FALSE);
  }
  ::ShowWindow(dialog, SW_SHOW);
  ::SetForegroundWindow(dialog);
  ::SetFocus(state.edit);

  MSG msg;
  while (!state.done && ::GetMessageW(&msg, nullptr, 0, 0)) {
    // A double-click on a row is "Open" for that row.
    if (msg.message == WM_LBUTTONDBLCLK && msg.hwnd == state.edit) {
      state.accepted = true;
      state.done = true;
      continue;
    }
    if (!::IsDialogMessageW(dialog, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
    }
  }

  int chosen = -1;
  if (state.accepted && state.edit) {
    const LRESULT sel = ::SendMessageW(state.edit, LB_GETCURSEL, 0, 0);
    if (sel != LB_ERR) {
      chosen = static_cast<int>(sel);
    }
  }
  if (owner) {
    ::EnableWindow(owner, TRUE);
    ::SetForegroundWindow(owner);
  }
  ::DestroyWindow(dialog);
  if (font) {
    ::DeleteObject(font);
  }
  return chosen;
}

}  // namespace phi_client
