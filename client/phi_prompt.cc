// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_prompt.h"

#include <windowsx.h>

#include <algorithm>
#include <functional>
#include <utility>

#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_modal_scope.h"
#include "phibrowser-win/client/phi_popup_chrome.h"

namespace phi_client {

namespace {

constexpr wchar_t kPromptClass[] = L"PhiPrompt";

// PhiAlertLayout, in design units: a 465-wide surface with 24 of padding
// around 417 of content; the mark, 24 to the title, 16 to the copy, 36 to
// the actions row; 130x36 actions 8 apart.
constexpr int kWidth = 465;
constexpr int kPadding = 24;
constexpr int kContentWidth = kWidth - 2 * kPadding;  // 417
constexpr int kMark = 28;
constexpr int kMarkToTitle = 24;
constexpr int kTitleHeight = 24;  // 18pt, one line.
constexpr int kCopySpacing = 16;
constexpr int kActionsSpacing = 36;
constexpr int kActionWidth = 130;
constexpr int kActionHeight = 36;
constexpr int kActionGap = 8;
// The accessory wells: the Mac's 240x24 field grown to the alert's width.
constexpr int kFieldHeight = 34;
constexpr int kFieldInsetX = 10;
constexpr int kListRowHeight = 20;
constexpr int kListMaxRows = 8;
constexpr int kListInset = 4;
constexpr int kRecorderHeight = 40;

constexpr UINT_PTR kControlId = 100;
constexpr UINT_PTR kActionIdFirst = 1000;

enum class Accessory { kNone, kField, kList, kRecorder };

struct Action {
  std::wstring label;
  PhiPopupButtonRole role = PhiPopupButtonRole::kSecondary;
  bool leading = false;
};

struct Spec {
  std::wstring title;
  std::wstring message;
  std::vector<Action> actions;
  int default_index = -1;
  int cancel_index = -1;
  // The action a list double-click fires. Normally the default action; the
  // security prompts make Cancel the default (Return denies) while a
  // double-click on a row still means "that row, confirmed".
  int list_activate_index = -1;
  Accessory accessory = Accessory::kNone;
  std::wstring field_text;      // kField's initial text.
  std::wstring placeholder;     // kRecorder's resting line.
  std::vector<std::wstring> options;  // kList's rows.
  int selected = -1;                  // kList's preselected row.
};

// One alert window: built from a Spec, run modally, read back.
class Alert {
 public:
  Alert(HWND owner, Spec spec) : owner_(owner), spec_(std::move(spec)) {
    static const ATOM atom = [] {
      WNDCLASSEXW wc = {};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = &Alert::WndProc;
      wc.hInstance = ::GetModuleHandleW(nullptr);
      wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
      wc.lpszClassName = kPromptClass;
      return ::RegisterClassExW(&wc);
    }();
    (void)atom;
    colors_ = PhiPopupColorsNow();
    // Created hidden at a placeholder size; Build measures the copy and
    // sizes the window to fit, then Run centres it on the owner.
    hwnd_ = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME, kPromptClass, spec_.title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, 0, 0, 100, 100,
        owner, nullptr, ::GetModuleHandleW(nullptr), this);
    if (!hwnd_) {
      return;
    }
    chrome_.Attach(hwnd_, kPhiCaptionClose, colors_);
    dpi_ = chrome_.dpi();
    title_font_ = PhiPopupFont(dpi_, 18, FW_SEMIBOLD);
    body_font_ = PhiPopupFont(dpi_, 14);
    button_font_ = PhiPopupFont(dpi_, 13);
    card_brush_ = ::CreateSolidBrush(colors_.card);
    Build();
  }

  ~Alert() {
    if (hwnd_ && ::IsWindow(hwnd_)) {
      ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      ::DestroyWindow(hwnd_);
    }
    for (HFONT font : {title_font_, body_font_, button_font_}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
    if (card_brush_) {
      ::DeleteObject(card_brush_);
    }
  }

  bool valid() const { return hwnd_ != nullptr; }
  HWND control() const { return control_; }
  bool done() const { return done_; }
  int chosen() const { return chosen_; }
  void Finish(int chosen) {
    chosen_ = chosen;
    done_ = true;
    // Wake the modal loop: a WM_COMMAND delivered via SendMessage from
    // outside the thread is processed INSIDE GetMessageW without making it
    // return, so `done` alone would leave the loop parked until unrelated
    // input.
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);
  }

