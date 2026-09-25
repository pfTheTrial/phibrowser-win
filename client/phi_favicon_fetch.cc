// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_favicon_fetch.h"

#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>

#include "phibrowser-win/client/phi_http.h"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ws2_32.lib")

namespace phi_client {

namespace {

using Microsoft::WRL::ComPtr;

constexpr int kTimeoutMs = 5000;
constexpr size_t kMaxBytes = 65536;

// The URL's host, lowercased, brackets stripped; "" when it has none.
std::string HostOf(const std::string& url, std::string* scheme) {
  const size_t colon = url.find("://");
  if (colon == std::string::npos) {
    return std::string();
  }
  *scheme = url.substr(0, colon);
  std::transform(scheme->begin(), scheme->end(), scheme->begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  std::string rest = url.substr(colon + 3);
  rest = rest.substr(0, rest.find_first_of("/?#"));
  if (const size_t at = rest.rfind('@'); at != std::string::npos) {
    rest = rest.substr(at + 1);
  }
  std::string host;
  if (!rest.empty() && rest[0] == '[') {
    host = rest.substr(1, rest.find(']') - 1);
  } else {
    host = rest.substr(0, rest.find(':'));
  }
  std::transform(host.begin(), host.end(), host.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return host;
}

bool IsIpLiteral(const std::string& host) {
  IN6_ADDR v6 = {};
  IN_ADDR v4 = {};
  return ::inet_pton(AF_INET6, host.c_str(), &v6) == 1 ||
         ::inet_pton(AF_INET, host.c_str(), &v4) == 1 ||
         (!host.empty() && std::all_of(host.begin(), host.end(), [](char c) {
           return std::isdigit(static_cast<unsigned char>(c)) || c == '.' ||
                  c == 'x';
         }));
}

bool EndsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

bool PhiFaviconHostAllowed(const std::string& page_url) {
  std::string scheme;
  const std::string host = HostOf(page_url, &scheme);
  if ((scheme != "http" && scheme != "https") || host.empty()) {
    return false;
  }
  if (host == "localhost" || EndsWith(host, ".localhost") || host == "local" ||
      EndsWith(host, ".local")) {
    return false;
  }
  return !IsIpLiteral(host);
}

bool PhiFetchSiteFavicon(const std::string& page_url, int px, std::string* bgra) {
  if (!PhiFaviconHostAllowed(page_url) || px <= 0) {
    return false;
  }
  std::string scheme;
  const std::string host = HostOf(page_url, &scheme);
  const PhiHttpResult result = PhiHttpRequestBounded(
      L"GET", "https://" + host + "/favicon.ico", L"Accept: image/*\r\n",
      kTimeoutMs, kMaxBytes);
  if (!result.transport_ok || result.status < 200 || result.status >= 300 ||
      result.body.empty()) {
    return false;
  }
  const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  bool ok = false;
  {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                     CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) &&
        SUCCEEDED(stream->InitializeFromMemory(
            reinterpret_cast<BYTE*>(const_cast<char*>(result.body.data())),
            static_cast<DWORD>(result.body.size()))) &&
        SUCCEEDED(factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder))) {
      // An .ico holds several sizes: the smallest one at least `px`, else
      // the largest.
      UINT frames = 0;
      decoder->GetFrameCount(&frames);
      ComPtr<IWICBitmapFrameDecode> best;
      UINT best_size = 0;
      for (UINT i = 0; i < frames; ++i) {
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT width = 0;
        UINT height = 0;
        if (FAILED(decoder->GetFrame(i, &frame)) ||
            FAILED(frame->GetSize(&width, &height)) || !width || !height) {
          continue;
        }
        const UINT size = std::max(width, height);
        const bool better =
            !best ||
            (size >= static_cast<UINT>(px)
                 ? (best_size < static_cast<UINT>(px) || size < best_size)
                 : (best_size < static_cast<UINT>(px) && size > best_size));
        if (better) {
          best = frame;
          best_size = size;
        }
      }
      ComPtr<IWICBitmapScaler> scaler;
      ComPtr<IWICFormatConverter> converter;
      if (best && SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
          SUCCEEDED(scaler->Initialize(best.Get(), static_cast<UINT>(px),
                                       static_cast<UINT>(px),
                                       WICBitmapInterpolationModeFant)) &&
          SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
          SUCCEEDED(converter->Initialize(
              scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
              nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        bgra->assign(static_cast<size_t>(px) * px * 4, '\0');
        ok = SUCCEEDED(converter->CopyPixels(
            nullptr, static_cast<UINT>(px * 4), static_cast<UINT>(bgra->size()),
            reinterpret_cast<BYTE*>(bgra->data())));
      }
    }
  }
  if (SUCCEEDED(com)) {
    ::CoUninitialize();
  }
  if (!ok) {
    bgra->clear();
  }
  return ok;
}

}  // namespace phi_client
