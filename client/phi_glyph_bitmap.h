// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_GLYPH_BITMAP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_GLYPH_BITMAP_H_

#include <windows.h>

#include <string>

namespace phi_client {

// A Space icon glyph — a Phi catalog stand-in, an emoji, or any text —
// rasterized `px` square into a 32bpp PREMULTIPLIED-alpha bitmap the way the
// sidebar draws it: the catalog's artwork tinted `ink`, colour emoji via
// DirectWrite, the GDI font glyph as the last resort. Themed menus draw such
// a bitmap with its alpha as an item's hbmpItem, which is how a menu row
// gets the same icon the strip shows (menu text can render neither the MDL2
// stand-ins nor colour emoji). Null when nothing could be drawn. The caller
// owns the bitmap (DeleteObject) and keeps it alive while a menu holds it.
HBITMAP PhiSpaceGlyphBitmap(const std::wstring& glyph, int px, COLORREF ink);

// A template image — a PNG whose alpha is the shape and whose colour is
// ignored, as the Mac's template-rendering imagesets — decoded and scaled
// `px` square, its coverage filled with `ink`, as the same kind of bitmap.
// Null when the file is missing or does not decode. The caller owns it.
HBITMAP PhiTemplateImageBitmap(const std::wstring& path, int px, COLORREF ink);

// Sets `bitmap` as the picture of the item with command `id` (MIIM_BITMAP).
void PhiSetMenuItemBitmap(HMENU menu, UINT id, HBITMAP bitmap);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_GLYPH_BITMAP_H_
