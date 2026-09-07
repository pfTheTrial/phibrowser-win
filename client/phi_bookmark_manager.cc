// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_bookmark_manager.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_folder_icons.h"
#include "phibrowser-win/client/phi_mac_icons.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

namespace {

// Page metrics (DIP), following the Mac page's proportions.
constexpr int kPagePad = 24;
constexpr int kHeaderHeight = 64;
constexpr int kSearchWidth = 260;
constexpr int kSearchHeight = 28;
constexpr int kButtonWidth = 104;
constexpr int kButtonHeight = 28;
constexpr int kRowHeight = 32;
constexpr int kIndent = 18;
constexpr int kTwisty = 14;
constexpr int kIconSize = 16;
constexpr int kColumnGap = 24;
// The Address column takes the trailing 45% of the body, as the Mac's
// stretching Address column settles to at a typical page width.
constexpr int kAddressPercent = 45;
constexpr int kRowRadius = 6;

const wchar_t kClassName[] = L"PhiBookmarkManager";
constexpr int kSearchId = 4101;

// Case-insensitive, whitespace-trimmed containment — the Mac matches
// localized/case/diacritic-insensitively; this keeps the case and whitespace
// halves and leaves diacritic folding to a later pass.
std::wstring Fold(const std::wstring& text) {
  std::wstring out = text;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
  return out;
}

std::wstring Trim(const std::wstring& text) {
  size_t begin = text.find_first_not_of(L" \t\r\n");
  if (begin == std::wstring::npos) {
    return std::wstring();
  }
  size_t end = text.find_last_not_of(L" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

// Puts `text` on the clipboard (the context menu's Copy Link).
void CopyToClipboard(HWND owner, const std::wstring& text) {
  if (!::OpenClipboard(owner)) {
    return;
  }
  ::EmptyClipboard();
  const size_t count = text.size() + 1;
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, count * sizeof(wchar_t))) {
    if (void* dest = ::GlobalLock(mem)) {
      // SAFETY: the allocation above is exactly `count` wchar_t.
      auto span = UNSAFE_BUFFERS(
          base::span<wchar_t>(static_cast<wchar_t*>(dest), count));
      std::copy(text.begin(), text.end(), span.begin());
      span[text.size()] = L'\0';
      ::GlobalUnlock(mem);
      if (::SetClipboardData(CF_UNICODETEXT, mem)) {
        mem = nullptr;  // The system owns it now.
      }
    }
    if (mem) {
      ::GlobalFree(mem);
    }
  }
  ::CloseClipboard();
}

}  // namespace

PhiBookmarkManager::PhiBookmarkManager(PhiWindowController* controller)
    : controller_(controller) {
  if (controller_ && controller_->shell_hwnd()) {
    dpi_ = ::GetDpiForWindow(controller_->shell_hwnd());
  }
}

