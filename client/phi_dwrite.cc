// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_dwrite.h"

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/no_destructor.h"

namespace phi_client {

namespace {

using Microsoft::WRL::ComPtr;

// Shared, lazily created, never torn down (process-lifetime singletons).
struct DWriteState {
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<ID2D1DCRenderTarget> target;
  std::map<int, ComPtr<IDWriteTextFormat>> formats;  // By pixel size.
  // The ink-centring offset for a drawn string, by (pixel size, text).
  std::map<std::pair<int, std::wstring>, D2D1_POINT_2F> ink_offsets;
  bool failed = false;
};

DWriteState& State() {
  static base::NoDestructor<DWriteState> state;
  return *state;
}

bool EnsureState() {
  DWriteState& state = State();
  if (state.failed) {
    return false;
  }
  if (state.target) {
    return true;
  }
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               state.d2d.GetAddressOf()))) {
    state.failed = true;
    return false;
  }
  if (FAILED(DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown**>(state.dwrite.GetAddressOf())))) {
    state.failed = true;
    return false;
  }
  // 96 DPI: one DIP == one pixel, so callers pass physical pixel sizes.
  const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_IGNORE),
      96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
  if (FAILED(state.d2d->CreateDCRenderTarget(&properties,
                                             state.target.GetAddressOf()))) {
    state.failed = true;
    return false;
  }
  return true;
}

IDWriteTextFormat* FormatFor(int pixel_size) {
  DWriteState& state = State();
  auto it = state.formats.find(pixel_size);
  if (it != state.formats.end()) {
    return it->second.Get();
  }
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(state.dwrite->CreateTextFormat(
          L"Segoe UI Emoji", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          static_cast<FLOAT>(pixel_size), L"", format.GetAddressOf()))) {
    return nullptr;
  }
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  return state.formats.emplace(pixel_size, std::move(format))
      .first->second.Get();
}

// How far a string's INK sits from the centre of a centre-aligned layout
// box. A glyph's ink is rarely centred on its advance — colour emoji in
// particular ride low and wide in their line box — so a box centred by
// metrics still reads off-centre inside a pill. The correction cancels the
// box size out (it depends only on the glyph and its size), so one
// measurement per string serves every draw; the map keeps it off the paint
// path after the first.
D2D1_POINT_2F InkCentringOffset(const std::wstring& text,
                                int pixel_size,
                                float width,
                                float height) {
  DWriteState& state = State();
  const auto key = std::make_pair(pixel_size, text);
  auto cached = state.ink_offsets.find(key);
  if (cached != state.ink_offsets.end()) {
    return cached->second;
  }
  D2D1_POINT_2F offset = D2D1::Point2F(0.0f, 0.0f);
  IDWriteTextFormat* format = FormatFor(pixel_size);
  ComPtr<IDWriteTextLayout> layout;
  DWRITE_OVERHANG_METRICS overhang = {};
  if (format &&
      SUCCEEDED(state.dwrite->CreateTextLayout(
          text.c_str(), static_cast<UINT32>(text.size()), format, width,
          height, layout.GetAddressOf())) &&
      SUCCEEDED(layout->GetOverhangMetrics(&overhang))) {
    // Each overhang is the ink's distance beyond that edge of the box, so
    // half the difference between opposite pairs recentres it. A font that
    // reports no ink bounds gives zeros and nothing moves; half the glyph
    // caps anything wilder.
    const float limit = pixel_size / 2.0f;
    offset = D2D1::Point2F(
        std::clamp((overhang.left - overhang.right) / 2.0f, -limit, limit),
        std::clamp((overhang.top - overhang.bottom) / 2.0f, -limit, limit));
  }
  state.ink_offsets.emplace(key, offset);
  return offset;
}

D2D1_COLOR_F ToColor(COLORREF color) {
  return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                      GetBValue(color) / 255.0f);
}

}  // namespace

