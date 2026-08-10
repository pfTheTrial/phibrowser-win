// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_

#include <windows.h>

#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_tab_store.h"
#include "phibrowser-win/client/phi_theme.h"
#include "phibrowser-win/client/phi_window_controller.h"

namespace phi_client {

// What the New Tab shortcut opens — the Mac's NewTabShortcutBehavior.
enum class PhiNewTabBehavior { kNewTabPage, kOmnibox };

// Everything the settings window reads and writes. Implemented by the
// coordinator, which owns the persisted values and pushes changes to the slots.
class PhiSettingsHost {
 public:
  virtual ~PhiSettingsHost() = default;

  // Theme
  virtual std::string ThemeId() const = 0;
  virtual PhiAppearance Appearance() const = 0;
  virtual int Saturation() const = 0;  // 0..100, the Mac's slider range.
  virtual void SetThemeId(const std::string& id) = 0;
  virtual void SetAppearance(PhiAppearance appearance) = 0;
  virtual void SetSaturation(int value) = 0;
  virtual const PhiPalette& Palette() const = 0;

  // Appearance
  virtual PhiLayoutMode LayoutMode() const = 0;
  virtual void RequestLayoutMode(PhiLayoutMode mode) = 0;

  // Browsing
  virtual PhiNewTabBehavior NewTabBehavior() const = 0;
  virtual void SetNewTabBehavior(PhiNewTabBehavior behavior) = 0;
  virtual bool AlwaysShowFullUrl() const = 0;
  virtual void SetAlwaysShowFullUrl(bool enabled) = 0;

  // Spaces / Profiles
  virtual PhiPinScope PinScope() const = 0;
  virtual void SetPinScope(PhiPinScope scope) = 0;
  // "name\ticonText\tprofileId" per Space; "id\tname" per profile.
  virtual std::vector<std::string> SpaceSummaries() const = 0;
  virtual std::vector<std::string> ProfileSummaries() const = 0;
  virtual void CreateProfileNamed(const std::string& name) = 0;
};

// The settings window, shaped like the Mac's Preferences: a TOOLBAR of panes
// across the top (SettingsWindowController is created with style:
// .toolbarItems — not a sidebar), and below it sections, each a small title
// over a rounded CARD holding its rows (GeneralSectionView wrapping a
// GeneralContainerView; label left, control trailing). A real
// top-level window, NOT an overlay inside a shell: the content region belongs
// to Chromium's DirectComposition surface, which composites above anything GDI
// draws there (windows-framework-design.md — the falsified overlay approach).
class PhiSettingsWindow {
 public:
  static void Open(PhiSettingsHost* host);
  static void Refresh();
  // The open window's HWND, or null. Reported over the control socket: a caller
  // in another session cannot enumerate our windows to find it.
  static HWND Handle();
  static std::string CurrentSection();
  static bool ShowSection(const std::string& name);

  ~PhiSettingsWindow();

  static LRESULT CALLBACK WndProc(HWND hwnd,
                                  UINT msg,
                                  WPARAM wparam,
                                  LPARAM lparam);

 private:
  explicit PhiSettingsWindow(PhiSettingsHost* host);

  // One drawn thing. Rows are built per section and hit-tested from the same
  // list, so what is drawn and what is clickable cannot drift apart.
  enum class ItemKind {
    kToolbarItem,    // A pane, in the toolbar across the top.
    kCard,           // The well a section's rows sit in.
    kSectionTitle,   // Small secondary label ABOVE a card.
    kRowLabel,
    kHint,
    kChip,
    kSwatch,
    kToggle,
    kSliderTrack,
    kListRow,
    kButton,
  };
  struct Item {
    ItemKind kind = ItemKind::kRowLabel;
    RECT bounds{};
    std::wstring text;
    bool selected = false;
    bool on = false;
    COLORREF swatch = 0;
    int value = 0;       // Slider position, or a chip's option index.
    std::string action;  // What a click does; empty = not clickable.
    std::string payload;
  };

  void Rebuild();
  void BuildGeneral(int* y);
  void BuildSpaces(int* y);
  void BuildProfiles(int* y);
  void BuildPlaceholder(int* y, const wchar_t* text);
  // A section = a small title, then a card holding its rows. BeginSection
  // remembers where the card starts; EndSection sizes it around the rows that
  // were added, the way the Mac's GeneralSectionView wraps a
  // GeneralContainerView.
  void BeginSection(int* y, const wchar_t* title);
  void EndSection(int* y);
  void AddHint(int* y, const wchar_t* text);
  // A row inside the open card; returns the rect its control occupies,
  // RIGHT-ALIGNED as on Mac (label left, Spacer, accessory trailing).
  RECT AddRow(int* y, const wchar_t* title, int control_height,
              int control_width);
  void AddChips(const RECT& area,
                const std::vector<std::wstring>& labels,
                int selected_index,
                const std::string& action);

  void Paint(HDC dc);
  void OnClick(int x, int y);
  void Apply(const Item& item, int x);
  int Scaled(int design_units) const;
  void EnsureFont();

  RAW_PTR_EXCLUSION PhiSettingsHost* host_;
  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;
  HFONT title_font_ = nullptr;
  HFONT small_font_ = nullptr;  // Section titles and toolbar labels.
  UINT font_dpi_ = 0;
  UINT dpi_ = 96;
  std::string section_ = "general";
  std::vector<Item> items_;
  // Index of the card being filled, so EndSection can size it.
  size_t open_card_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_SETTINGS_WINDOW_H_