PhiBookmarkManager::~PhiBookmarkManager() {
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::DestroyWindow(hwnd_);
  }
  for (HFONT font : {title_font_, row_font_, dim_font_, button_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  if (field_brush_) {
    ::DeleteObject(field_brush_);
  }
}

// static
const char* PhiBookmarkManager::ManagerUrl() {
  return "chrome://bookmarks/";
}

// static
bool PhiBookmarkManager::IsManagerUrl(const std::string& url) {
  return url.rfind("chrome://bookmarks", 0) == 0 ||
         url.rfind("phi://bookmarks", 0) == 0;
}

int PhiBookmarkManager::Scaled(int design) const {
  return ::MulDiv(design, static_cast<int>(dpi_), 96);
}

bool PhiBookmarkManager::visible() const {
  return hwnd_ && ::IsWindow(hwnd_) && ::IsWindowVisible(hwnd_);
}

void PhiBookmarkManager::EnsureFonts() {
  if (title_font_) {
    return;
  }
  auto make = [&](int size, int weight) {
    return ::CreateFontW(-Scaled(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, L"Segoe UI");
  };
  title_font_ = make(20, FW_SEMIBOLD);
  row_font_ = make(13, FW_NORMAL);
  dim_font_ = make(12, FW_NORMAL);
  button_font_ = make(12, FW_NORMAL);
}

void PhiBookmarkManager::EnsureWindow() {
  if (hwnd_ && ::IsWindow(hwnd_)) {
    return;
  }
  HWND owner = controller_ ? controller_->shell_hwnd() : nullptr;
  if (!owner) {
    return;
  }
  static const bool registered = [] {
    WNDCLASSW cls = {};
    cls.style = CS_DBLCLKS;  // The outline opens rows on double-click.
    cls.lpfnWndProc = &PhiBookmarkManager::PageProc;
    cls.hInstance = ::GetModuleHandleW(nullptr);
    cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = kClassName;
    return ::RegisterClassW(&cls) != 0;
  }();
  (void)registered;
  hwnd_ = ::CreateWindowExW(
      0, kClassName, L"",
      // WS_CLIPSIBLINGS keeps the content card's corner caps visible.
      WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0,
                            0, 0, 0, owner, nullptr,
                            ::GetModuleHandleW(nullptr), nullptr);
  if (!hwnd_) {
    return;
  }
  ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  EnsureFonts();
  EnsureSearchField();
}

void PhiBookmarkManager::EnsureSearchField() {
  if (search_ || !hwnd_) {
    return;
  }
  search_ = ::CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 0, 0, hwnd_,
      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(kSearchId)),
      ::GetModuleHandleW(nullptr), nullptr);
  if (search_ && row_font_) {
    ::SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(row_font_),
                   TRUE);
  }
  if (search_) {
    ::SendMessageW(search_, EM_SETCUEBANNER, TRUE,
                   reinterpret_cast<LPARAM>(L"Search bookmarks"));
  }
}

void PhiBookmarkManager::LayoutChrome() {
  if (!hwnd_) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd_, &rc);
  const int pad = Scaled(kPagePad);
  const int header_mid = rc.top + Scaled(kHeaderHeight) / 2;
  const int search_w = Scaled(kSearchWidth);
  const int search_h = Scaled(kSearchHeight);
  const int button_w = Scaled(kButtonWidth);
  const int button_h = Scaled(kButtonHeight);
  if (search_) {
    ::SetWindowPos(search_, nullptr, rc.right - pad - search_w,
                   header_mid - search_h / 2, search_w, search_h,
                   SWP_NOZORDER | SWP_NOACTIVATE);
  }
  new_folder_button_ = {rc.right - pad - search_w - Scaled(12) - button_w,
                        header_mid - button_h / 2,
                        rc.right - pad - search_w - Scaled(12),
                        header_mid + button_h / 2};
}

void PhiBookmarkManager::Show() {
  EnsureWindow();
  if (!hwnd_) {
    return;
  }
  Reposition();
  Refresh();
  ::ShowWindow(hwnd_, SW_SHOWNA);
  ::SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void PhiBookmarkManager::Hide() {
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::ShowWindow(hwnd_, SW_HIDE);
  }
}

void PhiBookmarkManager::Reposition() {
  if (!hwnd_ || !controller_) {
    return;
  }
  const RECT content = controller_->ContentRect();
  ::SetWindowPos(hwnd_, HWND_TOP, content.left, content.top,
                 content.right - content.left, content.bottom - content.top,
                 SWP_NOACTIVATE);
  LayoutChrome();
  Rebuild();
}

