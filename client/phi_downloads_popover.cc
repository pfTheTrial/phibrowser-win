// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_downloads_popover.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "base/compiler_specific.h"
#include "base/json/json_reader.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_dwrite.h"

namespace phi_client {

namespace {

// DownloadsListView's metrics, in DIP.
constexpr int kPanelWidth = 340;
constexpr int kPanelHeight = 317;
constexpr int kRowHeight = 56;
constexpr int kListPad = 12;   // Horizontal list padding.
constexpr int kIconSize = 30;
constexpr int kIconGap = 12;
constexpr int kActionButton = 24;
constexpr int kBottomBar = 33;

constexpr UINT_PTR kPollTimer = 0x521;  // 500ms live refresh while open.

COLORREF BlendOver(COLORREF base_color, COLORREF over, int alpha) {
  const int r = (GetRValue(over) * alpha + GetRValue(base_color) *
                 (255 - alpha)) / 255;
  const int g = (GetGValue(over) * alpha + GetGValue(base_color) *
                 (255 - alpha)) / 255;
  const int b = (GetBValue(over) * alpha + GetBValue(base_color) *
                 (255 - alpha)) / 255;
  return RGB(r, g, b);
}

void FillRect(HDC dc, const RECT& r, COLORREF color) {
  HBRUSH brush = ::CreateSolidBrush(color);
  ::FillRect(dc, &r, brush);
  ::DeleteObject(brush);
}

void FillRounded(HDC dc, const RECT& r, COLORREF color, int radius) {
  PhiFillRoundRectAA(dc, r, radius, color);
}

std::wstring FormatBytes(double bytes) {
  static constexpr std::array<const wchar_t*, 4> kUnits = {L"B", L"KB",
                                                           L"MB", L"GB"};
  size_t unit = 0;
  while (bytes >= 1024.0 && unit + 1 < kUnits.size()) {
    bytes /= 1024.0;
    ++unit;
  }
  const double rounded =
      unit == 0 ? bytes : std::round(bytes * 10.0) / 10.0;
  std::wstring text = std::to_wstring(rounded);
  const size_t dot = text.find(L'.');
  if (dot != std::wstring::npos) {
    text.resize(unit == 0 ? dot : dot + 2);
  }
  return text + L" " + kUnits[unit];
}

// DownloadItemRow's friendly completion time.
std::wstring FormatRelative(double unix_seconds) {
  const time_t now = ::time(nullptr);
  const double delta = now - unix_seconds;
  if (delta < 60) {
    return L"Just now";
  }
  if (delta < 3600) {
    return std::to_wstring(static_cast<int>(delta / 60)) + L" min ago";
  }
  if (delta < 12 * 3600) {
    return std::to_wstring(static_cast<int>(delta / 3600)) + L" hour ago";
  }
  if (delta < 3 * 86400) {
    const int days = static_cast<int>(delta / 86400);
    if (days == 0) {
      return L"Today";
    }
    if (days == 1) {
      return L"Yesterday";
    }
    return std::to_wstring(days) + L" days ago";
  }
  const time_t when = static_cast<time_t>(unix_seconds);
  tm parts;
  ::localtime_s(&parts, &when);
  wchar_t buffer[64];
  ::wcsftime(buffer, 64, L"%b %d, %Y", &parts);
  return buffer;
}

struct DownloadRow {
  int id = 0;
  std::wstring name;
  std::wstring host;
  std::wstring path;
  std::wstring url;
  std::string state;
  bool paused = false;
  bool exists = true;
  double received = 0;
  double total = 0;
  double speed = 0;
  double end_time = 0;
  // "normal|scanning|warning|blocked|policyBlocked" — the framework's
  // DownloadSafetyState (the Mac safety-state mapping).
  std::string danger = "normal";
};

struct RowAction {
  const char* verb;     // Framework DownloadAction verb, or "copy".
  wchar_t glyph;        // MDL2.
  bool enabled = true;
};

class Popover {
 public:
  static Popover* instance;
  static HWND last_owner;
  static ULONGLONG closed_tick;

