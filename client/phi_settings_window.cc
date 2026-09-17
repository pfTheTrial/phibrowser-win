// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_settings_window.h"

#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

// shlwapi.h (for AssocQueryStringW, the default-browser probe) defines
// StrCat as a macro; base/strings/strcat.h, reached through the preferences
// service's store, declares a function of that name and re-#defines it.
// Drop the Windows spelling — nothing here calls it.
#undef StrCat

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "phibrowser-win/client/phi_account_feedback.h"
#include "phibrowser-win/client/phi_account_profile.h"
#include "phibrowser-win/client/phi_agent_icons.h"
#include "phibrowser-win/client/phi_client_analytics.h"
#include "phibrowser-win/client/phi_emoji.h"
#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_fluent.h"
#include "phibrowser-win/client/phi_version.h"
#include "phibrowser-win/client/phi_updater.h"
#include "phibrowser-win/client/phi_sidebar_view.h"
#include "phibrowser-win/client/phi_glyph_bitmap.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_preferences.h"
#include "phibrowser-win/client/phi_profile_service.h"
#include "phibrowser-win/client/phi_prompt.h"
#include "phibrowser-win/client/phi_skill_installer.h"
#include "phibrowser-win/client/phi_space_manager.h"
#include "phibrowser-win/client/phi_url_router.h"

