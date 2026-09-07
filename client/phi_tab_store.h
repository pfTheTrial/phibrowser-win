// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TAB_STORE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TAB_STORE_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace phi_client {

// The client's own store for pinned tabs and bookmarks — the Windows twin of
// the Mac's LocalStore (Sources/LocalStorage). Same shape, deliberately: ONE
// row type distinguished by `type`, so pinned tabs and bookmarks share their
// storage, ordering and ownership rules exactly as they do on Mac.
// Persisted as the integer; 0 was an unused "tab" kind and is rejected on
// load.
enum class PhiRowType { kPinnedTab = 1, kBookmark = 2, kBookmarkFolder = 3 };

// The JSON file's schema version. 1: pinned splits were TWO rows cross-linked
// by splitPartnerGuid. 2 (2026-09-07): ONE row per cell for pins and
// bookmarks alike, pane B in url_b/title_b/favicon_b (see
// docs/sidebar-architecture.md §4.1); Load folds schema-1 pairs.
inline constexpr int kPhiTabStoreSchema = 2;

// WHERE A PINNED TAB LIVES — the Mac's PinnedTabScope, an APP-WIDE setting
// rather than a per-tab one. It decides who owns the row:
//
//   kSpace    profileId + spaceId   pinned in that one Space
//   kProfile  profileId, no space   pinned across every Space of a profile
//   kApp      neither               pinned everywhere
//
// Ordered by the Mac's hierarchyLevel (space 0, profile 1, app 2).
enum class PhiPinScope { kSpace = 0, kProfile = 1, kApp = 2 };

const char* PhiPinScopeName(PhiPinScope scope);
bool PhiPinScopeFromName(const std::string& name, PhiPinScope* out);

struct PhiRow {
  std::string guid;
  PhiRowType type = PhiRowType::kBookmark;
  std::string title;
  std::string url;
  int index = 0;
  // Ownership. Empty means "not scoped to one", which is how the Mac's nil
  // profileId/spaceId behave.
  std::string profile_id;
  std::string space_id;
  std::string parent_guid;  // Bookmarks only: the tree.
  // The site's favicon, captured when the row was created — the Mac keeps a
  // Data blob on the row for the same reason: a pinned tab has no live
  // WebContents to ask, so the icon has to be stored with it.
  std::string favicon_base64;  // Raw BGRA, as PhiFramework hands it over.
  int favicon_width = 0;
  int favicon_height = 0;
  // Survives a scope change so rows that came from one original pin can be
  // merged back together, as the Mac's pinLineageId does.
  std::string lineage_id;
  // Split cell (pins and bookmarks alike): ONE row, the second pane riding
  // here. url_b non-empty is what makes a row a split. Pane B keeps its own
  // title and favicon; a live pane B is tagged TagFor(guid, /*pane_b=*/true).
  std::string url_b;
  std::string title_b;
  std::string favicon_b_base64;
  int favicon_b_width = 0;
  int favicon_b_height = 0;
  // Bookmark FOLDERS only: which catalog symbol the folder wears (the Mac's
  // Bookmark.folderIconName). Empty and unknown ids both mean "default".
  std::string icon;
  // SPLIT rows only: the divider axis the split reopens with — "vertical"
  // (side-by-side) or "horizontal" (stacked), the Mac's SplitLayout raw
  // values. Empty means side-by-side, keeping older records compatible.
  std::string layout;
};

// Rows plus the one scope setting, persisted as a single JSON file with a
// schema number (kPhiTabStoreSchema). Older files are migrated on Load and
// rewritten at once, so a change is a migration, never a guess.
class PhiTabStore {
 public:
  PhiTabStore();
  ~PhiTabStore();

  // Reads `path` if it exists; an absent or unreadable file just means empty.
  void Load(const std::wstring& path);
  void Save() const;

  PhiPinScope pin_scope() const { return pin_scope_; }
  // Re-owns every pinned row for the new scope and merges rows that become
  // indistinguishable, the way the Mac's scope migration does. Returns false
  // when the scope was already set.
  bool SetPinScope(PhiPinScope scope);

  // Pinned tabs visible to (profile, space) under the current scope, ordered.
  std::vector<PhiRow> PinnedFor(const std::string& profile_id,
                                const std::string& space_id) const;
  // Pins `url`/`title`, owned according to the current scope. Returns the guid.
  std::string AddPinned(const std::string& profile_id,
                        const std::string& space_id,
                        const std::string& title,
                        const std::string& url,
                        const std::string& favicon_base64,
                        int favicon_width,
                        int favicon_height);

