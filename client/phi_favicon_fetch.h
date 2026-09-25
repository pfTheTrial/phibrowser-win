// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FAVICON_FETCH_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FAVICON_FETCH_H_

#include <string>

namespace phi_client {

// The Mac's PhiFaviconHostFilter: http(s) with a real host — never
// localhost, *.local or a numeric IP literal.
bool PhiFaviconHostAllowed(const std::string& page_url);

// The favicon backfill's network step (PhiFaviconFetcher.networkFavicon):
// https://<host>/favicon.ico, no cookies, 5 s, 64 KB, then decoded (WIC:
// ICO, PNG, …) to `px`×`px` premultiplied BGRA. Blocking; any thread.
bool PhiFetchSiteFavicon(const std::string& page_url,
                         int px,
                         std::string* bgra);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FAVICON_FETCH_H_
