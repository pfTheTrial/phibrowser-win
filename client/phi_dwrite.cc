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

// A string's INK — the pixels it actually covers — inside a centre-aligned
// layout box: its size, and how far its centre sits from the box's. Both
// are properties of the glyph and its em size alone (they cancel the box
// size out), which is what lets one measurement serve every draw.
struct InkMetrics {
  float dx = 0.0f;
  float dy = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

// Shared, lazily created, never torn down (process-lifetime singletons).
struct DWriteState {
  ComPtr<ID2D1Factory> d2d;
  ComPtr<IDWriteFactory> dwrite;
  ComPtr<ID2D1DCRenderTarget> target;
  std::map<int, ComPtr<IDWriteTextFormat>> formats;  // By pixel size.
  // The icon face (Segoe MDL2 Assets), resolved once: glyph rasterization
  // goes straight through the font face, no layout in the way.
  ComPtr<IDWriteFontFace> icon_face;
  bool icon_face_failed = false;
  // What a string's ink measures, and where it sits, by (em size, text).
  std::map<std::pair<int, std::wstring>, InkMetrics> ink_metrics;
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
const InkMetrics& MetricsFor(const std::wstring& text, int pixel_size) {
  DWriteState& state = State();
  const auto key = std::make_pair(pixel_size, text);
  auto cached = state.ink_metrics.find(key);
  if (cached != state.ink_metrics.end()) {
    return cached->second;
  }
  // Any box measures the same ink; a generous one keeps a wide glyph off
  // the wrap.
  const float box = static_cast<float>(pixel_size) * 4.0f;
  InkMetrics metrics;
  IDWriteTextFormat* format = FormatFor(pixel_size);
  ComPtr<IDWriteTextLayout> layout;
  DWRITE_OVERHANG_METRICS overhang = {};
  if (format &&
      SUCCEEDED(state.dwrite->CreateTextLayout(
          text.c_str(), static_cast<UINT32>(text.size()), format, box, box,
          layout.GetAddressOf())) &&
      SUCCEEDED(layout->GetOverhangMetrics(&overhang))) {
    // Each overhang is the ink's distance beyond that edge of the box: their
    // sum with the box is the ink's own size, and half the difference
    // between opposite pairs is how far off centre it sits. A font that
    // reports no ink bounds gives zeros and nothing moves; half the glyph
    // caps anything wilder.
    const float limit = pixel_size / 2.0f;
    metrics.dx =
        std::clamp((overhang.left - overhang.right) / 2.0f, -limit, limit);
    metrics.dy =
        std::clamp((overhang.top - overhang.bottom) / 2.0f, -limit, limit);
    metrics.width = std::max(0.0f, box + overhang.left + overhang.right);
    metrics.height = std::max(0.0f, box + overhang.top + overhang.bottom);
  }
  return state.ink_metrics.emplace(key, metrics).first->second;
}

// The icon face, created on first use. A missing face (a stripped Windows
// install) fails once and stays failed — every draw then takes the caller's
// GDI path.
IDWriteFontFace* IconFace() {
  DWriteState& state = State();
  if (state.icon_face) {
    return state.icon_face.Get();
  }
  if (state.icon_face_failed) {
    return nullptr;
  }
  state.icon_face_failed = true;  // Cleared on success below.
  ComPtr<IDWriteFontCollection> collection;
  UINT32 index = 0;
  BOOL exists = FALSE;
  ComPtr<IDWriteFontFamily> family;
  ComPtr<IDWriteFont> font;
  if (FAILED(state.dwrite->GetSystemFontCollection(collection.GetAddressOf(),
                                                   FALSE)) ||
      FAILED(collection->FindFamilyName(L"Segoe MDL2 Assets", &index,
                                        &exists)) ||
      !exists ||
      FAILED(collection->GetFontFamily(index, family.GetAddressOf())) ||
      FAILED(family->GetFirstMatchingFont(
          DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
          DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf())) ||
      FAILED(font->CreateFontFace(state.icon_face.GetAddressOf()))) {
    state.icon_face.Reset();
    return nullptr;
  }
  state.icon_face_failed = false;
  return state.icon_face.Get();
}

D2D1_COLOR_F ToColor(COLORREF color) {
  return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f,
                      GetBValue(color) / 255.0f);
}

}  // namespace

