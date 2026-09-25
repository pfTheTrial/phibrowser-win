// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_URL_RULE_NORMALIZE_H_
#define PHIBROWSER_WIN_CLIENT_PHI_URL_RULE_NORMALIZE_H_

#include <optional>
#include <string>
#include <string_view>

// URL rule normalisation — phibrowser-mac LocalStore.normalizedHost /
// normalizedPathPrefix (LocalStore+SpaceURLRule.swift), ported byte for
// byte: a rule's identity, its merge-partner signature and the routing
// table all read these canonical forms, so Windows and the Mac must produce
// the same bytes from the same input. Pinned by the Mac-generated vectors
// (test/sync-vectors: urlrule_normalized_host, urlrule_normalized_path_prefix).
//
//   host: leading White_Space trimmed; the trailing run of White_Space and
//         '.' stripped in one scan; Unicode full lowercase (per scalar, as
//         Swift's lowercased()).
//   path: nil stays nil; White_Space trimmed; "/" stays "/" (the root
//         ONLY — not "any path"); empty is nil (any path); a leading '/' is
//         ensured; percent-decoded when that is valid UTF-8, re-encoded
//         with urlPathAllowed; trailing '/' removed (never the lone one).
//
// PURE: STL only, host-tested.

namespace phi_client::store {

std::string NormalizedRuleHost(std::string_view raw);
// A new SpaceURLRule id, the Mac's UUID().uuidString form (uppercase).
std::string NewRuleId();
std::optional<std::string> NormalizedRulePathPrefix(
    const std::optional<std::string>& raw);

}  // namespace phi_client::store

#endif  // PHIBROWSER_WIN_CLIENT_PHI_URL_RULE_NORMALIZE_H_
