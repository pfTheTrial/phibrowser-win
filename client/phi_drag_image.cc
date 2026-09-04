// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_drag_image.h"

// GDI+ composes the image: rounded paths, aspect-filled snapshots and text
// with per-pixel alpha, which the layered window needs and plain GDI cannot
// produce (the split-drop cards and the agent overlay draw the same way).
#include <objidl.h>

#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "phibrowser-win/client/phi_theme.h"

namespace phi_client {

namespace {

constexpr wchar_t kImageClass[] = L"PhiDragImage";
constexpr UINT_PTR kSlideTimer = 1;
// AppKit's slide-back: a short ease-out to the starting position.
constexpr int kSlideBackMs = 250;
constexpr int kSlideTickMs = 8;

// The Mac placeholder's layout (makeTabPlaceholderSnapshot), in DIPs.
constexpr int kPlaceholderIcon = 28;
constexpr int kPlaceholderIconGap = 10;
constexpr int kPlaceholderInset = 12;
constexpr int kPlaceholderTextSize = 12;
constexpr int kPlaceholderMaxLines = 2;
constexpr int kSplitDivider = 2;

bool EnsureGdiPlus() {
  static const bool ok = [] {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    return Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok;
  }();
  return ok;
}

Gdiplus::Color Blend(COLORREF color, double alpha) {
  return Gdiplus::Color(static_cast<BYTE>(std::lround(alpha * 255.0)),
                        GetRValue(color), GetGValue(color), GetBValue(color));
}

void AddRoundedPath(Gdiplus::GraphicsPath* path,
                    const Gdiplus::RectF& r,
                    Gdiplus::REAL radius) {
  const Gdiplus::REAL d = radius * 2;
  if (d <= 0 || r.Width <= d || r.Height <= d) {
    path->AddRectangle(r);
    return;
  }
  path->AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
  path->AddArc(r.GetRight() - d, r.Y, d, d, 270.0f, 90.0f);
  path->AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0.0f, 90.0f);
  path->AddArc(r.X, r.GetBottom() - d, d, d, 90.0f, 90.0f);
  path->CloseFigure();
}

// A premultiplied 32bpp surface GDI+ draws into and the layered window shows.
struct Surface {
  std::vector<unsigned char> pargb;
  int width = 0;
  int height = 0;
  bool empty() const { return pargb.empty() || width <= 0 || height <= 0; }
};

Surface MakeSurface(int width, int height) {
  Surface s;
  if (width > 0 && height > 0) {
    s.width = width;
    s.height = height;
    s.pargb.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4,
                   0);
  }
  return s;
}

// GDI+ takes its pixels by non-const pointer even when it only reads them.
BYTE* Pixels(const std::vector<unsigned char>& bytes) {
  return const_cast<BYTE*>(bytes.data());
}

// Draws `bitmap` ASPECT-FILLED into `dest` (cropping the overhang, the Mac's
// drawnAsRoundedSnapshot), clipped to `dest`.
void DrawFilled(Gdiplus::Graphics* g,
                const PhiDragBitmap& bitmap,
                const Gdiplus::RectF& dest,
                bool premultiplied) {
  if (bitmap.empty() || dest.Width <= 0 || dest.Height <= 0) {
    return;
  }
  Gdiplus::Bitmap source(
      bitmap.width, bitmap.height, bitmap.width * 4,
      premultiplied ? PixelFormat32bppPARGB : PixelFormat32bppRGB,
      Pixels(bitmap.bgra));
  const double scale =
      std::max(static_cast<double>(dest.Width) / bitmap.width,
               static_cast<double>(dest.Height) / bitmap.height);
  const auto dw = static_cast<Gdiplus::REAL>(bitmap.width * scale);
  const auto dh = static_cast<Gdiplus::REAL>(bitmap.height * scale);
  const Gdiplus::RectF draw(dest.X + (dest.Width - dw) / 2,
                            dest.Y + (dest.Height - dh) / 2, dw, dh);
  g->SetClip(dest);
  g->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  g->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
  g->DrawImage(&source, draw);
  g->ResetClip();
}

