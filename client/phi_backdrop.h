// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_BACKDROP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_BACKDROP_H_

#include <algorithm>
#include <cstdint>

namespace phi_client {

// Recover coverage from GDI's opaque RGB painting over a known ground.
// Removing the ground contribution keeps antialiased glyph edges from
// acquiring opaque halos on glass. A 20% tint remains over the OS backdrop.
// Inputs are packed RGB (0xRRGGBB); output is premultiplied ARGB for a DIB.
inline int PhiBackdropCoverage(uint32_t pixel, uint32_t ground) {
  int coverage = 0;
  for (int shift : {0, 8, 16}) {
    const int c = (pixel >> shift) & 255;
    const int g = (ground >> shift) & 255;
    if (c < g) {
      coverage = std::max(coverage, ((g - c) * 255 + g - 1) / g);
    } else if (c > g) {
      coverage = std::max(coverage,
                          ((c - g) * 255 + 254 - g) / (255 - g));
    }
  }
  return coverage;
}

inline uint32_t PhiBackdropPixel(uint32_t pixel, uint32_t ground) {
  const int alpha = 51 + (PhiBackdropCoverage(pixel, ground) * 204 + 127) / 255;
  uint32_t result = static_cast<uint32_t>(alpha) << 24;
  for (int shift : {0, 8, 16}) {
    const int c = (pixel >> shift) & 255;
    const int g = (ground >> shift) & 255;
    const int premultiplied = c - (g * (255 - alpha) + 127) / 255;
    result |= static_cast<uint32_t>(std::clamp(premultiplied, 0, alpha))
              << shift;
  }
  return result;
}

// Replace a GDI image's opaque ground with a premultiplied backdrop, while
// preserving its controls' coverage. Derive the backdrop by removing the
// neutral white/black backing from the theme ground with PhiBackdropPixel:
// fading the already-tinted ground itself would attenuate the theme twice.
inline uint32_t PhiBackdropPixelOver(uint32_t pixel,
                                     uint32_t ground,
                                     uint32_t backdrop) {
  const int coverage = PhiBackdropCoverage(pixel, ground);
  const int remaining = 255 - coverage;
  const int alpha = coverage + ((backdrop >> 24) * remaining + 127) / 255;
  uint32_t result = static_cast<uint32_t>(alpha) << 24;
  for (int shift : {0, 8, 16}) {
    const int c = (pixel >> shift) & 255;
    const int g = (ground >> shift) & 255;
    const int b = (backdrop >> shift) & 255;
    const int channel = c - (g * remaining + 127) / 255 +
                        (b * remaining + 127) / 255;
    result |= static_cast<uint32_t>(std::clamp(channel, 0, alpha)) << shift;
  }
  return result;
}

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_BACKDROP_H_