bool PhiDrawColorText(HDC dc,
                      const RECT& rect,
                      const std::wstring& text,
                      int pixel_size,
                      COLORREF ink) {
  if (text.empty() || rect.right <= rect.left || rect.bottom <= rect.top ||
      !EnsureState()) {
    return false;
  }
  IDWriteTextFormat* format = FormatFor(pixel_size);
  if (!format) {
    return false;
  }
  DWriteState& state = State();
  // D2D repaints the whole bound rect on EndDraw, so start from what is
  // already painted beneath (the caller's pill or ground fill).
  const COLORREF background =
      ::GetPixel(dc, rect.left + 1, rect.top + 1);
  if (FAILED(state.target->BindDC(dc, &rect))) {
    return false;
  }
  state.target->BeginDraw();
  state.target->Clear(
      ToColor(background == CLR_INVALID ? RGB(0xFF, 0xFF, 0xFF)
                                        : background));
  ComPtr<ID2D1SolidColorBrush> brush;
  if (SUCCEEDED(state.target->CreateSolidColorBrush(
          ToColor(ink), brush.GetAddressOf()))) {
    const float width = static_cast<float>(rect.right - rect.left);
    const float height = static_cast<float>(rect.bottom - rect.top);
    // Drawn ink-centred in the rect, not advance-centred (see above).
    const D2D1_POINT_2F offset =
        InkCentringOffset(text, pixel_size, width, height);
    const D2D1_RECT_F bounds = D2D1::RectF(offset.x, offset.y,
                                           width + offset.x,
                                           height + offset.y);
    // ENABLE_COLOR_FONT is the whole point: COLR glyph layers render in
    // colour; non-colour glyphs take the ink brush.
    state.target->DrawTextW(text.c_str(), static_cast<UINT32>(text.size()),
                            format, bounds, brush.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
  }
  if (FAILED(state.target->EndDraw())) {
    // A lost target rebuilds on the next call.
    state.target.Reset();
    return false;
  }
  return true;
}

bool PhiDrawBgra(HDC dc,
                 const RECT& dest,
                 const unsigned char* bgra,
                 int width,
                 int height) {
  if (!bgra || width <= 0 || height <= 0 || dest.right <= dest.left ||
      dest.bottom <= dest.top || !EnsureState()) {
    return false;
  }
  DWriteState& state = State();
  const COLORREF background = ::GetPixel(dc, dest.left + 1, dest.top + 1);
  if (FAILED(state.target->BindDC(dc, &dest))) {
    return false;
  }
  ComPtr<ID2D1Bitmap> bitmap;
  const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                        D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(state.target->CreateBitmap(
          D2D1::SizeU(static_cast<UINT32>(width),
                      static_cast<UINT32>(height)),
          bgra, static_cast<UINT32>(width) * 4, properties,
          bitmap.GetAddressOf()))) {
    return false;
  }
  state.target->BeginDraw();
  state.target->Clear(
      ToColor(background == CLR_INVALID ? RGB(0xFF, 0xFF, 0xFF)
                                        : background));
  const D2D1_RECT_F bounds =
      D2D1::RectF(0.0f, 0.0f, static_cast<FLOAT>(dest.right - dest.left),
                  static_cast<FLOAT>(dest.bottom - dest.top));
  // A device context can interpolate with high-quality cubic; the plain DC
  // target only offers linear — still far above StretchDIBits.
  ComPtr<ID2D1DeviceContext> context;
  if (SUCCEEDED(state.target.As(&context))) {
    context->DrawBitmap(bitmap.Get(), &bounds, 1.0f,
                        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
  } else {
    state.target->DrawBitmap(bitmap.Get(), bounds, 1.0f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  }
  if (FAILED(state.target->EndDraw())) {
    state.target.Reset();
    return false;
  }
  return true;
}

bool PhiBlendBgra(HDC dc,
                  const RECT& dest,
                  const unsigned char* bgra,
                  int width,
                  int height) {
  if (!bgra || width <= 0 || height <= 0 || dest.right <= dest.left ||
      dest.bottom <= dest.top) {
    return false;
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // Top-down, matching the buffer.
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib =
      ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    if (dib) {
      ::DeleteObject(dib);
    }
    return false;
  }
  // SAFETY: the DIB section was sized width*height*4 above, and callers
  // pass a buffer of exactly that size.
  UNSAFE_BUFFERS(
      ::memcpy(bits, bgra, static_cast<size_t>(width) * height * 4));
  HDC src_dc = ::CreateCompatibleDC(dc);
  HGDIOBJ previous = ::SelectObject(src_dc, dib);
  const BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  const BOOL ok = ::GdiAlphaBlend(dc, dest.left, dest.top,
                                  dest.right - dest.left,
                                  dest.bottom - dest.top, src_dc, 0, 0, width,
                                  height, blend);
  ::SelectObject(src_dc, previous);
  ::DeleteDC(src_dc);
  ::DeleteObject(dib);
  return ok != FALSE;
}

namespace {

// Signed distance from a pixel centre to a pill's boundary (negative
// inside): distance to the horizontal core segment, minus the cap radius.
double CapsuleDistance(double px, double py, double w, double h) {
  const double radius = h / 2.0;
  const double cx = std::min(std::max(px, radius), w - radius);
  const double dx = px - cx;
  const double dy = py - radius;
  return std::sqrt(dx * dx + dy * dy) - radius;
}

// Signed distance to a rounded rectangle's boundary (negative inside): the
// distance to the inner core rectangle's nearest point, minus the corner
// radius.
double RoundRectDistance(double px, double py, double w, double h,
                         double radius) {
  radius = std::min(radius, std::min(w, h) / 2.0);
  const double cx = std::min(std::max(px, radius), w - radius);
  const double cy = std::min(std::max(py, radius), h - radius);
  const double dx = px - cx;
  const double dy = py - cy;
  return std::sqrt(dx * dx + dy * dy) - radius;
}

// Software-renders `coverage(x, y)` (0..1) of `ink` into a premultiplied
// buffer and blends it over the DC.
template <typename Coverage>
bool BlendCoverage(HDC dc, const RECT& rect, COLORREF ink,
                   Coverage coverage) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0) {
    return false;
  }
  std::vector<unsigned char> bgra(static_cast<size_t>(w) * h * 4);
  const unsigned r = GetRValue(ink);
  const unsigned g = GetGValue(ink);
  const unsigned b = GetBValue(ink);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double c = std::min(1.0, std::max(0.0, coverage(x + 0.5, y + 0.5)));
      const unsigned a = static_cast<unsigned>(c * 255.0 + 0.5);
      const size_t at = (static_cast<size_t>(y) * w + x) * 4;
      bgra[at] = static_cast<unsigned char>(b * a / 255);
      bgra[at + 1] = static_cast<unsigned char>(g * a / 255);
      bgra[at + 2] = static_cast<unsigned char>(r * a / 255);
      bgra[at + 3] = static_cast<unsigned char>(a);
    }
  }
  return PhiBlendBgra(dc, rect, bgra.data(), w, h);
}

}  // namespace

