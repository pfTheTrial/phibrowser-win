// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_ICONS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_ICONS_H_

#include <windows.h>

#include <string>
#include <vector>

#include "phibrowser-win/client/phi_mac_icons.h"

namespace phi_client {

// The Mac's Phi icon catalog (PhiIconCatalog.swift): 88 icons — 48
// pictograms, the digits 1..9 0, the letters A..Z, and ? ! ~ ` — with the
// SAME ids and the SAME order, so a "phi:<id>" stored by either client means
// the same icon on both. The Mac draws custom SVG assets; this client stands
// them in with Segoe MDL2 glyphs (pictograms) and plain characters (digits /
// letters / punctuation) until vector assets are ported.
struct PhiCatalogIcon {
  const char* id;       // "phi-icon-mail" — storage is "phi:" + id.
  const char* name;     // The Figma name, searched and shown as tooltip.
  wchar_t glyph;        // MDL2 codepoint (E7xx/E8xx…) or a plain character.
};

// All 88, in the Mac picker's order.
const std::vector<PhiCatalogIcon>& PhiIconCatalogAll();

// The glyph for a catalog id, as UTF-8 text ready for the icon pipeline.
// Empty when `id` is not in the catalog.
std::string PhiIconCatalogGlyph(const std::string& id);

// True when `text`'s first character sits in the private-use area — an MDL2
// glyph, which only renders with the Segoe MDL2 Assets font.
bool PhiIsPrivateGlyph(const std::wstring& text);

// The Mac catalog ARTWORK (all 88 imageset SVGs rasterized to 48px alpha
// masks — generated phi_icon_masks.cc): the real shapes, tinted at draw
// time via PhiTintedIconBgra like the sidebar's other template icons.
// Null when `id` is not in the catalog. ForGlyph resolves a stand-in
// glyph character back to its icon's mask, so draw sites holding display
// text can upgrade to the artwork without re-plumbing storage values.
const PhiMacIcon* PhiCatalogMask(const std::string& id);
const PhiMacIcon* PhiCatalogMaskForGlyph(const std::wstring& text);

// Draws `text`'s catalog artwork tinted `ink`, `px` square and centered in
// `rc`. False when `text` is not a catalog stand-in (emoji, initials) —
// callers fall through to their font path.
bool PhiDrawCatalogArtwork(HDC dc,
                           const RECT& rc,
                           const std::wstring& text,
                           int px,
                           COLORREF ink);

// The Phi app icon (resources\phi.ico beside the exe) at `size`, or null
// when the artwork is missing. Defined in phi_window_controller.cc.
HICON PhiAppIcon(int size);

// The Phi logo as premultiplied BGRA, extracted once from the ICO's 256px
// entry — crisp at any size through the D2D downscale path.
const std::vector<unsigned char>& PhiLogoBgra256();

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_ICONS_H_
