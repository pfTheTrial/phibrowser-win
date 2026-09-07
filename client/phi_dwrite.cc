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
#include <cstdlib>
#include <map>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
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

// ---------------------------------------------------------------------------
// Anti-aliased shapes.
//
// Every shape here is a signed-distance field sampled at pixel centres:
// negative inside, positive outside, in pixels. A fill's coverage is
// 0.5 - d clamped to 0..1 (a pixel whose centre sits on the boundary is
// half covered); a stroke of `width` is the band of distances [-width, 0]
// with the same half-pixel feather at both edges. The coverage becomes the
// alpha of a premultiplied buffer that GdiAlphaBlend composites over the
// DC — no Direct2D involved, so these work wherever GDI does.

namespace {

constexpr double kPi = 3.14159265358979323846;

double Clamp01(double v) {
  return std::min(1.0, std::max(0.0, v));
}

// Signed distance to a rounded rectangle's boundary: the distance to the
// inner core rectangle's nearest point, minus the corner radius. A corner
// left out of `corners` (bit 0..3 = TL, TR, BL, BR) stays square.
double RoundRectDistance(double px,
                         double py,
                         double w,
                         double h,
                         double radius,
                         unsigned corners = 0xF) {
  radius = std::max(0.0, std::min(radius, std::min(w, h) / 2.0));
  const bool left = px < w / 2.0;
  const bool top = py < h / 2.0;
  const unsigned bit = (top ? 0u : 2u) + (left ? 0u : 1u);
  const double r = (corners & (1u << bit)) ? radius : 0.0;
  const double cx = std::min(std::max(px, r), w - r);
  const double cy = std::min(std::max(py, r), h - r);
  const double dx = px - cx;
  const double dy = py - cy;
  if (dx == 0.0 || dy == 0.0) {
    // On an edge's run: the distance to the nearest of the four sides,
    // negative inside — the core-rect distance with r taken back off would
    // only be right for a pixel out past a corner.
    const double inside = std::min(std::min(px, w - px), std::min(py, h - py));
    if (px >= 0.0 && px <= w && py >= 0.0 && py <= h) {
      return -inside;
    }
    return std::sqrt(dx * dx + dy * dy) - r;
  }
  return std::sqrt(dx * dx + dy * dy) - r;
}

// Signed distance to the ellipse inscribed in w x h. Exact for a circle;
// for an ellipse the first-order estimate (f - 1) / |grad f| on the
// normalized radius f, which is accurate within a pixel of the boundary —
// the only place coverage is fractional.
double EllipseDistance(double px, double py, double w, double h) {
  const double a = w / 2.0;
  const double b = h / 2.0;
  if (a <= 0.0 || b <= 0.0) {
    return 1.0;
  }
  const double x = px - a;
  const double y = py - b;
  const double f = std::sqrt((x * x) / (a * a) + (y * y) / (b * b));
  if (f < 1e-6) {
    return -std::min(a, b);
  }
  const double grad =
      std::sqrt((x * x) / (a * a * a * a) + (y * y) / (b * b * b * b)) / f;
  return grad > 0.0 ? (f - 1.0) / grad : -std::min(a, b);
}

// Where along the rounded rectangle's perimeter the boundary point nearest
// (px, py) lies, in pixels from the top-left arc's end, clockwise — the
// parameter a dash pattern runs on. `*perimeter` receives the total.
double RoundRectPerimeterAt(double px,
                            double py,
                            double w,
                            double h,
                            double radius,
                            double* perimeter) {
  const double r = std::max(0.0, std::min(radius, std::min(w, h) / 2.0));
  const double run_x = w - 2.0 * r;
  const double run_y = h - 2.0 * r;
  const double arc = kPi * r / 2.0;
  *perimeter = 2.0 * run_x + 2.0 * run_y + 4.0 * arc;
  const double cx = std::min(std::max(px, r), w - r);
  const double cy = std::min(std::max(py, r), h - r);
  const double dx = px - cx;
  const double dy = py - cy;
  if (dx != 0.0 && dy != 0.0) {
    const double theta = std::atan2(dy, dx);  // -pi..pi, y down.
    if (dx > 0.0 && dy < 0.0) {               // Top-right arc.
      return run_x + (theta + kPi / 2.0) * r;
    }
    if (dx > 0.0) {                           // Bottom-right arc.
      return run_x + arc + run_y + theta * r;
    }
    if (dy > 0.0) {                           // Bottom-left arc.
      return 2.0 * run_x + 2.0 * arc + run_y + (theta - kPi / 2.0) * r;
    }
    return 2.0 * run_x + 3.0 * arc + 2.0 * run_y + (theta + kPi) * r;
  }
  // On a straight run: nearest side decides.
  const double to_top = py;
  const double to_bottom = h - py;
  const double to_left = px;
  const double to_right = w - px;
  const double nearest =
      std::min(std::min(to_top, to_bottom), std::min(to_left, to_right));
  if (nearest == to_top) {
    return cx - r;
  }
  if (nearest == to_right) {
    return run_x + arc + (cy - r);
  }
  if (nearest == to_bottom) {
    return run_x + 2.0 * arc + run_y + (w - r - cx);
  }
  return 2.0 * run_x + 3.0 * arc + run_y + (h - r - cy);
}

// The same for the inscribed ellipse: the angle scaled by the mean radius.
double EllipsePerimeterAt(double px,
                          double py,
                          double w,
                          double h,
                          double* perimeter) {
  const double a = w / 2.0;
  const double b = h / 2.0;
  const double mean = (a + b) / 2.0;
  *perimeter = 2.0 * kPi * mean;
  const double theta = std::atan2((py - b) / std::max(b, 1e-6),
                                  (px - a) / std::max(a, 1e-6));
  return (theta + kPi) * mean;
}

// Coverage of a dash pattern at perimeter position `t`: `dash` on, `gap`
// off, the period stretched so a whole number of them fit `perimeter`;
// dash ends feathered half a pixel like every other edge.
double DashCoverage(double t, double perimeter, int dash, int gap) {
  const double period = static_cast<double>(dash + gap);
  if (period <= 0.0 || perimeter <= period) {
    return 1.0;
  }
  const double count = std::max(1.0, std::round(perimeter / period));
  const double fitted = perimeter / count;
  const double on = dash * fitted / period;
  double s = std::fmod(t, fitted);
  if (s < 0.0) {
    s += fitted;
  }
  return Clamp01(std::min(s, on - s) + 0.5);
}

double FillCoverage(double d) {
  return Clamp01(0.5 - d);
}

double BandCoverage(double d, double width) {
  const double half = width / 2.0;
  return Clamp01(half + 0.5 - std::abs(d + half));
}

// Software-renders `coverage(x, y)` (0..1, in the rect's own frame) of
// `ink` into a premultiplied buffer and blends it over the DC.
template <typename Coverage>
bool BlendCoverage(HDC dc, const RECT& rect, COLORREF ink, Coverage coverage) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0) {
    return true;  // Nothing to draw is not a failure.
  }
  std::vector<unsigned char> bgra(static_cast<size_t>(w) * h * 4);
  const unsigned r = GetRValue(ink);
  const unsigned g = GetGValue(ink);
  const unsigned b = GetBValue(ink);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const double c = Clamp01(coverage(x + 0.5, y + 0.5));
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

