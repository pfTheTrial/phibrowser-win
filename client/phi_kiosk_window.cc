// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_kiosk_window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/values.h"
#include "chrome/browser/phinomenon/phi_framework/phi_framework_api.h"
#include "phibrowser-win/client/phi_coordinator.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_extensions_popover.h"
#include "phibrowser-win/client/phi_glyph_bitmap.h"
#include "phibrowser-win/client/phi_i18n.h"
#include "phibrowser-win/client/phi_icons.h"
#include "phibrowser-win/client/phi_new_tab_page.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

namespace {

constexpr wchar_t kKioskClassName[] = L"PhiKioskShell";

// Toolbar controls, in the order they are drawn and hit-tested. The window
// controls are NOT here: they arrive as non-client messages and ride
// `hot_caption_` instead.
enum HotControl {
  kHotBack = 0,
  kHotExtensions,
  kHotCopy,
  kHotSpaceOpen,
  kHotSpaceList,
};

// Design metrics (DIP), from the Mac's KioskBrowserToolbar.
constexpr int kBandHeight = 52;          // preferredHeight.
constexpr int kCaptionButtonWidth = 46;  // The Win11 caption button metric.
// The Mac clears the traffic lights with 78 leading points; the window
// controls are on the trailing edge here, so the band opens on the plain
// gutter the Mac gives its trailing control.
constexpr int kEdge = 12;
constexpr int kNavButton = 24;
constexpr int kCapsuleHeight = 32;  // addressBarHeight.
constexpr int kCapsuleInset = 5;    // The chip's and the actions' own inset.
constexpr int kChipHeight = 26;
constexpr int kChipMaxWidth = 138;
constexpr int kChipPad = 8;
constexpr int kAccessory = 24;  // The extensions / copy slots.
constexpr int kAccessoryGap = 2;
constexpr int kSpaceMenuHeight = 32;
constexpr int kSpaceMenuMaxWidth = 220;  // spaceMenuMaximumWidth.
constexpr int kSpaceChevron = 32;        // menuButtonWidth.
constexpr int kSpaceIcon = 12;           // iconSize.
constexpr int kSpacePad = 6;
constexpr int kGap = 8;
// The trailing cluster's breath before the window controls, as the shell's
// band gives its own (kBandTrailingGap).
constexpr int kCaptionGap = 12;
// Below this the capsule is too narrow to hold anything but the domain.
constexpr int kCapsuleContentsMinimum = 132;

// Opening size, when the framework gives us no better hint. The Mac restores
// an autosaved frame here; that is not ported yet, so every Kiosk opens at
// this size, centered on the work area.
constexpr int kInitialWidth = 980;
constexpr int kInitialHeight = 680;

// Segoe MDL2 glyphs, written as escapes: the literal private-use characters
// the first Peek panel used landed in the file as EMPTY string literals.
constexpr wchar_t kGlyphBack[] = L"\uE72B";
constexpr wchar_t kGlyphProfile[] = L"\uE77B";  // Contact.
constexpr wchar_t kGlyphExtensions[] = L"\uEA86";  // Puzzle.
constexpr wchar_t kGlyphCopy[] = L"\uE71B";  // Link.
constexpr wchar_t kGlyphCopied[] = L"\uE73E";  // Checkmark.
constexpr wchar_t kGlyphChevron[] = L"\uE70D";
constexpr wchar_t kGlyphMinimize[] = L"\uE921";
constexpr wchar_t kGlyphMaximize[] = L"\uE922";
constexpr wchar_t kGlyphRestore[] = L"\uE923";
constexpr wchar_t kGlyphClose[] = L"\uE8BB";

// Reverts the copy button's checkmark, a second after the copy.
constexpr UINT_PTR kCopyFlashTimer = 1;

std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }
  const int size = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr,
                                         0);
  std::wstring out(static_cast<size_t>(size), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                        static_cast<int>(utf8.size()), out.data(), size);
  return out;
}

std::string Narrow(const std::wstring& wide) {
  if (wide.empty()) {
    return std::string();
  }
  const int size =
      ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr,
                            nullptr);
  std::string out(static_cast<size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                        static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
}

// A rounded fill through the anti-aliased path, GDI's stair-stepping
// RoundRect standing in where Direct2D is unavailable.
void FillRounded(HDC dc, const RECT& rc, COLORREF fill, int radius) {
  PhiFillRoundRectAA(dc, rc, radius, fill);
}

void FillCapsule(HDC dc, const RECT& rc, COLORREF fill) {
  FillRounded(dc, rc, fill, (rc.bottom - rc.top) / 2);
}

void StrokeCapsule(HDC dc, const RECT& rc, COLORREF ink, int width) {
  PhiStrokeCapsuleAA(dc, rc, ink, width);
}

