// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "phibrowser-win/client/phi_telemetry.h"

#include <winhttp.h>

#include <optional>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"

#pragma comment(lib, "winhttp.lib")

namespace phi_client {
namespace {

// One batch never grows past this; the rest waits for the next round.
constexpr size_t kBatchEvents = 64;
// The spool's bound. Older events are dropped first — recent behaviour is
// what has diagnostic value, and an unbounded spool on a machine that never
// reaches its collector is a slow disk leak.
constexpr size_t kMaxQueuedEvents = 2000;
// Idle wake-up. A batch that has been sitting goes out even with no new
// events behind it.
constexpr DWORD kIdleFlushMs = 30 * 1000;
// Backoff after a failed batch: doubles to the cap, resets on success.
constexpr DWORD kBackoffStartMs = 5 * 1000;
constexpr DWORD kBackoffMaxMs = 10 * 60 * 1000;

int64_t NowUnixMs() {
  FILETIME ft = {};
  ::GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER v = {};
  v.LowPart = ft.dwLowDateTime;
  v.HighPart = ft.dwHighDateTime;
  // FILETIME is 100ns ticks since 1601; 11644473600s to the Unix epoch.
  return static_cast<int64_t>(v.QuadPart / 10000ULL) - 11644473600000LL;
}

// Splits an https URL into host / path. Returns false for anything that is
// not https — telemetry does not travel in the clear, and a misconfigured
// endpoint should switch the pipeline off rather than downgrade it.
bool SplitHttpsUrl(const std::string& url,
                   std::wstring* host,
                   std::wstring* path) {
  constexpr char kScheme[] = "https://";
  if (url.compare(0, sizeof(kScheme) - 1, kScheme) != 0) {
    return false;
  }
  const std::string rest = url.substr(sizeof(kScheme) - 1);
  if (rest.empty()) {
    return false;
  }
  const size_t slash = rest.find('/');
  const std::string host_part =
      slash == std::string::npos ? rest : rest.substr(0, slash);
  const std::string path_part =
      slash == std::string::npos ? std::string("/") : rest.substr(slash);
  if (host_part.empty() || host_part.find(':') != std::string::npos) {
    return false;  // No explicit ports: the default https port, or nothing.
  }
  *host = base::UTF8ToWide(host_part);
  *path = base::UTF8ToWide(path_part);
  return true;
}

std::wstring ConfigPath(const std::wstring& user_data_dir) {
  return user_data_dir + L"\\phi_telemetry.json";
}

std::string ReadFileUtf8(const std::wstring& path) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return std::string();
  }
  std::string out;
  char chunk[8192];
  DWORD read = 0;
  while (::ReadFile(file, chunk, sizeof(chunk), &read, nullptr) && read > 0) {
    out.append(chunk, read);
    if (out.size() > 8u * 1024 * 1024) {
      break;  // A spool this large is corrupt; take what we have.
    }
  }
  ::CloseHandle(file);
  return out;
}

// Atomic replace, the same shape phi_tab_store uses: write a temp beside the
// target, then MoveFileEx over it, so a crash mid-write cannot truncate the
// spool it was rewriting.
bool WriteFileAtomic(const std::wstring& path, const std::string& contents) {
  const std::wstring temp = path + L".tmp";
  HANDLE file = ::CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok = ::WriteFile(file, contents.data(),
                              static_cast<DWORD>(contents.size()), &written,
                              nullptr);
  ::CloseHandle(file);
  if (!ok || written != contents.size()) {
    ::DeleteFileW(temp.c_str());
    return false;
  }
  return ::MoveFileExW(temp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
         0;
}

}  // namespace

PhiTelemetry& PhiTelemetry::Get() {
  // Process-lifetime, deliberately never destroyed: the upload thread may be
  // mid-request while the process tears down, and a destructor racing that is
  // worse than leaking one object.
  static PhiTelemetry* instance = new PhiTelemetry();
  return *instance;
}