// The favicon + title placeholder, centred in `rect`.
void DrawPlaceholder(Gdiplus::Graphics* g,
                     const PhiDragPane& pane,
                     const Gdiplus::RectF& rect,
                     const PhiPalette& palette,
                     UINT dpi) {
  const auto px = [&](int dip) {
    return static_cast<Gdiplus::REAL>(::MulDiv(dip, static_cast<int>(dpi), 96));
  };
  const Gdiplus::REAL icon = px(kPlaceholderIcon);
  const Gdiplus::REAL inset = px(kPlaceholderInset);
  const Gdiplus::REAL text_width = std::max<Gdiplus::REAL>(0, rect.Width - inset * 2);

  Gdiplus::FontFamily family(L"Segoe UI");
  Gdiplus::Font font(&family, px(kPlaceholderTextSize),
                     Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
  Gdiplus::StringFormat format;
  format.SetAlignment(Gdiplus::StringAlignmentCenter);
  format.SetLineAlignment(Gdiplus::StringAlignmentNear);
  format.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);
  format.SetFormatFlags(Gdiplus::StringFormatFlagsLineLimit);
  // What the title takes, capped at two lines (the Mac truncates to fit).
  Gdiplus::REAL title_height = 0;
  const bool have_font = family.GetLastStatus() == Gdiplus::Ok &&
                         font.GetLastStatus() == Gdiplus::Ok;
  if (have_font && !pane.title.empty() && text_width > 0) {
    const Gdiplus::REAL line = font.GetHeight(g);
    Gdiplus::RectF measured;
    g->MeasureString(pane.title.c_str(), -1, &font,
                     Gdiplus::RectF(0, 0, text_width, line * 8), &format,
                     &measured);
    title_height = std::min(measured.Height, line * kPlaceholderMaxLines);
  }
  const Gdiplus::REAL total =
      icon + (title_height > 0 ? px(kPlaceholderIconGap) + title_height : 0);
  const Gdiplus::REAL top = rect.Y + (rect.Height - total) / 2;

  // The favicon, never upscaled past its own pixels (a 16px icon inflated to
  // 28 only blurs) — centred in the 28pt box either way.
  const Gdiplus::RectF box(rect.X + (rect.Width - icon) / 2, top, icon, icon);
  if (!pane.favicon.empty()) {
    Gdiplus::Bitmap favicon(pane.favicon.width, pane.favicon.height,
                            pane.favicon.width * 4, PixelFormat32bppPARGB,
                            Pixels(pane.favicon.bgra));
    const Gdiplus::REAL side = std::min<Gdiplus::REAL>(
        icon, static_cast<Gdiplus::REAL>(
                  std::max(pane.favicon.width, pane.favicon.height)));
    const Gdiplus::RectF draw(box.X + (icon - side) / 2,
                              box.Y + (icon - side) / 2, side, side);
    g->SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
    g->DrawImage(&favicon, draw);
  } else {
    // The Mac's fallback is SF's globe; Segoe MDL2's is its twin.
    Gdiplus::FontFamily glyphs(L"Segoe MDL2 Assets");
    if (glyphs.GetLastStatus() == Gdiplus::Ok) {
      Gdiplus::Font glyph_font(&glyphs, icon * 0.8f, Gdiplus::FontStyleRegular,
                               Gdiplus::UnitPixel);
      Gdiplus::StringFormat centred;
      centred.SetAlignment(Gdiplus::StringAlignmentCenter);
      centred.SetLineAlignment(Gdiplus::StringAlignmentCenter);
      Gdiplus::SolidBrush ink(Blend(palette.label_dim, 1.0));
      g->DrawString(L"", -1, &glyph_font, box, &centred, &ink);
    }
  }
  if (title_height > 0) {
    const Gdiplus::RectF text(rect.X + inset,
                              box.GetBottom() + px(kPlaceholderIconGap),
                              text_width, title_height);
    Gdiplus::SolidBrush ink(Blend(palette.label, 1.0));
    g->DrawString(pane.title.c_str(), -1, &font, text, &format, &ink);
  }
}

