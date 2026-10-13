// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_omnibox_session.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_panel.h"
#include "phibrowser-win/client/phi_shell_menus.h"

#pragma comment(lib, "dwmapi.lib")

namespace phi_client {

namespace {

// Omnibox suggestion dropdown metrics (DIP).
constexpr int kRowHeight = 32;
constexpr int kMaxRows = 8;
constexpr int kPopupPad = 12;   // The list's total vertical padding.
constexpr int kPopupInset = 6;  // Half of it, above the first row.

// The EDIT class's own window procedure, captured the first time a field is
// subclassed. PROCESS-WIDE on purpose: every address field in every shell
// window is an EDIT, so they all chain to the same base proc, and the value
// is a constant of the class rather than state of any one window. It is a
// plain pointer with no destructor, so it costs nothing at exit.
WNDPROC g_edit_base_proc = nullptr;

// Reads the whole text of an EDIT.
std::wstring TextOf(HWND edit) {
  const int len = ::GetWindowTextLengthW(edit);
  std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
  if (len > 0) {
    ::GetWindowTextW(edit, buffer.data(), len + 1);
  }
  return std::wstring(buffer.data());
}

// The clipboard's Unicode text, or empty.
std::wstring ClipboardText(HWND owner) {
  std::wstring text;
  if (::OpenClipboard(owner)) {
    if (HANDLE data = ::GetClipboardData(CF_UNICODETEXT)) {
      if (const wchar_t* chars =
              static_cast<const wchar_t*>(::GlobalLock(data))) {
        text = chars;
        ::GlobalUnlock(data);
      }
    }
    ::CloseClipboard();
  }
  return text;
}

RECT ToRect(const SidebarRect& r) {
  return RECT{r.left, r.top, r.right, r.bottom};
}

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiOmniboxSession*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        self->PaintPopup(hwnd, dc);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (self) {
        // The hover wash leaves with the pointer (WM_MOUSELEAVE below).
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
        self->PopupHover(GET_Y_LPARAM(lparam));
      }
      return 0;
    case WM_MOUSELEAVE:
      if (self) {
        self->PopupHover(-1);
      }
      return 0;
    case WM_LBUTTONDOWN:
      if (self) {
        self->PopupClick(GET_Y_LPARAM(lparam));
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

ATOM EnsurePopupClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &PopupProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PhiOmniboxPopup";
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

}  // namespace

// --- Lifetime ------------------------------------------------------------

PhiOmniboxSession::PhiOmniboxSession(PhiOmniboxShell* shell) : shell_(shell) {}

PhiOmniboxSession::~PhiOmniboxSession() {
  // The EDIT is a child of the shell window and dies with it; the popup is
  // a top-level window of ours, so it goes here.
  if (popup_ && ::IsWindow(popup_)) {
    ::DestroyWindow(popup_);
  }
  popup_ = nullptr;
  if (brush_) {
    ::DeleteObject(brush_);
  }
}

int PhiOmniboxSession::Scaled(int design_units) const {
  return shell_->OmniboxScaled(design_units);
}

// --- The address field ---------------------------------------------------

void PhiOmniboxSession::EnsureField() {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  if (field_ || !shell_hwnd) {
    return;
  }
  field_ = ::CreateWindowExW(
      0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0,
      10, 10, shell_hwnd, nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (!field_) {
    LOG(ERROR) << "[PhiClient] address field CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }
  if (HFONT font = shell_->OmniboxTextFont()) {
    ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
  // The Mac's placeholder, shown by the EDIT itself while empty (and by the
  // capture path, which renders without children).
  ::SendMessageW(field_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(
                     PhiStr("addressBar.configuration.defaultPlaceholder")));
  AttachField(field_);
  Position();
}

void PhiOmniboxSession::AttachField(HWND edit) {
  if (!edit || !::IsWindow(edit)) {
    return;
  }
  ::SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  WNDPROC previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
      edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FieldProc)));
  if (!g_edit_base_proc) {
    g_edit_base_proc = previous;  // The EDIT class proc — one for all fields.
  }
}

HWND PhiOmniboxSession::FocusedField() const {
  const HWND focus = ::GetFocus();
  if (!focus) {
    return nullptr;
  }
  if (field_ && focus == field_) {
    return field_;
  }
  const HWND native = shell_->OmniboxNativePageField();
  if (native && focus == native) {
    return native;
  }
  return nullptr;
}

bool PhiOmniboxSession::FieldHasFocus() const {
  return field_ && ::GetFocus() == field_;
}

void PhiOmniboxSession::SetFont(HFONT font) {
  if (field_ && font) {
    ::SendMessageW(field_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
}

void PhiOmniboxSession::Position() {
  if (!field_) {
    return;
  }
  // Sits INSIDE the painted field box, vertically centred.
  const RECT target = ToRect(PhiOmniboxTextRect(
      shell_->OmniboxCapsule(), /*text_height=*/Scaled(20),
      // SideAddressBar: text starts 12 in from the pill's leading edge, 8
      // clear before the in-field trailing stack (or the bare trailing edge
      // without one).
      /*leading_inset=*/Scaled(12), /*icon_gap=*/Scaled(8),
      /*trailing_gap=*/Scaled(8)));
  // Idempotent, because the paint path calls this too: the in-field
  // accessory set changes with the page — the reader button comes and goes —
  // and the EDIT is a real child window, so a stale width paints straight
  // over whatever just appeared beside it. Moving only on a real change
  // keeps that call from feeding itself more paints.
  RECT placed = target;
  const HWND parent = shell_->OmniboxShellWindow();
  RECT current = {};
  if (::GetWindowRect(field_, &current) &&
      ::MapWindowPoints(HWND_DESKTOP, parent,
                        reinterpret_cast<POINT*>(&current), 2) &&
      ::EqualRect(&current, &placed)) {
    return;
  }
  ::MoveWindow(field_, placed.left, placed.top, placed.right - placed.left,
               placed.bottom - placed.top, TRUE);
}

void PhiOmniboxSession::SetFieldVisible(bool visible) {
  if (field_) {
    ::ShowWindow(field_, visible ? SW_SHOW : SW_HIDE);
  }
}


void PhiOmniboxSession::Sync(bool force) {
  if (!field_) {
    return;
  }
  // Never overwrite a half-typed URL: nav state refreshes on every paint and
  // on every framework notification.
  if (!force && ::GetFocus() == field_) {
    return;
  }
  if (nav_url_ == TextOf(field_)) {
    return;  // Unchanged; setting it again would fight the caret.
  }
  programmatic_ = true;  // Not the user typing — no autocomplete.
  ::SetWindowTextW(field_, nav_url_.c_str());
  programmatic_ = false;
}

void PhiOmniboxSession::CommitField() {
  Commit(field_);
}

void PhiOmniboxSession::ShowFieldMenu(HWND edit, POINT screen) {
  DWORD sel_start = 0;
  DWORD sel_end = 0;
  ::SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start),
                 reinterpret_cast<LPARAM>(&sel_end));
  const bool has_selection = sel_end > sel_start;
  const bool has_text = ::GetWindowTextLengthW(edit) > 0;
  const bool clipboard_text =
      ::IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
  const bool can_undo = ::SendMessageW(edit, EM_CANUNDO, 0, 0) != 0;
  PhiMenuSpec spec = PhiBuildOmniboxFieldMenu(has_selection, has_text,
                                              clipboard_text, can_undo);
  menu_palette_ = shell_->OmniboxPalette();
  PhiPopupMenuRequest request;
  request.owner = shell_->OmniboxShellWindow();
  request.x = screen.x;
  request.y = screen.y;
  request.align = TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON;
  request.palette = &menu_palette_;
  const PhiMenuAction action = PhiRunPopupMenu(spec, request);
  if (!::IsWindow(edit)) {
    return;
  }
  if (action.is("edit.undo")) {
    ::SendMessageW(edit, WM_UNDO, 0, 0);
  } else if (action.is("edit.cut")) {
    ::SendMessageW(edit, WM_CUT, 0, 0);
  } else if (action.is("edit.copy")) {
    ::SendMessageW(edit, WM_COPY, 0, 0);
  } else if (action.is("edit.paste")) {
    ::SendMessageW(edit, WM_PASTE, 0, 0);
  } else if (action.is("edit.delete")) {
    ::SendMessageW(edit, WM_CLEAR, 0, 0);
  } else if (action.is("edit.selectAll")) {
    ::SendMessageW(edit, EM_SETSEL, 0, -1);
  } else if (action.is("edit.pasteAndGo")) {
    // Chrome's and Edge's row: the clipboard replaces the field and goes,
    // without a round trip through the keyboard — sanitized as Chrome's
    // paste is (line breaks folded, javascript: schemes stripped).
    const std::wstring text =
        PhiOmniboxTrimTyped(PhiOmniboxSanitizePaste(ClipboardText(edit)));
    if (text.empty()) {
      return;
    }
    ::SetWindowTextW(edit, text.c_str());
    ClosePopup(/*stop_query=*/false);
    Commit(edit);
  }
}

void PhiOmniboxSession::PasteInto(HWND edit) {
  // Every paste into the field (WM_PASTE, Ctrl+V, Shift+Insert, the menu)
  // lands sanitized: a pasted "javascript:..." never reaches Enter intact.
  const std::wstring text = PhiOmniboxSanitizePaste(ClipboardText(edit));
  if (!text.empty()) {
    ::SendMessageW(edit, EM_REPLACESEL, TRUE,
                   reinterpret_cast<LPARAM>(text.c_str()));
  }
}

void PhiOmniboxSession::Commit(HWND edit) {
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  if (!edit || !::IsWindow(edit) || !framework) {
    return;
  }
  const std::wstring typed = PhiOmniboxTrimTyped(TextOf(edit));
  if (typed.empty()) {
    return;
  }
  shell_->OmniboxNativePageCommitting();
  // The framework resolves what was typed the way the omnibox would — a bare
  // host becomes a URL, free text becomes a search.
  const BrowserId browser = shell_->OmniboxBrowser();
  if (!browser.valid()) {
    return;  // No browser attached: nowhere for the text to go.
  }
  if (new_tab_pending_ || shell_->OmniboxBrowserPlaceholding()) {
    // Omnibox new-tab mode — and a placeholder-mode browser, whose strip is
    // EMPTY (Navigate alone would no-op): the tab is born on commit. The
    // fresh tab is active, so Navigate applies the full typed-text
    // resolution to it (NewTab itself loads URLs literally).
    new_tab_pending_ = false;
    framework->NewTab(browser.value, "");
  }
  // The typed URL replaces the ORIGIN tab's page, which the reader is
  // covering — leave the reader, or what arrives loads unseen behind it
  // (the Mac's closeReaderOverlayForAddressBarNavigation).
  shell_->OmniboxLeaveReader();
  framework->Navigate(browser.value, base::WideToUTF8(typed).c_str());
  shell_->OmniboxFocusContent();  // Keys belong to the page again.
}

bool PhiOmniboxSession::FocusFieldIfVisible() {
  if (!field_ || !::IsWindowVisible(field_)) {
    return false;
  }
  ::SetFocus(field_);
  ::SendMessageW(field_, EM_SETSEL, 0, -1);
  return true;
}

void PhiOmniboxSession::StartNewTab() {
  // "New tab behavior = Omnibox": Ctrl+T / the New Tab row focus the
  // address field instead of opening a page; the tab is born on commit
  // (the Mac's toggleOmniBox new-tab mode). The page mode opens the NTP.
  if (shell_->OmniboxNewTabFocusesField() && field_ &&
      ::IsWindowVisible(field_)) {
    new_tab_pending_ = true;
    ::SetFocus(field_);
    ::SendMessageW(field_, EM_SETSEL, 0, -1);
    return;
  }
  const BrowserId browser = shell_->OmniboxBrowser();
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  if (framework && browser.valid()) {
    framework->NewTab(browser.value, "");
  }
}

void PhiOmniboxSession::Repaint() {
  if (field_ && ::IsWindow(field_)) {
    // Erase so WM_CTLCOLOREDIT hands the EDIT the current palette's brush.
    ::InvalidateRect(field_, nullptr, TRUE);
  }
}

HBRUSH PhiOmniboxSession::OnCtlColorEdit(HDC edit_dc, HWND edit) {
  if (!field_ || edit != field_) {
    return nullptr;
  }
  // The shell's CurrentPalette, not the host's: incognito overrides to
  // fixed-dark.
  const PhiPalette palette = shell_->OmniboxFieldPalette();
  // In the sidebar the field rests on the 4% wash (8% while hovered,
  // tracking the capsule's brighten); in a top bar it sits bare on the
  // bar's own ground, washing only under the cursor.
  // Comfortable's omnibox is a filled pill like the sidebar's, not a
  // bare top-bar field: without this the EDIT paints the toolbar's
  // white straight over the pill and leaves only its rim showing.
  // Balanced and comfortable wash over the bar's own windowBackground,
  // so the EDIT carries no Space tint there; only performance's
  // sidebar pill blends over the tinted ground (see kFieldBg).
  // The EDIT paints on the field's own fill (windows-native-ui.md §4, the
  // shell's PaintChrome): the control fill at rest, a step darker under
  // the pointer, the card body while focused — in every layout.
  const bool focused = ::GetFocus() == field_;
  const COLORREF bg = focused        ? palette.card
                      : field_hover_ ? PhiBlend(palette.ink, palette.card, 18)
                                     : palette.control_fill;
  ::SetTextColor(edit_dc, palette.label);
  ::SetBkColor(edit_dc, bg);
  if (!brush_ || brush_color_ != bg) {
    if (brush_) {
      ::DeleteObject(brush_);
    }
    brush_ = ::CreateSolidBrush(bg);
    brush_color_ = bg;
  }
  return brush_;
}

bool PhiOmniboxSession::OnShellCommand(WPARAM wparam, LPARAM lparam) {
  if (!field_ || lparam != reinterpret_cast<LPARAM>(field_) ||
      HIWORD(wparam) != EN_CHANGE) {
    return false;
  }
  OnFieldChanged(field_);  // The user typed: autocomplete runs.
  return true;
}

void PhiOmniboxSession::OnFieldChanged(HWND edit) {
  // EN_CHANGE while the user types: run/refresh the autocomplete query.
  // Programmatic sets (Sync, arrowing onto a row) are suppressed.
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  if (programmatic_ || !edit || !framework || ::GetFocus() != edit) {
    return;
  }
  const std::wstring typed = TextOf(edit);
  if (typed.empty()) {
    ClosePopup(/*stop_query=*/true);
    return;
  }
  const BrowserId browser = shell_->OmniboxBrowser();
  if (!browser.valid()) {
    return;
  }
  framework->StartOmniboxQuery(browser.value, base::WideToUTF8(typed).c_str());
}

// --- Navigation state ----------------------------------------------------

void PhiOmniboxSession::RebuildNavState(const phi_framework::NavState* nav) {
  nav_url_.clear();
  can_go_back_ = can_go_forward_ = nav_loading_ = false;
  if (!nav) {
    return;
  }
  // The NTP shows the PLACEHOLDER, not its chrome:// url (the Mac's
  // updateDisplayedURL isNTP blank).
  if (!PhiOmniboxHidesUrl(nav->url)) {
    nav_url_ = base::UTF8ToWide(nav->url);
  }
  can_go_back_ = nav->can_go_back;
  can_go_forward_ = nav->can_go_forward;
  nav_loading_ = nav->loading;
  Sync(/*force=*/false);
}

std::wstring PhiOmniboxSession::DisplayedUrl() const {
  // What the user actually sees, mid-edit.
  return field_ ? TextOf(field_) : nav_url_;
}

void PhiOmniboxSession::SetFieldHover(bool hovered) {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  if (!hovered) {
    // A leave from one side (EDIT vs shell rim) can land after the enter on
    // the other; trust the cursor, not the message order.
    POINT pt = {};
    if (::GetCursorPos(&pt) && shell_hwnd &&
        ::ScreenToClient(shell_hwnd, &pt)) {
      const RECT field = ToRect(shell_->OmniboxCapsule().field);
      hovered = ::PtInRect(&field, pt) != FALSE;
    }
  }
  if (field_hover_ == hovered) {
    return;
  }
  field_hover_ = hovered;
  if (shell_hwnd) {
    // The capsule's rim: the field plus the wash's antialiased edge.
    RECT field = ToRect(shell_->OmniboxCapsule().field);
    ::InflateRect(&field, Scaled(2), Scaled(2));
    ::InvalidateRect(shell_hwnd, &field, FALSE);
  }
  if (field_) {
    // The EDIT repaints through WM_CTLCOLOREDIT to pick up the new wash.
    ::InvalidateRect(field_, nullptr, TRUE);
  }
}

void PhiOmniboxSession::CopyLinkToClipboard() {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  if (nav_url_.empty() || !shell_hwnd || !::OpenClipboard(shell_hwnd)) {
    LOG(WARNING) << "copy link: nothing copied (url empty " << nav_url_.empty()
                 << ", clipboard error " << ::GetLastError() << ")";
    return;
  }
  // The Mac's phiBrandEnsuredUrlString: Chromium's own pages travel as
  // phi:// links.
  std::wstring url = nav_url_;
  static constexpr wchar_t kChromeScheme[] = L"chrome://";
  if (url.rfind(kChromeScheme, 0) == 0) {
    url = L"phi://" + url.substr(std::size(kChromeScheme) - 1);
  }
  ::EmptyClipboard();
  const size_t bytes = (url.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (wchar_t* dest = static_cast<wchar_t*>(::GlobalLock(mem))) {
      std::copy_n(url.c_str(), url.size() + 1, dest);
      ::GlobalUnlock(mem);
      if (!::SetClipboardData(CF_UNICODETEXT, mem)) {
        ::GlobalFree(mem);
      }
    } else {
      ::GlobalFree(mem);
    }
  }
  ::CloseClipboard();
  copy_link_flash_ = true;
  ::SetTimer(shell_hwnd, kCopyLinkTimer, 1000, nullptr);
  ::InvalidateRect(shell_hwnd, nullptr, FALSE);
}

void PhiOmniboxSession::RevertCopyLinkFlash() {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  if (shell_hwnd) {
    ::KillTimer(shell_hwnd, kCopyLinkTimer);
  }
  copy_link_flash_ = false;
  if (shell_hwnd) {
    ::InvalidateRect(shell_hwnd, nullptr, FALSE);
  }
}

// --- Suggestion dropdown -------------------------------------------------

void PhiOmniboxSession::OnResults(BrowserId browser_id,
                                  const std::string& json) {
  if (browser_id != shell_->OmniboxBrowser() || !FocusedField()) {
    return;  // A stale query, or the user is no longer typing.
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  rows_.clear();
  selected_ = -1;
  hovered_ = -1;
  if (root) {
    if (const base::ListValue* matches = root->FindList("matches")) {
      for (const base::Value& v : *matches) {
        const base::DictValue* m = v.GetIfDict();
        if (!m || rows_.size() >= static_cast<size_t>(kMaxRows)) {
          continue;
        }
        Row row;
        row.line = m->FindInt("line").value_or(static_cast<int>(rows_.size()));
        if (const std::string* s = m->FindString("type")) {
          row.type = *s;
        }
        if (const std::string* s = m->FindString("contents")) {
          row.contents = base::UTF8ToWide(*s);
        }
        if (const std::string* s = m->FindString("description")) {
          row.description = base::UTF8ToWide(*s);
        }
        if (const std::string* s = m->FindString("fillIntoEdit")) {
          row.fill = base::UTF8ToWide(*s);
        }
        row.has_tab_match = m->FindBool("hasTabMatch").value_or(false);
        rows_.push_back(std::move(row));
      }
    }
  }
  if (rows_.empty()) {
    ClosePopup(/*stop_query=*/false);
  } else {
    ShowPopup();
  }
}

void PhiOmniboxSession::ShowPopup() {
  const HWND shell_hwnd = shell_->OmniboxShellWindow();
  const HWND focused = FocusedField();
  if (!shell_hwnd || !focused) {
    return;
  }
  EnsurePopupClass();
  if (!popup_) {
    popup_ = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               L"PhiOmniboxPopup", L"", WS_POPUP, 0, 0, 0, 0,
                               shell_hwnd, nullptr,
                               ::GetModuleHandleW(nullptr), this);
    if (popup_) {
      // The flyout look PhiPopover gives its windows: DWM rounds the corners
      // and casts the shadow, so nothing here draws a curve or a square
      // rim inside the rounding.
      const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
      ::DwmSetWindowAttribute(popup_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                              sizeof(corner));
    }
  }
  if (!popup_) {
    return;
  }
  // DWM's hairline follows the palette (a Space or theme switch moves it).
  const COLORREF border = shell_->OmniboxPalette().border;
  ::DwmSetWindowAttribute(popup_, DWMWA_BORDER_COLOR, &border, sizeof(border));
  const int height = PhiOmniboxPopupHeight(static_cast<int>(rows_.size()),
                                           Scaled(kRowHeight),
                                           Scaled(kPopupPad));
  RECT anchor = {};
  int left = 0;
  int width = 0;
  if (focused != field_ && shell_->OmniboxNativePageBox(&anchor)) {
    // Flush under the new-tab page's box and exactly as wide — the Mac's
    // suggestion list grows out of the box itself.
    left = anchor.left;
    width = anchor.right - anchor.left;
  } else {
    ::GetWindowRect(focused, &anchor);
    left = anchor.left - Scaled(8);
    width = std::max<int>(anchor.right - anchor.left + Scaled(16), Scaled(320));
  }
  ::SetWindowPos(popup_, HWND_TOP, left, anchor.bottom + Scaled(6), width,
                 height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
  ::InvalidateRect(popup_, nullptr, TRUE);
}

void PhiOmniboxSession::ClosePopup(bool stop_query) {
  rows_.clear();
  selected_ = -1;
  hovered_ = -1;
  if (stop_query) {
    new_tab_pending_ = false;  // The gesture was abandoned.
  }
  if (popup_ && ::IsWindow(popup_)) {
    ::ShowWindow(popup_, SW_HIDE);
  }
  const BrowserId browser = shell_->OmniboxBrowser();
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  if (stop_query && framework && browser.valid()) {
    framework->StopOmniboxQuery(browser.value);
  }
}

void PhiOmniboxSession::MoveSelection(int delta) {
  if (rows_.empty()) {
    return;
  }
  selected_ =
      PhiOmniboxNextSelection(selected_, delta, static_cast<int>(rows_.size()));
  const HWND focused = FocusedField();
  if (selected_ >= 0 && focused) {
    const Row& row = rows_[static_cast<size_t>(selected_)];
    if (!row.fill.empty()) {
      // The row's fillIntoEdit lands in the field, caret at the end — the
      // omnibox arrow behavior.
      programmatic_ = true;
      ::SetWindowTextW(focused, row.fill.c_str());
      ::SendMessageW(focused, EM_SETSEL, row.fill.size(), row.fill.size());
      programmatic_ = false;
    }
  }
  if (popup_) {
    ::InvalidateRect(popup_, nullptr, TRUE);
  }
}

void PhiOmniboxSession::CommitSelection() {
  CommitRow(selected_);
}

void PhiOmniboxSession::CommitRow(int index) {
  phi_framework::PhiFramework* framework = shell_->OmniboxFramework();
  if (index < 0 || index >= static_cast<int>(rows_.size()) || !framework) {
    return;
  }
  const int line = rows_[static_cast<size_t>(index)].line;
  // The framework opens it with Chromium's own semantics (fixup,
  // switch-to-tab, shortcuts learning). Omnibox new-tab mode opens it in a
  // FRESH tab.
  const bool new_tab = new_tab_pending_;
  new_tab_pending_ = false;
  const BrowserId browser = shell_->OmniboxBrowser();
  if (!browser.valid()) {
    return;
  }
  framework->OpenOmniboxMatch(browser.value, line, new_tab);
  ClosePopup(/*stop_query=*/false);
  shell_->OmniboxFocusContent();
}

int PhiOmniboxSession::RowAtY(int y) const {
  return PhiOmniboxRowAtY(y, static_cast<int>(rows_.size()), Scaled(kRowHeight),
                          Scaled(kPopupInset));
}

void PhiOmniboxSession::PopupHover(int y) {
  // The pointer only LIGHTS a row; it never arms it. Enter commits what the
  // field shows unless the arrows picked a row, so a cursor resting over the
  // list cannot redirect a typed URL.
  const int row = y < 0 ? -1 : RowAtY(y);
  if (row != hovered_) {
    hovered_ = row;
    if (popup_) {
      ::InvalidateRect(popup_, nullptr, TRUE);
    }
  }
}

void PhiOmniboxSession::PopupClick(int y) {
  // A click opens the row under it, whatever the arrows had armed.
  CommitRow(RowAtY(y));
}

void PhiOmniboxSession::PaintPopup(HWND popup, HDC dc) {
  RECT rc = {};
  ::GetClientRect(popup, &rc);
  const PhiPalette palette = shell_->OmniboxPalette();
  HBRUSH bg = ::CreateSolidBrush(palette.card);
  ::FillRect(dc, &rc, bg);
  ::DeleteObject(bg);
  // No rim here: DWM draws the rounded border and shadow (ShowPopup).
  ::SetBkMode(dc, TRANSPARENT);
  HFONT text_font = shell_->OmniboxTextFont();
  HFONT icon_font = shell_->OmniboxIconFont();
  for (size_t i = 0; i < rows_.size(); ++i) {
    const Row& row = rows_[i];
    RECT r = {rc.left + Scaled(6),
              rc.top + Scaled(kPopupInset) +
                  static_cast<int>(i) * Scaled(kRowHeight),
              rc.right - Scaled(6), 0};
    r.bottom = r.top + Scaled(kRowHeight);
    if (static_cast<int>(i) == selected_) {
      PhiFillRoundRectAA(dc, r, Scaled(6), palette.hover_strong);
    } else if (static_cast<int>(i) == hovered_) {
      PhiFillRoundRectAA(dc, r, Scaled(6), palette.hover);
    }
    // Type glyph: search rows a magnifier, bookmarks a star, history a
    // clock, everything else a globe.
    const wchar_t glyph =
        row.type.find("search") != std::string::npos    ? L''
        : row.type.find("bookmark") != std::string::npos ? L''
        : row.type.find("history") != std::string::npos  ? L''
                                                         : L'';
    RECT glyph_rc = {r.left + Scaled(4), r.top, r.left + Scaled(28), r.bottom};
    HGDIOBJ glyph_font = icon_font ? ::SelectObject(dc, icon_font) : nullptr;
    ::SetTextColor(dc, palette.label_dim);
    const wchar_t glyph_text[2] = {glyph, L'\0'};
    PhiDrawTextAA(dc, glyph_text, 1, &glyph_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (glyph_font) {
      ::SelectObject(dc, glyph_font);
    }
    RECT text_rc = {glyph_rc.right + Scaled(6), r.top, r.right - Scaled(8),
                    r.bottom};
    HGDIOBJ prev_font = text_font ? ::SelectObject(dc, text_font) : nullptr;
    ::SetTextColor(dc, palette.label);
    RECT measure = text_rc;
    PhiDrawTextAA(dc, row.contents.c_str(), -1, &measure,
                DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    RECT contents_rc = text_rc;
    contents_rc.right = std::min(
        text_rc.right,
        static_cast<LONG>(text_rc.left + (measure.right - measure.left)));
    PhiDrawTextAA(dc, row.contents.c_str(), -1, &contents_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                    DT_NOPREFIX);
    if (!row.description.empty() &&
        contents_rc.right + Scaled(16) < text_rc.right) {
      RECT desc_rc = {contents_rc.right + Scaled(8), r.top, text_rc.right,
                      r.bottom};
      ::SetTextColor(dc, palette.label_dim);
      const std::wstring desc = L"— " + row.description;
      PhiDrawTextAA(dc, desc.c_str(), -1, &desc_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
    }
    if (prev_font) {
      ::SelectObject(dc, prev_font);
    }
  }
}

bool PhiOmniboxSession::PaintField(HWND field) {
  RECT rc = {};
  ::GetClientRect(field, &rc);
  if (rc.right <= 0 || rc.bottom <= 0) {
    return false;
  }
  if (!field_surface_ || field_surface_->width() != rc.right ||
      field_surface_->height() != rc.bottom) {
    field_surface_ = std::make_unique<PhiLayerSurface>(rc.right, rc.bottom);
  }
  if (!field_surface_->valid()) {
    return false;
  }
  PAINTSTRUCT ps = {};
  HDC dc = ::BeginPaint(field, &ps);
  const HDC buffer = field_surface_->dc();
  const HBRUSH background = reinterpret_cast<HBRUSH>(::SendMessageW(
      ::GetParent(field), WM_CTLCOLOREDIT, reinterpret_cast<WPARAM>(buffer),
      reinterpret_cast<LPARAM>(field)));
  ::FillRect(buffer, &rc,
              background ? background : ::GetSysColorBrush(COLOR_WINDOW));
  // Let the native EDIT draw selection, scrolling, cue text and IME using
  // its parent's palette. Supply opaque alpha: zero-alpha GDI glyphs get
  // washed out when the shell's DWM frame extends beneath the address row.
  ::CallWindowProcW(g_edit_base_proc, field, WM_PRINTCLIENT,
                    reinterpret_cast<WPARAM>(buffer),
                    PRF_CLIENT | PRF_ERASEBKGND);
  ::GdiFlush();
  // SAFETY: PhiLayerSurface owns a tightly packed, top-down 32-bit DIB.
  auto pixels = UNSAFE_BUFFERS(base::span<uint32_t>(
      static_cast<uint32_t*>(field_surface_->bits()),
      field_surface_->byte_size() / 4));
  for (auto& pixel : pixels) {
    pixel |= 0xFF000000u;
  }
  ::BitBlt(dc, 0, 0, rc.right, rc.bottom, buffer, 0, 0, SRCCOPY);
  ::EndPaint(field, &ps);
  return true;
}

// static
LRESULT CALLBACK PhiOmniboxSession::FieldProc(HWND hwnd,
                                              UINT msg,
                                              WPARAM wparam,
                                              LPARAM lparam) {
  if (msg == WM_NCDESTROY) {
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);  // See ShellWndProc.
    return ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiOmniboxSession*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self && self->shell_->OmniboxShellDestroying()) {
    // The field dies with the shell; its WM_KILLFOCUS would still reach the
    // framework for a window that is going.
    return ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
  }
  switch (msg) {
    case WM_PAINT:
      if (self && self->PaintField(hwnd)) {
        return 0;
      }
      break;
    case WM_ERASEBKGND:
      if (self) {
        return 1;  // PaintField supplies the complete opaque background.
      }
      break;
    case WM_KEYDOWN:
      if (wparam == VK_RETURN) {
        if (self) {
          if (self->selected_ >= 0) {
            self->CommitSelection();
          } else {
            // stop_query=false: the omnibox-new-tab pending flag must
            // survive into the commit (the query dies with focus anyway).
            self->ClosePopup(/*stop_query=*/false);
            self->Commit(hwnd);
          }
        }
        return 0;
      }
      if (wparam == VK_ESCAPE) {
        if (self) {
          if (self->popup_ && ::IsWindowVisible(self->popup_)) {
            // First Escape closes the list; the second reverts the field.
            self->ClosePopup(/*stop_query=*/true);
            return 0;
          }
          if (hwnd != self->field_) {
            // The new-tab page's omnibox has no URL to revert to: Escape
            // clears what was typed and keeps the caret.
            ::SetWindowTextW(hwnd, L"");
            return 0;
          }
          self->Sync(/*force=*/true);
          self->shell_->OmniboxFocusContent();
        }
        return 0;
      }
      if ((wparam == VK_DOWN || wparam == VK_UP) && self &&
          !self->rows_.empty()) {
        self->MoveSelection(wparam == VK_DOWN ? 1 : -1);
        return 0;
      }
      if (wparam == VK_INSERT && self && ::GetKeyState(VK_SHIFT) < 0 &&
          ::GetKeyState(VK_CONTROL) >= 0) {
        self->PasteInto(hwnd);  // Shift+Insert: the EDIT's own paste.
        return 0;
      }
      break;
    case WM_CHAR:
      // Both keys are handled above; letting them reach the EDIT would only
      // produce the "unhandled character" beep.
      if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
        return 0;
      }
      if (wparam == 0x16 && self) {
        self->PasteInto(hwnd);  // Ctrl+V pastes inside the EDIT, not by
        return 0;               // WM_PASTE: sanitize it here too.
      }
      break;
    case WM_PASTE:
      if (self) {
        self->PasteInto(hwnd);
        return 0;
      }
      break;
    case WM_KILLFOCUS:
      // The user left the field: the dropdown has nothing to complete.
      if (self) {
        self->ClosePopup(/*stop_query=*/true);
      }
      break;
    case WM_MOUSEMOVE:
      // The capsule's hover brighten while the cursor rides the EDIT (the
      // child swallows the shell's mouse moves over the text area). The
      // new-tab page's field has no capsule.
      if (self && hwnd == self->field_ && !self->field_hover_) {
        self->SetFieldHover(true);
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      break;
    case WM_MOUSELEAVE:
      if (self && hwnd == self->field_) {
        self->SetFieldHover(false);
      }
      break;
    case WM_SETFOCUS: {
      const LRESULT result =
          ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
      ::SendMessageW(hwnd, EM_SETSEL, 0, -1);  // Typing replaces the whole URL.
      ::InvalidateRect(hwnd, nullptr, FALSE);
      return result;
    }
    case WM_CONTEXTMENU:
      if (self) {
        self->shell_->OmniboxFieldPressed();
        // The EDIT's stock menu is replaced by the Fluent flyout (§3.5).
        // Shift+F10 / the menu key arrive with (-1, -1): anchor those on
        // the caret's field.
        POINT screen = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (screen.x == -1 && screen.y == -1) {
          RECT rc = {};
          ::GetWindowRect(hwnd, &rc);
          screen = {rc.left + (rc.right - rc.left) / 3, rc.bottom};
        }
        self->ShowFieldMenu(hwnd, screen);
        return 0;
      }
      break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
      // A press on the field is a press on the shell's chrome that the
      // shell's own click handler never sees (the EDIT is a child window):
      // the extension popup closes on it as on any other chrome press.
      if (self) {
        self->shell_->OmniboxFieldPressed();
      }
      break;
  }
  // EDIT also draws directly while editing/selecting. Repaint that output
  // through the opaque buffer after the native operation has completed.
  const bool repaint = self &&
      (msg == WM_CHAR || msg == WM_KEYDOWN || msg == WM_SETTEXT ||
       msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_LBUTTONDBLCLK ||
       (msg == WM_MOUSEMOVE && (wparam & MK_LBUTTON)) ||
       msg == WM_KILLFOCUS || msg == EM_SETSEL || msg == EM_REPLACESEL ||
       msg == EM_UNDO || msg == WM_PASTE || msg == WM_CUT || msg == WM_CLEAR ||
       msg == WM_UNDO || msg == WM_IME_COMPOSITION);
  const LRESULT result =
      ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
  if (repaint && ::IsWindow(hwnd)) {
    ::InvalidateRect(hwnd, nullptr, FALSE);
  }
  return result;
}

}  // namespace phi_client
