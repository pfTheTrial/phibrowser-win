// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_folder_icon_picker.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_mac_icons.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

// BookmarkFolderIconPicker's metrics, in DIP.
constexpr int kColumns = 6;
constexpr int kItemSize = 26;
constexpr int kIconSize = 20;
constexpr int kColumnSpacing = 16;
constexpr int kRowSpacing = 4;
constexpr int kGridWidth = 236;
constexpr int kPickerWidth = 264;
constexpr int kVerticalPad = 12;
constexpr int kItemRadius = 10;

const wchar_t kClassName[] = L"PhiFolderIconPicker";

struct PickerState {
  // Stack-scoped for one modal run; the controller outlives it.
  RAW_PTR_EXCLUSION PhiWindowController* controller = nullptr;
  std::string selected;
  std::string chosen;
  bool done = false;
  int hover = -1;
  UINT dpi = 96;
  std::vector<RECT> cells;  // Client coords, index-aligned with the catalog.
};

int Scaled(const PickerState& state, int design) {
  return ::MulDiv(design, static_cast<int>(state.dpi), 96);
}

void BuildCells(PickerState& state) {
  state.cells.clear();
  const size_t count = PhiFolderIconsAll().size();
  const int item = Scaled(state, kItemSize);
  const int gap = Scaled(state, kColumnSpacing);
  const int row_gap = Scaled(state, kRowSpacing);
  const int grid_left = (Scaled(state, kPickerWidth) -
                         Scaled(state, kGridWidth)) / 2;
  for (size_t i = 0; i < count; ++i) {
    const int column = static_cast<int>(i) % kColumns;
    const int row = static_cast<int>(i) / kColumns;
    const int x = grid_left + column * (item + gap);
    const int y = Scaled(state, kVerticalPad) + row * (item + row_gap);
    state.cells.push_back({x, y, x + item, y + item});
  }
}

int PickerHeight(const PickerState& state) {
  const size_t count = PhiFolderIconsAll().size();
  const int rows = static_cast<int>((count + kColumns - 1) / kColumns);
  return 2 * Scaled(state, kVerticalPad) + rows * Scaled(state, kItemSize) +
         std::max(0, rows - 1) * Scaled(state, kRowSpacing);
}

void Paint(HWND hwnd, PickerState& state, HDC dc) {
  RECT rc = {};
  ::GetClientRect(hwnd, &rc);
  const PhiPalette palette = state.controller
                                 ? state.controller->CurrentPalette()
                                 : PhiResolvePalette(PhiThemeById("pure"),
                                                     PhiAppearance::kSystem);
  HBRUSH ground = ::CreateSolidBrush(palette.window_bg);
  ::FillRect(dc, &rc, ground);
  ::DeleteObject(ground);

  const std::vector<PhiFolderIcon>& all = PhiFolderIconsAll();
  for (size_t i = 0; i < all.size() && i < state.cells.size(); ++i) {
    const RECT& cell = state.cells[i];
    const bool is_selected = state.selected == all[i].id;
    const bool is_hover = state.hover == static_cast<int>(i);
    if (is_selected || is_hover) {
      HBRUSH fill = ::CreateSolidBrush(is_selected ? palette.hover_strong
                                                   : palette.hover);
      HGDIOBJ prev_brush = ::SelectObject(dc, fill);
      HPEN pen = ::CreatePen(PS_SOLID, 1,
                             is_selected ? palette.accent : palette.border);
      HGDIOBJ prev_pen = ::SelectObject(dc, pen);
      const int r = Scaled(state, kItemRadius);
      ::RoundRect(dc, cell.left, cell.top, cell.right, cell.bottom, r, r);
      ::SelectObject(dc, prev_pen);
      ::SelectObject(dc, prev_brush);
      ::DeleteObject(pen);
      ::DeleteObject(fill);
    }
    const PhiMacIcon* mask = PhiFolderIconMask(all[i].id);
    if (!mask) {
      continue;
    }
    const int px = Scaled(state, kIconSize);
    RECT art = {cell.left + ((cell.right - cell.left) - px) / 2,
                cell.top + ((cell.bottom - cell.top) - px) / 2, 0, 0};
    art.right = art.left + px;
    art.bottom = art.top + px;
    const std::vector<unsigned char> bgra =
        PhiTintedIconBgra(*mask, palette.label, px);
    if (!bgra.empty()) {
      PhiBlendBgra(dc, art, bgra.data(), px, px);
    }
  }
}

