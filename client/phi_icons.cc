// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_icons.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string_view>

#include "base/compiler_specific.h"
#include "base/no_destructor.h"
#include "phibrowser-win/client/phi_agent_icons.h"
#include "phibrowser-win/client/phi_dwrite.h"
#include "phibrowser-win/client/phi_fluent.h"
#include "base/strings/utf_string_conversions.h"

namespace phi_client {

const std::vector<PhiCatalogIcon>& PhiIconCatalogAll() {
  // Ids and ORDER are the Mac's (PhiIconCatalog.swift). The Fluent twins
  // are the §5.9 map (the canvas board's table, code points from Segoe
  // Fluent Icons; the MDL2 face on Windows 10 lacks a few — F0E3, F133,
  // F196 — and those fall back to the mask at draw time). `false` rows keep
  // their mask: cake, chip, cube-transparent, database, rss, server,
  // sparkles, table, ticket, truck; their glyph is only a name.
  static const base::NoDestructor<std::vector<PhiCatalogIcon>> icons({
      {"phi-icon-mail", "mail", 0xE715, true},              // Mail
      {"phi-icon-color-swatch", "color-swatch", 0xE790, true},  // Color
      {"phi-icon-camera", "camera", 0xE722, true},          // Camera
      {"phi-icon-chat", "chat", 0xE90A, true},              // Comment
      {"phi-icon-bell", "bell", 0xEA8F, true},              // Ringer
      {"phi-icon-user", "user", 0xE77B, true},              // Contact
      {"phi-icon-globe-alt", "globe-alt", 0xE774, true},    // Globe
      {"phi-icon-sun", "sun", 0xE706, true},                // Brightness
      {"phi-icon-sparkles", "sparkles", 0xE734, false},
      {"phi-icon-credit-card", "credit-card", 0xE8C7, true},  // PaymentCard
      {"phi-icon-ticket", "ticket", 0xE8EC, false},
      {"phi-icon-moon", "moon", 0xE708, true},              // QuietHours
      {"phi-icon-briefcase", "briefcase", 0xE821, true},    // Work
      {"phi-icon-archive", "archive", 0xE7B8, true},        // Package
      {"phi-icon-lock-closed", "lock-closed", 0xE72E, true},  // Lock
      {"phi-icon-office-building", "office-building", 0xEC06, true},  // CityNext
      {"phi-icon-chart-pie", "chart-pie", 0xEB05, true},    // PieSingle
      {"phi-icon-shield-exclamation", "shield-exclamation", 0xEA18, true},  // Shield
      {"phi-icon-photograph", "photograph", 0xE91B, true},  // Photo
      {"phi-icon-cog", "cog", 0xE713, true},                // Settings
      {"phi-icon-filter", "filter", 0xE71C, true},          // Filter
      {"phi-icon-qrcode", "qrcode", 0xED14, true},          // QRCode
      {"phi-icon-clipboard-list", "clipboard-list", 0xF0E3, true},  // ClipboardList
      {"phi-icon-lightning-bolt", "lightning-bolt", 0xE945, true},  // LightningBolt
      {"phi-icon-light-bulb", "light-bulb", 0xEA80, true},  // Lightbulb
      {"phi-icon-view-grid-add", "view-grid-add", 0xE8A9, true},  // ViewAll
      {"phi-icon-md-library", "md-library", 0xE8F1, true},  // Library
      {"phi-icon-play", "play", 0xE768, true},              // Play
      {"phi-icon-presentation-chart-line", "presentation-chart-line", 0xE9D2, true},  // AreaChart
      {"phi-icon-chip", "chip", 0xE7F8, false},
      {"phi-icon-cube-transparent", "cube-transparent", 0xE8B3, false},
      {"phi-icon-search-circle", "search-circle", 0xE721, true},  // Search
      {"phi-icon-cloud", "cloud", 0xE753, true},            // Cloud
      {"phi-icon-academic-cap", "academic-cap", 0xE7BE, true},  // Education
      {"phi-icon-gift", "gift", 0xF133, true},              // GiftboxOpen
      {"phi-icon-truck", "truck", 0xE7C0, false},
      {"phi-icon-cake", "cake", 0xE728, false},
      {"phi-icon-beaker", "beaker", 0xF196, true},          // Beaker
      {"phi-icon-table", "table", 0xE80A, false},
      {"phi-icon-paper-airplane", "paper-airplane", 0xE724, true},  // Send
      {"phi-icon-device-mobile", "device-mobile", 0xE8EA, true},  // CellPhone
      {"phi-icon-music-note", "music-note", 0xEC4F, true},  // MusicNote
      {"phi-icon-film", "film", 0xE714, true},              // Video
      {"phi-icon-database", "database", 0xE7C3, false},
      {"phi-icon-server", "server", 0xE83B, false},
      {"phi-icon-identification", "identification", 0xE779, true},  // ContactInfo
      {"phi-icon-rss", "rss", 0xE736, false},
      {"phi-icon-map", "map", 0xE707, true},                // MapPin
      {"phi-icon-number-1", "1", L'1', false},
      {"phi-icon-number-2", "2", L'2', false},
      {"phi-icon-number-3", "3", L'3', false},
      {"phi-icon-number-4", "4", L'4', false},
      {"phi-icon-number-5", "5", L'5', false},
      {"phi-icon-number-6", "6", L'6', false},
      {"phi-icon-number-7", "7", L'7', false},
      {"phi-icon-number-8", "8", L'8', false},
      {"phi-icon-number-9", "9", L'9', false},
      {"phi-icon-number-0", "0", L'0', false},
      {"phi-icon-letter-a", "A", L'A', false},
      {"phi-icon-letter-b", "B", L'B', false},
      {"phi-icon-letter-c", "C", L'C', false},
      {"phi-icon-letter-d", "D", L'D', false},
      {"phi-icon-letter-e", "E", L'E', false},
      {"phi-icon-letter-f", "F", L'F', false},
      {"phi-icon-letter-g", "G", L'G', false},
      {"phi-icon-letter-h", "H", L'H', false},
      {"phi-icon-letter-i", "I", L'I', false},
      {"phi-icon-letter-j", "J", L'J', false},
      {"phi-icon-letter-k", "K", L'K', false},
      {"phi-icon-letter-l", "L", L'L', false},
      {"phi-icon-letter-m", "M", L'M', false},
      {"phi-icon-letter-n", "N", L'N', false},
      {"phi-icon-letter-o", "O", L'O', false},
      {"phi-icon-letter-p", "P", L'P', false},
      {"phi-icon-letter-q", "Q", L'Q', false},
      {"phi-icon-letter-r", "R", L'R', false},
      {"phi-icon-letter-s", "S", L'S', false},
      {"phi-icon-letter-t", "T", L'T', false},
      {"phi-icon-letter-u", "U", L'U', false},
      {"phi-icon-letter-v", "V", L'V', false},
      {"phi-icon-letter-w", "W", L'W', false},
      {"phi-icon-letter-x", "X", L'X', false},
      {"phi-icon-letter-y", "Y", L'Y', false},
      {"phi-icon-letter-z", "Z", L'Z', false},
      {"phi-icon-question-mark", "?", 0xE897, true},        // Help
      {"phi-icon-exclamation-mark", "!", 0xE8C9, true},     // Important
      {"phi-icon-tilde", "~", L'~', false},
      {"phi-icon-backtick", "`", L'`', false},
  });
  return *icons;
}

std::string PhiIconCatalogGlyph(const std::string& id) {
  for (const PhiCatalogIcon& icon : PhiIconCatalogAll()) {
    if (id == icon.id) {
      const std::wstring wide(1, icon.glyph);
      return base::WideToUTF8(wide);
    }
  }
  return std::string();
}

const PhiIconInkBox& PhiCatalogInkBox(const PhiMacIcon& icon) {
  // Measured once per mask (keyed by its pixel buffer, which the catalog
  // owns for the process lifetime) — a 48x48 scan is nothing next to the
  // paint it saves from re-measuring.
  static base::NoDestructor<std::map<const unsigned char*, PhiIconInkBox>>
      boxes;
  auto it = boxes->find(icon.alpha);
  if (it != boxes->end()) {
    return it->second;
  }
  PhiIconInkBox box = {icon.width, icon.height, -1, -1};
  for (int y = 0; y < icon.height; ++y) {
    for (int x = 0; x < icon.width; ++x) {
      // SAFETY: `alpha` is a width*height array by construction, and x/y
      // walk it in bounds.
      if (UNSAFE_BUFFERS(icon.alpha[y * icon.width + x]) > 32) {
        box.left = std::min(box.left, x);
        box.top = std::min(box.top, y);
        box.right = std::max(box.right, x);
        box.bottom = std::max(box.bottom, y);
      }
    }
  }
  if (box.right < box.left || box.bottom < box.top) {
    box = {0, 0, icon.width - 1, icon.height - 1};  // Blank: fit the canvas.
  }
  return boxes->emplace(icon.alpha, box).first->second;
}

bool PhiIsPrivateGlyph(const std::wstring& text) {
  return !text.empty() && text.front() >= 0xE000 && text.front() <= 0xF8FF;
}

namespace {

// The artwork a stand-in glyph names: the Phi catalog's, or an agent
// brand mark's (phi_agent_icons.h) — the same draw sites serve both.
const PhiMacIcon* MaskForGlyph(const std::wstring& text) {
  if (const PhiMacIcon* mask = PhiCatalogMaskForGlyph(text)) {
    return mask;
  }
  return PhiAgentIconMaskForGlyph(text);
}

// The catalog entry a stand-in glyph names (null for an agent mark, an
// emoji or initials).
const PhiCatalogIcon* CatalogForGlyph(const std::wstring& text) {
  if (text.size() != 1) {
    return nullptr;
  }
  for (const PhiCatalogIcon& icon : PhiIconCatalogAll()) {
    if (icon.glyph == text.front()) {
      return &icon;
    }
  }
  return nullptr;
}

// §5.9: a catalog pictogram with a Fluent twin draws from the icon font at
// `px` (false when the face lacks the glyph — the mask then stands in); a
// letter, digit or mark is TYPE, Segoe UI Variable Semibold at 13/16 of
// the icon size, centred. False when `text` is neither: the caller draws
// the mask.
bool DrawCatalogGlyph(HDC dc,
                      const RECT& rc,
                      const std::wstring& text,
                      int px,
                      COLORREF ink) {
  const PhiCatalogIcon* icon = CatalogForGlyph(text);
  if (!icon || px <= 0) {
    return false;
  }
  if (icon->fluent) {
    return PhiDrawIconGlyph(dc, rc, text, px, ink);
  }
  if (icon->glyph >= 0x80) {
    return false;  // Mask-only.
  }
  const int height = std::max(1, static_cast<int>(std::lround(px * 13.0 / 16.0)));
  static base::NoDestructor<std::map<int, HFONT>> fonts;
  HFONT& font = (*fonts)[height];
  if (!font) {
    LOGFONTW lf = {};
    const wchar_t* face = PhiTextFaceName(false);
    if (std::wstring_view(face) == L"Segoe UI") {
      face = L"Segoe UI Semibold";
    }
    const std::wstring name = face;
    std::copy_n(name.begin(),
                std::min(name.size(), std::size(lf.lfFaceName) - 1),
                std::begin(lf.lfFaceName));
    lf.lfHeight = -height;
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    font = ::CreateFontIndirectW(&lf);
  }
  HGDIOBJ previous = ::SelectObject(dc, font);
  const int old_mode = ::SetBkMode(dc, TRANSPARENT);
  const COLORREF old_ink = ::SetTextColor(dc, ink);
  RECT box = rc;
  PhiDrawTextAA(dc, text.c_str(), -1, &box,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  ::SetTextColor(dc, old_ink);
  ::SetBkMode(dc, old_mode);
  ::SelectObject(dc, previous);
  return true;
}

}  // namespace

bool PhiDrawCatalogArtwork(HDC dc,
                           const RECT& rc,
                           const std::wstring& text,
                           int px,
                           COLORREF ink) {
  if (DrawCatalogGlyph(dc, rc, text, px, ink)) {
    return true;
  }
  const PhiMacIcon* mask = MaskForGlyph(text);
  if (!mask || px <= 0) {
    return false;
  }
  const std::vector<unsigned char> bgra = PhiTintedIconBgra(*mask, ink, px);
  const int x = rc.left + ((rc.right - rc.left) - px) / 2;
  const int y = rc.top + ((rc.bottom - rc.top) - px) / 2;
  const RECT dest = {x, y, x + px, y + px};
  return PhiBlendBgra(dc, dest, bgra.data(), px, px);
}

bool PhiDrawCatalogIcon(HDC dc,
                        const RECT& rc,
                        const std::wstring& text,
                        int ink_size,
                        COLORREF ink) {
  if (DrawCatalogGlyph(dc, rc, text, ink_size, ink)) {
    return true;
  }
  const PhiMacIcon* mask = MaskForGlyph(text);
  if (!mask || ink_size <= 0 || text.empty()) {
    return false;
  }
  const PhiIconInkBox& box = PhiCatalogInkBox(*mask);
  const int ink_w = box.right - box.left + 1;
  const int ink_h = box.bottom - box.top + 1;
  const int span = std::max(ink_w, ink_h);
  if (span <= 0) {
    return false;
  }
  int canvas = 0;
  if (text.front() < 0x80) {
    // The catalog's letters, numbers and punctuation are TYPE, not pictures:
    // their imagesets hold a glyph at its own cap height (22 of the 48px
    // mask, the same for every one of them) inside the shared canvas, and
    // fitting THAT ink to an icon's span would set a "7" as tall as a cube.
    // They keep the canvas relation the Mac draws them with — a pictorial
    // icon covers about five eighths of it, and a fifth more again brings
    // the cap to about five sixths of an icon: type BESIDE the icons rather
    // than small print under them. Their stand-in glyph is the character
    // itself, which is what tells the two apart.
    canvas = ink_size * 48 / 25;
    // …with a floor. A mark drawn to that shared cap height can still be a
    // speck — the backtick's ink is five mask pixels — and a Space wearing
    // one would read as damage rather than as a character. Nothing in the
    // strip goes below half an icon.
    const double rendered = static_cast<double>(span) * canvas / mask->width;
    const double floor_span = ink_size * 0.55;
    if (rendered < floor_span) {
      canvas = static_cast<int>(
          std::lround(floor_span * mask->width / span));
    }
  } else {
    // The imagesets are 20x20 canvases holding a 16x16 drawing
    // (`translate(2 2) scale(0.8)` in every SVG), and each shape then covers
    // a different part of THAT: the cog fills it, the grid stops short.
    // Fitting the canvas to a slot therefore renders a row of unequal
    // icons. Fit the INK instead and they all read as one size — the Mac
    // reaches the same place from the other end, normalizing its agent
    // imagesets so every glyph's ink spans 42 of 54 and one ratio serves
    // them all (AgentDriverBadge.assetInkRatio).
    canvas = static_cast<int>(std::lround(static_cast<double>(ink_size) *
                                          mask->width / span));
  }
  canvas = std::max(1, canvas);
  const std::vector<unsigned char> bgra =
      PhiTintedIconBgra(*mask, ink, canvas);
  // Placed by the INK's centre, not the canvas's: a glyph is not always
  // centred in its imageset (the "7" sits a pixel to the left of its own),
  // and rounding the canvas into place instead would tilt it further.
  const double scale = static_cast<double>(canvas) / mask->width;
  const int x = (rc.left + rc.right) / 2 -
                static_cast<int>(std::lround((box.left + box.right + 1) / 2.0 *
                                             scale));
  const int y = (rc.top + rc.bottom) / 2 -
                static_cast<int>(std::lround((box.top + box.bottom + 1) / 2.0 *
                                             scale));
  const RECT dest = {x, y, x + canvas, y + canvas};
  return PhiBlendBgra(dc, dest, bgra.data(), canvas, canvas);
}

}  // namespace phi_client
