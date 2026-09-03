// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_auth.h"

#include <winsock2.h>

#include <ws2tcpip.h>

#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <memory>
#include <optional>
#include <vector>

#include <string_view>

#include "base/base64.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/containers/span.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace phi_client {

namespace {

// The Auth0 application the Mac ships (Resources/Auth0.plist — the staging
// tenant is what the repo carries; release swaps the plist). Overridable per
// ring with PHI_AUTH0_DOMAIN / PHI_AUTH0_CLIENT_ID / PHI_AUTH0_AUDIENCE.
constexpr char kDefaultDomain[] = "auth.stag.phibrowser.com";
constexpr char kDefaultClientId[] = "360ZYEcp1T2AexR9rzoUkIn0VTVWc0pv";
constexpr char kDefaultAudience[] = "https://phibrowser-stag.us.auth0.com/api/v2/";
// The Mac WebAuth's scope: offline_access buys the refresh token.
constexpr char kScope[] = "openid profile email offline_access";
// How long the loopback listener waits for the user to finish the hosted
// login before the attempt is abandoned.
constexpr int kSignInTimeoutSeconds = 300;

std::string EnvOr(const char* name, const char* fallback) {
  char value[512] = {};
  const DWORD length = ::GetEnvironmentVariableA(name, value, sizeof(value));
  if (length > 0 && length < sizeof(value)) {
    return std::string(value, length);
  }
  return fallback;
}

std::string Domain() {
  return EnvOr("PHI_AUTH0_DOMAIN", kDefaultDomain);
}
std::string ClientId() {
  return EnvOr("PHI_AUTH0_CLIENT_ID", kDefaultClientId);
}
std::string Audience() {
  return EnvOr("PHI_AUTH0_AUDIENCE", kDefaultAudience);
}

// --- Small codecs ----------------------------------------------------------

// base64url without padding (RFC 7636's alphabet for PKCE material).
std::string Base64Url(base::span<const uint8_t> bytes) {
  std::string b64 = base::Base64Encode(bytes);
  std::string out;
  out.reserve(b64.size());
  for (char c : b64) {
    if (c == '+') {
      out += '-';
    } else if (c == '/') {
      out += '_';
    } else if (c != '=') {
      out += c;
    }
  }
  return out;
}

// Decodes base64url (JWT segments): map the alphabet back and re-pad.
std::optional<std::string> Base64UrlDecode(const std::string& in) {
  std::string b64;
  b64.reserve(in.size() + 3);
  for (char c : in) {
    if (c == '-') {
      b64 += '+';
    } else if (c == '_') {
      b64 += '/';
    } else {
      b64 += c;
    }
  }
  while (b64.size() % 4) {
    b64 += '=';
  }
  return base::Base64Decode(b64).transform([](std::vector<uint8_t> v) {
    return std::string(v.begin(), v.end());
  });
}

bool RandomBytes(uint8_t* out, size_t length) {
  return BCryptGenRandom(nullptr, out, static_cast<ULONG>(length),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

// SHA-256 for the PKCE challenge.
bool Sha256(base::span<const uint8_t> in, uint8_t out[32]) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) {
    return false;
  }
  BCRYPT_HASH_HANDLE hash = nullptr;
  bool ok = false;
  if (!BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0)) {
    ok = !BCryptHashData(hash, const_cast<uint8_t*>(in.data()),
                         static_cast<ULONG>(in.size()), 0) &&
         !BCryptFinishHash(hash, out, 32, 0);
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return ok;
}

std::string UrlEncode(const std::string& in) {
  static constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(in.size() * 3);
  for (unsigned char c : in) {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (safe) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0xF];
    }
  }
  return out;
}

// --- DPAPI store (the PhiCredentials.dat pattern) ---------------------------

bool ReadFileBytes(const std::wstring& path, std::string* out) {
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return false;
  }
  char buf[8192];
  DWORD read = 0;
  while (::ReadFile(f, buf, sizeof(buf), &read, nullptr) && read > 0) {
    out->append(buf, read);
  }
  ::CloseHandle(f);
  return true;
}