void DrawPane(Gdiplus::Graphics* g,
              const PhiDragPane& pane,
              const Gdiplus::RectF& rect,
              const PhiPalette& palette,
              UINT dpi) {
  if (!pane.snapshot.empty()) {
    DrawFilled(g, pane.snapshot, rect, /*premultiplied=*/true);
  } else {
    DrawPlaceholder(g, pane, rect, palette, dpi);
  }
}

// `content` (opaque, rectangular) rounded at `radius` with anti-aliased
// corners, plus the hairline border the Mac strokes on its cards.
Surface Rounded(const Surface& content,
                int radius,
                bool border,
                COLORREF border_ink) {
  Surface out = MakeSurface(content.width, content.height);
  if (out.empty()) {
    return out;
  }
  Gdiplus::Bitmap source(content.width, content.height, content.width * 4,
                         PixelFormat32bppPARGB, Pixels(content.pargb));
  Gdiplus::Bitmap dest(out.width, out.height, out.width * 4,
                       PixelFormat32bppPARGB, out.pargb.data());
  Gdiplus::Graphics g(&dest);
  g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  g.Clear(Gdiplus::Color(0, 0, 0, 0));
  const Gdiplus::RectF whole(0, 0, static_cast<Gdiplus::REAL>(out.width),
                             static_cast<Gdiplus::REAL>(out.height));
  Gdiplus::GraphicsPath path;
  AddRoundedPath(&path, whole, static_cast<Gdiplus::REAL>(radius));
  Gdiplus::TextureBrush texture(&source);
  g.FillPath(&texture, &path);
  if (border) {
    Gdiplus::GraphicsPath edge;
    AddRoundedPath(&edge,
                   Gdiplus::RectF(0.5f, 0.5f, whole.Width - 1, whole.Height - 1),
                   static_cast<Gdiplus::REAL>(radius));
    Gdiplus::Pen pen(Blend(border_ink, 0.55), 1.0f);
    g.DrawPath(&pen, &edge);
  }
  return out;
}

Surface ComposeRow(const PhiDragBitmap& row, int radius) {
  Surface content = MakeSurface(row.width, row.height);
  if (content.empty()) {
    return content;
  }
  // The chrome's pixels carry no alpha (GDI leaves the byte at zero); read
  // them as opaque RGB.
  Gdiplus::Bitmap source(row.width, row.height, row.width * 4,
                         PixelFormat32bppRGB, Pixels(row.bgra));
  Gdiplus::Bitmap dest(content.width, content.height, content.width * 4,
                       PixelFormat32bppPARGB, content.pargb.data());
  Gdiplus::Graphics g(&dest);
  g.DrawImage(&source, 0, 0, content.width, content.height);
  return Rounded(content, radius, /*border=*/false, 0);
}