PhiTelemetry::PhiTelemetry() {
  ::InitializeCriticalSection(&lock_);
  wake_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

PhiTelemetry::~PhiTelemetry() = default;

void PhiTelemetry::Start(const std::wstring& user_data_dir) {
  ::EnterCriticalSection(&lock_);
  const bool already = started_;
  started_ = true;
  ::LeaveCriticalSection(&lock_);
  if (already || user_data_dir.empty()) {
    return;
  }

  const std::string config = ReadFileUtf8(ConfigPath(user_data_dir));
  std::string endpoint;
  std::string key;
  if (!config.empty()) {
    if (std::optional<base::DictValue> parsed =
            base::JSONReader::ReadDict(config, base::JSON_PARSE_RFC)) {
      if (const std::string* value = parsed->FindString("analytics_endpoint")) {
        endpoint = *value;
      }
      if (const std::string* value = parsed->FindString("analytics_key")) {
        key = *value;
      }
    } else {
      LOG(ERROR) << "[telemetry] phi_telemetry.json is not valid JSON; "
                 << "the pipeline stays off.";
    }
  }
  std::wstring host;
  std::wstring path;
  if (!endpoint.empty() && !SplitHttpsUrl(endpoint, &host, &path)) {
    LOG(ERROR) << "[telemetry] analytics_endpoint must be an https URL with "
               << "no explicit port; the pipeline stays off.";
    endpoint.clear();
  }
  if (endpoint.empty()) {
    // Unconfigured is the norm until a collector is chosen. Say so once, at
    // a level that does not shout in a normal session.
    VLOG(1) << "[telemetry] no analytics endpoint configured; events are "
            << "dropped after the framework hands them over.";
    return;
  }

  ::EnterCriticalSection(&lock_);
  endpoint_ = endpoint;
  api_key_ = key;
  spool_path_ = user_data_dir + L"\\phi_telemetry_spool.json";
  ::LeaveCriticalSection(&lock_);

  LoadSpool();
  thread_ = ::CreateThread(nullptr, 0, &PhiTelemetry::UploadThread, this, 0,
                           nullptr);
  LOG(INFO) << "[telemetry] uploader started for " << endpoint;
}

void PhiTelemetry::RecordEvent(const std::string& module,
                               const std::string& name,
                               const std::string& properties_json) {
  ::EnterCriticalSection(&lock_);
  const bool configured = !endpoint_.empty();
  if (configured) {
    Event event;
    event.module = module;
    event.name = name;
    event.properties_json =
        properties_json.empty() ? std::string("{}") : properties_json;
    event.unix_ms = NowUnixMs();
    queue_.push_back(std::move(event));
    while (queue_.size() > kMaxQueuedEvents) {
      queue_.pop_front();
      ++dropped_;
    }
  }
  ::LeaveCriticalSection(&lock_);
  if (configured && wake_) {
    ::SetEvent(wake_);
  }
}

void PhiTelemetry::Stop() {
  HANDLE thread = nullptr;
  ::EnterCriticalSection(&lock_);
  stopping_ = true;
  thread = thread_;
  thread_ = nullptr;
  ::LeaveCriticalSection(&lock_);
  if (wake_) {
    ::SetEvent(wake_);
  }
  if (thread) {
    // Bounded: a request in flight must not hold up the user's shutdown.
    ::WaitForSingleObject(thread, 2000);
    ::CloseHandle(thread);
  }
  WriteSpool();
}

std::string PhiTelemetry::StatusJson() {
  base::DictValue out;
  ::EnterCriticalSection(&lock_);
  out.Set("configured", !endpoint_.empty());
  out.Set("endpoint", endpoint_);  // The key is deliberately not reported.
  out.Set("queued", static_cast<int>(queue_.size()));
  out.Set("sent", static_cast<double>(sent_));
  out.Set("dropped", static_cast<double>(dropped_));
  out.Set("failures", static_cast<double>(failures_));
  out.Set("lastStatus", last_status_);
  out.Set("lastError", last_error_);
  ::LeaveCriticalSection(&lock_);
  std::optional<std::string> json = base::WriteJson(out);
  return json ? *json : std::string("{}");
}

DWORD WINAPI PhiTelemetry::UploadThread(void* self) {
  static_cast<PhiTelemetry*>(self)->RunUploadLoop();
  return 0;
}

void PhiTelemetry::RunUploadLoop() {
  DWORD backoff = 0;
  for (;;) {
    ::WaitForSingleObject(wake_, backoff ? backoff : kIdleFlushMs);
    ::EnterCriticalSection(&lock_);
    const bool stopping = stopping_;
    ::LeaveCriticalSection(&lock_);
    if (stopping) {
      return;
    }
    std::deque<Event> batch = TakeBatch();
    if (batch.empty()) {
      backoff = 0;
      continue;
    }
    const std::string body = BuildBatchBody(batch);
    int status = 0;
    std::string error;
    const bool ok = PostBatch(body, &status, &error);
    // 4xx is the collector rejecting THIS payload; retrying it forever would
    // wedge the queue behind a poison batch, so it is dropped and counted.
    // Transport failures and 5xx are retried.
    const bool permanent = !ok && status >= 400 && status < 500;
    ::EnterCriticalSection(&lock_);
    last_status_ = status;
    last_error_ = error;
    if (ok) {
      sent_ += batch.size();
    } else {
      ++failures_;
      if (permanent) {
        dropped_ += batch.size();
      }
    }
    ::LeaveCriticalSection(&lock_);
    if (ok) {
      backoff = 0;
      WriteSpool();
      continue;
    }
    if (permanent) {
      LOG(ERROR) << "[telemetry] collector rejected a batch (" << status
                 << "); dropping " << batch.size() << " events.";
      backoff = 0;
      WriteSpool();
      continue;
    }
    ReturnBatch(std::move(batch));
    WriteSpool();
    backoff = backoff ? (backoff * 2 > kBackoffMaxMs ? kBackoffMaxMs
                                                     : backoff * 2)
                      : kBackoffStartMs;
  }
}

std::deque<PhiTelemetry::Event> PhiTelemetry::TakeBatch() {
  std::deque<Event> batch;
  ::EnterCriticalSection(&lock_);
  while (!queue_.empty() && batch.size() < kBatchEvents) {
    batch.push_back(std::move(queue_.front()));
    queue_.pop_front();
  }
  ::LeaveCriticalSection(&lock_);
  return batch;
}

void PhiTelemetry::ReturnBatch(std::deque<Event> batch) {
  ::EnterCriticalSection(&lock_);
  while (!batch.empty()) {
    queue_.push_front(std::move(batch.back()));
    batch.pop_back();
  }
  while (queue_.size() > kMaxQueuedEvents) {
    queue_.pop_front();
    ++dropped_;
  }
  ::LeaveCriticalSection(&lock_);
}

std::string PhiTelemetry::BuildBatchBody(const std::deque<Event>& batch) const {
  base::ListValue rows;
  for (const Event& event : batch) {
    base::DictValue row;
    row.Set("module", event.module);
    row.Set("event", event.name);
    row.Set("ts", static_cast<double>(event.unix_ms));
    if (std::optional<base::Value> props = base::JSONReader::Read(
            event.properties_json, base::JSON_PARSE_RFC)) {
      row.Set("properties", std::move(*props));
    }
    rows.Append(std::move(row));
  }
  base::DictValue body;
  body.Set("batch", std::move(rows));
  std::optional<std::string> json = base::WriteJson(body);
  return json ? *json : std::string("{\"batch\":[]}");
}

bool PhiTelemetry::PostBatch(const std::string& body,
                             int* http_status,
                             std::string* err) {
  std::string endpoint;
  std::string key;
  ::EnterCriticalSection(&lock_);
  endpoint = endpoint_;
  key = api_key_;
  ::LeaveCriticalSection(&lock_);

  std::wstring host;
  std::wstring path;
  if (!SplitHttpsUrl(endpoint, &host, &path)) {
    *err = "bad endpoint";
    return false;
  }
  HINTERNET session = ::WinHttpOpen(L"PhiBrowser-Win",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    *err = "WinHttpOpen failed";
    return false;
  }
  bool ok = false;
  HINTERNET connect = ::WinHttpConnect(session, host.c_str(),
                                       INTERNET_DEFAULT_HTTPS_PORT, 0);
  if (connect) {
    HINTERNET request = ::WinHttpOpenRequest(
        connect, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request) {
      std::wstring headers = L"Content-Type: application/json";
      if (!key.empty()) {
        headers += L"\r\nAuthorization: Bearer " + base::UTF8ToWide(key);
      }
      if (::WinHttpSendRequest(request, headers.c_str(),
                               static_cast<DWORD>(-1),
                               const_cast<char*>(body.data()),
                               static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0) &&
          ::WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0;
        DWORD size = sizeof(status);
        ::WinHttpQueryHeaders(
            request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        *http_status = static_cast<int>(status);
        ok = status >= 200 && status < 300;
        if (!ok) {
          *err = "http " + std::to_string(status);
        }
      } else {
        *err = "send failed (" + std::to_string(::GetLastError()) + ")";
      }
      ::WinHttpCloseHandle(request);
    } else {
      *err = "WinHttpOpenRequest failed";
    }
    ::WinHttpCloseHandle(connect);
  } else {
    *err = "WinHttpConnect failed";
  }
  ::WinHttpCloseHandle(session);
  if (ok) {
    err->clear();
  }
  return ok;
}

void PhiTelemetry::LoadSpool() {
  std::wstring path;
  ::EnterCriticalSection(&lock_);
  path = spool_path_;
  ::LeaveCriticalSection(&lock_);
  if (path.empty()) {
    return;
  }
  const std::string contents = ReadFileUtf8(path);
  if (contents.empty()) {
    return;
  }
  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  const base::ListValue* rows = parsed ? parsed->FindList("batch") : nullptr;
  if (!rows) {
    return;
  }
  ::EnterCriticalSection(&lock_);
  for (const base::Value& value : *rows) {
    const base::DictValue* row = value.GetIfDict();
    if (!row) {
      continue;
    }
    Event event;
    if (const std::string* module = row->FindString("module")) {
      event.module = *module;
    }
    if (const std::string* name = row->FindString("event")) {
      event.name = *name;
    }
    event.unix_ms = static_cast<int64_t>(row->FindDouble("ts").value_or(0));
    if (const base::Value* props = row->Find("properties")) {
      std::optional<std::string> json = base::WriteJson(*props);
      event.properties_json = json ? *json : std::string("{}");
    } else {
      event.properties_json = "{}";
    }
    if (event.name.empty()) {
      continue;
    }
    queue_.push_back(std::move(event));
  }
  while (queue_.size() > kMaxQueuedEvents) {
    queue_.pop_front();
    ++dropped_;
  }
  const size_t restored = queue_.size();
  ::LeaveCriticalSection(&lock_);
  if (restored) {
    LOG(INFO) << "[telemetry] reloaded " << restored
              << " events left by the previous run.";
  }
}

void PhiTelemetry::WriteSpool() {
  std::wstring path;
  std::deque<Event> snapshot;
  ::EnterCriticalSection(&lock_);
  path = spool_path_;
  snapshot = queue_;
  ::LeaveCriticalSection(&lock_);
  if (path.empty()) {
    return;
  }
  if (snapshot.empty()) {
    ::DeleteFileW(path.c_str());
    return;
  }
  WriteFileAtomic(path, BuildBatchBody(snapshot));
}

}  // namespace phi_client