bool WriteFileBytes(const std::wstring& path, const std::string& bytes) {
  HANDLE f = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok = ::WriteFile(f, bytes.data(),
                              static_cast<DWORD>(bytes.size()), &written,
                              nullptr);
  ::CloseHandle(f);
  return ok && written == bytes.size();
}

std::optional<std::string> DpapiDecrypt(const std::string& blob) {
  DATA_BLOB in = {static_cast<DWORD>(blob.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
  DATA_BLOB out = {};
  if (!::CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0,
                            &out)) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  return result;
}

std::optional<std::string> DpapiEncrypt(const std::string& plain) {
  DATA_BLOB in = {static_cast<DWORD>(plain.size()),
                  reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
  DATA_BLOB out = {};
  if (!::CryptProtectData(&in, L"PhiAuth", nullptr, nullptr, nullptr, 0,
                          &out)) {
    return std::nullopt;
  }
  std::string result(reinterpret_cast<char*>(out.pbData), out.cbData);
  ::LocalFree(out.pbData);
  return result;
}

// --- WinHTTP ---------------------------------------------------------------

// POSTs `body` (form-encoded) to https://<domain><path>; the response body
// lands in `response`. False on transport failure (HTTP error bodies still
// return true — the caller reads the JSON's `error`).
bool HttpsPostForm(const std::string& domain,
                   const std::wstring& path,
                   const std::string& body,
                   std::string* response) {
  HINTERNET session = ::WinHttpOpen(L"PhiBrowser-Win",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    return false;
  }
  bool ok = false;
  HINTERNET connect = ::WinHttpConnect(
      session, base::UTF8ToWide(domain).c_str(),
      INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (connect) {
    HINTERNET request = ::WinHttpOpenRequest(
        connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request) {
      static const wchar_t kHeaders[] =
          L"Content-Type: application/x-www-form-urlencoded";
      if (::WinHttpSendRequest(request, kHeaders,
                               static_cast<DWORD>(-1),
                               const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0) &&
          ::WinHttpReceiveResponse(request, nullptr)) {
        DWORD available = 0;
        while (::WinHttpQueryDataAvailable(request, &available) &&
               available > 0) {
          std::vector<char> chunk(available);
          DWORD read = 0;
          if (!::WinHttpReadData(request, chunk.data(), available, &read) ||
              read == 0) {
            break;
          }
          response->append(chunk.data(), read);
        }
        ok = true;
      }
      ::WinHttpCloseHandle(request);
    }
    ::WinHttpCloseHandle(connect);
  }
  ::WinHttpCloseHandle(session);
  return ok;
}

// Parses the id_token's claim segment (payload only — the token arrived over
// the code exchange's TLS channel directly from the issuer, the same trust
// the Mac's SDK places in it).
std::optional<PhiAuthUser> UserFromIdToken(const std::string& id_token) {
  const size_t first = id_token.find('.');
  const size_t second =
      first == std::string::npos ? std::string::npos
                                 : id_token.find('.', first + 1);
  if (second == std::string::npos) {
    return std::nullopt;
  }
  const std::optional<std::string> payload =
      Base64UrlDecode(id_token.substr(first + 1, second - first - 1));
  if (!payload) {
    return std::nullopt;
  }
  std::optional<base::DictValue> claims =
      base::JSONReader::ReadDict(*payload, base::JSON_PARSE_RFC);
  if (!claims) {
    return std::nullopt;
  }
  PhiAuthUser user;
  if (const std::string* sub = claims->FindString("sub")) {
    user.sub = *sub;
  }
  if (user.sub.empty()) {
    return std::nullopt;  // The Mac refuses accounts without a subject too.
  }
  if (const std::string* name = claims->FindString("name")) {
    user.name = *name;
  }
  if (const std::string* email = claims->FindString("email")) {
    user.email = *email;
  }
  if (const std::string* picture = claims->FindString("picture")) {
    user.picture = *picture;
  }
  return user;
}

}  // namespace

// The sign-in attempt's cross-thread bag: everything the listener thread
// needs, owned by it after start.
struct PhiAuth::SignInJob {
  // The owning PhiAuth outlives the listener thread's job by design.
  RAW_PTR_EXCLUSION PhiAuth* auth = nullptr;
  SOCKET listener = INVALID_SOCKET;
  std::string verifier;
  std::string state;
  std::string redirect_uri;
};

PhiAuth::PhiAuth() {
  ::InitializeCriticalSection(&refresh_lock_);
}

PhiAuth::~PhiAuth() {
  ::DeleteCriticalSection(&refresh_lock_);
}

std::string PhiAuth::AccessTokenBlocking() {
  if (!signed_in_) {
    return std::string();
  }
  ::EnterCriticalSection(&refresh_lock_);
  if (access_token_.empty() && !RefreshSession()) {
    access_token_.clear();
  }
  std::string token = access_token_;
  ::LeaveCriticalSection(&refresh_lock_);
  return token;
}

void PhiAuth::InvalidateAccessToken() {
  ::EnterCriticalSection(&refresh_lock_);
  access_token_.clear();
  ::LeaveCriticalSection(&refresh_lock_);
}

// static
std::string PhiAuth::AccountBaseUrl() {
  const std::string override = EnvOr("PHI_ACCOUNT_BASE_URL", "");
  if (!override.empty()) {
    return override;
  }
  return Domain().find(".stag.") != std::string::npos
             ? "https://account.stag.phibrowser.com"
             : "https://account.phibrowser.com";
}

void PhiAuth::Load(const std::wstring& user_data_dir,
                   HWND notify_hwnd,
                   UINT notify_msg) {
  store_path_ = user_data_dir + L"\\PhiAuth.dat";
  notify_hwnd_ = notify_hwnd;
  notify_msg_ = notify_msg;

  std::string blob;
  if (!ReadFileBytes(store_path_, &blob) || blob.empty()) {
    return;
  }
  const std::optional<std::string> plain = DpapiDecrypt(blob);
  if (!plain) {
    LOG(ERROR) << "[PhiAuth] stored session unreadable; ignoring";
    return;
  }
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(*plain, base::JSON_PARSE_RFC);
  if (!root) {
    return;
  }
  if (const std::string* token = root->FindString("refreshToken")) {
    refresh_token_ = *token;
  }
  if (const base::DictValue* user = root->FindDict("user")) {
    const auto read = [&](const char* key) {
      const std::string* value = user->FindString(key);
      return value ? *value : std::string();
    };
    user_.sub = read("sub");
    user_.name = read("name");
    user_.email = read("email");
    user_.picture = read("picture");
  }
  if (refresh_token_.empty() || user_.sub.empty()) {
    refresh_token_.clear();
    return;
  }
  signed_in_ = true;
  LOG(INFO) << "[PhiAuth] session restored for " << user_.sub;
  // Validate in the background (the Mac's refreshAuthStatus at launch): a
  // revoked session signs out and notifies.
  ::CreateThread(nullptr, 0, &PhiAuth::RefreshThread, this, 0, nullptr);
}

void PhiAuth::BeginSignIn(std::function<void(const std::string&)> open_url) {
  if (signing_in_) {
    return;
  }

  WSADATA wsa = {};
  if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return;
  }

  // PKCE material: a 32-byte verifier and its S256 challenge.
  uint8_t random[32];
  if (!RandomBytes(random, sizeof(random))) {
    return;
  }
  const std::string verifier = Base64Url(random);
  uint8_t digest[32];
  if (!Sha256(base::as_byte_span(verifier), digest)) {
    return;
  }
  const std::string challenge = Base64Url(digest);
  uint8_t state_bytes[16];
  if (!RandomBytes(state_bytes, sizeof(state_bytes))) {
    return;
  }
  const std::string state = Base64Url(state_bytes);

  // The one-shot loopback listener on an ephemeral port.
  SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    return;
  }
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ||
      ::listen(listener, 1)) {
    ::closesocket(listener);
    return;
  }
  int addr_len = sizeof(addr);
  ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len);
  const int port = ntohs(addr.sin_port);

  auto* job = new SignInJob();
  job->auth = this;
  job->listener = listener;
  job->verifier = verifier;
  job->state = state;
  job->redirect_uri =
      "http://127.0.0.1:" + std::to_string(port) + "/callback";

  const std::string url =
      "https://" + Domain() + "/authorize?response_type=code&client_id=" +
      UrlEncode(ClientId()) + "&redirect_uri=" + UrlEncode(job->redirect_uri) +
      "&scope=" + UrlEncode(kScope) + "&audience=" + UrlEncode(Audience()) +
      "&code_challenge=" + challenge + "&code_challenge_method=S256" +
      "&state=" + state;

  signing_in_ = true;
  job_ = job;
  HANDLE thread =
      ::CreateThread(nullptr, 0, &PhiAuth::ListenerThread, job, 0, nullptr);
  if (!thread) {
    signing_in_ = false;
    job_ = nullptr;
    ::closesocket(listener);
    delete job;
    return;
  }
  ::CloseHandle(thread);
  LOG(INFO) << "[PhiAuth] sign-in started, callback on " << job->redirect_uri;
  open_url(url);
}

