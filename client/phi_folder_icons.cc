// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_folder_icons.h"

#include <algorithm>

#include "base/no_destructor.h"

namespace phi_client {

const std::vector<PhiFolderIcon>& PhiFolderIconsAll() {
  // BookmarkFolderIcon.allCases order — the picker reads left to right in
  // exactly this sequence on both clients.
  static const base::NoDestructor<std::vector<PhiFolderIcon>> catalog({
      {"default", "Default"},
      {"github", "GitHub"},
      {"discord", "Discord"},
      {"x", "X"},
      {"reddit", "Reddit"},
      {"facebook", "Facebook"},
      {"emoji-happy", "Happy"},
      {"emoji-sad", "Sad"},
      {"key", "Key"},
      {"download", "Download"},
      {"thumb-up", "Thumb Up"},
      {"chart-bar", "Chart"},
      {"location-marker", "Location"},
      {"currency-dollar", "Currency"},
      {"shield-check", "Shield"},
      {"heart", "Heart"},
      {"bookmark", "Bookmark"},
      {"refresh", "Refresh"},
      {"chat", "Chat"},
      {"fire", "Fire"},
      {"puzzle", "Puzzle"},
      {"variable", "Variable"},
      {"save", "Save"},
      {"backspace", "Backspace"},
  });
  return *catalog;
}

const char* PhiFolderIconDefaultId() {
  return "default";
}

const PhiMacIcon* PhiPlainFolderMask(bool expanded) {
  return PhiFolderIconMask(expanded ? "folder-open" : "folder-closed");
}

std::string PhiResolveFolderIcon(const std::string& stored) {
  const std::vector<PhiFolderIcon>& all = PhiFolderIconsAll();
  const bool known = std::any_of(
      all.begin(), all.end(),
      [&](const PhiFolderIcon& icon) { return stored == icon.id; });
  return known ? stored : std::string(PhiFolderIconDefaultId());
}

}  // namespace phi_client