// A Space's icon the way the sidebar draws it: the catalog's own artwork,
// colour emoji through DirectWrite, the font glyph as the last resort.
void DrawSpaceIcon(HDC dc,
                   const RECT& rc,
                   const std::wstring& icon,
                   int size,
                   COLORREF ink,
                   HFONT glyph_font,
                   HFONT text_font) {
  if (icon.empty()) {
    return;
  }
  if (PhiDrawCatalogIcon(dc, rc, icon, size, ink)) {
    return;
  }
  const bool private_glyph = PhiIsPrivateGlyph(icon);
  if (!private_glyph && PhiDrawColorIcon(dc, rc, icon, size, ink)) {
    return;
  }
  HFONT font = private_glyph ? glyph_font : text_font;
  HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
  ::SetTextColor(dc, ink);
  RECT box = rc;
  PhiDrawTextAA(dc, icon.c_str(), -1, &box,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (previous) {
    ::SelectObject(dc, previous);
  }
}

ATOM EnsureClass() {
  static const ATOM atom = [] {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PhiKioskWindow::WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = PhiAppIcon(32);
    wc.hIconSm = PhiAppIcon(16);
    // As in the shell: no erase, the paint covers everything.
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kKioskClassName;
    return ::RegisterClassExW(&wc);
  }();
  return atom;
}

}  // namespace

PhiKioskWindow::PhiKioskWindow(int64_t window_id,
                               HWND browser_hwnd,
                               phi_framework::PhiFramework* framework,
                               PhiCoordinator* coordinator)
    : window_id_(BrowserId{window_id}),
      browser_hwnd_(browser_hwnd),
      framework_(framework),
      coordinator_(coordinator) {
  EnsureClass();

  RECT work = {0, 0, kInitialWidth, kInitialHeight};
  ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int dpi = ::GetDpiForSystem();
  const int width = ::MulDiv(kInitialWidth, dpi, 96);
  const int height = ::MulDiv(kInitialHeight, dpi, 96);
  const int x = work.left + ((work.right - work.left) - width) / 2;
  const int y = work.top + ((work.bottom - work.top) - height) / 2;

  hwnd_ = ::CreateWindowExW(0, kKioskClassName, L"Phi",
                            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width,
                            height, nullptr, nullptr,
                            ::GetModuleHandleW(nullptr), this);
  if (!hwnd_) {
    LOG(ERROR) << "[PhiClient] kiosk CreateWindowEx failed err="
               << ::GetLastError();
    return;
  }
  dpi_ = ::GetDpiForWindow(hwnd_);

  // One frame recalculation now that the proc can see `this`: whatever
  // non-client layout the creation pass produced, this pass runs through OUR
  // WM_NCCALCSIZE and retires the OS caption, so the band below is the only
  // title bar this window has (the shell does the same).
  ::SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                     SWP_FRAMECHANGED);

  const DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &corner,
                          sizeof(corner));

  // Same adoption the shell performs: strip Chromium's top-level chrome and
  // clip its HWND into our content rect, so it stops being its own taskbar
  // and Alt-Tab entry.
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    LONG_PTR style = ::GetWindowLongPtrW(browser_hwnd_, GWL_STYLE);
    style &= ~(WS_POPUP | WS_OVERLAPPEDWINDOW | WS_CAPTION | WS_THICKFRAME |
               WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    style |= WS_CHILD | WS_CLIPSIBLINGS;
    ::SetWindowLongPtrW(browser_hwnd_, GWL_STYLE, style);
    LONG_PTR ex = ::GetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE);
    ex &= ~(WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_DLGMODALFRAME |
            WS_EX_STATICEDGE | WS_EX_APPWINDOW);
    ::SetWindowLongPtrW(browser_hwnd_, GWL_EXSTYLE, ex);
    ::SetParent(browser_hwnd_, hwnd_);
    ::SetWindowPos(browser_hwnd_, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_FRAMECHANGED);
  }

  RefreshProfile();
  RefreshState();
  PositionContent();
  ::ShowWindow(hwnd_, SW_SHOW);
  ::SetForegroundWindow(hwnd_);
}

PhiKioskWindow::~PhiKioskWindow() {
  // The browser HWND belongs to Chromium; hand it back to the desktop before
  // this window dies so its teardown is not reparented into a dead HWND.
  if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
    ::SetParent(browser_hwnd_, nullptr);
  }
  if (hwnd_ && ::IsWindow(hwnd_)) {
    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    ::DestroyWindow(hwnd_);
  }
  for (HFONT font : {ui_font_, ui_strong_font_, glyph_font_, caption_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
}

void PhiKioskWindow::RefreshState() {
  if (!framework_) {
    return;
  }
  const char* json = framework_->NavStateJson(window_id_.value);
  std::optional<base::DictValue> nav =
      json ? base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC)
           : std::nullopt;
  if (nav) {
    if (const std::string* url = nav->FindString("url")) {
      url_ = Widen(*url);
    }
    if (const std::string* title = nav->FindString("title")) {
      title_ = Widen(*title);
    }
    can_go_back_ = nav->FindBool("canGoBack").value_or(false);
  }
  RefreshSpaces();
  MeasureLabels();
  if (hwnd_ && ::IsWindow(hwnd_)) {
    // The page title names the window in the taskbar, as on the Mac.
    ::SetWindowTextW(hwnd_, title_.empty() ? L"Phi" : title_.c_str());
    ::InvalidateRect(hwnd_, nullptr, FALSE);
  }
}

void PhiKioskWindow::RefreshProfile() {
  if (!framework_) {
    return;
  }
  // The framework hands back a pointer into its own buffer: parse it now.
  const char* json = framework_->WindowProfileJson(window_id_.value);
  std::optional<base::DictValue> profile =
      json ? base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC)
           : std::nullopt;
  if (!profile) {
    return;
  }
  if (const std::string* name = profile->FindString("name")) {
    profile_name_ = Widen(*name);
    MeasureLabels();
  }
}