  // Shows the alert modally over its owner and pumps messages until an
  // action, Escape or the close button ends it. `filter` sees every message
  // first and returns true to swallow it (the chord recorder).
  int Run(const std::function<bool(const MSG&)>& filter = nullptr) {
    if (!hwnd_) {
      return -1;
    }
    RECT anchor = {};
    if (!owner_ || !::GetWindowRect(owner_, &anchor)) {
      ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
    }
    RECT frame = {};
    ::GetWindowRect(hwnd_, &frame);
    const int w = frame.right - frame.left;
    const int h = frame.bottom - frame.top;
    const int x = anchor.left + ((anchor.right - anchor.left) - w) / 2;
    const int y = anchor.top + ((anchor.bottom - anchor.top) - h) / 3;
    ::SetWindowPos(hwnd_, nullptr, std::max(x, 0), std::max(y, 0), 0, 0,
                   SWP_NOSIZE | SWP_NOZORDER);
    if (owner_) {
      ::EnableWindow(owner_, FALSE);  // Modal against its owner, as NSAlert.
    }
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    HWND focus = control_;
    if (!focus && spec_.default_index >= 0 &&
        static_cast<size_t>(spec_.default_index) < buttons_.size()) {
      focus = buttons_[static_cast<size_t>(spec_.default_index)];
    }
    if (!focus && !buttons_.empty()) {
      focus = buttons_.back();
    }
    ::SetFocus(focus ? focus : hwnd_);
    if (control_ && spec_.accessory == Accessory::kField) {
      ::SendMessageW(control_, EM_SETSEL, 0, -1);
    }

    {
      // Every PhiPrompt*/PhiAlert*/PhiNotice/PhiConfirm caller runs its
      // nested loop here, so this ONE scope is what keeps a pipe verb from
      // destroying the controller (or rebuilding the rows) whose handler is
      // waiting on the answer.
      PhiModalScope modal;
      MSG msg;
      while (!done_ && ::GetMessageW(&msg, nullptr, 0, 0)) {
        if (filter && filter(msg)) {
          continue;
        }
        if (!::IsDialogMessageW(hwnd_, &msg)) {
          ::TranslateMessage(&msg);
          ::DispatchMessageW(&msg);
        }
      }
    }
    if (owner_) {
      ::EnableWindow(owner_, TRUE);
      ::SetForegroundWindow(owner_);
    }
    return chosen_;
  }

  std::wstring ControlText() const {
    if (!control_) {
      return std::wstring();
    }
    const int length = ::GetWindowTextLengthW(control_);
    if (length <= 0) {
      return std::wstring();
    }
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(control_, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
  }

  int ListSelection() const {
    if (!control_) {
      return -1;
    }
    const LRESULT sel = ::SendMessageW(control_, LB_GETCURSEL, 0, 0);
    return sel == LB_ERR ? -1 : static_cast<int>(sel);
  }

 private:
  int Scaled(int design) const {
    return ::MulDiv(design, static_cast<int>(dpi_), 96);
  }

  HWND Child(const wchar_t* cls,
             const wchar_t* text,
             DWORD style,
             const RECT& rc,
             UINT_PTR id,
             HFONT font) {
    HWND control = ::CreateWindowExW(
        0, cls, text, WS_CHILD | WS_VISIBLE | style, rc.left, rc.top,
        rc.right - rc.left, rc.bottom - rc.top, hwnd_,
        reinterpret_cast<HMENU>(id), ::GetModuleHandleW(nullptr), nullptr);
    if (control && font) {
      ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font),
                     TRUE);
    }
    return control;
  }

