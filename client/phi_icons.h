// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_ICONS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_ICONS_H_

#include <string>
#include <vector>

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

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_ICONS_H_