void PhiKioskWindow::RefreshSpaces() {
  spaces_.clear();
  target_space_id_.clear();
  if (!coordinator_) {
    return;
  }
  for (const PhiSpaceView& space : coordinator_->Spaces()) {
    SpaceRow row;
    row.id = space.id;
    row.name = Widen(!space.name.empty() ? space.name : "Space " + space.id);
    row.icon = Widen(space.icon_text);
    spaces_.push_back(std::move(row));
  }
  target_space_id_ = coordinator_->KioskTargetSpaceId();
}

const PhiKioskWindow::SpaceRow* PhiKioskWindow::TargetSpace() const {
  for (const SpaceRow& row : spaces_) {
    if (row.id == target_space_id_) {
      return &row;
    }
  }
  return spaces_.empty() ? nullptr : &spaces_.front();
}

const PhiPalette& PhiKioskWindow::Palette() const {
  // A Kiosk belongs to no Space, so PaletteForSlot falls through to the
  // app-wide palette — which is what this window should follow.
  static const PhiPalette kFallback =
      PhiResolvePalette(PhiThemeById("pure"), PhiAppearance::kLight);
  return coordinator_ ? coordinator_->PaletteForSlot(window_id_.value)
                      : kFallback;
}

void PhiKioskWindow::EnsureFonts() {
  if (font_dpi_ == dpi_ && ui_font_) {
    return;
  }
  NONCLIENTMETRICSW metrics = {};
  metrics.cbSize = sizeof(metrics);
  if (!::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics),
                                    &metrics, 0, static_cast<UINT>(dpi_))) {
    return;  // Keep whatever we had rather than falling back to no font.
  }
  // The face tracks the system (Segoe UI); the sizes are the Mac's — 13pt
  // for the domain and the chip labels.
  LOGFONTW ui = metrics.lfMessageFont;
  ui.lfHeight = -Scaled(13);
  HFONT ui_built = ::CreateFontIndirectW(&ui);
  ui.lfWeight = FW_SEMIBOLD;
  HFONT strong_built = ::CreateFontIndirectW(&ui);

  LOGFONTW glyph = metrics.lfMessageFont;
  // The unsafe-libc-call gate rules out wcscpy_s; copy the face name directly.
  const std::wstring glyph_face = L"Segoe MDL2 Assets";
  std::fill(std::begin(glyph.lfFaceName), std::end(glyph.lfFaceName), L'\0');
  std::copy_n(glyph_face.begin(),
              std::min(glyph_face.size(), std::size(glyph.lfFaceName) - 1),
              std::begin(glyph.lfFaceName));
  glyph.lfHeight = -Scaled(12);
  HFONT glyph_built = ::CreateFontIndirectW(&glyph);
  // The window controls wear the OS buttons' own glyph size.
  glyph.lfHeight = -Scaled(10);
  HFONT caption_built = ::CreateFontIndirectW(&glyph);

  if (!ui_built || !strong_built || !glyph_built || !caption_built) {
    for (HFONT font : {ui_built, strong_built, glyph_built, caption_built}) {
      if (font) {
        ::DeleteObject(font);
      }
    }
    return;
  }
  for (HFONT font : {ui_font_, ui_strong_font_, glyph_font_, caption_font_}) {
    if (font) {
      ::DeleteObject(font);
    }
  }
  ui_font_ = ui_built;
  ui_strong_font_ = strong_built;
  glyph_font_ = glyph_built;
  caption_font_ = caption_built;
  font_dpi_ = dpi_;
}

int PhiKioskWindow::TextWidth(HFONT font, const std::wstring& text) {
  if (!hwnd_ || text.empty()) {
    return 0;
  }
  HDC dc = ::GetDC(hwnd_);
  if (!dc) {
    return 0;
  }
  HGDIOBJ previous = font ? ::SelectObject(dc, font) : nullptr;
  SIZE size = {};
  ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()),
                          &size);
  if (previous) {
    ::SelectObject(dc, previous);
  }
  ::ReleaseDC(hwnd_, dc);
  return size.cx;
}

void PhiKioskWindow::MeasureLabels() {
  EnsureFonts();
  if (!ui_font_ || !ui_strong_font_) {
    return;
  }
  profile_text_w_ = TextWidth(ui_strong_font_, profile_name_);
  space_prefix_w_ = TextWidth(ui_font_, PhiStr("kiosk.toolbar.openInSpace"));
  const SpaceRow* target = TargetSpace();
  space_name_w_ = target ? TextWidth(ui_strong_font_, target->name) : 0;
}

std::wstring PhiKioskWindow::DisplayUrl() const {
  if (url_.empty() || url_ == L"about:blank" ||
      url_.rfind(L"chrome-extension:", 0) == 0 ||
      PhiNewTabPage::IsNewTabUrl(Narrow(url_))) {
    return std::wstring();
  }
  const size_t scheme = url_.find(L"://");
  if (scheme == std::wstring::npos) {
    return url_;  // No host to show: the Mac shows the string as it stands.
  }
  std::wstring host = url_.substr(scheme + 3);
  const size_t path = host.find_first_of(L"/?#");
  if (path != std::wstring::npos) {
    host = host.substr(0, path);
  }
  if (host.rfind(L"www.", 0) == 0) {
    host = host.substr(4);
  }
  return host.empty() ? url_ : host;
}

