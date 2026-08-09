// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_window_controller.h"

#include <windowsx.h>

#include <optional>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"

namespace phi_client {

namespace {

constexpr wchar_t kShellClassName[] = L"PhiClientShell";
constexpr int kSidebarWidth = 240;

// Placeholder sidebar colors (bold, so the client region is unmistakable).
constexpr COLORREF kBandColor = RGB(0x5A, 0x78, 0xFF);
constexpr COLORREF kAccentColor = RGB(0x1A, 0x2A, 0x88);
constexpr COLORREF kInkColor = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kButtonColor = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kButtonInk = RGB(0x2B, 0x2D, 0x31);

// The "+ New Window" button in the sidebar (client rect coords).
constexpr RECT kNewWindowButton = {16, 240, 224, 284};

// Toolbar: back / forward / reload buttons and the URL field.
constexpr RECT kBackButton    = {16, 190, 52, 226};
constexpr RECT kForwardButton = {56, 190, 92, 226};
constexpr RECT kReloadButton  = {96, 190, 132, 226};
constexpr RECT kUrlField      = {16, 146, 224, 182};
constexpr COLORREF kUrlFieldBg = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kUrlInk = RGB(0x2B, 0x2D, 0x31);
constexpr COLORREF kBtnEnabled = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kBtnDisabled = RGB(0x8C, 0xA4, 0xFF);

// Space chips row (between the button and the tab list).
constexpr int kSpacesTop = 296;
constexpr int kSpaceChipH = 32;
constexpr int kSpaceChipW = 64;
constexpr int kSpaceChipGap = 6;
constexpr COLORREF kSpaceActiveBg = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kSpaceActiveInk = RGB(0x2B, 0x2D, 0x31);
constexpr COLORREF kSpaceInk = RGB(0xE6, 0xEA, 0xFF);

// Tab list geometry, below the spaces row.
constexpr int kTabsTop = 374;  // below the chips row AND the create buttons
constexpr int kTabsInset = 12;
constexpr int kTabRowHeight = 30;
constexpr int kTabRowGap = 4;
constexpr int kCloseBoxSize = 16;
constexpr COLORREF kTabActiveBg = RGB(0xFF, 0xFF, 0xFF);
constexpr COLORREF kTabActiveInk = RGB(0x2B, 0x2D, 0x31);
constexpr COLORREF kTabInk = RGB(0xE6, 0xEA, 0xFF);

// The stock EDIT window proc, saved when the first address field is
// subclassed. One per process — every EDIT shares the class proc.
WNDPROC g_edit_base_proc = nullptr;

ATOM EnsureShellClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PhiWindowController::ShellWndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kShellClassName;
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

}  // namespace

PhiWindowController::PhiWindowController(int64_t window_id,
                                        HWND browser_hwnd,
                                        phi_framework::PhiFramework* framework,
                                        PhiSlotHost* host)
    : window_id_(window_id),
      browser_hwnd_(browser_hwnd),
      framework_(framework),
      host_(host) {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    LOG(ERROR) << "[PhiClient] adopt: invalid browser HWND for window "
               << window_id_;
    return;
  }
  EnsureShellClass();

  // Create the client shell top-level at the browser window's current bounds,
  // so the transition from Chromium's own frame to ours is in-place.
  RECT wr = {};
  ::GetWindowRect(browser_hwnd_, &wr);
  int x = wr.left, y = wr.top;
  int w = wr.right - wr.left, h = wr.bottom - wr.top;
  if (w <= 0 || h <= 0) {
    w = 1200;
    h = 800;
    x = CW_USEDEFAULT;
    y = CW_USEDEFAULT;
  }