void GdiFill(HDC dc, const RECT& rect, COLORREF fill) {
  if (rect.right <= rect.left || rect.bottom <= rect.top) {
    return;
  }
  HBRUSH brush = ::CreateSolidBrush(fill);
  ::FillRect(dc, &rect, brush);
  ::DeleteObject(brush);
}

// The GDI rendering of the same shape, for when a blend surface cannot be
// had (a DIB allocation failing under memory pressure). Stair-stepped, but
// the shape is there.
void GdiRoundRect(HDC dc,
                  const RECT& rect,
                  int radius,
                  const COLORREF* fill,
                  const COLORREF* ink,
                  int width) {
  HBRUSH brush = fill ? ::CreateSolidBrush(*fill) : nullptr;
  HPEN pen = ink ? ::CreatePen(PS_SOLID, width, *ink) : nullptr;
  HGDIOBJ old_brush = ::SelectObject(
      dc, brush ? static_cast<HGDIOBJ>(brush) : ::GetStockObject(NULL_BRUSH));
  HGDIOBJ old_pen = ::SelectObject(
      dc, pen ? static_cast<HGDIOBJ>(pen) : ::GetStockObject(NULL_PEN));
  // A NULL_PEN fill stops a pixel short; a stroked one does not.
  const int grow = pen ? 0 : 1;
  ::RoundRect(dc, rect.left, rect.top, rect.right + grow, rect.bottom + grow,
              2 * radius, 2 * radius);
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  if (pen) {
    ::DeleteObject(pen);
  }
  if (brush) {
    ::DeleteObject(brush);
  }
}