Surface ComposeCard(const PhiDragCard& card,
                    const PhiPalette& palette,
                    UINT dpi) {
  const auto px = [&](int dip) {
    return ::MulDiv(dip, static_cast<int>(dpi), 96);
  };
  int width = px(PhiDragImage::kCardWidth);
  int height = px(PhiDragImage::kCardHeight);
  if (card.split) {
    width = px(card.stacked ? PhiDragImage::kSplitShortEdge
                            : PhiDragImage::kSplitLongEdge);
    height = px(card.stacked ? PhiDragImage::kSplitLongEdge
                             : PhiDragImage::kSplitShortEdge);
  }
  Surface content = MakeSurface(width, height);
  if (content.empty()) {
    return content;
  }
  {
    Gdiplus::Bitmap dest(width, height, width * 4, PixelFormat32bppPARGB,
                         content.pargb.data());
    Gdiplus::Graphics g(&dest);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    // The Mac's controlBackgroundColor at 98%, drawn opaque here: the
    // rounding pass supplies the only transparency the card has.
    g.Clear(Blend(palette.window_bg, 1.0));
    const Gdiplus::RectF whole(0, 0, static_cast<Gdiplus::REAL>(width),
                               static_cast<Gdiplus::REAL>(height));
    if (!card.whole.empty()) {
      DrawFilled(&g, card.whole, whole, /*premultiplied=*/true);
    } else if (card.split) {
      // Pane rects from the split's own layout and ratio, a divider between.
      const double share = std::min(0.95, std::max(0.05, card.ratio));
      const int divider = px(kSplitDivider);
      Gdiplus::RectF primary, secondary, gap;
      if (card.stacked) {
        const int usable = std::max(0, height - divider);
        const int lead = static_cast<int>(usable * share);
        primary = Gdiplus::RectF(0, 0, whole.Width,
                                 static_cast<Gdiplus::REAL>(lead));
        gap = Gdiplus::RectF(0, static_cast<Gdiplus::REAL>(lead), whole.Width,
                             static_cast<Gdiplus::REAL>(divider));
        secondary = Gdiplus::RectF(
            0, static_cast<Gdiplus::REAL>(lead + divider), whole.Width,
            static_cast<Gdiplus::REAL>(height - lead - divider));
      } else {
        const int usable = std::max(0, width - divider);
        const int lead = static_cast<int>(usable * share);
        primary = Gdiplus::RectF(0, 0, static_cast<Gdiplus::REAL>(lead),
                                 whole.Height);
        gap = Gdiplus::RectF(static_cast<Gdiplus::REAL>(lead), 0,
                             static_cast<Gdiplus::REAL>(divider),
                             whole.Height);
        secondary = Gdiplus::RectF(
            static_cast<Gdiplus::REAL>(lead + divider), 0,
            static_cast<Gdiplus::REAL>(width - lead - divider), whole.Height);
      }
      DrawPane(&g, card.a, primary, palette, dpi);
      DrawPane(&g, card.b, secondary, palette, dpi);
      Gdiplus::SolidBrush rule(Blend(palette.separator, 1.0));
      g.FillRectangle(&rule, gap);
    } else {
      DrawPane(&g, card.a, whole, palette, dpi);
    }
  }
  return Rounded(content, px(PhiDragImage::kCardRadius), /*border=*/true,
                 palette.border);
}

class Image {
 public:
  static Image* instance;