void PhiBookmarkManager::Refresh() {
  if (!hwnd_) {
    return;
  }
  Rebuild();
  ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void PhiBookmarkManager::AppendSubtree(const std::string& parent_guid,
                                       int depth) {
  if (!controller_ || !controller_->host()) {
    return;
  }
  for (PhiStoredRowView& view :
       controller_->host()->BookmarkRows(controller_->window_id(),
                                         parent_guid)) {
    Row row;
    row.guid = view.guid;
    row.parent_guid = parent_guid;
    row.title = base::UTF8ToWide(!view.title.empty() ? view.title : view.url);
    row.url = view.url;
    row.title_b = base::UTF8ToWide(view.title_b);
    row.url_b = view.url_b;
    row.favicon_b = std::move(view.favicon_b_bgra);
    row.favicon_b_width = view.favicon_b_width;
    row.favicon_b_height = view.favicon_b_height;
    row.icon = view.icon;
    row.folder = view.folder;
    row.depth = depth;
    row.favicon = std::move(view.favicon_bgra);
    row.favicon_width = view.favicon_width;
    row.favicon_height = view.favicon_height;
    if (row.folder) {
      row.child_count = static_cast<int>(
          controller_->host()
              ->BookmarkRows(controller_->window_id(), row.guid)
              .size());
      row.expanded = expanded_.count(row.guid) > 0;
    }
    const bool expanded = row.folder && row.expanded;
    const std::string guid = row.guid;
    rows_.push_back(std::move(row));
    if (expanded) {
      AppendSubtree(guid, depth + 1);
    }
  }
}

void PhiBookmarkManager::AppendMatches(const std::string& parent_guid) {
  if (!controller_ || !controller_->host()) {
    return;
  }
  const std::wstring needle = Fold(Trim(query_));
  for (PhiStoredRowView& view :
       controller_->host()->BookmarkRows(controller_->window_id(),
                                         parent_guid)) {
    const std::wstring title = base::UTF8ToWide(view.title);
    const std::wstring url = base::UTF8ToWide(view.url);
    const std::wstring title_b = base::UTF8ToWide(view.title_b);
    const std::wstring url_b = base::UTF8ToWide(view.url_b);
    const bool hit = Fold(title).find(needle) != std::wstring::npos ||
                     Fold(url).find(needle) != std::wstring::npos ||
                     Fold(title_b).find(needle) != std::wstring::npos ||
                     Fold(url_b).find(needle) != std::wstring::npos;
    const std::string guid = view.guid;
    const bool folder = view.folder;
    if (hit) {
      Row row;
      row.guid = view.guid;
      row.parent_guid = parent_guid;
      row.title =
          base::UTF8ToWide(!view.title.empty() ? view.title : view.url);
      row.url = view.url;
      row.title_b = title_b;
      row.url_b = view.url_b;
      row.favicon_b = std::move(view.favicon_b_bgra);
      row.favicon_b_width = view.favicon_b_width;
      row.favicon_b_height = view.favicon_b_height;
      row.icon = view.icon;
      row.folder = folder;
      // Results are FLAT root rows and folders are not expandable here, as
      // the spec requires — a flat index is not a sibling index.
      row.depth = 0;
      row.favicon = std::move(view.favicon_bgra);
      row.favicon_width = view.favicon_width;
      row.favicon_height = view.favicon_height;
      if (folder) {
        row.child_count = static_cast<int>(
            controller_->host()
                ->BookmarkRows(controller_->window_id(), guid)
                .size());
      }
      rows_.push_back(std::move(row));
    }
    if (folder) {
      AppendMatches(guid);  // Depth-first source order.
    }
  }
}

void PhiBookmarkManager::Rebuild() {
  rows_.clear();
  if (!hwnd_) {
    return;
  }
  if (Trim(query_).empty()) {
    AppendSubtree(std::string(), 0);
  } else {
    AppendMatches(std::string());
  }

  RECT rc = {};
  ::GetClientRect(hwnd_, &rc);
  const int pad = Scaled(kPagePad);
  int y = rc.top + Scaled(kHeaderHeight) - scroll_;
  for (Row& row : rows_) {
    row.bounds = {rc.left + pad, y, rc.right - pad, y + Scaled(kRowHeight)};
    const int text_left =
        row.bounds.left + row.depth * Scaled(kIndent) + Scaled(4);
    row.disclosure = {text_left, y + (Scaled(kRowHeight) - Scaled(kTwisty)) / 2,
                      text_left + Scaled(kTwisty),
                      y + (Scaled(kRowHeight) + Scaled(kTwisty)) / 2};
    y += Scaled(kRowHeight);
  }
  ClampScroll();
}

int PhiBookmarkManager::ContentHeight() const {
  return static_cast<int>(rows_.size()) * Scaled(kRowHeight);
}

void PhiBookmarkManager::ClampScroll() {
  if (!hwnd_) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd_, &rc);
  const int visible = rc.bottom - rc.top - Scaled(kHeaderHeight);
  const int limit = std::max(0, ContentHeight() - visible);
  scroll_ = std::max(0, std::min(limit, scroll_));
}