void GdiEllipse(HDC dc,
                const RECT& rect,
                const COLORREF* fill,
                const COLORREF* ink,
                int width) {
  HBRUSH brush = fill ? ::CreateSolidBrush(*fill) : nullptr;
  HPEN pen = ink ? ::CreatePen(PS_SOLID, width, *ink) : nullptr;
  HGDIOBJ old_brush = ::SelectObject(
      dc, brush ? static_cast<HGDIOBJ>(brush) : ::GetStockObject(NULL_BRUSH));
  HGDIOBJ old_pen = ::SelectObject(
      dc, pen ? static_cast<HGDIOBJ>(pen) : ::GetStockObject(NULL_PEN));
  const int grow = pen ? 0 : 1;
  ::Ellipse(dc, rect.left, rect.top, rect.right + grow, rect.bottom + grow);
  ::SelectObject(dc, old_pen);
  ::SelectObject(dc, old_brush);
  if (pen) {
    ::DeleteObject(pen);
  }
  if (brush) {
    ::DeleteObject(brush);
  }
}

// Blends `coverage` (in `rect`'s frame) over the four `tile`-square corner
// tiles of `rect` only — the straight runs between them are the caller's
// to fill, opaque and cheap.
template <typename Coverage>
bool BlendCornerTiles(HDC dc,
                      const RECT& rect,
                      int tile,
                      COLORREF ink,
                      Coverage coverage) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  tile = std::min(tile, std::min(w, h));
  const int xs[2] = {0, w - tile};
  const int ys[2] = {0, h - tile};
  for (int y : ys) {
    for (int x : xs) {
      const RECT part = {rect.left + x, rect.top + y, rect.left + x + tile,
                         rect.top + y + tile};
      if (!BlendCoverage(dc, part, ink, [&](double px, double py) {
            return coverage(px + x, py + y);
          })) {
        return false;
      }
      if (w <= tile) {
        break;  // One column: both tiles are the same pixels.
      }
    }
    if (h <= tile) {
      break;
    }
  }
  return true;
}

}  // namespace

bool PhiFillRoundRectAA(HDC dc, const RECT& rect, int radius, COLORREF fill) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0) {
    return false;
  }
  const int r = std::max(0, std::min(radius, std::min(w, h) / 2));
  if (r == 0) {
    GdiFill(dc, rect, fill);
    return true;
  }
  // The cross between the corner tiles is straight-edged on pixel bounds:
  // opaque, through GDI.
  GdiFill(dc, {rect.left + r, rect.top, rect.right - r, rect.bottom}, fill);
  GdiFill(dc, {rect.left, rect.top + r, rect.left + r, rect.bottom - r}, fill);
  GdiFill(dc, {rect.right - r, rect.top + r, rect.right, rect.bottom - r},
          fill);
  if (!BlendCornerTiles(dc, rect, r, fill, [&](double px, double py) {
        return FillCoverage(RoundRectDistance(px, py, w, h, r));
      })) {
    GdiRoundRect(dc, rect, r, &fill, nullptr, 0);
  }
  return true;
}

bool PhiStrokeRoundRectAA(HDC dc,
                          const RECT& rect,
                          int radius,
                          COLORREF ink,
                          int width) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0 || width <= 0) {
    return false;
  }
  if (2 * width >= std::min(w, h)) {
    return PhiFillRoundRectAA(dc, rect, radius, ink);  // All band.
  }
  const int r = std::max(0, std::min(radius, std::min(w, h) / 2));
  // The tiles must hold the whole band where it turns: the larger of the
  // radius and the stroke width.
  const int tile = std::min(std::max(r, width), std::min(w, h) / 2);
  GdiFill(dc, {rect.left + tile, rect.top, rect.right - tile, rect.top + width},
          ink);
  GdiFill(dc,
          {rect.left + tile, rect.bottom - width, rect.right - tile,
           rect.bottom},
          ink);
  GdiFill(dc,
          {rect.left, rect.top + tile, rect.left + width, rect.bottom - tile},
          ink);
  GdiFill(dc,
          {rect.right - width, rect.top + tile, rect.right, rect.bottom - tile},
          ink);
  if (!BlendCornerTiles(dc, rect, tile, ink, [&](double px, double py) {
        return BandCoverage(RoundRectDistance(px, py, w, h, r), width);
      })) {
    GdiRoundRect(dc, rect, r, nullptr, &ink, width);
  }
  return true;
}

