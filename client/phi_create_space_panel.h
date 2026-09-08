// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_CREATE_SPACE_PANEL_H_
#define PHIBROWSER_WIN_CLIENT_PHI_CREATE_SPACE_PANEL_H_

#include <windows.h>

#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_panel.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

class PhiWindowController;

// The Mac's CreateSpacePanel in its sidebar style (UserInterface/Sidebar/
// Spaces/CreateSpacePanel.swift, mounted by SidebarViewController.
// showCreateSpaceOverlay): the "Create a Space" form that fills the sidebar
// below the Spaces strip when the strip's "+" is clicked — a name field, the
// profile picker, the icon / emoji picker, the theme dots with their
// saturation slider, then Create and Cancel.
//
// A child window of the shell, like the native new-tab page: the sidebar
// content it covers stays painted underneath and simply shows again when the
// form leaves. The form arrives already reading as a NEW Space — a random
// Phi icon and a random theme distinct from the source Space's — previewed
// live: the theme on the whole chrome (the controller's palette override),
// the icon on the strip's dashed placeholder pip.
class PhiCreateSpacePanel : public PhiPanel {
 public:
  explicit PhiCreateSpacePanel(PhiWindowController* controller);
  ~PhiCreateSpacePanel() override;

  PhiCreateSpacePanel(const PhiCreateSpacePanel&) = delete;
  PhiCreateSpacePanel& operator=(const PhiCreateSpacePanel&) = delete;

  // Mounts the form over `bounds` (shell client pixels) in its clean state.
  // The profile picker opens on `initial_profile_id` (the active Space's),
  // the random theme avoids `source_theme_id`, and the slider rests at
  // `saturation` until the user drags it.
  void Show(const RECT& bounds,
            const std::string& initial_profile_id,
            const std::string& source_theme_id,
            int saturation);
  // Unmounts. Animated is the Mac's cancel — the form leaves to the right;
  // a successful create hides at once, its new Space presenting over this
  // window.
  void Hide(bool animated);
  bool visible() const;

  // Follows the sidebar (and the monitor's DPI) on relayout.
  void Reposition(const RECT& bounds);
  void Refresh();
  void FocusField();

  // A profile created from the form's "New Profile…" row has appeared: the
  // picker selects it (the Mac's promptCreateProfile completion).
  void OnProfileCreated(const std::string& profile_id);

 protected:
  // PhiPanel: the form's state, its paint and its hit handling. Everything
  // else — registration, the `this` binding, the cached back buffer, the
  // fonts, the dpi and the capture — comes from the base (phi_window.h).
  void Paint(HDC dc, const RECT& dirty) override;
  void OnSize(int width, int height) override;
  void OnDpiChanged(UINT old_dpi) override;
  void OnMouseDown(int hit, POINT client_pt) override;
  void OnWheel(int delta, POINT client_pt) override;
  void OnCaptureLost() override;
  bool OnKey(WPARAM virtual_key) override;
  PhiPalette QueryPalette() const override;
  LRESULT OnMessage(UINT message,
                    WPARAM wparam,
                    LPARAM lparam,
                    bool* handled) override;

 private:
  enum class Tab { kIcon, kEmoji };
  enum class Hit {
    kNone,
    kProfilePill,
    kTabIcon,
    kTabEmoji,
    kCell,
    kDot,
    kSlider,
    kCreate,
    kCancel,
  };
  struct Geometry;

  void EnsureWindow();
  void EnsureField();
  // The seven faces, out of the base's per-dpi font set. const because the
  // measuring passes run from const geometry code.
  HFONT TitleFont() const;
  HFONT SubtitleFont() const;
  HFONT BodyFont() const;
  HFONT BodyMediumFont() const;
  HFONT PillFont() const;
  HFONT EmojiFont() const;
  HFONT Mdl2Font() const;
  void LayoutField();
  void ReloadProfiles();
  std::wstring SelectedProfileName() const;
  const std::vector<std::pair<std::wstring, std::string>>& Cells() const;
  Geometry ComputeGeometry() const;
  RECT CellRect(const Geometry& g, size_t index) const;
  void ClampScroll(const Geometry& g);
  // The form's own richer hit test; the base's one-argument HitTest (the
  // accessibility hit list) stays visible beside it.
  using PhiWindow::HitTest;
  Hit HitTest(const Geometry& g, POINT pt, int* index) const;
  void SelectTheme(size_t index);
  void SelectIcon(size_t index);
  void SetSliderFromPoint(const Geometry& g, int x);
  void PushPreview();
  void ShowProfileMenu(const Geometry& g);
  void Create();
  void Cancel();
  void PaintForm(HDC dc);
  void PaintCells(HDC dc, const Geometry& g, const PhiPalette& palette);
  void PaintSlider(HDC dc, const RECT& bounds, const PhiPalette& palette);

  static LRESULT CALLBACK FieldProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);

  RAW_PTR_EXCLUSION PhiWindowController* const controller_;
  HWND field_ = nullptr;
  WNDPROC field_base_proc_ = nullptr;
  HBRUSH field_brush_ = nullptr;
  COLORREF field_brush_color_ = 0;

  // The form's state — CreateSpacePanel's @State.
  std::vector<std::pair<std::string, std::wstring>> profiles_;  // id, name
  std::string profile_id_;
  std::string icon_;      // Storage form ("phi:<id>" / "emoji:<id>").
  std::string theme_id_;
  int slider_ = 50;
  bool user_adjusted_slider_ = false;
  Tab tab_ = Tab::kIcon;
  int grid_scroll_ = 0;   // The picker's own scroll, pixels.
  int form_scroll_ = 0;   // The column's scroll when taller than the sidebar.
  Hit hover_ = Hit::kNone;
  int hover_index_ = -1;
  bool dragging_slider_ = false;
  // Held while the saturation slider is being dragged; the base ends it on
  // WM_CAPTURECHANGED and WM_CANCELMODE alike (lane 3 §4.2).
  Capture slider_capture_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_CREATE_SPACE_PANEL_H_
