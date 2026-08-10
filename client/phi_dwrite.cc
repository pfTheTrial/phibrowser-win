// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_dwrite.h"

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <map>

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
    const D2D1_RECT_F bounds = D2D1::RectF(
        0.0f, 0.0f, static_cast<FLOAT>(rect.right - rect.left),
        static_cast<FLOAT>(rect.bottom - rect.top));
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

}  // namespace phi_client