// static
DWORD WINAPI PhiAuth::ListenerThread(void* param) {
  std::unique_ptr<SignInJob> job(static_cast<SignInJob*>(param));
  PhiAuth* auth = job->auth;

  // One connection, or give up after the window a human needs.
  fd_set read_set;
  FD_ZERO(&read_set);
  UNSAFE_BUFFERS(FD_SET(job->listener, &read_set));
  timeval timeout = {kSignInTimeoutSeconds, 0};
  std::string code;
  if (::select(0, &read_set, nullptr, nullptr, &timeout) == 1) {
    SOCKET client = ::accept(job->listener, nullptr, nullptr);
    if (client != INVALID_SOCKET) {
      char buffer[4096] = {};
      const int received = ::recv(client, buffer, sizeof(buffer) - 1, 0);
      if (received > 0) {
        // "GET /callback?code=...&state=... HTTP/1.1"
        const std::string request(buffer, received);
        const size_t line_end = request.find("\r\n");
        const std::string line = request.substr(0, line_end);
        const auto param_of = [&line](const std::string& key) {
          const std::string needle = key + "=";
          size_t begin = line.find("?" + needle);
          if (begin == std::string::npos) {
            begin = line.find("&" + needle);
          }
          if (begin == std::string::npos) {
            return std::string();
          }
          begin += needle.size() + 1;
          const size_t end = line.find_first_of("& ", begin);
          return line.substr(begin, end - begin);
        };
        if (param_of("state") == job->state) {
          code = param_of("code");
        }
        const char* body =
            code.empty()
                ? "<html><body style='font-family:sans-serif'>"
                  "<h3>Sign-in was not completed.</h3>"
                  "You can close this tab and try again from Phi."
                  "</body></html>"
                : "<html><body style='font-family:sans-serif'>"
                  "<h3>You're signed in.</h3>"
                  "You can close this tab and return to Phi."
                  "</body></html>";
        std::string reply =
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
            "Connection: close\r\nContent-Length: " +
            std::to_string(strlen(body)) + "\r\n\r\n" + body;
        ::send(client, reply.data(), static_cast<int>(reply.size()), 0);
      }
      ::closesocket(client);
    }
  }
  ::closesocket(job->listener);

  if (!code.empty()) {
    auth->CompleteSignIn(code, job->verifier, job->redirect_uri);
  } else {
    LOG(INFO) << "[PhiAuth] sign-in abandoned (timeout or bad callback)";
  }
  auth->job_ = nullptr;
  auth->signing_in_ = false;
  auth->NotifyChanged();
  return 0;
}

