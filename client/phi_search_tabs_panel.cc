// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_search_tabs_panel.h"

#include "phibrowser-win/client/phi_dwrite.h"

#include <windows.h>

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

// Design metrics (DIP), the Mac panel's centered 680pt form.
constexpr int kPanelWidth = 680;
constexpr int kPanelHeight = 520;
constexpr int kFieldHeight = 40;
constexpr int kRowHeight = 36;
constexpr int kHeaderHeight = 26;
constexpr int kPad = 10;
// Section caps: a browse-y handful unfiltered, more once the user types.
constexpr size_t kUnfilteredCap = 6;
constexpr size_t kFilteredCap = 20;

WNDPROC g_search_edit_base_proc = nullptr;

// One visible line of the palette: either a section header or an openable
// result.
struct Row {
  enum class Kind { kHeader, kOpenTab, kPinned, kBookmark, kRecentlyClosed };
  Kind kind = Kind::kHeader;
  std::wstring title;
  std::wstring url;     // Dim, after the title.
  std::wstring detail;  // Right-aligned recency text.
  // Which of these means something depends on `kind`.
  int64_t tab_id = 0;
  int64_t window_id = 0;
  int64_t session_id = 0;
  std::string guid;
  std::string url_utf8;
};

std::wstring Lower(const std::wstring& text) {
  std::wstring out = text;
  ::CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
  return out;
}

// Every space-separated token must appear in the row's title or url.
bool Matches(const std::wstring& haystack_lower, const std::wstring& query) {
  size_t start = 0;
  while (start < query.size()) {
    size_t end = query.find(L' ', start);
    if (end == std::wstring::npos) {
      end = query.size();
    }
    if (end > start) {
      const std::wstring token = Lower(query.substr(start, end - start));
      if (haystack_lower.find(token) == std::wstring::npos) {
        return false;
      }
    }
    start = end + 1;
  }
  return true;
}

