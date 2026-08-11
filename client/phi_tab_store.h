// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TAB_STORE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TAB_STORE_H_

#include <optional>
#include <string>
#include <vector>

namespace phi_client {

// The client's own store for pinned tabs and bookmarks — the Windows twin of
// the Mac's LocalStore (Sources/LocalStorage). Same shape, deliberately: ONE
// row type distinguished by `type`, so pinned tabs and bookmarks share their
// storage, ordering and ownership rules exactly as they do on Mac.
enum class PhiRowType { kTab = 0, kPinnedTab = 1, kBookmark = 2, kBookmarkFolder = 3 };

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
};

// Rows plus the one scope setting, persisted as a single JSON file. Kept
// deliberately simple: the Mac needs SwiftData migrations because its schema
// has nine versions behind it; this starts at one.
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
  // Removes every row OWNED by `space_id` (space-scoped pins, the Space's
  // bookmarks) — what deleting a Space takes with it on Mac. Rows owned at
  // profile or app scope carry no space id and stay. Returns removed count.
  int RemoveForSpace(const std::string& space_id);
  // Moves a row within its siblings.
  bool Reorder(const std::string& guid, int new_index);

  const std::vector<PhiRow>& rows() const { return rows_; }

 private:
  // The (profileId, spaceId) a new pinned row gets under the current scope.
  void OwnerForScope(const std::string& profile_id,
                     const std::string& space_id,
                     std::string* out_profile,
                     std::string* out_space) const;
  int NextIndexFor(PhiRowType type,
                   const std::string& profile_id,
                   const std::string& space_id,
                   const std::string& parent_guid) const;

  std::vector<PhiRow> rows_;
  PhiPinScope pin_scope_ = PhiPinScope::kProfile;  // The Mac's fallback.
  std::wstring path_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TAB_STORE_H_