void PhiAuth::CompleteSignIn(const std::string& code,
                             const std::string& verifier,
                             const std::string& redirect_uri) {
  const std::string body =
      "grant_type=authorization_code&client_id=" + UrlEncode(ClientId()) +
      "&code_verifier=" + verifier + "&code=" + UrlEncode(code) +
      "&redirect_uri=" + UrlEncode(redirect_uri);
  std::string response;
  if (!HttpsPostForm(Domain(), L"/oauth/token", body, &response)) {
    LOG(ERROR) << "[PhiAuth] token exchange transport failure";
    return;
  }
  std::optional<base::DictValue> json =
      base::JSONReader::ReadDict(response, base::JSON_PARSE_RFC);
  if (!json) {
    return;
  }
  if (const std::string* error = json->FindString("error")) {
    LOG(ERROR) << "[PhiAuth] token exchange refused: " << *error;
    return;
  }
  const std::string* id_token = json->FindString("id_token");
  const std::string* refresh = json->FindString("refresh_token");
  const std::string* access = json->FindString("access_token");
  std::optional<PhiAuthUser> user =
      id_token ? UserFromIdToken(*id_token) : std::nullopt;
  if (!user || !refresh) {
    LOG(ERROR) << "[PhiAuth] token exchange incomplete";
    return;
  }
  user_ = *user;
  refresh_token_ = *refresh;
  access_token_ = access ? *access : std::string();
  signed_in_ = true;
  Persist();
  LOG(INFO) << "[PhiAuth] signed in as " << user_.sub;
}