bool PhiFillCapsuleAA(HDC dc, const RECT& rect, COLORREF fill) {
  return PhiFillRoundRectAA(dc, rect, (rect.bottom - rect.top) / 2, fill);
}

bool PhiStrokeCapsuleAA(HDC dc, const RECT& rect, COLORREF ink, int width) {
  return PhiStrokeRoundRectAA(dc, rect, (rect.bottom - rect.top) / 2, ink,
                              width);
}

bool PhiFillEllipseAA(HDC dc, const RECT& rect, COLORREF fill) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0) {
    return false;
  }
  if (!BlendCoverage(dc, rect, fill, [&](double px, double py) {
        return FillCoverage(EllipseDistance(px, py, w, h));
      })) {
    GdiEllipse(dc, rect, &fill, nullptr, 0);
  }
  return true;
}

bool PhiStrokeEllipseAA(HDC dc, const RECT& rect, COLORREF ink, int width) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0 || width <= 0) {
    return false;
  }
  if (!BlendCoverage(dc, rect, ink, [&](double px, double py) {
        return BandCoverage(EllipseDistance(px, py, w, h), width);
      })) {
    GdiEllipse(dc, rect, nullptr, &ink, width);
  }
  return true;
}

bool PhiStrokeRoundRectDashedAA(HDC dc,
                                const RECT& rect,
                                int radius,
                                COLORREF ink,
                                int width,
                                int dash,
                                int gap) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0 || width <= 0) {
    return false;
  }
  const int r = std::max(0, std::min(radius, std::min(w, h) / 2));
  if (!BlendCoverage(dc, rect, ink, [&](double px, double py) {
        const double band =
            BandCoverage(RoundRectDistance(px, py, w, h, r), width);
        if (band <= 0.0) {
          return 0.0;
        }
        double perimeter = 0.0;
        const double t = RoundRectPerimeterAt(px, py, w, h, r, &perimeter);
        return band * DashCoverage(t, perimeter, dash, gap);
      })) {
    GdiRoundRect(dc, rect, r, nullptr, &ink, width);
  }
  return true;
}

bool PhiStrokeEllipseDashedAA(HDC dc,
                              const RECT& rect,
                              COLORREF ink,
                              int width,
                              int dash,
                              int gap) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0 || width <= 0) {
    return false;
  }
  if (!BlendCoverage(dc, rect, ink, [&](double px, double py) {
        const double band = BandCoverage(EllipseDistance(px, py, w, h), width);
        if (band <= 0.0) {
          return 0.0;
        }
        double perimeter = 0.0;
        const double t = EllipsePerimeterAt(px, py, w, h, &perimeter);
        return band * DashCoverage(t, perimeter, dash, gap);
      })) {
    GdiEllipse(dc, rect, nullptr, &ink, width);
  }
  return true;
}

bool PhiKnockOutRoundRectAA(HDC dc,
                            const RECT& rect,
                            int radius,
                            COLORREF outside) {
  const int w = rect.right - rect.left;
  const int h = rect.bottom - rect.top;
  if (w <= 0 || h <= 0) {
    return false;
  }
  const int r = std::max(0, std::min(radius, std::min(w, h) / 2));
  if (r == 0) {
    return true;
  }
  return BlendCornerTiles(dc, rect, r, outside, [&](double px, double py) {
    return 1.0 - FillCoverage(RoundRectDistance(px, py, w, h, r));
  });
}

double PhiRoundRectCoverage(double px,
                            double py,
                            double width,
                            double height,
                            double radius,
                            unsigned corners) {
  return FillCoverage(
      RoundRectDistance(px, py, width, height, radius, corners));
}

// ---------------------------------------------------------------------------
// Icon glyphs through DrawText's signature.