  Image(HWND owner, UINT dpi) : owner_(owner), dpi_(dpi) {
    Register();
    // Topmost so it rides above every shell window and the Chromium child;
    // WS_EX_TRANSPARENT so hit tests fall through to whatever is under it
    // (the shell keeps the capture the drag runs on, and WindowFromPoint
    // finds the window BELOW for the cross-window drop); tool window and
    // no-activate so it never takes focus or a taskbar slot.
    window_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST |
            WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kImageClass, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr,
        ::GetModuleHandleW(nullptr), nullptr);
  }

  ~Image() {
    if (window_ && ::IsWindow(window_)) {
      ::KillTimer(window_, kSlideTimer);
      ::DestroyWindow(window_);
    }
  }

  HWND owner() const { return owner_; }
  bool sliding() const { return sliding_; }

  void SetRow(const PhiDragBitmap& row, int radius, POINT grip) {
    row_ = ComposeRow(row, radius);
    grip_ = grip;
  }

  void ShowRow(POINT cursor) {
    mode_ = Mode::kRow;
    Present(row_, Origin(cursor));
  }

  void ShowCard(const PhiDragCard& card,
                const PhiPalette& palette,
                POINT cursor) {
    if (card_.empty()) {
      card_ = ComposeCard(card, palette, dpi_);
    }
    if (card_.empty()) {
      ShowRow(cursor);  // Nothing to show instead; the row stays.
      return;
    }
    mode_ = Mode::kCard;
    Present(card_, Origin(cursor));
  }

  void MoveTo(POINT cursor) {
    if (sliding_ || !window_) {
      return;
    }
    const POINT origin = Origin(cursor);
    ::SetWindowPos(window_, HWND_TOPMOST, origin.x, origin.y, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }

  void Conceal() {
    if (window_) {
      ::ShowWindow(window_, SW_HIDE);
    }
  }

  void SlideBack(const RECT& home) {
    if (!window_ || row_.empty()) {
      return;
    }
    if (mode_ != Mode::kRow) {
      // Slide the ROW home: it is the row's place the image returns to.
      RECT now = {};
      ::GetWindowRect(window_, &now);
      mode_ = Mode::kRow;
      Present(row_, POINT{now.left, now.top});
    }
    RECT now = {};
    ::GetWindowRect(window_, &now);
    slide_from_ = {now.left, now.top};
    slide_to_ = {home.left, home.top};
    slide_start_ = ::GetTickCount64();
    sliding_ = true;
    ::ShowWindow(window_, SW_SHOWNOACTIVATE);
    ::SetTimer(window_, kSlideTimer, kSlideTickMs, nullptr);
  }

  // One slide tick; true once the image has arrived.
  bool Tick() {
    const double linear = std::min(
        1.0, static_cast<double>(::GetTickCount64() - slide_start_) /
                 kSlideBackMs);
    const double eased = 1.0 - std::pow(1.0 - linear, 3.0);
    const int x = static_cast<int>(
        std::lround(slide_from_.x + (slide_to_.x - slide_from_.x) * eased));
    const int y = static_cast<int>(
        std::lround(slide_from_.y + (slide_to_.y - slide_from_.y) * eased));
    ::SetWindowPos(window_, HWND_TOPMOST, x, y, 0, 0,
                   SWP_NOSIZE | SWP_NOACTIVATE);
    return linear >= 1.0;
  }

  static LRESULT CALLBACK WndProc(HWND hwnd,
                                  UINT msg,
                                  WPARAM wparam,
                                  LPARAM lparam) {
    if (msg == WM_TIMER && wparam == kSlideTimer) {
      if (instance && instance->window_ == hwnd && instance->sliding_) {
        if (instance->Tick()) {
          Image* done = instance;
          instance = nullptr;
          delete done;  // Destroys hwnd; nothing below touches it.
        }
      } else {
        ::KillTimer(hwnd, kSlideTimer);
      }
      return 0;
    }
    if (msg == WM_NCDESTROY && instance && instance->window_ == hwnd) {
      // Torn down from outside (the owner went away): forget the handle so
      // the destructor does not destroy it twice.
      instance->window_ = nullptr;
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
  }

 private:
  enum class Mode { kRow, kCard };

  void Register() {
    static bool registered = false;
    if (registered) {
      return;
    }
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = &Image::WndProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = kImageClass;
    ::RegisterClassExW(&wc);
    registered = true;
  }

  // Where the image's top-left goes for `cursor`: the row keeps the grip
  // the cursor lifted it by; the card hangs mostly ABOVE the cursor — the
  // Mac's snapshotDragFrame puts the pointer a quarter up from its bottom
  // edge so the card does not bury the drop target under it.
  POINT Origin(POINT cursor) const {
    if (mode_ == Mode::kCard && !card_.empty()) {
      return {cursor.x - card_.width / 2, cursor.y - card_.height * 3 / 4};
    }
    return {cursor.x - grip_.x, cursor.y - grip_.y};
  }

  void Present(const Surface& surface, POINT origin) {
    if (!window_ || surface.empty()) {
      return;
    }
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = surface.width;
    info.bmiHeader.biHeight = -surface.height;  // Top-down.
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = ::GetDC(nullptr);
    HDC mem = ::CreateCompatibleDC(screen);
    HBITMAP dib =
        ::CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (!dib || !bits) {
      if (dib) {
        ::DeleteObject(dib);
      }
      ::DeleteDC(mem);
      return;
    }
    // SAFETY: the DIB was just created with exactly the surface's byte count
    // (width * height * 4, top-down, no stride padding at 32bpp).
    UNSAFE_BUFFERS(base::span<unsigned char>(static_cast<unsigned char*>(bits),
                                             surface.pargb.size()))
        .copy_from(base::span<const unsigned char>(surface.pargb));
    HGDIOBJ old = ::SelectObject(mem, dib);
    POINT src = {0, 0};
    SIZE size = {surface.width, surface.height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    ::UpdateLayeredWindow(window_, nullptr, &origin, &size, mem, &src, 0,
                          &blend, ULW_ALPHA);
    ::SelectObject(mem, old);
    ::DeleteObject(dib);
    ::DeleteDC(mem);
    ::SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE |
                       SWP_SHOWWINDOW);
  }

  HWND owner_ = nullptr;
  UINT dpi_ = 96;
  HWND window_ = nullptr;
  Mode mode_ = Mode::kRow;
  Surface row_;
  Surface card_;  // Composed on first use, then cached for the drag.
  POINT grip_{};
  bool sliding_ = false;
  POINT slide_from_{};
  POINT slide_to_{};
  ULONGLONG slide_start_ = 0;
};

Image* Image::instance = nullptr;

}  // namespace

