// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_BOOKMARK_MANAGER_H_
#define PHIBROWSER_WIN_CLIENT_PHI_BOOKMARK_MANAGER_H_

#include <windows.h>

#include <set>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_client {

class PhiWindowController;
struct PhiPalette;

// The Mac's native bookmark manager (UserInterface/WebContent/BookmarkManager,
// docs/plans/2026-08-12-native-bookmark-manager.md): the Space's bookmark tree
// as a two-column outline, replacing Chromium's own bookmark surface.
//
// URL-DRIVEN, exactly as the Mac specifies: the page is not a window mode but
// a view mounted over the content card while the active tab sits on
// chrome://bookmarks. The tab stays a real browser tab — closing, moving and
// splitting it keep normal semantics — and navigating away unmounts the page.
// Structurally this is the Group Overview's child-window trick, held for the
// life of the tab instead of a keypress.
//
// Columns follow the spec: Website carries the disclosure, the folder symbol
// or the favicon(s) and the title; Address carries a folder's child count, a
// bookmark's URL, or "primary | secondary" for a split.
class PhiBookmarkManager {
 public:
  explicit PhiBookmarkManager(PhiWindowController* controller);
  ~PhiBookmarkManager();

  PhiBookmarkManager(const PhiBookmarkManager&) = delete;
  PhiBookmarkManager& operator=(const PhiBookmarkManager&) = delete;

  // True when `url` is the bookmark-manager route. Both the Chromium spelling
  // and the Phi one resolve here, as URLProcessor normalizes them on Mac.
  static bool IsManagerUrl(const std::string& url);
  // What the menu item navigates to.
  static const char* ManagerUrl();

  // Shows the page over the content card and refreshes it from the store.
  void Show();
  // Takes it down, leaving the Chromium view to paint again.
  void Hide();
  bool visible() const;

  // Follows the content card when the shell relayouts.
  void Reposition();
  // Re-reads the tree (a store mutation happened elsewhere).
  void Refresh();

 private:
  // One visible outline row — the flattened projection the Mac builds in
  // BookmarkManagerProjection, including its display signature inputs.
  struct Row {
    std::string guid;
    std::string parent_guid;
    std::wstring title;
    std::string url;
    std::wstring title_b;  // Split secondary.
    std::string url_b;
    std::string icon;  // Folder catalog symbol.
    bool folder = false;
    int depth = 0;
    bool expanded = false;
    int child_count = 0;
    std::vector<unsigned char> favicon;
    int favicon_width = 0;
    int favicon_height = 0;
    std::vector<unsigned char> favicon_b;
    int favicon_b_width = 0;
    int favicon_b_height = 0;
    RECT bounds{};      // Row rect in client coords, scroll applied.
    RECT disclosure{};  // The folder's twisty.
  };

  int Scaled(int design) const;
  void EnsureWindow();
  void EnsureFonts();
  void EnsureSearchField();
  void LayoutChrome();

  // Rebuilds `rows_` from the store: the outline in normal mode, or the flat
  // depth-first match list while a search is active.
  void Rebuild();
  void AppendSubtree(const std::string& parent_guid, int depth);
  void AppendMatches(const std::string& parent_guid);

  void Paint(HDC dc);
  void PaintRow(HDC dc, const Row& row, const PhiPalette& palette);
  int RowAt(POINT pt) const;  // -1 when the point is on no row.
  int ContentHeight() const;
  void ClampScroll();

  void OnClick(POINT pt, bool ctrl, bool shift);
  void OnDoubleClick(POINT pt);
  void OnContextMenu(POINT pt);
  void OnKey(WPARAM key);
  void OpenRow(const Row& row);
  void DeleteSelection();
  void NewFolder(const std::string& parent_guid);
  void EditRow(const Row& row);
  // Ordered selected guids, in visible order, as the spec requires actions to
  // derive them at invocation time.
  std::vector<std::string> SelectedGuids() const;

  static LRESULT CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);

  RAW_PTR_EXCLUSION PhiWindowController* const controller_;
  HWND hwnd_ = nullptr;
  HWND search_ = nullptr;
  HFONT title_font_ = nullptr;
  HFONT row_font_ = nullptr;
  HFONT dim_font_ = nullptr;
  HFONT button_font_ = nullptr;
  // Cached background for the search EDIT: WM_CTLCOLOREDIT must hand back a
  // REAL brush — a null one leaves the field unpainted (it renders black).
  HBRUSH field_brush_ = nullptr;
  COLORREF field_brush_color_ = 0;
  UINT dpi_ = 96;

  std::vector<Row> rows_;
  std::set<std::string> expanded_;
  std::set<std::string> selected_;
  std::string anchor_guid_;  // Shift-range origin.
  std::wstring query_;
  int scroll_ = 0;
  int hover_ = -1;
  RECT new_folder_button_{};
  bool new_folder_hot_ = false;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_BOOKMARK_MANAGER_H_