  // Measures the copy, sizes the window and places the children.
  void Build() {
    const int x0 = Scaled(kPadding);
    const int content_w = Scaled(kContentWidth);
    int y = chrome_.CaptionHeight() + Scaled(kPadding);

    mark_ = {x0, y, x0 + Scaled(kMark), y + Scaled(kMark)};
    y += Scaled(kMark + kMarkToTitle);
    title_ = {x0, y, x0 + content_w, y + Scaled(kTitleHeight)};
    y += Scaled(kTitleHeight);

    // The copy wraps at the content width; its height is the text's.
    int message_h = 0;
    if (!spec_.message.empty()) {
      HDC dc = ::GetDC(hwnd_);
      HGDIOBJ previous = body_font_ ? ::SelectObject(dc, body_font_) : nullptr;
      RECT measure = {0, 0, content_w, 0};
      ::DrawTextW(dc, spec_.message.c_str(), -1, &measure,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
      message_h = measure.bottom;
      if (previous) {
        ::SelectObject(dc, previous);
      }
      ::ReleaseDC(hwnd_, dc);
      y += Scaled(kCopySpacing);
      message_ = {x0, y, x0 + content_w, y + message_h};
      y += message_h;
    }

    int well_h = 0;
    switch (spec_.accessory) {
      case Accessory::kNone:
        break;
      case Accessory::kField:
        well_h = Scaled(kFieldHeight);
        break;
      case Accessory::kList: {
        const int rows = std::clamp(static_cast<int>(spec_.options.size()),
                                    1, kListMaxRows);
        well_h = Scaled(rows * kListRowHeight + 2 * kListInset);
        break;
      }
      case Accessory::kRecorder:
        well_h = Scaled(kRecorderHeight);
        break;
    }
    if (well_h) {
      y += Scaled(kCopySpacing);
      well_ = {x0, y, x0 + content_w, y + well_h};
      y += well_h;
    }

    y += Scaled(kActionsSpacing);
    actions_top_ = y;
    y += Scaled(kActionHeight) + Scaled(kPadding);

    // Size the window: the content plus the band and the thin borders.
    const int content_h = ::MulDiv(y - chrome_.CaptionHeight(), 96,
                                   static_cast<int>(dpi_));
    const SIZE outer = chrome_.OuterSize(kWidth, content_h);
    ::SetWindowPos(hwnd_, nullptr, 0, 0, outer.cx, outer.cy,
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    BuildAccessory();
    BuildActions();
  }

  void BuildAccessory() {
    switch (spec_.accessory) {
      case Accessory::kNone:
        return;
      case Accessory::kField: {
        // The EDIT sits inset in the drawn well, borderless — the well's
        // stroke is the field's border.
        const int line = Scaled(20);
        RECT rc = {well_.left + Scaled(kFieldInsetX),
                   (well_.top + well_.bottom - line) / 2,
                   well_.right - Scaled(kFieldInsetX),
                   (well_.top + well_.bottom + line) / 2};
        control_ = Child(L"EDIT", spec_.field_text.c_str(),
                         WS_TABSTOP | ES_AUTOHSCROLL | ES_LEFT, rc,
                         kControlId, body_font_);
        break;
      }
      case Accessory::kList: {
        RECT rc = {well_.left + Scaled(kListInset), well_.top + Scaled(kListInset),
                   well_.right - Scaled(kListInset),
                   well_.bottom - Scaled(kListInset)};
        control_ = Child(L"LISTBOX", L"",
                         WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                             LBS_NOINTEGRALHEIGHT,
                         rc, kControlId, button_font_);
        if (!control_) {
          return;
        }
        ::SendMessageW(control_, LB_SETITEMHEIGHT, 0, Scaled(kListRowHeight));
        for (const std::wstring& option : spec_.options) {
          ::SendMessageW(control_, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(option.c_str()));
        }
        if (spec_.selected >= 0 &&
            spec_.selected < static_cast<int>(spec_.options.size())) {
          ::SendMessageW(control_, LB_SETCURSEL,
                         static_cast<WPARAM>(spec_.selected), 0);
        }
        break;
      }
      case Accessory::kRecorder: {
        RECT rc = {well_.left + Scaled(2), well_.top + Scaled(2),
                   well_.right - Scaled(2), well_.bottom - Scaled(2)};
        control_ = Child(L"STATIC", spec_.placeholder.c_str(),
                         SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX, rc,
                         kControlId, body_font_);
        break;
      }
    }
    PhiPopupThemeControl(control_, colors_.dark);
  }

  void BuildActions() {
    const int w = Scaled(kActionWidth);
    const int h = Scaled(kActionHeight);
    const int gap = Scaled(kActionGap);
    int left = Scaled(kPadding);
    int right = Scaled(kPadding + kContentWidth);
    buttons_.assign(spec_.actions.size(), nullptr);
    // Leading actions from the left edge, the rest packed at the right with
    // the last one rightmost.
    for (size_t i = 0; i < spec_.actions.size(); ++i) {
      if (!spec_.actions[i].leading) {
        continue;
      }
      RECT rc = {left, actions_top_, left + w, actions_top_ + h};
      buttons_[i] = Child(L"BUTTON", spec_.actions[i].label.c_str(),
                          WS_TABSTOP | BS_PUSHBUTTON, rc, kActionIdFirst + i,
                          button_font_);
      PhiPopupStyleButton(buttons_[i], spec_.actions[i].role);
      left += w + gap;
    }
    for (size_t n = spec_.actions.size(); n > 0; --n) {
      const size_t i = n - 1;
      if (spec_.actions[i].leading) {
        continue;
      }
      RECT rc = {right - w, actions_top_, right, actions_top_ + h};
      buttons_[i] = Child(L"BUTTON", spec_.actions[i].label.c_str(),
                          WS_TABSTOP | BS_PUSHBUTTON, rc, kActionIdFirst + i,
                          button_font_);
      PhiPopupStyleButton(buttons_[i], spec_.actions[i].role);
      right -= w + gap;
    }
  }

  void Paint() {
    PAINTSTRUCT ps = {};
    HDC dc = ::BeginPaint(hwnd_, &ps);
    if (!dc) {
      return;
    }
    RECT client = {};
    ::GetClientRect(hwnd_, &client);
    HBRUSH ground = ::CreateSolidBrush(colors_.bg);
    ::FillRect(dc, &client, ground);
    ::DeleteObject(ground);
    // No title in the band — PhiAlert carries its title in the body.
    chrome_.PaintCaption(dc, colors_, /*show_mark=*/false, L"");

    const std::vector<unsigned char>& logo = PhiLogoBgra256();
    if (!logo.empty()) {
      PhiDrawBgra(dc, mark_, logo.data(), 256, 256);
    }
    ::SetBkMode(dc, TRANSPARENT);
    HGDIOBJ previous = title_font_ ? ::SelectObject(dc, title_font_) : nullptr;
    ::SetTextColor(dc, colors_.label_strong);
    RECT title = title_;
    ::DrawTextW(dc, spec_.title.c_str(), -1, &title,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                    DT_END_ELLIPSIS);
    if (!spec_.message.empty()) {
      if (body_font_) {
        ::SelectObject(dc, body_font_);
      }
      ::SetTextColor(dc, colors_.label);
      RECT message = message_;
      ::DrawTextW(dc, spec_.message.c_str(), -1, &message,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }
    if (spec_.accessory != Accessory::kNone) {
      PhiPopupPaintWell(dc, well_, colors_, dpi_);
    }
    if (previous) {
      ::SelectObject(dc, previous);
    }
    ::EndPaint(hwnd_, &ps);
  }

  LRESULT Handle(UINT msg, WPARAM wparam, LPARAM lparam) {
    LRESULT result = 0;
    if (chrome_.HandleMessage(msg, wparam, lparam, &result)) {
      return result;
    }
    switch (msg) {
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
        Paint();
        return 0;
      case DM_GETDEFID:
        // IsDialogMessage's Return: the default action, or none.
        if (spec_.default_index >= 0) {
          return MAKELRESULT(kActionIdFirst + spec_.default_index,
                             DC_HASDEFID);
        }
        return 0;
      case WM_COMMAND: {
        const UINT id = LOWORD(wparam);
        const UINT code = HIWORD(wparam);
        if (id >= kActionIdFirst &&
            id < kActionIdFirst + spec_.actions.size() &&
            (code == BN_CLICKED || code == 0)) {
          Finish(static_cast<int>(id - kActionIdFirst));
          return 0;
        }
        if (id == IDOK) {
          // Return with no default action registered: nothing happens,
          // the Mac's `.noButton` mode.
          if (spec_.default_index >= 0) {
            Finish(spec_.default_index);
          }
          return 0;
        }
        if (id == IDCANCEL) {
          Finish(spec_.cancel_index);
          return 0;
        }
        if (id == kControlId && code == LBN_DBLCLK) {
          const int activate = spec_.list_activate_index >= 0
                                   ? spec_.list_activate_index
                                   : spec_.default_index;
          if (activate >= 0) {
            Finish(activate);  // A double-click confirms the row.
          }
          return 0;
        }
        break;
      }
      case WM_DRAWITEM:
        if (PhiPopupDrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam),
                               colors_, button_font_)) {
          return TRUE;
        }
        break;
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
      case WM_CTLCOLORSTATIC: {
        // The accessory paints on the well's card colour.
        HDC dc = reinterpret_cast<HDC>(wparam);
        ::SetTextColor(dc, colors_.label);
        ::SetBkColor(dc, colors_.card);
        return reinterpret_cast<LRESULT>(card_brush_);
      }
      case WM_CLOSE:
        Finish(spec_.cancel_index);
        return 0;
      default:
        break;
    }
    return ::DefWindowProcW(hwnd_, msg, wparam, lparam);
  }

  static LRESULT CALLBACK WndProc(HWND hwnd,
                                  UINT msg,
                                  WPARAM wparam,
                                  LPARAM lparam) {
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      auto* self = reinterpret_cast<Alert*>(cs->lpCreateParams);
      self->hwnd_ = hwnd;  // The chrome needs it before CreateWindow returns.
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    auto* self =
        reinterpret_cast<Alert*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self || self->hwnd_ != hwnd) {
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    return self->Handle(msg, wparam, lparam);
  }

  HWND owner_ = nullptr;
  Spec spec_;
  HWND hwnd_ = nullptr;
  PhiPopupChrome chrome_;
  PhiPopupColors colors_;
  UINT dpi_ = 96;
  HFONT title_font_ = nullptr;
  HFONT body_font_ = nullptr;
  HFONT button_font_ = nullptr;
  HBRUSH card_brush_ = nullptr;
  HWND control_ = nullptr;
  std::vector<HWND> buttons_;
  RECT mark_ = {};
  RECT title_ = {};
  RECT message_ = {};
  RECT well_ = {};
  int actions_top_ = 0;
  bool done_ = false;
  int chosen_ = -1;
};

PhiPopupButtonRole RoleFor(PhiAlertRole role) {
  switch (role) {
    case PhiAlertRole::kPrimary:
      return PhiPopupButtonRole::kPrimary;
    case PhiAlertRole::kDestructive:
      return PhiPopupButtonRole::kDestructive;
    case PhiAlertRole::kSecondary:
      break;
  }
  return PhiPopupButtonRole::kSecondary;
}

}  // namespace