namespace phi_client {

namespace {

constexpr wchar_t kClassName[] = L"PhiSettingsWindow";

}  // namespace

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
bool PhiThisExeIsDefaultBrowser() {
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

namespace {

// The Mac window: content is a FIXED 680x561 in every pane
// (each pane pins view.snp to that size); the toolbar rides above it.
// Windows Settings (docs/windows-native-ui.md §7): resizable, 1120 × 760
// by default, never under 760 × 560; a 280 navigation rail (48 under 900
// wide) and a page column 40 in from it.
constexpr int kWidth = 1120;
constexpr int kHeight = 760;
constexpr int kMinWidth = 760;
constexpr int kMinHeight = 560;
constexpr int kRailWidth = 280;
constexpr int kRailNarrow = 48;
constexpr int kRailBreakpoint = 900;
constexpr int kRailItemHeight = 36;
constexpr int kContentPad = 40;
constexpr int kContentMaxWidth = 1000;
constexpr int kPageTitleHeight = 36;   // 28 dip.
constexpr int kPageTitleGap = 20;
// The custom caption strip: the OS caption is retired (WM_NCCALCSIZE) so
// the pane title can sit CENTERED with the Phi mark leading — the Mac
// window's look. Both the strip and its close button (Win11's 46dip
// metric) are PhiPopupChrome's now; kCaptionHeight is what this file
// still needs to know for kChromeTop.
constexpr int kCaptionHeight = kPhiCaptionHeightDip;
constexpr int kToolbarHeight = 0;  // The rail replaced the toolbar.
// Everything below the caption strip and the pane toolbar.
constexpr int kChromeTop = kCaptionHeight + kToolbarHeight;
// Page insets 36 on every side; 24 between sections; 16 title -> card.
constexpr int kSectionGap = 24;
// Posted by the profile card's decode worker when a frame worth repainting
// for has landed.
constexpr UINT kMsgProfileCardFrame = WM_APP + 0x3A1;
constexpr int kTitleToCard = 8;
constexpr int kCardPadding = 12;   // Inner horizontal padding of a card.
constexpr int kRowPadding = 14;    // Vertical padding around a row's content.
constexpr int kSectionTitleHeight = 20;  // 14/600 group headers.
constexpr int kHintHeight = 16;          // 12 dip descriptions.
constexpr int kRowLabelHeight = 19;      // 14 dip labels.
constexpr int kCardRadius = 4;
constexpr int kRadius = 4;
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
constexpr int kSliderGeneral = 200;
constexpr int kSliderSpaces = 200;  // themeSliderWidth in the Spaces detail.
constexpr int kSliderKnob = 18;
// Layout / appearance / new-tab picture cards: 122x72 image, r5, 2pt accent
// ring when selected, 11pt caption 10 below, 16 apart (GeneralSttingCardView).
constexpr int kImageCardW = 122;
constexpr int kImageCardH = 72;
constexpr int kImageCardGap = 16;
constexpr int kImageCardBlock = 72 + 10 + 15;
// Switches: the Mac's .mini switch is about 26x15.
constexpr int kToggleWidth = 40;
constexpr int kToggleHeight = 20;
// Expander cards (windows-native-ui.md §7.1 / §7.2): a 56 dip header
// row, 8 between cards, and the inline icon picker's four lines.
constexpr int kExpanderHeight = 56;
constexpr int kExpanderGap = 8;
constexpr int kPickerLines = 4;
// The rail's "Find a setting" field (§7): 32 high under the caption.
constexpr int kRailSearchHeight = 32;
constexpr int kAboutMark = 48;
// The gap between the General pane's single-profile detail cards.
constexpr int kDetailCardGap = 16;
// The Profiles trailing pill: inner width 110, H10/V5, primary@0.06, r6.
constexpr int kPillWidth = 160;
constexpr int kPillHeight = 32;
// The Mac's SettingsIconChip: a 24pt rounded square (r6) filled with the
// colour's gradient, carrying a 12pt semibold white glyph, 12pt from the
// text it leads — so a chip-led row's text column starts 36 in, and the
// rows grouped under such a header are indented to match.
constexpr int kChipSize = 24;
constexpr int kChipRadius = 6;
constexpr int kChipGap = 12;
constexpr int kChipIndent = kChipSize + kChipGap;
// Apple's system colours, which the Mac pane names directly (.indigo,
// .orange, .blue, .teal).
constexpr COLORREF kChipIndigo = RGB(0x58, 0x56, 0xD6);
constexpr COLORREF kChipOrange = RGB(0xFF, 0x95, 0x00);
constexpr COLORREF kChipBlue = RGB(0x00, 0x7A, 0xFF);
constexpr COLORREF kChipTeal = RGB(0x30, 0xB0, 0xC7);
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
// The pages (windows-native-ui.md §7), in rail order. The ids are the
// remote surface's (`account` stays `account` — impl log D19).
constexpr std::array<SectionDef, 11> kSections = {{
    {"account", L"You", 0xE77B},          // Contact
    {"general", L"General", 0xE713},      // Settings
    {"appearance", L"Appearance", 0xE790},  // Color
    {"spaces", L"Spaces", 0xE8A9},        // ViewAll
    {"profiles", L"Profiles", 0xE716},    // People
    {"navigation", L"Navigation", 0xE71B},  // Link
    {"ai", L"Phi & AI", 0xE734},          // FavoriteStar
    {"shortcuts", L"Shortcuts", 0xE765},  // Keyboard
    {"advanced", L"Advanced", 0xE9F5},    // Processing
    {"developer", L"Developer", 0xEC7A},  // DeveloperTools
    {"about", L"About", 0xE946},          // Info
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
// theme overlay's HSB saturation 0.1 -> 0.5 across the bar (and, for Pure,
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

}  // namespace

PhiSettingsWindow::~PhiSettingsWindow() {
  host_->RemoveSettingsObserver(this);
  Destroy();  // The base clears GWLP_USERDATA at WM_NCDESTROY.
  if (measure_dc_) {
    ::DeleteDC(measure_dc_);
    measure_dc_ = nullptr;
  }
  for (HFONT font :
       {font_, title_font_, small_font_, hint_font_, emoji_font_, mdl2_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  for (const auto& [size, font] : icon_fonts_) {
    (void)size;
    ::DeleteObject(font);
  }
}

PhiSettingsWindow::PhiSettingsWindow(PhiSettingsHost* host)
    : host_(host),
      prefs_(&host->Preferences()),
      spaces_(&host->SpaceList()),
      profiles_(&host->ProfileList()),
      account_(&host->Account()) {
  // Subscribed for the life of the object (the destructor unsubscribes),
  // whether or not the window below comes up.
  host_->AddSettingsObserver(this);
}

bool PhiSettingsWindow::OpenWindow() {
  ClassSpec spec;
  spec.name = kClassName;
  spec.style = CS_HREDRAW | CS_VREDRAW;
  // Phi's own icon in the taskbar and alt-tab — not the stock one.
  spec.icon = PhiAppIcon(32);
  spec.icon_small = PhiAppIcon(16);

  Options options;
  // A Windows Settings window: min / max / close, resizable (§7).
  options.caption_buttons =
      kPhiCaptionClose | kPhiCaptionMinimize | kPhiCaptionMaximize;
  options.dim_when_inactive = false;
  options.dialog_keys = kPhiDialogNone;  // Every control here is drawn.

  const RECT bounds = {CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT + 100,
                       CW_USEDEFAULT + 100};
  if (!CreateSecondary(spec, 0,
                       WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU |
                           WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
                       bounds, nullptr, L"Settings", options)) {
    LOG(ERROR) << "[PhiClient] settings CreateWindowEx failed err="
               << ::GetLastError();
    return false;
  }
  profile_poll_ = NewTimer();
  card_timer_ = NewTimer();
  profile_card_.SetNotify(hwnd(), kMsgProfileCardFrame);
  // Content 680x561 under the caption strip and toolbar band — fixed,
  // non-resizable. WM_NCCALCSIZE keeps only the thin side/bottom borders,
  // so the window size is the content plus exactly those.
  const int border_x = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi()) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi());
  const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi()) +
                       ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi());
  // The last size the user left it at (dip), else the §7 default.
  int width = kWidth;
  int height = kHeight;
  if (prefs_->SettingsWindowSize(&width, &height)) {
    width = std::max(kMinWidth, width);
    height = std::max(kMinHeight, height);
  }
  ::SetWindowPos(hwnd(), nullptr, 0, 0, Scaled(width) + 2 * border_x,
                 Scaled(kChromeTop + height) + border_y,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_FRAMECHANGED);
  EnsureFont();
  SyncTitle();
  return true;
}

void PhiSettingsWindow::EnsureFont() {
  if (font_ && font_dpi_ == dpi()) {
    return;
  }
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                    &metrics, 0, dpi())) {
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
  // The §2.2 ramp: body 14, group headers 14/600, descriptions 12, the
  // page title 28/600 (Segoe UI Variable through PhiFluentFont).
  swap(&font_, PhiFluentFont(dpi(), 14));
  swap(&title_font_, PhiFluentFont(dpi(), 28, FW_SEMIBOLD));
  swap(&small_font_, PhiFluentFont(dpi(), 14, FW_SEMIBOLD));
  swap(&hint_font_, PhiFluentFont(dpi(), 12));
  (void)make;
  LOGFONTW emoji = metrics.lfMessageFont;
  const std::wstring face = L"Segoe UI Emoji";
  std::fill(std::begin(emoji.lfFaceName), std::end(emoji.lfFaceName), L'\0');
  std::copy_n(face.begin(),
              std::min(face.size(), std::size(emoji.lfFaceName) - 1),
              std::begin(emoji.lfFaceName));
  emoji.lfHeight = -Scaled(14);
  swap(&emoji_font_, ::CreateFontIndirectW(&emoji));
  LOGFONTW mdl2 = metrics.lfMessageFont;
  const std::wstring mdl2_face = PhiIconFaceName();
  std::fill(std::begin(mdl2.lfFaceName), std::end(mdl2.lfFaceName), L'\0');
  std::copy_n(mdl2_face.begin(),
              std::min(mdl2_face.size(), std::size(mdl2.lfFaceName) - 1),
              std::begin(mdl2.lfFaceName));
  mdl2.lfHeight = -Scaled(14);
  swap(&mdl2_font_, ::CreateFontIndirectW(&mdl2));
  // The caption glyph font is the frame's now (PhiPopupGlyphFont).
  for (const auto& [size, cached] : icon_fonts_) {
    (void)size;
    ::DeleteObject(cached);
  }
  icon_fonts_.clear();
  font_dpi_ = dpi();
}

HFONT PhiSettingsWindow::IconFont(int size) {
  for (const auto& [cached_size, font] : icon_fonts_) {
    if (cached_size == size) {
      return font;
    }
  }
  LOGFONTW lf = {};
  if (!mdl2_font_ || !::GetObjectW(mdl2_font_, sizeof(lf), &lf)) {
    return mdl2_font_;
  }
  lf.lfHeight = -Scaled(size);
  HFONT built = ::CreateFontIndirectW(&lf);
  if (!built) {
    return mdl2_font_;
  }
  icon_fonts_.emplace_back(size, built);
  return built;
}

void PhiSettingsWindow::SyncTitle() {
  // Windows Settings: the window is "Settings"; the page title is in the
  // page (§7).
  ::SetWindowTextW(hwnd(), L"Settings");
}

// static
void PhiSettingsWindow::Open(PhiSettingsHost* host) {
  if (!g_instance || !g_instance->alive()) {
    delete g_instance;
    g_instance = new PhiSettingsWindow(host);
    if (!g_instance->OpenWindow()) {
      delete g_instance;
      g_instance = nullptr;
    }
  }
  if (g_instance) {
    ::ShowWindow(g_instance->hwnd(), SW_SHOW);
    ::SetForegroundWindow(g_instance->hwnd());
    g_instance->ScheduleRebuild();
  }
}

// static
HWND PhiSettingsWindow::Handle() {
  return g_instance && g_instance->alive() ? g_instance->hwnd() : nullptr;
}

// static
void PhiSettingsWindow::Refresh() {
  if (Handle()) {
    g_instance->ScheduleRebuild();
  }
}

void PhiSettingsWindow::OnSettingsChanged(PhiSettingsChange /*what*/) {
  // Every pane reads the whole host, so one rebuild covers any domain.
  // This is THE reason the rebuild could come off the paint path: phase
  // 4 gave the window a real change signal, so WM_PAINT no longer has to
  // assume the model moved under it.
  if (alive()) {
    ScheduleRebuild();
  }
}

void PhiSettingsWindow::ScheduleRebuild() {
  if (!alive()) {
    return;
  }
  Rebuild();
  Invalidate();
  // ClearHits() drops the base's hover index, so a rebuild fired by a click
  // would leave the control still under the pointer un-lit until the mouse
  // moved. Re-run the hover through THE path — the wndproc — rather than a
  // copy of it. Never during a slider drag: that move handler rebuilds, and
  // feeding it its own message would not terminate.
  POINT pt = {};
  if (!dragging_ && ::GetCursorPos(&pt) && ::WindowFromPoint(pt) == hwnd()) {
    ::ScreenToClient(hwnd(), &pt);
    ::SendMessageW(hwnd(), WM_MOUSEMOVE, 0,
                   MAKELPARAM(static_cast<WORD>(pt.x),
                              static_cast<WORD>(pt.y)));
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
    if (name == "developer" && g_instance &&
        !g_instance->prefs_->DeveloperMode()) {
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
  g_instance->picker_open_ = space_id;
  g_instance->rail_search_.clear();
  g_instance->search_filter_.clear();
  g_instance->scroll_ = 0;
  g_instance->SyncTitle();
  Refresh();
}

// --- Section plumbing --------------------------------------------------------

void PhiSettingsWindow::BeginSection(int* y, const wchar_t* title) {
  // Where the section began, heading included, so a filter that keeps
  // none of its rows can take the whole thing back out.
  if (heading_pending_) {
    section_start_ = heading_start_;
    section_start_y_ = heading_start_y_;
    heading_pending_ = false;
  } else {
    section_start_ = items_.size();
    section_start_y_ = *y;
  }
  if (title && title[0]) {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = title;
    heading.bounds = {content_left_, *y, content_right_,
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    *y += Scaled(kSectionTitleHeight) + Scaled(kTitleToCard);
  }
  // The card is emitted FIRST so it paints behind its rows; EndSection sizes
  // it around whatever was added. The card OPENS at `*y` and CLOSES at the
  // `*y` a row leaves behind, so its padding is kRowPadding on both edges —
  // it used to open four pixels above its first row and close sixteen below
  // its last, which made every card top-tight and bottom-loose and left a
  // card whose only line was a hint (Developer's two empty lists) with the
  // text riding its top border.
  Item card;
  card.kind = ItemKind::kCard;
  card.bounds = {content_left_, *y, content_right_, 0};
  items_.push_back(std::move(card));
  open_card_ = items_.size() - 1;
  card_rows_ = 0;
  *y += Scaled(kRowPadding);
}

void PhiSettingsWindow::EndSection(int* y) {
  if (!search_filter_.empty()) {
    if (card_rows_ == 0) {
      // The filter kept nothing here: the card, its title and heading go.
      items_.resize(std::min(items_.size(), section_start_));
      *y = section_start_y_;
      card_rows_ = 0;
      return;
    }
    // A divider that ended up trailing the last kept row — behind the
    // empty-rect controls of the rows the filter dropped after it.
    while (items_.size() > open_card_ + 1) {
      const Item& last = items_.back();
      const bool empty = last.bounds.bottom == 0 && last.bounds.top == 0;
      if (!empty && last.kind != ItemKind::kDivider) {
        break;
      }
      items_.pop_back();
    }
  }
  items_[open_card_].bounds.bottom = *y;
  card_rows_ = 0;
  *y += Scaled(kSectionGap);
}

bool PhiSettingsWindow::SearchMatches(const wchar_t* title,
                                      const wchar_t* hint) const {
  if (search_filter_.empty()) {
    return true;
  }
  const auto contains = [this](const wchar_t* text) {
    if (!text || !*text) {
      return false;
    }
    std::wstring folded(text);
    for (wchar_t& c : folded) {
      c = static_cast<wchar_t>(::towlower(c));
    }
    return folded.find(search_filter_) != std::wstring::npos;
  };
  return contains(title) || contains(hint);
}

int PhiSettingsWindow::WrappedHeight(HFONT font, const wchar_t* text,
                                     int width) const {
  if (!font || !text || !*text || width <= 0) {
    return 0;
  }
  if (!measure_dc_) {
    measure_dc_ = ::CreateCompatibleDC(nullptr);
    if (!measure_dc_) {
      return 0;
    }
  }
  HGDIOBJ previous = ::SelectObject(measure_dc_, font);
  RECT box = {0, 0, width, 0};
  ::DrawTextW(measure_dc_, text, -1, &box,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  ::SelectObject(measure_dc_, previous);
  return box.bottom - box.top;
}

void PhiSettingsWindow::AddHint(int* y, const wchar_t* text) {
  if (!search_filter_.empty()) {
    return;  // A page's loose captions have no row to hang from here.
  }
  const int left = (content_left_ + Scaled(kCardPadding));
  const int right = (content_right_ - Scaled(kCardPadding));
  const int height = std::max(Scaled(kHintHeight),
                              WrappedHeight(hint_font_, text, right - left));
  Item hint;
  hint.kind = ItemKind::kHint;
  hint.text = text;
  hint.wrap = true;
  hint.bounds = {left, *y, right, *y + height};
  items_.push_back(std::move(hint));
  *y += height + Scaled(6);
}

RECT PhiSettingsWindow::AddRow(int* y, const wchar_t* title,
                               int control_height, int control_width,
                               const wchar_t* hint, int text_indent,
                               wchar_t chip_glyph, COLORREF chip_color,
                               int row_padding) {
  // The rail search: a row whose title and description carry none of the
  // typed text is not built at all. Its caller's control then lands on an
  // empty rect, which neither paints (it clips out under the caption) nor
  // publishes a hit.
  last_row_skipped_ = !SearchMatches(title, hint);
  if (last_row_skipped_) {
    return RECT{};
  }
  ++filter_hits_;
  const int padding = row_padding < 0 ? kRowPadding : row_padding;
  const int gutter = (content_left_ + Scaled(kCardPadding));
  const int left = gutter + Scaled(text_indent);
  const int right = (content_right_ - Scaled(kCardPadding));
  const int text_right = right - Scaled(control_width) - Scaled(12);
  const int text_width = std::max(1, text_right - left);
  // The Mac's row is ONE VStack(spacing: 4): the 13pt title over its 11pt
  // description, both wrapping, with the trailing control centred on the
  // pair. Measuring here is what stops a long line being chopped with an
  // ellipsis ("…so each profile manages its ow…").
  const int title_height = std::max(
      Scaled(kRowLabelHeight), WrappedHeight(font_, title, text_width));
  const int hint_height =
      hint && *hint ? WrappedHeight(hint_font_, hint, text_width) : 0;
  const int text_height =
      title_height + (hint_height > 0 ? Scaled(4) + hint_height : 0);
  const int row_height = std::max(text_height, Scaled(control_height));
  const int text_top = *y + (row_height - text_height) / 2;
  if (chip_glyph) {
    // HStack(alignment: .top): the chip's top meets the title's, so it
    // hangs a little below the first line rather than centring on a block
    // whose height the description decides.
    Item chip;
    chip.kind = ItemKind::kIconChip;
    chip.text = std::wstring(1, chip_glyph);
    chip.swatch = chip_color;
    chip.bounds = {gutter, text_top, gutter + Scaled(kChipSize),
                   text_top + Scaled(kChipSize)};
    items_.push_back(std::move(chip));
  }
  Item label;
  label.kind = ItemKind::kRowLabel;
  label.text = title;
  label.wrap = title_height > Scaled(kRowLabelHeight);
  label.bounds = {left, text_top, text_right, text_top + title_height};
  items_.push_back(std::move(label));
  if (hint_height > 0) {
    Item note;
    note.kind = ItemKind::kHint;
    note.text = hint;
    note.wrap = true;
    note.bounds = {left, text_top + title_height + Scaled(4), text_right,
                   text_top + text_height};
    items_.push_back(std::move(note));
  }
  // The control is TRAILING, as on Mac (label, Spacer, accessory).
  RECT control = {right - Scaled(control_width),
                  *y + (row_height - Scaled(control_height)) / 2, right,
                  *y + (row_height + Scaled(control_height)) / 2};
  *y += row_height + Scaled(padding);
  ++card_rows_;
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
  BeginSection(y, PhiStr("settings.general.language.sectionTitle"));
  {
    // The row was a dead chip until the string catalog existed. Now it is the
    // language picker: "System default" leads because it is the default and
    // the only choice that follows a later OS change, and the rest are
    // endonyms, so someone hunting for their own language does not first have
    // to read the language the app happens to be in.
    const RECT area = AddRow(y, PhiStr("settings.displayLanguage"), kPillHeight,
                             kPillWidth,
                             PhiStr("settings.languageHint"));
    Item pill;
    pill.kind = ItemKind::kPopup;
    pill.text = prefs_->LanguagePreference().empty()
                    ? PhiStr("settings.languageSystemDefault")
                    : PhiLocaleDisplayName(prefs_->LanguagePreference());
    pill.action = "language";
    pill.bounds = area;
    items_.push_back(std::move(pill));
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
      card.selected = prefs_->new_tab_behavior() == definition.value;
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
  AddToggleRow(y, L"Restore last session", prefs_->RestoreLastSession(),
               /*enabled=*/true, "restoresession",
               prefs_->RestoreLastSession()
                   ? L"Reopen your windows and tabs the next time you open "
                     L"Phi."
                   : L"Phi starts with a new window. Closing a window may "
                     L"sign you out of some sites.");
  AddDividerRow(y);
  {
    // Telemetry consent (docs/architecture-review.md §3.9): the ONLY switch
    // for the analytics pipeline. Off by default, and deliberately out of
    // reach of agentSpace.settings.set. Windows-only, so it stays here
    // while the Mac's other browsing rows moved to Advanced.
    AddToggleRow(y, PhiStr("settings.general.analytics.toggle"),
                 prefs_->AnalyticsEnabled(), /*enabled=*/true, "analytics",
                 PhiStr("settings.general.analytics.hint"));
  }
  EndSection(y);

  // Profile: with several profiles just the jump link; with one, the Mac
  // INLINES the profile detail (search engine, downloads, data links).
  if (profiles_->Profiles().size() > 1) {
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
  } else if (search_filter_.empty()) {
    // (Its detail rows are not filterable: the results page skips them.)
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = L"Profile";
    heading.bounds = {content_left_, *y, content_right_,
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    *y += Scaled(kSectionTitleHeight) + Scaled(kTitleToCard);
    const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
    const std::string profile_id =
        profiles.empty() ? std::string() : profiles.front().id;
    BuildProfileDetail(y, content_left_, content_right_,
                       profile_id);
    *y += Scaled(kSectionGap) - Scaled(kDetailCardGap);
  }
}

// The Mac's AdvancedSettingsView (6d1f7b12): the browsing rows and the
// Developer-mode switch that used to sit in General, as four untitled
// cards — the page rows, the browsing rows, Developer mode, then the
// browser-settings link.
void PhiSettingsWindow::BuildAppearance(int* y) {
  const auto& themes = PhiBuiltInThemes();
  const bool locked = spaces_->Summaries().size() > 1;

  BeginSection(y, L"Layout");
  {
    // "Layout mode": three picture cards, in the Mac's allCases order —
    // Balanced, Performance, Comfortable.
    // Two layouts (impl log D9), Edge's words.
    const RECT area = AddRow(y, L"Tabs", kImageCardBlock,
                             2 * kImageCardW + kImageCardGap);
    const struct {
      const wchar_t* title;
      PhiLayoutMode mode;
      const char* sketch;
    } kCards[] = {
        {L"Horizontal tabs", PhiLayoutMode::kComfortable, "comfortable"},
        {L"Vertical tabs", PhiLayoutMode::kBalanced, "balanced"},
    };
    int i = 0;
    for (const auto& definition : kCards) {
      Item card;
      card.kind = ItemKind::kImageCard;
      card.text = definition.title;
      card.payload = definition.sketch;
      card.selected = prefs_->LayoutMode() == definition.mode;
      card.value = static_cast<int>(definition.mode);
      card.action = "layout";
      const int x = area.left + i * Scaled(kImageCardW + kImageCardGap);
      card.bounds = {x, area.top, x + Scaled(kImageCardW),
                     area.top + Scaled(kImageCardBlock)};
      items_.push_back(std::move(card));
      ++i;
    }
  }
  EndSection(y);

  BeginSection(y, L"Appearance");
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
      card.selected = prefs_->Appearance() == definition.value;
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

  BeginSection(y, L"Space color");
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
      swatch.selected = prefs_->ThemeId() == themes[i].id;
      swatch.enabled = !locked;
      const PhiPalette resolved = PhiResolvePalette(
          themes[i], prefs_->Appearance(), prefs_->Saturation());
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
    slider.value = prefs_->Saturation();
    slider.enabled = !locked;
    slider.action = "saturation";
    // The track ramps the SELECTED theme's overlay colour (Pure ramps
    // brightness instead — flagged via `on`).
    const PhiTheme& selected = PhiThemeById(prefs_->ThemeId());
    slider.swatch = prefs_->Palette().dark ? selected.dark.overlay
                                          : selected.light.overlay;
    slider.on = prefs_->ThemeId() == "pure";
    slider.bounds = area;
    items_.push_back(std::move(slider));
  }
  if (locked) {
    AddDividerRow(y);
    // The Mac's exact lock text, with the jump link trailing.
    const RECT area = AddRow(y,
                             L"You have more than one Space, so each Space "
                             L"sets its own color and opacity.",
                             kRowLabelHeight, 220);
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


  BeginSection(y, L"Tabs");
  AddToggleRow(y, L"Show a preview card when hovering over a tab",
               prefs_->ShowTabPreviews(),
               /*enabled=*/true, "tabpreviews");
  AddDividerRow(y);
  AddToggleRow(y, L"Show open tab indicators",
               prefs_->ShowOpenTabIndicators(),
               /*enabled=*/true, "openindicators",
               L"Show a dot on inactive pinned tabs and bookmarks that are "
               L"currently open.");
  AddDividerRow(y);
  AddToggleRow(y, L"Dim unloaded tab icons", prefs_->DimUnloadedTabIcons(),
               /*enabled=*/true, "dimunloaded",
               L"Dim tab icons when their pages are unloaded from memory.");
  AddDividerRow(y);
  AddToggleRow(y, L"Always show full URL", prefs_->AlwaysShowFullUrl(),
               /*enabled=*/true, "fullurl");
  EndSection(y);
}

void PhiSettingsWindow::BuildAbout(int* y) {
  BeginSection(y, nullptr);
  {
    // The brand mark at 48 leading the name and version (§7 About).
    const std::wstring version = std::wstring(L"Version ") + PHI_WIN_VERSION_WIDE;
    RECT area = AddRow(y, L"Phi Browser", kAboutMark, 160, version.c_str(),
                       kAboutMark + 16);
    if (area.right > area.left) {
      Item mark;
      mark.kind = ItemKind::kAppMark;
      mark.bounds = {content_left_ + Scaled(kCardPadding), area.top,
                     content_left_ + Scaled(kCardPadding) + Scaled(kAboutMark),
                     area.top + Scaled(kAboutMark)};
      items_.push_back(std::move(mark));
      const int mid = (area.top + area.bottom) / 2;
      area.top = mid - Scaled(kPillHeight) / 2;
      area.bottom = area.top + Scaled(kPillHeight);
      Item button;
      button.kind = ItemKind::kButton;
      button.text = L"Check for update";
      button.enabled = PhiUpdater::Available();
      button.action = "checkupdate";
      button.bounds = area;
      items_.push_back(std::move(button));
    }

  }
  EndSection(y);

  BeginSection(y, nullptr);
  {
    const size_t first = items_.size();
    AddRow(y, L"What's new", kRowLabelHeight, 20);
    for (size_t i = first; i < items_.size(); ++i) {
      items_[i].enabled = false;
    }
  }
  AddDividerRow(y);
  {
    const size_t first = items_.size();
    AddRow(y, L"Documentation", kRowLabelHeight, 20);
    for (size_t i = first; i < items_.size(); ++i) {
      items_[i].enabled = false;
    }
  }
  EndSection(y);
  AddHint(y, L"Report an issue from the app menu's Help.");
}

void PhiSettingsWindow::BuildAdvanced(int* y) {
  BeginSection(y, nullptr);
  {
    // "Copy Link to Highlight" has no Windows counterpart yet: the row is a
    // placeholder, like Auto picture-in-picture below it.
    const size_t first = items_.size();
    AddToggleRow(y, L"Shorten links to selected text", /*on=*/true,
                 /*enabled=*/false, std::string(),
                 L"Applies to \u201CCopy Link to Highlight\u201D in the "
                 L"right-click menu. Turn off to copy the full link.");
    for (size_t i = first; i < items_.size(); ++i) {
      items_[i].enabled = false;
    }
  }
  AddDividerRow(y);
  {
    const size_t first = items_.size();
    const RECT area = AddRow(
        y, L"Auto picture-in-picture", kPillHeight, kPillWidth,
        L"Pop out playing video when you switch tabs or apps");
    for (size_t i = first; i < items_.size(); ++i) {
      items_[i].enabled = false;  // The row is a placeholder, like its pill.
    }
    Item pill;
    pill.kind = ItemKind::kPopup;
    pill.text = L"Normal";
    pill.enabled = false;
    pill.bounds = area;
    items_.push_back(std::move(pill));
  }
  EndSection(y);

  BeginSection(y, nullptr);
  // The description belongs TO the row (the Mac stacks title and
  // description in one VStack); a divider between them fenced the sentence
  // off from the switch it explains.
  AddToggleRow(y, L"Developer mode", prefs_->DeveloperMode(),
               /*enabled=*/true, "devmode",
               prefs_->DeveloperMode()
                   ? L"Agent access, permissions, and the password manager "
                     L"live in the Developer tab."
                   : L"Turning developer mode off turns off agent access "
                     L"and the agent password manager, and revokes every "
                     L"allowed agent and credential approval. An agent "
                     L"that connects later can ask you to turn it back on.");
  EndSection(y);

  BeginSection(y, nullptr);
  {
    // Chevron.right, 12pt semibold textSecondary on Mac — a kIcon glyph.
    // The whole row opens chrome://settings, as on Mac — emitted FIRST so
    // its hover wash paints behind the label and the chevron.
    Item row;
    row.kind = ItemKind::kListRow;
    row.action = "browsersettings";
    items_.push_back(std::move(row));
    const size_t row_index = items_.size() - 1;
    const RECT area = AddRow(y, L"Additional browser settings",
                             kRowLabelHeight, 20);
    Item chevron;
    chevron.kind = ItemKind::kIcon;
    chevron.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
    items_[row_index].bounds = {content_left_ + Scaled(4),
                                area.top - Scaled(6),
                                content_right_ - Scaled(4),
                                area.bottom + Scaled(6)};
  }
  EndSection(y);
}

void PhiSettingsWindow::AddDividerRow(int* y) {
  if (!search_filter_.empty() &&
      (last_row_skipped_ || card_rows_ == 0 ||
       (!items_.empty() && items_.back().kind == ItemKind::kDivider))) {
    return;  // Nothing above it to separate.
  }
  Item divider;
  divider.kind = ItemKind::kDivider;
  divider.bounds = {(content_left_ + Scaled(kCardPadding)), *y - Scaled(6),
                    (content_right_ - Scaled(kCardPadding)),
                    *y - Scaled(6) + 1};
  items_.push_back(std::move(divider));
  *y += Scaled(2);
}

void PhiSettingsWindow::AddSubHint(int* y, const wchar_t* text) {
  if (!search_filter_.empty() && last_row_skipped_) {
    return;  // Its row was filtered out.
  }
  const int left = (content_left_ + Scaled(kCardPadding));
  const int right = (content_right_ - Scaled(kCardPadding));
  // Tucked under the row above it at the Mac's 4pt VStack spacing — or,
  // when it IS the card's first line (Developer's empty grant lists), on
  // the card's own padding. The old unconditional pull-up put that case
  // four pixels ABOVE the card's top edge.
  const int top =
      card_rows_ > 0 ? *y - Scaled(kRowPadding) + Scaled(4) : *y;
  const int height = std::max(Scaled(kHintHeight),
                              WrappedHeight(hint_font_, text, right - left));
  Item hint;
  hint.kind = ItemKind::kHint;
  hint.text = text;
  hint.wrap = true;
  hint.bounds = {left, top, right, top + height};
  items_.push_back(std::move(hint));
  *y = top + height + Scaled(kRowPadding);
  ++card_rows_;
}

void PhiSettingsWindow::AddSectionHeading(int* y, const wchar_t* title,
                                          const wchar_t* description) {
  heading_pending_ = true;
  heading_start_ = items_.size();
  heading_start_y_ = *y;
  Item heading;
  heading.kind = ItemKind::kSectionTitle;
  heading.text = title;
  heading.bounds = {content_left_, *y, content_right_,
                    *y + Scaled(kSectionTitleHeight)};
  items_.push_back(std::move(heading));
  *y += Scaled(kSectionTitleHeight) + Scaled(4);
  const int width = content_right_ - content_left_;
  const int height = std::max(
      Scaled(kHintHeight), WrappedHeight(hint_font_, description, width));
  Item subtitle;
  subtitle.kind = ItemKind::kSubtitle;
  subtitle.text = description;
  subtitle.wrap = true;
  subtitle.bounds = {content_left_, *y, content_right_,
                     *y + height};
  items_.push_back(std::move(subtitle));
  // Leaves `y` where BeginSection(y, nullptr) expects it: the card follows at
  // the same title-to-card gap a titled section uses.
  *y += height + Scaled(kTitleToCard);
}

void PhiSettingsWindow::AddToggleRow(int* y, const wchar_t* title, bool on,
                                     bool enabled,
                                     const std::string& action,
                                     const wchar_t* hint, int text_indent,
                                     wchar_t chip_glyph,
                                     COLORREF chip_color, int row_padding) {
  const size_t first = items_.size();
  // kToggleHeight, not kToggleHeight + 4: the row's padding is AddRow's
  // business. Handing the extra four points to the painter drew the Mac's
  // 26x15 mini switch as a 26x19 one — squat, and with barely half the
  // knob travel, which is what wedged the knob against the end.
  const RECT area = AddRow(y, title, kToggleHeight, kToggleWidth, hint,
                           text_indent, chip_glyph, chip_color, row_padding);
  // A dead row must READ dead. The label and its description used to paint
  // at full strength beside a washed-out switch, so the Kiosk pair and
  // "Apply theme to text selection" looked exactly like the live rows
  // above them and simply did nothing when clicked.
  for (size_t i = first; i < items_.size(); ++i) {
    items_[i].enabled = enabled;
  }
  Item toggle;
  toggle.kind = ItemKind::kToggle;
  toggle.on = on;
  toggle.enabled = enabled;
  toggle.action = action;
  toggle.bounds = area;
  items_.push_back(std::move(toggle));
}

void PhiSettingsWindow::AddExpander(int* y, const std::wstring& name,
                                    const std::wstring& hint,
                                    const std::wstring& icon, bool avatar,
                                    COLORREF tint, bool expanded, bool badge,
                                    const std::string& action,
                                    const std::string& payload) {
  Item header;
  header.kind = ItemKind::kExpander;
  header.text = name;
  header.detail = hint;
  header.icon = icon;
  header.value = avatar ? 1 : 0;
  header.swatch = tint;
  header.on = expanded;
  header.selected = badge;
  header.action = action;
  header.payload = payload;
  // The card opened kRowPadding above; the header row fills the card's
  // width minus a 4 dip inset so its hover wash sits inside the border.
  header.bounds = {content_left_ + Scaled(4), *y - Scaled(kRowPadding) + Scaled(4),
                   content_right_ - Scaled(4),
                   *y - Scaled(kRowPadding) + Scaled(4) + Scaled(kExpanderHeight)};
  items_.push_back(std::move(header));
  *y = *y - Scaled(kRowPadding) + Scaled(4) + Scaled(kExpanderHeight) +
       Scaled(4);
  ++card_rows_;
}

void PhiSettingsWindow::AddSpaceBody(int* y, const PhiSpaceSummary& space,
                                     int ordinal) {
  const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
  const bool expanded_only = search_filter_.empty();
  AddDividerRow(y);
  *y += Scaled(kRowPadding);
  // Name — the value under the label, "Rename…" trailing.
  {
    const RECT area = AddRow(y, L"Name", kPillHeight, 100,
                             base::UTF8ToWide(space.name).c_str());
    Item button;
    button.kind = ItemKind::kButton;
    button.text = L"Rename…";
    button.action = "renspace";
    button.payload = space.id;
    button.bounds = area;
    items_.push_back(std::move(button));
  }
  AddDividerRow(y);
  // Profile — a ComboBox of the profiles; re-binding respawns the Space's
  // browser in the new profile (the Mac's changeProfile).
  {
    const RECT area = AddRow(y, L"Profile", kPillHeight, kPillWidth,
                             L"The browsing profile this Space uses: its "
                             L"cookies, history and extensions.");
    Item pill;
    pill.kind = ItemKind::kPopup;
    for (const PhiProfileSummary& profile : profiles) {
      if (profile.id == space.profile_id) {
        pill.text = base::UTF8ToWide(profile.name);
      }
    }
    if (pill.text.empty()) {
      pill.text = base::UTF8ToWide(space.profile_id);
    }
    pill.action = "spaceprofile";
    pill.payload = space.id;
    pill.bounds = area;
    items_.push_back(std::move(pill));
  }
  AddDividerRow(y);
  // Icon — the current icon in a 32 cell, "Change…" beside it; the
  // picker unfolds under the row (§7.1), scrolling in place.
  {
    const bool open = expanded_only && picker_open_ == space.id;
    const RECT area = AddRow(y, L"Icon", 32, 32 + 8 + 100);
    Item cell;
    cell.kind = ItemKind::kEmojiCell;
    cell.text = base::UTF8ToWide(space.icon_text);
    cell.selected = true;
    cell.action = "spaceiconpicker";
    cell.payload = space.id;
    cell.bounds = {area.left, area.top, area.left + Scaled(32), area.bottom};
    items_.push_back(std::move(cell));
    Item button;
    button.kind = ItemKind::kButton;
    button.text = open ? L"Done" : L"Change…";
    button.action = "spaceiconpicker";
    button.payload = space.id;
    button.bounds = {area.right - Scaled(100), area.top, area.right,
                     area.bottom};
    items_.push_back(std::move(button));
    if (open) {
      const int left = content_left_ + Scaled(kCardPadding);
      const int right = content_right_ - Scaled(kCardPadding);
      // The segmented control (Icon | Emoji), then four lines of cells.
      const struct {
        const wchar_t* label;
        const char* tab;
      } kTabs[] = {{L"Icon", "icon"}, {L"Emoji", "emoji"}};
      int seg_x = left;
      for (const auto& tab : kTabs) {
        Item segment;
        segment.kind = ItemKind::kButton;
        segment.text = tab.label;
        segment.selected = picker_tab_ == tab.tab;
        segment.action = "pickertab";
        segment.payload = tab.tab;
        segment.bounds = {seg_x, *y, seg_x + Scaled(72), *y + Scaled(28)};
        items_.push_back(std::move(segment));
        seg_x += Scaled(72) + Scaled(4);
      }
      *y += Scaled(28) + Scaled(8);
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
      const std::wstring current_icon = base::UTF8ToWide(space.icon_text);
      const int pitch = Scaled(kEmojiCell + kEmojiGap);
      const int per_row =
          std::max(1, (right - left + Scaled(kEmojiGap)) / pitch);
      const int rows_fit = kPickerLines;
      const int lines =
          (static_cast<int>(cells.size()) + per_row - 1) / per_row;
      picker_max_line_ = std::max(0, lines - rows_fit);
      picker_scroll_ =
          std::max(0, std::min(picker_scroll_, picker_max_line_));
      const int grid_top = *y;
      picker_grid_ = {left, grid_top, right, grid_top + rows_fit * pitch};
      const size_t first = static_cast<size_t>(picker_scroll_) *
                           static_cast<size_t>(per_row);
      const size_t per_page = static_cast<size_t>(per_row * rows_fit);
      for (size_t i = first; i < cells.size() && i < first + per_page; ++i) {
        const int index = static_cast<int>(i - first);
        const int col = index % per_row;
        const int line = index / per_row;
        Item pick;
        pick.kind = ItemKind::kEmojiCell;
        pick.text = cells[i].first;
        pick.selected =
            !current_icon.empty() && cells[i].first == current_icon;
        pick.action = "spaceicon";
        pick.payload = cells[i].second;
        pick.space = space.id;
        const int x = left + col * pitch;
        const int top = grid_top + line * pitch;
        pick.bounds = {x, top, x + Scaled(kEmojiCell),
                       top + Scaled(kEmojiCell)};
        items_.push_back(std::move(pick));
      }
      *y = grid_top + rows_fit * pitch + Scaled(kRowPadding);
    }
  }
  AddDividerRow(y);
  // Color — the theme dots; a Space pinning no theme follows the app's.
  {
    const auto& themes = PhiBuiltInThemes();
    const int block = static_cast<int>(themes.size()) *
                          (kSwatchItemSpaces + kSwatchGapSpaces) -
                      kSwatchGapSpaces;
    const bool inherited = space.theme_id.empty();
    const std::string effective_theme =
        inherited ? prefs_->ThemeId() : space.theme_id;
    const RECT area = AddRow(y, L"Color", kSwatchBlockSpaces, block,
                             inherited ? L"Follows the app-wide theme."
                                       : nullptr);
    int x = area.left;
    for (size_t i = 0; i < themes.size(); ++i) {
      Item swatch;
      swatch.kind = ItemKind::kSwatch;
      swatch.text = base::UTF8ToWide(themes[i].name);
      swatch.selected = effective_theme == themes[i].id;
      swatch.on = swatch.selected && inherited;
      const PhiPalette resolved = PhiResolvePalette(
          themes[i], prefs_->Appearance(), prefs_->Saturation());
      swatch.swatch = i == 0 ? RGB(0xFF, 0xFF, 0xFF) : resolved.accent;
      swatch.ring = resolved.accent;
      swatch.value = kSwatchDotSpaces;
      swatch.action = "spacetheme";
      swatch.payload = themes[i].id;
      swatch.space = space.id;
      swatch.bounds = {x, area.top, x + Scaled(kSwatchItemSpaces),
                       area.top + Scaled(kSwatchBlockSpaces)};
      items_.push_back(std::move(swatch));
      x += Scaled(kSwatchItemSpaces + kSwatchGapSpaces);
    }
  }
  AddDividerRow(y);
  // Saturation — this Space's slider (−1 follows the app-wide one).
  {
    const RECT area = AddRow(y, L"Saturation", kSliderHeight, kSliderSpaces);
    Item slider;
    slider.kind = ItemKind::kSliderTrack;
    slider.value =
        space.saturation < 0 ? prefs_->Saturation() : space.saturation;
    slider.action = "spacesat";
    slider.payload = space.id;
    const std::string effective =
        space.theme_id.empty() ? prefs_->ThemeId() : space.theme_id;
    const PhiTheme& theme = PhiThemeById(effective);
    slider.swatch =
        prefs_->Palette().dark ? theme.dark.overlay : theme.light.overlay;
    slider.on = effective == "pure";
    slider.bounds = area;
    items_.push_back(std::move(slider));
  }
  AddDividerRow(y);
  // Shortcut — Ctrl+n follows the strip order (the first nine Spaces).
  {
    const RECT area = AddRow(y, L"Shortcut", kPillHeight, 80,
                             L"Follows the Space's position in the strip.");
    if (ordinal >= 1 && ordinal <= 9) {
      Item cap;
      cap.kind = ItemKind::kKeycap;
      cap.text = L"Ctrl+" + std::to_wstring(ordinal);
      cap.bounds = area;
      items_.push_back(std::move(cap));
    }
  }
  AddDividerRow(y);
  // Delete — the danger button, disabled for the last Space.
  {
    const bool deletable = spaces_->CanDeleteSpace(space.id);
    const RECT area = AddRow(
        y, L"Delete this Space", kPillHeight, 120,
        deletable ? L"Its bookmarks and pinned tabs are removed with it."
                  : L"The last Space cannot be deleted.");
    Item button;
    button.kind = ItemKind::kButton;
    button.text = L"Delete Space";
    button.danger = true;
    button.enabled = deletable;
    button.action = "delspace";
    button.payload = space.id;
    button.bounds = area;
    items_.push_back(std::move(button));
  }
}

void PhiSettingsWindow::BuildSpaces(int* y) {
  const std::vector<PhiSpaceSummary> spaces = spaces_->Summaries();
  const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
  const bool searching = !search_filter_.empty();
  // The group header: "Your Spaces" with the "+ New Space" button on the
  // same line (§7.1). Neither shows on the results page.
  if (!searching) {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = L"Your Spaces";
    heading.bounds = {content_left_, *y, content_right_ - Scaled(140),
                      *y + Scaled(kPillHeight)};
    items_.push_back(std::move(heading));
    Item add;
    add.kind = ItemKind::kButton;
    add.text = L"  New Space";
    add.action = "newspace";
    add.bounds = {content_right_ - Scaled(130), *y, content_right_,
                  *y + Scaled(kPillHeight)};
    items_.push_back(std::move(add));
    *y += Scaled(kPillHeight) + Scaled(kTitleToCard);
  }
  int ordinal = 0;
  for (const PhiSpaceSummary& space : spaces) {
    ++ordinal;
    const std::wstring name = base::UTF8ToWide(space.name);
    std::wstring profile_name;
    for (const PhiProfileSummary& profile : profiles) {
      if (profile.id == space.profile_id) {
        profile_name = base::UTF8ToWide(profile.name);
      }
    }
    if (searching && !SearchMatches(name.c_str(), profile_name.c_str())) {
      continue;
    }
    ++filter_hits_;
    std::wstring hint = profile_name.empty() ? L"" : profile_name;
    if (ordinal <= 9) {
      hint += (hint.empty() ? L"" : L"  ·  ") + std::wstring(L"Ctrl+") +
              std::to_wstring(ordinal);
    }
    const bool expanded = !searching && selected_space_ == space.id;
    BeginSection(y, nullptr);
    AddExpander(y, name, hint, base::UTF8ToWide(space.icon_text), false,
                prefs_->Palette().accent, expanded, false, "selectspace",
                space.id);
    if (expanded) {
      AddSpaceBody(y, space, ordinal);
    }
    EndSection(y);
    *y -= Scaled(kSectionGap - kExpanderGap);
  }
  if (!searching) {
    *y += Scaled(kSectionGap - kExpanderGap);
  }

  // Pane-wide card: pinned tab scope, with the live description.
  BeginSection(y, nullptr);
  {
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
    const RECT area =
        AddRow(y, L"Pinned tab scope", kPillHeight, kPillWidth, description);
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

  // The routing rules belong with the Spaces they route into; the same row
  // also sits on the Navigation pane beside the other link-opening controls.
  AddUrlRulesSection(y);
}

// The Mac's Navigations pane (NavigationsSettingsView): the link-opening
// controls, collected out of General and Spaces. Peek, the Ctrl+Alt-click
// Kiosk switch and URL Rules are real.
//
// The EXTERNAL-LINKS Kiosk switch is drawn for the shape of the pane but sits
// INERT: the client has no external-link entry point yet. It becomes a live
// row once one exists, the way the Ctrl+Alt row did when the framework took
// the Mac's Cmd+Opt remap (Browser::OpenURLFromTab) for Windows.
void PhiSettingsWindow::BuildNavigation(int* y) {
  AddSectionHeading(y, L"Kiosk",
                    L"Open links in a lightweight window, one page at a "
                    L"time.");
  BeginSection(y, nullptr);
  AddToggleRow(y, L"Open external links in Kiosk", /*on=*/false,
               /*enabled=*/false, std::string(),
               L"Links opened from other apps use a focused, single-page "
               L"Kiosk window.");
  AddDividerRow(y);
  AddToggleRow(y, L"Open Kiosk when clicking links with Ctrl+Alt held",
               prefs_->OpenKioskOnCtrlAltClick(), /*enabled=*/true,
               "kioskctrlalt",
               L"Hold Ctrl and Alt while clicking a link to open it in a "
               L"Kiosk window.");
  AddDividerRow(y);
  AddSubHint(y, L"Kiosk also opens from a link's context menu and from URL "
                L"rules.");
  EndSection(y);

  AddSectionHeading(y, L"Peek",
                    L"Preview a link without opening it in a new tab.");
  const bool peek = prefs_->PeekViewEnabled();
  BeginSection(y, nullptr);
  AddToggleRow(y, L"Enable Peek View", peek, /*enabled=*/true, "peekview",
               L"Shift-click a link, or choose \u201COpen Link in Peek "
               L"View\u201D, to preview it in a floating panel over the "
               L"page.");
  AddDividerRow(y);
  // The sub-switch is dimmed and inert while the master is off, as on Mac.
  AddToggleRow(y, L"Automatically peek from pinned tabs and bookmarks",
               prefs_->AutoPeekViewEnabled(), /*enabled=*/peek, "autopeek",
               L"Links that leave a pinned tab\u2019s or bookmark\u2019s "
               L"site open in a Peek panel instead of a new tab.");
  EndSection(y);

  AddUrlRulesSection(y);
}

// The "URL Rules" section the Spaces and Navigation panes share: one row
// that opens the universal editor (PhiUrlRulesWindow), as the Mac's
// "Manage URL Rules…" row does.
void PhiSettingsWindow::AddUrlRulesSection(int* y) {
  BeginSection(y, L"URL Rules");
  {
    // The whole row opens the editor, not just the trailing chevron —
    // emitted FIRST so its hover wash paints behind the text.
    Item row;
    row.kind = ItemKind::kListRow;
    row.action = "urlrules";
    items_.push_back(std::move(row));
    const size_t row_index = items_.size() - 1;
    const size_t first = items_.size();
    const RECT area =
        AddRow(y, L"Manage URL Rules…", kRowLabelHeight, 20,
               L"Route matching domains or URLs to a Space, Incognito, or "
               L"Kiosk.");
    Item chevron;
    chevron.kind = ItemKind::kIcon;
    chevron.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
    chevron.bounds = area;
    items_.push_back(std::move(chevron));
    items_[row_index].bounds = {
        content_left_ + Scaled(4), items_[first].bounds.top - Scaled(6),
        content_right_ - Scaled(4),
        items_[items_.size() - 2].bounds.bottom + Scaled(6)};
  }
  EndSection(y);
}

void PhiSettingsWindow::AddProfileBody(
    int* y, const PhiProfileSummary& profile,
    const std::vector<PhiSpaceSummary>& spaces) {
  const std::string& id = profile.id;
  AddDividerRow(y);
  *y += Scaled(kRowPadding);
  {
    const RECT area = AddRow(y, L"Name", kPillHeight, 100,
                             base::UTF8ToWide(profile.name).c_str());
    Item button;
    button.kind = ItemKind::kButton;
    button.text = L"Rename…";
    button.action = "renprofile";
    button.payload = id;
    button.bounds = area;
    items_.push_back(std::move(button));
  }
  AddDividerRow(y);
  // Search engine / download location — live, per profile. Both read
  // empty while the profile is not loaded, and the rows grey out; one
  // short poll per rebuild fills them in once the loads land.
  {
    const std::vector<PhiSearchEngineRow> engines =
        profiles_->SearchEngines(id);
    std::wstring engine_name = L"Unavailable";
    for (const PhiSearchEngineRow& engine : engines) {
      if (engine.is_default) {
        engine_name = base::UTF8ToWide(engine.name);
        break;
      }
    }
    const std::string download_dir = profiles_->DownloadDirectory(id);
    if ((engines.empty() || download_dir.empty()) && alive()) {
      profile_poll_.Start(700);
    }
    std::wstring download_leaf = L"Choose…";
    if (!download_dir.empty()) {
      const size_t cut = download_dir.find_last_of("\\\\/");
      download_leaf = base::UTF8ToWide(
          cut == std::string::npos ? download_dir
                                   : download_dir.substr(cut + 1));
    }
    {
      const RECT area = AddRow(y, L"Search engine", kPillHeight, kPillWidth);
      Item pill;
      pill.kind = ItemKind::kPopup;
      pill.text = engine_name;
      pill.enabled = !engines.empty();
      pill.action = "searchengine";
      pill.payload = id;
      pill.bounds = area;
      items_.push_back(std::move(pill));
    }
    AddDividerRow(y);
    {
      const RECT area =
          AddRow(y, L"Download location", kPillHeight, kPillWidth);
      Item pill;
      pill.kind = ItemKind::kPopup;
      pill.text = download_leaf;
      pill.enabled = !download_dir.empty();
      pill.action = "downloadfolder";
      pill.payload = id;
      pill.bounds = area;
      items_.push_back(std::move(pill));
    }
  }
  AddDividerRow(y);
  // Used by — the Spaces bound to this profile.
  int bound = 0;
  {
    std::wstring names;
    for (const PhiSpaceSummary& space : spaces) {
      if (space.profile_id == id) {
        ++bound;
        names += (names.empty() ? L"" : L", ") + base::UTF8ToWide(space.name);
      }
    }
    AddRow(y, L"Used by", kRowLabelHeight, 20,
           bound == 0 ? L"No Space uses this profile." : names.c_str());
  }
  AddDividerRow(y);
  // The data pages: three rows into chrome://settings for this profile.
  {
    const struct {
      wchar_t glyph;
      const wchar_t* title;
      const char* url;
    } kRows[] = {
        {0xE72E, L"Privacy and security", "chrome://settings/privacy"},
        {0xE7ED, L"Notifications", "chrome://settings/content/notifications"},
        {0xE74D, L"Clear browsing data", "chrome://settings/clearBrowserData"},
    };
    size_t placed = 0;
    for (const auto& def : kRows) {
      const size_t row_index = items_.size();
      Item row;
      row.kind = ItemKind::kListRow;
      row.action = "openpage";
      row.payload = def.url;
      items_.push_back(std::move(row));
      const int top = *y;
      const RECT area = AddRow(y, def.title, kRowLabelHeight, 24, nullptr,
                               36, 0, 0, 10);
      if (area.right > area.left) {
        Item glyph;
        glyph.kind = ItemKind::kIcon;
        glyph.text = std::wstring(1, def.glyph);
        glyph.bounds = {content_left_ + Scaled(kCardPadding), area.top,
                        content_left_ + Scaled(kCardPadding) + Scaled(24),
                        area.bottom};
        items_.push_back(std::move(glyph));
        Item arrow;
        arrow.kind = ItemKind::kIcon;
        arrow.text = std::wstring(1, wchar_t(0xE72A));
        arrow.selected = true;
        arrow.bounds = area;
        items_.push_back(std::move(arrow));
        items_[row_index].bounds = {content_left_ + Scaled(4), top - Scaled(4),
                                    content_right_ - Scaled(4),
                                    *y - Scaled(10) + Scaled(4)};
      }
      if (++placed < std::size(kRows)) {
        AddDividerRow(y);
      }
    }
  }
  AddDividerRow(y);
  // Delete — Default is permanent; a profile still bound to a Space stays.
  {
    const bool is_default = id == "Default";
    const bool deletable = !is_default && bound == 0;
    const RECT area = AddRow(
        y, L"Delete this profile", kPillHeight, 120,
        is_default ? L"The Default profile cannot be deleted."
        : bound > 0
            ? L"Move its Spaces to another profile first."
            : L"Its cookies, history, extensions and saved data are "
              L"removed with it.");
    Item button;
    button.kind = ItemKind::kButton;
    button.text = L"Delete profile";
    button.danger = true;
    button.enabled = deletable;
    button.action = "delprofile";
    button.payload = id;
    button.bounds = area;
    items_.push_back(std::move(button));
  }
}

void PhiSettingsWindow::BuildProfiles(int* y) {
  const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
  const std::vector<PhiSpaceSummary> spaces = spaces_->Summaries();
  const bool searching = !search_filter_.empty();
  if (!searching) {
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    heading.text = L"Your profiles";
    heading.bounds = {content_left_, *y, content_right_ - Scaled(140),
                      *y + Scaled(kPillHeight)};
    items_.push_back(std::move(heading));
    Item add;
    add.kind = ItemKind::kButton;
    add.text = L"  New profile";
    add.action = "addprofile";
    add.bounds = {content_right_ - Scaled(130), *y, content_right_,
                  *y + Scaled(kPillHeight)};
    items_.push_back(std::move(add));
    *y += Scaled(kPillHeight) + Scaled(kTitleToCard);
  }
  // Avatar tints, by position — a profile has no colour of its own.
  static constexpr COLORREF kTints[] = {
      RGB(0x0F, 0x6C, 0xBD), RGB(0xCA, 0x50, 0x10), RGB(0x0B, 0x6A, 0x0B),
      RGB(0x88, 0x17, 0x98), RGB(0xC2, 0x39, 0x62), RGB(0x03, 0x83, 0x87),
  };
  size_t at = 0;
  for (const PhiProfileSummary& profile : profiles) {
    const std::wstring name = base::UTF8ToWide(profile.name);
    const COLORREF tint = kTints[at++ % std::size(kTints)];
    std::wstring used;
    int bound = 0;
    for (const PhiSpaceSummary& space : spaces) {
      if (space.profile_id == profile.id) {
        ++bound;
        used += (used.empty() ? L"" : L", ") + base::UTF8ToWide(space.name);
      }
    }
    if (searching && !SearchMatches(name.c_str(), used.c_str())) {
      continue;
    }
    ++filter_hits_;
    std::wstring hint =
        bound == 0 ? L"Not used"
                   : L"Used by " + used + L"  ·  " +
                         std::to_wstring(bound) +
                         (bound == 1 ? L" Space" : L" Spaces");
    const bool expanded = !searching && selected_profile_ == profile.id;
    BeginSection(y, nullptr);
    AddExpander(y, name, hint, name.empty() ? L"?" : name.substr(0, 1), true,
                tint, expanded, profile.id == "Default", "selectprofile",
                profile.id);
    if (expanded) {
      AddProfileBody(y, profile, spaces);
    }
    EndSection(y);
    *y -= Scaled(kSectionGap - kExpanderGap);
  }
  if (!searching) {
    *y += Scaled(kSectionGap - kExpanderGap);
    AddHint(y, L"Each profile keeps its own cookies, history, extensions "
               L"and saved data. A Space browses in exactly one profile.");
  }
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
    const std::vector<PhiSearchEngineRow> engines =
        profiles_->SearchEngines(profile_id);
    std::wstring engine_name = L"Unavailable";
    for (const PhiSearchEngineRow& engine : engines) {
      if (engine.is_default) {
        engine_name = base::UTF8ToWide(engine.name);
        break;
      }
    }
    const std::string download_dir = profiles_->DownloadDirectory(profile_id);
    // A selected profile loads on demand (the framework kicks the profile
    // and its keyword service). While either read is still pending, poll:
    // one short timer per rebuild, re-armed only while data is missing, so
    // the pane fills in by itself once the loads land.
    if ((engines.empty() || download_dir.empty()) && alive()) {
      profile_poll_.Start(700);
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
    *y += 2 * row_h + Scaled(kDetailCardGap);
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
      // The whole row is the button, as on Mac — behind its own content, so
      // the hover wash does not paint over the label.
      Item row;
      row.kind = ItemKind::kListRow;
      row.action = "openpage";
      row.payload = def.url;
      row.bounds = {left + Scaled(4), top, right - Scaled(4), top + row_h};
      items_.push_back(std::move(row));
      detail_row(top, def.glyph, def.title, true);
      Item arrow;
      arrow.kind = ItemKind::kIcon;
      arrow.text = std::wstring(1, wchar_t(0xE72A));  // Forward arrow
      arrow.selected = true;  // kIcon: selected = accent ink (the Mac's).
      arrow.bounds = {right - Scaled(kCardPadding + 24), top,
                      right - Scaled(kCardPadding), top + row_h};
      items_.push_back(std::move(arrow));
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
  // The Mac pane's frame: the profile card on the LEFT (264 wide at x=24,
  // holding a 240x380 card with "Download image" under it), the account
  // row and the Default Browser section stacked on the right. Signed out
  // (the Mac's Guest presentation) the card is hidden and the right column
  // takes the whole width. The account is OPTIONAL on Windows
  // (docs/login-port.md): nothing else is withheld.
  const int top = *y;
  // The Mac's `offset(24)` is 36 minus the 12pt of shadow padding its card
  // view carries INSIDE its frame; the card's VISIBLE edge sits at 36 like
  // every other pane's content: card 36..276, right column 292..644.
  const int left = content_left_;
  const int card_w = Scaled(240);
  const int card_h = Scaled(380);
  const int right_edge = content_right_;
  std::string account_name;
  std::string account_email;
  const bool signed_in =
      account_->AccountInfo(&account_name, &account_email);
  const bool signing_in = account_->AccountSigningIn();
  const bool reauth = account_->AccountReauthRequired();
  const bool busy = account_->AccountBusy();
  const std::string error = account_->AccountLastError();
  const bool show_card = signed_in && !reauth;
  int bottom = top;
  if (show_card) {
    Item card;
    card.kind = ItemKind::kProfileCard;
    card.action = "card";  // Hit-tested for the drag; Apply ignores it.
    card.bounds = {left, top, left + card_w, top + card_h};
    items_.push_back(std::move(card));
    // The Mac's rounded "Download image" button, centred under the card
    // (shadow padding 10 + 4).
    Item download;
    download.kind = ItemKind::kButton;
    download.text = PhiStr("settings.account.sharing.downloadImageButton");
    download.action = "downloadcard";
    const int button_w = Scaled(150);
    download.bounds = {left + (card_w - button_w) / 2, top + card_h + Scaled(14),
                       left + (card_w + button_w) / 2,
                       top + card_h + Scaled(14) + Scaled(24)};
    bottom = download.bounds.bottom;
    items_.push_back(std::move(download));
  }
  const int right_left = show_card ? left + card_w + Scaled(16) : left;
  {
    // The account row (the Mac's 92pt AccountView): the avatar with its
    // edit badge, the name with its pencil, the email, a status line, and
    // the one action — Sign out / Reauthenticate / Cancel / Sign in.
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {right_left, top, right_edge, top + Scaled(92)};
    items_.push_back(std::move(card));
    const int button_w = Scaled(108);
    const int text_right = right_edge - Scaled(kCardPadding) - button_w - Scaled(8);
    Item avatar;
    avatar.kind = ItemKind::kAvatar;
    avatar.text = std::wstring(1, wchar_t(0xE77B));  // Contact
    avatar.value = 36;  // Fallback glyph size.
    const int avatar_px = Scaled(56);
    avatar.bounds = {right_left + Scaled(12), top + (Scaled(92) - avatar_px) / 2,
                     right_left + Scaled(12) + avatar_px,
                     top + (Scaled(92) - avatar_px) / 2 + avatar_px};
    if (show_card) {
      avatar.action = "avatar";
      avatar.enabled = !busy;
    }
    items_.push_back(std::move(avatar));
    const int text_left = right_left + Scaled(12) + avatar_px + Scaled(12);
    std::wstring error_text;
    if (error == "account_mismatch") {
      error_text = PhiStr("settings.account.error.accountMismatch");
    } else if (error == "refused") {
      error_text = PhiStr("settings.account.error.refused");
    } else if (error == "incomplete") {
      error_text = PhiStr("settings.account.error.incomplete");
    } else if (error == "profile") {
      error_text = PhiStr("settings.account.error.profile");
    } else if (error == "update") {
      error_text = PhiStr("settings.account.error.update");
    }
    if (signed_in) {
      Item name;
      name.kind = ItemKind::kEditableLabel;
      name.text = !account_name.empty()
                      ? base::UTF8ToWide(account_name)
                      : PhiStr("settings.account.displayName.fallback");
      if (show_card) {
        name.action = "rename";
        name.enabled = !busy;
      }
      name.bounds = {text_left, top + Scaled(16), text_right, top + Scaled(46)};
      items_.push_back(std::move(name));
      Item email;
      email.kind = ItemKind::kHint;
      email.text = base::UTF8ToWide(account_email);
      email.bounds = {text_left, top + Scaled(47), text_right,
                      top + Scaled(47) + Scaled(kHintHeight)};
      items_.push_back(std::move(email));
      std::wstring status;
      if (reauth) {
        status = PhiStr("settings.account.reauthentication.warning");
      } else if (busy) {
        status = PhiStr("settings.account.saving");
      } else {
        status = error_text;
      }
      if (!status.empty()) {
        Item line;
        line.kind = ItemKind::kHint;
        line.text = status;
        line.bounds = {text_left, top + Scaled(63), text_right,
                       top + Scaled(63) + Scaled(kHintHeight)};
        items_.push_back(std::move(line));
      }
    } else {
      Item label;
      label.kind = ItemKind::kRowLabel;
      label.text = signing_in ? PhiStr("settings.finishSigningInFromThe")
                              : PhiStr("settings.youAreNotSignedIn");
      const bool has_error = !signing_in && !error_text.empty();
      label.bounds = {text_left, top, text_right,
                      has_error ? top + Scaled(58) : top + Scaled(92)};
      items_.push_back(std::move(label));
      if (has_error) {
        Item failure;
        failure.kind = ItemKind::kHint;
        failure.text = error_text;
        failure.bounds = {text_left, top + Scaled(56), text_right,
                          top + Scaled(56) + Scaled(kHintHeight)};
        items_.push_back(std::move(failure));
      }
    }
    Item button;
    button.kind = ItemKind::kButton;
    if (reauth) {
      button.text = PhiStr("oobe.reauthentication.confirmButton");
      button.action = "signin";
    } else if (signed_in) {
      button.text = PhiStr("settings.account.logoutButton");
      button.action = "signout";
    } else if (signing_in) {
      button.text = PhiStr("settings.account.cancelSignIn");
      button.action = "cancelsignin";
    } else {
      button.text = PhiStr("settings.account.guest.loginButton");
      button.action = "signin";
    }
    button.enabled = !busy;
    button.bounds = {right_edge - Scaled(kCardPadding) - button_w,
                     top + Scaled(34), right_edge - Scaled(kCardPadding),
                     top + Scaled(58)};
    items_.push_back(std::move(button));
  }
  const int section_top = top + Scaled(92) + Scaled(20);
  {
    // Default Browser (the Mac's 74pt section), with a LIVE button.
    Item card;
    card.kind = ItemKind::kCard;
    card.bounds = {right_left, section_top, right_edge,
                   section_top + Scaled(74)};
    items_.push_back(std::move(card));
    if (!is_default_browser_.has_value()) {
      is_default_browser_ = PhiThisExeIsDefaultBrowser();
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
  *y = std::max(bottom, section_top + Scaled(74)) + Scaled(kSectionGap);
}

void PhiSettingsWindow::BuildAi(int* y) {
  AddToggleRow(y, L"Enable AI features in Phi Browser", /*on=*/true,
               /*enabled=*/false, std::string());
  *y += Scaled(8);
  BeginSection(y, L"Browser memory");
  {
    const size_t first = items_.size();
    const RECT area = AddRow(y, L"View and manage your browser memory",
                             kRowLabelHeight, 20);
    items_[first].enabled = false;
    Item chevron;
    chevron.kind = ItemKind::kIcon;
    chevron.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
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
    const size_t first = items_.size();
    const RECT area = AddRow(y, L"Private AI", kRowLabelHeight, 20);
    items_[first].enabled = false;
    Item chevron;
    chevron.kind = ItemKind::kIcon;
    chevron.text = std::wstring(1, wchar_t(0xE76C));  // ChevronRight
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
               std::string(),
               L"External Data Connectors help to provide additional "
               L"context for better AI experience");
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
    heading.bounds = {content_left_, *y, content_left_ + Scaled(200),
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    Item restore;
    restore.kind = ItemKind::kButton;
    restore.text = L"Restore all shortcuts";
    restore.enabled = prefs_->AnyShortcutCustomized();
    restore.action = "restoreshortcuts";
    restore.bounds = {content_right_ - Scaled(150),
                      *y - Scaled(4),
                      content_right_,
                      *y - Scaled(4) + Scaled(22)};
    items_.push_back(std::move(restore));
    *y += Scaled(kSectionTitleHeight) + Scaled(10);
  }
  Item card;
  card.kind = ItemKind::kCard;
  card.on = true;  // The white variant.
  card.bounds = {content_left_, *y, content_right_, 0};
  items_.push_back(std::move(card));
  const size_t card_index = items_.size() - 1;

  // The in-card filter ("Find shortcut keys"): typing anywhere while this
  // pane is up feeds it (WM_CHAR), Esc clears, Backspace deletes.
  {
    Item search;
    search.kind = ItemKind::kSearchField;
    search.text = shortcut_search_;
    search.bounds = {content_left_, *y, content_right_,
                     *y + Scaled(40)};
    items_.push_back(std::move(search));
    *y += Scaled(40);
    Item divider;
    divider.kind = ItemKind::kDivider;
    divider.bounds = {content_left_, *y, content_right_, *y + 1};
    items_.push_back(std::move(divider));
    *y += 1;
  }

  // The table, grouped as on Mac (App, File, Edit, View, History,
  // Bookmarks, Window, Tab, Spaces). The preferences service serves the
  // rows: rebindable ones (clickable keycap → the chord recorder) carry a
  // stable name and their EFFECTIVE chord; fixed rows are informational.
  // Plus the live "Go to Space" rows for the first nine Spaces, which are
  // the pane's own and belong to no shortcut table.
  struct ShortcutRowDef {
    std::wstring group;
    std::wstring name;
    std::wstring keys;
    std::string id;  // Rebindable stable name; empty = fixed.
    bool customized = false;
  };
  std::vector<ShortcutRowDef> rows;
  for (const PhiShortcutRow& row : prefs_->ShortcutRows()) {
    rows.push_back({base::UTF8ToWide(row.group), base::UTF8ToWide(row.label),
                    base::UTF8ToWide(row.keys), row.name, row.customized});
  }
  {
    int at = 0;
    for (const PhiSpaceSummary& space : spaces_->Summaries()) {
      if (++at > 9) {
        break;
      }
      rows.push_back({L"Spaces",
                      L"Go to Space \u201C" + base::UTF8ToWide(space.name) +
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
  size_t last_rule = std::numeric_limits<size_t>::max();
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
      heading.bounds = {(content_left_ + Scaled(kCardPadding)), *y,
                        (content_right_ - Scaled(kCardPadding)),
                        *y + Scaled(35)};
      items_.push_back(std::move(heading));
      *y += Scaled(35);
      Item rule;
      rule.kind = ItemKind::kDivider;
      rule.bounds = {(content_left_ + Scaled(kCardPadding)), *y,
                     (content_right_ - Scaled(kCardPadding)), *y + 1};
      items_.push_back(std::move(rule));
      *y += 1;
    }
    Item label;
    label.kind = ItemKind::kRowLabel;
    label.text = row.name;
    label.bounds = {(content_left_ + Scaled(kCardPadding)), *y,
                    (content_right_ - Scaled(kCardPadding)) - Scaled(150),
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
    cap.bounds = {(content_right_ - Scaled(kCardPadding)) - cap_w,
                  *y + (row_h - Scaled(22)) / 2,
                  (content_right_ - Scaled(kCardPadding)),
                  *y + (row_h + Scaled(22)) / 2};
    items_.push_back(std::move(cap));
    *y += row_h;
    Item rule;
    rule.kind = ItemKind::kDivider;
    rule.bounds = {(content_left_ + Scaled(kCardPadding)), *y,
                   (content_right_ - Scaled(kCardPadding)), *y + 1};
    items_.push_back(std::move(rule));
    last_rule = items_.size() - 1;
    *y += 1;
  }
  // The table's rules go BETWEEN rows; the loop left one hanging under the
  // last row, inside the card's own bottom padding.
  if (last_rule != std::numeric_limits<size_t>::max()) {
    items_.erase(items_.begin() + static_cast<ptrdiff_t>(last_rule));
    *y -= 1;
  }
  if (!any) {
    Item empty;
    empty.kind = ItemKind::kHint;
    empty.text = L"No Results";
    empty.bounds = {content_left_, *y + Scaled(40),
                    content_right_, *y + Scaled(40) + Scaled(16)};
    items_.push_back(std::move(empty));
    *y += Scaled(96);
  }
  *y += Scaled(8);
  items_[card_index].bounds.bottom = *y;
  *y += Scaled(kSectionGap);
}

void PhiSettingsWindow::BuildDeveloper(int* y) {
  // The Mac's two cards under one "Agent control" title: the ACCESS card
  // (the master gate) and, under it, the PERMISSIONS card — what an
  // approved agent may touch. Both are chip-led, which is the visual
  // language the whole Mac Developer pane shares.
  BeginSection(y, L"Agent control");
  // The master switch. Windows has no code-signature identity for a pipe
  // client, so there is no per-program approval list under it: on, every
  // local program holding the launch token is admitted without a prompt;
  // off, the first one to connect asks the user to turn this on.
  AddToggleRow(y, L"Allow agents to control Phi (CDP)",
               prefs_->AllowAgentControl(), /*enabled=*/true, "agentcontrol",
               L"Agent tools drive Phi over the DevTools Protocol through "
               L"a private pipe only this machine's processes can reach. "
               L"On, any program running as you that can read Phi\u2019s "
               L"profile folder is admitted without asking; off, the first "
               L"one to connect asks you to turn this on.",
               kChipIndent, 0xE735, kChipIndigo);  // sparkles (filled)
  if (prefs_->AllowAgentControl()) {
    // Under the gate, the Mac's SkillInstallRowView: the phi-browser
    // skill shipped beside the exe, linked into an agent's skills folder
    // from a menu of agents ("Add skill to…"). Shown only while agents
    // are allowed in, as on Mac.
    AddDividerRow(y);
    const RECT area = AddRow(
        y, PhiStr("settings.developer.skillInstall.sectionTitle"),
        kPillHeight, kPillWidth,
        PhiStr("settings.developer.skillInstall.description"), kChipIndent,
        0xEA86, kChipTeal);  // Puzzle — puzzlepiece.extension.fill
    Item pill;
    pill.kind = ItemKind::kPopup;
    pill.text = PhiStr("settings.developer.skillInstall.targetMenuButton");
    pill.action = "installskill";
    pill.bounds = area;
    items_.push_back(std::move(pill));
  }
  EndSection(y);

  BeginSection(y, nullptr);
  // The user-space gate: off, a connected agent works only inside its own
  // agent Spaces; on, the management verbs reach the user's Spaces too.
  AddToggleRow(y, L"Allow agents to operate your Spaces",
               prefs_->AllowAgentUserSpaces(), /*enabled=*/true,
               "agentuserspaces",
               L"Lets connected agents read and change your own Spaces, "
               L"tabs, bookmarks, URL rules and profiles. Off, they can "
               L"only work in their own agent Spaces.",
               kChipIndent, 0xE8A9, kChipOrange);  // square.grid.2x2.fill
  AddDividerRow(y);
  // "Profiles agents can use": a chip-led header, then one switch per
  // profile INDENTED to the header's text column, exactly as the Mac's
  // profilesRows does with .padding(.leading, 36).
  AddRow(y, L"Profiles agents can use", kRowLabelHeight, 0,
         L"Choose which profiles agents may create their own Spaces in. "
         L"Turning a profile off stops agents from opening any new Space "
         L"bound to it; existing Spaces are unaffected.",
         kChipIndent, 0xE716, kChipBlue);  // person.2.fill
  const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
  if (profiles.empty()) {
    AddSubHint(y, L"No profiles found.");
  } else {
    for (const PhiProfileSummary& profile : profiles) {
      const size_t first = items_.size();
      AddToggleRow(y, base::UTF8ToWide(profile.name).c_str(),
                   prefs_->AgentProfileAllowed(profile.id),
                   /*enabled=*/true, "agentprofile", nullptr, kChipIndent,
                   /*chip_glyph=*/0, /*chip_color=*/0, /*row_padding=*/6);
      // The switch is the item AddToggleRow pushed last; the profile is
      // its identity, so a list that moves cannot revoke a neighbour.
      for (size_t i = first; i < items_.size(); ++i) {
        if (items_[i].kind == ItemKind::kToggle) {
          items_[i].payload = profile.id;
        }
      }
    }
  }
  EndSection(y);
}

void PhiSettingsWindow::Rebuild() {
  items_.clear();
  open_card_ = 0;
  if (!host_) {
    ClearHits();
    return;
  }
  // The navigation rail down the left (§7): 280 wide, 48 under the 900
  // breakpoint (icons only), 36 dip items with a 16 icon and a 14 label.
  // Two pages are not always mounted: Developer outside developer mode,
  // and Phi & AI while the AI suite is off (phi_features.h).
  const bool developer = prefs_->DeveloperMode();
  const auto unmounted = [developer](const char* id) {
    const std::string name(id);
    return (!developer && name == "developer") ||
           (!kPhiAiEnabled && name == "ai");
  };
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const bool narrow = client.right < Scaled(kRailBreakpoint);
  rail_w_ = Scaled(narrow ? kRailNarrow : kRailWidth);
  content_left_ = rail_w_ + Scaled(kContentPad);
  content_right_ = std::max(content_left_ + Scaled(200),
                            std::min(static_cast<int>(client.right) - Scaled(kContentPad + 12),
                                     content_left_ + Scaled(kContentMaxWidth)));
  int rail_y = Scaled(kCaptionHeight + 8);
  if (!narrow) {
    // "Find a setting" (§7): the rail's field, fed by WM_CHAR on every
    // page but Shortcuts (which has its own). Built before the page items
    // so it paints with the rail, outside the content clip.
    Item search;
    search.kind = ItemKind::kSearchField;
    search.text = rail_search_;
    search.detail = L"Find a setting";
    search.on = true;  // Draw its own field (the rail has no card).
    search.bounds = {Scaled(12), rail_y, rail_w_ - Scaled(12),
                     rail_y + Scaled(kRailSearchHeight)};
    items_.push_back(std::move(search));
    rail_y += Scaled(kRailSearchHeight + 12);
  }
  for (const SectionDef& section : kSections) {
    if (unmounted(section.id)) {
      continue;
    }
    Item entry;
    entry.kind = ItemKind::kToolbarItem;
    entry.text = narrow ? std::wstring() : section.label;
    entry.detail = section.label;  // The tooltip / narrow rail's name.
    entry.payload = section.id;
    entry.selected = section_ == section.id;
    entry.action = "section";
    entry.bounds = {Scaled(4), rail_y, rail_w_ - Scaled(4),
                    rail_y + Scaled(kRailItemHeight)};
    items_.push_back(std::move(entry));
    rail_y += Scaled(kRailItemHeight + 4);
  }
  if (unmounted(section_.c_str())) {
    section_ = "general";  // The page vanished under us, as on Mac.
    SyncTitle();
  }

  picker_grid_ = {};  // Only the Spaces pane mounts the icon picker.
  picker_max_line_ = 0;
  search_filter_.clear();
  filter_hits_ = 0;
  last_row_skipped_ = false;
  heading_pending_ = false;
  const bool searching = !rail_search_.empty() && section_ != "shortcuts";
  if (searching) {
    search_filter_ = rail_search_;
    for (wchar_t& c : search_filter_) {
      c = static_cast<wchar_t>(::towlower(c));
    }
  }
  int y = Scaled(kChromeTop + kContentPad) - scroll_;
  const int top = y;
  {
    // The page title (28/600), then the groups.
    Item title;
    title.kind = ItemKind::kPageTitle;
    for (const SectionDef& section : kSections) {
      if (section_ == section.id) {
        title.text = section.label;
      }
    }
    if (searching) {
      title.text = L"Results for \u201C" + rail_search_ + L"\u201D";
    }
    title.bounds = {content_left_, y, content_right_,
                    y + Scaled(kPageTitleHeight)};
    items_.push_back(std::move(title));
    y += Scaled(kPageTitleHeight + kPageTitleGap);
  }
  if (searching) {
    BuildSearchResults(&y);
  } else if (section_ == "general") {
    BuildGeneral(&y);
  } else if (section_ == "appearance") {
    BuildAppearance(&y);
  } else if (section_ == "about") {
    BuildAbout(&y);
  } else if (section_ == "spaces") {
    BuildSpaces(&y);
  } else if (section_ == "navigation") {
    BuildNavigation(&y);
  } else if (section_ == "profiles") {
    BuildProfiles(&y);
  } else if (section_ == "shortcuts") {
    BuildShortcuts(&y);
  } else if (section_ == "ai") {
    BuildAi(&y);
  } else if (section_ == "account") {
    BuildAccount(&y);
  } else if (section_ == "advanced") {
    BuildAdvanced(&y);
  } else if (section_ == "developer") {
    BuildDeveloper(&y);
  } else {
    section_ = "general";  // A retired pane id (e.g. "phiLink").
    BuildGeneral(&y);
  }
  // The running `y` carries the trailing section gap and, on the
  // master-detail panes, none of a detail column that outgrows the band —
  // so measure the content by what was actually laid out. It decides both
  // the scroll limit and the indicator's thumb.
  int bottom = top;
  for (const Item& item : items_) {
    if (item.kind != ItemKind::kToolbarItem) {
      bottom = std::max<int>(bottom, item.bounds.bottom);
    }
  }
  content_height_ = bottom - top + content_left_;
  PublishHits();
}

// The results page (§7 "Find a setting"): every page's rows that carry
// the typed text, under that page's heading — the page builders run with
// the filter up, and AddRow / EndSection keep only what matches.
void PhiSettingsWindow::BuildSearchResults(int* y) {
  const bool developer = prefs_->DeveloperMode();
  struct Page {
    const char* id;
    void (PhiSettingsWindow::*build)(int*);
  };
  const Page kPages[] = {
      {"general", &PhiSettingsWindow::BuildGeneral},
      {"appearance", &PhiSettingsWindow::BuildAppearance},
      {"spaces", &PhiSettingsWindow::BuildSpaces},
      {"profiles", &PhiSettingsWindow::BuildProfiles},
      {"navigation", &PhiSettingsWindow::BuildNavigation},
      {"ai", &PhiSettingsWindow::BuildAi},
      {"advanced", &PhiSettingsWindow::BuildAdvanced},
      {"developer", &PhiSettingsWindow::BuildDeveloper},
      {"about", &PhiSettingsWindow::BuildAbout},
  };
  int total = 0;
  for (const Page& page : kPages) {
    const std::string id(page.id);
    if ((id == "developer" && !developer) || (id == "ai" && !kPhiAiEnabled)) {
      continue;
    }
    const size_t start = items_.size();
    const int start_y = *y;
    Item heading;
    heading.kind = ItemKind::kSectionTitle;
    for (const SectionDef& section : kSections) {
      if (id == section.id) {
        heading.text = section.label;
      }
    }
    heading.bounds = {content_left_, *y, content_right_,
                      *y + Scaled(kSectionTitleHeight)};
    items_.push_back(std::move(heading));
    *y += Scaled(kSectionTitleHeight) + Scaled(kTitleToCard);
    filter_hits_ = 0;
    (this->*page.build)(y);
    if (filter_hits_ == 0) {
      items_.resize(start);
      *y = start_y;
      continue;
    }
    total += filter_hits_;
  }
  if (total == 0) {
    Item empty;
    empty.kind = ItemKind::kHint;
    empty.text = L"No settings match \u201C" + rail_search_ + L"\u201D.";
    empty.bounds = {content_left_, *y, content_right_,
                    *y + Scaled(kHintHeight)};
    items_.push_back(std::move(empty));
    *y += Scaled(kHintHeight);
  }
}

// The base's hit list is THE rect list (phi_window.h): hover, clicks,
// tooltips and UI Automation all read it and nothing else. This window used
// to hit-test items_ by hand from its own WM_LBUTTONDOWN, which is why it
// had no hover state and answered a screen reader with an empty window.
void PhiSettingsWindow::PublishHits() {
  ClearHits();
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const int content_top = Scaled(kChromeTop) + 1;
  for (size_t i = 0; i < items_.size(); ++i) {
    const Item& item = items_[i];
    if (item.action.empty()) {
      continue;
    }
    // Pane content scrolls UNDER the toolbar and off the bottom; the paint
    // pass clips it there, so the hit list has to as well. PhiHitAt takes
    // the LAST match, and content is added after the toolbar — an unclipped
    // row that had scrolled up would take the pane buttons' clicks.
    RECT bounds = item.bounds;
    if (item.kind != ItemKind::kToolbarItem) {
      if (bounds.bottom <= content_top || bounds.top >= client.bottom) {
        continue;
      }
      bounds.top = std::max<LONG>(bounds.top, content_top);
      bounds.bottom = std::min<LONG>(bounds.bottom, client.bottom);
    }
    std::wstring name = item.text.empty() ? item.detail : item.text;
    if (name.empty()) {
      // A row rect that sits BEHIND its own content is named by the label
      // inside it ("Additional browser settings", the data links).
      for (size_t j = i + 1; j < items_.size(); ++j) {
        if (items_[j].kind == ItemKind::kCard) {
          break;
        }
        if (items_[j].kind == ItemKind::kRowLabel &&
            items_[j].bounds.top >= item.bounds.top &&
            items_[j].bounds.bottom <= item.bounds.bottom) {
          name = items_[j].text;
          break;
        }
      }
    }
    if (name.empty()) {
      // A bare trailing control (a switch, a slider, a pill) is named by
      // the row label it trails, inside the same card.
      for (size_t j = i; j-- > 0;) {
        if (items_[j].kind == ItemKind::kCard) {
          break;
        }
        if (items_[j].kind == ItemKind::kRowLabel) {
          name = items_[j].text;
          break;
        }
      }
    }
    PhiHitRole role = PhiHitRole::kButton;
    switch (item.kind) {
      case ItemKind::kToolbarItem:
        role = PhiHitRole::kTab;
        break;
      case ItemKind::kToggle:
        role = PhiHitRole::kToggle;
        break;
      case ItemKind::kSliderTrack:
        role = PhiHitRole::kSlider;
        break;
      case ItemKind::kListRow:
      case ItemKind::kEmojiCell:
      case ItemKind::kSwatch:
      case ItemKind::kImageCard:
        role = PhiHitRole::kListItem;
        break;
      default:
        break;
    }
    AddHit(bounds, static_cast<int>(i), std::move(name), role,
           item.kind == ItemKind::kToggle ? item.on : item.selected,
           item.enabled);
  }
}

int PhiSettingsWindow::ItemOfHit(int hit) const {
  if (hit < 0 || hit >= static_cast<int>(hits().size())) {
    return -1;
  }
  const int index = hits()[static_cast<size_t>(hit)].key;
  return index >= 0 && index < static_cast<int>(items_.size()) ? index : -1;
}

void PhiSettingsWindow::Paint(HDC dc, const RECT&) {
  if (!host_) {
    return;
  }
  // The Mac's fixedWindowBackground: flat, theme-INdependent — #EAEAEA
  // light / #252525 dark. Only the appearance choice matters here.
  const bool dark = prefs_->Palette().dark;
  const COLORREF bg = dark ? RGB(0x20, 0x20, 0x20) : RGB(0xF3, 0xF3, 0xF3);
  const COLORREF ink = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
  const COLORREF label = Blend(ink, bg, dark ? 230 : 222);
  const COLORREF secondary = Blend(ink, bg, 158);
  const COLORREF tertiary = Blend(ink, bg, 107);
  const COLORREF card_bg = dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xFF, 0xFF, 0xFF);
  const COLORREF card_border = Blend(ink, bg, dark ? 20 : 23);
  const COLORREF separator = Blend(ink, bg, 18);
  // A row divider is drawn ON the card, not on the page: blended against
  // the page ground it came out ~4% over a card already 2% dark, which on
  // a screen is no line at all. AppKit's Divider is nearer 12%.
  const COLORREF rule_ink = Blend(ink, card_bg, 18);
  // The pointer's wash — the one cue every control here lacked.
  const COLORREF hover_wash = Blend(ink, bg, dark ? 15 : 11);
  const COLORREF accent = prefs_->Palette().accent;

  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  HBRUSH page = ::CreateSolidBrush(bg);
  ::FillRect(dc, &client, page);
  ::DeleteObject(page);
  // The toolbar band shares the ground; a separator closes it.
  (void)separator;

  // The caption strip: Phi mark leading, the pane title CENTERED, the
  // close button trailing (Win11's metric, red when hot) — the shared
  // frame's band, at the same metrics the strip used to draw by hand.
  PaintBand(dc);

  ::SetBkMode(dc, TRANSPARENT);
  HGDIOBJ previous_font = font_ ? ::SelectObject(dc, font_) : nullptr;
  bool clipped = false;

  const int hot_item = ItemOfHit(hovered());
  for (size_t item_index = 0; item_index < items_.size(); ++item_index) {
    const Item& item = items_[item_index];
    const bool rail_field = item.kind == ItemKind::kSearchField && item.on;
    if (item.kind != ItemKind::kToolbarItem && !rail_field && !clipped) {
      // Pane content scrolls; nothing of it may paint over the toolbar.
      ::IntersectClipRect(dc, rail_w_, Scaled(kChromeTop), client.right,
                          client.bottom);
      clipped = true;
    }
    RECT bounds = item.bounds;
    const bool hot =
        item.enabled && static_cast<int>(item_index) == hot_item;
    const COLORREF row_ink = item.enabled ? label : Blend(label, bg, 120);
    switch (item.kind) {
      case ItemKind::kToolbarItem: {
        // A rail item (§7): the selected one on ink 6 % with a 3 × 16
        // accent indicator at its leading edge, hover ink 4.5 %, a 16 dip
        // icon and the 14 label (dropped on the narrow rail).
        if (item.selected) {
          FillRounded(dc, bounds, Blend(ink, bg, 15), Scaled(kRadius));
          const int mid = (bounds.top + bounds.bottom) / 2;
          RECT bar = {bounds.left, mid - Scaled(8), bounds.left + Scaled(3),
                      mid + Scaled(8)};
          FillRounded(dc, bar, accent, Scaled(1));
        } else if (hot) {
          FillRounded(dc, bounds, hover_wash, Scaled(kRadius));
        }
        wchar_t mark = 0xE713;
        for (const SectionDef& section : kSections) {
          if (item.payload == section.id) {
            mark = section.glyph;
            break;
          }
        }
        const int icon_px = Scaled(16);
        const int mid_y = (bounds.top + bounds.bottom) / 2;
        const int icon_left = item.text.empty()
                                  ? (bounds.left + bounds.right - icon_px) / 2
                                  : bounds.left + Scaled(12);
        RECT glyph = {icon_left, mid_y - icon_px / 2, icon_left + icon_px,
                      mid_y + icon_px / 2};
        const std::wstring mark_text(1, mark);
        PhiDrawGlyphIcon(dc, glyph, mark_text.c_str(), icon_px, row_ink);
        if (!item.text.empty()) {
          RECT caption = {glyph.right + Scaled(12), bounds.top,
                          bounds.right - Scaled(8), bounds.bottom};
          ::SetTextColor(dc, row_ink);
          PhiDrawTextAA(dc, item.text.c_str(), -1, &caption,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                            DT_END_ELLIPSIS);
        }
        break;
      }
      case ItemKind::kPageTitle:
        if (title_font_) {
          ::SelectObject(dc, title_font_);
        }
        ::SetTextColor(dc, ink);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kCard:
        // A settings card (§7): `card` fill, 1 px stroke, radius 4.
        StrokeRounded(dc, bounds, card_bg, card_border, Scaled(kCardRadius),
                      1);
        break;
      case ItemKind::kSectionTitle:
        if (small_font_) {
          ::SelectObject(dc, small_font_);
        }
        ::SetTextColor(dc, label);  // The group header, 14/600.
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
        ::SetTextColor(dc, item.enabled ? tertiary : Blend(tertiary, bg, 120));
        // Wrapped items were MEASURED at this width, so DT_TOP lands them
        // exactly in the box the layout reserved. DT_WORDBREAK silently
        // disables DT_VCENTER, which is why a one-line item keeps the
        // centred single-line path.
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    item.wrap ? (DT_LEFT | DT_TOP | DT_WORDBREAK |
                                 DT_NOPREFIX)
                              : (DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                 DT_END_ELLIPSIS | DT_NOPREFIX));
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      case ItemKind::kRowLabel:
        ::SetTextColor(dc, row_ink);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    item.wrap ? (DT_LEFT | DT_TOP | DT_WORDBREAK |
                                 DT_NOPREFIX)
                              : (DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                 DT_END_ELLIPSIS | DT_NOPREFIX));
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
        HBRUSH line = ::CreateSolidBrush(rule_ink);
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
          // `on` marks a colour this Space INHERITS from the app-wide
          // choice rather than pins: same dot, a hairline ring in the
          // secondary ink instead of two points of accent.
          const COLORREF ring_color = item.ring ? item.ring : accent;
          Ellipse(dc, ring_rc, card_bg,
                  !item.enabled ? tertiary
                                : (item.on ? secondary : ring_color),
                  Scaled(item.on ? 1 : 2));
        } else if (hot) {
          Ellipse(dc, ring_rc, card_bg, Blend(ink, bg, 46), 1);
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
        } else if (hot) {
          OutlineRounded(dc, image, Blend(ink, bg, 60), Scaled(5), 1);
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
        // The Mac's .mini switch: a 26x15 capsule carrying an 11pt knob
        // inset 2 on every side, so the knob travels its own width.
        const int height = bounds.bottom - bounds.top;
        const int radius = height / 2;
        COLORREF fill = item.on ? accent : Blend(ink, bg, 60);
        if (!item.enabled) {
          fill = Blend(fill, bg, 120);
        }
        if (hot) {
          // A switch has no room for a wash; the halo is the hover cue.
          RECT halo = bounds;
          ::InflateRect(&halo, Scaled(3), Scaled(3));
          FillRounded(dc, halo, hover_wash, (halo.bottom - halo.top) / 2);
        }
        FillRounded(dc, bounds, fill, radius);
        if (!item.on) {
          // The off track is a flat wash a shade lighter than the card
          // border it sits next to; a hairline keeps its ends legible.
          OutlineRounded(dc, bounds, Blend(ink, bg, 30), radius, 1);
        }
        const int inset = Scaled(2);
        const int knob = height - 2 * inset;
        const int left = item.on ? bounds.right - knob - inset
                                 : bounds.left + inset;
        RECT dot = {left, bounds.top + inset, left + knob,
                    bounds.top + inset + knob};
        // The knob wears the Mac's soft shadow as one darker pixel of its
        // own track: a plain white disc had no edge at all against a
        // disabled switch's pale fill.
        Ellipse(dc, dot, RGB(0xFF, 0xFF, 0xFF),
                Blend(RGB(0x00, 0x00, 0x00), fill, 46), 1);
        break;
      }
      case ItemKind::kSliderTrack: {
        // The Mac bar: 10pt tall, full width, rounded r5, a horizontal
        // gradient of the theme overlay's HSB saturation 0.1 -> 0.5 (Pure
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
            tint = HsvToRgb(hue, 0.1 + 0.4 * t, val);
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
        // A ComboBox (§7): the control fill with a 1 px stroke, radius 4.
        StrokeRounded(dc, bounds, Blend(ink, card_bg, hot ? 18 : 11),
                      card_border, Scaled(kRadius), 1);
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
        } else if (hot) {
          FillRounded(dc, bounds, hover_wash, Scaled(kRadius));
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
        if (hot && !item.action.empty()) {
          RECT wash = bounds;
          ::InflateRect(&wash, -Scaled(4), -Scaled(4));
          FillRounded(dc, wash, hover_wash, Scaled(kRadius));
        }
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
        // The sized glyph comes from the cache; it used to be a
        // CreateFontIndirectW / DeleteObject pair PER ITEM PER PAINT
        // (lane 3 ranked finding 12).
        HFONT icon_font =
            item.value > 0 ? IconFont(item.value) : mdl2_font_;
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
        StrokeRounded(dc, bounds, cap_bg,
                      item.on ? accent : (hot ? secondary : rule_ink),
                      Scaled(6), 1);
        ::SetTextColor(dc, item.on ? accent : secondary);
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        break;
      }
      case ItemKind::kSearchField: {
        // Magnifier + the live filter text (placeholder when empty). The
        // rail's field (`on`) draws its own control fill; the Shortcuts
        // one rides its card.
        if (item.on) {
          StrokeRounded(dc, bounds, Blend(ink, bg, item.text.empty() ? 8 : 4),
                        card_border, Scaled(kRadius), 1);
          if (!item.text.empty()) {
            RECT edge = {bounds.left + Scaled(8), bounds.bottom - Scaled(2),
                         bounds.right - Scaled(8), bounds.bottom};
            FillRounded(dc, edge, accent, 1);
          }
        }
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
          PhiDrawTextAA(dc,
                        item.detail.empty() ? L"Find shortcut keys"
                                            : item.detail.c_str(),
                        -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
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
        } else if (hot) {
          FillRounded(dc, bounds, hover_wash, Scaled(10));
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
      case ItemKind::kAvatar: {
        // The account's picture as a circle (premultiplied BGRA from the
        // profile service, decoded once per size), the Contact glyph
        // until there is one; the Mac's 16pt pencil badge at the bottom
        // right when the picture can be edited.
        const int px = bounds.right - bounds.left;
        const std::vector<uint8_t>& circle = account_->AccountAvatarCircle(px);
        if (!circle.empty()) {
          PhiBlendBgra(dc, bounds, circle.data(), px, px);
        } else {
          HFONT icon_font =
              item.value > 0 ? IconFont(item.value) : mdl2_font_;
          if (icon_font) {
            ::SelectObject(dc, icon_font);
          }
          ::SetTextColor(dc, item.enabled ? secondary : tertiary);
          PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (!item.action.empty()) {
          const int badge = Scaled(16);
          RECT badge_rc = {bounds.right - badge, bounds.bottom - badge,
                           bounds.right, bounds.bottom};
          PhiFillEllipseAA(dc, badge_rc, card_bg);
          PhiStrokeEllipseAA(dc, badge_rc, card_border, std::max(1, Scaled(1)));
          if (HFONT pencil = IconFont(9)) {
            ::SelectObject(dc, pencil);
          }
          ::SetTextColor(dc, item.enabled ? secondary : tertiary);
          PhiDrawTextAA(dc, L"\uE70F", -1, &badge_rc,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kEditableLabel: {
        // The Mac's name label: 13pt semibold, a pencil trailing the text
        // (nameEditIconButton, 11pt, 4pt gap); the whole row edits.
        if (title_font_) {
          ::SelectObject(dc, title_font_);
        }
        ::SetTextColor(dc, item.enabled ? row_ink : tertiary);
        RECT measure = bounds;
        PhiDrawTextAA(dc, item.text.c_str(), -1, &measure,
                      DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        const int pencil_w = Scaled(11);
        const int text_w = std::min<int>(measure.right - measure.left,
                                         bounds.right - bounds.left - pencil_w -
                                             Scaled(4));
        RECT text_rc = {bounds.left, bounds.top, bounds.left + text_w,
                        bounds.bottom};
        PhiDrawTextAA(dc, item.text.c_str(), -1, &text_rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        if (!item.action.empty()) {
          RECT pencil_rc = {text_rc.right + Scaled(4), bounds.top,
                            text_rc.right + Scaled(4) + pencil_w, bounds.bottom};
          if (HFONT pencil = IconFont(11)) {
            ::SelectObject(dc, pencil);
          }
          ::SetTextColor(dc, hot ? row_ink : secondary);
          PhiDrawTextAA(dc, L"\uE70F", -1, &pencil_rc,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
      case ItemKind::kProfileCard: {
        // The conch card, themed like the Mac's ProfileCardViewController:
        // the theme's overlay colour for the appearance in force.
        const PhiTheme& theme = PhiThemeById(prefs_->ThemeId());
        profile_card_.SetTheme(dark ? theme.dark.overlay : theme.light.overlay,
                               prefs_->ThemeId() == "pure", dark);
        std::string name;
        account_->AccountInfo(&name, nullptr);
        const std::optional<PhiAccountProfile>& profile =
            account_->Profile().profile();
        profile_card_.SetIdentity(name, profile ? profile->created_at
                                                : std::string());
        profile_card_.Paint(dc, bounds);
        if (profile_card_.animating() && !card_timer_.running()) {
          card_timer_.Start(16, /*coalescable=*/true);
        }
        break;
      }
      case ItemKind::kIconChip: {
        // The Mac's SettingsIconChip: a 24pt rounded square filled with
        // `color.gradient` — SwiftUI's subtle lighter-top, darker-bottom
        // ramp — under a 12pt white glyph. The ramp is painted square and
        // its corners knocked back to the card ground afterwards, the same
        // way the saturation track is.
        COLORREF tint = item.swatch;
        if (!item.enabled) {
          tint = Blend(tint, card_bg, 110);
        }
        const int bands = 12;
        const int span = std::max(1, static_cast<int>(bounds.bottom -
                                                      bounds.top));
        for (int i = 0; i < bands; ++i) {
          RECT band = {bounds.left, bounds.top + span * i / bands,
                       bounds.right, bounds.top + span * (i + 1) / bands};
          // +8% white at the top down to -8% black at the bottom.
          const double t = (i + 0.5) / bands;
          const COLORREF shade =
              t < 0.5 ? Blend(RGB(0xFF, 0xFF, 0xFF), tint,
                              static_cast<int>(20 * (1.0 - 2.0 * t)))
                      : Blend(RGB(0x00, 0x00, 0x00), tint,
                              static_cast<int>(20 * (2.0 * t - 1.0)));
          HBRUSH brush = ::CreateSolidBrush(shade);
          ::FillRect(dc, &band, brush);
          ::DeleteObject(brush);
        }
        PhiKnockOutRoundRectAA(dc, bounds, Scaled(kChipRadius), card_bg);
        if (HFONT chip_font = IconFont(14)) {
          ::SelectObject(dc, chip_font);
        }
        ::SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
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
      case ItemKind::kButton: {
        if (item.selected) {
          FillRounded(dc, bounds, accent, Scaled(kRadius));
        } else {
          StrokeRounded(dc, bounds, Blend(ink, bg, hot ? 24 : 10),
                        card_border, Scaled(kRadius), 1);
        }
        // A destructive button reads in the danger red (§2.3), dimmed
        // like any other when disabled.
        const COLORREF danger = prefs_->Palette().destructive;
        ::SetTextColor(dc, item.selected ? RGB(0xFF, 0xFF, 0xFF)
                           : item.danger   ? (item.enabled
                                                  ? danger
                                                  : Blend(danger, bg, 120))
                                           : row_ink);
        // A leading PUA glyph ("+ New Space") comes from the icon face.
        if (!item.text.empty() && PhiIsPrivateGlyph(item.text.substr(0, 1))) {
          RECT glyph = {bounds.left + Scaled(12), bounds.top,
                        bounds.left + Scaled(28), bounds.bottom};
          if (HFONT icon_font = IconFont(12)) {
            ::SelectObject(dc, icon_font);
          }
          PhiDrawTextAA(dc, item.text.substr(0, 1).c_str(), -1, &glyph,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          if (font_) {
            ::SelectObject(dc, font_);
          }
          RECT rest = {glyph.right + Scaled(4), bounds.top,
                       bounds.right - Scaled(8), bounds.bottom};
          std::wstring tail = item.text.substr(1);
          while (!tail.empty() && tail.front() == L' ') {
            tail.erase(tail.begin());
          }
          PhiDrawTextAA(dc, tail.c_str(), -1, &rest,
                        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
          break;
        }
        PhiDrawTextAA(dc, item.text.c_str(), -1, &bounds,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        break;
      }
      case ItemKind::kAppMark: {
        // The brand mark at the item's size (About).
        const int px = bounds.right - bounds.left;
        if (HICON mark = PhiAppIcon(px)) {
          ::DrawIconEx(dc, bounds.left, bounds.top, mark, px, px, 0, nullptr,
                       DI_NORMAL);
        }
        break;
      }
      case ItemKind::kExpander: {
        // An expander header (§7.1 / §7.2): hover wash, a 28 tile (the
        // Space's icon on an accent-soft square, or the profile's initial
        // on a filled circle), name 14 over hint 12, an optional Default
        // badge, and the chevron.
        if (hot) {
          FillRounded(dc, bounds, hover_wash, Scaled(kRadius));
        }
        const int mid = (bounds.top + bounds.bottom) / 2;
        int x = bounds.left + Scaled(12);
        RECT tile = {x, mid - Scaled(14), x + Scaled(28), mid + Scaled(14)};
        if (item.value == 1) {
          PhiFillEllipseAA(dc, tile, item.swatch);
          if (small_font_) {
            ::SelectObject(dc, small_font_);
          }
          ::SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
          PhiDrawTextAA(dc, item.icon.c_str(), -1, &tile,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        } else {
          FillRounded(dc, tile, Blend(item.swatch, card_bg, 34), Scaled(6));
          RECT glyph_rc = tile;
          ::InflateRect(&glyph_rc, -Scaled(5), -Scaled(5));
          bool drawn = PhiDrawCatalogIcon(dc, glyph_rc, item.icon, Scaled(16),
                                          row_ink);
          if (!drawn && !PhiIsPrivateGlyph(item.icon)) {
            drawn = PhiDrawColorIcon(dc, glyph_rc, item.icon, Scaled(16),
                                     row_ink);
          }
          if (!drawn) {
            HFONT cell_font =
                PhiIsPrivateGlyph(item.icon) ? mdl2_font_ : emoji_font_;
            if (cell_font) {
              ::SelectObject(dc, cell_font);
            }
            ::SetTextColor(dc, row_ink);
            PhiDrawTextAA(dc, item.icon.c_str(), -1, &tile,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          }
        }
        x = tile.right + Scaled(12);
        int text_right = bounds.right - Scaled(40);
        if (item.selected) {
          // The "Default" badge, trailing the text block.
          RECT badge = {text_right - Scaled(60), mid - Scaled(10), text_right,
                        mid + Scaled(10)};
          FillRounded(dc, badge, Blend(ink, bg, 25), Scaled(10));
          if (hint_font_) {
            ::SelectObject(dc, hint_font_);
          }
          ::SetTextColor(dc, secondary);
          PhiDrawTextAA(dc, L"Default", -1, &badge,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          text_right = badge.left - Scaled(12);
        }
        if (font_) {
          ::SelectObject(dc, font_);
        }
        ::SetTextColor(dc, row_ink);
        RECT name = {x, mid - Scaled(18), text_right, mid};
        PhiDrawTextAA(dc, item.text.c_str(), -1, &name,
                      DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        if (hint_font_) {
          ::SelectObject(dc, hint_font_);
        }
        ::SetTextColor(dc, secondary);
        RECT hint = {x, mid + Scaled(2), text_right, mid + Scaled(18)};
        PhiDrawTextAA(dc, item.detail.c_str(), -1, &hint,
                      DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS |
                          DT_NOPREFIX);
        if (mdl2_font_) {
          ::SelectObject(dc, mdl2_font_);
        }
        ::SetTextColor(dc, secondary);
        RECT chevron = {bounds.right - Scaled(36), bounds.top,
                        bounds.right - Scaled(12), bounds.bottom};
        PhiDrawTextAA(dc, item.on ? L"\uE70E" : L"\uE70D", -1, &chevron,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (font_) {
          ::SelectObject(dc, font_);
        }
        break;
      }
    }
  }

  // Panes taller than the window (General, Developer, Shortcuts) scrolled
  // under the wheel with nothing on screen saying so, and no way to tell
  // how far down the content already was.
  const int visible = client.bottom - Scaled(kChromeTop);
  if (content_height_ > visible && visible > 0) {
    const int track_top = Scaled(kChromeTop) + Scaled(6);
    const int track_height = visible - Scaled(12);
    const int thumb_height =
        std::max(Scaled(28),
                 static_cast<int>(static_cast<int64_t>(track_height) *
                                  visible / content_height_));
    const int span = std::max(1, content_height_ - visible);
    const int at = track_top + (track_height - thumb_height) *
                                   std::min(scroll_, span) / span;
    // A Fluent scrollbar: 2 dip at rest, 6 while dragged.
    const int thick = scroll_drag_ ? Scaled(6) : Scaled(2);
    RECT thumb = {client.right - Scaled(4) - thick, at, client.right - Scaled(4),
                  at + thumb_height};
    FillRounded(dc, thumb, Blend(ink, bg, scroll_drag_ ? 90 : 46),
                thick / 2);
    // The drag target is generous: the 14 dip lane the thumb rides in.
    thumb_rect_ = {client.right - Scaled(14), at, client.right, at + thumb_height};
  } else {
    thumb_rect_ = {};
  }

  ::SelectClipRgn(dc, nullptr);
  if (previous_font) {
    ::SelectObject(dc, previous_font);
  }
}

namespace {

// Settings ▸ Developer ▸ "Add skill to…": the Mac's installSkill(for:) and
// installAll(). Every real folder in the way is asked about first; each
// outcome is presented once — one alert per agent from the menu, ONE
// aggregate alert for "All agents".
bool ConfirmSkillOverwrite(HWND owner, const std::wstring& path) {
  return PhiConfirm(
      owner,
      PhiStr("settings.developer.skillInstall.overwriteConfirmation.title"),
      PhiStrF("settings.developer.skillInstall.overwriteConfirmation.message",
              {path})
          .c_str(),
      PhiStr("settings.developer.skillInstall.overwriteConfirmation."
             "replaceButton"),
      /*destructive=*/false);
}

void InstallSkillFor(HWND owner, const PhiSkillTarget& target) {
  const PhiSkillInstallResult result = PhiInstallSkill(
      target, [owner](const std::wstring& path) {
        return ConfirmSkillOverwrite(owner, path);
      });
  switch (result.outcome) {
    case PhiSkillInstallOutcome::kSuccess:
      PhiNotice(
          owner,
          PhiStr("settings.developer.skillInstall.single.successTitle"),
          PhiStrF("settings.developer.skillInstall.single.successMessage",
                  {target.name})
              .c_str());
      break;
    case PhiSkillInstallOutcome::kCancelled:
      break;
    case PhiSkillInstallOutcome::kFailure:
      PhiNotice(owner,
                PhiStr("settings.developer.skillInstall.single.failureTitle"),
                result.error.c_str());
      break;
  }
}

void InstallSkillForAllAgents(HWND owner) {
  std::vector<std::wstring> succeeded;
  std::vector<std::wstring> failed;
  for (const PhiSkillTarget& target : PhiSkillTargets()) {
    const PhiSkillInstallResult result = PhiInstallSkill(
        target, [owner](const std::wstring& path) {
          return ConfirmSkillOverwrite(owner, path);
        });
    switch (result.outcome) {
      case PhiSkillInstallOutcome::kSuccess:
        succeeded.push_back(target.name);
        break;
      case PhiSkillInstallOutcome::kCancelled:
        break;
      case PhiSkillInstallOutcome::kFailure:
        failed.push_back(std::wstring(target.name) + L": " + result.error);
        break;
    }
  }
  if (succeeded.empty() && failed.empty()) {
    return;
  }
  if (failed.empty()) {
    std::wstring names;
    for (const std::wstring& name : succeeded) {
      names += (names.empty() ? L"" : L", ") + name;
    }
    PhiNotice(owner,
              PhiStr("settings.developer.skillInstall.all.successTitle"),
              PhiStrF("settings.developer.skillInstall.all.successMessage",
                      {names})
                  .c_str());
    return;
  }
  std::wstring body;
  for (const std::wstring& line : failed) {
    body += (body.empty() ? L"" : L"\n") + line;
  }
  PhiNotice(owner, PhiStr("settings.developer.skillInstall.all.failureTitle"),
            body.c_str());
}

}  // namespace

void PhiSettingsWindow::Apply(const Item& item, int x) {
  if (item.action == "section") {
    section_ = item.payload;
    scroll_ = 0;
    shortcut_search_.clear();
    rail_search_.clear();
    SyncTitle();
  } else if (item.action == "checkupdate") {
    if (!PhiUpdater::CheckForUpdateWithUi()) {
      PhiNotice(hwnd(), PhiStr("menuBar.checkForUpdate"),
                PhiStr("menuBar.updaterUnavailableBody"));
    }
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
  } else if (item.action == "openindicators") {
    host_->SetShowOpenTabIndicators(!item.on);
  } else if (item.action == "dimunloaded") {
    host_->SetDimUnloadedTabIcons(!item.on);
  } else if (item.action == "fullurl") {
    host_->SetAlwaysShowFullUrl(!item.on);
  } else if (item.action == "restoresession") {
    host_->SetRestoreLastSession(!item.on);
  } else if (item.action == "analytics") {
    // The Mac's metrics_reporting_changed: the consent transition itself,
    // captured BEFORE the switch moves so an opt-out is the last event the
    // collector sees from this install (an opt-in follows the same order
    // and is simply dropped at the gate, as on Mac before identify).
    host_->CaptureAnalytics(kAnalyticsModuleSettings,
                            kEventMetricsReportingChanged,
                            PhiFlagProperties("enabled", !item.on));
    host_->SetAnalyticsEnabled(!item.on);
  } else if (item.action == "peekview") {
    const bool on = !item.on;
    host_->CaptureAnalytics(kAnalyticsModulePeek, kEventPeekSettingChanged,
                            PhiFlagProperties("enabled", on));
    host_->SetPeekViewEnabled(on);
    // The automatic sub-switch follows the master's transitions: switching
    // Peek on re-arms it, switching Peek off stands it down (the Mac's
    // NavigationsSettingsView does exactly this — and its onChange reports
    // the follow-on flip too, when it is one).
    if (prefs_->AutoPeekViewEnabled() != on) {
      host_->CaptureAnalytics(kAnalyticsModulePeek,
                              kEventPeekAutoSettingChanged,
                              PhiFlagProperties("enabled", on));
    }
    host_->SetAutoPeekViewEnabled(on);
  } else if (item.action == "autopeek") {
    host_->CaptureAnalytics(kAnalyticsModulePeek, kEventPeekAutoSettingChanged,
                            PhiFlagProperties("enabled", !item.on));
    host_->SetAutoPeekViewEnabled(!item.on);
  } else if (item.action == "kioskctrlalt") {
    host_->SetOpenKioskOnCtrlAltClick(!item.on);  // The Mac reports nothing.
  } else if (item.action == "devmode") {
    host_->SetDeveloperMode(!item.on);
  } else if (item.action == "agentprofile") {
    host_->SetAgentProfileAllowed(item.payload, !item.on);
  } else if (item.action == "agentcontrol") {
    host_->SetAllowAgentControl(!item.on);
  } else if (item.action == "agentuserspaces") {
    host_->SetAllowAgentUserSpaces(!item.on);
  } else if (item.action == "gospaces") {
    section_ = "spaces";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "gonavigation") {
    section_ = "navigation";
    scroll_ = 0;
    SyncTitle();
  } else if (item.action == "urlrules") {
    // Navigation ▸ "Manage URL Rules…": the universal editor window (the
    // Mac's NavigationsSettingsView row over openURLRulesEditor).
    host_->RequestUrlRulesEditor(hwnd());
  } else if (item.action == "shortcut") {
    PhiShortcutChord chord;
    const std::wstring message =
        L"Press the new keys for \u201C" + item.detail + L"\u201D.";
    if (PhiPromptShortcut(hwnd(), L"Change Shortcut", message.c_str(),
                          &chord)) {
      host_->RebindShortcut(item.payload, chord.vk, chord.ctrl, chord.shift,
                            chord.alt);
    }
  } else if (item.action == "restoreshortcuts") {
    host_->RestoreAllShortcuts();
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
    const std::vector<PhiSearchEngineRow> engines =
        profiles_->SearchEngines(item.payload);
    HMENU menu = ::CreatePopupMenu();
    if (!menu || engines.empty()) {
      if (menu) {
        ::DestroyMenu(menu);
      }
      return;
    }
    std::vector<std::string> keywords;
    for (size_t i = 0; i < engines.size(); ++i) {
      keywords.push_back(engines[i].keyword);
      ::AppendMenuW(menu, MF_STRING | (engines[i].is_default ? MF_CHECKED : 0),
                    static_cast<UINT>(i) + 1,
                    base::UTF8ToWide(engines[i].name).c_str());
    }
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    if (chosen >= 1 && chosen <= keywords.size()) {
      host_->SetSearchEngine(item.payload, keywords[chosen - 1]);
    }
  } else if (item.action == "downloadfolder") {
    const std::wstring folder = PickFolder(
        hwnd(), L"Choose a download location for this profile.");
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
    // No Rebuild() here (or in any other action): OnClick calls
    // ScheduleRebuild() once, after Apply, and that is the only path.
    if (host_) {
      host_->RequestSignIn();
    }
  } else if (item.action == "signout") {
    // The Mac asks first (settings.account.logoutConfirmation); its message
    // promises the sign-in screen, which Windows has none of.
    if (host_ &&
        PhiConfirm(hwnd(), PhiStr("settings.account.logoutConfirmation.title"),
                   PhiStr("settings.account.logoutConfirmation.body"),
                   PhiStr("settings.account.logoutConfirmation.logoutButton"),
                   /*destructive=*/false)) {
      host_->RequestSignOut();
    }
  } else if (item.action == "cancelsignin") {
    account_->RequestCancelSignIn();
  } else if (item.action == "rename") {
    // The Mac's rename dialog: one field, Save, and its two validations.
    std::string current;
    account_->AccountInfo(&current, nullptr);
    std::wstring value = base::UTF8ToWide(current);
    if (!PhiPromptText(hwnd(), PhiStr("settings.account.renameDialog.title"),
                       nullptr, &value,
                       PhiStr("settings.account.renameDialog.saveButton"))) {
      return;
    }
    // Trim; PhiPromptText already refuses an empty entry, a blank one
    // gets the Mac's empty-name alert.
    const size_t first = value.find_first_not_of(L" \t\r\n");
    const size_t last = value.find_last_not_of(L" \t\r\n");
    value = first == std::wstring::npos ? std::wstring()
                                        : value.substr(first, last - first + 1);
    if (value.empty()) {
      PhiNotice(hwnd(),
                PhiStr("settings.account.renameDialog.emptyNameError.title"),
                PhiStr("settings.account.renameDialog.emptyNameError.message"));
      return;
    }
    if (value.size() > static_cast<size_t>(
                           PhiAccountProfileService::kMaxNameLength)) {
      PhiNotice(
          hwnd(),
          PhiStr("settings.account.renameDialog.nameTooLongError.title"),
          PhiStrF("settings.account.renameDialog.nameTooLongError.message",
                  {std::to_wstring(PhiAccountProfileService::kMaxNameLength)})
              .c_str());
      return;
    }
    account_->RequestRenameAccount(base::WideToUTF8(value));
  } else if (item.action == "avatar") {
    account_->RequestEditAvatar();
  } else if (item.action == "downloadcard") {
    profile_card_.ExportPng(hwnd());
  } else if (item.action == "card") {
    // The card's click is its drag (OnMouseDown); nothing on release.
  } else if (item.action == "language") {
    // System default, then every locale the catalog carries, each named in
    // its own language.
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    const std::string current = prefs_->LanguagePreference();
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
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    const bool picked =
        chosen == 1 || (chosen >= 2 && chosen < 2 + locales.size());
    if (picked) {
      const std::string chosen_tag =
          chosen == 1 ? std::string() : locales[chosen - 2];
      if (chosen_tag != current) {
        // The Mac's language_changed: this picker only, the two stored
        // preferences ("system" for follow-the-OS).
        host_->CaptureAnalytics(
            kAnalyticsModuleSettings, kEventLanguageChanged,
            PhiLanguageChangedProperties(current, chosen_tag));
      }
      host_->SetLanguagePreference(chosen_tag);
    }
  } else if (item.action == "installskill") {
    // The Mac's Menu: "All agents", then every agent with its brand mark
    // and skills folder, ticked when that folder already links to THIS
    // build's skill (picking it again refreshes the link). The mark is the
    // row's bitmap, which hides a menu's own check glyph, so the tick is
    // the Mac's trailing ✓ in the label.
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    const std::vector<PhiSkillTarget>& targets = PhiSkillTargets();
    const std::set<std::string> installed = PhiInstalledSkillTargetIds();
    ::AppendMenuW(menu, MF_STRING, 1,
                  PhiStr("settings.developer.skillInstall.allAgentsAction"));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::vector<HBITMAP> bitmaps;
    // 14 dip: the Mac's 15 pt frame around a 14/18 ink fill, so the mark
    // sits a little under the menu text's height rather than over it.
    const int icon_px = Scaled(14);
    for (size_t i = 0; i < targets.size(); ++i) {
      const bool ticked = installed.count(targets[i].id) > 0;
      const std::wstring label = std::wstring(targets[i].name) + L"    " +
                                 PhiSkillDisplayPath(targets[i]) +
                                 (ticked ? L"    \u2713" : L"");
      const UINT id = static_cast<UINT>(i) + 2;
      ::AppendMenuW(menu, MF_STRING | (ticked ? MF_CHECKED : 0), id,
                    label.c_str());
      if (HBITMAP bitmap = PhiSpaceGlyphBitmap(
              PhiAgentIconGlyph(targets[i].icon), icon_px,
              ::GetSysColor(COLOR_MENUTEXT))) {
        bitmaps.push_back(bitmap);
        PhiSetMenuItemBitmap(menu, id, bitmap);
      }
    }
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    for (HBITMAP bitmap : bitmaps) {
      ::DeleteObject(bitmap);
    }
    if (chosen == 1) {
      InstallSkillForAllAgents(hwnd());
    } else if (chosen >= 2 && chosen < 2 + targets.size()) {
      InstallSkillFor(hwnd(), targets[chosen - 2]);
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
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
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
    host_->SetSpaceTheme(item.space, item.payload);
  } else if (item.action == "spaceicon") {
    host_->SetSpaceIcon(item.space, item.payload);
  } else if (item.action == "selectspace") {
    // The expander header: a click folds the open one, opens another.
    selected_space_ = selected_space_ == item.payload ? "" : item.payload;
    picker_open_.clear();
  } else if (item.action == "selectprofile") {
    selected_profile_ =
        selected_profile_ == item.payload ? "" : item.payload;
  } else if (item.action == "spaceiconpicker") {
    picker_open_ = picker_open_ == item.payload ? "" : item.payload;
    picker_scroll_ = 0;
  } else if (item.action == "spaceprofile") {
    // The Profile ComboBox: a menu of the profiles, the bound one checked.
    HMENU menu = ::CreatePopupMenu();
    if (!menu) {
      return;
    }
    std::string bound;
    for (const PhiSpaceSummary& space : spaces_->Summaries()) {
      if (space.id == item.payload) {
        bound = space.profile_id;
      }
    }
    const std::vector<PhiProfileSummary> profiles = profiles_->Profiles();
    UINT id = 1;
    for (const PhiProfileSummary& profile : profiles) {
      ::AppendMenuW(menu, MF_STRING | (profile.id == bound ? MF_CHECKED : 0),
                    id++, base::UTF8ToWide(profile.name).c_str());
    }
    POINT anchor = {item.bounds.left, item.bounds.bottom};
    ::ClientToScreen(hwnd(), &anchor);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x,
        anchor.y, 0, hwnd(), nullptr));
    ::DestroyMenu(menu);
    if (chosen >= 1 && chosen <= profiles.size()) {
      const std::string& target = profiles[chosen - 1].id;
      if (target != bound && !host_->SetSpaceProfile(item.payload, target)) {
        PhiNotice(hwnd(), L"Profile not changed",
                  L"The default Space keeps its profile.");
      }
    }
  } else if (item.action == "pickertab") {
    picker_tab_ = item.payload;
    picker_scroll_ = 0;
  } else if (item.action == "newspace") {
    // The create form in its own window, opened on the selected Space's
    // profile (else the first's) — the Mac's newSpace. The bare name
    // prompt stays only for a Settings window with no browser window
    // behind it to present the new Space in.
    std::string profile_id;
    for (const PhiSpaceSummary& space : spaces_->Summaries()) {
      if (profile_id.empty() || space.id == selected_space_) {
        profile_id = space.profile_id;
      }
    }
    if (!host_->OpenCreateSpaceWindow(profile_id, hwnd())) {
      std::wstring name;
      if (PhiPromptText(hwnd(), L"New Space",
                        L"Enter a name for the new Space.", &name)) {
        host_->CreateSpaceNamed(base::WideToUTF8(name));
      }
    }
  } else if (item.action == "renspace") {
    std::wstring name;
    for (const PhiSpaceSummary& space : spaces_->Summaries()) {
      if (space.id == selected_space_) {
        name = base::UTF8ToWide(space.name);
        break;
      }
    }
    if (PhiPromptText(hwnd(), L"Rename Space",
                      L"Enter a new name for this Space.", &name)) {
      host_->RenameSpace(selected_space_, base::WideToUTF8(name));
    }
  } else if (item.action == "delspace") {
    if (PhiConfirm(hwnd(), PhiStr("dialog.deleteSpace.title"),
                   L"Bookmarks and pinned tabs belonging to this Space will "
                   L"also be removed. This action cannot be undone.",
                   L"Delete") &&
        host_->DeleteSpace(selected_space_)) {
      selected_space_.clear();
    }
  } else if (item.action == "renprofile") {
    std::wstring name;
    for (const PhiProfileSummary& profile : profiles_->Profiles()) {
      if (profile.id == selected_profile_) {
        name = base::UTF8ToWide(profile.name);
        break;
      }
    }
    if (PhiPromptText(hwnd(), L"Rename Profile",
                      L"Enter a new name for this profile.", &name)) {
      host_->RenameProfileById(selected_profile_, base::WideToUTF8(name));
    }
  } else if (item.action == "delprofile") {
    if (PhiConfirm(hwnd(), PhiStr("dialog.deleteProfile.title"),
                   L"All cookies, history, extensions, and saved data on "
                   L"this profile will be permanently removed. This cannot "
                   L"be undone.",
                   L"Delete") &&
        host_->DeleteProfileById(selected_profile_)) {
      selected_profile_.clear();
    }
  } else if (item.action == "addprofile") {
    std::wstring name;
    if (PhiPromptText(hwnd(), L"New Profile",
                      L"Enter a name for the new profile. Each profile has "
                      L"its own cookies, history, and extensions.",
                      &name)) {
      host_->CreateProfileNamed(base::WideToUTF8(name));
    }
  }
}

void PhiSettingsWindow::OnMouseDown(int hit, POINT client_pt) {
  pressed_ = -1;
  dragging_ = false;
  const int index = ItemOfHit(hit);
  if (index < 0 || !items_[static_cast<size_t>(index)].enabled) {
    return;
  }
  pressed_ = index;
  const Item item = items_[static_cast<size_t>(index)];
  // A slider is the one control that acts on the PRESS, and it keeps
  // following the pointer — outside the window too — until the button
  // comes back up. Everything else waits for the release.
  if (item.action == "saturation" || item.action == "spacesat") {
    dragging_ = true;
    drag_item_ = item;
    drag_ = Capture(this);
    Apply(item, client_pt.x);
    ScheduleRebuild();
  } else if (item.kind == ItemKind::kProfileCard) {
    // The card is dragged through its grid, and springs back on release.
    card_drag_ = true;
    drag_ = Capture(this);
    profile_card_.BeginDrag(client_pt);
    Invalidate();
  }
}

void PhiSettingsWindow::OnClick(int hit, POINT client_pt) {
  const int pressed = pressed_;
  pressed_ = -1;
  if (dragging_) {
    dragging_ = false;
    drag_.Release();
    return;
  }
  if (card_drag_) {
    card_drag_ = false;
    drag_.Release();
    profile_card_.EndDrag();
    if (profile_card_.animating() && !card_timer_.running()) {
      card_timer_.Start(16, /*coalescable=*/true);
    }
    return;
  }
  // Press and release must land on the SAME control: the old handler fired
  // from WM_LBUTTONDOWN and then again from every WM_MOUSEMOVE that still
  // had the button down, so dragging across the pane flipped every switch
  // the pointer crossed.
  const int index = ItemOfHit(hit);
  if (index < 0 || index != pressed) {
    return;
  }
  // By value: Apply() may open a modal menu or prompt whose nested loop
  // rebuilds items_ underneath us.
  const Item item = items_[static_cast<size_t>(index)];
  if (item.action.empty() || !item.enabled) {
    return;
  }
  Apply(item, client_pt.x);
  ScheduleRebuild();
}

void PhiSettingsWindow::OnWheel(int delta, POINT client_pt) {
  // Pane content can outgrow the fixed window; the Mac scrolls, so do we.
  // The offset feeds Rebuild(), so the scroll is a rebuild — not something
  // WM_PAINT discovers.
  if (picker_max_line_ > 0 && ::PtInRect(&picker_grid_, client_pt)) {
    // Over the icon picker: three cell rows per notch, in place.
    const int rows = std::max(1, 3 * std::abs(delta) / WHEEL_DELTA);
    picker_scroll_ =
        std::max(0, std::min(picker_max_line_,
                             picker_scroll_ + (delta > 0 ? -rows : rows)));
    ScheduleRebuild();
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd(), &client);
  const int visible = client.bottom - Scaled(kChromeTop);
  const int max_scroll = std::max(0, content_height_ - visible);
  scroll_ = std::max(0, std::min(max_scroll, scroll_ - delta / 2));
  ScheduleRebuild();
}

void PhiSettingsWindow::OnEscape() {
  // Esc walks back out, one step at a time — the filter, then the window
  // (which only hides, as WM_CLOSE does).
  if (!shortcut_search_.empty() || !rail_search_.empty()) {
    shortcut_search_.clear();
    rail_search_.clear();
    scroll_ = 0;
    ScheduleRebuild();
    return;
  }
  ::ShowWindow(hwnd(), SW_HIDE);
}

LRESULT PhiSettingsWindow::OnWindowMessage(UINT msg,
                                        WPARAM wparam,
                                        LPARAM lparam,
                                        bool* handled) {
  switch (msg) {
    case WM_GETMINMAXINFO: {
      // Never under 760 × 560 (§7).
      MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize.x = Scaled(kMinWidth);
      info->ptMinTrackSize.y = Scaled(kMinHeight);
      *handled = true;
      return 0;
    }
    case WM_SIZE:
      // The rail's breakpoint and the page column follow the client rect.
      ScheduleRebuild();
      break;
    case WM_EXITSIZEMOVE: {
      // A resize by hand is remembered (dip, content under the caption);
      // maximize is a window state, not a size, and stays out of it.
      if (!::IsZoomed(hwnd()) && !::IsIconic(hwnd())) {
        RECT client = {};
        ::GetClientRect(hwnd(), &client);
        const int width = ::MulDiv(client.right, 96, static_cast<int>(dpi()));
        const int height =
            ::MulDiv(client.bottom, 96, static_cast<int>(dpi())) - kChromeTop;
        if (width >= kMinWidth && height >= kMinHeight) {
          host_->SetSettingsWindowSize(width, height);
        }
      }
      break;
    }
    case WM_LBUTTONDOWN: {
      const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (thumb_rect_.right > thumb_rect_.left && ::PtInRect(&thumb_rect_, pt)) {
        scroll_drag_ = true;
        scroll_drag_grab_ = pt.y - thumb_rect_.top;
        ::SetCapture(hwnd());
        Invalidate();
        *handled = true;
        return 0;
      }
      break;
    }
    case WM_LBUTTONUP:
      if (scroll_drag_) {
        scroll_drag_ = false;
        if (::GetCapture() == hwnd()) {
          ::ReleaseCapture();
        }
        Invalidate();
        *handled = true;
        return 0;
      }
      break;
    case WM_MOUSEMOVE:
      if (scroll_drag_) {
        RECT client = {};
        ::GetClientRect(hwnd(), &client);
        const int visible = client.bottom - Scaled(kChromeTop);
        const int track_top = Scaled(kChromeTop) + Scaled(6);
        const int track_height = visible - Scaled(12);
        const int thumb_height = thumb_rect_.bottom - thumb_rect_.top;
        const int span = std::max(1, track_height - thumb_height);
        const int max_scroll = std::max(0, content_height_ - visible);
        const int y = GET_Y_LPARAM(lparam) - scroll_drag_grab_ - track_top;
        scroll_ = std::max(0, std::min(max_scroll, max_scroll * y / span));
        ScheduleRebuild();
        *handled = true;
        return 0;
      }
      // Only a slider drag is answered here; every other move belongs to
      // the base, which updates the hover the paint pass reads. The drag
      // replays the item CAPTURED on the press, not whatever happens to
      // sit under the pointer now.
      if (dragging_ && (wparam & MK_LBUTTON)) {
        Apply(drag_item_, GET_X_LPARAM(lparam));
        ScheduleRebuild();
        *handled = true;
        return 0;
      }
      if (card_drag_ && (wparam & MK_LBUTTON)) {
        if (profile_card_.Drag({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)})) {
          Invalidate();
        }
        *handled = true;
        return 0;
      }
      break;
    case kMsgProfileCardFrame:
      // A frame decoded: repaint, and let the intro start if it can.
      Invalidate();
      if (profile_card_.animating() && !card_timer_.running()) {
        card_timer_.Start(16, /*coalescable=*/true);
      }
      *handled = true;
      return 0;
    case WM_SETCURSOR: {
      // The house rule (phi_popup_chrome.cc): a hand for links, the arrow
      // for everything else — the hover wash is the cue there.
      const int index = ItemOfHit(hovered());
      if (LOWORD(lparam) == HTCLIENT && index >= 0 &&
          items_[static_cast<size_t>(index)].kind == ItemKind::kLink &&
          items_[static_cast<size_t>(index)].enabled) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
        *handled = true;
        return TRUE;
      }
      break;
    }
    case WM_CHAR:
      // Typing anywhere feeds a filter: the Shortcuts pane's own field,
      // or the rail's "Find a setting" on every other page — Esc clears,
      // Backspace deletes.
      {
        std::wstring& target =
            section_ == "shortcuts" ? shortcut_search_ : rail_search_;
        const wchar_t c = static_cast<wchar_t>(wparam);
        // Escape arrives as WM_KEYDOWN first and OnEscape() has already
        // cleared the filter by now; swallow the character it leaves.
        if (c == VK_ESCAPE) {
          *handled = true;
          return 0;
        }
        if (c == VK_BACK) {
          if (!target.empty()) {
            target.pop_back();
          }
        } else if (c >= L' ') {
          target.push_back(c);
        } else {
          *handled = true;
          return 0;
        }
        scroll_ = 0;
        ScheduleRebuild();
      }
      *handled = true;
      return 0;
    case WM_ACTIVATE:
      // The "Set as default" button hands the choice to the Windows
      // Settings app, so the answer changes while this window is in the
      // background. Coming back to the front is the cue to ask again —
      // the Mac re-checks in viewWillAppear for the same reason. This is
      // the ONLY place the registry probe is dropped; everywhere else it
      // is served from the cache. Falls through so the frame and
      // DefWindowProc still see the activation.
      if (LOWORD(wparam) != WA_INACTIVE) {
        is_default_browser_.reset();
        // The avatar editor and the hosted login are tabs; coming back is
        // the cue to re-read the profile (rate-limited in the service).
        account_->RefreshAccountProfile(/*force=*/false);
        ScheduleRebuild();
      }
      break;
    case WM_CLOSE:
      ::ShowWindow(hwnd(), SW_HIDE);  // Keep it around; reopening is instant.
      *handled = true;
      return 0;
    default:
      break;
  }
  return PhiSecondaryWindow::OnWindowMessage(msg, wparam, lparam, handled);
}

void PhiSettingsWindow::OnDpiChanged(UINT old_dpi) {
  PhiSecondaryWindow::OnDpiChanged(old_dpi);
  EnsureFont();
  ScheduleRebuild();
}

void PhiSettingsWindow::OnTimerFired(UINT_PTR id) {
  if (id == card_timer_.id()) {
    if (profile_card_.Tick()) {
      Invalidate();
    }
    if (!profile_card_.animating()) {
      card_timer_.Stop();
    }
    return;
  }
  if (id != profile_poll_.id()) {
    return;
  }
  // Re-read the pending profile; the rebuild re-arms while pending.
  profile_poll_.Stop();
  ScheduleRebuild();
}

void PhiSettingsWindow::OnCaptureLost() {
  scroll_drag_ = false;
  // A stolen capture ends the card drag where the release would have.
  if (card_drag_) {
    card_drag_ = false;
    profile_card_.EndDrag();
    if (profile_card_.animating() && !card_timer_.running()) {
      card_timer_.Start(16, /*coalescable=*/true);
    }
  }
  dragging_ = false;
}

void PhiSettingsWindow::OnNcDestroy() {
  profile_poll_.Stop();
  card_timer_.Stop();
  PhiSecondaryWindow::OnNcDestroy();
}

}  // namespace phi_client