int PhiBookmarkManager::RowAt(POINT pt) const {
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (::PtInRect(&rows_[i].bounds, pt)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

std::vector<std::string> PhiBookmarkManager::SelectedGuids() const {
  std::vector<std::string> out;
  for (const Row& row : rows_) {
    if (selected_.count(row.guid) > 0) {
      out.push_back(row.guid);
    }
  }
  return out;
}

void PhiBookmarkManager::PaintRow(HDC dc,
                                  const Row& row,
                                  const PhiPalette& palette) {
  const bool is_selected = selected_.count(row.guid) > 0;
  if (is_selected) {
    HBRUSH fill = ::CreateSolidBrush(palette.hover_strong);
    HGDIOBJ prev_brush = ::SelectObject(dc, fill);
    HPEN pen = ::CreatePen(PS_SOLID, 1, palette.accent);
    HGDIOBJ prev_pen = ::SelectObject(dc, pen);
    const int r = Scaled(kRowRadius);
    ::RoundRect(dc, row.bounds.left, row.bounds.top, row.bounds.right,
                row.bounds.bottom, r, r);
    ::SelectObject(dc, prev_pen);
    ::SelectObject(dc, prev_brush);
    ::DeleteObject(pen);
    ::DeleteObject(fill);
  }

  const int mid = (row.bounds.top + row.bounds.bottom) / 2;
  int x = row.bounds.left + row.depth * Scaled(kIndent) + Scaled(4);

  // Disclosure twisty — folders only, and never while a search flattens the
  // tree (the spec makes search results non-expandable).
  if (row.folder && Trim(query_).empty()) {
    RECT twisty = row.disclosure;
    ::SetTextColor(dc, palette.label_dim);
    HGDIOBJ prev = ::SelectObject(dc, row_font_);
    ::DrawTextW(dc, row.expanded ? L"▾" : L"▸", -1, &twisty,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, prev);
  }
  x += Scaled(kTwisty) + Scaled(6);

  const int icon_px = Scaled(kIconSize);
  RECT icon = {x, mid - icon_px / 2, x + icon_px, mid + icon_px / 2};
  if (row.folder) {
    const std::string resolved = PhiResolveFolderIcon(row.icon);
    const PhiMacIcon* mask = resolved == PhiFolderIconDefaultId()
                                 ? PhiPlainFolderMask(row.expanded)
                                 : PhiFolderIconMask(resolved);
    if (mask) {
      const std::vector<unsigned char> bgra =
          PhiTintedIconBgra(*mask, palette.label_dim, icon_px);
      if (!bgra.empty()) {
        PhiBlendBgra(dc, icon, bgra.data(), icon_px, icon_px);
      }
    }
  } else if (!row.favicon.empty()) {
    PhiDrawBgra(dc, icon, row.favicon.data(), row.favicon_width,
                row.favicon_height);
  }
  x = icon.right + Scaled(8);
  // A split bookmark shows BOTH favicons, as its Website cell does on Mac.
  if (!row.url_b.empty() && !row.favicon_b.empty()) {
    RECT icon_b = {x, mid - icon_px / 2, x + icon_px, mid + icon_px / 2};
    PhiDrawBgra(dc, icon_b, row.favicon_b.data(), row.favicon_b_width,
                row.favicon_b_height);
    x = icon_b.right + Scaled(8);
  }

  RECT rc = {};
  ::GetClientRect(hwnd_, &rc);
  const int body_width = row.bounds.right - row.bounds.left;
  const int address_left =
      row.bounds.right - body_width * kAddressPercent / 100;
  RECT title_rc = {x, row.bounds.top, address_left - Scaled(kColumnGap),
                   row.bounds.bottom};
  HGDIOBJ prev_font = ::SelectObject(dc, row_font_);
  ::SetTextColor(dc, palette.label);
  ::DrawTextW(dc, row.title.c_str(), -1, &title_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);

  // Address column: a folder's child count, a bookmark's URL, or the split's
  // "primary | secondary".
  std::wstring address;
  if (row.folder) {
    address = base::UTF8ToWide(base::NumberToString(row.child_count)) +
              (row.child_count == 1 ? L" item" : L" items");
  } else if (!row.url_b.empty()) {
    address = base::UTF8ToWide(row.url) + L"  |  " + base::UTF8ToWide(row.url_b);
  } else {
    address = base::UTF8ToWide(row.url);
  }
  RECT address_rc = {address_left, row.bounds.top, row.bounds.right,
                     row.bounds.bottom};
  ::SelectObject(dc, dim_font_);
  ::SetTextColor(dc, palette.label_dim);
  ::DrawTextW(dc, address.c_str(), -1, &address_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);
  ::SelectObject(dc, prev_font);
}

void PhiBookmarkManager::Paint(HDC dc) {
  if (!controller_) {
    return;
  }
  RECT rc = {};
  ::GetClientRect(hwnd_, &rc);
  const PhiPalette palette = controller_->CurrentPalette();
  HBRUSH ground = ::CreateSolidBrush(palette.window_bg);
  ::FillRect(dc, &rc, ground);
  ::DeleteObject(ground);
  ::SetBkMode(dc, TRANSPARENT);

  const int pad = Scaled(kPagePad);
  RECT title_rc = {rc.left + pad, rc.top, rc.left + pad + Scaled(300),
                   rc.top + Scaled(kHeaderHeight)};
  HGDIOBJ prev_font = ::SelectObject(dc, title_font_);
  ::SetTextColor(dc, palette.label);
  ::DrawTextW(dc, L"Bookmarks", -1, &title_rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // "New Folder" button.
  HBRUSH fill = ::CreateSolidBrush(new_folder_hot_ ? palette.hover_strong
                                                   : palette.hover);
  HGDIOBJ prev_brush = ::SelectObject(dc, fill);
  HPEN pen = ::CreatePen(PS_SOLID, 1, palette.border);
  HGDIOBJ prev_pen = ::SelectObject(dc, pen);
  ::RoundRect(dc, new_folder_button_.left, new_folder_button_.top,
              new_folder_button_.right, new_folder_button_.bottom,
              Scaled(6), Scaled(6));
  ::SelectObject(dc, prev_pen);
  ::SelectObject(dc, prev_brush);
  ::DeleteObject(pen);
  ::DeleteObject(fill);
  ::SelectObject(dc, button_font_);
  ::SetTextColor(dc, palette.label);
  RECT button_text = new_folder_button_;
  ::DrawTextW(dc, L"New Folder", -1, &button_text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  // A hairline under the header, then the rows.
  RECT rule = {rc.left + pad, rc.top + Scaled(kHeaderHeight) - 1,
               rc.right - pad, rc.top + Scaled(kHeaderHeight)};
  HBRUSH line = ::CreateSolidBrush(palette.separator);
  ::FillRect(dc, &rule, line);
  ::DeleteObject(line);
  ::SelectObject(dc, prev_font);

  if (rows_.empty()) {
    // The Mac's centred empty state; no synthetic node is added to the tree.
    RECT empty = {rc.left, rc.top + Scaled(kHeaderHeight), rc.right,
                  rc.bottom};
    HGDIOBJ prev = ::SelectObject(dc, row_font_);
    ::SetTextColor(dc, palette.label_tertiary);
    ::DrawTextW(dc,
                Trim(query_).empty() ? L"No bookmarks yet"
                                     : L"No bookmarks match your search",
                -1, &empty,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, prev);
    return;
  }

  // Clip the list so rows never paint over the header.
  const RECT body = {rc.left, rc.top + Scaled(kHeaderHeight), rc.right,
                     rc.bottom};
  HRGN clip = ::CreateRectRgnIndirect(&body);
  ::SelectClipRgn(dc, clip);
  for (const Row& row : rows_) {
    if (row.bounds.bottom < body.top || row.bounds.top > body.bottom) {
      continue;
    }
    PaintRow(dc, row, palette);
  }
  ::SelectClipRgn(dc, nullptr);
  ::DeleteObject(clip);
}

void PhiBookmarkManager::OnClick(POINT pt, bool ctrl, bool shift) {
  if (::PtInRect(&new_folder_button_, pt)) {
    NewFolder(std::string());
    return;
  }
  const int index = RowAt(pt);
  if (index < 0) {
    selected_.clear();
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    return;
  }
  const Row& row = rows_[static_cast<size_t>(index)];
  if (row.folder && Trim(query_).empty() && ::PtInRect(&row.disclosure, pt)) {
    if (expanded_.count(row.guid) > 0) {
      expanded_.erase(row.guid);
    } else {
      expanded_.insert(row.guid);
    }
    Refresh();
    return;
  }

  if (ctrl) {
    if (selected_.count(row.guid) > 0) {
      selected_.erase(row.guid);
    } else {
      selected_.insert(row.guid);
      anchor_guid_ = row.guid;
    }
  } else if (shift && !anchor_guid_.empty()) {
    int anchor = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
      if (rows_[i].guid == anchor_guid_) {
        anchor = static_cast<int>(i);
        break;
      }
    }
    if (anchor >= 0) {
      selected_.clear();
      const int from = std::min(anchor, index);
      const int to = std::max(anchor, index);
      for (int i = from; i <= to; ++i) {
        selected_.insert(rows_[static_cast<size_t>(i)].guid);
      }
    }
  } else {
    selected_.clear();
    selected_.insert(row.guid);
    anchor_guid_ = row.guid;
  }
  ::InvalidateRect(hwnd_, nullptr, TRUE);
}

void PhiBookmarkManager::OpenRow(const Row& row) {
  if (row.folder || row.url.empty() || !controller_ || !controller_->host()) {
    return;
  }
  controller_->host()->RequestOpen(controller_->window_id(), row.url);
  if (!row.url_b.empty()) {
    controller_->host()->RequestOpen(controller_->window_id(), row.url_b);
  }
}

void PhiBookmarkManager::OnDoubleClick(POINT pt) {
  const int index = RowAt(pt);
  if (index < 0) {
    return;
  }
  const Row row = rows_[static_cast<size_t>(index)];
  if (row.folder) {
    if (Trim(query_).empty()) {
      if (expanded_.count(row.guid) > 0) {
        expanded_.erase(row.guid);
      } else {
        expanded_.insert(row.guid);
      }
      Refresh();
    }
    return;
  }
  OpenRow(row);
}

void PhiBookmarkManager::NewFolder(const std::string& parent_guid) {
  if (!controller_ || !controller_->host()) {
    return;
  }
  std::wstring name = L"Untitled";
  if (!PhiPromptText(controller_->shell_hwnd(), L"New Folder",
                     L"Folder name:", &name)) {
    return;
  }
  if (!parent_guid.empty()) {
    expanded_.insert(parent_guid);
  }
  controller_->host()->AddBookmarkFolder(controller_->window_id(), parent_guid,
                                         base::WideToUTF8(name));
  Refresh();
}

void PhiBookmarkManager::EditRow(const Row& row) {
  if (!controller_ || !controller_->host()) {
    return;
  }
  std::wstring name = row.title;
  if (PhiPromptText(controller_->shell_hwnd(),
                    row.folder ? L"Rename Folder" : L"Rename Bookmark",
                    row.folder ? L"Enter a new name for this folder."
                               : L"Enter a new name for this bookmark.",
                    &name)) {
    controller_->host()->RenameStoredRow(row.guid, base::WideToUTF8(name));
  }
  Refresh();
}

void PhiBookmarkManager::DeleteSelection() {
  if (!controller_ || !controller_->host()) {
    return;
  }
  const std::vector<std::string> guids = SelectedGuids();
  if (guids.empty()) {
    return;
  }
  // The spec: a multi-item or folder deletion confirms; a single leaf goes
  // straight through, as the sidebar already behaves.
  bool needs_confirm = guids.size() > 1;
  for (const Row& row : rows_) {
    if (row.folder && selected_.count(row.guid) > 0) {
      needs_confirm = true;
    }
  }
  if (needs_confirm) {
    const std::wstring message =
        guids.size() > 1
            ? L"Delete the " +
                  base::UTF8ToWide(base::NumberToString(guids.size())) +
                  L" selected items?"
            : L"Delete this folder and everything inside it?";
    if (!PhiConfirm(controller_->shell_hwnd(), L"Delete Bookmarks",
                    message.c_str(), L"Delete")) {
      return;
    }
  }
  for (const std::string& guid : guids) {
    controller_->host()->RemoveStoredRow(guid);  // A folder takes its subtree.
  }
  selected_.clear();
  Refresh();
}

void PhiBookmarkManager::OnContextMenu(POINT pt) {
  const int index = RowAt(pt);
  if (index < 0) {
    return;
  }
  const Row row = rows_[static_cast<size_t>(index)];
  // Right-clicking an unselected row replaces the selection; right-clicking
  // inside the batch keeps it (the spec's rule).
  if (selected_.count(row.guid) == 0) {
    selected_.clear();
    selected_.insert(row.guid);
    anchor_guid_ = row.guid;
    ::InvalidateRect(hwnd_, nullptr, TRUE);
  }
  const bool batch = selected_.size() > 1;
  const bool split = !row.url_b.empty();

  enum : UINT { kOpen = 1, kCopy, kCopyLeft, kCopyRight, kEdit, kNewNested,
                kDelete };
  HMENU menu = ::CreatePopupMenu();
  if (!menu) {
    return;
  }
  if (!batch) {
    if (!row.folder) {
      ::AppendMenuW(menu, MF_STRING, kOpen, L"Open");
      if (split) {
        ::AppendMenuW(menu, MF_STRING, kCopyLeft, L"Copy Left URL");
        ::AppendMenuW(menu, MF_STRING, kCopyRight, L"Copy Right URL");
      } else {
        ::AppendMenuW(menu, MF_STRING, kCopy, L"Copy Link");
      }
    }
    ::AppendMenuW(menu, MF_STRING, kEdit, L"Edit...");
    if (row.folder) {
      ::AppendMenuW(menu, MF_STRING, kNewNested, L"New Nested Folder...");
    }
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  }
  ::AppendMenuW(menu, MF_STRING, kDelete, L"Delete");

  POINT screen = pt;
  ::ClientToScreen(hwnd_, &screen);
  const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
      screen.x, screen.y, 0, hwnd_, nullptr));
  ::DestroyMenu(menu);

  switch (chosen) {
    case kOpen:
      OpenRow(row);
      break;
    case kCopy:
    case kCopyLeft:
      CopyToClipboard(hwnd_, base::UTF8ToWide(row.url));
      break;
    case kCopyRight:
      CopyToClipboard(hwnd_, base::UTF8ToWide(row.url_b));
      break;
    case kEdit:
      EditRow(row);
      break;
    case kNewNested:
      NewFolder(row.guid);
      break;
    case kDelete:
      DeleteSelection();
      break;
    default:
      break;
  }
}

void PhiBookmarkManager::OnKey(WPARAM key) {
  if (key == VK_DELETE) {
    DeleteSelection();
  }
}

// static
LRESULT CALLBACK PhiBookmarkManager::PageProc(HWND hwnd,
                                              UINT msg,
                                              WPARAM w,
                                              LPARAM l) {
  auto* self = reinterpret_cast<PhiBookmarkManager*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        // Back-buffered: the list repaints wholesale on hover and scroll.
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        const int page_w = rc.right - rc.left;
        const int page_h = rc.bottom - rc.top;
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP buffer =
            mem ? ::CreateCompatibleBitmap(dc, page_w, page_h) : nullptr;
        if (mem && buffer) {
          HGDIOBJ prev = ::SelectObject(mem, buffer);
          self->Paint(mem);
          ::BitBlt(dc, 0, 0, page_w, page_h, mem, 0, 0, SRCCOPY);
          ::SelectObject(mem, prev);
        } else {
          self->Paint(dc);
        }
        if (buffer) {
          ::DeleteObject(buffer);
        }
        if (mem) {
          ::DeleteDC(mem);
        }
      }
      ::EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN:
      if (self) {
        ::SetFocus(hwnd);
        self->OnClick({GET_X_LPARAM(l), GET_Y_LPARAM(l)},
                      (::GetKeyState(VK_CONTROL) & 0x8000) != 0,
                      (::GetKeyState(VK_SHIFT) & 0x8000) != 0);
      }
      return 0;
    case WM_LBUTTONDBLCLK:
      if (self) {
        self->OnDoubleClick({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
      }
      return 0;
    case WM_RBUTTONUP:
      if (self) {
        self->OnContextMenu({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
      }
      return 0;
    case WM_MOUSEMOVE:
      if (self) {
        const POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const bool hot = ::PtInRect(&self->new_folder_button_, pt) != FALSE;
        const int row = self->RowAt(pt);
        if (hot != self->new_folder_hot_ || row != self->hover_) {
          self->new_folder_hot_ = hot;
          self->hover_ = row;
          ::InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      return 0;
    case WM_MOUSEWHEEL:
      if (self) {
        const int delta = GET_WHEEL_DELTA_WPARAM(w);
        self->scroll_ -= delta / 2;
        self->ClampScroll();
        self->Rebuild();
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    case WM_KEYDOWN:
      if (self) {
        self->OnKey(w);
      }
      return 0;
    case WM_SIZE:
      if (self) {
        self->LayoutChrome();
        self->Rebuild();
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    case WM_COMMAND:
      if (self && HIWORD(w) == EN_CHANGE &&
          LOWORD(w) == static_cast<WORD>(kSearchId)) {
        const int length = ::GetWindowTextLengthW(self->search_);
        std::wstring text;
        if (length > 0) {
          text.resize(static_cast<size_t>(length) + 1, L'\0');
          const int copied = ::GetWindowTextW(self->search_, text.data(),
                                              length + 1);
          text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
        }
        self->query_ = text;
        self->scroll_ = 0;
        self->Refresh();
        return 0;
      }
      break;
    case WM_CTLCOLOREDIT:
      if (self && self->controller_) {
        const PhiPalette palette = self->controller_->CurrentPalette();
        ::SetTextColor(reinterpret_cast<HDC>(w), palette.label);
        ::SetBkColor(reinterpret_cast<HDC>(w), palette.hover_strong);
        if (!self->field_brush_ ||
            self->field_brush_color_ != palette.hover_strong) {
          if (self->field_brush_) {
            ::DeleteObject(self->field_brush_);
          }
          self->field_brush_ = ::CreateSolidBrush(palette.hover_strong);
          self->field_brush_color_ = palette.hover_strong;
        }
        return reinterpret_cast<LRESULT>(self->field_brush_);
      }
      break;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, msg, w, l);
}

}  // namespace phi_client
