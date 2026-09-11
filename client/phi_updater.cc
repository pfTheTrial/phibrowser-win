// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_updater.h"

#include <windows.h>

#include <string>

#include "base/logging.h"
#include "phibrowser-win/client/phi_features.h"
#include "phibrowser-win/client/phi_version.h"

namespace phi_client {

namespace {

// The Windows twin of the Mac's SUFEED_URL
// (ota.phibrowser.com/mac-public/appcast/PhiBrowserMacUpdate.xml). A
// --dev-hooks build may override it with the PHI_APPCAST_URL environment
// variable; a release build has no such door (kPhiDevHooks).
constexpr char kDefaultAppcastUrl[] =
    "https://ota.phibrowser.com/win-nightly/appcast/PhiBrowserWinUpdate.xml";

// The appcast's EdDSA (Ed25519) public key, base64 — the counterpart of the
// key installer/make-appcast.sh signs with (`openssl genpkey -algorithm
// ed25519`; derive this value with `openssl pkey -in key.pem -pubout
// -outform DER | tail -c32 | base64`). EMPTY until the release key is
// minted, and WITHOUT IT THE UPDATER STAYS OFF: HTTPS authenticates the
// server, not the feed — anyone who can write to the OTA bucket, or sit on
// a proxy the box trusts, would otherwise hand every install an executable
// that runs as the user. tools/release-gate.py refuses a release while this
// is empty.
constexpr char kEdDsaPublicKey[] = "";

// The WinSparkle C surface we use, resolved by hand: the DLL is optional.
using SetAppcastUrlFn = void(__cdecl*)(const char*);
using SetAppDetailsFn = void(__cdecl*)(const wchar_t*,
                                       const wchar_t*,
                                       const wchar_t*);
using SetEdDsaKeyFn = int(__cdecl*)(const char*);
using SetAutomaticCheckFn = void(__cdecl*)(int);
using VoidFn = void(__cdecl*)();

struct Sparkle {
  HMODULE dll = nullptr;
  SetAppcastUrlFn set_appcast_url = nullptr;
  SetAppDetailsFn set_app_details = nullptr;
  SetEdDsaKeyFn set_eddsa_public_key = nullptr;
  SetAutomaticCheckFn set_automatic_check = nullptr;
  VoidFn init = nullptr;
  VoidFn check_update_with_ui = nullptr;
  VoidFn cleanup = nullptr;
};

Sparkle g_sparkle;
bool g_initialized = false;

// WinSparkle.dll sits beside phi_client.exe (the sync and the installer both
// place it there). An absolute path keeps the search-order honest — no
// PATH/current-directory lookup for a DLL we execute.
std::wstring DllPath() {
  wchar_t exe[MAX_PATH] = {};
  const DWORD length = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::wstring();
  }
  std::wstring path(exe);
  const size_t slash = path.find_last_of(L'\\');
  if (slash == std::wstring::npos) {
    return std::wstring();
  }
  path.resize(slash + 1);
  path += L"WinSparkle.dll";
  return path;
}

bool Load() {
  if (g_sparkle.dll) {
    return true;
  }
  const std::wstring path = DllPath();
  if (path.empty()) {
    return false;
  }
  HMODULE dll = ::LoadLibraryExW(path.c_str(), nullptr,
                                 LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!dll) {
    LOG(INFO) << "[PhiUpdater] WinSparkle.dll not present; updater off";
    return false;
  }
  auto proc = [dll](const char* name) {
    return ::GetProcAddress(dll, name);
  };
  Sparkle sparkle;
  sparkle.dll = dll;
  sparkle.set_appcast_url = reinterpret_cast<SetAppcastUrlFn>(
      proc("win_sparkle_set_appcast_url"));
  sparkle.set_app_details = reinterpret_cast<SetAppDetailsFn>(
      proc("win_sparkle_set_app_details"));
  sparkle.set_eddsa_public_key = reinterpret_cast<SetEdDsaKeyFn>(
      proc("win_sparkle_set_eddsa_public_key"));
  sparkle.set_automatic_check = reinterpret_cast<SetAutomaticCheckFn>(
      proc("win_sparkle_set_automatic_check_for_updates"));
  sparkle.init = reinterpret_cast<VoidFn>(proc("win_sparkle_init"));
  sparkle.check_update_with_ui = reinterpret_cast<VoidFn>(
      proc("win_sparkle_check_update_with_ui"));
  sparkle.cleanup = reinterpret_cast<VoidFn>(proc("win_sparkle_cleanup"));
  if (!sparkle.set_appcast_url || !sparkle.set_app_details ||
      !sparkle.set_eddsa_public_key || !sparkle.init ||
      !sparkle.check_update_with_ui || !sparkle.cleanup) {
    LOG(ERROR) << "[PhiUpdater] WinSparkle.dll is missing exports; ignored";
    ::FreeLibrary(dll);
    return false;
  }
  g_sparkle = sparkle;
  return true;
}

// The PHI_APPCAST_URL / PHI_APPCAST_ED25519 overrides exist ONLY in a
// --dev-hooks build. Together they replace the update feed and the key that
// signs it, so in a release exe any process that can set an environment
// variable for us could hand us its own "update"; there the built-in values
// are the only values (docs/architecture-review/lane7-cross-cutting.md §6).
std::string AppcastUrl() {
  if constexpr (kPhiDevHooks) {
    char value[1024] = {};
    const DWORD length =
        ::GetEnvironmentVariableA("PHI_APPCAST_URL", value, sizeof(value));
    if (length > 0 && length < sizeof(value)) {
      LOG(INFO) << "[PhiUpdater] appcast override: " << value;
      return std::string(value, length);
    }
  }
  return kDefaultAppcastUrl;
}

// The built-in key, or in a --dev-hooks build the PHI_APPCAST_ED25519
// override (signature testing against a dev feed). Empty means no updater.
std::string EdDsaPublicKey() {
  if constexpr (kPhiDevHooks) {
    char value[128] = {};
    const DWORD length =
        ::GetEnvironmentVariableA("PHI_APPCAST_ED25519", value, sizeof(value));
    if (length > 0 && length < sizeof(value)) {
      LOG(INFO) << "[PhiUpdater] appcast signing key override in effect";
      return std::string(value, length);
    }
  }
  return kEdDsaPublicKey;
}

}  // namespace

