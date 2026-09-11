// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_OMNIBOX_SESSION_H_
#define PHIBROWSER_WIN_CLIENT_PHI_OMNIBOX_SESSION_H_

#include <string>
#include <vector>

#include "phibrowser-win/client/phi_sidebar_layout.h"

// The address field and everything that hangs off it, as ONE component of
// the shell window (docs/architecture-review.md §3.5, lane 1 cluster F): the
// EDIT child and its subclass, the suggestion dropdown and its window, the
// results model, the navigation state the toolbar draws, and the commit
// paths. The shell forwards messages here and asks this for what it paints;
// nothing else touches the field, the popup or the nav state.
//
// The top half of this header is PURE — plain ints and SidebarRect, no Win32
// — so the geometry and the selection arithmetic are tested on the Linux
// host (test/model/omnibox_session_test.cc). A host translation unit defines
// PHI_OMNIBOX_SESSION_MODEL_ONLY before including it and gets that half
// alone; the client build sees the whole file.

namespace phi_client {

// The address capsule as the session needs it: the box itself and every
// accessory that can sit INSIDE it. Which accessories exist varies by layout
// mode and by page (the reader button comes and goes), so the session reads
// them rather than assuming a set.
struct PhiOmniboxCapsule {
  SidebarRect field;            // The painted pill.
  SidebarRect page_icon;        // Comfortable's leading site chip.
  SidebarRect field_ext_strip;  // Pinned-extension icons.
  SidebarRect reader;
  SidebarRect copy_link;
  SidebarRect extensions;
};

// The EDIT's rect inside `capsule`: a `text_height`-tall line centred in the
// box, starting after the page icon (plus `icon_gap`) or at `leading_inset`
// without one, and stopping `trailing_gap` before the LEFTMOST accessory
// that actually sits inside the capsule — the EDIT must not extend under the
// buttons or it eats their clicks. All arguments are already in pixels.
inline SidebarRect PhiOmniboxTextRect(const PhiOmniboxCapsule& capsule,
                                      int text_height,
                                      int leading_inset,
                                      int icon_gap,
                                      int trailing_gap) {
  const SidebarRect& box = capsule.field;
  // SideAddressBar: the text stops at the LEFTMOST accessory that sits
  // inside the capsule. Rects outside it (the comfortable layout keeps its
  // extensions button in the top bar) are not candidates.
  int right_stop = box.right;
  for (const SidebarRect& accessory :
       {capsule.field_ext_strip, capsule.reader, capsule.copy_link,
        capsule.extensions}) {
    if (accessory.right > accessory.left && accessory.left > box.left &&
        accessory.left < right_stop) {
      right_stop = accessory.left;
    }
  }
  const int text_left = capsule.page_icon.right > capsule.page_icon.left
                            ? capsule.page_icon.right + icon_gap
                            : box.left + leading_inset;
  const int width = right_stop - trailing_gap - text_left;
  // A single-line EDIT draws its text at the top of its own rect, so filling
  // the whole box would leave the text hugging the upper edge.
  const int top = box.top + (box.bottom - box.top - text_height) / 2;
  return SidebarRect{text_left, top, text_left + (width > 0 ? width : 1),
                     top + text_height};
}

// The dropdown's height for `rows` rows of `row_height`, plus its padding.
inline int PhiOmniboxPopupHeight(int rows, int row_height, int padding) {
  return rows * row_height + padding;
}

// The row at client-y `y` in a dropdown of `rows`, or -1 outside them.
inline int PhiOmniboxRowAtY(int y, int rows, int row_height, int top_pad) {
  // Truncating division, so the top pad belongs to the first row rather than
  // to nothing: a press on the popup's own border still picks a suggestion.
  const int row = (y - top_pad) / (row_height > 0 ? row_height : 1);
  return row >= 0 && row < rows ? row : -1;
}

// Arrowing: `delta` rows from `selected` (-1 = the field's own text), with
// both ends wrapping THROUGH the field — up from it lands on the last row,
// down past the last returns to it.
inline int PhiOmniboxNextSelection(int selected, int delta, int rows) {
  int next = selected + delta;
  if (next < -1) {
    next = rows - 1;  // Up from the field wraps to the last row.
  } else if (next >= rows) {
    next = -1;  // Down past the last row returns to the field's own text.
  }
  return next;
}

// What Enter commits: the typed text with its surrounding blanks removed.
inline std::wstring PhiOmniboxTrimTyped(const std::wstring& typed) {
  const size_t first = typed.find_first_not_of(L" \t");
  const size_t last = typed.find_last_not_of(L" \t");
  return first == std::wstring::npos ? std::wstring()
                                     : typed.substr(first, last - first + 1);
}

// True when the page's url must show as the PLACEHOLDER rather than itself —
// the new-tab page, whose chrome:// url the user never typed (the Mac's
// updateDisplayedURL isNTP blank).
inline bool PhiOmniboxHidesUrl(const std::string& url) {
  return url.rfind("chrome://newtab", 0) == 0;
}

}  // namespace phi_client

