// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_MAC_ICONS_H_
#define PHIBROWSER_WIN_CLIENT_PHI_MAC_ICONS_H_

#include <windows.h>

#include <vector>

namespace phi_client {

// The Mac sidebar's own icon artwork (memory-icon, sidebar-chat and
// sidebar-feedback imagesets), rasterized from their PDFs into 48px alpha
// masks. All three are template images on Mac — the SHAPE is the asset and
// the ink comes from the theme — so the client tints them at draw time.
struct PhiMacIcon {
  int width;
  int height;
  const unsigned char* alpha;
};

const PhiMacIcon& PhiDownloadIcon();
const PhiMacIcon& PhiMemoryIcon();
const PhiMacIcon& PhiChatIcon();
const PhiMacIcon& PhiFeedbackIcon();

// `icon` tinted `color` as premultiplied BGRA at `out_size` pixels square
// (area-averaged from the 48px mask), ready for PhiBlendBgra — the Mac's
// template rendering. Pass out_size <= 0 to keep the mask's own size.
std::vector<unsigned char> PhiTintedIconBgra(const PhiMacIcon& icon,
                                             COLORREF color,
                                             int out_size);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_MAC_ICONS_H_