  shell_hwnd_ = ::CreateWindowExW(
      0, kShellClassName, L"Phi", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w,
      h, nullptr, nullptr, ::GetModuleHandleW(nullptr), this);
  if (!shell_hwnd_) {
    LOG(ERROR) << "[PhiClient] shell CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }

  AdoptBrowserAsChild();
  EnsureUrlField();
  // The window the slot was created with backs the default Space.
  space_browsers_["1"] = window_id_;
  browser_hwnds_[window_id_] = browser_hwnd_;
  space_order_.push_back("1");
  active_space_ = "1";
  active_browser_ = window_id_;
  if (framework_) {
    framework_->SetWindowSpace(window_id_, "1");
  }
  LayoutContents();
  ::ShowWindow(shell_hwnd_, SW_SHOW);
  LOG(INFO) << "[PhiClient] shell window " << window_id_
            << " shell_hwnd=" << shell_hwnd_ << " browser_hwnd=" << browser_hwnd_;
}

PhiWindowController::~PhiWindowController() {
  // Detach the browser child before tearing down our window, so its teardown
  // is not entangled with ours. Full close plumbing is a later slice.
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetParent(browser_hwnd_, nullptr);
  }
  if (shell_hwnd_ && ::IsWindow(shell_hwnd_)) {
    ::DestroyWindow(shell_hwnd_);  // Takes the address field child with it.
  }
  if (url_font_) {
    ::DeleteObject(url_font_);
  }
}

void PhiWindowController::EnsureUrlField() {
  if (url_field_ || !shell_hwnd_) {
    return;
  }
  // The shell's message font, so the field looks like the rest of the system
  // rather than the EDIT default (the ancient bitmap "System" font).
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                              &metrics, 0)) {
    url_font_ = ::CreateFontIndirectW(&metrics.lfMessageFont);
  }
  // Sits INSIDE the painted field box, vertically centred: a single-line EDIT
  // draws its text at the top of its own rect, so filling the whole box would
  // leave the text hugging the upper edge.
  const int text_height = 20;
  const int top = kUrlField.top + (kUrlField.bottom - kUrlField.top -
                                   text_height) / 2;
  url_field_ = ::CreateWindowExW(
      0, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, kUrlField.left + 6,
      top, kUrlField.right - kUrlField.left - 12, text_height, shell_hwnd_,
      nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (!url_field_) {
    LOG(ERROR) << "[PhiClient] address field CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }
  if (url_font_) {
    ::SendMessageW(url_field_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(url_font_), TRUE);
  }
  ::SetWindowLongPtrW(url_field_, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
  WNDPROC previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
      url_field_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&UrlFieldProc)));
  if (!g_edit_base_proc) {
    g_edit_base_proc = previous;
  }
}

void PhiWindowController::SyncUrlField(bool force) {
  if (!url_field_) {
    return;
  }
  // Never overwrite a half-typed URL: nav state refreshes on every paint and
  // on every framework notification.
  if (!force && ::GetFocus() == url_field_) {
    return;
  }
  const int len = ::GetWindowTextLengthW(url_field_);
  std::vector<wchar_t> current(static_cast<size_t>(len) + 1, L'\0');
  if (len > 0) {
    ::GetWindowTextW(url_field_, current.data(), len + 1);
  }
  if (nav_url_ == current.data()) {
    return;  // Unchanged; setting it again would fight the caret.
  }
  ::SetWindowTextW(url_field_, nav_url_.c_str());
}

void PhiWindowController::CommitUrlField() {
  if (!url_field_ || !framework_) {
    return;
  }
  const int len = ::GetWindowTextLengthW(url_field_);
  std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
  if (len > 0) {
    ::GetWindowTextW(url_field_, buffer.data(), len + 1);
  }
  std::wstring typed(buffer.data());
  const size_t first = typed.find_first_not_of(L" \t");
  const size_t last = typed.find_last_not_of(L" \t");
  typed = (first == std::wstring::npos) ? std::wstring()
                                        : typed.substr(first, last - first + 1);
  if (typed.empty()) {
    return;
  }
  // The framework resolves what was typed the way the omnibox would — a bare
  // host becomes a URL, free text becomes a search.
  framework_->Navigate(active_browser_ ? active_browser_ : window_id_,
                       base::WideToUTF8(typed).c_str());
  FocusActiveBrowser();  // Keys belong to the page again once we've navigated.
  RefreshTabs();
}

