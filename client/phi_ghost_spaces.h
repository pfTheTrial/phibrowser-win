// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_GHOST_SPACES_H_
#define PHIBROWSER_WIN_CLIENT_PHI_GHOST_SPACES_H_

#include <map>
#include <string>
#include <vector>

namespace phi_client {

// Which Spaces currently exist only as a PARKED window (a "ghost") rather
// than a live browser — the client half of lazy Space restore.
//
// The link between a Space and a parked window is the window's SessionID from
// the PREVIOUS run: the client remembers, per Space, the id its browser had,
// and that same id is what the session file (and so the ghost store) names.
// Materializing hands that id back to the framework, which rebuilds the
// window inside a ScopedRestoredFromWindowId scope so it can be re-attached
// to its Space.
//
// TWO RULES FROM THE FRAMEWORK CONTRACT ARE LOAD-BEARING HERE:
//
//   * The settlement receipt is READ OFF THE WHOLE GHOST STORE, not off what
//     one replay diverted. A profile a reopen never replayed keeps whatever
//     an earlier one parked for it. So ReplaceFromReceipt REPLACES the park
//     set; merging would strand windows that the store no longer reports,
//     and an empty receipt is a meaningful answer ("nothing is parked"), not
//     a missing one.
//   * Materialize has THREE outcomes, not two. kRefusedForNow means "ask
//     again later" and the record MUST be kept, or the Space can never come
//     back; kNoSuchGhost means nothing will ever satisfy it and the record
//     MUST be dropped, or the Space is pinned on a materialization that
//     cannot succeed.
class PhiGhostSpaces {
 public:
  // What the client remembers about a Space's window between runs.
  struct Remembered {
    std::string profile;  // On-disk basename, the ghost store's key.
    int window_id = 0;    // The window's SessionID in the previous session.
  };

  PhiGhostSpaces();
  ~PhiGhostSpaces();

  // Persistence, alongside the other client stores.
  void Load(const std::wstring& path);
  void Save() const;

  // A Space's browser is live: remember the pair so a later run can name it,
  // and clear any ghost record (it is not parked any more).
  void NoteLiveWindow(const std::string& space_id,
                      const std::string& profile,
                      int window_id);

  // What the client should ARM as the eager set: the window ids it wants
  // rebuilt immediately (everything else parks). Empty means "park all".
  std::vector<int> EagerWindowIds(const std::string& active_space_id) const;

  // Takes the framework's settlement receipt and REPLACES the park set from
  // it. Window ids the client cannot map back to a Space are kept as
  // orphans, so they can still be dropped rather than leaking.
  void ReplaceFromReceipt(const std::string& receipt_json);

  // True while `space_id` exists only as a parked window.
  bool IsParked(const std::string& space_id) const;
  // The parked window for `space_id`, or nullptr.
  const Remembered* Parked(const std::string& space_id) const;

  // The Space stopped being a ghost: it materialized, or it was deleted.
  void ClearParked(const std::string& space_id);

  // True when nothing has been remembered yet — the client has no basis to
  // gate a replay on, so it must not try.
  bool Empty() const { return remembered_.empty(); }

  // Which Space remembers `window_id` as its window, or nullptr. This is how
  // a materialized window — announced with the PREVIOUS session's id — finds
  // the Space it belongs to.
  const std::string* SpaceForWindowId(int window_id) const;

  // Every parked window that no Space claims — dropped rather than left to
  // be replayed as a loose window by the next unfiltered restore.
  std::vector<Remembered> Orphans() const;
  void ClearOrphans();

 private:
  std::wstring path_;
  // Space -> the window it had, whether or not it is currently parked.
  std::map<std::string, Remembered> remembered_;
  // The subset that the last receipt says is parked right now.
  std::map<std::string, Remembered> parked_;
  std::vector<Remembered> orphans_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_GHOST_SPACES_H_