  // Bookmarks directly under `parent_guid` (empty = the root for this owner).
  std::vector<PhiRow> BookmarksFor(const std::string& profile_id,
                                   const std::string& space_id,
                                   const std::string& parent_guid) const;
  std::string AddBookmark(const std::string& profile_id,
                          const std::string& space_id,
                          const std::string& parent_guid,
                          const std::string& title,
                          const std::string& url,
                          bool folder,
                          const std::string& favicon_base64,
                          int favicon_width,
                          int favicon_height);

  // Removes a row and, for a folder, everything beneath it.
  bool Remove(const std::string& guid);
  // Whether a row with `guid` exists (any type).
  bool Contains(const std::string& guid) const;
  // Makes `guid` a split cell (pin or bookmark) by setting its second pane,
  // or clears pane B when `url_b` is empty. False when unknown.
  bool SetRowSecondary(const std::string& guid,
                       const std::string& url_b,
                       const std::string& title_b,
                       const std::string& favicon_b_base64 = std::string(),
                       int favicon_b_width = 0,
                       int favicon_b_height = 0);
  // Retitles a row (the context menus' Rename). False when unknown.
  bool RenameRow(const std::string& guid, const std::string& title);
  // Sets a bookmark folder's catalog symbol. False when unknown or unchanged.
  bool SetRowIcon(const std::string& guid, const std::string& icon);
  // Sets a split row's divider axis. False when unknown or unchanged.
  bool SetRowLayout(const std::string& guid, const std::string& layout);
  // Swaps a split cell's two panes (title/url/favicon). False unless split.
  bool SwapPanes(const std::string& guid);
  // Removes every row OWNED by `space_id` (space-scoped pins, the Space's
  // bookmarks) — what deleting a Space takes with it on Mac. Rows owned at
  // profile or app scope carry no space id and stay. Returns removed count.
  int RemoveForSpace(const std::string& space_id);
  // Re-homes every row OWNED by `space_id` onto `profile_id` — what changing
  // a Space's profile does. The Mac's promise is that bookmarks (and, at
  // Space scope, pinned tabs) stay with the SPACE, so the profile half of
  // their owner moves with it; rows owned at profile or app scope carry no
  // space id and stay where they are. Returns the number rewritten.
  int RehomeSpace(const std::string& space_id, const std::string& profile_id);
  // Moves a row within its siblings.
  bool Reorder(const std::string& guid, int new_index);
  // The row with `guid`, or null. The pointer holds only until the next
  // mutation on this store.
  const PhiRow* Find(const std::string& guid) const;
  // Rewrites a row's url (pinned tabs and bookmarks; folders refuse).
  bool SetRowUrl(const std::string& guid, const std::string& url);
  // Reparents a bookmark row under `new_parent` (empty = the owner's root)
  // at `new_index` (negative appends). The parent must be a folder with the
  // same owner; moving a folder into itself or a descendant is refused.
  bool Move(const std::string& guid,
            const std::string& new_parent,
            int new_index);

  const std::vector<PhiRow>& rows() const { return rows_; }
  // Schema 1 -> 2 leftovers: the guid of every pinned row folded away as a
  // pane B, mapped to the tag its live tab must carry now
  // (TagFor(survivor, true)). Session restore brings such tabs back with the
  // old guid as their tag — possibly launches later — so the map is
  // persisted with the file and each entry dropped once applied.
  const std::map<std::string, std::string>& legacy_tag_aliases() const {
    return legacy_tag_aliases_;
  }
  void DropLegacyTagAlias(const std::string& old_guid);

 private:
  // The (profileId, spaceId) a new pinned row gets under the current scope.
  void OwnerForScope(const std::string& profile_id,
                     const std::string& space_id,
                     std::string* out_profile,
                     std::string* out_space) const;
  // Schema 1 -> 2 (see kPhiTabStoreSchema).
  void FoldLegacySplitPairs(
      const std::map<std::string, std::string>& partner_of);
  int NextIndexFor(PhiRowType type,
                   const std::string& profile_id,
                   const std::string& space_id,
                   const std::string& parent_guid) const;

  std::vector<PhiRow> rows_;
  std::map<std::string, std::string> legacy_tag_aliases_;
  PhiPinScope pin_scope_ = PhiPinScope::kProfile;  // The Mac's fallback.
  std::wstring path_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TAB_STORE_H_