int PhiAlert(HWND owner,
             const wchar_t* title,
             const wchar_t* message,
             const std::vector<PhiAlertAction>& actions,
             int default_index,
             int cancel_index) {
  Spec spec;
  spec.title = title ? title : L"";
  spec.message = message ? message : L"";
  for (const PhiAlertAction& action : actions) {
    spec.actions.push_back({action.label, RoleFor(action.role), action.leading});
  }
  spec.default_index =
      default_index < static_cast<int>(actions.size()) ? default_index : -1;
  spec.cancel_index =
      cancel_index < static_cast<int>(actions.size()) ? cancel_index : -1;
  Alert alert(owner, std::move(spec));
  return alert.valid() ? alert.Run() : -1;
}

void PhiNotice(HWND owner, const wchar_t* title, const wchar_t* message) {
  PhiAlert(owner, title, message, {{L"OK", PhiAlertRole::kPrimary}}, 0, 0);
}

bool PhiPromptText(HWND owner,
                   const wchar_t* title,
                   const wchar_t* message,
                   std::wstring* value,
                   const wchar_t* confirm_label) {
  Spec spec;
  spec.title = title ? title : L"";
  spec.message = message ? message : L"";
  spec.accessory = Accessory::kField;
  spec.field_text = value ? *value : std::wstring();
  spec.actions = {{L"Cancel", PhiPopupButtonRole::kSecondary},
                  {confirm_label ? confirm_label : L"OK",
                   PhiPopupButtonRole::kPrimary}};
  spec.default_index = 1;
  spec.cancel_index = 0;
  Alert alert(owner, std::move(spec));
  if (!alert.valid()) {
    return false;
  }
  const bool accepted = alert.Run() == 1;
  const std::wstring entered = alert.ControlText();
  if (!accepted || entered.empty()) {
    return false;
  }
  if (value) {
    *value = entered;
  }
  return true;
}