// static
LRESULT CALLBACK PhiWindowController::UrlFieldProc(HWND hwnd,
                                                   UINT msg,
                                                   WPARAM wparam,
                                                   LPARAM lparam) {
  auto* self = reinterpret_cast<PhiWindowController*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_KEYDOWN:
      if (wparam == VK_RETURN) {
        if (self) {
          self->CommitUrlField();
        }
        return 0;
      }
      if (wparam == VK_ESCAPE) {
        if (self) {
          self->SyncUrlField(/*force=*/true);
          self->FocusActiveBrowser();
        }
        return 0;
      }
      break;
    case WM_CHAR:
      // Both keys are handled above; letting them reach the EDIT would only
      // produce the "unhandled character" beep.
      if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
        return 0;
      }
      break;
    case WM_SETFOCUS: {
      const LRESULT result =
          ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
      ::SendMessageW(hwnd, EM_SETSEL, 0, -1);  // Typing replaces the whole URL.
      return result;
    }
  }
  return ::CallWindowProcW(g_edit_base_proc, hwnd, msg, wparam, lparam);
}

void PhiWindowController::AdoptBrowserAsChild() {
  // Strip the top-level chrome/styles and make Chromium's window a clipped
  // child of our shell. SetParent on a WS_CHILD window is a true child reparent
  // (hwnd_message_handler.cc SetParentOrOwner), so it clips into our content
  // rect and stops being an independent taskbar/Alt-Tab entry.
  LONG_PTR style = ::GetWindowLongPtrW(browser_hwnd_, GWL_STYLE);
  style &= ~(WS_POPUP | WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME |
             WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
  style |= WS_CHILD | WS_CLIPSIBLINGS;
  ::SetWindowLongPtrW(browser_hwnd_, GWL_STYLE, style);
  // Drop the top-level extended frame edges too; without this + FRAMECHANGED,
  // Windows keeps the old non-client metrics and paints a dead band at the
  // top of the content region.
  LONG_PTR ex = ::GetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE);
  ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
          WS_EX_STATICEDGE | WS_EX_APPWINDOW);
  ::SetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE, ex);
  ::SetParent(browser_hwnd_, shell_hwnd_);
  ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);
}

void PhiWindowController::LayoutContents() {
  if (!shell_hwnd_) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(shell_hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    // Content fills everything right of the sidebar.
    ::SetWindowPos(browser_hwnd_, nullptr, kSidebarWidth, 0,
                   (w > kSidebarWidth) ? (w - kSidebarWidth) : 0, h,
                   SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
}

void PhiWindowController::RefreshTabs() {
  RebuildTabs();
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
    ::UpdateWindow(shell_hwnd_);  // Paint now, not at the next idle.
  }
}

void PhiWindowController::ScheduleRefresh() {
  if (shell_hwnd_) {
    ::InvalidateRect(shell_hwnd_, nullptr, FALSE);
  }
}

void PhiWindowController::AttachSpaceBrowser(const std::string& space_id,
                                             int64_t browser_window_id,
                                             HWND browser_hwnd) {
  if (!browser_hwnd || !::IsWindow(browser_hwnd)) {
    return;
  }
  // Reparent it into this shell exactly like the primary browser, then show it
  // in place of the current Space's.
  HWND previous = browser_hwnd_;
  browser_hwnd_ = browser_hwnd;
  AdoptBrowserAsChild();
  if (previous && previous != browser_hwnd && ::IsWindow(previous)) {
    ::ShowWindow(previous, SW_HIDE);
  }
  if (space_browsers_.find(space_id) == space_browsers_.end()) {
    space_order_.push_back(space_id);
  }
  space_browsers_[space_id] = browser_window_id;
  browser_hwnds_[browser_window_id] = browser_hwnd;
  active_space_ = space_id;
  active_browser_ = browser_window_id;
  if (framework_) {
    framework_->SetWindowSpace(browser_window_id, space_id.c_str());
  }
  LayoutContents();
  RefreshTabs();
}

void PhiWindowController::FocusActiveBrowser() {
  // Showing a different browser child is not enough: as a WS_CHILD it never
  // gets WM_ACTIVATE, so without this the keyboard still goes to the browser
  // that was visible before the Space switch (input appears dead until the
  // user clicks). Give the child Win32 focus AND drive Chromium's own
  // activation across the boundary.
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetFocus(browser_hwnd_);
  }
  if (framework_ && active_browser_) {
    framework_->FocusBrowserWindow(active_browser_);
  }
}

