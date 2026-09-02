// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_DWRITE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_DWRITE_H_

#include <windows.h>

#include <string>

namespace phi_client {

// Draws `text` centred in `rect` with COLOR-FONT rendering (Segoe UI Emoji's
// COLR layers) — what GDI's DrawText cannot do. The pixel under the rect's
// corner is read back as the background, so callers paint their fills first.
// Returns false when Direct2D/DirectWrite are unavailable; the caller then
// falls back to the monochrome GDI path.
bool PhiDrawColorText(HDC dc,
                      const RECT& rect,
                      const std::wstring& text,
                      int pixel_size,
                      COLORREF ink);

// Draws a PREMULTIPLIED BGRA buffer scaled into `dest` with high-quality
// interpolation — crisp favicons and logos at any DPI, where GDI's
// StretchDIBits stair-steps. Same fallback contract as PhiDrawColorText.
bool PhiDrawBgra(HDC dc,
                 const RECT& dest,
                 const unsigned char* bgra,
                 int width,
                 int height);

// Alpha-blends a PREMULTIPLIED BGRA buffer over whatever the DC already
// holds — no background read-back, so it survives gradients and curved
// fills where PhiDrawBgra's flat-colour assumption breaks. GDI blending is
// unfiltered, so pass a buffer already at `dest`'s pixel size.
bool PhiBlendBgra(HDC dc,
                  const RECT& dest,
                  const unsigned char* bgra,
                  int width,
                  int height);

// Anti-aliased capsule (pill: corner radius = half the height) fill and
// stroke, software-rendered and alpha-blended over the DC — GDI's RoundRect
// stair-steps at pill radii. The stroke hugs the inside of the boundary.
bool PhiFillCapsuleAA(HDC dc, const RECT& rect, COLORREF fill);
bool PhiStrokeCapsuleAA(HDC dc, const RECT& rect, COLORREF ink, int width);

// The same, for a rounded rectangle of corner `radius` (the Mac's
// continuous-corner cards — the omnibox box, radius 14). A radius of half
// the height is the capsule.
bool PhiFillRoundRectAA(HDC dc, const RECT& rect, int radius, COLORREF fill);
bool PhiStrokeRoundRectAA(HDC dc,
                          const RECT& rect,
                          int radius,
                          COLORREF ink,
                          int width);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_DWRITE_H_