#if !defined(PHI_OMNIBOX_SESSION_MODEL_ONLY)

#include <windows.h>

#include "base/memory/raw_ptr_exclusion.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_ids.h"
#include "phibrowser-win/client/phi_shell_layout.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

// What the session needs from the shell window that owns it: geometry, the
// chrome's fonts and palette, the browser its text acts on, and the two
// hand-backs (focus to the page, the reader overlay out of the way) a commit
// ends with. A narrow interface rather than `friend`: the shell implements it
// privately, so none of this widens its public surface. Every name carries
// the component's prefix — the shell implements one such interface per
// component and they must not collide.
class PhiOmniboxShell {
 public:
  // The window the field, the popup and every repaint belong to.
  virtual HWND OmniboxShellWindow() const = 0;
  // The shell is being torn down: the field's remaining messages must not
  // reach the framework for a window that is going.
  virtual bool OmniboxShellDestroying() const = 0;
  // Design units → this monitor's pixels.
  virtual int OmniboxScaled(int design_units) const = 0;
  virtual HFONT OmniboxTextFont() const = 0;
  virtual HFONT OmniboxIconFont() const = 0;
  virtual PhiPalette OmniboxPalette() const = 0;
  virtual PhiLayoutMode OmniboxLayoutMode() const = 0;
  // This turn's capsule geometry (the shell's cached layout).
  virtual PhiOmniboxCapsule OmniboxCapsule() = 0;

  virtual phi_framework::PhiFramework* OmniboxFramework() const = 0;
  // The browser the slot shows — what typed text navigates.
  virtual BrowserId OmniboxBrowser() const = 0;
  // That browser is in placeholder mode: its strip is EMPTY, so a commit
  // must birth the tab before it navigates.
  virtual bool OmniboxBrowserPlaceholding() const = 0;
  // "New tab behavior = Omnibox": Ctrl+T focuses the field instead of
  // opening a page.
  virtual bool OmniboxNewTabFocusesField() const = 0;
  // Keys belong to the page again.
  virtual void OmniboxFocusContent() = 0;
  // A typed URL replaces the page the reader is covering.
  virtual void OmniboxLeaveReader() = 0;

  // The native new-tab page's own omnibox, which joins this session
  // (phi_new_tab_page.cc calls AttachField on it), or null with no page up.
  virtual HWND OmniboxNativePageField() const = 0;
  // That page's box in screen coords — the dropdown grows out of it.
  virtual bool OmniboxNativePageBox(RECT* out) const = 0;
  // A commit is about to land while the native page is up: it blanks its
  // controls now (omniBoxDidClear) so nothing of it lingers over the
  // arriving page.
  virtual void OmniboxNativePageCommitting() = 0;

 protected:
  // Non-virtual and protected: the shell OWNS the session, never the other
  // way round, so nothing ever deletes through this pointer.
  ~PhiOmniboxShell() = default;
};