bool PhiWindowController::HasSpace(const std::string& space_id) const {
  return space_browsers_.find(space_id) != space_browsers_.end();
}

bool PhiWindowController::ShowSpace(const std::string& space_id) {
  auto it = space_browsers_.find(space_id);
  if (it == space_browsers_.end()) {
    return false;
  }
  auto hwnd_it = browser_hwnds_.find(it->second);
  if (hwnd_it == browser_hwnds_.end() || !::IsWindow(hwnd_it->second)) {
    return false;
  }
  if (browser_hwnd_ && browser_hwnd_ != hwnd_it->second &&
      ::IsWindow(browser_hwnd_)) {
    ::ShowWindow(browser_hwnd_, SW_HIDE);
  }
  browser_hwnd_ = hwnd_it->second;
  active_space_ = space_id;
  active_browser_ = it->second;
  LayoutContents();  // Also re-shows the newly visible child.
  FocusActiveBrowser();
  RefreshTabs();
  return true;
}

bool PhiWindowController::ForgetBrowser(int64_t browser_window_id) {
  browser_hwnds_.erase(browser_window_id);
  for (auto it = space_browsers_.begin(); it != space_browsers_.end();) {
    if (it->second == browser_window_id) {
      std::erase(space_order_, it->first);
      it = space_browsers_.erase(it);
    } else {
      ++it;
    }
  }
  if (active_browser_ == browser_window_id) {
    active_browser_ = 0;
    browser_hwnd_ = nullptr;
    // Fall back to any remaining Space so the slot stays usable.
    if (!space_order_.empty()) {
      ShowSpace(space_order_.front());
    }
  }
  return !space_browsers_.empty();
}

bool PhiWindowController::GetBounds(int* x, int* y, int* w, int* h) const {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return false;
  }
  RECT r = {};
  if (!::GetWindowRect(shell_hwnd_, &r)) {
    return false;
  }
  *x = r.left;
  *y = r.top;
  *w = r.right - r.left;
  *h = r.bottom - r.top;
  return true;
}

bool PhiWindowController::SetBounds(int x, int y, int w, int h) {
  if (!shell_hwnd_ || !::IsWindow(shell_hwnd_)) {
    return false;
  }
  return ::SetWindowPos(shell_hwnd_, nullptr, x, y, w, h,
                        SWP_NOZORDER | SWP_NOACTIVATE) != 0;
}

void PhiWindowController::RebuildNavState() {
  nav_url_.clear();
  can_go_back_ = can_go_forward_ = nav_loading_ = false;
  if (!framework_) {
    return;
  }
  std::optional<base::DictValue> st = base::JSONReader::ReadDict(
      framework_->NavStateJson(active_browser_ ? active_browser_ : window_id_), base::JSON_PARSE_RFC);
  if (!st) {
    return;
  }
  if (const std::string* u = st->FindString("url")) {
    nav_url_ = base::UTF8ToWide(*u);
  }
  can_go_back_ = st->FindBool("canGoBack").value_or(false);
  can_go_forward_ = st->FindBool("canGoForward").value_or(false);
  nav_loading_ = st->FindBool("loading").value_or(false);
  SyncUrlField(/*force=*/false);
}

