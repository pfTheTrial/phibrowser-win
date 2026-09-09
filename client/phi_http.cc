// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_http.h"

#include <windows.h>

#include <winhttp.h>

#include <cstdlib>

#include "base/strings/utf_string_conversions.h"

#pragma comment(lib, "winhttp.lib")

namespace phi_client {

bool PhiSplitUrl(const std::string& url,
                 std::wstring* host,
                 std::wstring* path,
                 int* port,
                 bool* secure) {
  std::string rest;
  if (url.rfind("https://", 0) == 0) {
    *secure = true;
    rest = url.substr(8);
  } else if (url.rfind("http://", 0) == 0) {
    *secure = false;
    rest = url.substr(7);
  } else {
    return false;
  }
  const size_t slash = rest.find('/');
  std::string authority =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  const std::string path_part =
      slash == std::string::npos ? std::string("/") : rest.substr(slash);
  *port = *secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
  const size_t colon = authority.find(':');
  if (colon != std::string::npos) {
    *port = std::atoi(authority.substr(colon + 1).c_str());
    authority = authority.substr(0, colon);
  }
  if (authority.empty() || *port <= 0 || *port > 65535) {
    return false;
  }
  *host = base::UTF8ToWide(authority);
  *path = base::UTF8ToWide(path_part);
  return true;
}

PhiHttpResult PhiHttpRequest(const wchar_t* method,
                             const std::string& url,
                             const std::wstring& headers,
                             const std::string& body) {
  PhiHttpResult result;
  std::wstring host;
  std::wstring path;
  int port = 0;
  bool secure = false;
  if (!PhiSplitUrl(url, &host, &path, &port, &secure)) {
    return result;
  }
  HINTERNET session = ::WinHttpOpen(L"PhiBrowser-Win",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    return result;
  }
  HINTERNET connect = ::WinHttpConnect(session, host.c_str(),
                                       static_cast<INTERNET_PORT>(port), 0);
  if (connect) {
    HINTERNET request = ::WinHttpOpenRequest(
        connect, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (request) {
      if (::WinHttpSendRequest(request,
                               headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                               : headers.c_str(),
                               static_cast<DWORD>(-1),
                               body.empty() ? WINHTTP_NO_REQUEST_DATA
                                            : const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0) &&
          ::WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        ::WinHttpQueryHeaders(
            request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
            WINHTTP_NO_HEADER_INDEX);
        result.status = static_cast<int>(status);
        result.transport_ok = true;
        DWORD available = 0;
        while (::WinHttpQueryDataAvailable(request, &available) &&
               available > 0) {
          std::string chunk(available, '\0');
          DWORD read = 0;
          if (!::WinHttpReadData(request, chunk.data(), available, &read) ||
              read == 0) {
            break;
          }
          result.body.append(chunk.data(), read);
        }
      }
      ::WinHttpCloseHandle(request);
    }
    ::WinHttpCloseHandle(connect);
  }
  ::WinHttpCloseHandle(session);
  return result;
}

}  // namespace phi_client