class PhiOmniboxSession {
 public:
  // Reverts the copy-link checkmark after its second. The shell owns the
  // timer space, so its WM_TIMER routes this id back here.
  static constexpr UINT_PTR kCopyLinkTimer = 0x523;

  explicit PhiOmniboxSession(PhiOmniboxShell* shell);
  PhiOmniboxSession(const PhiOmniboxSession&) = delete;
  PhiOmniboxSession& operator=(const PhiOmniboxSession&) = delete;
  ~PhiOmniboxSession();

  // --- The address field ---------------------------------------------------
  // A real Win32 EDIT child, so typing, selection, clipboard, IME and the
  // caret all behave natively (Chromium's own omnibox is hollowed away under
  // IS_WIN_PHI_CLIENT, and hand-rolling text editing would reproduce it
  // badly). Created once the shell window exists.
  void EnsureField();
  // Joins another EDIT to this session — the native new-tab page's omnibox
  // gets the same subclass (Enter / Escape / arrows, the dropdown) and
  // reports its EN_CHANGE here.
  void AttachField(HWND edit);
  // By const REFERENCE, so the shell can publish a view of it (PhiMenuBar
  // still reads the field by member name — see phi_shell_window.h).
  const HWND& field() const { return field_; }
  // The session's field, or the native page's, whichever holds keyboard
  // focus — the query, the dropdown and the arrow fills all act on this one.
  HWND FocusedField() const;
  // The address field itself has focus (the native page's does not count).
  bool FieldHasFocus() const;
  // The shell's UI font changed (a DPI move).
  void SetFont(HFONT font);
  // Re-seats the EDIT inside this turn's capsule. Idempotent: the paint path
  // calls it too, and moving on no change keeps it from feeding itself more
  // paints.
  void Position();
  void SetFieldVisible(bool visible);
  // Moves the EDIT into `host` (a window of the shell's — the floating
  // sidebar panel) for as long as the shell has no field of its own: the
  // collapsed performance layout, where the panel shows the sidebar's
  // address bar and this is what makes it a REAL one. `offset` is where the
  // host's client origin sits in the shell's client space (the capsule is
  // laid out in the shell's coordinates; the field is placed in the host's).
  // The host forwards the EDIT's WM_COMMAND and WM_CTLCOLOREDIT to
  // OnShellCommand / OnCtlColorEdit, as the shell does. UnhostField hands
  // it back; the shell then decides its visibility.
  void HostField(HWND host, POINT offset);
  void UnhostField();
  bool field_hosted() const { return host_ != nullptr; }
  // Pushes the current page's URL into the field. Skipped while the user is
  // typing unless `force` (the Escape-reverts path).
  void Sync(bool force);
  // Navigates to whatever is typed in `edit` — the address field or the
  // native page's omnibox — then hands focus back to the page.
  void Commit(HWND edit);
  void CommitField();  // Commit(field_).
  // Focuses the field and selects it whole, when it is on screen; false when
  // there is no visible field to focus (placeholder mode's entry point).
  bool FocusFieldIfVisible();
  // The New Tab entry points: in omnibox mode this focuses the field and the
  // tab is born on commit; in page mode it opens a tab now.
  void StartNewTab();
  // The EDIT only repaints on its own events, so a palette change (Space
  // swap ramp, instant switch) must invalidate it or it keeps the old wash.
  void Repaint();
  // WM_CTLCOLOREDIT for `edit`: the palette's brush, or null when `edit` is
  // not this session's field.
  HBRUSH OnCtlColorEdit(HDC edit_dc, HWND edit);
  // WM_COMMAND from the shell: true when it was this field's EN_CHANGE.
  bool OnShellCommand(WPARAM wparam, LPARAM lparam);
  // EN_CHANGE while the user types: run/refresh the autocomplete query.
  void OnFieldChanged(HWND edit);