void PhiWindowController::RebuildChips() {
  chips_.clear();
  if (!host_) {
    return;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(host_->AllSpacesJson(), base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_list()) {
    return;
  }
  int x = kTabsInset;
  for (const base::Value& v : parsed->GetList()) {
    if (!v.is_dict()) {
      continue;
    }
    const base::DictValue& d = v.GetDict();
    const std::string* id = d.FindString("id");
    if (!id) {
      continue;
    }
    if (x + kSpaceChipW > kSidebarWidth - kTabsInset) {
      break;  // Row is full; a scroller is future work.
    }
    SpaceChip chip;
    chip.id = *id;
    chip.label = L"S" + base::UTF8ToWide(*id);
    chip.open = HasSpace(*id);
    chip.bounds = {x, kSpacesTop, x + kSpaceChipW, kSpacesTop + kSpaceChipH};
    chips_.push_back(std::move(chip));
    x += kSpaceChipW + kSpaceChipGap;
  }
  // Two create buttons on the row below: same profile, and new profile.
  add_space_button_ = {kTabsInset, kSpacesTop + kSpaceChipH + 6,
                       kTabsInset + 100, kSpacesTop + kSpaceChipH + 34};
  add_profile_space_button_ = {kTabsInset + 106, kSpacesTop + kSpaceChipH + 6,
                               kSidebarWidth - kTabsInset,
                               kSpacesTop + kSpaceChipH + 34};
}

void PhiWindowController::RebuildTabs() {
  RebuildNavState();
  RebuildChips();
  tabs_.clear();
  if (framework_) {
    std::optional<base::Value> parsed = base::JSONReader::Read(
        framework_->ListTabsJson(active_browser_ ? active_browser_ : window_id_), base::JSON_PARSE_RFC);
    if (parsed && parsed->is_list()) {
      int y = kTabsTop;
      for (const base::Value& v : parsed->GetList()) {
        if (!v.is_dict()) {
          continue;
        }
        const base::DictValue& d = v.GetDict();
        TabRow row;
        row.index = d.FindInt("index").value_or(0);
        row.active = d.FindBool("active").value_or(false);
        const std::string* title = d.FindString("title");
        const std::string* url = d.FindString("url");
        std::string label = title && !title->empty()
                                ? *title
                                : (url ? *url : std::string("(untitled)"));
        row.title = base::UTF8ToWide(label);
        row.bounds = {kTabsInset, y, kSidebarWidth - kTabsInset,
                      y + kTabRowHeight};
        row.close_box = {row.bounds.right - kCloseBoxSize - 4,
                         y + (kTabRowHeight - kCloseBoxSize) / 2,
                         row.bounds.right - 4,
                         y + (kTabRowHeight + kCloseBoxSize) / 2};
        tabs_.push_back(std::move(row));
        y += kTabRowHeight + kTabRowGap;
      }
    }
  }
}

void PhiWindowController::OnSidebarClick(int x, int y) {
  // Clicking anywhere else in the sidebar means the user is done editing the
  // address; hand the keyboard back to the page (the field is a child window,
  // so clicks on it never reach here).
  if (url_field_ && ::GetFocus() == url_field_) {
    FocusActiveBrowser();
  }
  POINT pt = {x, y};
  if (framework_ && ::PtInRect(&kBackButton, pt)) {
    framework_->GoBack(active_browser_ ? active_browser_ : window_id_);
    RefreshTabs();
    return;
  }
  if (framework_ && ::PtInRect(&kForwardButton, pt)) {
    framework_->GoForward(active_browser_ ? active_browser_ : window_id_);
    RefreshTabs();
    return;
  }
  if (framework_ && ::PtInRect(&kReloadButton, pt)) {
    framework_->Reload(active_browser_ ? active_browser_ : window_id_, /*bypass_cache=*/false);
    RefreshTabs();
    return;
  }
  if (::PtInRect(&kNewWindowButton, pt)) {
    if (framework_) {
      framework_->NewWindow(window_id_);
    }
    return;
  }
  for (const SpaceChip& chip : chips_) {
    if (!::PtInRect(&chip.bounds, pt)) {
      continue;
    }
    if (chip.open) {
      ShowSpace(chip.id);
    } else if (host_) {
      // Not yet in this slot: the owner creates its browser in the Space's
      // profile and attaches it here.
      host_->RequestSwitchSpace(window_id_, chip.id);
    }
    RefreshTabs();
    return;
  }
  if (host_ && ::PtInRect(&add_space_button_, pt)) {
    host_->RequestCreateSpace(window_id_, /*new_profile=*/false);
    RefreshTabs();
    return;
  }
  if (host_ && ::PtInRect(&add_profile_space_button_, pt)) {
    host_->RequestCreateSpace(window_id_, /*new_profile=*/true);
    RefreshTabs();
    return;
  }
  for (const TabRow& row : tabs_) {
    if (!::PtInRect(&row.bounds, pt)) {
      continue;
    }
    if (framework_) {
      if (::PtInRect(&row.close_box, pt)) {
        framework_->CloseTab(active_browser_ ? active_browser_ : window_id_, row.index);
      } else {
        framework_->ActivateTab(active_browser_ ? active_browser_ : window_id_, row.index);
      }
    }
    RefreshTabs();
    return;
  }
}

void PhiWindowController::PaintSidebar(HDC dc,
                                       const RECT& client,
                                       bool include_child_controls) {
  RECT bar = {0, 0, kSidebarWidth, client.bottom};
  HBRUSH band = ::CreateSolidBrush(kBandColor);
  ::FillRect(dc, &bar, band);
  ::DeleteObject(band);

  RECT stripe = {kSidebarWidth - 3, 0, kSidebarWidth, client.bottom};
  HBRUSH accent = ::CreateSolidBrush(kAccentColor);
  ::FillRect(dc, &stripe, accent);
  ::DeleteObject(accent);

  std::wstring text =
      L"Phi  ·  window " + base::UTF8ToWide(std::to_string(window_id_));
  ::SetBkMode(dc, TRANSPARENT);
  ::SetTextColor(dc, kInkColor);
  RECT text_rc = {16, 20, kSidebarWidth - 12, kUrlField.top - 8};
  ::DrawTextW(dc, text.c_str(), -1, &text_rc, DT_LEFT | DT_TOP | DT_WORDBREAK);

  // URL field: the box behind the editable EDIT child. The child draws the
  // text on screen; on the capture path (no children rendered) we draw it here
  // so a screenshot still shows the address.
  HBRUSH url_bg = ::CreateSolidBrush(kUrlFieldBg);
  ::FillRect(dc, &kUrlField, url_bg);
  ::DeleteObject(url_bg);
  if (include_child_controls) {
    std::wstring shown = nav_url_;
    if (url_field_) {
      const int len = ::GetWindowTextLengthW(url_field_);
      std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
      if (len > 0) {
        ::GetWindowTextW(url_field_, buffer.data(), len + 1);
      }
      shown.assign(buffer.data());  // What the user actually sees, mid-edit.
    }
    ::SetTextColor(dc, kUrlInk);
    RECT url_rc = kUrlField;
    url_rc.left += 8;
    url_rc.right -= 8;
    ::DrawTextW(dc, shown.empty() ? L"(no page)" : shown.c_str(), -1, &url_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
  }

  // Back / forward / reload, greyed when unavailable.
  auto draw_button = [&](RECT rect, const wchar_t* glyph, bool enabled) {
    ::SetTextColor(dc, enabled ? kBtnEnabled : kBtnDisabled);
    ::DrawTextW(dc, glyph, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  };
  draw_button(kBackButton, L"<", can_go_back_);
  draw_button(kForwardButton, L">", can_go_forward_);
  draw_button(kReloadButton, nav_loading_ ? L"x" : L"R", true);

  // "+ New Window" button — proves the slot model (a click spawns a second
  // shell window via the framework).
  RECT btn = kNewWindowButton;
  HBRUSH btn_brush = ::CreateSolidBrush(kButtonColor);
  ::FillRect(dc, &btn, btn_brush);
  ::DeleteObject(btn_brush);
  ::SetTextColor(dc, kButtonInk);
  ::DrawTextW(dc, L"+  New Window", -1, &btn,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);

  // Space chips: EVERY Space (not only those open here) so any can be entered.
  for (const SpaceChip& chip : chips_) {
    const bool active = (chip.id == active_space_);
    if (active) {
      HBRUSH b = ::CreateSolidBrush(kSpaceActiveBg);
      ::FillRect(dc, &chip.bounds, b);
      ::DeleteObject(b);
    }
    ::SetTextColor(dc, active ? kSpaceActiveInk : kSpaceInk);
    ::DrawTextW(dc, chip.label.c_str(), -1, const_cast<RECT*>(&chip.bounds),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  // Create-Space buttons: same profile as the current Space, or a new profile.
  auto draw_pill = [&](RECT r, const wchar_t* text) {
    HBRUSH b = ::CreateSolidBrush(kButtonColor);
    ::FillRect(dc, &r, b);
    ::DeleteObject(b);
    ::SetTextColor(dc, kButtonInk);
    ::DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  };
  draw_pill(add_space_button_, L"+ Space");
  draw_pill(add_profile_space_button_, L"+ Space & profile");

  // Tab rows, driven by live framework state: the active tab gets a white
  // chip, each row a close box.
  for (const TabRow& row : tabs_) {
    if (row.bounds.bottom > client.bottom) {
      break;  // Out of sidebar space.
    }
    if (row.active) {
      HBRUSH chip = ::CreateSolidBrush(kTabActiveBg);
      ::FillRect(dc, &row.bounds, chip);
      ::DeleteObject(chip);
    }
    ::SetTextColor(dc, row.active ? kTabActiveInk : kTabInk);
    RECT title_rc = row.bounds;
    title_rc.left += 8;
    title_rc.right = row.close_box.left - 4;
    ::DrawTextW(dc, row.title.c_str(), -1, &title_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // Close affordance: a small x.
    ::DrawTextW(dc, L"x", -1, const_cast<RECT*>(&row.close_box),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
}

// static
LRESULT CALLBACK PhiWindowController::ShellWndProc(HWND hwnd,
                                                   UINT msg,
                                                   WPARAM wparam,
                                                   LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiWindowController*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_MOUSEACTIVATE: {
      // Let clicks activate without being eaten, and drive Chromium's own
      // activation across the boundary so input/focus/IME reach the page.
      // EXCEPT for clicks landing in our sidebar: the address field is a real
      // focusable control there, and handing focus to the page would fight the
      // click that is about to focus it.
      POINT cursor = {};
      ::GetCursorPos(&cursor);
      ::ScreenToClient(hwnd, &cursor);
      const bool in_sidebar = cursor.x < kSidebarWidth;
      if (!in_sidebar && self && self->browser_hwnd_ &&
          ::IsWindow(self->browser_hwnd_)) {
        ::SetFocus(self->browser_hwnd_);
        if (self->framework_) {
          self->framework_->FocusBrowserWindow(self->window_id_);
        }
      }
      return MA_ACTIVATE;
    }
    case WM_ACTIVATE:
    case WM_SETFOCUS:
      // Activation/focus arriving at the shell top-level must be forwarded to
      // the child: as a WS_CHILD, Chromium's window reports IsTopLevelWindow()
      // == false and skips its own activation path.
      if (self && self->browser_hwnd_ && ::IsWindow(self->browser_hwnd_) &&
          ::GetFocus() != self->url_field_ &&
          !(msg == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
        ::SetFocus(self->browser_hwnd_);
        if (self->framework_) {
          self->framework_->FocusBrowserWindow(self->window_id_);
        }
      }
      return 0;
    case WM_LBUTTONDOWN:
      if (self) {
        self->OnSidebarClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
      }
      return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    case WM_SIZE:
      if (self) {
        self->LayoutContents();
      }
      return 0;
    case WM_PRINTCLIENT: {
      // PrintWindow (used by screenshots/thumbnails) sends this instead of
      // WM_PAINT. Without handling it, a capture returns the LAST PAINTED
      // frame, not current state — so render fresh into the supplied DC.
      if (self) {
        HDC dc = reinterpret_cast<HDC>(wparam);
        self->RebuildTabs();
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        self->PaintSidebar(dc, rc, /*include_child_controls=*/true);
      }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        // Re-read framework state at paint time: the sidebar then reflects
        // reality even if a change notification was missed or coalesced.
        self->RebuildTabs();
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        self->PaintSidebar(dc, rc, /*include_child_controls=*/false);
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_CLOSE:
      // Route through Chromium's normal close path (unload handlers, session
      // bookkeeping). When the Browser actually goes away the framework fires
      // OnBrowserWindowClosing and the coordinator destroys this controller —
      // the shell window dies THEN, not here.
      if (self && self->framework_) {
        self->framework_->CloseBrowserWindow(self->window_id_);
      } else {
        ::DestroyWindow(hwnd);
      }
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace phi_client