// static
DWORD WINAPI PhiAuth::RefreshThread(void* param) {
  auto* auth = static_cast<PhiAuth*>(param);
  ::EnterCriticalSection(&auth->refresh_lock_);
  const bool alive = auth->RefreshSession();
  ::LeaveCriticalSection(&auth->refresh_lock_);
  if (!alive) {
    LOG(INFO) << "[PhiAuth] stored session no longer valid; signing out";
    auth->SignOut();
  }
  auth->NotifyChanged();
  return 0;
}

bool PhiAuth::RefreshSession() {
  if (refresh_token_.empty()) {
    return false;
  }
  const std::string body =
      "grant_type=refresh_token&client_id=" + UrlEncode(ClientId()) +
      "&refresh_token=" + UrlEncode(refresh_token_);
  std::string response;
  if (!HttpsPostForm(Domain(), L"/oauth/token", body, &response)) {
    // Transport failure (offline) keeps the cached session — the Mac's
    // offline launch behaves the same.
    return true;
  }
  std::optional<base::DictValue> json =
      base::JSONReader::ReadDict(response, base::JSON_PARSE_RFC);
  if (!json) {
    return true;
  }
  if (json->FindString("error")) {
    return false;  // invalid_grant etc: the session is dead.
  }
  if (const std::string* access = json->FindString("access_token")) {
    access_token_ = *access;
  }
  if (const std::string* rotated = json->FindString("refresh_token")) {
    if (!rotated->empty() && *rotated != refresh_token_) {
      refresh_token_ = *rotated;  // Rotation-enabled tenants.
      Persist();
    }
  }
  if (const std::string* id_token = json->FindString("id_token")) {
    if (std::optional<PhiAuthUser> user = UserFromIdToken(*id_token)) {
      user_ = *user;
      Persist();
    }
  }
  return true;
}

void PhiAuth::SignOut() {
  signed_in_ = false;
  user_ = PhiAuthUser();
  refresh_token_.clear();
  access_token_.clear();
  if (!store_path_.empty()) {
    ::DeleteFileW(store_path_.c_str());
  }
}

void PhiAuth::Persist() {
  base::DictValue root;
  root.Set("refreshToken", refresh_token_);
  base::DictValue user;
  user.Set("sub", user_.sub);
  user.Set("name", user_.name);
  user.Set("email", user_.email);
  user.Set("picture", user_.picture);
  root.Set("user", std::move(user));
  std::string json;
  base::JSONWriter::Write(root, &json);
  const std::optional<std::string> blob = DpapiEncrypt(json);
  if (!blob || !WriteFileBytes(store_path_, *blob)) {
    LOG(ERROR) << "[PhiAuth] failed to persist the session";
  }
}

void PhiAuth::NotifyChanged() {
  if (notify_hwnd_ && ::IsWindow(notify_hwnd_)) {
    ::PostMessageW(notify_hwnd_, notify_msg_, 0, 0);
  }
}

}  // namespace phi_client
