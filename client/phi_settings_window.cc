// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_settings_window.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_prompt.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiSettingsWindow";

// Is Phi the browser Windows launches for an http: URL?
//
// The Mac asks LaunchServices for the handler of "http://example.com" and
// compares BUNDLE IDENTIFIERS. Windows has no bundle identity, so the same
// question is asked of the association store and answered by comparing
// EXECUTABLE PATHS — AssocQueryString resolves the user's choice through
// UserChoice and the ProgId's open command, which is the whole chain the
// shell itself walks.
//
// A registry read, not file I/O, so it does not need the raw-Win32 dance the
// rest of the client does to stay off the UI thread's blocking check.
bool ThisExeIsDefaultBrowser() {
  wchar_t handler[MAX_PATH * 2] = {};
  DWORD length = static_cast<DWORD>(std::size(handler));
  if (FAILED(::AssocQueryStringW(
          ASSOCF_IS_PROTOCOL | ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_EXECUTABLE,
          L"http", nullptr, handler, &length))) {
    return false;  // No handler at all, or none this API can name.
  }
  wchar_t self[MAX_PATH * 2] = {};
  if (!::GetModuleFileNameW(nullptr, self,
                            static_cast<DWORD>(std::size(self)))) {
    return false;
  }
  const bool ours =
      ::CompareStringOrdinal(handler, -1, self, -1, /*bIgnoreCase=*/TRUE) ==
      CSTR_EQUAL;
  // The one observable for a state the user cannot otherwise inspect: when
  // the row says "not your default browser", this says who is.
  LOG(INFO) << "[PhiClient] http handler: " << base::WideToUTF8(handler)
            << (ours ? " (ours)" : " (not ours)");
  return ours;
}

// The Mac window: content is a FIXED 680x561 in every pane
// (each pane pins view.snp to that size); the toolbar rides above it.
constexpr int kWidth = 680;
constexpr int kHeight = 561;
// The custom caption strip: the OS caption is retired (WM_NCCALCSIZE) so
// the pane title can sit CENTERED with the Phi mark leading — the Mac
// window's look. The close button uses the Win11 caption metric.
constexpr int kCaptionHeight = 32;
constexpr int kCaptionButtonWidth = 46;
constexpr int kToolbarHeight = 68;
// Everything below the caption strip and the pane toolbar.
constexpr int kChromeTop = kCaptionHeight + kToolbarHeight;
constexpr int kToolbarItemWidth = 78;
constexpr int kToolbarIcon = 22;
// Page insets 36 on every side; 24 between sections; 16 title -> card.
constexpr int kMargin = 36;
constexpr int kSectionGap = 24;
constexpr int kTitleToCard = 16;
constexpr int kCardPadding = 12;   // Inner horizontal padding of a card.
constexpr int kRowPadding = 12;    // Vertical padding around a row's content.
constexpr int kSectionTitleHeight = 15;  // 12pt section titles.
constexpr int kHintHeight = 15;          // 11pt hints.
constexpr int kRowLabelHeight = 16;      // 13pt labels.
constexpr int kCardRadius = 7;
constexpr int kRadius = 6;
// Theme swatches: a 22pt dot in a 26pt ring frame, 2pt ring when selected,
// caption (11pt) below only while selected (ThemeSwatchView).
constexpr int kSwatchDot = 22;  // Selection ring = dot + 4.
constexpr int kSwatchItemGeneral = 30;   // Per-item width in General…
constexpr int kSwatchGapGeneral = 13;    // …with 13pt spacing.
// The Spaces detail's compact swatches (SpacesSettingsView's overrides):
// dot 16 in a 20pt ring frame, 10pt spacing, 10pt caption.
constexpr int kSwatchDotSpaces = 16;
constexpr int kSwatchItemSpaces = 20;
constexpr int kSwatchGapSpaces = 10;
constexpr int kSwatchBlock = 26 + 8 + 15;  // ring + gap + caption line.
constexpr int kSwatchBlockSpaces = 20 + 8 + 13;
// The saturation slider: 10pt-tall full-width rounded bar with a gradient,
// an 18pt white knob (ThemeOpacitySliderView).
constexpr int kSliderHeight = 20;
constexpr int kSliderGeneral = 324;
constexpr int kSliderSpaces = 200;  // themeSliderWidth in the Spaces detail.
constexpr int kSliderKnob = 18;
// Layout / appearance / new-tab picture cards: 122x72 image, r5, 2pt accent
// ring when selected, 11pt caption 10 below, 16 apart (GeneralSttingCardView).
constexpr int kImageCardW = 122;
constexpr int kImageCardH = 72;
constexpr int kImageCardGap = 16;
constexpr int kImageCardBlock = 72 + 10 + 15;
// Switches: the Mac's .mini switch is about 26x15.
constexpr int kToggleWidth = 26;
constexpr int kToggleHeight = 15;
// Master-detail lists (Profiles 240 wide, Spaces 280) with a 34pt tool strip.
constexpr int kProfilesListWidth = 240;
constexpr int kSpacesListWidth = 280;
constexpr int kMasterDetailGap = 16;
constexpr int kListHeaderHeight = 34;
constexpr int kListRowHeight = 32;  // 6/6 vertical padding around 20pt content.
constexpr int kToolStripHeight = 34;
constexpr int kSpacesBlockHeight = 431;  // The Mac's fixed master-detail band.
// The Profiles trailing pill: inner width 110, H10/V5, primary@0.06, r6.
constexpr int kPillWidth = 130;
constexpr int kPillHeight = 24;
// Icon picker grid (IconPicker): 26pt cells, 4pt gaps, r10 cells.
constexpr int kEmojiCell = 26;
constexpr int kEmojiGap = 4;

// The Mac's panes(), in order (AppController+Settings.panes(): Account,
// General, Profiles, Spaces, Navigation, Phi & AI, Shortcuts, Developer —
// there is NO Phi Link pane; IMChannels exists in source but is never
// mounted). The Account pane is titled with the user's name ("You" when
// signed out).
// Glyphs are MDL2 codepoints standing in for the Mac's 32pt template PDFs
// (~14-18pt of ink); Developer mirrors the Mac's SF "hammer".
struct SectionDef {
  const char* id;
  const wchar_t* label;
  wchar_t glyph;
};
constexpr std::array<SectionDef, 8> kSections = {{
    {"account", L"You", 0xE77B},        // Contact
    {"general", L"General", 0xE713},    // Settings gear
    {"profiles", L"Profiles", 0xE716},  // People
    {"spaces", L"Spaces", 0xE8A9},      // ViewAll grid
    {"navigation", L"Navigation", 0xE71B},  // Link — the Mac's navigations.svg
    {"ai", L"Phi & AI", 0xE734},        // Sparkle (the catalog's own pick)
    {"shortcuts", L"Shortcuts", 0xE765},  // Keyboard
    {"developer", L"Developer", 0xE90F},  // Repair — the Mac's hammer
}};

PhiSettingsWindow* g_instance = nullptr;

// The shape helpers take the TRUE corner radius (a capsule is half the
// height) and render antialiased — see phi_dwrite.h. Their GDI forebears
// took the corner ellipse's diameter, so a bar asked for "radius 4" came
// out at 2, and every curve was stair-stepped.
void FillRounded(HDC dc, const RECT& r, COLORREF fill, int radius) {
  PhiFillRoundRectAA(dc, r, radius, fill);
}

void StrokeRounded(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
                   int radius, int width) {
  PhiFillRoundRectAA(dc, r, radius, fill);
  PhiStrokeRoundRectAA(dc, r, radius, border, width);
}

void Ellipse(HDC dc, const RECT& r, COLORREF fill, COLORREF border,
             int border_width) {
  PhiFillEllipseAA(dc, r, fill);
  if (border_width > 0) {
    PhiStrokeEllipseAA(dc, r, border, border_width);
  }
}

// `over` at alpha/255 on `base` — the Mac's percentage overlays, opaque.
COLORREF Blend(COLORREF over, COLORREF base, int alpha) {
  auto mix = [&](int a, int b) { return (a * alpha + b * (255 - alpha)) / 255; };
  return RGB(mix(GetRValue(over), GetRValue(base)),
             mix(GetGValue(over), GetGValue(base)),
             mix(GetBValue(over), GetBValue(base)));
}

// A rounded border with NO fill — for outlines over already-painted content
// (StrokeRounded fills, which erased the slider gradient it framed).
void OutlineRounded(HDC dc, const RECT& r, COLORREF border, int radius,
                    int width) {
  PhiStrokeRoundRectAA(dc, r, radius, border, width);
}

// RGB <-> HSV, for the saturation slider's track gradient — the Mac ramps the
// theme overlay's HSB saturation 0.1 -> 0.9 across the bar (and, for Pure,
// its brightness instead).
void RgbToHsv(COLORREF c, double* h, double* s, double* v) {
  const double r = GetRValue(c) / 255.0, g = GetGValue(c) / 255.0,
               b = GetBValue(c) / 255.0;
  const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
  const double d = mx - mn;
  *v = mx;
  *s = mx <= 0.0 ? 0.0 : d / mx;
  if (d <= 0.0) {
    *h = 0.0;
  } else if (mx == r) {
    *h = 60.0 * std::fmod((g - b) / d, 6.0);
  } else if (mx == g) {
    *h = 60.0 * ((b - r) / d + 2.0);
  } else {
    *h = 60.0 * ((r - g) / d + 4.0);
  }
  if (*h < 0) {
    *h += 360.0;
  }
}

COLORREF HsvToRgb(double h, double s, double v) {
  const double c = v * s;
  const double x = c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0));
  const double m = v - c;
  double r = 0, g = 0, b = 0;
  if (h < 60) {
    r = c; g = x;
  } else if (h < 120) {
    r = x; g = c;
  } else if (h < 180) {
    g = c; b = x;
  } else if (h < 240) {
    g = x; b = c;
  } else if (h < 300) {
    r = x; b = c;
  } else {
    r = c; b = x;
  }
  return RGB(static_cast<int>((r + m) * 255 + 0.5),
             static_cast<int>((g + m) * 255 + 0.5),
             static_cast<int>((b + m) * 255 + 0.5));
}

// Re-reads a selected profile whose data is still loading (see
// BuildProfileDetail); one-shot, re-armed per rebuild while pending.
constexpr UINT_PTR kProfilePollTimer = 0x517;

// The Mac's NSOpenPanel (directories only): the modern IFileOpenDialog in
// folder-picking mode. Empty when cancelled. The browser UI thread is
// already a COM STA, so no CoInitialize here.
std::wstring PickFolder(HWND owner, const wchar_t* title) {
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) {
    return std::wstring();
  }
  DWORD options = 0;
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  dialog->SetTitle(title);
  if (FAILED(dialog->Show(owner))) {
    return std::wstring();  // Cancelled.
  }
  Microsoft::WRL::ComPtr<IShellItem> item;
  if (FAILED(dialog->GetResult(&item))) {
    return std::wstring();
  }
  wchar_t* path = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) {
    return std::wstring();
  }
  std::wstring result = path;
  ::CoTaskMemFree(path);
  return result;
}

// "a\tb\tc" -> {"a","b","c"}
std::vector<std::string> SplitTabs(const std::string& value) {
  std::vector<std::string> parts;
  size_t start = 0;
  for (;;) {
    const size_t tab = value.find('\t', start);
    if (tab == std::string::npos) {
      parts.push_back(value.substr(start));
      return parts;
    }
    parts.push_back(value.substr(start, tab - start));
    start = tab + 1;
  }
}

}  // namespace

PhiSettingsWindow::~PhiSettingsWindow() {
  if (font_) {
    ::DeleteObject(font_);
  }
  if (title_font_) {
    ::DeleteObject(title_font_);
  }
  if (small_font_) {
    ::DeleteObject(small_font_);
  }
  if (hint_font_) {
    ::DeleteObject(hint_font_);
  }
  if (emoji_font_) {
    ::DeleteObject(emoji_font_);
  }
  if (mdl2_font_) {
    ::DeleteObject(mdl2_font_);
  }
  if (caption_font_) {
    ::DeleteObject(caption_font_);
  }
}

PhiSettingsWindow::PhiSettingsWindow(PhiSettingsHost* host) : host_(host) {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PhiSettingsWindow::WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    return ::RegisterClassExW(&wc);
  }();
  (void)atom;

  hwnd_ = ::CreateWindowExW(0, kClassName, L"General",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr,
                            nullptr, ::GetModuleHandleW(nullptr), this);
  if (!hwnd_) {
    LOG(ERROR) << "[PhiClient] settings CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }
  if (const UINT dpi = ::GetDpiForWindow(hwnd_)) {
    dpi_ = dpi;
  }
  // Content 680x561 under the caption strip and toolbar band — fixed,
  // non-resizable. WM_NCCALCSIZE keeps only the thin side/bottom borders,
  // so the window size is the content plus exactly those.
  const int border_x = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi_) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
  const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi_) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
  ::SetWindowPos(hwnd_, nullptr, 0, 0, Scaled(kWidth) + 2 * border_x,
                 Scaled(kChromeTop + kHeight) + border_y,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
  // Phi's own icon in the taskbar and alt-tab — not the stock one.
  if (HICON big = PhiAppIcon(32)) {
    ::SendMessageW(hwnd_, WM_SETICON, ICON_BIG,
                   reinterpret_cast<LPARAM>(big));
  }
  if (HICON small_icon = PhiAppIcon(16)) {
    ::SendMessageW(hwnd_, WM_SETICON, ICON_SMALL,
                   reinterpret_cast<LPARAM>(small_icon));
  }
  EnsureFont();
  SyncTitle();
}

int PhiSettingsWindow::Scaled(int design_units) const {
  return ::MulDiv(design_units, static_cast<int>(dpi_), 96);
}

void PhiSettingsWindow::EnsureFont() {
  if (font_ && font_dpi_ == dpi_) {
    return;
  }
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                    &metrics, 0, dpi_)) {
    return;
  }
  // The Mac's ladder: 13pt rows, 12pt section titles, 11pt hints.
  auto make = [&](int size, int weight) {
    LOGFONTW lf = metrics.lfMessageFont;
    lf.lfHeight = -Scaled(size);
    lf.lfWeight = weight;
    return ::CreateFontIndirectW(&lf);
  };
  auto swap = [](HFONT* slot, HFONT built) {
    if (!built) {
      return;
    }
    if (*slot) {
      ::DeleteObject(*slot);
    }
    *slot = built;
  };
  swap(&font_, make(13, FW_NORMAL));
  swap(&title_font_, make(13, FW_SEMIBOLD));
  swap(&small_font_, make(12, FW_NORMAL));
  swap(&hint_font_, make(11, FW_NORMAL));
  LOGFONTW emoji = metrics.lfMessageFont;
  const std::wstring face = L"Segoe UI Emoji";
  std::fill(std::begin(emoji.lfFaceName), std::end(emoji.lfFaceName), L'\0');
  std::copy_n(face.begin(),
              std::min(face.size(), std::size(emoji.lfFaceName) - 1),
              std::begin(emoji.lfFaceName));
  emoji.lfHeight = -Scaled(14);
  swap(&emoji_font_, ::CreateFontIndirectW(&emoji));
  LOGFONTW mdl2 = metrics.lfMessageFont;
  const std::wstring mdl2_face = L"Segoe MDL2 Assets";
  std::fill(std::begin(mdl2.lfFaceName), std::end(mdl2.lfFaceName), L'\0');
  std::copy_n(mdl2_face.begin(),
              std::min(mdl2_face.size(), std::size(mdl2.lfFaceName) - 1),
              std::begin(mdl2.lfFaceName));
  mdl2.lfHeight = -Scaled(14);
  swap(&mdl2_font_, ::CreateFontIndirectW(&mdl2));
  LOGFONTW caption = mdl2;
  caption.lfHeight = -Scaled(10);  // The OS caption glyph size.
  swap(&caption_font_, ::CreateFontIndirectW(&caption));
  font_dpi_ = dpi_;
}

void PhiSettingsWindow::SyncTitle() {
  if (section_ == "urlrules") {
    ::SetWindowTextW(hwnd_, L"URL Rules");
    return;
  }
  // The Mac library titles the window with the selected pane's title.
  for (const SectionDef& section : kSections) {
    if (section_ == section.id) {
      ::SetWindowTextW(hwnd_, section.label);
      return;
    }
  }
}

// static
void PhiSettingsWindow::Open(PhiSettingsHost* host) {
  if (!g_instance || !g_instance->hwnd_ || !::IsWindow(g_instance->hwnd_)) {
    delete g_instance;
    g_instance = new PhiSettingsWindow(host);
  }
  if (g_instance && g_instance->hwnd_) {
    ::ShowWindow(g_instance->hwnd_, SW_SHOW);
    ::SetForegroundWindow(g_instance->hwnd_);
    ::InvalidateRect(g_instance->hwnd_, nullptr, TRUE);
  }
}

// static
HWND PhiSettingsWindow::Handle() {
  if (g_instance && g_instance->hwnd_ && ::IsWindow(g_instance->hwnd_)) {
    return g_instance->hwnd_;
  }
  return nullptr;
}

// static
void PhiSettingsWindow::Refresh() {
  if (Handle()) {
    ::InvalidateRect(g_instance->hwnd_, nullptr, TRUE);
  }
}

// static
std::string PhiSettingsWindow::CurrentSection() {
  return g_instance ? g_instance->section_ : std::string();
}

// static
bool PhiSettingsWindow::ShowSection(const std::string& name) {
  for (const SectionDef& section : kSections) {
    if (name != section.id) {
      continue;
    }
    // Developer exists only in developer mode, exactly as on Mac.
    if (name == "developer" && g_instance && g_instance->host_ &&
        !g_instance->host_->DeveloperMode()) {
      return false;
    }
    // Phi & AI is not mounted while the AI suite is off.
    if (name == "ai" && !kPhiAiEnabled) {
      return false;
    }
    if (g_instance) {
      g_instance->section_ = name;
      g_instance->scroll_ = 0;
      g_instance->SyncTitle();
      Refresh();
    }
    return true;
  }
  return false;
}

