// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_HTTP_H_
#define PHIBROWSER_WIN_CLIENT_PHI_HTTP_H_

#include <string>

namespace phi_client {

// One HTTP round trip over WinHTTP — the client's requests to the Auth0
// tenant, the account service and the feedback uploads' presigned URLs
// (lane 7 §5.5 counted four copies of this loop). BLOCKING: worker threads
// only, never the UI thread.
struct PhiHttpResult {
  // The request reached a server and a status line came back. False on a
  // DNS/TCP/TLS failure (offline) or a URL the helper cannot parse; `status`
  // and `body` are meaningless then. An HTTP error (4xx/5xx) is still
  // transport_ok — the caller reads `status`.
  bool transport_ok = false;
  int status = 0;
  std::string body;
};

// `url` is absolute (http or https, an optional port). `headers` is zero or
// more "Name: value\r\n" lines; `body` is sent as is (empty = no body). The
// proxy is Windows' automatic configuration.
PhiHttpResult PhiHttpRequest(const wchar_t* method,
                             const std::string& url,
                             const std::wstring& headers,
                             const std::string& body);

// Splits an absolute URL into WinHTTP's parts. Exposed for the outbox's
// presigned URLs; false when the scheme or authority is not usable.
bool PhiSplitUrl(const std::string& url,
                 std::wstring* host,
                 std::wstring* path,
                 int* port,
                 bool* secure);

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_HTTP_H_
