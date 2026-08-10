// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_EMOJI_H_
#define PHIBROWSER_WIN_CLIENT_PHI_EMOJI_H_

#include <string>
#include <vector>

namespace phi_client {

// The Mac client's Space-icon emoji library, ported. The catalog file itself is
// the SAME artifact the Mac bundles (Resources/Emoji/emoji-catalog.json,
// Unicode 17.0, nine groups, 1914 emoji) so both clients offer exactly the same
// icons and agree on every id.
struct PhiEmoji {
  std::string id;    // Unicode codepoint sequence, e.g. "1F600".
  std::string text;  // The emoji itself, UTF-8.
  std::string name;
};

struct PhiEmojiGroup {
  std::string name;
  std::vector<PhiEmoji> items;
};

// Loaded once from <exe dir>\resources\emoji-catalog.json. Empty if the file is
// missing — callers fall back rather than fail.
const std::vector<PhiEmojiGroup>& PhiEmojiGroups();
// The emoji for `id`, or empty.
std::string PhiEmojiTextForId(const std::string& id);

// A Space's icon is stored the way the Mac stores it (IconPickerSelection):
//   "emoji:<id>"  an emoji from the catalog
//   "phi:<id>"    one of the Mac's own icons — recognised, not yet drawn here
// so a Space configured on either client means the same thing on the other.
inline constexpr char kPhiEmojiPrefix[] = "emoji:";
inline constexpr char kPhiIconPrefix[] = "phi:";
// The Mac default Space's icon (LocalStore.defaultSpaceIconName).
inline constexpr char kPhiDefaultSpaceIcon[] = "phi:phi-icon-view-grid-add";

// Resolves a stored icon to display text. Returns empty for a "phi:" icon (no
// glyph for it yet) and for anything unrecognised.
std::string PhiIconDisplayText(const std::string& storage_value);
// True when `storage_value` is a form this client understands well enough to
// store — an emoji id in the catalog, or any "phi:" id.
bool PhiIconIsValid(const std::string& storage_value);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_EMOJI_H_