PhiKioskWindow::Layout PhiKioskWindow::ComputeLayout() {
  Layout out = {};
  RECT client = {};
  if (hwnd_) {
    ::GetClientRect(hwnd_, &client);
  }
  if (font_dpi_ != dpi_) {
    MeasureLabels();  // Builds this DPI's fonts, then re-measures on them.
  }

  const int bar = Scaled(kBandHeight);
  const int mid = bar / 2;
  const int btn = Scaled(kNavButton);
  const int cap = Scaled(kCaptionButtonWidth);

  out.band = {0, 0, client.right, bar};
  out.content = {0, bar, client.right, client.bottom};

  // The window controls close the band, minimize / maximize / close in the
  // system's own order.
  out.caption_close = {client.right - cap, 0, client.right, bar};
  out.caption_max = {out.caption_close.left - cap, 0, out.caption_close.left,
                     bar};
  out.caption_min = {out.caption_max.left - cap, 0, out.caption_max.left, bar};

  // "Open in <Space>" leads them: prefix, the Space's icon, its name, then
  // the chevron half that lists the others.
  int trailing = out.caption_min.left - Scaled(kCaptionGap);
  if (TargetSpace()) {
    const int menu_h = Scaled(kSpaceMenuHeight);
    int width = Scaled(kSpacePad) + space_prefix_w_ + Scaled(kSpacePad) +
                Scaled(kSpaceIcon) + Scaled(kSpacePad) + space_name_w_ +
                Scaled(kSpacePad) + Scaled(kSpaceChevron);
    width = (std::min)(width, Scaled(kSpaceMenuMaxWidth));
    out.space_menu = {trailing - width, mid - menu_h / 2, trailing,
                      mid + menu_h / 2};
    out.space_chevron = {out.space_menu.right - Scaled(kSpaceChevron),
                         out.space_menu.top, out.space_menu.right,
                         out.space_menu.bottom};
    out.space_primary = {out.space_menu.left, out.space_menu.top,
                         out.space_chevron.left, out.space_menu.bottom};
    trailing = out.space_menu.left - Scaled(kGap);
  }

  int x = Scaled(kEdge);
  if (can_go_back_) {
    out.back = {x, mid - btn / 2, x + btn, mid + btn / 2};
    x = out.back.right + Scaled(kGap);
  }
  const int capsule_h = Scaled(kCapsuleHeight);
  out.address = {x, mid - capsule_h / 2, (std::max)(x, trailing),
                 mid + capsule_h / 2};
  out.url = {out.address.left + Scaled(kChipPad), out.address.top,
             out.address.right - Scaled(kChipPad), out.address.bottom};

  if (out.address.right - out.address.left < Scaled(kCapsuleContentsMinimum)) {
    return out;  // Too narrow for anything but the domain.
  }
  const int inset = Scaled(kCapsuleInset);
  const int slot = Scaled(kAccessory);
  out.copy = {out.address.right - inset - slot, mid - slot / 2,
              out.address.right - inset, mid + slot / 2};
  out.extensions = {out.copy.left - Scaled(kAccessoryGap) - slot, out.copy.top,
                    out.copy.left - Scaled(kAccessoryGap), out.copy.bottom};

  const int chip_h = Scaled(kChipHeight);
  const int chip_left = out.address.left + inset;
  int chip_w = Scaled(kChipPad) + Scaled(kSpaceIcon) + Scaled(kSpacePad) +
               profile_text_w_ + Scaled(kChipPad);
  chip_w = (std::min)(chip_w, Scaled(kChipMaxWidth));
  // Never past the actions: the domain keeps a readable run between them.
  chip_w = (std::min)(chip_w, static_cast<int>(out.extensions.left) -
                                  Scaled(kSpacePad) - chip_left - Scaled(48));
  if (!profile_name_.empty() && chip_w > 0) {
    out.profile = {chip_left, mid - chip_h / 2, chip_left + chip_w,
                   mid + chip_h / 2};
    out.url.left = out.profile.right + Scaled(kSpacePad);
  }
  out.url.right = out.extensions.left - Scaled(kSpacePad);
  return out;
}