LRESULT CALLBACK PickerProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
  auto* state = reinterpret_cast<PickerState*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (state) {
        Paint(hwnd, *state, dc);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!state) {
        break;
      }
      const POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      int hover = -1;
      for (size_t i = 0; i < state->cells.size(); ++i) {
        if (::PtInRect(&state->cells[i], pt)) {
          hover = static_cast<int>(i);
          break;
        }
      }
      if (hover != state->hover) {
        state->hover = hover;
        ::InvalidateRect(hwnd, nullptr, FALSE);
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      if (state && state->hover != -1) {
        state->hover = -1;
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP: {
      if (!state) {
        break;
      }
      const POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
      const std::vector<PhiFolderIcon>& all = PhiFolderIconsAll();
      for (size_t i = 0; i < state->cells.size() && i < all.size(); ++i) {
        if (::PtInRect(&state->cells[i], pt)) {
          state->chosen = all[i].id;
          break;
        }
      }
      state->done = true;
      ::PostMessageW(hwnd, WM_NULL, 0, 0);  // Wake the modal loop.
      return 0;
    }
    case WM_KEYDOWN:
      if (state && w == VK_ESCAPE) {
        state->done = true;
        ::PostMessageW(hwnd, WM_NULL, 0, 0);
      }
      return 0;
    case WM_ACTIVATE:
      // Dismiss like a popover the moment focus goes elsewhere.
      if (state && LOWORD(w) == WA_INACTIVE) {
        state->done = true;
        ::PostMessageW(hwnd, WM_NULL, 0, 0);
      }
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

void RegisterPickerClass() {
  static const bool registered = [] {
    WNDCLASSW cls = {};
    cls.lpfnWndProc = &PickerProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kClassName;
    return ::RegisterClassW(&cls) != 0;
  }();
  (void)registered;
}

}  // namespace

std::string PhiPickFolderIcon(PhiWindowController* controller,
                              const RECT& anchor_screen,
                              const std::string& selected) {
  HWND owner = controller ? controller->shell_hwnd() : nullptr;
  if (!owner) {
    return std::string();
  }
  RegisterPickerClass();

  PickerState state;
  state.controller = controller;
  state.selected = PhiResolveFolderIcon(selected);
  state.dpi = ::GetDpiForWindow(owner);
  BuildCells(state);

  const int w = Scaled(state, kPickerWidth);
  const int h = PickerHeight(state);
  int x = anchor_screen.left;
  int y = anchor_screen.bottom + Scaled(state, 4);
  HMONITOR monitor = ::MonitorFromRect(&anchor_screen, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info = {sizeof(info)};
  if (::GetMonitorInfoW(monitor, &info)) {
    x = std::max(static_cast<int>(info.rcWork.left),
                 std::min(x, static_cast<int>(info.rcWork.right) - w));
    if (y + h > info.rcWork.bottom) {
      y = std::max(static_cast<int>(info.rcWork.top),
                   static_cast<int>(anchor_screen.top) - h -
                       Scaled(state, 4));
    }
  }

  HWND picker = ::CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"", WS_POPUP,
                                  x, y, w, h, owner, nullptr,
                                  ::GetModuleHandleW(nullptr), nullptr);
  if (!picker) {
    return std::string();
  }
  ::SetWindowLongPtrW(picker, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(&state));
  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(picker, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                          sizeof(corner));
  ::ShowWindow(picker, SW_SHOW);
  ::SetForegroundWindow(picker);

  MSG msg;
  while (!state.done && ::GetMessageW(&msg, nullptr, 0, 0)) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
  }

  ::SetWindowLongPtrW(picker, GWLP_USERDATA, 0);
  ::DestroyWindow(picker);
  return state.chosen;
}

}  // namespace phi_client