// static
void PhiSettingsWindow::ShowSpacePicker(const std::string& space_id) {
  if (!g_instance) {
    return;
  }
  g_instance->section_ = "spaces";
  g_instance->selected_space_ = space_id;
  g_instance->scroll_ = 0;
  g_instance->SyncTitle();
  Refresh();
}

// --- Section plumbing --------------------------------------------------------

void PhiSettingsWindow::BeginSection(int* y, const wchar_t* title) {
  if (title && title[0]) {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = title;
    heading.bounds = {Scaled(kMargin), *y, Scaled(kWidth - kMargin),
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    *y += Scaled(kSectionTitleHeight) + Scaled(kTitleToCard) -
          Scaled(kRowPadding);
  }
  // The card is emitted FIRST so it paints behind its rows; EndSection sizes
  // it around whatever was added.
  Item card;
  card.kind = ItemKind::kCard;
  card.bounds = {Scaled(kMargin), *y + Scaled(kRowPadding) - Scaled(4),
                 Scaled(kWidth - kMargin), 0};
  items_.push_back(std::move(card));
  open_card_ = items_.size() - 1;
  *y += Scaled(kRowPadding);
}

void PhiSettingsWindow::EndSection(int* y) {
  items_[open_card_].bounds.bottom = *y + Scaled(4);
  *y += Scaled(kSectionGap);
}

void PhiSettingsWindow::AddHint(int* y, const wchar_t* text) {
  Item hint;
  hint.kind = ItemKind::kHint;
  hint.text = text;
  hint.bounds = {Scaled(kMargin + kCardPadding), *y,
                 Scaled(kWidth - kMargin - kCardPadding),
                 *y + Scaled(kHintHeight)};
  items_.push_back(std::move(hint));
  *y += Scaled(kHintHeight) + Scaled(6);
}

RECT PhiSettingsWindow::AddRow(int* y, const wchar_t* title,
                               int control_height, int control_width) {
  const int row_height =
      std::max(Scaled(kRowLabelHeight), Scaled(control_height));
  Item label;
  label.kind = ItemKind::kRowLabel;
  label.text = title;
  label.bounds = {Scaled(kMargin + kCardPadding), *y,
                  Scaled(kWidth - kMargin - kCardPadding) -
                      Scaled(control_width) - Scaled(12),
                  *y + row_height};
  items_.push_back(std::move(label));
  // The control is TRAILING, as on Mac (label, Spacer, accessory).
  RECT control = {Scaled(kWidth - kMargin - kCardPadding) -
                      Scaled(control_width),
                  *y + (row_height - Scaled(control_height)) / 2,
                  Scaled(kWidth - kMargin - kCardPadding),
                  *y + (row_height + Scaled(control_height)) / 2};
  *y += row_height + Scaled(kRowPadding);
  return control;
}

void PhiSettingsWindow::AddChips(const RECT& area,
                                 const std::vector<std::wstring>& labels,
                                 int selected_index,
                                 const std::string& action) {
  // Retained for popup-style option groups: one popup pill showing the
  // current choice; clicking cycles to the next option (the Mac uses a menu).
  Item pill;
  pill.kind = ItemKind::kPopup;
  if (selected_index >= 0 &&
      selected_index < static_cast<int>(labels.size())) {
    pill.text = labels[static_cast<size_t>(selected_index)];
  }
  pill.value = selected_index;
  pill.action = action;
  pill.bounds = area;
  items_.push_back(std::move(pill));
}

// --- Panes -------------------------------------------------------------------

void PhiSettingsWindow::BuildGeneral(int* y) {
  const auto& themes = PhiBuiltInThemes();
  // With more than one Space, each Space owns colour and saturation; the Mac
  // locks these rows at 40% and adds the jump link.
  const bool locked = host_->SpaceSummaries().size() > 1;

  // The Mac's section order: Language, Theme, Appearance, Browsing,
  // Profile, Developer.
  BeginSection(y, PhiStr("settings.general.language.sectionTitle"));
  {
    // The row was a dead chip until the string catalog existed. Now it is the
    // language picker: "System default" leads because it is the default and
    // the only choice that follows a later OS change, and the rest are
    // endonyms, so someone hunting for their own language does not first have
    // to read the language the app happens to be in.
    const RECT area = AddRow(y, PhiStr("settings.displayLanguage"), kPillHeight,
                             kPillWidth);
    AddSubHint(y, PhiStr("settings.languageHint"));
    Item pill;
    pill.kind = ItemKind::kPopup;
    pill.text = host_->LanguagePreference().empty()
                    ? PhiStr("settings.languageSystemDefault")
                    : PhiLocaleDisplayName(host_->LanguagePreference());
    pill.action = "language";
    pill.bounds = area;
    items_.push_back(std::move(pill));
  }
  EndSection(y);

  BeginSection(y, L"Theme");
  {
    // Row "Color": 22pt dots on 30pt centres, caption under the selected one.
    const int width = static_cast<int>(themes.size()) * kSwatchItemGeneral +
                      (static_cast<int>(themes.size()) - 1) * kSwatchGapGeneral;
    const RECT area = AddRow(y, L"Color", kSwatchBlock, width);
    for (size_t i = 0; i < themes.size(); ++i) {
      Item swatch;
      swatch.kind = ItemKind::kSwatch;
      swatch.text = base::UTF8ToWide(themes[i].name);
      swatch.payload = themes[i].id;
      swatch.selected = host_->ThemeId() == themes[i].id;
      swatch.enabled = !locked;
      const PhiPalette resolved = PhiResolvePalette(
          themes[i], host_->Appearance(), host_->Saturation());
      // The dot is the theme's own colour; Pure fills white (contrast ring).
      // The selection ring is that theme's OWN accent, as on Mac.
      swatch.swatch = i == 0 ? RGB(0xFF, 0xFF, 0xFF) : resolved.accent;
      swatch.ring = resolved.accent;
      swatch.action = "theme";
      const int x = area.left + static_cast<int>(i) *
                                    Scaled(kSwatchItemGeneral +
                                           kSwatchGapGeneral);
      swatch.bounds = {x, area.top, x + Scaled(kSwatchItemGeneral),
                       area.top + Scaled(kSwatchBlock)};
      items_.push_back(std::move(swatch));
    }
  }
  AddDividerRow(y);
  {
    const RECT area = AddRow(y, L"Saturation", kSliderHeight, kSliderGeneral);
    Item slider;
    slider.kind = ItemKind::kSliderTrack;
    slider.value = host_->Saturation();
    slider.enabled = !locked;
    slider.action = "saturation";
    // The track ramps the SELECTED theme's overlay colour (Pure ramps
    // brightness instead — flagged via `on`).
    const PhiTheme& selected = PhiThemeById(host_->ThemeId());
    slider.swatch = host_->Palette().dark ? selected.dark.overlay
                                          : selected.light.overlay;
    slider.on = host_->ThemeId() == "pure";
    slider.bounds = area;
    items_.push_back(std::move(slider));
  }
  if (locked) {
    AddDividerRow(y);
    // The Mac's exact lock text, with the jump link trailing.
    const RECT area = AddRow(y,
                             L"You have more than one Space, so each Space "
                             L"sets its own color and opacity.",
                             kRowLabelHeight, 160);
    Item link;
    link.kind = ItemKind::kLink;
    link.text = L"Edit in Spaces settings…";
    link.action = "gospaces";
    link.bounds = area;
    items_.push_back(std::move(link));
  }
  AddDividerRow(y);
  AddToggleRow(y, L"Apply theme to text selection on web pages",
               /*on=*/true, /*enabled=*/false, std::string());
  EndSection(y);

  BeginSection(y, L"Appearance");
  {
    // "Layout mode": three picture cards, in the Mac's allCases order —
    // Balanced, Performance, Comfortable.
    const RECT area = AddRow(y, L"Layout mode", kImageCardBlock,
                             3 * kImageCardW + 2 * kImageCardGap);
    const struct {
      const wchar_t* title;
      PhiLayoutMode mode;
      const char* sketch;
    } kCards[] = {
        {L"Balanced", PhiLayoutMode::kBalanced, "balanced"},
        {L"Performance", PhiLayoutMode::kPerformance, "performance"},
        {L"Comfortable", PhiLayoutMode::kComfortable, "comfortable"},
    };
    int i = 0;
    for (const auto& definition : kCards) {
      Item card;
      card.kind = ItemKind::kImageCard;
      card.text = definition.title;
      card.payload = definition.sketch;
      card.selected = host_->LayoutMode() == definition.mode;
      card.value = static_cast<int>(definition.mode);
      card.action = "layout";
      const int x = area.left + i * Scaled(kImageCardW + kImageCardGap);
      card.bounds = {x, area.top, x + Scaled(kImageCardW),
                     area.top + Scaled(kImageCardBlock)};
      items_.push_back(std::move(card));
      ++i;
    }
  }
  AddDividerRow(y);
  {
    const RECT area = AddRow(y, L"Color appearance", kImageCardBlock,
                             3 * kImageCardW + 2 * kImageCardGap);
    const struct {
      const wchar_t* title;
      PhiAppearance value;
      const char* sketch;
    } kCards[] = {
        {L"System", PhiAppearance::kSystem, "system"},
        {L"Light", PhiAppearance::kLight, "light"},
        {L"Dark", PhiAppearance::kDark, "dark"},
    };
    int i = 0;
    for (const auto& definition : kCards) {
      Item card;
      card.kind = ItemKind::kImageCard;
      card.text = definition.title;
      card.payload = definition.sketch;
      card.selected = host_->Appearance() == definition.value;
      card.value = static_cast<int>(definition.value);
      card.action = "appearance";
      const int x = area.left + i * Scaled(kImageCardW + kImageCardGap);
      card.bounds = {x, area.top, x + Scaled(kImageCardW),
                     area.top + Scaled(kImageCardBlock)};
      items_.push_back(std::move(card));
      ++i;
    }
  }
  EndSection(y);

  BeginSection(y, L"Browsing");
  {
    // "New tab behavior": two picture cards. New Tab Page opens the native
    // new-tab page (phi_new_tab_page.cc, the Mac's NewTabViewController);
    // Omnibox focuses the address field and the tab is born on commit.
    const RECT area = AddRow(y, L"New tab behavior", kImageCardBlock,
                             2 * kImageCardW + kImageCardGap);
    const struct {
      const wchar_t* title;
      PhiNewTabBehavior value;
      const char* sketch;
      bool enabled;
    } kCards[] = {
        {L"New Tab Page", PhiNewTabBehavior::kNewTabPage, "ntp", true},
        {L"Omnibox", PhiNewTabBehavior::kOmnibox, "omnibox", true},
    };
    int i = 0;
    for (const auto& definition : kCards) {
      Item card;
      card.kind = ItemKind::kImageCard;
      card.text = definition.title;
      card.payload = definition.sketch;
      card.selected = host_->NewTabBehavior() == definition.value;
      card.enabled = definition.enabled;
      card.value = static_cast<int>(definition.value);
      card.action = "newtab";
      const int x = area.left + i * Scaled(kImageCardW + kImageCardGap);
      card.bounds = {x, area.top, x + Scaled(kImageCardW),
                     area.top + Scaled(kImageCardBlock)};
      items_.push_back(std::move(card));
      ++i;
    }
  }
  AddDividerRow(y);
  AddToggleRow(y, L"Always show full URL", host_->AlwaysShowFullUrl(),
               /*enabled=*/true, "fullurl");
  AddDividerRow(y);
  AddToggleRow(y, L"Show tab preview cards", host_->ShowTabPreviews(),
               /*enabled=*/true, "tabpreviews");
  AddDividerRow(y);
  {
    const RECT area = AddRow(y, L"Auto picture-in-picture", kPillHeight,
                             kPillWidth);
    AddSubHint(y, L"Pop out playing video when you switch tabs or apps");
    Item pill;
    pill.kind = ItemKind::kPopup;
    pill.text = L"Normal";
    pill.enabled = false;
    pill.bounds = area;
    items_.push_back(std::move(pill));
  }
  AddDividerRow(y);
  {
    AddToggleRow(y, L"Restore last session", host_->RestoreLastSession(),
                 /*enabled=*/true, "restoresession");
    AddSubHint(y, L"Reopen your windows and tabs the next time you open Phi.");
  }
  AddDividerRow(y);
  {
    // Chevron.right, 12pt semibold textSecondary on Mac — a kIcon glyph.
    const RECT area = AddRow(y, L"Additional browser settings",
                             kRowLabelHeight, 20);
    Item chevron;
    chevron.kind = ItemKind::kIcon;
    chevron.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
    // The whole row opens chrome://settings, as on Mac.
    Item row;
    row.kind = ItemKind::kListRow;
    row.text.clear();
    row.action = "browsersettings";
    row.bounds = {Scaled(kMargin), area.top - Scaled(4),
                  Scaled(kWidth - kMargin), area.bottom + Scaled(4)};
    items_.push_back(std::move(row));
  }
  EndSection(y);

  // Profile: with several profiles just the jump link; with one, the Mac
  // INLINES the profile detail (search engine, downloads, data links).
  if (host_->ProfileSummaries().size() > 1) {
    BeginSection(y, L"Profile");
    const RECT area = AddRow(y,
                             L"You have more than one profile, so each "
                             L"profile manages its own settings.",
                             kRowLabelHeight, 170);
    Item link;
    link.kind = ItemKind::kLink;
    link.text = L"Edit in Profiles settings…";
    link.action = "goprofiles";
    link.bounds = area;
    items_.push_back(std::move(link));
    EndSection(y);
  } else {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = L"Profile";
    heading.bounds = {Scaled(kMargin), *y, Scaled(kWidth - kMargin),
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    *y += Scaled(kSectionTitleHeight) + Scaled(kTitleToCard);
    const std::vector<std::string> profiles = host_->ProfileSummaries();
    const std::string profile_id =
        profiles.empty() ? std::string() : SplitTabs(profiles.front())[0];
    BuildProfileDetail(y, Scaled(kMargin), Scaled(kWidth - kMargin),
                       profile_id);
    *y += Scaled(kSectionGap) - Scaled(kMasterDetailGap);
  }

  BeginSection(y, L"Developer");
  AddToggleRow(y, L"Developer mode", host_->DeveloperMode(), /*enabled=*/true,
               "devmode");
  AddDividerRow(y);
  AddSubHint(y, host_->DeveloperMode()
                    ? L"Agent access, permissions, and the password manager "
                      L"live in the Developer tab."
                    : L"Turning developer mode off turns off agent access "
                      L"and the agent password manager. An agent that "
                      L"connects later can ask you to turn it back on.");
  *y += Scaled(kRowPadding);
  EndSection(y);
}

void PhiSettingsWindow::AddDividerRow(int* y) {
  Item divider;
  divider.kind = ItemKind::kDivider;
  divider.bounds = {Scaled(kMargin + kCardPadding), *y - Scaled(6),
                    Scaled(kWidth - kMargin - kCardPadding),
                    *y - Scaled(6) + 1};
  items_.push_back(std::move(divider));
  *y += Scaled(2);
}

void PhiSettingsWindow::AddSubHint(int* y, const wchar_t* text) {
  Item hint;
  hint.kind = ItemKind::kHint;
  hint.text = text;
  hint.bounds = {Scaled(kMargin + kCardPadding), *y - Scaled(8),
                 Scaled(kWidth - kMargin - kCardPadding),
                 *y - Scaled(8) + Scaled(kHintHeight)};
  items_.push_back(std::move(hint));
  *y += Scaled(kHintHeight);
}

void PhiSettingsWindow::AddSectionHeading(int* y, const wchar_t* title,
                                          const wchar_t* description) {
  Item heading;
  heading.kind = ItemKind::kSectionTitle;
  heading.text = title;
  heading.bounds = {Scaled(kMargin), *y, Scaled(kWidth - kMargin),
                    *y + Scaled(kSectionTitleHeight)};
  items_.push_back(std::move(heading));
  *y += Scaled(kSectionTitleHeight) + Scaled(4);
  Item subtitle;
  subtitle.kind = ItemKind::kSubtitle;
  subtitle.text = description;
  subtitle.bounds = {Scaled(kMargin), *y, Scaled(kWidth - kMargin),
                     *y + Scaled(kHintHeight)};
  items_.push_back(std::move(subtitle));
  // Leaves `y` where BeginSection(y, nullptr) expects it: the card follows at
  // the same title-to-card gap a titled section uses.
  *y += Scaled(kHintHeight) + Scaled(kTitleToCard) - Scaled(kRowPadding);
}

void PhiSettingsWindow::AddToggleRow(int* y, const wchar_t* title, bool on,
                                     bool enabled,
                                     const std::string& action) {
  const RECT area = AddRow(y, title, kToggleHeight + 4, kToggleWidth);
  Item toggle;
  toggle.kind = ItemKind::kToggle;
  toggle.on = on;
  toggle.enabled = enabled;
  toggle.action = action;
  toggle.bounds = area;
  items_.push_back(std::move(toggle));
}

void PhiSettingsWindow::BuildSpaces(int* y) {
  const std::vector<std::string> spaces = host_->SpaceSummaries();
  // Resolve (and default) the selection.
  bool found = false;
  for (const std::string& summary : spaces) {
    const std::vector<std::string> parts = SplitTabs(summary);
    if (parts.size() > 3 && parts[3] == selected_space_) {
      found = true;
      break;
    }
  }
  if (!found) {
    selected_space_.clear();
    if (!spaces.empty()) {
      const std::vector<std::string> parts = SplitTabs(spaces.front());
      if (parts.size() > 3) {
        selected_space_ = parts[3];
      }
    }
  }
  std::string selected_theme;
  int selected_saturation = -1;
  for (const std::string& summary : spaces) {
    const std::vector<std::string> parts = SplitTabs(summary);
    if (parts.size() > 3 && parts[3] == selected_space_) {
      selected_theme = parts.size() > 4 ? parts[4] : std::string();
      if (parts.size() > 5) {
        int parsed = -1;
        base::StringToInt(parts[5], &parsed);
        selected_saturation = parsed;
      }
      break;
    }
  }

  // The master-detail band: list 280 wide, detail beside, fixed height.
  const int band_top = *y;
  const int band_bottom = band_top + Scaled(kSpacesBlockHeight);
  const int list_left = Scaled(kMargin);
  const int list_right = list_left + Scaled(kSpacesListWidth);
  {
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {list_left, band_top, list_right, band_bottom};
    items_.push_back(std::move(card));
  }
  {
    Item header;
    header.kind = ItemKind::kSectionTitle;
    header.text = L"Your Spaces";
    header.bounds = {list_left + Scaled(kCardPadding), band_top + Scaled(10),
                     list_right - Scaled(kCardPadding),
                     band_top + Scaled(10) + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(header));
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {list_left, band_top + Scaled(kListHeaderHeight),
                      list_right, band_top + Scaled(kListHeaderHeight) + 1};
    items_.push_back(std::move(divider));
  }
  // The trailing profile is shown by NAME (the Mac row hosts a profile
  // picker; bindings are immutable here, so it reads as a static menu).
  std::vector<std::pair<std::string, std::wstring>> profile_names;
  for (const std::string& summary : host_->ProfileSummaries()) {
    const std::vector<std::string> parts = SplitTabs(summary);
    profile_names.emplace_back(
        parts[0],
        base::UTF8ToWide(parts.size() > 1 ? parts[1] : parts[0]));
  }
  int row_y = band_top + Scaled(kListHeaderHeight) + Scaled(6);
  const int rows_bottom = band_bottom - Scaled(kToolStripHeight) - Scaled(6);
  for (const std::string& summary : spaces) {
    if (row_y + Scaled(kListRowHeight) > rows_bottom) {
      break;  // The Mac scrolls the inner list; ours clips for now.
    }
    const std::vector<std::string> parts = SplitTabs(summary);
    const std::string id = parts.size() > 3 ? parts[3] : std::string();
    Item row;
    row.kind = ItemKind::kListRow;
    row.text = base::UTF8ToWide(parts[0]);
    if (parts.size() > 1) {
      // The Space's icon, drawn by the paint pass as the Mac catalog's
      // artwork or a colour emoji (it used to be pasted into the text,
      // which dropped catalog icons and greyed emoji out).
      row.icon = base::UTF8ToWide(parts[1]);
    }
    row.detail = L"";
    if (parts.size() > 2) {
      for (const auto& profile : profile_names) {
        if (profile.first == parts[2]) {
          row.detail = profile.second;
          break;
        }
      }
    }
    row.selected = id == selected_space_;
    // The default ROLE's holder wears the badge, as on Mac (it moves when
    // the default Space is deleted).
    row.on = host_ && id == host_->DefaultSpaceId();
    row.value = 1;       // Solid-accent selection (the Spaces list style).
    row.action = "selectspace";
    row.payload = id;
    row.bounds = {list_left + Scaled(6), row_y, list_right - Scaled(6),
                  row_y + Scaled(kListRowHeight)};
    items_.push_back(std::move(row));
    row_y += Scaled(kListRowHeight);
  }
  {
    // The 34pt tool strip: new / delete / rename (the latter two wait for
    // their surfaces, exactly like the sidebar menu's entries).
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {list_left, band_bottom - Scaled(kToolStripHeight),
                      list_right, band_bottom - Scaled(kToolStripHeight) + 1};
    items_.push_back(std::move(divider));
    const struct {
      wchar_t glyph;
      const char* action;
      bool enabled;
    } kTools[] = {
        {0xE710, "newspace", true},  // Add
        // Only the LAST Space is permanent (the Mac's last-Space rule).
        {0xE738, "delspace",
         !selected_space_.empty() && host_ &&
             host_->CanDeleteSpace(selected_space_)},  // Remove
        {0xE70F, "renspace", !selected_space_.empty()},        // Edit
    };
    int x = list_left;
    for (const auto& tool : kTools) {
      Item button;
      button.kind = ItemKind::kToolButton;
      button.text = std::wstring(1, tool.glyph);
      button.enabled = tool.enabled;
      button.action = tool.action;
      button.bounds = {x, band_bottom - Scaled(kToolStripHeight),
                       x + Scaled(kToolStripHeight), band_bottom};
      items_.push_back(std::move(button));
      x += Scaled(kToolStripHeight);
      // The Mac's 1x20 separatorColor rule between the buttons.
      Item rule;
      rule.kind = ItemKind::kDivider;
      rule.bounds = {x, band_bottom - Scaled(kToolStripHeight) + Scaled(7),
                     x + 1, band_bottom - Scaled(7)};
      items_.push_back(std::move(rule));
    }
  }

  // Detail column: icon picker card, then colour + saturation card.
  const int detail_left = list_right + Scaled(kMasterDetailGap);
  const int detail_right = Scaled(kWidth - kMargin);
  if (!selected_space_.empty()) {
    // Card 1: the icon picker, absorbing the band's spare height (the Mac
    // gives the picker `maxHeight: .infinity` above the fixed theme card).
    const int theme_card_h = Scaled(110);
    const int picker_bottom =
        band_bottom - Scaled(kMasterDetailGap) - theme_card_h;
    {
      Item card;
      card.kind = ItemKind::kCard;
      card.bounds = {detail_left, band_top, detail_right, picker_bottom};
      items_.push_back(std::move(card));
    }
    // The Mac picker's segmented control (128x26, centered): the Phi icon
    // catalog, then the emoji library. The grid below scrolls in place
    // under the wheel, as the Mac's does.
    {
      const struct {
        const wchar_t* label;
        const char* tab;
      } kTabs[] = {{L"Icon", "icon"}, {L"Emoji", "emoji"}};
      int seg_x = (detail_left + detail_right) / 2 - Scaled(64);
      for (const auto& tab : kTabs) {
        Item segment;
        segment.kind = ItemKind::kButton;
        segment.text = tab.label;
        segment.selected = picker_tab_ == tab.tab;
        segment.action = "pickertab";
        segment.payload = tab.tab;
        segment.bounds = {seg_x, band_top + Scaled(8), seg_x + Scaled(64),
                          band_top + Scaled(8) + Scaled(26)};
        items_.push_back(std::move(segment));
        seg_x += Scaled(64);
      }
    }
    // One flat cell list: the catalog, or every emoji group in Mac order.
    std::vector<std::pair<std::wstring, std::string>> cells;
    if (picker_tab_ == "icon") {
      for (const PhiCatalogIcon& icon : PhiIconCatalogAll()) {
        cells.emplace_back(std::wstring(1, icon.glyph),
                           std::string("phi:") + icon.id);
      }
    } else {
      for (const PhiEmojiGroup& group : PhiEmojiGroups()) {
        for (const PhiEmoji& emoji : group.items) {
          cells.emplace_back(base::UTF8ToWide(emoji.text),
                             "emoji:" + emoji.id);
        }
      }
    }
    // The Space's current icon, so its cell reads selected as on Mac.
    std::wstring current_icon;
    for (const std::string& summary : spaces) {
      const std::vector<std::string> parts = SplitTabs(summary);
      if (parts.size() > 3 && parts[3] == selected_space_) {
        current_icon = base::UTF8ToWide(parts[1]);
        break;
      }
    }
    // The grid scrolls a whole cell row at a time (three per wheel notch,
    // like the create-space form's grid), so no row ever straddles the
    // viewport's edge and nothing needs clipping. `picker_scroll_` is the
    // first visible row, clamped here against the content.
    const int grid_left = detail_left + Scaled(10);
    const int grid_top = band_top + Scaled(34);
    const int grid_bottom = picker_bottom - Scaled(8);
    const int pitch = Scaled(kEmojiCell + kEmojiGap);
    const int per_row = std::max(
        1, (detail_right - Scaled(10) - grid_left + Scaled(kEmojiGap)) /
               pitch);
    const int rows_fit = std::max(1, (grid_bottom - grid_top) / pitch);
    const int lines =
        (static_cast<int>(cells.size()) + per_row - 1) / per_row;
    picker_max_line_ = std::max(0, lines - rows_fit);
    picker_scroll_ = std::max(0, std::min(picker_scroll_, picker_max_line_));
    picker_grid_ = {detail_left, grid_top, detail_right, grid_bottom};
    const size_t first = static_cast<size_t>(picker_scroll_) *
                         static_cast<size_t>(per_row);
    const size_t per_page = static_cast<size_t>(per_row * rows_fit);
    for (size_t i = first; i < cells.size() && i < first + per_page; ++i) {
      const int index = static_cast<int>(i - first);
      const int col = index % per_row;
      const int line = index / per_row;
      Item cell;
      cell.kind = ItemKind::kEmojiCell;
      cell.text = cells[i].first;
      cell.selected = !current_icon.empty() && cells[i].first == current_icon;
      cell.action = "spaceicon";
      cell.payload = selected_space_ + "\t" + cells[i].second;
      const int x = grid_left + col * pitch;
      const int top = grid_top + line * pitch;
      cell.bounds = {x, top, x + Scaled(kEmojiCell),
                     top + Scaled(kEmojiCell)};
      items_.push_back(std::move(cell));
    }

    // Card 2: the Mac's compact theme card - "Color" with 16pt dots in
    // 20pt frames (spacing 10), a divider, then the 200pt Saturation
    // slider, applied to THIS Space. There is no "follow the app" dot on
    // Mac; un-pinning stays in the sidebar's Edit Theme menu.
    const int theme_top = picker_bottom + Scaled(kMasterDetailGap);
    {
      Item card;
      card.kind = ItemKind::kCard;
      card.bounds = {detail_left, theme_top, detail_right,
                     theme_top + theme_card_h};
      items_.push_back(std::move(card));
    }
    {
      Item label;
      label.kind = ItemKind::kRowLabel;
      label.text = L"Color";
      label.bounds = {detail_left + Scaled(kCardPadding),
                      theme_top + Scaled(kRowPadding),
                      detail_left + Scaled(kCardPadding) + Scaled(46),
                      theme_top + Scaled(kRowPadding) + Scaled(20)};
      items_.push_back(std::move(label));
      const auto& themes = PhiBuiltInThemes();
      int x = detail_left + Scaled(kCardPadding) + Scaled(46) + Scaled(12);
      for (size_t i = 0; i < themes.size(); ++i) {
        Item swatch;
        swatch.kind = ItemKind::kSwatch;
        swatch.text = base::UTF8ToWide(themes[i].name);
        swatch.selected = selected_theme == themes[i].id;
        const PhiPalette resolved = PhiResolvePalette(
            themes[i], host_->Appearance(), host_->Saturation());
        swatch.swatch = i == 0 ? RGB(0xFF, 0xFF, 0xFF) : resolved.accent;
        swatch.ring = resolved.accent;
        swatch.value = kSwatchDotSpaces;  // Compact 16pt dot.
        swatch.action = "spacetheme";
        swatch.payload = selected_space_ + "\t" + themes[i].id;
        swatch.bounds = {x, theme_top + Scaled(kRowPadding),
                         x + Scaled(kSwatchItemSpaces),
                         theme_top + Scaled(kRowPadding) +
                             Scaled(kSwatchBlockSpaces)};
        items_.push_back(std::move(swatch));
        x += Scaled(kSwatchItemSpaces + kSwatchGapSpaces);
      }
    }
    const int color_bottom = theme_top + Scaled(kRowPadding) +
                             Scaled(kSwatchBlockSpaces) + Scaled(kRowPadding);
    {
      Item divider;
      divider.kind = ItemKind::kDivider;
      divider.bounds = {detail_left + Scaled(kCardPadding), color_bottom,
                        detail_right - Scaled(kCardPadding),
                        color_bottom + 1};
      items_.push_back(std::move(divider));
    }
    {
      const int slider_top = color_bottom + 1 + Scaled(kRowPadding);
      Item label;
      label.kind = ItemKind::kRowLabel;
      label.text = L"Saturation";
      label.bounds = {detail_left + Scaled(kCardPadding), slider_top,
                      detail_left + Scaled(90),
                      slider_top + Scaled(kSliderHeight)};
      items_.push_back(std::move(label));
      Item slider;
      slider.kind = ItemKind::kSliderTrack;
      slider.value = selected_saturation < 0 ? host_->Saturation()
                                             : selected_saturation;
      slider.action = "spacesat";
      slider.payload = selected_space_;
      // The track ramps THIS Space's effective theme.
      const std::string effective =
          selected_theme.empty() ? host_->ThemeId() : selected_theme;
      const PhiTheme& theme = PhiThemeById(effective);
      slider.swatch = host_->Palette().dark ? theme.dark.overlay
                                            : theme.light.overlay;
      slider.on = effective == "pure";
      slider.bounds = {detail_right - Scaled(kCardPadding) -
                           Scaled(kSliderSpaces),
                       slider_top,
                       detail_right - Scaled(kCardPadding),
                       slider_top + Scaled(kSliderHeight)};
      items_.push_back(std::move(slider));
    }
  } else {
    Item empty;
    empty.kind = ItemKind::kHint;
    empty.text = L"Select a Space to view its settings.";
    empty.bounds = {detail_left, band_top + Scaled(200), detail_right,
                    band_top + Scaled(200) + Scaled(kHintHeight)};
    items_.push_back(std::move(empty));
  }
  *y = band_bottom + Scaled(kMasterDetailGap);

  // Pane-wide card: pinned tab scope, with the live description.
  BeginSection(y, nullptr);
  {
    const RECT area = AddRow(y, L"Pinned tab scope", kPillHeight, kPillWidth);
    const wchar_t* description = L"";
    switch (host_->PinScope()) {
      case PhiPinScope::kSpace:
        description = L"Each Space has its own pinned tabs.";
        break;
      case PhiPinScope::kProfile:
        description = L"Spaces using the same profile share pinned tabs.";
        break;
      case PhiPinScope::kApp:
        description = L"Pinned tabs are shared across all profiles and "
                      L"Spaces.";
        break;
    }
    AddSubHint(y, description);
    Item pill;
    pill.kind = ItemKind::kPopup;
    pill.text = host_->PinScope() == PhiPinScope::kSpace     ? L"Space"
                : host_->PinScope() == PhiPinScope::kProfile ? L"Profile"
                                                             : L"App";
    pill.action = "pinscope";
    pill.bounds = area;
    items_.push_back(std::move(pill));
  }
  EndSection(y);
}

// The Mac's Navigations pane (NavigationsSettingsView): the link-opening
// controls, collected out of General and Spaces. Peek and URL Rules are real.
//
// The two KIOSK switches are drawn for the shape of the pane but sit INERT,
// because neither gesture reaches Windows: the Cmd+Opt link remap is
// IS_MAC_PHI in Browser::OpenURLFromTab, and the client has no external-link
// entry point at all. They become live rows once the framework takes a client
// answer for them, the way the Peek pair now does.
void PhiSettingsWindow::BuildNavigation(int* y) {
  AddSectionHeading(y, L"Kiosk",
                    L"Open links in a lightweight window, one page at a "
                    L"time.");
  BeginSection(y, nullptr);
  AddToggleRow(y, L"Open external links in Kiosk", /*on=*/false,
               /*enabled=*/false, std::string());
  AddSubHint(y, L"Links opened from other apps use a focused, single-page "
                L"Kiosk window.");
  AddDividerRow(y);
  AddToggleRow(y, L"Open Kiosk when clicking links with Ctrl+Alt held",
               /*on=*/true, /*enabled=*/false, std::string());
  AddSubHint(y, L"Hold Ctrl and Alt while clicking a link to open it in a "
                L"Kiosk window.");
  AddDividerRow(y);
  AddSubHint(y, L"On Windows, Kiosk opens from a link's context menu and "
                L"from URL rules.");
  *y += Scaled(kRowPadding);
  EndSection(y);

  AddSectionHeading(y, L"Peek",
                    L"Preview a link without opening it in a new tab.");
  const bool peek = host_->PeekViewEnabled();
  BeginSection(y, nullptr);
  AddToggleRow(y, L"Enable Peek View", peek, /*enabled=*/true, "peekview");
  AddSubHint(y, L"Choose \u201COpen Link in Peek View\u201D to preview a "
                L"link in a floating panel over the page.");
  AddDividerRow(y);
  // The sub-switch is dimmed and inert while the master is off, as on Mac.
  AddToggleRow(y, L"Automatically peek from pinned tabs and bookmarks",
               host_->AutoPeekViewEnabled(), /*enabled=*/peek, "autopeek");
  AddSubHint(y, L"Links that leave a pinned tab\u2019s or bookmark\u2019s "
                L"site open in a Peek panel instead of a new tab.");
  EndSection(y);

  BeginSection(y, L"URL Rules");
  {
    const RECT area = AddRow(y, L"Manage URL Rules…", kRowLabelHeight, 20);
    Item chevron;
    chevron.kind = ItemKind::kLink;
    chevron.text = L"›";
    chevron.action = "urlrules";
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
    // The whole row opens the editor, not just the trailing chevron.
    Item& label = items_[items_.size() - 2];
    if (label.kind == ItemKind::kRowLabel) {
      label.action = "urlrules";
    }
    AddSubHint(y, L"Route matching domains or URLs to a Space, or ask every "
                  L"time.");
  }
  EndSection(y);
}

namespace {

// The Mac editor's match modes, derived from / encoded into the stored
// (host, pathPrefix) wire pair.
enum class RuleMatchType { kDomainSuffix, kDomain, kDomainContains, kUrl };

RuleMatchType RuleTypeOf(const std::string& host,
                         const std::string& path_prefix) {
  if (!path_prefix.empty()) {
    return RuleMatchType::kUrl;
  }
  if (host.size() > 2 && host.front() == '*' && host.back() == '*') {
    return RuleMatchType::kDomainContains;
  }
  if (host.rfind("*.", 0) == 0) {
    return RuleMatchType::kDomainSuffix;
  }
  return RuleMatchType::kDomain;
}

const wchar_t* RuleTypeLabel(RuleMatchType type) {
  switch (type) {
    case RuleMatchType::kDomainSuffix:
      return L"Domain suffix";
    case RuleMatchType::kDomain:
      return L"Domain";
    case RuleMatchType::kDomainContains:
      return L"Domain contains";
    case RuleMatchType::kUrl:
      return L"URL";
  }
  return L"";
}

// The user-facing value: the stored pair minus the wire sigils.
std::string RuleDisplayValue(const std::string& host,
                             const std::string& path_prefix) {
  switch (RuleTypeOf(host, path_prefix)) {
    case RuleMatchType::kDomainSuffix:
      return host.substr(2);
    case RuleMatchType::kDomainContains:
      return host.substr(1, host.size() - 2);
    case RuleMatchType::kUrl:
      return host + path_prefix;
    case RuleMatchType::kDomain:
      break;
  }
  return host;
}

// Simplified Mac normalizedPathPrefix: trim, collapse trailing slashes,
// treat a bare "/" as "any path", ensure the leading "/". (The Mac also
// re-percent-encodes; typed ASCII prefixes — the practical case — match
// GURL::path() without it.)
std::string NormalizePathPrefix(std::string prefix) {
  while (!prefix.empty() &&
         (prefix.back() == ' ' || prefix.back() == '\t')) {
    prefix.pop_back();
  }
  while (!prefix.empty() && prefix.front() == ' ') {
    prefix.erase(prefix.begin());
  }
  while (prefix.size() > 1 && prefix.back() == '/') {
    prefix.pop_back();
  }
  if (prefix == "/" || prefix.empty()) {
    return std::string();
  }
  if (prefix.front() != '/') {
    prefix.insert(prefix.begin(), '/');
  }
  return prefix;
}

// Drops a pasted ":8080" — GURL hosts never carry one. Only cuts when
// everything after the last ':' is digits, leaving IPv6 "[::1]" alone.
std::string StripPort(const std::string& host) {
  const size_t colon = host.rfind(':');
  if (colon == std::string::npos || colon + 1 >= host.size()) {
    return host;
  }
  for (size_t i = colon + 1; i < host.size(); ++i) {
    if (host[i] < '0' || host[i] > '9') {
      return host;
    }
  }
  return host.substr(0, colon);
}

// The Mac editor's encode(): translate (type, typed value) into the stored
// (host, pathPrefix) pair. The type is the single source of truth — typed
// sigils are stripped and the type's own form re-added. An input that
// reduces to nothing yields an empty host, which the caller drops.
void RuleEncode(RuleMatchType type,
                std::string value,
                std::string* host,
                std::string* path_prefix) {
  host->clear();
  path_prefix->clear();
  const size_t scheme = value.find("://");
  if (scheme != std::string::npos) {
    value = value.substr(scheme + 3);
  }
  if (type == RuleMatchType::kUrl) {
    std::string host_part = value;
    std::string path_part;
    const size_t slash = value.find('/');
    if (slash != std::string::npos) {
      host_part = value.substr(0, slash);
      path_part = value.substr(slash);
    }
    if (host_part.rfind("*.", 0) == 0) {
      host_part = host_part.substr(2);
    }
    *host = StripPort(host_part);
    *path_prefix = NormalizePathPrefix(path_part);
    return;
  }
  const size_t slash = value.find('/');
  if (slash != std::string::npos) {
    value = value.substr(0, slash);
  }
  value = StripPort(value);
  switch (type) {
    case RuleMatchType::kDomainSuffix: {
      const std::string bare =
          value.rfind("*.", 0) == 0 ? value.substr(2) : value;
      *host = bare.empty() ? std::string() : "*." + bare;
      break;
    }
    case RuleMatchType::kDomain:
      *host = value.rfind("*.", 0) == 0 ? value.substr(2) : value;
      break;
    case RuleMatchType::kDomainContains: {
      std::string needle = value;
      while (!needle.empty() && needle.front() == '*') {
        needle.erase(needle.begin());
      }
      while (!needle.empty() && needle.back() == '*') {
        needle.pop_back();
      }
      *host = needle.empty() ? std::string() : "*" + needle + "*";
      break;
    }
    case RuleMatchType::kUrl:
      break;  // Handled above.
  }
}

// Derives the match type a typed pattern implies: an explicit path means
// URL, explicit sigils keep their form, and a bare host defaults to the
// Mac's domain-suffix (host + subdomains).
RuleMatchType RuleTypeForInput(const std::string& value) {
  std::string work = value;
  const size_t scheme = work.find("://");
  if (scheme != std::string::npos) {
    work = work.substr(scheme + 3);
  }
  const size_t slash = work.find('/');
  if (slash != std::string::npos && slash + 1 < work.size()) {
    return RuleMatchType::kUrl;
  }
  if (slash != std::string::npos) {
    work = work.substr(0, slash);
  }
  if (work.size() > 2 && work.front() == '*' && work.back() == '*') {
    return RuleMatchType::kDomainContains;
  }
  if (work.rfind("*.", 0) == 0) {
    return RuleMatchType::kDomainSuffix;
  }
  return RuleMatchType::kDomainSuffix;  // The Mac's default for a bare host.
}

}  // namespace

void PhiSettingsWindow::BuildUrlRules(int* y) {
  // Back link to the Navigation pane the editor drills out of.
  {
    Item back;
    back.kind = ItemKind::kLink;
    back.text = L"\u2039 Navigation";
    back.action = "gonavigation";
    back.bounds = {Scaled(kMargin), *y, Scaled(kMargin) + Scaled(110),
                   *y + Scaled(kRowLabelHeight)};
    items_.push_back(std::move(back));
    *y += Scaled(kRowLabelHeight) + Scaled(8);
  }
  AddHint(y, L"URLs matching a rule always open in its Space, no matter "
             L"where you click or type them.");
  *y += Scaled(4);

  const std::vector<std::string> rules = host_->URLRuleRows();
  // Space names for the target pills.
  std::vector<std::pair<std::string, std::wstring>> space_names;
  for (const std::string& summary : host_->SpaceSummaries()) {
    const std::vector<std::string> parts = SplitTabs(summary);
    if (parts.size() > 3) {
      space_names.emplace_back(parts[3], base::UTF8ToWide(parts[0]));
    }
  }

  BeginSection(y, nullptr);
  if (rules.empty()) {
    Item hint;
    hint.kind = ItemKind::kHint;
    hint.text = L"No rules yet.";
    hint.bounds = {Scaled(kMargin + kCardPadding), *y,
                   Scaled(kWidth - kMargin - kCardPadding),
                   *y + Scaled(kHintHeight)};
    items_.push_back(std::move(hint));
    *y += Scaled(kHintHeight) + Scaled(kRowPadding);
  }
  for (size_t i = 0; i < rules.size(); ++i) {
    if (i > 0) {
      AddDividerRow(y);
    }
    const std::vector<std::string> parts = SplitTabs(rules[i]);
    const std::string space_id = parts.size() > 0 ? parts[0] : std::string();
    const std::string host = parts.size() > 1 ? parts[1] : std::string();
    const std::string prefix = parts.size() > 2 ? parts[2] : std::string();
    const bool ask = parts.size() > 3 && parts[3] == "1";
    const RuleMatchType type = RuleTypeOf(host, prefix);
    const std::string index_payload = base::NumberToString(i);

    const int row_height = Scaled(kPillHeight);
    const int right = Scaled(kWidth - kMargin - kCardPadding);
    // Trailing to leading: delete glyph, target pill, type pill; the
    // pattern text takes the rest.
    RECT del = {right - Scaled(24), *y, right, *y + row_height};
    RECT target = {del.left - Scaled(8) - Scaled(120), *y, del.left -
                   Scaled(8), *y + row_height};
    RECT type_rect = {target.left - Scaled(8) - Scaled(110), *y,
                      target.left - Scaled(8), *y + row_height};

    Item pattern;
    pattern.kind = ItemKind::kRowLabel;
    pattern.text = base::UTF8ToWide(RuleDisplayValue(host, prefix));
    pattern.action = "editrule";
    pattern.payload = index_payload;
    pattern.bounds = {Scaled(kMargin + kCardPadding), *y,
                      type_rect.left - Scaled(12), *y + row_height};
    items_.push_back(std::move(pattern));

    Item type_pill;
    type_pill.kind = ItemKind::kPopup;
    type_pill.text = RuleTypeLabel(type);
    type_pill.action = "ruletype";
    type_pill.payload = index_payload;
    type_pill.bounds = type_rect;
    items_.push_back(std::move(type_pill));

    Item target_pill;
    target_pill.kind = ItemKind::kPopup;
    if (ask) {
      target_pill.text = L"Ask every time";
    } else {
      target_pill.text = L"?";
      for (const auto& [id, name] : space_names) {
        if (id == space_id) {
          target_pill.text = name;
          break;
        }
      }
    }
    target_pill.action = "ruletarget";
    target_pill.payload = index_payload;
    target_pill.bounds = target;
    items_.push_back(std::move(target_pill));

    Item remove;
    remove.kind = ItemKind::kToolButton;
    remove.text = L"\uE74D";  // MDL2 Delete.
    remove.action = "delrule";
    remove.payload = index_payload;
    remove.bounds = del;
    items_.push_back(std::move(remove));

    *y += row_height + Scaled(kRowPadding);
  }
  EndSection(y);

  {
    Item add;
    add.kind = ItemKind::kButton;
    add.text = L"Add Rule";
    add.action = "addrule";
    add.bounds = {Scaled(kMargin), *y, Scaled(kMargin) + Scaled(96),
                  *y + Scaled(26)};
    items_.push_back(std::move(add));
    *y += Scaled(26);
  }
}

void PhiSettingsWindow::BuildProfiles(int* y) {
  const std::vector<std::string> profiles = host_->ProfileSummaries();
  const std::vector<std::string> spaces = host_->SpaceSummaries();
  bool found = false;
  for (const std::string& summary : profiles) {
    const std::vector<std::string> parts = SplitTabs(summary);
    if (!parts.empty() && parts[0] == selected_profile_) {
      found = true;
      break;
    }
  }
  if (!found) {
    selected_profile_.clear();
    if (!profiles.empty()) {
      selected_profile_ = SplitTabs(profiles.front())[0];
    }
  }

  const int band_top = *y;
  const int band_bottom = band_top + Scaled(kSpacesBlockHeight);
  const int list_left = Scaled(kMargin);
  const int list_right = list_left + Scaled(kProfilesListWidth);
  {
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {list_left, band_top, list_right, band_bottom};
    items_.push_back(std::move(card));
    Item header;
    header.kind = ItemKind::kSectionTitle;
    header.text = L"Your Profiles";
    header.bounds = {list_left + Scaled(kCardPadding), band_top + Scaled(10),
                     list_right - Scaled(kCardPadding),
                     band_top + Scaled(10) + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(header));
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {list_left, band_top + Scaled(kListHeaderHeight),
                      list_right, band_top + Scaled(kListHeaderHeight) + 1};
    items_.push_back(std::move(divider));
  }
  int row_y = band_top + Scaled(kListHeaderHeight) + Scaled(6);
  const int rows_bottom = band_bottom - Scaled(kToolStripHeight) - Scaled(6);
  for (const std::string& summary : profiles) {
    if (row_y + Scaled(kListRowHeight) > rows_bottom) {
      break;
    }
    const std::vector<std::string> parts = SplitTabs(summary);
    const std::string& id = parts[0];
    int bound = 0;
    for (const std::string& space : spaces) {
      const std::vector<std::string> space_parts = SplitTabs(space);
      if (space_parts.size() > 2 && space_parts[2] == id) {
        ++bound;
      }
    }
    Item row;
    row.kind = ItemKind::kListRow;
    row.text = base::UTF8ToWide(parts.size() > 1 ? parts[1] : id);
    row.detail =
        bound == 0
            ? L"Not used"
            : base::UTF8ToWide(base::NumberToString(bound) + " Spaces");
    row.selected = id == selected_profile_;
    row.on = id == "Default";  // Wears the badge.
    row.value = 2;  // 15% accent wash selection (the Profiles list style).
    row.action = "selectprofile";
    row.payload = id;
    row.bounds = {list_left + Scaled(6), row_y, list_right - Scaled(6),
                  row_y + Scaled(kListRowHeight)};
    items_.push_back(std::move(row));
    row_y += Scaled(kListRowHeight);
  }
  {
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {list_left, band_bottom - Scaled(kToolStripHeight),
                      list_right, band_bottom - Scaled(kToolStripHeight) + 1};
    items_.push_back(std::move(divider));
    const struct {
      wchar_t glyph;
      const char* action;
      bool enabled;
    } kTools[] = {
        {0xE710, "addprofile", true},  // Add
        // Default is permanent; a profile still bound to a Space stays.
        {0xE738, "delprofile", [&] {
           if (selected_profile_.empty() || selected_profile_ == "Default") {
             return false;
           }
           for (const std::string& space : spaces) {
             const std::vector<std::string> parts = SplitTabs(space);
             if (parts.size() > 2 && parts[2] == selected_profile_) {
               return false;
             }
           }
           return true;
         }()},
        {0xE70F, "renprofile", !selected_profile_.empty()},  // Edit
    };
    int x = list_left;
    for (const auto& tool : kTools) {
      Item button;
      button.kind = ItemKind::kToolButton;
      button.text = std::wstring(1, tool.glyph);
      button.enabled = tool.enabled;
      button.action = tool.action;
      button.bounds = {x, band_bottom - Scaled(kToolStripHeight),
                       x + Scaled(kToolStripHeight), band_bottom};
      items_.push_back(std::move(button));
      x += Scaled(kToolStripHeight);
      Item rule;
      rule.kind = ItemKind::kDivider;
      rule.bounds = {x, band_bottom - Scaled(kToolStripHeight) + Scaled(7),
                     x + 1, band_bottom - Scaled(7)};
      items_.push_back(std::move(rule));
    }
  }

  // Detail column: the Mac's ProfileDetailSettingsView, shared with the
  // General pane's single-profile case.
  const int detail_left = list_right + Scaled(kMasterDetailGap);
  const int detail_right = Scaled(kWidth - kMargin);
  if (!selected_profile_.empty()) {
    int detail_y = band_top;
    BuildProfileDetail(&detail_y, detail_left, detail_right,
                       selected_profile_);
  } else {
    Item empty;
    empty.kind = ItemKind::kHint;
    empty.text = L"Select a profile to view its settings.";
    empty.bounds = {detail_left, band_top + Scaled(200), detail_right,
                    band_top + Scaled(200) + Scaled(kHintHeight)};
    items_.push_back(std::move(empty));
  }
  *y = band_bottom + Scaled(kSectionGap);
}

void PhiSettingsWindow::BuildProfileDetail(int* y, int left, int right,
                                           const std::string& profile_id) {
  // A SettingsDetailRow: MDL2 glyph in a 24pt gutter, 13pt label, trailing
  // control — the Mac's exact anatomy (icon 16pt textSecondary, 12pt gaps,
  // 12pt vertical padding).
  const int row_h = Scaled(48);
  const auto detail_row = [&](int top, wchar_t glyph, const wchar_t* title,
                              bool enabled) {
    Item icon;
    icon.kind = ItemKind::kIcon;
    icon.text = std::wstring(1, glyph);
    icon.enabled = enabled;
    icon.bounds = {left + Scaled(kCardPadding), top,
                   left + Scaled(kCardPadding) + Scaled(24), top + row_h};
    items_.push_back(std::move(icon));
    Item label;
    label.kind = ItemKind::kRowLabel;
    label.text = title;
    label.enabled = enabled;
    label.bounds = {left + Scaled(kCardPadding) + Scaled(24) + Scaled(12),
                    top, right - Scaled(kCardPadding + 150), top + row_h};
    items_.push_back(std::move(label));
  };
  const auto trailing_pill = [&](int top, const std::wstring& text,
                                 bool enabled, const char* action) {
    Item pill;
    pill.kind = ItemKind::kPopup;
    pill.text = text;
    pill.enabled = enabled;
    if (action) {
      pill.action = action;
      pill.payload = profile_id;
    }
    pill.bounds = {right - Scaled(kCardPadding) - Scaled(kPillWidth),
                   top + (row_h - Scaled(kPillHeight)) / 2,
                   right - Scaled(kCardPadding),
                   top + (row_h + Scaled(kPillHeight)) / 2};
    items_.push_back(std::move(pill));
  };

  // Card 1: Search Engine / Download Location — live, per profile. Both
  // read empty while the profile is not loaded, and the rows gray out.
  {
    const std::vector<std::string> engines = host_->SearchEngines(profile_id);
    std::wstring engine_name = L"Unavailable";
    for (const std::string& engine : engines) {
      const std::vector<std::string> parts = SplitTabs(engine);
      if (parts.size() > 2 && parts[2] == "1") {
        engine_name = base::UTF8ToWide(parts[1]);
        break;
      }
    }
    const std::string download_dir = host_->DownloadDirectory(profile_id);
    // A selected profile loads on demand (the framework kicks the profile
    // and its keyword service). While either read is still pending, poll:
    // one short timer per rebuild, re-armed only while data is missing, so
    // the pane fills in by itself once the loads land.
    if ((engines.empty() || download_dir.empty()) && hwnd_) {
      ::SetTimer(hwnd_, kProfilePollTimer, 700, nullptr);
    }
    // The Mac pill shows only the folder's last path component.
    std::wstring download_leaf = L"Choose…";
    if (!download_dir.empty()) {
      const size_t cut = download_dir.find_last_of("\\\\/");
      download_leaf = base::UTF8ToWide(
          cut == std::string::npos ? download_dir
                                   : download_dir.substr(cut + 1));
    }

    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {left, *y, right, *y + 2 * row_h};
    items_.push_back(std::move(card));
    detail_row(*y, 0xE721, L"Search Engine", !engines.empty());
    trailing_pill(*y, engine_name, !engines.empty(), "searchengine");
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {left + Scaled(kCardPadding), *y + row_h,
                      right - Scaled(kCardPadding), *y + row_h + 1};
    items_.push_back(std::move(divider));
    detail_row(*y + row_h, 0xE896, L"Download Location",
               !download_dir.empty());
    trailing_pill(*y + row_h, download_leaf, !download_dir.empty(),
                  "downloadfolder");
    *y += 2 * row_h + Scaled(kMasterDetailGap);
  }

  // "Your Data and Settings": three live links into chrome://settings for
  // this profile's data pages, each with the Mac's accent arrow.
  {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = L"Your Data and Settings";
    heading.bounds = {left + Scaled(2), *y, right,
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    *y += Scaled(kSectionTitleHeight) + Scaled(8);
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {left, *y, right, *y + 3 * row_h};
    items_.push_back(std::move(card));
    const struct {
      wchar_t glyph;
      const wchar_t* title;
      const char* url;
    } kRows[] = {
        {0xE72E, L"Privacy and Security", "chrome://settings/privacy"},
        {0xE7ED, L"Notifications",
         "chrome://settings/content/notifications"},
        {0xE74D, L"Clear Browsing Data",
         "chrome://settings/clearBrowserData"},
    };
    int i = 0;
    for (const auto& def : kRows) {
      const int top = *y + i * row_h;
      detail_row(top, def.glyph, def.title, true);
      Item arrow;
      arrow.kind = ItemKind::kIcon;
      arrow.text = std::wstring(1, wchar_t(0xE72A));  // Forward arrow
      arrow.selected = true;  // kIcon: selected = accent ink (the Mac's).
      arrow.bounds = {right - Scaled(kCardPadding + 24), top,
                      right - Scaled(kCardPadding), top + row_h};
      items_.push_back(std::move(arrow));
      // The whole row is the button, as on Mac.
      Item row;
      row.kind = ItemKind::kListRow;
      row.action = "openpage";
      row.payload = def.url;
      row.bounds = {left + Scaled(4), top, right - Scaled(4), top + row_h};
      items_.push_back(std::move(row));
      if (i < 2) {
        Item divider;
        divider.kind = ItemKind::kDivider;
        divider.bounds = {left + Scaled(kCardPadding), top + row_h,
                          right - Scaled(kCardPadding), top + row_h + 1};
        items_.push_back(std::move(divider));
      }
      ++i;
    }
    *y += 3 * row_h;
  }
}

void PhiSettingsWindow::BuildAccount(int* y) {
  // The Mac pane's frame: a profile card on the LEFT (264 wide at x=24),
  // the account card and the Default Browser section stacked on the right.
  // Sign-in does not exist on this build, so the account side is inert;
  // the default-browser button IS live (Windows' default-apps page).
  const int top = *y;
  const int left = Scaled(24);
  const int card_w = Scaled(264);
  const int right_left = left + card_w + Scaled(16);
  const int right_edge = Scaled(kWidth - kMargin);
  std::string account_name;
  std::string account_email;
  const bool signed_in =
      host_ && host_->AccountInfo(&account_name, &account_email);
  const bool signing_in = host_ && host_->AccountSigningIn();
  {
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {left, top, left + card_w, top + Scaled(186)};
    items_.push_back(std::move(card));
    Item avatar;
    avatar.kind = ItemKind::kIcon;
    avatar.text = std::wstring(1, wchar_t(0xE77B));  // Contact
    avatar.value = 56;  // Glyph size.
    avatar.bounds = {left, top + Scaled(28), left + card_w,
                     top + Scaled(28) + Scaled(72)};
    items_.push_back(std::move(avatar));
    Item name;
    name.kind = ItemKind::kToolButton;  // Centered plain text.
    name.text = signed_in && !account_name.empty()
                    ? base::UTF8ToWide(account_name)
                    : L"You";
    name.bounds = {left, top + Scaled(108), left + card_w,
                   top + Scaled(108) + Scaled(22)};
    items_.push_back(std::move(name));
    Item hint;
    hint.kind = ItemKind::kHint;
    hint.text = signed_in ? base::UTF8ToWide(account_email)
                : signing_in
                    ? L"Finish signing in from the opened tab…"
                    : L"Sign in to use your Phi account.";
    hint.bounds = {left + Scaled(30), top + Scaled(134),
                   left + card_w - Scaled(10),
                   top + Scaled(134) + Scaled(kHintHeight)};
    items_.push_back(std::move(hint));
  }
  {
    // The account card (the Mac's 92pt row): status + Sign in / Logout.
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {right_left, top, right_edge, top + Scaled(92)};
    items_.push_back(std::move(card));
    Item label;
    label.kind = ItemKind::kRowLabel;
    label.text = signed_in
                     ? L"Signed in as " + base::UTF8ToWide(
                           account_email.empty() ? account_name
                                                 : account_email)
                     : L"You are not signed in.";
    label.bounds = {right_left + Scaled(kCardPadding), top,
                    right_edge - Scaled(kCardPadding + 100),
                    top + Scaled(92)};
    items_.push_back(std::move(label));
    Item button;
    button.kind = ItemKind::kButton;
    button.text = signed_in ? L"Logout" : L"Sign in";
    button.action = signed_in ? "signout" : "signin";
    button.enabled = !signing_in;
    button.bounds = {right_edge - Scaled(kCardPadding) - Scaled(84),
                     top + Scaled(34), right_edge - Scaled(kCardPadding),
                     top + Scaled(58)};
    items_.push_back(std::move(button));
  }
  {
    // Default Browser (the Mac's 74pt section), with a LIVE button.
    const int section_top = top + Scaled(92) + Scaled(20);
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {right_left, section_top, right_edge,
                   section_top + Scaled(74)};
    items_.push_back(std::move(card));
    if (!is_default_browser_.has_value()) {
      is_default_browser_ = ThisExeIsDefaultBrowser();
    }
    const bool is_default = *is_default_browser_;
    Item label;
    label.kind = ItemKind::kRowLabel;
    label.text =
        PhiStr(is_default ? "settings.account.defaultBrowser.defaultStatus"
                          : "settings.account.defaultBrowser.notDefaultStatus");
    label.bounds = {right_left + Scaled(kCardPadding), section_top,
                    right_edge - Scaled(kCardPadding + 124),
                    section_top + Scaled(74)};
    items_.push_back(std::move(label));
    Item button;
    button.kind = ItemKind::kButton;
    button.text = PhiStr("settings.account.defaultBrowser.setDefaultButton");
    button.action = "defaultbrowser";
    // Already default: the button stays visible but goes inert and dim, as
    // it does on Mac.
    button.enabled = !is_default;
    button.bounds = {right_edge - Scaled(kCardPadding) - Scaled(116),
                     section_top + Scaled(25),
                     right_edge - Scaled(kCardPadding),
                     section_top + Scaled(49)};
    items_.push_back(std::move(button));
  }
  *y = top + Scaled(186) + Scaled(kSectionGap);
}

void PhiSettingsWindow::BuildAi(int* y) {
  AddToggleRow(y, L"Enable AI features in Phi Browser", /*on=*/true,
               /*enabled=*/false, std::string());
  *y += Scaled(8);
  BeginSection(y, L"Browser memory");
  {
    const RECT area = AddRow(y, L"View and manage your browser memory",
                             kRowLabelHeight, 20);
    Item chevron;
    chevron.kind = ItemKind::kLink;
    chevron.text = L"›";
    chevron.enabled = false;
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
  }
  EndSection(y);
  BeginSection(y, L"Phi Sentinel");
  AddToggleRow(y, L"Launch Phi Sentinel automatically at login", true, false,
               std::string());
  AddDividerRow(y);
  {
    const RECT area = AddRow(y, L"Private AI", kRowLabelHeight, 20);
    Item chevron;
    chevron.kind = ItemKind::kLink;
    chevron.text = L"›";
    chevron.enabled = false;
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
  }
  EndSection(y);
  BeginSection(y, L"New Tab Page");
  AddToggleRow(y, L"Show proactive suggestions on the new tab page", true,
               false, std::string());
  EndSection(y);
  BeginSection(y, L"AI Sidebar");
  AddToggleRow(y, L"Automatically add current tab as context to new "
                  L"conversation",
               true, false, std::string());
  EndSection(y);
  BeginSection(y, L"External Data Connectors");
  AddToggleRow(y, L"Enable External Data Connectors", true, false,
               std::string());
  AddDividerRow(y);
  AddSubHint(y, L"External Data Connectors help to provide additional "
                L"context for better AI experience");
  *y += Scaled(kRowPadding);
  EndSection(y);
}

void PhiSettingsWindow::BuildShortcuts(int* y) {
  // The Mac Shortcuts pane predates the shared card system: the header row
  // (title + "Restore all shortcuts") sits OUTSIDE one full-height card
  // that is WHITE in light mode, with the search field and the grouped
  // shortcut table inside.
  {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = L"Shortcuts";
    heading.bounds = {Scaled(kMargin), *y, Scaled(kMargin) + Scaled(200),
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    Item restore;
    restore.kind = ItemKind::kButton;
    restore.text = L"Restore all shortcuts";
    restore.enabled = host_->AnyShortcutCustomized();
    restore.action = "restoreshortcuts";
    restore.bounds = {Scaled(kWidth - kMargin) - Scaled(150),
                      *y - Scaled(4),
                      Scaled(kWidth - kMargin),
                      *y - Scaled(4) + Scaled(22)};
    items_.push_back(std::move(restore));
    *y += Scaled(kSectionTitleHeight) + Scaled(10);
  }
  Item card;
  card.kind = ItemKind::kCard;
  card.on = true;  // The white variant.
  card.bounds = {Scaled(kMargin), *y, Scaled(kWidth - kMargin), 0};
  items_.push_back(std::move(card));
  const size_t card_index = items_.size() - 1;

  // The in-card filter ("Find shortcut keys"): typing anywhere while this
  // pane is up feeds it (WM_CHAR), Esc clears, Backspace deletes.
  {
    Item search;
    search.kind = ItemKind::kSearchField;
    search.text = shortcut_search_;
    search.bounds = {Scaled(kMargin), *y, Scaled(kWidth - kMargin),
                     *y + Scaled(40)};
    items_.push_back(std::move(search));
    *y += Scaled(40);
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {Scaled(kMargin), *y, Scaled(kWidth - kMargin), *y + 1};
    items_.push_back(std::move(divider));
    *y += 1;
  }

  // The table, grouped as on Mac (App, File, Edit, View, History,
  // Bookmarks, Window, Tab, Spaces). The host serves the rows: rebindable
  // ones (clickable keycap → the chord recorder) carry a stable name and
  // their EFFECTIVE chord; fixed rows are informational. Plus the live
  // "Go to Space" rows for the first nine Spaces.
  struct ShortcutRowDef {
    std::wstring group;
    std::wstring name;
    std::wstring keys;
    std::string id;  // Rebindable stable name; empty = fixed.
    bool customized = false;
  };
  std::vector<ShortcutRowDef> rows;
  for (const std::string& summary : host_->ShortcutRows()) {
    const std::vector<std::string> parts = SplitTabs(summary);
    if (parts.size() < 4) {
      continue;
    }
    rows.push_back({base::UTF8ToWide(parts[0]), base::UTF8ToWide(parts[1]),
                    base::UTF8ToWide(parts[2]), parts[3],
                    parts.size() > 4 && parts[4] == "1"});
  }
  {
    int at = 0;
    for (const std::string& summary : host_->SpaceSummaries()) {
      if (++at > 9) {
        break;
      }
      const std::vector<std::string> parts = SplitTabs(summary);
      rows.push_back({L"Spaces",
                      L"Go to Space \u201C" + base::UTF8ToWide(parts[0]) +
                          L"\u201D",
                      L"Ctrl+" + std::to_wstring(at)});
    }
  }

  std::wstring filter = shortcut_search_;
  for (wchar_t& c : filter) {
    c = towlower(c);
  }
  const auto matches = [&](const ShortcutRowDef& row) {
    if (filter.empty()) {
      return true;
    }
    std::wstring name = row.name, keys = row.keys;
    for (wchar_t& c : name) {
      c = towlower(c);
    }
    for (wchar_t& c : keys) {
      c = towlower(c);
    }
    return name.find(filter) != std::wstring::npos ||
           keys.find(filter) != std::wstring::npos;
  };

  const int row_h = Scaled(42);
  std::wstring group;
  bool any = false;
  for (const auto& row : rows) {
    if (!matches(row)) {
      continue;
    }
    any = true;
    if (group != row.group) {
      group = row.group;
      // Category title (12pt semibold secondary, 35pt band) over a rule.
      Item heading;
      heading.kind = ItemKind::kSectionTitle;
      heading.text = row.group;
      heading.bounds = {Scaled(kMargin + kCardPadding), *y,
                        Scaled(kWidth - kMargin - kCardPadding),
                        *y + Scaled(35)};
      items_.push_back(std::move(heading));
      *y += Scaled(35);
      Item rule;
      rule.kind = ItemKind::kDivider;
      rule.bounds = {Scaled(kMargin + kCardPadding), *y,
                     Scaled(kWidth - kMargin - kCardPadding), *y + 1};
      items_.push_back(std::move(rule));
      *y += 1;
    }
    Item label;
    label.kind = ItemKind::kRowLabel;
    label.text = row.name;
    label.bounds = {Scaled(kMargin + kCardPadding), *y,
                    Scaled(kWidth - kMargin - kCardPadding) - Scaled(150),
                    *y + row_h};
    items_.push_back(std::move(label));
    // One pill holding the whole chord (min width 80), the Mac's keycap.
    // A rebindable row's keycap opens the chord recorder; `detail` carries
    // the label for the prompt's message, `on` marks a customised chord.
    const int cap_w =
        std::max(Scaled(80),
                 Scaled(static_cast<int>(row.keys.size()) * 7 + 24));
    Item cap;
    cap.kind = ItemKind::kKeycap;
    cap.text = row.keys;
    cap.on = row.customized;
    if (!row.id.empty()) {
      cap.action = "shortcut";
      cap.payload = row.id;
      cap.detail = row.name;
    }
    cap.bounds = {Scaled(kWidth - kMargin - kCardPadding) - cap_w,
                  *y + (row_h - Scaled(22)) / 2,
                  Scaled(kWidth - kMargin - kCardPadding),
                  *y + (row_h + Scaled(22)) / 2};
    items_.push_back(std::move(cap));
    *y += row_h;
    Item rule;
    rule.kind = ItemKind::kDivider;
    rule.bounds = {Scaled(kMargin + kCardPadding), *y,
                   Scaled(kWidth - kMargin - kCardPadding), *y + 1};
    items_.push_back(std::move(rule));
    *y += 1;
  }
  if (!any) {
    Item empty;
    empty.kind = ItemKind::kHint;
    empty.text = L"No Results";
    empty.bounds = {Scaled(kMargin), *y + Scaled(40),
                    Scaled(kWidth - kMargin), *y + Scaled(40) + Scaled(16)};
    items_.push_back(std::move(empty));
    *y += Scaled(96);
  }
  *y += Scaled(8);
  items_[card_index].bounds.bottom = *y;
  *y += Scaled(kSectionGap);
}

void PhiSettingsWindow::BuildDeveloper(int* y) {
  BeginSection(y, L"Agent control");
  AddToggleRow(y, L"Allow agents to control Phi (CDP)", /*on=*/true,
               /*enabled=*/false, std::string());
  AddDividerRow(y);
  AddSubHint(y, L"Agent tools drive Phi over the DevTools Protocol through "
                L"a private socket only this machine's processes can reach.");
  *y += Scaled(kRowPadding);
  EndSection(y);
  BeginSection(y, L"Approved agents");
  const std::vector<std::string> grants = host_->AgentGrantRows();
  if (grants.empty()) {
    AddSubHint(y, L"No program holds \u201CAlways Allow\u201D. Anything "
                  L"connecting to control Phi is approved per connection.");
  } else {
    for (size_t i = 0; i < grants.size(); ++i) {
      const std::wstring path = base::UTF8ToWide(grants[i]);
      const RECT control = AddRow(y, path.c_str(), 22, 22);
      Item remove;
      remove.kind = ItemKind::kToolButton;
      remove.text = L"\uE74D";  // Delete — revokes the standing grant.
      remove.action = "removegrant";
      remove.payload = base::NumberToString(static_cast<int>(i));
      remove.bounds = control;
      items_.push_back(std::move(remove));
      if (i + 1 < grants.size()) {
        AddDividerRow(y);
      }
    }
  }
  *y += Scaled(kRowPadding);
  EndSection(y);
  BeginSection(y, L"Credential approvals");
  const std::vector<std::string> creds = host_->CredentialGrantRows();
  if (creds.empty()) {
    AddSubHint(y, L"No standing approvals. Every credential request is "
                  L"approved individually.");
  } else {
    for (size_t i = 0; i < creds.size(); ++i) {
      const std::vector<std::string> parts = SplitTabs(creds[i]);
      const bool reveal = parts.size() > 1 && parts[1] == "reveal";
      // The exposure is part of the row: "reveal" means the secret is handed
      // to the agent, "fill" means it never leaves Phi.
      const std::wstring label =
          base::UTF8ToWide(parts[0]) +
          (reveal ? L"  \u2014 reveal to agent" : L"  \u2014 fill only");
      const RECT control = AddRow(y, label.c_str(), 22, 22);
      Item remove;
      remove.kind = ItemKind::kToolButton;
      remove.text = L"\uE74D";
      remove.action = "removecredgrant";
      remove.payload = base::NumberToString(static_cast<int>(i));
      remove.bounds = control;
      items_.push_back(std::move(remove));
      if (i + 1 < creds.size()) {
        AddDividerRow(y);
      }
    }
  }
  *y += Scaled(kRowPadding);
  EndSection(y);
  BeginSection(y, L"Credential activity");
  const std::vector<std::string> audit = host_->CredentialAuditRows();
  if (audit.empty()) {
    AddSubHint(y, L"No credential requests yet.");
  } else {
    // Newest first; the pane shows the most recent ten.
    const size_t shown = std::min<size_t>(audit.size(), 10);
    for (size_t i = 0; i < shown; ++i) {
      const std::vector<std::string> parts = SplitTabs(audit[i]);
      if (parts.size() < 4) {
        continue;
      }
      AddSubHint(y, (base::UTF8ToWide(parts[0]) + L"  " +
                     base::UTF8ToWide(parts[1]) + L"  " +
                     base::UTF8ToWide(parts[2]) + L"  \u2014  " +
                     base::UTF8ToWide(parts[3]))
                        .c_str());
    }
  }
  *y += Scaled(kRowPadding);
  EndSection(y);
  BeginSection(y, L"Agent Password Manager");
  AddToggleRow(y, L"Bitwarden password manager", /*on=*/false,
               /*enabled=*/false, std::string());
  AddDividerRow(y);
  AddSubHint(y, L"Logins for Phi and agent requests, held by a separate "
                L"helper — passwords never touch the browser process.");
  *y += Scaled(kRowPadding);
  EndSection(y);
}

void PhiSettingsWindow::Rebuild() {
  items_.clear();
  open_card_ = 0;
  if (!host_) {
    return;
  }
  // The pane toolbar, centred across the top. Two panes are not always
  // mounted: Developer outside developer mode, and Phi & AI while the AI
  // suite is off (phi_features.h).
  const bool developer = host_->DeveloperMode();
  const auto unmounted = [developer](const char* id) {
    const std::string name(id);
    return (!developer && name == "developer") ||
           (!kPhiAiEnabled && name == "ai");
  };
  int shown = 0;
  for (const SectionDef& section : kSections) {
    if (!unmounted(section.id)) {
      ++shown;
    }
  }
  int x = Scaled((kWidth - shown * kToolbarItemWidth) / 2);
  for (const SectionDef& section : kSections) {
    if (unmounted(section.id)) {
      continue;
    }
    Item entry;
    entry.kind = ItemKind::kToolbarItem;
    entry.text = section.label;
    entry.payload = section.id;
    // The URL Rules editor is a drill-in of the Navigation pane; its toolbar
    // item stays lit.
    entry.selected = section_ == section.id ||
                     (section_ == "urlrules" &&
                      std::string(section.id) == "navigation");
    entry.action = "section";
    entry.bounds = {x, Scaled(kCaptionHeight + 8),
                    x + Scaled(kToolbarItemWidth),
                    Scaled(kChromeTop - 8)};
    items_.push_back(std::move(entry));
    x += Scaled(kToolbarItemWidth);
  }
  if (unmounted(section_.c_str())) {
    section_ = "general";  // The pane vanished under us, as on Mac.
    SyncTitle();
  }

  picker_grid_ = {};  // Only the Spaces pane mounts the icon picker.
  picker_max_line_ = 0;
  int y = Scaled(kChromeTop + kMargin) - scroll_;
  const int top = y;
  if (section_ == "general") {
    BuildGeneral(&y);
  } else if (section_ == "spaces") {
    BuildSpaces(&y);
  } else if (section_ == "navigation") {
    BuildNavigation(&y);
  } else if (section_ == "urlrules") {
    BuildUrlRules(&y);
  } else if (section_ == "profiles") {
    BuildProfiles(&y);
  } else if (section_ == "shortcuts") {
    BuildShortcuts(&y);
  } else if (section_ == "ai") {
    BuildAi(&y);
  } else if (section_ == "account") {
    BuildAccount(&y);
  } else if (section_ == "developer") {
    BuildDeveloper(&y);
  } else {
    section_ = "general";  // A retired pane id (e.g. "phiLink").
    BuildGeneral(&y);
  }
  content_height_ = y - top + Scaled(kMargin);
}

void PhiSettingsWindow::Paint(HDC dc) {
  if (!host_) {
    return;
  }
  // The Mac's fixedWindowBackground: flat, theme-INdependent — #EAEAEA
  // light / #252525 dark. Only the appearance choice matters here.
  const bool dark = host_->Palette().dark;
  const COLORREF bg = dark ? RGB(0x25, 0x25, 0x25) : RGB(0xEA, 0xEA, 0xEA);
  const COLORREF ink = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
  const COLORREF label = dark ? Blend(RGB(0xFF, 0xFF, 0xFF), bg, 204)
                              : RGB(0x26, 0x26, 0x26);
  const COLORREF secondary = dark ? RGB(0xB3, 0xB3, 0xB3) : RGB(0x66, 0x66, 0x66);
  const COLORREF tertiary = Blend(ink, bg, 77);
  const COLORREF card_bg = Blend(ink, bg, 5);        // settingItemBackground 2%
  const COLORREF card_border = Blend(ink, bg, 20);   // border 8%
  const COLORREF separator = Blend(ink, bg, 15);
  const COLORREF accent = host_->Palette().accent;

  RECT client = {};
  ::GetClientRect(hwnd_, &client);
  HBRUSH page = ::CreateSolidBrush(bg);
  ::FillRect(dc, &client, page);
  ::DeleteObject(page);
  // The toolbar band shares the ground; a separator closes it.
  RECT bar_sep = {0, Scaled(kChromeTop), client.right,
                  Scaled(kChromeTop) + 1};
  HBRUSH sep_brush = ::CreateSolidBrush(separator);
  ::FillRect(dc, &bar_sep, sep_brush);
  ::DeleteObject(sep_brush);

  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = font_ ? ::SelectObject(dc, font_) : nullptr;

  // The caption strip: Phi mark leading, the pane title CENTERED, the
  // close button trailing (Win11's metric, red when hot).
  {
    const std::vector<unsigned char>& logo = PhiLogoBgra256();
    if (!logo.empty()) {
      const int mark = Scaled(16);
      RECT mark_rc = {Scaled(12), (Scaled(kCaptionHeight) - mark) / 2,
                      Scaled(12) + mark,
                      (Scaled(kCaptionHeight) + mark) / 2};
      PhiDrawBgra(dc, mark_rc, logo.data(), 256, 256);
    }
    wchar_t title[128] = {};
    ::GetWindowTextW(hwnd_, title, 127);
    if (title_font_) {
      ::SelectObject(dc, title_font_);
    }
    ::SetTextColor(dc, label);
    RECT title_rc = {0, 0, client.right, Scaled(kCaptionHeight)};
    PhiDrawTextAA(dc, title, -1, &title_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT close_rc = {client.right - Scaled(kCaptionButtonWidth), 0,
                     client.right, Scaled(kCaptionHeight)};
    if (close_hot_) {
      HBRUSH hot = ::CreateSolidBrush(RGB(0xC4, 0x2B, 0x1C));
      ::FillRect(dc, &close_rc, hot);
      ::DeleteObject(hot);
    }
    if (caption_font_) {
      ::SelectObject(dc, caption_font_);
    }
    ::SetTextColor(dc, close_hot_ ? RGB(0xFF, 0xFF, 0xFF) : label);
    PhiDrawTextAA(dc, L"", -1, &close_rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (font_) {
      ::SelectObject(dc, font_);
    }
  }
  bool clipped = false;

  for (const Item& item : items_) {
    if (item.kind != ItemKind::kToolbarItem && !clipped) {
      // Pane content scrolls; nothing of it may paint over the toolbar.
      ::IntersectClipRect(dc, 0, Scaled(kChromeTop) + 1, client.right,
                          client.bottom);
      clipped = true;
    }
    RECT bounds = item.bounds;
    const COLORREF row_ink = item.enabled ? label : Blend(label, bg, 120);
    switch (item.kind) {
      case ItemKind::kToolbarItem: {
        if (item.selected) {
          FillRounded(dc, bounds, Blend(ink, bg, 20), Scaled(kRadius));
        }
        ::SetTextColor(dc, item.selected ? accent : secondary);
        RECT glyph = bounds;
        glyph.bottom = glyph.top + Scaled(kToolbarIcon + 8);
        wchar_t mark = 0xE713;
        for (const SectionDef& section : kSections) {
          if (item.payload == section.id) {
            mark = section.glyph;
            break;
          }
        }
        // MDL2 stand-ins for the Mac's template PDFs (~16pt of ink).
        if (mdl2_font_) {
          ::SelectObject(dc, mdl2_font_);
        }
        const std::wstring mark_text(1, mark);
        PhiDrawTextAA(dc, mark_text.c_str(), -1, &glyph,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT caption = bounds;
        caption.top = glyph.bottom;
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        // NOPREFIX: "Phi & AI" must keep its ampersand.
        PhiDrawTextAA(dc, item.text.c_str(), -1, &caption,
                    DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kCard:
        // `on` marks the Shortcuts pane's own chrome: WHITE in light mode
        // (the Mac rolls its own card there), the usual 2% wash otherwise.
        StrokeRounded(dc, bounds,
                      item.on && !dark ? RGB(0xFF, 0xFF, 0xFF) : card_bg,
                      card_border,
                      Scaled(item.on ? 8 : kCardRadius), 1);
        break;
      case ItemKind::kSectionTitle:
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        ::SetTextColor(dc, secondary);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kSubtitle:
      case ItemKind::kHint:
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        ::SetTextColor(dc, tertiary);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kRowLabel:
        // Single line, truly centred: DT_WORDBREAK silently disables
        // DT_VCENTER, which left labels riding the top of tall rows.
        ::SetTextColor(dc, row_ink);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
        break;
      case ItemKind::kLink:
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        ::SetTextColor(dc, item.enabled ? accent : tertiary);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kDivider: {
        HBRUSH line = ::CreateSolidBrush(separator);
        ::FillRect(dc, &bounds, line);
        ::DeleteObject(line);
        break;
      }
      case ItemKind::kSwatch: {
        // A dot (22pt, or `value` when the pane overrides — the Spaces
        // detail's 16) centred in its item width; a 2pt ring in the
        // theme's OWN accent when selected; the caption (11pt) shows only
        // for the selected swatch.
        const int cx = (bounds.left + bounds.right) / 2;
        const int dot = Scaled(item.value > 0 ? item.value : kSwatchDot);
        const int ring = dot + Scaled(4);
        RECT ring_rc = {cx - ring / 2, bounds.top, cx + ring / 2,
                        bounds.top + ring};
        if (item.selected) {
          const COLORREF ring_color = item.ring ? item.ring : accent;
          Ellipse(dc, ring_rc, card_bg,
                  item.enabled ? ring_color : tertiary, Scaled(2));
        }
        RECT dot_rc = {cx - dot / 2, bounds.top + (ring - dot) / 2,
                       cx + dot / 2, bounds.top + (ring + dot) / 2};
        COLORREF fill = item.swatch;
        if (!item.enabled) {
          fill = Blend(fill, bg, 100);
        }
        // Pure is white and needs the contrast hairline.
        Ellipse(dc, dot_rc, fill, Blend(ink, bg, 31), 1);
        if (item.selected && !item.text.empty()) {
          if (hint_font_) {
            ::SelectObject(dc, hint_font_);
          }
          RECT caption = {bounds.left - Scaled(6), bounds.top + ring +
                          Scaled(6), bounds.right + Scaled(6), bounds.bottom};
          ::SetTextColor(dc, row_ink);
          PhiDrawTextAA(dc, item.text.c_str(), -1, &caption,
                      DT_CENTER | DT_TOP | DT_SINGLELINE);
          if (font_) {
            ::SelectObject(dc, font_);
          }
        }
        break;
      }
      case ItemKind::kImageCard: {
        // The Mac's 122x72 tile PDFs, redrawn: a mini window card (r8,
        // ink 2%) carrying per-mode chrome at ink 6%/10%, plus the baked
        // #3AA4D5 agent pill. Selection is a 2pt theme ring at r5;
        // UNSELECTED tiles have no outline at all (the Mac's lineWidth 0).
        RECT image = {bounds.left, bounds.top, bounds.right,
                      bounds.top + Scaled(kImageCardH)};
        const bool tile_dark =
            item.payload == "dark" || (item.payload != "light" &&
                                       item.payload != "system" && dark);
        const COLORREF tile_ground =
            item.payload == "dark"
                ? RGB(0x34, 0x34, 0x34)
                : (tile_dark ? RGB(0x2E, 0x2E, 0x2E) : bg);
        const COLORREF tile_ink =
            tile_dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
        const COLORREF kAgentPill = RGB(0x3A, 0xA4, 0xD5);
        // The window card fills the tile; "system" splits it light/dark.
        const auto draw_tile = [&](const RECT& rc, COLORREF ground,
                                   COLORREF ink_color, bool full) {
          const COLORREF card2 = Blend(ink_color, ground, 5);
          const COLORREF chrome6 = Blend(ink_color, ground, 15);
          const COLORREF chrome10 = Blend(ink_color, ground, 26);
          HBRUSH ground_brush = ::CreateSolidBrush(ground);
          ::FillRect(dc, &rc, ground_brush);
          ::DeleteObject(ground_brush);
          RECT card_rc = {image.left + Scaled(1), image.top + Scaled(1),
                          image.right - Scaled(1), image.bottom - Scaled(1)};
          const int save_tile = ::SaveDC(dc);
          ::IntersectClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
          FillRounded(dc, card_rc, card2, Scaled(8));
          const auto rr = [&](int x, int y, int w, int h, COLORREF c,
                              int r) {
            RECT shape = {image.left + Scaled(x), image.top + Scaled(y),
                          image.left + Scaled(x + w),
                          image.top + Scaled(y + h)};
            FillRounded(dc, shape, c, Scaled(r));
          };
          if (item.payload == "performance") {
            rr(4, 9, 27, 8, chrome6, 4);        // Sidebar address bar.
            rr(32, 1, 1, 70, chrome10, 0);      // Sidebar divider.
            rr(15, 59, 14, 8, kAgentPill, 8);   // Agent pill, bottom.
          } else if (item.payload == "balanced") {
            rr(32, 1, 1, 70, chrome10, 0);
            rr(44, 2, 59, 8, chrome6, 8);       // Top address bar.
            rr(105, 2, 14, 8, kAgentPill, 8);
            rr(33, 10, 88, 1, chrome10, 0);     // Header hairline.
          } else if (item.payload == "comfortable") {
            RECT tab = {image.left + Scaled(8), image.top + Scaled(2),
                        image.left + Scaled(36), image.top + Scaled(11)};
            OutlineRounded(dc, tab, chrome10, Scaled(4), 1);  // Active tab.
            rr(1, 10, 120, 1, chrome10, 0);     // Strip baseline.
            rr(38, 2, 23, 8, chrome6, 5);       // Inactive tab.
            rr(105, 2, 14, 8, kAgentPill, 8);
          } else if (item.payload == "ntp") {
            rr(37, 26, 48, 10, chrome6, 5);     // Centered NTP field.
            rr(105, 2, 14, 8, kAgentPill, 8);
          } else if (item.payload == "omnibox") {
            rr(24, 8, 74, 10, chrome6, 5);      // Dropped omnibox.
            rr(28, 22, 56, 3, chrome6, 2);      // Suggestion lines.
            rr(28, 29, 44, 3, chrome6, 2);
            rr(105, 2, 14, 8, kAgentPill, 8);
          } else {
            // The appearance tiles: two tab chips + the agent pill.
            RECT tab = {image.left + Scaled(8), image.top + Scaled(2),
                        image.left + Scaled(36), image.top + Scaled(11)};
            OutlineRounded(dc, tab, chrome10, Scaled(4), 1);
            rr(38, 2, 23, 8, chrome6, 5);
            rr(62, 2, 23, 8, chrome6, 5);
            rr(105, 2, 14, 8, kAgentPill, 8);
          }
          ::RestoreDC(dc, save_tile);
          (void)full;
        };
        if (item.payload == "system") {
          RECT left_half = {image.left, image.top,
                            (image.left + image.right) / 2, image.bottom};
          RECT right_half = {(image.left + image.right) / 2, image.top,
                             image.right, image.bottom};
          draw_tile(left_half, bg, RGB(0x00, 0x00, 0x00), false);
          draw_tile(right_half, RGB(0x34, 0x34, 0x34),
                    RGB(0xFF, 0xFF, 0xFF), false);
        } else {
          draw_tile(image, tile_ground, tile_ink, true);
        }
        if (!item.enabled) {
          // The Mac dims disabled pickers to 40%.
          HBRUSH wash = ::CreateSolidBrush(bg);
          // Approximate the opacity drop with a light hatch of the ground.
          for (int line = image.top; line < image.bottom; line += 2) {
            RECT stripe = {image.left, line, image.right, line + 1};
            ::FillRect(dc, &stripe, wash);
          }
          ::DeleteObject(wash);
        }
        if (item.selected) {
          OutlineRounded(dc, image, accent, Scaled(5), Scaled(2));
        }
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        RECT caption = {bounds.left, image.bottom + Scaled(10) - Scaled(4),
                        bounds.right, bounds.bottom};
        ::SetTextColor(dc, row_ink);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &caption,
                    DT_CENTER | DT_TOP | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kToggle: {
        const int height = bounds.bottom - bounds.top;
        COLORREF fill = item.on ? accent : Blend(ink, bg, 60);
        if (!item.enabled) {
          fill = Blend(fill, bg, 120);
        }
        FillRounded(dc, bounds, fill, height / 2);
        const int knob = height - Scaled(4);
        const int left = item.on ? bounds.right - knob - Scaled(2)
                                 : bounds.left + Scaled(2);
        RECT dot = {left, bounds.top + Scaled(2), left + knob,
                    bounds.top + Scaled(2) + knob};
        Ellipse(dc, dot, RGB(0xFF, 0xFF, 0xFF), 0, 0);
        break;
      }
      case ItemKind::kSliderTrack: {
        // The Mac bar: 10pt tall, full width, rounded r5, a horizontal
        // gradient of the theme overlay's HSB saturation 0.1 -> 0.9 (Pure
        // ramps brightness instead, brightest at the LEFT); a 1pt border;
        // an 18pt white knob whose travel is width - knob.
        const int mid = (bounds.top + bounds.bottom) / 2;
        RECT track = {bounds.left, mid - Scaled(5), bounds.right,
                      mid + Scaled(5)};
        const int span = track.right - track.left;
        double hue = 0, sat = 0, val = 0;
        RgbToHsv(item.swatch, &hue, &sat, &val);
        // The gradient is painted square and its corners knocked back to
        // the card ground afterwards (antialiased), where a region clip
        // would cut the rounded track on the pixel grid.
        const int bands = 32;
        for (int i = 0; i < bands; ++i) {
          RECT band = {track.left + span * i / bands, track.top,
                       track.left + span * (i + 1) / bands, track.bottom};
          const double t = (i + 0.5) / bands;
          COLORREF tint;
          if (item.on) {
            // Pure: brightness 0.94 -> 0.84 light, 0.40 -> 0.0 dark.
            const double hi = dark ? 0.40 : 0.94;
            const double lo = dark ? 0.0 : 0.84;
            tint = HsvToRgb(hue, sat, hi + (lo - hi) * t);
          } else {
            tint = HsvToRgb(hue, 0.1 + 0.8 * t, val);
          }
          HBRUSH brush = ::CreateSolidBrush(item.enabled
                                                ? tint
                                                : Blend(tint, bg, 140));
          ::FillRect(dc, &band, brush);
          ::DeleteObject(brush);
        }
        PhiKnockOutRoundRectAA(dc, track, Scaled(5), card_bg);
        OutlineRounded(dc, track, card_border, Scaled(5), 1);
        const int knob = Scaled(kSliderKnob);
        const int at = track.left + (span - knob) * item.value / 100;
        RECT dot = {at, mid - knob / 2, at + knob, mid + knob / 2};
        Ellipse(dc, dot, RGB(0xFF, 0xFF, 0xFF), card_border, 1);
        break;
      }
      case ItemKind::kPopup: {
        FillRounded(dc, bounds, Blend(ink, bg, 15), Scaled(kRadius));
        ::SetTextColor(dc, row_ink);
        RECT text = bounds;
        text.left += Scaled(10);
        text.right -= Scaled(18);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &text,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT arrows = bounds;
        arrows.left = bounds.right - Scaled(16);
        ::SetTextColor(dc, secondary);
        PhiDrawTextAA(dc, L"↕", -1, &arrows,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
      }
      case ItemKind::kListRow: {
        // Two Mac selection styles: Spaces (`value` 1) fills the SOLID
        // accent with white content; Profiles (`value` 2) a 15% accent
        // wash with normal ink.
        const bool solid = item.value == 1;
        if (item.selected) {
          FillRounded(dc, bounds,
                      solid ? accent : Blend(accent, card_bg, 38),
                      Scaled(kRadius));
        }
        const bool on_accent = solid && item.selected;
        const COLORREF text_ink = on_accent ? RGB(0xFF, 0xFF, 0xFF)
                                            : row_ink;
        int x = bounds.left + Scaled(solid ? 8 : 10);
        if (solid) {
          // The Spaces row's drag handle (line.3.horizontal).
          if (mdl2_font_) {
            ::SelectObject(dc, mdl2_font_);
          }
          ::SetTextColor(dc, on_accent ? RGB(0xFF, 0xFF, 0xFF) : secondary);
          RECT handle = {x, bounds.top, x + Scaled(14), bounds.bottom};
          PhiDrawTextAA(dc, L"\uE700", -1, &handle,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
          if (font_) {
            ::SelectObject(dc, font_);
          }
          x += Scaled(14) + Scaled(8);
        }
        if (!item.icon.empty()) {
          // The Space's icon in an 18pt box: catalog artwork tinted the
          // row ink, colour emoji via DirectWrite, GDI glyph as the last
          // resort — the same ladder as the picker's cells. Both draws take
          // the icon's INK size, so a column of Spaces reads as one size
          // whatever medium each icon comes from.
          const int mid = (bounds.top + bounds.bottom) / 2;
          RECT icon_rc = {x, mid - Scaled(9), x + Scaled(18), mid + Scaled(9)};
          const bool drawn =
              PhiDrawCatalogIcon(dc, icon_rc, item.icon, Scaled(13),
                                 text_ink) ||
              (!PhiIsPrivateGlyph(item.icon) &&
               PhiDrawColorIcon(dc, icon_rc, item.icon, Scaled(13),
                                text_ink));
          if (!drawn) {
            HFONT icon_font =
                PhiIsPrivateGlyph(item.icon) ? mdl2_font_ : emoji_font_;
            if (icon_font) {
              ::SelectObject(dc, icon_font);
            }
            ::SetTextColor(dc, text_ink);
            PhiDrawTextAA(dc, item.icon.c_str(), -1, &icon_rc,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (font_) {
              ::SelectObject(dc, font_);
            }
          }
          x += Scaled(18) + Scaled(8);
        }
        ::SetTextColor(dc, text_ink);
        RECT text = bounds;
        text.left = x;
        text.right -= Scaled(item.on ? 130 : 76);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &text,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                        DT_NOPREFIX);
        int badge_left = text.left;
        if (!item.text.empty()) {
          RECT measured = {0, 0, 0, 0};
          PhiDrawTextAA(dc, item.text.c_str(), -1, &measured,
                      DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
          badge_left += std::min<int>(measured.right,
                                      text.right - text.left);
        }
        if (item.on) {
          // The Default badge: a 10pt capsule (secondary 15%, or white 20%
          // on the accent).
          if (hint_font_) {
            ::SelectObject(dc, hint_font_);
          }
          RECT cap = {badge_left + Scaled(8),
                      (bounds.top + bounds.bottom) / 2 - Scaled(9),
                      badge_left + Scaled(8) + Scaled(52),
                      (bounds.top + bounds.bottom) / 2 + Scaled(9)};
          FillRounded(dc, cap,
                      on_accent ? Blend(RGB(0xFF, 0xFF, 0xFF), accent, 51)
                                : Blend(ink, bg, 38),
                      (cap.bottom - cap.top) / 2);
          ::SetTextColor(dc, on_accent ? RGB(0xFF, 0xFF, 0xFF) : secondary);
          PhiDrawTextAA(dc, L"Default", -1, &cap,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          if (font_) {
            ::SelectObject(dc, font_);
          }
        }
        if (!item.detail.empty()) {
          if (hint_font_) {
            ::SelectObject(dc, hint_font_);
          }
          RECT detail = bounds;
          detail.right -= Scaled(solid ? 20 : 8);
          ::SetTextColor(dc, on_accent ? RGB(0xFF, 0xFF, 0xFF) : secondary);
          PhiDrawTextAA(dc, item.detail.c_str(), -1, &detail,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
          if (solid) {
            // The Mac row hosts a profile picker; its chevron.up.down
            // stands in (bindings are immutable here).
            RECT chev = bounds;
            chev.right -= Scaled(6);
            PhiDrawTextAA(dc, L"↕", -1, &chev,
                        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
          }
          if (font_) {
            ::SelectObject(dc, font_);
          }
        }
        break;
      }
      case ItemKind::kToolButton: {
        // The +/-/pencil strip glyphs are MDL2; pager arrows and plain
        // captions ride the UI font.
        const bool mdl2 = !item.text.empty() && PhiIsPrivateGlyph(item.text);
        if (mdl2 && mdl2_font_) {
          ::SelectObject(dc, mdl2_font_);
        }
        ::SetTextColor(dc, item.enabled ? row_ink : tertiary);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (mdl2 && font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kIcon: {
        // A lone MDL2 glyph: secondary at rest, accent when `selected`
        // (the data links' arrow), tertiary when disabled; `value`
        // overrides the 16pt size (the Account avatar).
        HFONT icon_font = mdl2_font_;
        HFONT sized = nullptr;
        if (item.value > 0) {
          LOGFONTW lf = {};
          if (mdl2_font_ && ::GetObjectW(mdl2_font_, sizeof(lf), &lf)) {
            lf.lfHeight = -Scaled(item.value);
            sized = ::CreateFontIndirectW(&lf);
            if (sized) {
              icon_font = sized;
            }
          }
        }
        if (icon_font) {
          ::SelectObject(dc, icon_font);
        }
        ::SetTextColor(dc, !item.enabled ? tertiary
                                         : (item.selected ? accent
                                                          : secondary));
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        if (sized) {
          ::DeleteObject(sized);
        }
        break;
      }
      case ItemKind::kKeycap: {
        // The Mac keycap: one pill for the whole chord — r6,
        // controlBackground fill, hairline separator border, 13pt
        // secondary text.
        const COLORREF cap_bg =
            dark ? Blend(RGB(0xFF, 0xFF, 0xFF), bg, 13)
                 : RGB(0xFF, 0xFF, 0xFF);
        // A customised chord (`on`) shows in the accent, the Mac's cue that
        // the row departs from its default.
        StrokeRounded(dc, bounds, cap_bg, item.on ? accent : separator,
                      Scaled(6), 1);
        ::SetTextColor(dc, item.on ? accent : secondary);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        break;
      }
      case ItemKind::kSearchField: {
        // Magnifier + the live filter text (placeholder when empty).
        if (mdl2_font_) {
          ::SelectObject(dc, mdl2_font_);
        }
        ::SetTextColor(dc, secondary);
        RECT glass = {bounds.left + Scaled(12), bounds.top,
                      bounds.left + Scaled(28), bounds.bottom};
        PhiDrawTextAA(dc, L"", -1, &glass,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        RECT text = {glass.right + Scaled(8), bounds.top,
                     bounds.right - Scaled(12), bounds.bottom};
        if (item.text.empty()) {
          ::SetTextColor(dc, tertiary);
          PhiDrawTextAA(dc, L"Find shortcut keys", -1, &text,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
          ::SetTextColor(dc, row_ink);
          PhiDrawTextAA(dc, item.text.c_str(), -1, &text,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        break;
      }
      case ItemKind::kEmojiCell: {
        if (item.selected) {
          FillRounded(dc, bounds, Blend(ink, bg, 25), Scaled(10));
        }
        {
          // Inset: the colour pass repaints its whole rect, and a selected
          // cell's rounded fill must survive at the corners.
          RECT glyph_rc = bounds;
          ::InflateRect(&glyph_rc, -Scaled(4), -Scaled(4));
          // One ink size for every cell: the catalog's shapes and the
          // emoji each cover a different share of their own box, so sizing
          // by canvas or em leaves the grid visibly ragged.
          if (PhiDrawCatalogIcon(dc, glyph_rc, item.text, Scaled(15),
                                 row_ink)) {
            break;  // The Mac catalog's own artwork, tinted the row ink.
          }
          if (!PhiIsPrivateGlyph(item.text) &&
              PhiDrawColorIcon(dc, glyph_rc, item.text, Scaled(15),
                               row_ink)) {
            break;  // Colour emoji via DirectWrite; GDI is the fallback.
          }
        }
        {
          HFONT cell_font =
              PhiIsPrivateGlyph(item.text) ? mdl2_font_ : emoji_font_;
          if (cell_font) {
            ::SelectObject(dc, cell_font);
          }
        }
        ::SetTextColor(dc, row_ink);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kBadge: {
        FillRounded(dc, bounds, Blend(ink, bg, 25),
                    (bounds.bottom - bounds.top) / 2);
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        ::SetTextColor(dc, secondary);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kButton:
        if (item.selected) {
          FillRounded(dc, bounds, accent, Scaled(kRadius));
        } else {
          StrokeRounded(dc, bounds, Blend(ink, bg, 10), card_border,
                        Scaled(kRadius), 1);
        }
        ::SetTextColor(dc, item.selected ? RGB(0xFF, 0xFF, 0xFF) : row_ink);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
    }
  }

  ::SelectClipRgn(dc, nullptr);
  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

void PhiSettingsWindow::Apply(const Item& item, int x) {
  if (item.action == "section") {
    section_ = item.payload;
    scroll_ = 0;
    shortcut_search_.clear();
    SyncTitle();
  } else if (item.action == "theme") {
    host_->SetThemeId(item.payload);
  } else if (item.action == "appearance") {
    host_->SetAppearance(static_cast<PhiAppearance>(item.value));
  } else if (item.action == "layout") {
    host_->RequestLayoutMode(static_cast<PhiLayoutMode>(item.value));
  } else if (item.action == "newtab") {
    host_->SetNewTabBehavior(static_cast<PhiNewTabBehavior>(item.value));
  } else if (item.action == "tabpreviews") {
    host_->SetShowTabPreviews(!item.on);
  } else if (item.action == "fullurl") {
    host_->SetAlwaysShowFullUrl(!item.on);
  } else if (item.action == "restoresession") {
    host_->SetRestoreLastSession(!item.on);
  } else if (item.action == "peekview") {
    const bool on = !item.on;
    host_->SetPeekViewEnabled(on);
    // The automatic sub-switch follows the master's transitions: switching
    // Peek on re-arms it, switching Peek off stands it down (the Mac's
    // NavigationsSettingsView does exactly this).
    host_->SetAutoPeekViewEnabled(on);
  } else if (item.action == "autopeek") {
    host_->SetAutoPeekViewEnabled(!item.on);
  } else if (item.action == "devmode") {
    host_->SetDeveloperMode(!item.on);
  } else if (item.action == "gospaces") {
    section_ = "spaces";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "gonavigation") {
    section_ = "navigation";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "urlrules") {
    section_ = "urlrules";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "shortcut") {
    PhiShortcutChord chord;
    const std::wstring message =
        L"Press the new keys for \u201C" + item.detail + L"\u201D.";
    if (PhiPromptShortcut(hwnd_, L"Change Shortcut", message.c_str(),
                          &chord)) {
      host_->RebindShortcut(item.payload, chord.vk, chord.ctrl, chord.shift,
                            chord.alt);
    }
  } else if (item.action == "restoreshortcuts") {
    host_->RestoreAllShortcuts();
  } else if (item.action == "removegrant") {
    int index = -1;
    base::StringToInt(item.payload, &index);
    host_->RemoveAgentGrant(index);
  } else if (item.action == "removecredgrant") {
    int index = -1;
    base::StringToInt(item.payload, &index);
    host_->RemoveCredentialGrant(index);
  } else if (item.action == "addrule") {
    std::wstring value;
    if (PhiPromptText(hwnd_, L"Add URL Rule",
                      L"Enter a domain or URL (e.g. example.com or "
                      L"example.com/path).",
                      &value)) {
      const std::string typed = base::WideToUTF8(value);
      std::string host;
      std::string prefix;
      RuleEncode(RuleTypeForInput(typed), typed, &host, &prefix);
      std::string first_space;
      for (const std::string& summary : host_->SpaceSummaries()) {
        const std::vector<std::string> parts = SplitTabs(summary);
        if (parts.size() > 3) {
          first_space = parts[3];
          break;
        }
      }
      if (!host.empty() && !first_space.empty()) {
        host_->AddURLRule(first_space, host, prefix, false);
      }
    }
  } else if (item.action == "editrule") {
    int index = -1;
    base::StringToInt(item.payload, &index);
    const std::vector<std::string> rules = host_->URLRuleRows();
    if (index >= 0 && index < static_cast<int>(rules.size())) {
      const std::vector<std::string> parts = SplitTabs(rules[index]);
      const std::string old_host =
          parts.size() > 1 ? parts[1] : std::string();
      const std::string old_prefix =
          parts.size() > 2 ? parts[2] : std::string();
      std::wstring value =
          base::UTF8ToWide(RuleDisplayValue(old_host, old_prefix));
      if (PhiPromptText(hwnd_, L"Edit URL Rule",
                        L"Enter a domain or URL for this rule.", &value)) {
        const std::string typed = base::WideToUTF8(value);
        // A typed path or explicit sigils re-derive the match type; a bare
        // host keeps the rule's current one.
        RuleMatchType type = RuleTypeOf(old_host, old_prefix);
        const RuleMatchType derived = RuleTypeForInput(typed);
        if (derived != RuleMatchType::kDomainSuffix ||
            typed.rfind("*.", 0) == 0) {
          type = derived;
        }
        std::string host;
        std::string prefix;
        RuleEncode(type, typed, &host, &prefix);
        if (!host.empty()) {
          host_->SetURLRulePattern(index, host, prefix);
        }
      }
    }
  } else if (item.action == "ruletype") {
    int index = -1;
    base::StringToInt(item.payload, &index);
    const std::vector<std::string> rules = host_->URLRuleRows();
    if (index >= 0 && index < static_cast<int>(rules.size())) {
      const std::vector<std::string> parts = SplitTabs(rules[index]);
      const std::string old_host =
          parts.size() > 1 ? parts[1] : std::string();
      const std::string old_prefix =
          parts.size() > 2 ? parts[2] : std::string();
      const RuleMatchType current = RuleTypeOf(old_host, old_prefix);
      HMENU menu = ::CreatePopupMenu();
      if (!menu) {
        return;
      }
      const std::array<RuleMatchType, 4> kTypes = {
          RuleMatchType::kDomainSuffix, RuleMatchType::kDomain,
          RuleMatchType::kDomainContains, RuleMatchType::kUrl};
      for (size_t i = 0; i < kTypes.size(); ++i) {
        ::AppendMenuW(menu,
                      MF_STRING | (kTypes[i] == current ? MF_CHECKED : 0),
                      static_cast<UINT>(i) + 1, RuleTypeLabel(kTypes[i]));
      }
      POINT anchor = {item.bounds.left, item.bounds.bottom};
      ::ClientToScreen(hwnd_, &anchor);
      const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
          anchor.y, 0, hwnd_, nullptr));
      ::DestroyMenu(menu);
      if (chosen >= 1 && chosen <= kTypes.size()) {
        // Re-encode the current display value into the new type's form.
        std::string host;
        std::string prefix;
        RuleEncode(kTypes[chosen - 1],
                   RuleDisplayValue(old_host, old_prefix), &host, &prefix);
        if (!host.empty()) {
          host_->SetURLRulePattern(index, host, prefix);
        }
      }
    }
  } else if (item.action == "ruletarget") {
    int index = -1;
    base::StringToInt(item.payload, &index);
    const std::vector<std::string> rules = host_->URLRuleRows();
    if (index >= 0 && index < static_cast<int>(rules.size())) {
      const std::vector<std::string> parts = SplitTabs(rules[index]);
      const std::string current_space =
          parts.size() > 0 ? parts[0] : std::string();
      const bool ask = parts.size() > 3 && parts[3] == "1";
      std::vector<std::string> space_ids;
      HMENU menu = ::CreatePopupMenu();
      if (!menu) {
        return;
      }
      for (const std::string& summary : host_->SpaceSummaries()) {
        const std::vector<std::string> space_parts = SplitTabs(summary);
        if (space_parts.size() <= 3) {
          continue;
        }
        const bool checked = !ask && space_parts[3] == current_space;
        ::AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0),
                      static_cast<UINT>(space_ids.size()) + 1,
                      base::UTF8ToWide(space_parts[0]).c_str());
        space_ids.push_back(space_parts[3]);
      }
      ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      // "Ask every time" rides the rule's target Space as the prompt's
      // suggested default, the Mac's askSpaceTag.
      ::AppendMenuW(menu, MF_STRING | (ask ? MF_CHECKED : 0),
                    static_cast<UINT>(space_ids.size()) + 1,
                    L"Ask every time");
      POINT anchor = {item.bounds.left, item.bounds.bottom};
      ::ClientToScreen(hwnd_, &anchor);
      const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
          menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
          anchor.y, 0, hwnd_, nullptr));
      ::DestroyMenu(menu);
      if (chosen >= 1 && chosen <= space_ids.size()) {
        host_->SetURLRuleTarget(index, space_ids[chosen - 1], false);
      } else if (chosen == space_ids.size() + 1) {
        host_->SetURLRuleTarget(index, current_space, true);
      }
    }
  } else if (item.action == "delrule") {
    int index = -1;
    base::StringToInt(item.payload, &index);
    if (index >= 0) {
      host_->RemoveURLRule(index);
    }
  } else if (item.action == "goprofiles") {
    section_ = "profiles";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "browsersettings") {
    host_->OpenBrowserSettings();
  } else if (item.action == "openpage") {
    host_->OpenBrowserPage(item.payload);
  } else if (item.action == "searchengine") {
    // The Mac's inline picker as a popup: every engine, the default checked.
    const std::vector<std::string> engines =
        host_->SearchEngines(item.payload);
    HMENU menu = ::CreatePopupMenu();
    if (!menu || engines.empty()) {
      if (menu) {
        ::DestroyMenu(menu);
      }
      return;
    }
    std::vector<std::string> keywords;
    for (size_t i = 0; i < engines.size(); ++i) {
      const std::vector<std::string> parts = SplitTabs(engines[i]);
      keywords.push_back(parts[0]);
      const bool is_default = parts.size() > 2 && parts[2] == "1";
      ::AppendMenuW(menu, MF_STRING | (is_default ? MF_CHECKED : 0),
                    static_cast<UINT>(i) + 1,
                    base::UTF8ToWide(parts.size() > 1 ? parts[1] : parts[0])
                        .c_str());
    }
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd_, &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd_, nullptr));
    ::DestroyMenu(menu);
    if (chosen >= 1 && chosen <= keywords.size()) {
      host_->SetSearchEngine(item.payload, keywords[chosen - 1]);
    }
  } else if (item.action == "downloadfolder") {
    const std::wstring folder = PickFolder(
        hwnd_, L"Choose a download location for this profile.");
    if (!folder.empty()) {
      host_->SetDownloadDirectory(item.payload, base::WideToUTF8(folder));
    }
  } else if (item.action == "defaultbrowser") {
    // Windows hands the choice itself to its own Settings page — but only
    // offers a browser that has registered, so the framework registers Phi
    // first and then deep-links to its entry there. The answer changes while
    // that page is up: WM_ACTIVATE re-asks when this window comes back.
    if (host_) {
      host_->RequestDefaultBrowser();
    } else {
      ::ShellExecuteW(nullptr, L"open", L"ms-settings:defaultapps", nullptr,
                      nullptr, SW_SHOWNORMAL);
    }
  } else if (item.action == "signin") {
    if (host_) {
      host_->RequestSignIn();
      Rebuild();  // The hint flips to "finish signing in…".
    }
  } else if (item.action == "signout") {
    if (host_) {
      host_->RequestSignOut();
      Rebuild();
    }
  } else if (item.action == "language") {
    // System default, then every locale the catalog carries, each named in
    // its own language.
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    const std::string current = host_->LanguagePreference();
    ::AppendMenuW(menu, MF_STRING | (current.empty() ? MF_CHECKED : 0), 1,
                  PhiStr("settings.languageSystemDefault"));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const std::vector<std::string>& locales = PhiAvailableLocales();
    for (size_t i = 0; i < locales.size(); ++i) {
      ::AppendMenuW(menu,
                    MF_STRING | (current == locales[i] ? MF_CHECKED : 0),
                    static_cast<UINT>(i) + 2,
                    PhiLocaleDisplayName(locales[i]));
    }
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd_, &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd_, nullptr));
    ::DestroyMenu(menu);
    if (chosen == 1) {
      host_->SetLanguagePreference(std::string());
    } else if (chosen >= 2 && chosen < 2 + locales.size()) {
      host_->SetLanguagePreference(locales[chosen - 2]);
    }
  } else if (item.action == "pinscope") {
    // The Mac's popup: Space / Profile / App.
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    const PhiPinScope current = host_->PinScope();
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiPinScope::kSpace ? MF_CHECKED
                                                              : 0),
                  1, L"Space");
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiPinScope::kProfile ? MF_CHECKED
                                                                : 0),
                  2, L"Profile");
    ::AppendMenuW(menu,
                  MF_STRING | (current == PhiPinScope::kApp ? MF_CHECKED : 0),
                  3, L"App");
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd_, &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd_, nullptr));
    ::DestroyMenu(menu);
    if (chosen >= 1 && chosen <= 3) {
      host_->SetPinScope(chosen == 1   ? PhiPinScope::kSpace
                         : chosen == 2 ? PhiPinScope::kProfile
                                       : PhiPinScope::kApp);
    }
  } else if (item.action == "saturation") {
    const int span = item.bounds.right - item.bounds.left;
    if (span > 0) {
      const int value = (x - item.bounds.left) * 100 / span;
      host_->SetSaturation(value < 0 ? 0 : (value > 100 ? 100 : value));
    }
  } else if (item.action == "spacesat") {
    const int span = item.bounds.right - item.bounds.left;
    if (span > 0) {
      const int value = (x - item.bounds.left) * 100 / span;
      host_->SetSpaceSaturation(item.payload,
                                value < 0 ? 0 : (value > 100 ? 100 : value));
    }
  } else if (item.action == "spacetheme") {
    const std::vector<std::string> parts = SplitTabs(item.payload);
    if (!parts.empty()) {
      host_->SetSpaceTheme(parts[0],
                           parts.size() > 1 ? parts[1] : std::string());
    }
  } else if (item.action == "spaceicon") {
    const std::vector<std::string> parts = SplitTabs(item.payload);
    if (parts.size() > 1) {
      host_->SetSpaceIcon(parts[0], parts[1]);
    }
  } else if (item.action == "selectspace") {
    selected_space_ = item.payload;
  } else if (item.action == "selectprofile") {
    selected_profile_ = item.payload;
  } else if (item.action == "pickertab") {
    picker_tab_ = item.payload;
    picker_scroll_ = 0;
  } else if (item.action == "newspace") {
    std::wstring name;
    if (PhiPromptText(hwnd_, L"New Space", L"Enter a name for the new Space.",
                      &name)) {
      host_->CreateSpaceNamed(base::WideToUTF8(name));
    }
  } else if (item.action == "renspace") {
    std::wstring name;
    for (const std::string& summary : host_->SpaceSummaries()) {
      const std::vector<std::string> parts = SplitTabs(summary);
      if (parts.size() > 3 && parts[3] == selected_space_) {
        name = base::UTF8ToWide(parts[0]);
        break;
      }
    }
    if (PhiPromptText(hwnd_, L"Rename Space",
                      L"Enter a new name for this Space.", &name)) {
      host_->RenameSpace(selected_space_, base::WideToUTF8(name));
    }
  } else if (item.action == "delspace") {
    if (PhiConfirm(hwnd_, L"Delete Space",
                   L"Bookmarks and pinned tabs belonging to this Space will "
                   L"also be removed. This action cannot be undone.",
                   L"Delete") &&
        host_->DeleteSpace(selected_space_)) {
      selected_space_.clear();
    }
  } else if (item.action == "renprofile") {
    std::wstring name;
    for (const std::string& summary : host_->ProfileSummaries()) {
      const std::vector<std::string> parts = SplitTabs(summary);
      if (!parts.empty() && parts[0] == selected_profile_) {
        name = base::UTF8ToWide(parts.size() > 1 ? parts[1] : parts[0]);
        break;
      }
    }
    if (PhiPromptText(hwnd_, L"Rename Profile",
                      L"Enter a new name for this profile.", &name)) {
      host_->RenameProfileById(selected_profile_, base::WideToUTF8(name));
    }
  } else if (item.action == "delprofile") {
    if (PhiConfirm(hwnd_, L"Delete Profile",
                   L"All cookies, history, extensions, and saved data on "
                   L"this profile will be permanently removed. This cannot "
                   L"be undone.",
                   L"Delete") &&
        host_->DeleteProfileById(selected_profile_)) {
      selected_profile_.clear();
    }
  } else if (item.action == "addprofile") {
    std::wstring name;
    if (PhiPromptText(hwnd_, L"New Profile",
                      L"Enter a name for the new profile. Each profile has "
                      L"its own cookies, history, and extensions.",
                      &name)) {
      host_->CreateProfileNamed(base::WideToUTF8(name));
    }
  }
}

void PhiSettingsWindow::OnClick(int x, int y) {
  if (!host_) {
    return;
  }
  POINT pt = {x, y};
  for (const Item& item : items_) {
    if (item.action.empty() || !item.enabled ||
        !::PtInRect(&item.bounds, pt)) {
      continue;
    }
    Apply(item, x);
    ::InvalidateRect(hwnd_, nullptr, TRUE);
    return;
  }
}

// static
LRESULT CALLBACK PhiSettingsWindow::WndProc(HWND hwnd,
                                            UINT msg,
                                            WPARAM wparam,
                                            LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiSettingsWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_NCCALCSIZE:
      // Claim the OS caption for the client (the strip is drawn there);
      // only the thin side/bottom borders stay non-client. Both wparam
      // forms matter — the first calc at creation is the FALSE one.
      if (self) {
        RECT* rect =
            wparam == TRUE
                ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                : reinterpret_cast<RECT*>(lparam);
        const UINT dpi = ::GetDpiForWindow(hwnd);
        const int border_x =
            ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
            ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        const int border_y =
            ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
            ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
        rect->left += border_x;
        rect->right -= border_x;
        rect->bottom -= border_y;
        return 0;
      }
      break;
    case WM_NCHITTEST: {
      if (!self) {
        break;
      }
      const LRESULT hit = ::DefWindowProcW(hwnd, msg, wparam, lparam);
      if (hit != HTCLIENT) {
        return hit;  // The real (non-resizable) frame.
      }
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ::ScreenToClient(hwnd, &pt);
      if (pt.y >= self->Scaled(kCaptionHeight)) {
        return HTCLIENT;
      }
      RECT client = {};
      ::GetClientRect(hwnd, &client);
      return pt.x >= client.right - self->Scaled(kCaptionButtonWidth)
                 ? HTCLOSE
                 : HTCAPTION;
    }
    case WM_NCMOUSEMOVE:
      if (self) {
        const bool hot = wparam == HTCLOSE;
        if (hot != self->close_hot_) {
          self->close_hot_ = hot;
          ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE | TME_NONCLIENT,
                                 hwnd, 0};
        ::TrackMouseEvent(&track);
      }
      break;
    case WM_NCMOUSELEAVE:
      if (self && self->close_hot_) {
        self->close_hot_ = false;
        ::InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    case WM_NCLBUTTONDOWN:
      // Swallow presses on the drawn close button; the action fires on
      // release over it, like the OS one.
      if (self && wparam == HTCLOSE) {
        self->pressed_caption_ = HTCLOSE;
        return 0;
      }
      break;
    case WM_NCLBUTTONUP:
      if (self && self->pressed_caption_ == HTCLOSE) {
        self->pressed_caption_ = 0;
        if (wparam == HTCLOSE) {
          ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
      }
      break;
    case WM_ERASEBKGND:
      // Nothing erases: the pane below paints every pixel of the client area
      // itself. Letting the class brush blank it first is what made scrolling
      // and switching panes flash white.
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps = {};
      HDC dc = ::BeginPaint(hwnd, &ps);
      if (self) {
        // Back-buffered, like the bookmark manager: a pane repaints wholesale
        // on every scroll notch and hover, and drawing that straight to the
        // window shows the ground between the fill and the content.
        RECT rc = {};
        ::GetClientRect(hwnd, &rc);
        const int page_w = rc.right - rc.left;
        const int page_h = rc.bottom - rc.top;
        HDC mem = ::CreateCompatibleDC(dc);
        HBITMAP buffer =
            mem ? ::CreateCompatibleBitmap(dc, page_w, page_h) : nullptr;
        self->Rebuild();
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
        self->OnClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
      }
      return 0;
    case WM_MOUSEMOVE:
      if (self) {
        if (self->close_hot_) {
          self->close_hot_ = false;  // The cursor moved into the client.
          ::InvalidateRect(hwnd, nullptr, FALSE);
        }
        // Dragging a saturation slider.
        if (wparam & MK_LBUTTON) {
          self->OnClick(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        }
      }
      return 0;
    case WM_TIMER:
      if (self && wparam == kProfilePollTimer) {
        // Re-read the pending profile; the rebuild re-arms while pending.
        ::KillTimer(hwnd, kProfilePollTimer);
        ::InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
      }
      return 0;
    case WM_CHAR:
      // The Shortcuts filter: typing anywhere while the pane is up feeds
      // the search field — Esc clears, Backspace deletes.
      if (self && self->section_ == "shortcuts") {
        const wchar_t c = static_cast<wchar_t>(wparam);
        if (c == VK_ESCAPE) {
          self->shortcut_search_.clear();
        } else if (c == VK_BACK) {
          if (!self->shortcut_search_.empty()) {
            self->shortcut_search_.pop_back();
          }
        } else if (c >= L' ') {
          self->shortcut_search_.push_back(c);
        } else {
          return 0;
        }
        self->scroll_ = 0;
        ::InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
      }
      return 0;
    case WM_MOUSEWHEEL:
      // Pane content can outgrow the fixed window; the Mac scrolls, so do we.
      if (self) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ::ScreenToClient(hwnd, &pt);
        if (self->picker_max_line_ > 0 &&
            ::PtInRect(&self->picker_grid_, pt)) {
          // Over the icon picker: three cell rows per notch, in place.
          const int rows = std::max(1, 3 * std::abs(delta) / WHEEL_DELTA);
          self->picker_scroll_ = std::max(
              0, std::min(self->picker_max_line_,
                          self->picker_scroll_ + (delta > 0 ? -rows : rows)));
          ::InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
        RECT client = {};
        ::GetClientRect(hwnd, &client);
        const int visible = client.bottom - self->Scaled(kChromeTop);
        const int max_scroll =
            std::max(0, self->content_height_ - visible);
        self->scroll_ = std::max(
            0, std::min(max_scroll, self->scroll_ - delta / 2));
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    case WM_DPICHANGED:
      if (self) {
        if (const UINT dpi = ::GetDpiForWindow(hwnd)) {
          self->dpi_ = dpi;
        }
        self->EnsureFont();
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested) {
          ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;
    case WM_ACTIVATE:
      // The "Set as default" button hands the choice to the Windows Settings
      // app, so the answer changes while this window is in the background.
      // Coming back to the front is the cue to ask again — the Mac re-checks
      // in viewWillAppear for the same reason. Falls through to
      // DefWindowProc, which the custom caption still needs.
      if (self && LOWORD(wparam) != WA_INACTIVE) {
        self->is_default_browser_.reset();
        ::InvalidateRect(hwnd, nullptr, TRUE);
      }
      break;
    case WM_CLOSE:
      ::ShowWindow(hwnd, SW_HIDE);  // Keep it around; reopening is instant.
      return 0;
  }
  return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace phi_client