void PhiKioskWindow::PositionContent() {
  if (!browser_hwnd_ || !::IsWindow(browser_hwnd_)) {
    return;
  }
  const Layout layout = ComputeLayout();
  ::SetWindowPos(browser_hwnd_, nullptr, layout.content.left,
                 layout.content.top, layout.content.right - layout.content.left,
                 layout.content.bottom - layout.content.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void PhiKioskWindow::Paint() {
  PAINTSTRUCT ps = {};
  HDC dc = ::BeginPaint(hwnd_, &ps);
  if (!dc) {
    return;
  }
  RECT client = {};
  ::GetClientRect(hwnd_, &client);

  const PhiPalette palette = Palette();

  // Back-buffer: the toolbar sits directly against the accelerated web
  // surface, and painting it straight to the window tears while resizing.
  HDC mem = ::CreateCompatibleDC(dc);
  HBITMAP bmp = ::CreateCompatibleBitmap(dc, client.right, client.bottom);
  HGDIOBJ old_bmp = ::SelectObject(mem, bmp);

  const Layout layout = ComputeLayout();
  HBRUSH bar_brush = ::CreateSolidBrush(palette.window_bg);
  ::FillRect(mem, &layout.band, bar_brush);
  ::DeleteObject(bar_brush);

  // Hairline under the band, separating chrome from page.
  RECT rule = {0, layout.band.bottom - 1, client.right, layout.band.bottom};
  HBRUSH rule_brush = ::CreateSolidBrush(palette.separator);
  ::FillRect(mem, &rule, rule_brush);
  ::DeleteObject(rule_brush);

  ::SetBkMode(mem, TRANSPARENT);

  const auto draw_glyph = [&](const RECT& rc, const wchar_t* glyph,
                              COLORREF ink) {
    ::SetTextColor(mem, ink);
    RECT box = rc;
    PhiDrawTextAA(mem, glyph, -1, &box,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  };

  // The window controls: the hot fill first (close turns the Win11 red),
  // then the OS buttons' own glyphs.
  if (hot_caption_ == HTMINBUTTON || hot_caption_ == HTMAXBUTTON) {
    const RECT& hot_rc =
        hot_caption_ == HTMINBUTTON ? layout.caption_min : layout.caption_max;
    HBRUSH hover_brush = ::CreateSolidBrush(palette.hover);
    ::FillRect(mem, &hot_rc, hover_brush);
    ::DeleteObject(hover_brush);
  } else if (hot_caption_ == HTCLOSE) {
    HBRUSH close_brush = ::CreateSolidBrush(RGB(0xC4, 0x2B, 0x1C));
    ::FillRect(mem, &layout.caption_close, close_brush);
    ::DeleteObject(close_brush);
  }
  HGDIOBJ previous_font =
      caption_font_ ? ::SelectObject(mem, caption_font_) : nullptr;
  const bool zoomed = hwnd_ && ::IsZoomed(hwnd_);
  draw_glyph(layout.caption_min, kGlyphMinimize, palette.label);
  draw_glyph(layout.caption_max, zoomed ? kGlyphRestore : kGlyphMaximize,
             palette.label);
  draw_glyph(layout.caption_close, kGlyphClose,
             hot_caption_ == HTCLOSE ? RGB(0xFF, 0xFF, 0xFF) : palette.label);
  if (previous_font) {
    ::SelectObject(mem, previous_font);
  }

  // The address capsule: the Mac's bordered field, holding the profile chip,
  // the domain and the two actions.
  FillCapsule(mem, layout.address, palette.hover);
  StrokeCapsule(mem, layout.address, palette.border, 1);

  if (layout.profile.right > layout.profile.left) {
    FillCapsule(mem, layout.profile, palette.hover_strong);
    RECT icon = {layout.profile.left + Scaled(kChipPad), layout.profile.top,
                 layout.profile.left + Scaled(kChipPad) + Scaled(kSpaceIcon),
                 layout.profile.bottom};
    HGDIOBJ previous_chip_font =
        glyph_font_ ? ::SelectObject(mem, glyph_font_) : nullptr;
    draw_glyph(icon, kGlyphProfile, palette.label_dim);
    if (previous_chip_font) {
      ::SelectObject(mem, previous_chip_font);
    }
    HGDIOBJ previous_name_font =
        ui_strong_font_ ? ::SelectObject(mem, ui_strong_font_) : nullptr;
    ::SetTextColor(mem, palette.label);
    RECT name = {icon.right + Scaled(kSpacePad), layout.profile.top,
                 layout.profile.right - Scaled(kChipPad) / 2,
                 layout.profile.bottom};
    PhiDrawTextAA(mem, profile_name_.c_str(), -1, &name,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                    DT_END_ELLIPSIS);
    if (previous_name_font) {
      ::SelectObject(mem, previous_name_font);
    }
  }

  // The domain, centred in the run the chip and the actions leave — a Kiosk
  // has no omnibox, so it shows what it is displaying and nothing more.
  HGDIOBJ previous_url_font =
      ui_font_ ? ::SelectObject(mem, ui_font_) : nullptr;
  ::SetTextColor(mem, palette.label);
  RECT url_rc = layout.url;
  const std::wstring shown = DisplayUrl();
  PhiDrawTextAA(mem, shown.c_str(), -1, &url_rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                  DT_END_ELLIPSIS);
  if (previous_url_font) {
    ::SelectObject(mem, previous_url_font);
  }

  HGDIOBJ previous_glyph_font =
      glyph_font_ ? ::SelectObject(mem, glyph_font_) : nullptr;
  const auto draw_control = [&](int index, const RECT& rc, const wchar_t* glyph,
                                bool enabled, COLORREF wash) {
    if (rc.right <= rc.left) {
      return;
    }
    if (index == hot_ && enabled) {
      // The Mac header buttons' treatment: cornerRadius 4 on a plain wash.
      FillRounded(mem, rc, wash, Scaled(4));
    }
    draw_glyph(rc, glyph, enabled ? palette.label : palette.label_tertiary);
  };
  draw_control(kHotBack, layout.back, kGlyphBack, can_go_back_, palette.hover);
  draw_control(kHotExtensions, layout.extensions, kGlyphExtensions, true,
               palette.hover_strong);
  draw_control(kHotCopy, layout.copy,
               copy_flash_ ? kGlyphCopied : kGlyphCopy, true,
               palette.hover_strong);
  if (previous_glyph_font) {
    ::SelectObject(mem, previous_glyph_font);
  }

  // "Open in <Space>": one capsule, two halves — the opening action and the
  // Space list — with the hover wash clipped to the half under the pointer.
  if (layout.space_menu.right > layout.space_menu.left) {
    FillCapsule(mem, layout.space_menu, palette.hover);
    for (const auto& [index, segment] :
         {std::pair<int, RECT>{kHotSpaceOpen, layout.space_primary},
          std::pair<int, RECT>{kHotSpaceList, layout.space_chevron}}) {
      if (hot_ != index) {
        continue;
      }
      const int save = ::SaveDC(mem);
      ::IntersectClipRect(mem, segment.left, segment.top, segment.right,
                          segment.bottom);
      FillCapsule(mem, layout.space_menu, palette.hover_strong);
      ::RestoreDC(mem, save);
    }
    StrokeCapsule(mem, layout.space_menu, palette.border, 1);

    const SpaceRow* target = TargetSpace();
    const std::wstring prefix = PhiStr("kiosk.toolbar.openInSpace");
    int x = layout.space_primary.left + Scaled(kSpacePad);
    HGDIOBJ previous_prefix_font =
        ui_font_ ? ::SelectObject(mem, ui_font_) : nullptr;
    ::SetTextColor(mem, palette.label_dim);
    RECT prefix_rc = {x, layout.space_menu.top,
                      layout.space_primary.right - Scaled(kSpacePad),
                      layout.space_menu.bottom};
    PhiDrawTextAA(mem, prefix.c_str(), -1, &prefix_rc,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    x += space_prefix_w_ + Scaled(kSpacePad);
    if (previous_prefix_font) {
      ::SelectObject(mem, previous_prefix_font);
    }
    if (target && x + Scaled(kSpaceIcon) < layout.space_primary.right) {
      RECT icon = {x, layout.space_menu.top, x + Scaled(kSpaceIcon),
                   layout.space_menu.bottom};
      DrawSpaceIcon(mem, icon, target->icon, Scaled(kSpaceIcon), palette.label,
                    glyph_font_, ui_font_);
      x = icon.right + Scaled(kSpacePad);
      HGDIOBJ previous_space_font =
          ui_strong_font_ ? ::SelectObject(mem, ui_strong_font_) : nullptr;
      ::SetTextColor(mem, palette.label);
      RECT name = {x, layout.space_menu.top,
                   layout.space_primary.right - Scaled(kSpacePad),
                   layout.space_menu.bottom};
      PhiDrawTextAA(mem, target->name.c_str(), -1, &name,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX |
                      DT_END_ELLIPSIS);
      if (previous_space_font) {
        ::SelectObject(mem, previous_space_font);
      }
    }
    // The divider between the halves, the Mac's 18pt rule.
    const int rule_h = Scaled(18);
    RECT divider = {layout.space_chevron.left,
                    (layout.space_menu.top + layout.space_menu.bottom - rule_h) / 2,
                    layout.space_chevron.left + 1,
                    (layout.space_menu.top + layout.space_menu.bottom + rule_h) / 2};
    HBRUSH divider_brush = ::CreateSolidBrush(palette.border);
    ::FillRect(mem, &divider, divider_brush);
    ::DeleteObject(divider_brush);

    HGDIOBJ previous_chevron_font =
        caption_font_ ? ::SelectObject(mem, caption_font_) : nullptr;
    draw_glyph(layout.space_chevron, kGlyphChevron, palette.label_dim);
    if (previous_chevron_font) {
      ::SelectObject(mem, previous_chevron_font);
    }
  }

  ::BitBlt(dc, 0, 0, client.right, layout.band.bottom, mem, 0, 0, SRCCOPY);

  ::SelectObject(mem, old_bmp);
  ::DeleteObject(bmp);
  ::DeleteDC(mem);
  ::EndPaint(hwnd_, &ps);
}

void PhiKioskWindow::OnMouseMove(POINT pt) {
  const Layout layout = ComputeLayout();
  int hot = -1;
  if (can_go_back_ && ::PtInRect(&layout.back, pt)) {
    hot = kHotBack;
  } else if (::PtInRect(&layout.extensions, pt)) {
    hot = kHotExtensions;
  } else if (::PtInRect(&layout.copy, pt)) {
    hot = kHotCopy;
  } else if (::PtInRect(&layout.space_primary, pt)) {
    hot = kHotSpaceOpen;
  } else if (::PtInRect(&layout.space_chevron, pt)) {
    hot = kHotSpaceList;
  }
  if (hot != hot_) {
    hot_ = hot;
    ::InvalidateRect(hwnd_, &layout.band, FALSE);
  }
  if (hot >= 0) {
    TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd_, 0};
    ::TrackMouseEvent(&track);
  }
}

void PhiKioskWindow::CopyUrl() {
  if (url_.empty() || !::OpenClipboard(hwnd_)) {
    return;
  }
  ::EmptyClipboard();
  const size_t bytes = (url_.size() + 1) * sizeof(wchar_t);
  if (HGLOBAL mem = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
    if (wchar_t* dest = static_cast<wchar_t*>(::GlobalLock(mem))) {
      std::copy_n(url_.c_str(), url_.size() + 1, dest);
      ::GlobalUnlock(mem);
      if (!::SetClipboardData(CF_UNICODETEXT, mem)) {
        ::GlobalFree(mem);
      }
    } else {
      ::GlobalFree(mem);
    }
  }
  ::CloseClipboard();
  // The glyph swaps to a checkmark for a second (the Mac's copyURLButton).
  copy_flash_ = true;
  ::SetTimer(hwnd_, kCopyFlashTimer, 1000, nullptr);
  ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhiKioskWindow::OpenInSpace(const std::string& space_id) {
  if (!framework_ || !coordinator_ || space_id.empty()) {
    return;
  }
  const std::string url = Narrow(url_);
  if (url.empty()) {
    return;
  }
  coordinator_->OnOpenLinkInSpace(window_id_.value, space_id.c_str(),
                                  url.c_str());
  // Closing tears this object down through OnBrowserWindowClosing: touch
  // nothing after it.
  framework_->CloseBrowserWindow(window_id_.value);
}

void PhiKioskWindow::ShowSpaceList(const RECT& anchor) {
  if (spaces_.empty()) {
    return;
  }
  HMENU menu = ::CreatePopupMenu();
  if (!menu) {
    return;
  }
  // Each row wears its Space's icon as a bitmap — the strip's artwork or
  // colour emoji, which menu text can render neither of.
  std::vector<HBITMAP> bitmaps;
  for (size_t i = 0; i < spaces_.size(); ++i) {
    ::AppendMenuW(menu,
                  MF_STRING | (spaces_[i].id == target_space_id_
                                   ? MF_CHECKED | MF_DEFAULT
                                   : 0),
                  static_cast<UINT>(i) + 1, spaces_[i].name.c_str());
    if (HBITMAP bitmap = PhiSpaceGlyphBitmap(spaces_[i].icon, Scaled(16),
                                             ::GetSysColor(COLOR_MENUTEXT))) {
      bitmaps.push_back(bitmap);
      PhiSetMenuItemBitmap(menu, static_cast<UINT>(i) + 1, bitmap);
    }
  }
  POINT screen = {anchor.left, anchor.bottom};
  ::ClientToScreen(hwnd_, &screen);
  const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
      menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, screen.x, screen.y, 0,
      hwnd_, nullptr));
  ::DestroyMenu(menu);
  for (HBITMAP bitmap : bitmaps) {
    ::DeleteObject(bitmap);
  }
  if (chosen >= 1 && chosen <= spaces_.size()) {
    OpenInSpace(spaces_[chosen - 1].id);  // Tears this object down.
  }
}

void PhiKioskWindow::OnClick(POINT pt) {
  if (!framework_) {
    return;
  }
  const Layout layout = ComputeLayout();
  if (can_go_back_ && ::PtInRect(&layout.back, pt)) {
    framework_->GoBack(window_id_.value);
  } else if (::PtInRect(&layout.extensions, pt)) {
    RECT anchor = layout.extensions;
    ::MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
    PhiExtensionsPopover::Toggle(hwnd_, anchor, Palette(), framework_,
                                 window_id_);
  } else if (::PtInRect(&layout.copy, pt)) {
    CopyUrl();
  } else if (::PtInRect(&layout.space_primary, pt)) {
    OpenInSpace(target_space_id_);  // Tears this object down.
  } else if (::PtInRect(&layout.space_chevron, pt)) {
    ShowSpaceList(layout.space_menu);  // May tear this object down.
  }
}

int PhiKioskWindow::CaptionHitTest(POINT client_pt) {
  const Layout layout = ComputeLayout();
  if (!::PtInRect(&layout.band, client_pt)) {
    return 0;
  }
  if (::PtInRect(&layout.caption_close, client_pt)) {
    return HTCLOSE;
  }
  if (::PtInRect(&layout.caption_max, client_pt)) {
    return HTMAXBUTTON;
  }
  if (::PtInRect(&layout.caption_min, client_pt)) {
    return HTMINBUTTON;
  }
  // The band's own controls take their own clicks; without this the band
  // would drag the window out from under the pointer instead. The capsule's
  // bare run is deliberately NOT one of them — it takes no clicks, so it
  // drags, which is the Mac's isMovableByWindowBackground.
  if ((can_go_back_ && ::PtInRect(&layout.back, client_pt)) ||
      ::PtInRect(&layout.extensions, client_pt) ||
      ::PtInRect(&layout.copy, client_pt) ||
      ::PtInRect(&layout.space_menu, client_pt)) {
    return HTCLIENT;
  }
  return HTCAPTION;
}

void PhiKioskWindow::SetCaptionHot(int code) {
  if (hot_caption_ == code) {
    return;
  }
  hot_caption_ = code;
  if (hwnd_) {
    const RECT band = ComputeLayout().band;
    ::InvalidateRect(hwnd_, &band, FALSE);
  }
}

LRESULT PhiKioskWindow::Handle(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      Paint();
      return 0;
    case WM_SIZE:
      PositionContent();
      ::InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_NCCALCSIZE: {
      // Claim the caption for the client area: only the resize borders stay
      // non-client. The top edge keeps NO border — the band goes flush to it
      // (WM_NCHITTEST serves HTTOP from its first pixels). BOTH wparam forms
      // must be handled: the first calc at creation comes with wparam FALSE
      // (a bare RECT), and skipping it leaves the OS caption alive — its
      // buttons then sit doubled above the band's.
      RECT* rect = wparam == TRUE
                       ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]
                       : reinterpret_cast<RECT*>(lparam);
      const UINT dpi = ::GetDpiForWindow(hwnd_);
      const int border_x = ::GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) +
                           ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
      const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                           ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
      rect->left += border_x;
      rect->right -= border_x;
      rect->bottom -= border_y;
      if (::IsZoomed(hwnd_)) {
        rect->top += border_y;  // Maximized frames overhang the monitor.
      }
      return 0;
    }
    case WM_NCHITTEST: {
      const LRESULT hit = ::DefWindowProcW(hwnd_, msg, wparam, lparam);
      if (hit != HTCLIENT) {
        return hit;  // The real resize borders.
      }
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ::ScreenToClient(hwnd_, &pt);
      const UINT dpi = ::GetDpiForWindow(hwnd_);
      const int border_y = ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                           ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
      if (!::IsZoomed(hwnd_) && pt.y < border_y) {
        return HTTOP;  // The band's first pixels still resize.
      }
      const int band_hit = CaptionHitTest(pt);
      return band_hit ? band_hit : HTCLIENT;
    }
    case WM_NCLBUTTONDOWN:
      // Swallow presses on our drawn window controls; the action fires on
      // release over the same button, like the OS ones.
      if (wparam == HTMINBUTTON || wparam == HTMAXBUTTON || wparam == HTCLOSE) {
        pressed_caption_ = static_cast<int>(wparam);
        return 0;
      }
      break;
    case WM_NCLBUTTONUP:
      if (pressed_caption_ && static_cast<int>(wparam) == pressed_caption_) {
        const int pressed = pressed_caption_;
        pressed_caption_ = 0;
        if (pressed == HTMINBUTTON) {
          ::ShowWindow(hwnd_, SW_MINIMIZE);
        } else if (pressed == HTMAXBUTTON) {
          ::ShowWindow(hwnd_, ::IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        } else {
          ::PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
        return 0;
      }
      pressed_caption_ = 0;
      break;
    case WM_NCMOUSEMOVE:
      SetCaptionHot(wparam == HTMINBUTTON || wparam == HTMAXBUTTON ||
                            wparam == HTCLOSE
                        ? static_cast<int>(wparam)
                        : 0);
      // Most of the band hit-tests as HTCAPTION, so a pointer crossing it
      // arrives here and not as WM_MOUSEMOVE: nothing the band's own
      // controls were washing is under it any more.
      if (hot_ != -1) {
        hot_ = -1;
        ::InvalidateRect(hwnd_, nullptr, FALSE);
      }
      {
        TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE | TME_NONCLIENT,
                                 hwnd_, 0};
        ::TrackMouseEvent(&track);
      }
      return 0;
    case WM_NCMOUSELEAVE:
      SetCaptionHot(0);
      return 0;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wparam);
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      if (suggested) {
        ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left,
                       suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
      }
      PositionContent();
      ::InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEMOVE: {
      SetCaptionHot(0);  // Client moves mean we left the window controls.
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      OnMouseMove(pt);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hot_ != -1) {
        hot_ = -1;
        ::InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    case WM_TIMER:
      if (wparam == kCopyFlashTimer) {
        ::KillTimer(hwnd_, kCopyFlashTimer);
        copy_flash_ = false;
        ::InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_LBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      OnClick(pt);
      return 0;
    }
    case WM_ACTIVATE:
      if (LOWORD(wparam) != WA_INACTIVE) {
        // The main window's active Space may have moved while this window
        // was away, and the menu must name where the page will really go.
        RefreshSpaces();
        MeasureLabels();
        ::InvalidateRect(hwnd_, nullptr, FALSE);
      }
      break;  // Activation is still the system's to finish.
    case WM_SETFOCUS:
      // Keystrokes belong to the page, never to this bare chrome.
      if (browser_hwnd_ && ::IsWindow(browser_hwnd_)) {
        ::SetFocus(browser_hwnd_);
      }
      return 0;
    case WM_CLOSE:
      // Route through the framework so the Browser tears down properly; the
      // coordinator drops this window when OnBrowserWindowClosing arrives.
      if (framework_) {
        framework_->CloseBrowserWindow(window_id_.value);
        return 0;
      }
      break;
  }
  return ::DefWindowProcW(hwnd_, msg, wparam, lparam);
}

// static
LRESULT CALLBACK PhiKioskWindow::WndProc(HWND hwnd,
                                         UINT msg,
                                         WPARAM wparam,
                                         LPARAM lparam) {
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    auto* self =
        static_cast<PhiKioskWindow*>(create->lpCreateParams);
    if (self) {
      self->hwnd_ = hwnd;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  auto* self = reinterpret_cast<PhiKioskWindow*>(
      ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (!self) {
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }
  return self->Handle(msg, wparam, lparam);
}

}  // namespace phi_client