// static
void PhiDragImage::Begin(HWND owner,
                         UINT dpi,
                         const PhiDragBitmap& row,
                         int radius,
                         POINT grip,
                         POINT cursor) {
  if (!owner || row.empty() || !EnsureGdiPlus()) {
    return;
  }
  Hide();
  Image::instance = new Image(owner, dpi);
  Image::instance->SetRow(row, radius, grip);
  Image::instance->ShowRow(cursor);
}

// static
void PhiDragImage::ShowRow(POINT cursor) {
  if (Image::instance && !Image::instance->sliding()) {
    Image::instance->ShowRow(cursor);
  }
}

// static
void PhiDragImage::ShowCard(const PhiDragCard& card,
                            const PhiPalette& palette,
                            POINT cursor) {
  if (Image::instance && !Image::instance->sliding()) {
    Image::instance->ShowCard(card, palette, cursor);
  }
}

// static
void PhiDragImage::MoveTo(POINT cursor) {
  if (Image::instance) {
    Image::instance->MoveTo(cursor);
  }
}

// static
void PhiDragImage::Conceal() {
  if (Image::instance && !Image::instance->sliding()) {
    Image::instance->Conceal();
  }
}

// static
void PhiDragImage::Hide() {
  delete Image::instance;
  Image::instance = nullptr;
}

// static
void PhiDragImage::SlideBack(const RECT& home) {
  if (!Image::instance) {
    return;
  }
  if (Image::instance->sliding()) {
    return;  // Already on its way.
  }
  Image::instance->SlideBack(home);
}

// static
void PhiDragImage::OwnerGone(HWND owner) {
  if (Image::instance && Image::instance->owner() == owner) {
    Hide();
  }
}

// static
bool PhiDragImage::active() {
  return Image::instance != nullptr && !Image::instance->sliding();
}

bool PhiDragBitmapIsUniform(const PhiDragBitmap& bitmap) {
  if (bitmap.empty()) {
    return true;
  }
  const base::span<const unsigned char> pixels(bitmap.bgra);
  constexpr int kGrid = 12;
  uint32_t first = 0;
  bool have_first = false;
  for (int gy = 0; gy < kGrid; ++gy) {
    for (int gx = 0; gx < kGrid; ++gx) {
      const int x = static_cast<int>(
          (static_cast<int64_t>(bitmap.width) * (gx * 2 + 1)) / (kGrid * 2));
      const int y = static_cast<int>(
          (static_cast<int64_t>(bitmap.height) * (gy * 2 + 1)) / (kGrid * 2));
      const size_t at =
          (static_cast<size_t>(y) * static_cast<size_t>(bitmap.width) +
           static_cast<size_t>(x)) *
          4;
      if (at + 3 > pixels.size()) {
        continue;
      }
      // Colour only: the alpha byte is whatever the capture left there.
      const uint32_t rgb = (static_cast<uint32_t>(pixels[at]) << 16) |
                           (static_cast<uint32_t>(pixels[at + 1]) << 8) |
                           static_cast<uint32_t>(pixels[at + 2]);
      if (!have_first) {
        first = rgb;
        have_first = true;
      } else if (rgb != first) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace phi_client