bool PhiConfirm(HWND owner,
                const wchar_t* title,
                const wchar_t* message,
                const wchar_t* confirm_label,
                bool destructive) {
  // The confirming button answers Return only when it is not destructive —
  // an irreversible action wants a deliberate click, the Mac's noButton.
  return PhiAlert(owner, title, message,
                  {{L"Cancel", PhiAlertRole::kSecondary},
                   {confirm_label ? confirm_label : L"OK",
                    destructive ? PhiAlertRole::kDestructive
                                : PhiAlertRole::kPrimary}},
                  destructive ? -1 : 1, 0) == 1;
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
  constexpr wchar_t kResting[] = L"Type the new shortcut\u2026";
  Spec spec;
  spec.title = title ? title : L"";
  spec.message = message ? message : L"";
  spec.accessory = Accessory::kRecorder;
  spec.placeholder = kResting;
  spec.actions = {{L"Cancel", PhiPopupButtonRole::kSecondary}};
  spec.cancel_index = 0;
  Alert alert(owner, std::move(spec));
  if (!alert.valid()) {
    return false;
  }
  HWND preview = alert.control();

  const auto is_modifier = [](UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN;
  };
  const auto held_prefix = [&] {
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
    return text.empty() ? std::wstring(kResting) : text + L"\u2026";
  };

  PhiShortcutChord captured;
  bool accepted = false;
  // The recorder itself: every key in the modal loop is a candidate.
  // WM_SYSKEYDOWN carries the Alt chords; consuming it also keeps
  // DefWindowProc out of the menu loop.
  const auto filter = [&](const MSG& msg) {
    if (msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) {
      const UINT vk = static_cast<UINT>(msg.wParam);
      const bool ctrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
      const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
      const bool alt = (::GetKeyState(VK_MENU) & 0x8000) != 0;
      if (is_modifier(vk)) {
        ::SetWindowTextW(preview, held_prefix().c_str());
        return true;
      }
      if (vk == VK_ESCAPE && !ctrl && !alt) {
        alert.Finish(0);
        return true;
      }
      if (ctrl || alt || (vk >= VK_F1 && vk <= VK_F24)) {
        captured = {vk, ctrl, shift, alt};
        accepted = true;
        alert.Finish(-1);
        return true;
      }
      // A bare key: browsers require a modifier so pages keep their typing.
      ::SetWindowTextW(preview, L"Include Ctrl or Alt");
      return true;
    }
    if (msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP) {
      if (is_modifier(static_cast<UINT>(msg.wParam))) {
        ::SetWindowTextW(preview, held_prefix().c_str());
      }
      return true;
    }
    return false;
  };
  alert.Run(filter);
  if (!accepted) {
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
                    int default_index,
                    const wchar_t* confirm_label,
                    const wchar_t* cancel_label,
                    bool cancel_is_default) {
  Spec spec;
  spec.title = title ? title : L"";
  spec.message = message ? message : L"";
  spec.accessory = Accessory::kList;
  spec.options = options;
  spec.selected = default_index;
  spec.actions = {{cancel_label ? cancel_label : L"Keep Here",
                   PhiPopupButtonRole::kSecondary},
                  {confirm_label ? confirm_label : L"Open",
                   PhiPopupButtonRole::kPrimary}};
  spec.default_index = cancel_is_default ? 0 : 1;
  spec.cancel_index = 0;
  spec.list_activate_index = 1;
  Alert alert(owner, std::move(spec));
  if (!alert.valid()) {
    return -1;
  }
  const bool accepted = alert.Run() == 1;
  return accepted ? alert.ListSelection() : -1;
}

}  // namespace phi_client