// static
void PhiUpdater::Init() {
  if (g_initialized) {
    return;
  }
  // No key, no updater — in every build. A feed that cannot be verified is
  // never fetched, let alone installed; the menu item goes dark
  // (Available) rather than offering an unsigned download.
  const std::string eddsa_key = EdDsaPublicKey();
  if (eddsa_key.empty()) {
    LOG(ERROR) << "[PhiUpdater] no appcast signing key built in; updater off";
    return;
  }
  if (!Load()) {
    return;
  }
  // The key goes in BEFORE the feed is named and before init, so there is
  // no window in which WinSparkle could check keyless; a rejected key keeps
  // it uninitialised for the same reason.
  if (!g_sparkle.set_eddsa_public_key(eddsa_key.c_str())) {
    LOG(ERROR) << "[PhiUpdater] EdDSA public key rejected; updater off";
    return;
  }
  const std::string url = AppcastUrl();
  g_sparkle.set_appcast_url(url.c_str());
  g_sparkle.set_app_details(L"Fyde Innovations", L"Phi Browser",
                            PHI_WIN_VERSION_WIDE);
  if (g_sparkle.set_automatic_check) {
    g_sparkle.set_automatic_check(1);  // Default daily background checks.
  }
  g_sparkle.init();
  g_initialized = true;
  LOG(INFO) << "[PhiUpdater] init v" << PHI_WIN_VERSION_STRING
            << " appcast=" << url;
}

// static
bool PhiUpdater::CheckForUpdateWithUi() {
  if (!g_initialized) {
    Init();  // Late first use: the menu can fire before/without startup init.
  }
  if (!g_initialized) {
    return false;
  }
  g_sparkle.check_update_with_ui();
  return true;
}

// static
bool PhiUpdater::Available() {
  return !EdDsaPublicKey().empty() && (g_sparkle.dll != nullptr || Load());
}

// static
void PhiUpdater::Shutdown() {
  if (g_initialized && g_sparkle.cleanup) {
    g_sparkle.cleanup();
    g_initialized = false;
  }
}

}  // namespace phi_client
