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
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_popover.h"
#include "phibrowser-win/client/phi_util.h"

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

constexpr UINT kPollIntervalMs = 500;  // Live refresh while open.
// hits_ keys: row i is i * kKeyStride, its action `a` is + 1 + a. The
// footer button has its own key. The base resolves hover and clicks over
// the list; nothing here hit-tests by hand any more.
constexpr int kKeyStride = 8;
constexpr int kAllDownloadsKey = -1;

void FillRect(HDC dc, const RECT& r, COLORREF color) {
  HBRUSH brush = ::CreateSolidBrush(color);
  ::FillRect(dc, &r, brush);
  ::DeleteObject(brush);
}

void FillRounded(HDC dc, const RECT& r, COLORREF color, int radius) {
  PhiFillRoundRectAA(dc, r, radius, color);
}

std::wstring FormatBytes(double bytes) {
  // The unit names are catalog keys rather than literals. The FIGURE is
  // still built with std::to_wstring, which is C-locale: a German user reads
  // "9.3 MB" where the system writes "9,3 MB". The fix is
  // PhiFormatNumber(bytes, 1) — deliberately NOT made here, because
  // GetNumberFormatEx also groups, so "1023 B" would become "1,023 B" and
  // this change is required to render identically in English. It is on the
  // list for the pass that converts the rest of this file.
  static constexpr std::array<const char*, 4> kUnitKeys = {
      "downloads.b", "downloads.kb", "downloads.mb", "downloads.gb"};
  size_t unit = 0;
  while (bytes >= 1024.0 && unit + 1 < kUnitKeys.size()) {
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
  return text + L" " + PhiStr(kUnitKeys[unit]);
}

// DownloadItemRow's friendly completion time. The ladder itself is
// PhiRelativeSpanFor (phi_util.h) so its boundaries are pinned by a host
// test; this function only names the strings. Before this change the sentence
// was CONCATENATED — `n + L" days ago"` — which reads "1 days ago" at 48
// hours, cannot be reordered by a translator, and made the string
// untranslatable in any language whose number goes elsewhere.
std::wstring FormatRelative(double unix_seconds) {
  const auto now = static_cast<double>(::time(nullptr));
  const auto delta = static_cast<long long>(now - unix_seconds);
  const PhiRelativeSpan span = PhiRelativeSpanFor(delta);
  switch (span.unit) {
    case PhiRelativeUnit::kJustNow:
      return PhiStr("downloads.item.completedTime.justNow");
    case PhiRelativeUnit::kMinutes:
      return PhiStrN("downloads.item.completedTime.minutesAgo", span.count);
    case PhiRelativeUnit::kHours:
      return PhiStrN("downloads.item.completedTime.hoursAgo", span.count);
    case PhiRelativeUnit::kToday:
      return PhiStr("downloads.item.completedTime.today");
    case PhiRelativeUnit::kYesterday:
      return PhiStr("downloads.item.completedTime.yesterday");
    case PhiRelativeUnit::kDays:
      return PhiStrN("downloads.item.completedTime.daysAgo", span.count);
    case PhiRelativeUnit::kAbsolute:
      break;
  }
  // "%b %d, %Y" was English wearing a format string's clothes: it puts the
  // month first for every locale on earth and abbreviates it in English.
  // The locale's own short date is both correct and narrower, which this
  // 340pt row wants.
  return PhiFormatDate(static_cast<int64_t>(unix_seconds) * 1000,
                       PhiDateStyle::kShortDate);
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

// The popover keeps its download rows, its Paint() and its per-row
// actions. The window, the placement, the back buffer, hover, the poll
// timer, dismissal and lifetime belong to PhiPopover (lane 3 §6.2).
class Popover : public PhiPopover {
 public:
  static Popover* instance;

  Popover(phi_framework::PhiFramework* framework, BrowserId browser)
      : framework_(framework), browser_id_(browser.value) {}

  ~Popover() override {
    for (auto& [ext, icon] : icons_) {
      if (icon) {
        ::DestroyIcon(icon);
      }
    }
  }

  bool Open(HWND owner, const RECT& anchor, const PhiPalette& colors) {
    ClassSpec spec;
    spec.name = L"PhiDownloadsPopover";

    Options options;  // Activating, DWM-rounded, 500 ms opening grace.

    Placement placement;
    placement.anchor = anchor;
    placement.side = PhiPopoverSide::kAbove;
    placement.gap_dip = 8;
    placement.flip = false;  // The bottom-bar button always drops upward.
    placement.monitor_from_owner = true;

    SIZE probe = {1, 1};
    if (!CreatePopover(spec, owner, probe, placement, options)) {
      return false;
    }
    instance = this;  // Before Show(): activation churn can destroy us.
    SetPalette(colors);
    SIZE size = {Scaled(kPanelWidth), Scaled(kPanelHeight)};
    Reposition(placement, size);
    Refresh();
    Show();
    poll_ = NewTimer();
    poll_.Start(kPollIntervalMs);
    return true;
  }

 protected:
  void Paint(HDC dc, const RECT&) override {
    RECT client = {};
    ::GetClientRect(hwnd(), &client);
    PaintPanel(dc, client);
  }

  void OnTimerFired(UINT_PTR id) override {
    if (id == poll_.id()) {
      Refresh();
    }
  }

  void OnHover(int hit, int) override {
    hover_row_ = -1;
    hover_action_ = -1;
    hover_all_ = false;
    if (hit < 0) {
      return;
    }
    const int key = hits_[static_cast<size_t>(hit)].key;
    if (key == kAllDownloadsKey) {
      hover_all_ = true;
      return;
    }
    hover_row_ = key / kKeyStride;
    const int sub = key % kKeyStride;
    if (sub > 0) {
      hover_action_ = sub - 1;
    }
  }

  void OnWheel(int delta, POINT) override {
    scroll_ = std::max(
        0, std::min(max_scroll_, scroll_ - delta * Scaled(kRowHeight) /
                                               WHEEL_DELTA));
    BuildHits();
    Invalidate();
  }

  void OnClick(int hit, POINT) override {
    OnHover(hit, -1);
    RunHoveredAction();
  }

  void OnDpiChanged(UINT) override {
    ::SetWindowPos(hwnd(), nullptr, 0, 0, Scaled(kPanelWidth),
                   Scaled(kPanelHeight),
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    BuildHits();
  }

  void OnNcDestroy() override {
    poll_.Stop();
    PhiPopover::RecordClose(owner());
    if (instance == this) {
      instance = nullptr;
    }
  }

 private:
  HFONT NameFont() { return fonts().Get(13, FW_NORMAL); }
  HFONT NameStrikeFont() {
    PhiFontDesc desc;
    desc.size = 13;
    desc.strikeout = true;
    return fonts().Get(desc);
  }
  HFONT SmallFont() { return fonts().Get(11, FW_NORMAL); }
  HFONT SmallBoldFont() { return fonts().Get(11, FW_BOLD); }
  HFONT GlyphFont() {
    PhiFontDesc desc;
    desc.size = 12;
    desc.face = L"Segoe MDL2 Assets";
    return fonts().Get(desc);
  }
  HFONT EmptyFont() {
    PhiFontDesc desc;
    desc.size = 32;
    desc.face = L"Segoe MDL2 Assets";
    return fonts().Get(desc);
  }

  // THE layout pass: every visible row, its action buttons and the footer
  // link, into the base's hit list. Re-run whenever the rows or the scroll
  // offset move — never from Paint().
  void BuildHits() {
    ClearHits();
    const RECT list = ListRect();
    for (size_t i = 0; i < rows_.size(); ++i) {
      RECT row = RowRect(static_cast<int>(i));
      RECT visible = row;
      visible.top = std::max(row.top, list.top);
      visible.bottom = std::min(row.bottom, list.bottom);
      if (visible.bottom <= visible.top) {
        continue;
      }
      AddHit(visible, static_cast<int>(i) * kKeyStride, rows_[i].name,
             PhiHitRole::kListItem);
      const std::vector<RowAction> actions = ActionsFor(rows_[i]);
      for (size_t a = 0; a < actions.size(); ++a) {
        RECT button = ActionRect(row, static_cast<int>(a),
                                 static_cast<int>(actions.size()));
        button.top = std::max(button.top, list.top);
        button.bottom = std::min(button.bottom, list.bottom);
        if (button.bottom <= button.top) {
          continue;
        }
        AddHit(button,
               static_cast<int>(i) * kKeyStride + 1 + static_cast<int>(a),
               base::UTF8ToWide(actions[a].verb), PhiHitRole::kButton, false,
               actions[a].enabled);
      }
    }
    AddHit(AllDownloadsRect(), kAllDownloadsKey, L"All Downloads",
           PhiHitRole::kButton);
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
        // A row can arrive before the framework has a name for it (an
        // in-flight download whose target is still being chosen); the row
        // shape is the framework's, not ours, so never assume the key.
        if (const std::string* name = dict->FindString("name")) {
          row.name = base::UTF8ToWide(*name);
        }
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
    BuildHits();
    Invalidate();
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
    ::GetClientRect(hwnd(), &rc);
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

  RECT AllDownloadsRect() {
    RECT rc;
    ::GetClientRect(hwnd(), &rc);
    RECT measure = {0, 0, 0, 0};
    HDC dc = ::GetDC(hwnd());
    HGDIOBJ old = ::SelectObject(dc, SmallFont());
    PhiDrawTextAA(dc, L"All Downloads", -1, &measure,
                DT_CALCRECT | DT_SINGLELINE);
    ::SelectObject(dc, old);
    ::ReleaseDC(hwnd(), dc);
    const int w = measure.right + Scaled(8 + 4 + 10 + 8);
    const int h = measure.bottom + Scaled(8);
    const int mid = rc.bottom - Scaled(kBottomBar) / 2;
    return {rc.right - Scaled(12) - w, mid - h / 2, rc.right - Scaled(12),
            mid + h / 2};
  }

  void PaintPanel(HDC dc, const RECT& client) {
    const PhiPalette& p = palette();
    FillRect(dc, client, p.window_bg);
    ::SetBkMode(dc, TRANSPARENT);
    const COLORREF dim = PhiBlend(p.label, p.window_bg, 128);

    if (rows_.empty()) {
      RECT list = ListRect();
      HGDIOBJ old = ::SelectObject(dc, EmptyFont());
      ::SetTextColor(dc, p.label_dim);
      RECT glyph = list;
      glyph.bottom = (list.top + list.bottom) / 2 + Scaled(6);
      PhiDrawTextAA(dc, L"", -1, &glyph,
                  DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
      ::SelectObject(dc, SmallFont());
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
          dc, failed ? NameStrikeFont() : NameFont());
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
        ::SelectObject(dc, SmallBoldFont());
        std::wstring tag = std::wstring(L"· ") + status;
        PhiDrawTextAA(dc, tag.c_str(), -1, &tag_measure,
                    DT_CALCRECT | DT_SINGLELINE);
        name_draw.right =
            std::min(static_cast<int>(name_rc.right - tag_measure.right) -
                         Scaled(4),
                     static_cast<int>(name_rc.left + name_measure.right));
        ::SelectObject(dc, failed ? NameStrikeFont() : NameFont());
        PhiDrawTextAA(dc, item.name.c_str(), -1, &name_draw,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
        RECT tag_rc = {std::min(name_draw.right,
                                name_draw.left + name_measure.right) +
                           Scaled(4),
                       name_rc.top, name_rc.right, name_rc.bottom};
        ::SelectObject(dc, SmallBoldFont());
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
      ::SelectObject(dc, SmallFont());
      ::SetTextColor(dc, dim);
      if (item.state == "in_progress") {
        if (hover_row_ == static_cast<int>(i)) {
          std::wstring text =
              PhiStrF("downloads.item.transfer.progressLabel",
                      {FormatBytes(item.received), FormatBytes(item.total)});
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
          FillRounded(dc, track, PhiBlend(p.accent, p.window_bg, 51),
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
                : PhiStrF("downloads.item.sourceHostLabel", {item.host});
        PhiDrawTextAA(dc, text.c_str(), -1, &sub,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      }

      // Action buttons.
      ::SelectObject(dc, GlyphFont());
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
        FillRect(dc, line, PhiBlend(p.label, p.window_bg, 26));
      }
    }

    // Bottom bar: divider + trailing "All Downloads ›".
    RECT bar = client;
    bar.top = client.bottom - Scaled(kBottomBar);
    FillRect(dc, bar, p.window_bg);
    RECT divider = {bar.left, bar.top, bar.right, bar.top + 1};
    FillRect(dc, divider, PhiBlend(p.label, p.window_bg, 26));
    const RECT button = AllDownloadsRect();
    if (hover_all_) {
      FillRounded(dc, button, PhiBlend(p.accent, p.window_bg, 15),
                  Scaled(6));
    }
    HGDIOBJ old = ::SelectObject(dc, SmallFont());
    ::SetTextColor(dc, p.accent);
    RECT text = {button.left + Scaled(8), button.top,
                 button.right - Scaled(18), button.bottom};
    PhiDrawTextAA(dc, L"All Downloads", -1, &text,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    ::SelectObject(dc, GlyphFont());
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

  void RunHoveredAction() {
    if (hover_all_) {
      if (framework_) {
        framework_->Navigate(browser_id_, "chrome://downloads/");
      }
      Dismiss();
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
    if (!::OpenClipboard(hwnd())) {
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

  // Not rewritten to raw_ptr: the popover lives strictly inside the
  // framework's lifetime (owned popup of the shell window).
  RAW_PTR_EXCLUSION phi_framework::PhiFramework* framework_;
  // The browser the popover was opened for (BrowserId::value; the
  // framework calls below are still int64_t until phase 2).
  int64_t browser_id_;
  Timer poll_;
  std::vector<DownloadRow> rows_;
  std::string last_json_;
  std::map<std::wstring, HICON> icons_;
  int scroll_ = 0;
  int max_scroll_ = 0;
  int hover_row_ = -1;
  int hover_action_ = -1;
  bool hover_all_ = false;
};

Popover* Popover::instance = nullptr;

}  // namespace

void PhiDownloadsPopover::Toggle(HWND owner,
                                 const RECT& anchor_screen,
                                 const PhiPalette& palette,
                                 phi_framework::PhiFramework* framework,
                                 BrowserId browser) {
  if (Popover::instance) {
    Popover::instance->Dismiss();
    return;
  }
  // The click that dismissed the popover (deactivating it) must not
  // immediately reopen it — the flyout toggle race, now the family's.
  if (PhiPopover::ShouldSuppressReopen(owner)) {
    return;
  }
  Popover* popover = new Popover(framework, browser);
  popover->DeleteWhenDestroyed();
  if (!popover->Open(owner, anchor_screen, palette)) {
    delete popover;
  }
}

}  // namespace phi_client