class Panel {
 public:
  explicit Panel(PhiWindowController* controller) : controller_(controller) {
    dpi_ = ::GetDpiForWindow(controller_->shell_hwnd());
    Register();
    const int w = Scaled(kPanelWidth);
    const int h = Scaled(kPanelHeight);
    RECT owner_rect = {};
    ::GetWindowRect(controller_->shell_hwnd(), &owner_rect);
    const int x =
        (owner_rect.left + owner_rect.right) / 2 - w / 2;
    const int y = owner_rect.top +
                  (owner_rect.bottom - owner_rect.top) / 5;
    hwnd_ = ::CreateWindowExW(WS_EX_TOOLWINDOW, L"PhiSearchTabsPanel", L"",
                              WS_POPUP, x, y, w, h,
                              controller_->shell_hwnd(), nullptr,
                              ::GetModuleHandleW(nullptr), this);
    EnsureFonts();
    FetchSources();
    BuildRows(std::wstring());
    // The search field: a real EDIT so IME/caret/clipboard just work.
    field_ = ::CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, Scaled(kPad) * 2,
        Scaled(kPad) + (Scaled(kFieldHeight) - Scaled(20)) / 2,
        w - Scaled(kPad) * 4, Scaled(20), hwnd_, nullptr,
        ::GetModuleHandleW(nullptr), nullptr);
    ::SendMessageW(field_, WM_SETFONT,
                   reinterpret_cast<WPARAM>(field_font_), TRUE);
    ::SetWindowLongPtrW(field_, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(this));
    WNDPROC previous = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
        field_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FieldProc)));
    if (!g_search_edit_base_proc) {
      g_search_edit_base_proc = previous;
    }
    ::SendMessageW(field_, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"Search tabs, bookmarks..."));
    created_tick_ = ::GetTickCount64();
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    ::SetFocus(field_);
  }

  ~Panel() {
    for (HFONT font : {field_font_, row_font_, dim_font_, header_font_}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
    if (field_brush_) {
      ::DeleteObject(field_brush_);
    }
  }

  HWND hwnd() const { return hwnd_; }
  PhiWindowController* controller() const { return controller_; }

  static Panel* instance;

 private:
  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }

  void Register() {
    static bool registered = [] {
      WNDCLASSEXW wc = {};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = &PanelProc;
      wc.hInstance = ::GetModuleHandleW(nullptr);
      wc.lpszClassName = L"PhiSearchTabsPanel";
      return ::RegisterClassExW(&wc) != 0;
    }();
    (void)registered;
  }

  void EnsureFonts() {
    const auto make = [&](int size, int weight) {
      return ::CreateFontW(-Scaled(size), 0, 0, 0, weight, FALSE, FALSE,
                           FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
    };
    field_font_ = make(15, FW_NORMAL);
    row_font_ = make(13, FW_NORMAL);
    dim_font_ = make(13, FW_NORMAL);
    header_font_ = make(11, FW_SEMIBOLD);
  }

  // Reads every source ONCE at open; typing only re-filters.
  void FetchSources() {
    open_tabs_.clear();
    pinned_.clear();
    bookmarks_.clear();
    recently_closed_.clear();
    PhiSlotHost* host = controller_->host();
    phi_framework::PhiFramework* framework = controller_->framework();
    const int64_t browser = controller_->active_browser_id().value;
    if (framework && browser) {
      std::optional<base::DictValue> data = base::JSONReader::ReadDict(
          framework->SearchTabsDataJson(browser), base::JSON_PARSE_RFC);
      if (data) {
        if (const base::ListValue* tabs = data->FindList("openTabs")) {
          for (const base::Value& v : *tabs) {
            const base::DictValue* d = v.GetIfDict();
            if (!d) {
              continue;
            }
            Row row;
            row.kind = Row::Kind::kOpenTab;
            if (const std::string* s = d->FindString("title")) {
              row.title = base::UTF8ToWide(*s);
            }
            if (const std::string* s = d->FindString("url")) {
              row.url = base::UTF8ToWide(*s);
            }
            if (const std::string* s = d->FindString("lastActiveText")) {
              row.detail = base::UTF8ToWide(*s);
            }
            row.tab_id =
                static_cast<int64_t>(d->FindDouble("tabId").value_or(0));
            row.window_id =
                static_cast<int64_t>(d->FindDouble("windowId").value_or(0));
            open_tabs_.push_back(std::move(row));
          }
        }
        if (const base::ListValue* closed =
                data->FindList("recentlyClosed")) {
          for (const base::Value& v : *closed) {
            const base::DictValue* d = v.GetIfDict();
            if (!d) {
              continue;
            }
            Row row;
            row.kind = Row::Kind::kRecentlyClosed;
            if (const std::string* s = d->FindString("title")) {
              row.title = base::UTF8ToWide(*s);
            }
            if (const std::string* s = d->FindString("url")) {
              row.url = base::UTF8ToWide(*s);
            }
            if (const std::string* s = d->FindString("lastActiveText")) {
              row.detail = base::UTF8ToWide(*s);
            }
            row.session_id =
                static_cast<int64_t>(d->FindDouble("sessionId").value_or(0));
            recently_closed_.push_back(std::move(row));
          }
        }
      }
    }
    if (host) {
      for (const PhiStoredRowView& pin :
           host->PinnedRows(controller_->slot().value)) {
        Row row;
        row.kind = Row::Kind::kPinned;
        row.title =
            base::UTF8ToWide(!pin.title.empty() ? pin.title : pin.url);
        row.url = base::UTF8ToWide(pin.url);
        row.guid = pin.guid;
        row.url_utf8 = pin.url;
        pinned_.push_back(std::move(row));
      }
      // Bookmarks flatten depth-first, folders skipped (the palette opens
      // pages, not folders — the Mac drills folders via a submenu we skip).
      const auto descend = [&](const auto& self,
                               const std::string& parent) -> void {
        for (const PhiStoredRowView& view :
             host->BookmarkRows(controller_->slot().value, parent)) {
          if (view.folder) {
            self(self, view.guid);
            continue;
          }
          if (view.url.empty()) {
            continue;
          }
          Row row;
          row.kind = Row::Kind::kBookmark;
          row.title =
              base::UTF8ToWide(!view.title.empty() ? view.title : view.url);
          row.url = base::UTF8ToWide(view.url);
          row.guid = view.guid;
          row.url_utf8 = view.url;
          bookmarks_.push_back(std::move(row));
        }
      };
      descend(descend, std::string());
    }
  }

  void AppendSection(const wchar_t* header, const std::vector<Row>& source,
                     const std::wstring& query) {
    const size_t cap = query.empty() ? kUnfilteredCap : kFilteredCap;
    size_t added = 0;
    for (const Row& row : source) {
      if (added >= cap) {
        break;
      }
      if (!query.empty() && !Matches(Lower(row.title + L" " + row.url),
                                     query)) {
        continue;
      }
      if (added == 0) {
        Row head;
        head.kind = Row::Kind::kHeader;
        head.title = header;
        rows_.push_back(std::move(head));
      }
      rows_.push_back(row);
      ++added;
    }
  }

  void BuildRows(const std::wstring& query) {
    rows_.clear();
    selected_ = -1;
    AppendSection(L"Open Tabs", open_tabs_, query);
    AppendSection(L"Pinned Tabs", pinned_, query);
    AppendSection(L"Bookmarks", bookmarks_, query);
    AppendSection(L"Recently Closed", recently_closed_, query);
    // Arm the first openable row, the Mac's default selection.
    for (size_t i = 0; i < rows_.size(); ++i) {
      if (rows_[i].kind != Row::Kind::kHeader) {
        selected_ = static_cast<int>(i);
        break;
      }
    }
    scroll_ = 0;
  }

  void Requery() {
    const int len = ::GetWindowTextLengthW(field_);
    std::vector<wchar_t> buffer(static_cast<size_t>(len) + 1, L'\0');
    if (len > 0) {
      ::GetWindowTextW(field_, buffer.data(), len + 1);
    }
    BuildRows(buffer.data());
    ::InvalidateRect(hwnd_, nullptr, TRUE);
  }

  RECT ListRect() const {
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    rc.top += Scaled(kPad) + Scaled(kFieldHeight);
    rc.left += Scaled(kPad);
    rc.right -= Scaled(kPad);
    rc.bottom -= Scaled(kPad);
    return rc;
  }

  int RowTop(size_t index) const {
    int y = 0;
    for (size_t i = 0; i < index; ++i) {
      y += rows_[i].kind == Row::Kind::kHeader ? Scaled(kHeaderHeight)
                                               : Scaled(kRowHeight);
    }
    return y;
  }

  int RowAtY(int client_y) const {
    const RECT list = ListRect();
    int y = list.top - scroll_;
    for (size_t i = 0; i < rows_.size(); ++i) {
      const int h = rows_[i].kind == Row::Kind::kHeader
                        ? Scaled(kHeaderHeight)
                        : Scaled(kRowHeight);
      if (client_y >= y && client_y < y + h) {
        return rows_[i].kind == Row::Kind::kHeader ? -1
                                                   : static_cast<int>(i);
      }
      y += h;
    }
    return -1;
  }

  void MoveSelection(int direction) {
    if (rows_.empty()) {
      return;
    }
    int i = selected_;
    const int count = static_cast<int>(rows_.size());
    for (int step = 0; step < count; ++step) {
      i += direction;
      if (i < 0) {
        i = count - 1;
      } else if (i >= count) {
        i = 0;
      }
      if (rows_[static_cast<size_t>(i)].kind != Row::Kind::kHeader) {
        selected_ = i;
        EnsureVisible(static_cast<size_t>(i));
        ::InvalidateRect(hwnd_, nullptr, TRUE);
        return;
      }
    }
  }

  void EnsureVisible(size_t index) {
    const RECT list = ListRect();
    const int view = list.bottom - list.top;
    const int top = RowTop(index);
    const int bottom = top + Scaled(kRowHeight);
    if (top - scroll_ < 0) {
      scroll_ = top;
    } else if (bottom - scroll_ > view) {
      scroll_ = bottom - view;
    }
  }

  void Commit() {
    if (selected_ < 0 ||
        selected_ >= static_cast<int>(rows_.size())) {
      return;
    }
    const Row row = rows_[static_cast<size_t>(selected_)];
    PhiWindowController* controller = controller_;
    ::DestroyWindow(hwnd_);  // First — the action may refocus other windows.
    PhiSlotHost* host = controller->host();
    phi_framework::PhiFramework* framework = controller->framework();
    const int64_t browser = controller->active_browser_id().value;
    switch (row.kind) {
      case Row::Kind::kOpenTab:
        if (framework && framework->ActivateSearchTab(row.tab_id) && host) {
          // The tab may live in another window (or a hidden Space) — the
          // client surfaces the right shell.
          host->FocusWindowForBrowser(row.window_id);
        }
        break;
      case Row::Kind::kPinned:
        controller->OpenOrFocusPinned(row.guid, row.url_utf8);
        break;
      case Row::Kind::kBookmark:
        if (host) {
          host->RequestOpen(controller->slot().value, row.url_utf8);
        }
        break;
      case Row::Kind::kRecentlyClosed:
        if (framework && browser) {
          framework->OpenRecentlyClosedEntry(browser, row.session_id);
        }
        break;
      case Row::Kind::kHeader:
        break;
    }
  }

  void Paint(HDC dc) {
    RECT rc = {};
    ::GetClientRect(hwnd_, &rc);
    const PhiPalette palette = controller_->CurrentPalette();
    HBRUSH bg = ::CreateSolidBrush(palette.card);
    ::FillRect(dc, &rc, bg);
    ::DeleteObject(bg);
    HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
    HGDIOBJ old_pen = ::SelectObject(dc, pen);
    HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, rc.left, rc.top, rc.right, rc.bottom);
    // A hairline under the search field.
    ::MoveToEx(dc, rc.left,
               rc.top + Scaled(kPad) + Scaled(kFieldHeight) - Scaled(4),
               nullptr);
    ::LineTo(dc, rc.right,
             rc.top + Scaled(kPad) + Scaled(kFieldHeight) - Scaled(4));
    ::SelectObject(dc, old_brush);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(pen);

    ::SetBkMode(dc, TRANSPARENT);
    const RECT list = ListRect();
    ::IntersectClipRect(dc, list.left, list.top, list.right, list.bottom);
    int y = list.top - scroll_;
    for (size_t i = 0; i < rows_.size(); ++i) {
      const Row& row = rows_[i];
      if (row.kind == Row::Kind::kHeader) {
        RECT r = {list.left + Scaled(6), y, list.right - Scaled(6),
                  y + Scaled(kHeaderHeight)};
        ::SelectObject(dc, header_font_);
        ::SetTextColor(dc, palette.label_dim);
        ::DrawTextW(dc, row.title.c_str(), -1, &r,
                    DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
        y += Scaled(kHeaderHeight);
        continue;
      }
      RECT r = {list.left, y, list.right, y + Scaled(kRowHeight)};
      if (static_cast<int>(i) == selected_) {
        PhiFillRoundRectAA(dc, r, Scaled(6), palette.hover_strong);
      }
      // Recency right-aligned; title then dim url fill the rest.
      RECT detail_rc = {r.right - Scaled(150), r.top, r.right - Scaled(10),
                        r.bottom};
      if (!row.detail.empty()) {
        ::SelectObject(dc, dim_font_);
        ::SetTextColor(dc, palette.label_tertiary);
        ::DrawTextW(dc, row.detail.c_str(), -1, &detail_rc,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }
      RECT text_rc = {r.left + Scaled(10), r.top,
                      detail_rc.left - Scaled(8), r.bottom};
      ::SelectObject(dc, row_font_);
      ::SetTextColor(dc, palette.label);
      RECT measure = text_rc;
      ::DrawTextW(dc, row.title.c_str(), -1, &measure,
                  DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      RECT title_rc = text_rc;
      title_rc.right = std::min(
          text_rc.right,
          static_cast<LONG>(text_rc.left + (measure.right - measure.left)));
      ::DrawTextW(dc, row.title.c_str(), -1, &title_rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                      DT_NOPREFIX);
      if (!row.url.empty() && title_rc.right + Scaled(16) < text_rc.right) {
        RECT url_rc = {title_rc.right + Scaled(8), r.top, text_rc.right,
                       r.bottom};
        ::SetTextColor(dc, palette.label_dim);
        const std::wstring dim = L"— " + row.url;
        ::DrawTextW(dc, dim.c_str(), -1, &url_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
      }
      y += Scaled(kRowHeight);
    }
    ::SelectClipRgn(dc, nullptr);
  }

  static LRESULT CALLBACK FieldProc(HWND hwnd, UINT msg, WPARAM wparam,
                                    LPARAM lparam) {
    Panel* self = reinterpret_cast<Panel*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
      case WM_KEYDOWN:
        if (!self) {
          break;
        }
        if (wparam == VK_DOWN || wparam == VK_UP) {
          self->MoveSelection(wparam == VK_DOWN ? 1 : -1);
          return 0;
        }
        if (wparam == VK_RETURN) {
          self->Commit();  // Destroys the panel.
          return 0;
        }
        if (wparam == VK_ESCAPE) {
          ::DestroyWindow(self->hwnd_);
          return 0;
        }
        break;
      case WM_CHAR:
        if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
          return 0;  // Handled above; the EDIT would only beep.
        }
        break;
    }
    return ::CallWindowProcW(g_search_edit_base_proc, hwnd, msg, wparam,
                             lparam);
  }

  static LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM w,
                                    LPARAM l) {
    Panel* self = instance && instance->hwnd_ == hwnd ? instance : nullptr;
    switch (msg) {
      case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        break;
      }
      case WM_ACTIVATE:
        if (self && LOWORD(w) == WA_INACTIVE) {
          // Opening grace absorbs the invoker's activation churn.
          if (::GetTickCount64() - self->created_tick_ >= 500) {
            ::DestroyWindow(hwnd);
            return 0;
          }
        }
        break;
      case WM_COMMAND:
        if (self && reinterpret_cast<HWND>(l) == self->field_ &&
            HIWORD(w) == EN_CHANGE) {
          self->Requery();
          return 0;
        }
        break;
      case WM_CTLCOLOREDIT:
        if (self) {
          const PhiPalette palette = self->controller_->CurrentPalette();
          HDC edit_dc = reinterpret_cast<HDC>(w);
          ::SetTextColor(edit_dc, palette.label);
          ::SetBkColor(edit_dc, palette.card);
          if (!self->field_brush_) {
            self->field_brush_ = ::CreateSolidBrush(palette.card);
          }
          return reinterpret_cast<LRESULT>(self->field_brush_);
        }
        break;
      case WM_MOUSEMOVE:
        if (self) {
          const int row = self->RowAtY(GET_Y_LPARAM(l));
          if (row >= 0 && row != self->selected_) {
            self->selected_ = row;
            ::InvalidateRect(hwnd, nullptr, TRUE);
          }
        }
        return 0;
      case WM_LBUTTONDOWN:
        if (self) {
          const int row = self->RowAtY(GET_Y_LPARAM(l));
          if (row >= 0) {
            self->selected_ = row;
            self->Commit();  // Destroys the panel.
          }
        }
        return 0;
      case WM_MOUSEWHEEL:
        if (self) {
          self->scroll_ = std::max(
              0, self->scroll_ - GET_WHEEL_DELTA_WPARAM(w) / 2);
          ::InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
      case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = ::BeginPaint(hwnd, &ps);
        if (self) {
          self->Paint(dc);
        }
        ::EndPaint(hwnd, &ps);
        return 0;
      }
      case WM_DESTROY:
        if (self) {
          Panel::instance = nullptr;
          delete self;
        }
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, w, l);
  }

  RAW_PTR_EXCLUSION PhiWindowController* const controller_;
  UINT dpi_ = 96;
  HWND hwnd_ = nullptr;
  HWND field_ = nullptr;
  HFONT field_font_ = nullptr;
  HFONT row_font_ = nullptr;
  HFONT dim_font_ = nullptr;
  HFONT header_font_ = nullptr;
  HBRUSH field_brush_ = nullptr;
  ULONGLONG created_tick_ = 0;
  std::vector<Row> open_tabs_;
  std::vector<Row> pinned_;
  std::vector<Row> bookmarks_;
  std::vector<Row> recently_closed_;
  std::vector<Row> rows_;
  int selected_ = -1;
  int scroll_ = 0;
};

Panel* Panel::instance = nullptr;

}  // namespace

// static
void PhiSearchTabsPanel::Toggle(PhiWindowController* controller) {
  if (Panel::instance) {
    ::DestroyWindow(Panel::instance->hwnd());
    return;
  }
  if (!controller) {
    return;
  }
  Panel::instance = new Panel(controller);
}

// static
void PhiSearchTabsPanel::CloseIfBoundTo(const PhiWindowController* controller) {
  if (Panel::instance && Panel::instance->controller() == controller) {
    // WM_DESTROY deletes the panel and clears the singleton.
    ::DestroyWindow(Panel::instance->hwnd());
  }
}

}  // namespace phi_client
