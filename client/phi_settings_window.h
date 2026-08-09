// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_

#include <windows.h>

#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

// What the settings window changes. Implemented by the coordinator, which owns
// the persisted values and pushes them to every slot.
class PhiSettingsHost {
 public:
  virtual ~PhiSettingsHost() = default;
  virtual std::string ThemeId() const = 0;
  virtual PhiAppearance Appearance() const = 0;
  virtual PhiLayoutMode LayoutMode() const = 0;
  virtual const PhiPalette& Palette() const = 0;
  virtual void SetThemeId(const std::string& id) = 0;
  virtual void SetAppearance(PhiAppearance appearance) = 0;
  virtual void RequestLayoutMode(PhiLayoutMode mode) = 0;
};

// A real top-level settings window, NOT an overlay inside a shell window: the
// content region belongs to Chromium's DirectComposition surface, which
// composites above anything GDI draws there (windows-framework-design.md — the
// falsified overlay approach). Its own window sidesteps that entirely.
//
// Sections mirror the Mac's General settings: Appearance (System / Light /
// Dark), Theme (the eight built-ins), Layout (the three modes).
class PhiSettingsWindow {
 public:
  // Shows the window, creating it on first use, and brings it forward.
  static void Open(PhiSettingsHost* host);
  // Repaints an open settings window after a change made elsewhere.
  static void Refresh();
  // The open window's HWND, or null. Reported over the control socket: a
  // caller in another session cannot enumerate our windows to find it.
  static HWND Handle();

  ~PhiSettingsWindow();

  static LRESULT CALLBACK WndProc(HWND hwnd,
                                  UINT msg,
                                  WPARAM wparam,
                                  LPARAM lparam);

 private:
  explicit PhiSettingsWindow(PhiSettingsHost* host);

  void Paint(HDC dc);
  void OnClick(int x, int y);
  int Scaled(int design_units) const;
  void EnsureFont();

  // A clickable choice: its rect and what picking it does.
  struct Choice {
    RECT bounds{};
    std::wstring label;
    bool selected = false;
    COLORREF swatch = 0;   // Themes draw their tint; 0 means "no swatch".
    int kind = 0;          // 0 appearance, 1 theme, 2 layout
    int index = 0;
    std::string id;
  };
  void Rebuild();

  RAW_PTR_EXCLUSION PhiSettingsHost* host_;
  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;
  UINT font_dpi_ = 0;
  UINT dpi_ = 96;
  std::vector<Choice> choices_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_