bool PhiFillRoundRectAA(HDC dc, const RECT& rect, int radius, COLORREF fill) {
  const double w = rect.right - rect.left;
  const double h = rect.bottom - rect.top;
  return BlendCoverage(dc, rect, fill, [&](double px, double py) {
    return 0.5 - RoundRectDistance(px, py, w, h, radius);
  });
}

bool PhiStrokeRoundRectAA(HDC dc,
                          const RECT& rect,
                          int radius,
                          COLORREF ink,
                          int width) {
  if (width <= 0) {
    return false;
  }
  const double w = rect.right - rect.left;
  const double h = rect.bottom - rect.top;
  const double half = width / 2.0;
  return BlendCoverage(dc, rect, ink, [&](double px, double py) {
    // Band of `width` just inside the boundary, feathered half a pixel.
    const double d = RoundRectDistance(px, py, w, h, radius) + half;
    return half + 0.5 - std::abs(d);
  });
}

bool PhiFillCapsuleAA(HDC dc, const RECT& rect, COLORREF fill) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0) {
    return false;
  }
  std::vector<unsigned char> bgra(static_cast<size_t>(w) * h * 4);
  const unsigned r = GetRValue(fill);
  const unsigned g = GetGValue(fill);
  const unsigned b = GetBValue(fill);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double d = CapsuleDistance(x + 0.5, y + 0.5, w, h);
      const double coverage = std::min(1.0, std::max(0.0, 0.5 - d));
      const unsigned a = static_cast<unsigned>(coverage * 255.0 + 0.5);
      const size_t at = (static_cast<size_t>(y) * w + x) * 4;
      bgra[at] = static_cast<unsigned char>(b * a / 255);
      bgra[at + 1] = static_cast<unsigned char>(g * a / 255);
      bgra[at + 2] = static_cast<unsigned char>(r * a / 255);
      bgra[at + 3] = static_cast<unsigned char>(a);
    }
  }
  return PhiBlendBgra(dc, rect, bgra.data(), w, h);
}

bool PhiStrokeCapsuleAA(HDC dc, const RECT& rect, COLORREF ink, int width) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0 || width <= 0) {
    return false;
  }
  std::vector<unsigned char> bgra(static_cast<size_t>(w) * h * 4);
  const unsigned r = GetRValue(ink);
  const unsigned g = GetGValue(ink);
  const unsigned b = GetBValue(ink);
  const double half = width / 2.0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      // Band of `width` just inside the boundary, feathered half a pixel.
      const double d = CapsuleDistance(x + 0.5, y + 0.5, w, h) + half;
      const double coverage =
          std::min(1.0, std::max(0.0, half + 0.5 - std::abs(d)));
      const unsigned a = static_cast<unsigned>(coverage * 255.0 + 0.5);
      const size_t at = (static_cast<size_t>(y) * w + x) * 4;
      bgra[at] = static_cast<unsigned char>(b * a / 255);
      bgra[at + 1] = static_cast<unsigned char>(g * a / 255);
      bgra[at + 2] = static_cast<unsigned char>(r * a / 255);
      bgra[at + 3] = static_cast<unsigned char>(a);
    }
  }
  return PhiBlendBgra(dc, rect, bgra.data(), w, h);
}

}  // namespace phi_client