  // --- Navigation state ----------------------------------------------------
  // Re-reads the toolbar's navigation state from the active browser's
  // snapshot (null = no browser, which clears it).
  void RebuildNavState(const phi_framework::NavState* nav);
  // Const references for the same reason as field().
  const std::wstring& nav_url() const { return nav_url_; }
  const bool& can_go_back() const { return can_go_back_; }
  const bool& can_go_forward() const { return can_go_forward_; }
  const bool& nav_loading() const { return nav_loading_; }
  // What the field SHOWS — the typed text mid-edit, the page's url at rest.
  // The capture path paints this, because it renders without children.
  std::wstring DisplayedUrl() const;
  // The capsule's hover brighten (SideAddressBar hoveredColor: resting 4%
  // wash → emphasized 8%). Fed from BOTH sides — the shell's mouse moves for
  // the capsule's rim, the EDIT subclass for the text area the child covers.
  bool field_hover() const { return field_hover_; }
  void SetFieldHover(bool hovered);
  // The in-field copy-link button: the current URL to the clipboard, the
  // glyph swapped to a checkmark for a second (the Mac copyURLButton's
  // triggeredSystemName/triggeredRevertDelay).
  bool copy_link_flash() const { return copy_link_flash_; }
  void CopyLinkToClipboard();
  void RevertCopyLinkFlash();

  // --- Suggestion dropdown -------------------------------------------------
  // Autocomplete results for `browser_id`'s running query (the framework's
  // json — the one JSON site that stays, by design). A result for any other
  // browser, or one arriving after the user stopped typing, is stale.
  void OnResults(BrowserId browser_id, const std::string& json);
  void ClosePopup(bool stop_query);
  // Called by the dropdown's own window procedure.
  void PaintPopup(HWND popup, HDC dc);
  void PopupHover(int y);
  void PopupClick(int y);

 private:
  // Rows mirror the framework's OnOmniboxResults json; the popup is a
  // non-activating WS_POPUP under the address field, so the EDIT keeps focus
  // and its caret while the list is up.
  struct Row {
    int line = 0;
    std::string type;
    std::wstring contents;
    std::wstring description;
    std::wstring fill;  // fillIntoEdit — what arrowing onto the row types.
    bool has_tab_match = false;
  };

  void ShowPopup();  // Creates/positions the popup under the focused field.
  void MoveSelection(int delta);
  void CommitSelection();
  int RowAtY(int y) const;
  int Scaled(int design_units) const;

  // Subclass proc: Enter commits, Escape reverts, focus selects all.
  static LRESULT CALLBACK FieldProc(HWND hwnd,
                                    UINT msg,
                                    WPARAM wparam,
                                    LPARAM lparam);

  RAW_PTR_EXCLUSION PhiOmniboxShell* const shell_;

  HWND field_ = nullptr;
  // HostField's window and its client origin in shell space; null when the
  // field is the shell's own child.
  HWND host_ = nullptr;
  POINT host_offset_ = {};
  // The EDIT paints itself in SYSTEM colours, which clash under a dark
  // theme. WM_CTLCOLOREDIT hands it the palette instead; the brush must
  // outlive the message, so it is cached and rebuilt only when the colour
  // changes.
  HBRUSH brush_ = nullptr;
  COLORREF brush_color_ = CLR_INVALID;

  // Navigation state for the client toolbar, refreshed with the tab list.
  std::wstring nav_url_;
  bool can_go_back_ = false;
  bool can_go_forward_ = false;
  bool nav_loading_ = false;
  bool copy_link_flash_ = false;
  bool field_hover_ = false;

  std::vector<Row> rows_;
  int selected_ = -1;  // -1 = nothing armed; Enter commits the text.
  HWND popup_ = nullptr;
  bool programmatic_ = false;  // Suppresses EN_CHANGE feedback.
  // Set by StartNewTab in omnibox mode: the next commit opens a NEW tab.
  bool new_tab_pending_ = false;
};

}  // namespace phi_client

#endif  // !defined(PHI_OMNIBOX_SESSION_MODEL_ONLY)

#endif  // PHIBROWSER_WIN_CLIENT_PHI_OMNIBOX_SESSION_H_
