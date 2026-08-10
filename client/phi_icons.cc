// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_icons.h"

#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"

namespace phi_client {

const std::vector<PhiCatalogIcon>& PhiIconCatalogAll() {
  // Ids and ORDER are the Mac's (PhiIconCatalog.swift). Glyphs are Segoe
  // MDL2 Assets stand-ins from the stable E7xx/E8xx core set — close in
  // meaning, not the Mac's artwork; the artwork port is a later slice.
  static const base::NoDestructor<std::vector<PhiCatalogIcon>> icons({
      {"phi-icon-mail", "mail", 0xE715},
      {"phi-icon-color-swatch", "color-swatch", 0xE790},
      {"phi-icon-camera", "camera", 0xE722},
      {"phi-icon-chat", "chat", 0xE8BD},
      {"phi-icon-bell", "bell", 0xE7ED},
      {"phi-icon-user", "user", 0xE77B},
      {"phi-icon-globe-alt", "globe-alt", 0xE774},
      {"phi-icon-sun", "sun", 0xE706},
      {"phi-icon-sparkles", "sparkles", 0xE734},
      {"phi-icon-credit-card", "credit-card", 0xE8C7},
      {"phi-icon-ticket", "ticket", 0xE8EC},
      {"phi-icon-moon", "moon", 0xE708},
      {"phi-icon-briefcase", "briefcase", 0xE821},
      {"phi-icon-archive", "archive", 0xE7B8},
      {"phi-icon-lock-closed", "lock-closed", 0xE72E},
      {"phi-icon-office-building", "office-building", 0xE80F},
      {"phi-icon-chart-pie", "chart-pie", 0xE9D9},
      {"phi-icon-shield-exclamation", "shield-exclamation", 0xEA18},
      {"phi-icon-photograph", "photograph", 0xE91B},
      {"phi-icon-cog", "cog", 0xE713},
      {"phi-icon-filter", "filter", 0xE71C},
      {"phi-icon-qrcode", "qrcode", 0xE8A6},
      {"phi-icon-clipboard-list", "clipboard-list", 0xE77F},
      {"phi-icon-lightning-bolt", "lightning-bolt", 0xE945},
      {"phi-icon-light-bulb", "light-bulb", 0xEA80},
      {"phi-icon-view-grid-add", "view-grid-add", 0xE8A9},
      {"phi-icon-md-library", "md-library", 0xE8F1},
      {"phi-icon-play", "play", 0xE768},
      {"phi-icon-presentation-chart-line", "presentation-chart-line", 0xE9D2},
      {"phi-icon-chip", "chip", 0xE7F8},
      {"phi-icon-cube-transparent", "cube-transparent", 0xE8B3},
      {"phi-icon-search-circle", "search-circle", 0xE721},
      {"phi-icon-cloud", "cloud", 0xE753},
      {"phi-icon-academic-cap", "academic-cap", 0xE7BE},
      {"phi-icon-gift", "gift", 0xE719},
      {"phi-icon-truck", "truck", 0xE7C0},
      {"phi-icon-cake", "cake", 0xE728},
      {"phi-icon-beaker", "beaker", 0xE9F9},
      {"phi-icon-table", "table", 0xE80A},
      {"phi-icon-paper-airplane", "paper-airplane", 0xE724},
      {"phi-icon-device-mobile", "device-mobile", 0xE8EA},
      {"phi-icon-music-note", "music-note", 0xE8D6},
      {"phi-icon-film", "film", 0xE714},
      {"phi-icon-database", "database", 0xE7C3},
      {"phi-icon-server", "server", 0xE83B},
      {"phi-icon-identification", "identification", 0xE8D4},
      {"phi-icon-rss", "rss", 0xE736},
      {"phi-icon-map", "map", 0xE707},
      {"phi-icon-number-1", "1", L'1'},
      {"phi-icon-number-2", "2", L'2'},
      {"phi-icon-number-3", "3", L'3'},
      {"phi-icon-number-4", "4", L'4'},
      {"phi-icon-number-5", "5", L'5'},
      {"phi-icon-number-6", "6", L'6'},
      {"phi-icon-number-7", "7", L'7'},
      {"phi-icon-number-8", "8", L'8'},
      {"phi-icon-number-9", "9", L'9'},
      {"phi-icon-number-0", "0", L'0'},
      {"phi-icon-letter-a", "A", L'A'},
      {"phi-icon-letter-b", "B", L'B'},
      {"phi-icon-letter-c", "C", L'C'},
      {"phi-icon-letter-d", "D", L'D'},
      {"phi-icon-letter-e", "E", L'E'},
      {"phi-icon-letter-f", "F", L'F'},
      {"phi-icon-letter-g", "G", L'G'},
      {"phi-icon-letter-h", "H", L'H'},
      {"phi-icon-letter-i", "I", L'I'},
      {"phi-icon-letter-j", "J", L'J'},
      {"phi-icon-letter-k", "K", L'K'},
      {"phi-icon-letter-l", "L", L'L'},
      {"phi-icon-letter-m", "M", L'M'},
      {"phi-icon-letter-n", "N", L'N'},
      {"phi-icon-letter-o", "O", L'O'},
      {"phi-icon-letter-p", "P", L'P'},
      {"phi-icon-letter-q", "Q", L'Q'},
      {"phi-icon-letter-r", "R", L'R'},
      {"phi-icon-letter-s", "S", L'S'},
      {"phi-icon-letter-t", "T", L'T'},
      {"phi-icon-letter-u", "U", L'U'},
      {"phi-icon-letter-v", "V", L'V'},
      {"phi-icon-letter-w", "W", L'W'},
      {"phi-icon-letter-x", "X", L'X'},
      {"phi-icon-letter-y", "Y", L'Y'},
      {"phi-icon-letter-z", "Z", L'Z'},
      {"phi-icon-question-mark", "?", L'?'},
      {"phi-icon-exclamation-mark", "!", L'!'},
      {"phi-icon-tilde", "~", L'~'},
      {"phi-icon-backtick", "`", L'`'},
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

bool PhiIsPrivateGlyph(const std::wstring& text) {
  return !text.empty() && text.front() >= 0xE000 && text.front() <= 0xF8FF;
}

}  // namespace phi_client