  Popover(HWND owner, const RECT& anchor, const PhiPalette& palette,
          phi_framework::PhiFramework* framework, int64_t browser_id)
      : owner_(owner), palette_(palette), framework_(framework),
        browser_id_(browser_id) {
    dpi_ = ::GetDpiForWindow(owner);
    Register();
    const int w = Scaled(kPanelWidth);
    const int h = Scaled(kPanelHeight);
    int x = anchor.left;
    int y = anchor.top - h - Scaled(8);
    HMONITOR monitor = ::MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info = {sizeof(info)};
    if (::GetMonitorInfoW(monitor, &info)) {
      x = std::max(static_cast<int>(info.rcWork.left),
                   std::min(x, static_cast<int>(info.rcWork.right) - w));
      y = std::max(static_cast<int>(info.rcWork.top), y);
    }
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW, L"PhiDownloadsPopover", L"", WS_POPUP, x, y, w, h,
        owner, nullptr, ::GetModuleHandleW(nullptr), this);
    ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE,
                            &kCorner, sizeof(kCorner));
    EnsureFonts();
    Refresh();
    created_tick_ = ::GetTickCount64();
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    ::SetTimer(hwnd_, kPollTimer, 500, nullptr);
  }

  ~Popover() {
    for (auto& [ext, icon] : icons_) {
      if (icon) {
        ::DestroyIcon(icon);
      }
    }
    for (HFONT font : {name_font_, name_strike_font_, small_font_,
                       small_bold_font_, glyph_font_, empty_font_}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
  }

  HWND hwnd() const { return hwnd_; }

 private:
  static inline const DWM_WINDOW_CORNER_PREFERENCE kCorner = DWMWCP_ROUND;

  int Scaled(int design) const { return ::MulDiv(design, dpi_, 96); }

  static void Register() {
    static bool done = false;
    if (done) {
      return;
    }
    done = true;
    WNDCLASSW cls = {};
    cls.lpfnWndProc = &Popover::WndProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = L"PhiDownloadsPopover";
    ::RegisterClassW(&cls);
  }

  void EnsureFonts() {
    auto make = [&](int size, int weight, bool strike, const wchar_t* face) {
      return ::CreateFontW(-Scaled(size), 0, 0, 0, weight, FALSE, FALSE,
                           strike, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, face);
    };
    name_font_ = make(13, FW_NORMAL, false, L"Segoe UI");
    name_strike_font_ = make(13, FW_NORMAL, true, L"Segoe UI");
    small_font_ = make(11, FW_NORMAL, false, L"Segoe UI");
    small_bold_font_ = make(11, FW_BOLD, false, L"Segoe UI");
    glyph_font_ = make(12, FW_NORMAL, false, L"Segoe MDL2 Assets");
    empty_font_ = make(32, FW_NORMAL, false, L"Segoe MDL2 Assets");
  }

  void Refresh() {
    const char* json =
        framework_ ? framework_->ListDownloadsJson(browser_id_) : "[]";
    if (json == last_json_) {
      return;
    }
    last_json_ = json;
    rows_.clear();
    std::optional<base::Value> parsed =
        base::JSONReader::Read(last_json_, base::JSON_PARSE_RFC);
    if (parsed && parsed->is_list()) {
      for (const base::Value& value : parsed->GetList()) {
        const base::DictValue* dict = value.GetIfDict();
        if (!dict) {
          continue;
        }
        DownloadRow row;
        row.id = dict->FindInt("id").value_or(0);
        row.name = base::UTF8ToWide(*dict->FindString("name"));
        if (const std::string* host = dict->FindString("host")) {
          row.host = base::UTF8ToWide(*host);
        }
        if (const std::string* path = dict->FindString("path")) {
          row.path = base::UTF8ToWide(*path);
        }
        if (const std::string* url = dict->FindString("url")) {
          row.url = base::UTF8ToWide(*url);
        }
        if (const std::string* state = dict->FindString("state")) {
          row.state = *state;
        }
        row.paused = dict->FindBool("paused").value_or(false);
        row.exists = dict->FindBool("exists").value_or(true);
        row.received = dict->FindDouble("received").value_or(0);
        row.total = dict->FindDouble("total").value_or(0);
        row.speed = dict->FindDouble("speed").value_or(0);
        row.end_time = dict->FindDouble("endTime").value_or(0);
        if (const std::string* danger = dict->FindString("danger")) {
          row.danger = *danger;
        }
        rows_.push_back(std::move(row));
      }
    }
    const int list_h = Scaled(kPanelHeight - kBottomBar);
    max_scroll_ = std::max(
        0, static_cast<int>(rows_.size()) * Scaled(kRowHeight) - list_h);
    scroll_ = std::min(scroll_, max_scroll_);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
  }

  // The per-state action set (DownloadItemRow's normalActionButtons).
  std::vector<RowAction> ActionsFor(const DownloadRow& row) const {
    // Safe Browsing states first (the Mac popover's keep/discard): a
    // warning offers Keep + Discard, a block offers Discard only.
    // "remove" IS the discard — Chromium's bubble does the same (cancel +
    // delete the temporary file). Scanning rows keep the normal actions.
    if (row.danger == "warning") {
      return {{"keep", L''}, {"remove", L''}};
    }
    if (row.danger == "blocked" || row.danger == "policyBlocked") {
      return {{"remove", L''}};
    }
    if (row.state == "in_progress") {
      return {{"copy", L''},
              {row.paused ? "resume" : "pause",
               row.paused ? L'' : L''},
              {"cancel", L''}};
    }
    if (row.state == "complete") {
      return {{"copy", L''},
              {"show", L'', row.exists},
              {"remove", L''}};
    }
    return {{"copy", L''}, {"remove", L''}};
  }

  HICON IconFor(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : path.substr(dot);
    auto found = icons_.find(ext);
    if (found != icons_.end()) {
      return found->second;
    }
    SHFILEINFOW info = {};
    ::SHGetFileInfoW((L"file" + ext).c_str(), FILE_ATTRIBUTE_NORMAL, &info,
                     sizeof(info),
                     SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
    icons_[ext] = info.hIcon;
    return info.hIcon;
  }

  RECT ListRect() const {
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    rc.bottom -= Scaled(kBottomBar);
    return rc;
  }

  RECT RowRect(int index) const {
    const RECT list = ListRect();
    const int top = list.top + index * Scaled(kRowHeight) - scroll_;
    return {list.left + Scaled(kListPad), top,
            list.right - Scaled(kListPad), top + Scaled(kRowHeight)};
  }

  RECT ActionRect(const RECT& row, int ordinal, int count) const {
    const int btn = Scaled(kActionButton);
    const int gap = Scaled(4);
    const int mid = (row.top + row.bottom) / 2;
    const int right = row.right - (count - 1 - ordinal) * (btn + gap);
    return {right - btn, mid - btn / 2, right, mid + btn / 2};
  }

  RECT AllDownloadsRect() const {
    RECT rc;
    ::GetClientRect(hwnd_, &rc);
    RECT measure = {0, 0, 0, 0};
    HDC dc = ::GetDC(hwnd_);
    HGDIOBJ old = ::SelectObject(dc, small_font_);
    PhiDrawTextAA(dc, L"All Downloads", -1, &measure,
                DT_CALCRECT | DT_SINGLELINE);
    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd_, dc);
    const int w = measure.right + Scaled(8 + 4 + 10 + 8);
    const int h = measure.bottom + Scaled(8);
    const int mid = rc.bottom - Scaled(kBottomBar) / 2;
    return {rc.right - Scaled(12) - w, mid - h / 2, rc.right - Scaled(12),
            mid + h / 2};
  }

  void Paint(HDC dc, const RECT& client) {
    const PhiPalette& p = palette_;
    FillRect(dc, client, p.window_bg);
    ::SetBkMode(dc, TRANSPARENT);
    const COLORREF dim = BlendOver(p.window_bg, p.label, 128);

    if (rows_.empty()) {
      RECT list = ListRect();
      HGDIOBJ old = ::SelectObject(dc, empty_font_);
      ::SetTextColor(dc, p.label_dim);
      RECT glyph = list;
      glyph.bottom = (list.top + list.bottom) / 2 + Scaled(6);
      PhiDrawTextAA(dc, L"", -1, &glyph,
                  DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
      ::SelectObject(dc, small_font_);
      RECT text = list;
      text.top = glyph.bottom + Scaled(8);
      PhiDrawTextAA(dc, L"No Downloads", -1, &text,
                  DT_CENTER | DT_TOP | DT_SINGLELINE);
      ::SelectObject(dc, old);
    }

    const RECT list = ListRect();
    for (size_t i = 0; i < rows_.size(); ++i) {
      const RECT row = RowRect(static_cast<int>(i));
      if (row.bottom <= list.top || row.top >= list.bottom) {
        continue;
      }
      const DownloadRow& item = rows_[i];
      const bool failed = item.state == "cancelled" ||
                          item.state == "interrupted" ||
                          (item.state == "complete" && !item.exists);

      // File-type icon, from the shell like NSWorkspace's.
      const int icon = Scaled(kIconSize);
      const int mid = (row.top + row.bottom) / 2;
      if (HICON handle = IconFor(item.path)) {
        ::DrawIconEx(dc, row.left, mid - icon / 2, handle, icon, icon, 0,
                     nullptr, DI_NORMAL);
      }

      const std::vector<RowAction> actions = ActionsFor(item);
      const int actions_w = static_cast<int>(actions.size()) *
                                Scaled(kActionButton) +
                            (static_cast<int>(actions.size()) - 1) * Scaled(4);
      RECT info = {row.left + icon + Scaled(kIconGap), row.top,
                   row.right - actions_w - Scaled(8), row.bottom};

      // Name line (13pt, struck through for failed/cancelled/deleted), with
      // the Mac's "· Canceled/Failed/Deleted" bold suffix.
      HGDIOBJ old = ::SelectObject(
          dc, failed ? name_strike_font_ : name_font_);
      ::SetTextColor(dc, p.label);
      RECT name_rc = {info.left, mid - Scaled(18), info.right,
                      mid - Scaled(1)};
      const wchar_t* status =
          item.danger == "warning"         ? L"Dangerous"
          : item.danger == "blocked"       ? L"Blocked"
          : item.danger == "policyBlocked" ? L"Blocked by policy"
          : item.danger == "scanning"      ? L"Scanning..."
          : item.state == "cancelled"      ? L"Canceled"
          : item.state == "interrupted"
              ? L"Failed"
              : (item.state == "complete" && !item.exists ? L"Deleted"
                                                          : nullptr);
      RECT name_measure = name_rc;
      PhiDrawTextAA(dc, item.name.c_str(), -1, &name_measure,
                  DT_CALCRECT | DT_SINGLELINE);
      RECT name_draw = name_rc;
      if (status) {
        RECT tag_measure = {0, 0, 0, 0};
        ::SelectObject(dc, small_bold_font_);
        std::wstring tag = std::wstring(L"· ") + status;
        PhiDrawTextAA(dc, tag.c_str(), -1, &tag_measure,
                    DT_CALCRECT | DT_SINGLELINE);
        name_draw.right =
            std::min(static_cast<int>(name_rc.right - tag_measure.right) -
                         Scaled(4),
                     static_cast<int>(name_rc.left + name_measure.right));
        ::SelectObject(dc, failed ? name_strike_font_ : name_font_);
        PhiDrawTextAA(dc, item.name.c_str(), -1, &name_draw,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
        RECT tag_rc = {std::min(name_draw.right,
                                name_draw.left + name_measure.right) +
                           Scaled(4),
                       name_rc.top, name_rc.right, name_rc.bottom};
        ::SelectObject(dc, small_bold_font_);
        ::SetTextColor(dc, dim);
        PhiDrawTextAA(dc, tag.c_str(), -1, &tag_rc,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      } else {
        PhiDrawTextAA(dc, item.name.c_str(), -1, &name_draw,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
      }

      // Second line: progress (bar, or figures while hovered) for live
      // downloads, relative date or "From host" otherwise.
      RECT sub = {info.left, mid + Scaled(2), info.right, mid + Scaled(17)};
      ::SelectObject(dc, small_font_);
      ::SetTextColor(dc, dim);
      if (item.state == "in_progress") {
        if (hover_row_ == static_cast<int>(i)) {
          std::wstring text = FormatBytes(item.received) + L" of " +
                              FormatBytes(item.total);
          if (item.paused) {
            text += L" ・Paused";
          } else if (item.speed > 0) {
            text += L" ・ " + FormatBytes(item.speed) + L"/s";
          }
          PhiDrawTextAA(dc, text.c_str(), -1, &sub,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
          const int bar_mid = (sub.top + sub.bottom) / 2;
          RECT track = {sub.left, bar_mid - Scaled(1), sub.right,
                        bar_mid + Scaled(2)};
          FillRounded(dc, track, BlendOver(p.window_bg, p.accent, 51),
                      Scaled(3));
          if (item.total > 0) {
            RECT fill = track;
            fill.right = fill.left + static_cast<int>(
                (track.right - track.left) * item.received / item.total);
            FillRounded(dc, fill, p.accent, Scaled(3));
          }
        }
      } else {
        const std::wstring text =
            item.state == "complete" && item.end_time > 0
                ? FormatRelative(item.end_time)
                : L"From " + item.host;
        PhiDrawTextAA(dc, text.c_str(), -1, &sub,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      }

      // Action buttons.
      ::SelectObject(dc, glyph_font_);
      for (size_t a = 0; a < actions.size(); ++a) {
        const RECT button = ActionRect(row, static_cast<int>(a),
                                       static_cast<int>(actions.size()));
        if (hover_row_ == static_cast<int>(i) &&
            hover_action_ == static_cast<int>(a) && actions[a].enabled) {
          FillRounded(dc, button, p.hover_strong, Scaled(6));
        }
        ::SetTextColor(dc, actions[a].enabled ? p.label : p.label_tertiary);
        wchar_t glyph[2] = {actions[a].glyph, 0};
        PhiDrawTextAA(dc, glyph, 1, const_cast<RECT*>(&button),
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      ::SelectObject(dc, old);

      // Separator (not after the last row).
      if (i + 1 < rows_.size()) {
        RECT line = {row.left, row.bottom - 1, row.right, row.bottom};
        FillRect(dc, line, BlendOver(p.window_bg, p.label, 26));
      }
    }

    // Bottom bar: divider + trailing "All Downloads ›".
    RECT bar = client;
    bar.top = client.bottom - Scaled(kBottomBar);
    FillRect(dc, bar, p.window_bg);
    RECT divider = {bar.left, bar.top, bar.right, bar.top + 1};
    FillRect(dc, divider, BlendOver(p.window_bg, p.label, 26));
    const RECT button = AllDownloadsRect();
    if (hover_all_) {
      FillRounded(dc, button, BlendOver(p.window_bg, p.accent, 15),
                  Scaled(6));
    }
    HGDIOBJ old = ::SelectObject(dc, small_font_);
    ::SetTextColor(dc, p.accent);
    RECT text = {button.left + Scaled(8), button.top,
                 button.right - Scaled(18), button.bottom};
    PhiDrawTextAA(dc, L"All Downloads", -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, glyph_font_);
    RECT chevron = {text.right, button.top, button.right, button.bottom};
    PhiDrawTextAA(dc, L"", 1, &chevron,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, old);

    // Hairline border over everything (the panel edge).
    HPEN pen = ::CreatePen(PS_SOLID, 1, p.border);
    HGDIOBJ old_pen = ::SelectObject(dc, pen);
    HGDIOBJ old_brush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
    ::Rectangle(dc, client.left, client.top, client.right, client.bottom);
    ::SelectObject(dc, old_brush);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(pen);
  }

  void OnMouseMove(int x, int y) {
    const POINT pt = {x, y};
    int row = -1;
    int action = -1;
    bool all = false;
    const RECT list = ListRect();
    if (::PtInRect(&list, pt)) {
      for (size_t i = 0; i < rows_.size(); ++i) {
        RECT rc = RowRect(static_cast<int>(i));
        if (!::PtInRect(&rc, pt)) {
          continue;
        }
        row = static_cast<int>(i);
        const std::vector<RowAction> actions = ActionsFor(rows_[i]);
        for (size_t a = 0; a < actions.size(); ++a) {
          RECT button = ActionRect(rc, static_cast<int>(a),
                                   static_cast<int>(actions.size()));
          if (::PtInRect(&button, pt)) {
            action = static_cast<int>(a);
          }
        }
        break;
      }
    } else {
      RECT button = AllDownloadsRect();
      all = ::PtInRect(&button, pt) != FALSE;
    }
    if (row != hover_row_ || action != hover_action_ || all != hover_all_) {
      hover_row_ = row;
      hover_action_ = action;
      hover_all_ = all;
      ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd_, 0};
    ::TrackMouseEvent(&track);
  }

  void OnClick() {
    if (hover_all_) {
      if (framework_) {
        framework_->Navigate(browser_id_, "chrome://downloads/");
      }
      ::DestroyWindow(hwnd_);
      return;
    }
    if (hover_row_ < 0 || hover_action_ < 0 ||
        hover_row_ >= static_cast<int>(rows_.size())) {
      return;
    }
    const DownloadRow& row = rows_[static_cast<size_t>(hover_row_)];
    const std::vector<RowAction> actions = ActionsFor(row);
    if (hover_action_ >= static_cast<int>(actions.size()) ||
        !actions[static_cast<size_t>(hover_action_)].enabled) {
      return;
    }
    const RowAction& action = actions[static_cast<size_t>(hover_action_)];
    if (std::string_view(action.verb) == "copy") {
      CopyToClipboard(row.url);
      return;
    }
    if (framework_) {
      framework_->DownloadAction(browser_id_, static_cast<uint32_t>(row.id),
                                 action.verb);
      last_json_.clear();  // Force the next poll to repaint.
      Refresh();
    }
  }

  void CopyToClipboard(const std::wstring& text) {
    if (!::OpenClipboard(hwnd_)) {
      return;
    }
    ::EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL handle = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
      if (void* target = ::GlobalLock(handle)) {
        // SAFETY: the allocation above is exactly `bytes` long.
        UNSAFE_BUFFERS(::memcpy(target, text.c_str(), bytes));
        ::GlobalUnlock(handle);
        ::SetClipboardData(CF_UNICODETEXT, handle);
      } else {
        ::GlobalFree(handle);
      }
    }
    ::CloseClipboard();
  }

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM w,
                                  LPARAM l) {
    Popover* self = reinterpret_cast<Popover*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (message) {
      case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(l);
        self = static_cast<Popover*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(self));
        break;
      }
      case WM_ACTIVATE:
        if (self && LOWORD(w) == WA_INACTIVE) {
          // Opening grace: a menu teardown or the browser's async focus can
          // deactivate the popover the instant it opens; a real click-away
          // cannot happen this soon. Dismissal resumes after the grace.
          if (::GetTickCount64() - self->created_tick_ < 500) {
            break;
          }
          ::DestroyWindow(hwnd);
          return 0;
        }
        break;
      case WM_TIMER:
        if (self && w == kPollTimer) {
          self->Refresh();
          return 0;
        }
        break;
      case WM_MOUSEWHEEL:
        if (self) {
          self->scroll_ = std::max(
              0, std::min(self->max_scroll_,
                          self->scroll_ -
                              GET_WHEEL_DELTA_WPARAM(w) *
                                  self->Scaled(kRowHeight) / WHEEL_DELTA));
          ::InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        break;
      case WM_MOUSEMOVE:
        if (self) {
          self->OnMouseMove(GET_X_LPARAM(l), GET_Y_LPARAM(l));
          return 0;
        }
        break;
      case WM_MOUSELEAVE:
        if (self) {
          self->hover_row_ = -1;
          self->hover_action_ = -1;
          self->hover_all_ = false;
          ::InvalidateRect(hwnd, nullptr, FALSE);
          return 0;
        }
        break;
      case WM_LBUTTONUP:
        if (self) {
          self->OnClick();
          return 0;
        }
        break;
      case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = ::BeginPaint(hwnd, &paint);
        RECT client;
        ::GetClientRect(hwnd, &client);
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP bitmap =
            ::CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = ::SelectObject(mem, bitmap);
        if (self) {
          self->Paint(mem, client);
        }
        ::BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
        ::SelectObject(mem, old);
        ::DeleteObject(bitmap);
        ::DeleteDC(mem);
        ::EndPaint(hwnd, &paint);
        return 0;
      }
      case WM_DESTROY:
        if (self) {
          ::KillTimer(hwnd, kPollTimer);
          Popover::closed_tick = ::GetTickCount64();
          Popover::last_owner = self->owner_;
          Popover::instance = nullptr;
          ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
          delete self;
        }
        return 0;
    }
    return ::DefWindowProcW(hwnd, message, w, l);
  }

  HWND owner_;
  HWND hwnd_ = nullptr;
  // When the popover was shown — the opening grace against activation churn.
  ULONGLONG created_tick_ = 0;
  PhiPalette palette_;
  // Not rewritten to raw_ptr: the popover lives strictly inside the
  // framework's lifetime (owned popup of the shell window).
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_;
  int64_t browser_id_;
  UINT dpi_ = 96;
  std::vector<DownloadRow> rows_;
  std::string last_json_;
  std::map<std::wstring, HICON> icons_;
  int scroll_ = 0;
  int max_scroll_ = 0;
  int hover_row_ = -1;
  int hover_action_ = -1;
  bool hover_all_ = false;
  HFONT name_font_ = nullptr;
  HFONT name_strike_font_ = nullptr;
  HFONT small_font_ = nullptr;
  HFONT small_bold_font_ = nullptr;
  HFONT glyph_font_ = nullptr;
  HFONT empty_font_ = nullptr;
};

Popover* Popover::instance = nullptr;
HWND Popover::last_owner = nullptr;
ULONGLONG Popover::closed_tick = 0;

}  // namespace

void PhiDownloadsPopover::Toggle(HWND owner,
                                 const RECT& anchor_screen,
                                 const PhiPalette& palette,
                                 phi_framework::PhiFramework* framework,
                                 int64_t browser_id) {
  if (Popover::instance) {
    ::DestroyWindow(Popover::instance->hwnd());
    return;
  }
  // The click that dismissed the popover (deactivating it) must not
  // immediately reopen it — the flyout toggle race.
  if (Popover::last_owner == owner &&
      ::GetTickCount64() - Popover::closed_tick < 300) {
    return;
  }
  Popover::instance =
      new Popover(owner, anchor_screen, palette, framework, browser_id);
}

}  // namespace phi_client