namespace {

// The ground colour the caller already painted under `rect`, sampled just
// inside its corner — pulled inside the DC's clip box first: a pixel outside
// the clip reads CLR_INVALID (a pip half-hidden by the Spaces strip's
// viewport clip would otherwise clear to white).
COLORREF SampleBackground(HDC dc, const RECT& rect) {
  POINT sample = {rect.left + 1, rect.top + 1};
  RECT clip = {};
  if (::GetClipBox(dc, &clip) > NULLREGION && clip.right > clip.left &&
      clip.bottom > clip.top) {
    sample.x = std::max(clip.left, std::min(sample.x, clip.right - 1));
    sample.y = std::max(clip.top, std::min(sample.y, clip.bottom - 1));
  }
  return ::GetPixel(dc, sample.x, sample.y);
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
  const COLORREF background = SampleBackground(dc, rect);
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
    const InkMetrics& metrics = MetricsFor(text, pixel_size);
    const D2D1_RECT_F bounds = D2D1::RectF(metrics.dx, metrics.dy,
                                           width + metrics.dx,
                                           height + metrics.dy);
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

bool PhiDrawColorIcon(HDC dc,
                      const RECT& rect,
                      const std::wstring& text,
                      int ink_size,
                      COLORREF ink) {
  if (text.empty() || ink_size <= 0 || !EnsureState()) {
    return false;
  }
  // An em size says nothing about how much of it a glyph covers: a face
  // emoji nearly fills its line box where a heart or a cog sits well inside
  // it, so a row drawn at one em size reads as a row of different sizes.
  // Measure the ink at the asked-for size and scale the em by what it
  // missed — ink is linear in em, so one correction lands it.
  const InkMetrics& probe = MetricsFor(text, ink_size);
  const float span = std::max(probe.width, probe.height);
  int em = ink_size;
  if (span > 0.5f) {
    em = std::clamp(
        static_cast<int>(std::lround(ink_size * ink_size / span)), 1,
        ink_size * 4);
  }
  return PhiDrawColorText(dc, rect, text, em, ink);
}

bool PhiDrawIconGlyph(HDC dc,
                      const RECT& rect,
                      const std::wstring& glyph,
                      int pixel_size,
                      COLORREF ink) {
  if (glyph.empty() || pixel_size <= 0 || rect.right <= rect.left ||
      rect.bottom <= rect.top || !EnsureState()) {
    return false;
  }
  DWriteState& state = State();
  IDWriteFontFace* face = IconFace();
  if (!face) {
    return false;
  }
  // One code point, surrogate pair included — the MDL2 icons are BMP
  // private-use characters, but the pair costs two lines to honour.
  UINT32 codepoint = static_cast<UINT32>(glyph[0]);
  if (glyph.size() >= 2 && glyph[0] >= 0xD800 && glyph[0] <= 0xDBFF &&
      glyph[1] >= 0xDC00 && glyph[1] <= 0xDFFF) {
    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) +
                (static_cast<UINT32>(glyph[1]) - 0xDC00);
  }
  UINT16 index = 0;
  if (FAILED(face->GetGlyphIndices(&codepoint, 1, &index)) || index == 0) {
    return false;  // .notdef: not this face's glyph, let GDI try.
  }
  DWRITE_FONT_METRICS font_metrics = {};
  face->GetMetrics(&font_metrics);
  DWRITE_GLYPH_METRICS glyph_metrics = {};
  if (!font_metrics.designUnitsPerEm ||
      FAILED(face->GetDesignGlyphMetrics(&index, 1, &glyph_metrics, FALSE))) {
    return false;
  }
  // Placed exactly where DT_CENTER | DT_VCENTER would put it: the advance
  // centred across the box, the ascent + descent line box centred down it.
  const float em = static_cast<float>(pixel_size);
  const float upem = static_cast<float>(font_metrics.designUnitsPerEm);
  const float ascent = em * font_metrics.ascent / upem;
  const float descent = em * font_metrics.descent / upem;
  const float advance = em * glyph_metrics.advanceWidth / upem;
  const float box_w = static_cast<float>(rect.right - rect.left);
  const float box_h = static_cast<float>(rect.bottom - rect.top);
  const float origin_x = (box_w - advance) / 2.0f;
  const float origin_y = (box_h - (ascent + descent)) / 2.0f + ascent;
  DWRITE_GLYPH_RUN run = {};
  run.fontFace = face;
  run.fontEmSize = em;
  run.glyphCount = 1;
  run.glyphIndices = &index;
  ComPtr<IDWriteGlyphRunAnalysis> analysis;
  // NATURAL_SYMMETRIC is what makes this worth doing: outlines are hinted
  // vertically only and antialiased in both axes, so a circle stays round
  // instead of being snapped onto the pixel grid stem by stem.
  if (FAILED(state.dwrite->CreateGlyphRunAnalysis(
          &run, 1.0f, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
          DWRITE_MEASURING_MODE_NATURAL, origin_x, origin_y,
          analysis.GetAddressOf()))) {
    return false;
  }
  // The coverage texture is three bytes per pixel (one per subpixel);
  // averaging them is the grayscale coverage, free of ClearType's colour.
  RECT bounds = {};
  if (FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1,
                                             &bounds)) ||
      bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
    return false;
  }
  // Clipped to the box, as DrawText clips to its rect.
  bounds.left = std::max<LONG>(bounds.left, 0);
  bounds.top = std::max<LONG>(bounds.top, 0);
  bounds.right = std::min<LONG>(bounds.right, static_cast<LONG>(box_w));
  bounds.bottom = std::min<LONG>(bounds.bottom, static_cast<LONG>(box_h));
  if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
    return false;
  }
  const int w = bounds.right - bounds.left;
  const int h = bounds.bottom - bounds.top;
  std::vector<unsigned char> coverage(static_cast<size_t>(w) * h * 3);
  if (FAILED(analysis->CreateAlphaTexture(
          DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, coverage.data(),
          static_cast<UINT32>(coverage.size())))) {
    return false;
  }
  std::vector<unsigned char> bgra(static_cast<size_t>(w) * h * 4);
  const unsigned r = GetRValue(ink);
  const unsigned g = GetGValue(ink);
  const unsigned b = GetBValue(ink);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t from = (static_cast<size_t>(y) * w + x) * 3;
      const unsigned a =
          (static_cast<unsigned>(coverage[from]) + coverage[from + 1] +
           coverage[from + 2] + 1) /
          3;
      const size_t at = (static_cast<size_t>(y) * w + x) * 4;
      bgra[at] = static_cast<unsigned char>(b * a / 255);
      bgra[at + 1] = static_cast<unsigned char>(g * a / 255);
      bgra[at + 2] = static_cast<unsigned char>(r * a / 255);
      bgra[at + 3] = static_cast<unsigned char>(a);
    }
  }
  const RECT dest = {rect.left + bounds.left, rect.top + bounds.top,
                     rect.left + bounds.right, rect.top + bounds.bottom};
  return PhiBlendBgra(dc, dest, bgra.data(), w, h);
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
  const COLORREF background = SampleBackground(dc, dest);
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