int PhiDrawTextAA(HDC dc,
                  const wchar_t* text,
                  int length,
                  RECT* rect,
                  UINT format) {
  if (text && rect && !(format & DT_CALCRECT)) {
    // SAFETY: DrawTextW's contract — `text` holds `length` characters, or
    // is NUL-terminated when `length` is -1.
    const std::wstring_view view = UNSAFE_BUFFERS(
        length < 0 ? std::wstring_view(text)
                   : std::wstring_view(text, static_cast<size_t>(length)));
    // One code point, surrogate pair allowed; the MDL2 icons are BMP
    // private-use characters (U+E000..U+F8FF).
    const bool one = view.size() == 1 ||
                     (view.size() == 2 && view[0] >= 0xD800 &&
                      view[0] <= 0xDBFF);
    if (one && view[0] >= 0xE000 && view[0] <= 0xF8FF) {
      LOGFONTW lf = {};
      HGDIOBJ font = ::GetCurrentObject(dc, OBJ_FONT);
      if (font && ::GetObjectW(font, sizeof(lf), &lf) &&
          std::wstring_view(lf.lfFaceName) == L"Segoe MDL2 Assets" &&
          lf.lfHeight != 0) {
        // A negative height is the em size; a positive one the cell
        // height, which for this face is the em plus a little leading.
        const int em = std::abs(static_cast<int>(lf.lfHeight));
        RECT box = *rect;
        if (format & DT_RIGHT) {
          box.left = std::max(box.left, box.right - em);
        } else if (!(format & DT_CENTER)) {
          box.right = std::min(box.right, box.left + em);
        }
        if (format & DT_BOTTOM) {
          box.top = std::max(box.top, box.bottom - em);
        } else if (!(format & DT_VCENTER)) {
          box.bottom = std::min(box.bottom, box.top + em);
        }
        if (PhiDrawIconGlyph(dc, box, std::wstring(view), em,
                             ::GetTextColor(dc))) {
          return em;
        }
      }
    }
  }
  return ::DrawTextW(dc, text, length, rect, format);
}

// ---------------------------------------------------------------------------
// Shaped layered windows.

bool PhiCommitShapedLayer(HWND hwnd,
                          int width,
                          int height,
                          base::FunctionRef<void(HDC)> paint,
                          base::FunctionRef<double(double, double)> coverage) {
  if (!hwnd || !::IsWindow(hwnd) || width <= 0 || height <= 0) {
    return false;
  }
  BITMAPINFO info = {};
  info.bmiHeader.biSize = sizeof(info.bmiHeader);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // Top-down.
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
  HDC screen = ::GetDC(nullptr);
  HDC mem = ::CreateCompatibleDC(screen);
  HGDIOBJ previous = ::SelectObject(mem, dib);
  paint(mem);
  ::GdiFlush();
  const size_t bytes = static_cast<size_t>(width) * height * 4;
  // SAFETY: the DIB section was sized width * height * 4 above.
  const auto pixels = UNSAFE_BUFFERS(
      base::span<unsigned char>(static_cast<unsigned char*>(bits), bytes));
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t at = (static_cast<size_t>(y) * width + x) * 4;
      const double c = Clamp01(coverage(x + 0.5, y + 0.5));
      const unsigned a = static_cast<unsigned>(c * 255.0 + 0.5);
      pixels[at] = static_cast<unsigned char>(pixels[at] * a / 255);
      pixels[at + 1] = static_cast<unsigned char>(pixels[at + 1] * a / 255);
      pixels[at + 2] = static_cast<unsigned char>(pixels[at + 2] * a / 255);
      pixels[at + 3] = static_cast<unsigned char>(a);
    }
  }
  SIZE size = {width, height};
  POINT origin = {0, 0};
  BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  const BOOL ok = ::UpdateLayeredWindow(hwnd, nullptr, nullptr, &size, mem,
                                        &origin, 0, &blend, ULW_ALPHA);
  ::SelectObject(mem, previous);
  ::DeleteDC(mem);
  ::ReleaseDC(nullptr, screen);
  ::DeleteObject(dib);
  return ok != FALSE;
}

bool PhiCommitCornerCap(HWND cap,
                        int corner,
                        int radius,
                        COLORREF tint,
                        COLORREF ring) {
  if (radius <= 0 || corner < 0 || corner > 3) {
    return false;
  }
  // The circle is centred on the cap's inner corner — the card's corner
  // centre — so the quarter inside the cap is the card's arc.
  const double cx = (corner == 0 || corner == 2) ? radius : 0.0;
  const double cy = corner < 2 ? radius : 0.0;
  const double r = radius;
  auto ring_at = [&](double px, double py) {
    const double d = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
    return Clamp01(1.0 - std::abs(d - r - 0.5));  // The band [r, r + 1].
  };
  return PhiCommitShapedLayer(
      cap, radius, radius,
      [&](HDC dc) {
        // The tint everywhere, then the hairline's colour where the ring
        // runs — the coverage pass keys the alpha.
        GdiFill(dc, {0, 0, radius, radius}, tint);
        BlendCoverage(dc, {0, 0, radius, radius}, ring, ring_at);
      },
      [&](double px, double py) {
        const double d =
            std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
        return Clamp01(0.5 + (d - r));  // Outside the circle.
      });
}

}  // namespace phi_client
