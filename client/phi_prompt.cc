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

std::wstring PhiFormatChord(const PhiShortcutChord& chord) {
  if (!chord.vk) {
    return L"\u2014";
  }
  std::wstring text;
  if (chord.ctrl) {
    text += L"Ctrl+";
  }
  if (chord.alt) {
    text += L"Alt+";
  }
  if (chord.shift) {
    text += L"Shift+";
  }
  const UINT vk = chord.vk;
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
    text += static_cast<wchar_t>(vk);
    return text;
  }
  if (vk >= VK_F1 && vk <= VK_F24) {
    text += L"F" + std::to_wstring(vk - VK_F1 + 1);
    return text;
  }
  switch (vk) {
    case VK_LEFT:   return text + L"Left";
    case VK_RIGHT:  return text + L"Right";
    case VK_UP:     return text + L"Up";
    case VK_DOWN:   return text + L"Down";
    case VK_HOME:   return text + L"Home";
    case VK_END:    return text + L"End";
    case VK_PRIOR:  return text + L"PageUp";
    case VK_NEXT:   return text + L"PageDown";
    case VK_SPACE:  return text + L"Space";
    case VK_RETURN: return text + L"Enter";
    case VK_BACK:   return text + L"Backspace";
    case VK_DELETE: return text + L"Delete";
    case VK_INSERT: return text + L"Insert";
    case VK_TAB:    return text + L"Tab";
  }
  // Punctuation and everything else — the layout's character for the key.
  const UINT ch = ::MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0xFFFF;
  if (ch) {
    text += static_cast<wchar_t>(ch);
  } else {
    text += L"Key" + std::to_wstring(vk);
  }
  return text;
}

bool PhiPromptShortcut(HWND owner,
                       const wchar_t* title,
                       const wchar_t* message,
                       PhiShortcutChord* out) {
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
  const int width = scaled(360);
  const int height = scaled(160);

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
  // The live chord preview reuses PromptState.edit as its "the control" slot.
  state.edit = child(L"STATIC", L"Type the new shortcut\u2026",
                     WS_BORDER | SS_CENTER | SS_CENTERIMAGE, 16, 42, 320, 30,
                     0);
  child(L"BUTTON", L"Cancel", WS_TABSTOP, 260, 90, 76, 26, IDCANCEL);

  if (owner) {
    ::EnableWindow(owner, FALSE);
  }
  ::ShowWindow(dialog, SW_SHOW);
  ::SetForegroundWindow(dialog);
  ::SetFocus(dialog);

  const auto is_modifier = [](UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN;
  };
  const auto held_prefix = [] {
    std::wstring text;
    if (::GetKeyState(VK_CONTROL) & 0x8000) {
      text += L"Ctrl+";
    }
    if (::GetKeyState(VK_MENU) & 0x8000) {
      text += L"Alt+";
    }
    if (::GetKeyState(VK_SHIFT) & 0x8000) {
      text += L"Shift+";
    }
    return text.empty() ? std::wstring(L"Type the new shortcut\u2026")
                        : text + L"\u2026";
  };

  PhiShortcutChord captured;
  MSG msg;
  while (!state.done && ::GetMessageW(&msg, nullptr, 0, 0)) {
    // The recorder itself: every key in the modal loop is a candidate.
    // WM_SYSKEYDOWN carries the Alt chords; consuming it also keeps
    // DefWindowProc out of the menu loop.
    if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
      const UINT vk = static_cast<UINT>(msg.wParam);
      const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
      const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
      if (is_modifier(vk)) {
        ::SetWindowTextW(state.edit, held_prefix().c_str());
        continue;
      }
      if (vk == VK_ESCAPE && !ctrl && !alt) {
        state.done = true;
        continue;
      }
      if (ctrl || alt || (vk >= VK_F1 && vk <= VK_F24)) {
        captured = {vk, ctrl, shift, alt};
        state.accepted = true;
        state.done = true;
        continue;
      }
      // A bare key: browsers require a modifier so pages keep their typing.
      ::SetWindowTextW(state.edit, L"Include Ctrl or Alt");
      continue;
    }
    if (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) {
      if (is_modifier(static_cast<UINT>(msg.wParam))) {
        ::SetWindowTextW(state.edit, held_prefix().c_str());
      }
      continue;
    }
    if (!::IsDialogMessageW(dialog, &msg)) {
      ::TranslateMessage(&msg);
      ::DispatchMessageW(&msg);
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
  if (!state.accepted) {
    return false;
  }
  if (out) {
    *out = captured;
  }
  return true;
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
  // The message may be several lines (a credential request states its
  // EXPOSURE on its own line, and that line must never be clipped): size the
  // message band to the text and push everything below it down.
  const std::wstring message_text(message ? message : L"");
  int message_lines =
      1 + static_cast<int>(std::count(message_text.begin(),
                                      message_text.end(), L'\n'));
  message_lines = std::min(message_lines, 8);
  const int message_height = message_lines * 16 + 6;
  const int list_top = 12 + message_height + 8;
  const int width = scaled(380);
  const int height = scaled(list_top + list_height + 88);

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
  child(L"STATIC", message, 0, 16, 12, 340, message_height, 0);
  // The list reuses PromptState.edit as its "the control" slot; LBS_NOTIFY's
  // double-click lands on IDOK below.
  state.edit = child(L"LISTBOX", L"",
                     WS_BORDER | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY, 16,
                     list_top, 340, list_height, 100);
  for (const std::wstring& option : options) {
    ::SendMessageW(state.edit, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(option.c_str()));
  }
  if (default_index >= 0 &&
      default_index < static_cast<int>(options.size())) {
    ::SendMessageW(state.edit, LB_SETCURSEL,
                   static_cast<WPARAM>(default_index), 0);
  }
  const int buttons_y = list_top + list_height + 12;
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
