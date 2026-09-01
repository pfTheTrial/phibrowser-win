// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_TELEMETRY_H_
#define PHIBROWSER_WIN_CLIENT_PHI_TELEMETRY_H_

#include <windows.h>

#include <cstdint>
#include <deque>
#include <string>

namespace phi_client {

// The client's outbound telemetry: product-analytics events produced anywhere
// in the browser process (phi_analytics -> the framework delegate -> here),
// spooled to disk and POSTed to a collector in batches.
//
// BACKEND-AGNOSTIC BY DESIGN. The destination is deployment configuration —
// `phi_telemetry.json` in the user data directory, the same file the crash
// reporter reads its collector out of — and an unconfigured build spools
// NOTHING and sends NOTHING. Picking a service later is an edit to that file
// plus, when its ingest shape differs, one function here (BuildBatchBody).
//
//   {
//     "analytics_endpoint": "https://collector.example/ingest",
//     "analytics_key": "<write key, sent as a bearer token>",
//     "crash_endpoint": "https://collector.example/minidump"
//   }
//
// Durability is deliberately modest, matching what the events are worth: the
// spool survives a restart, a batch that fails is retried with backoff, and a
// spool that outgrows its bound drops its OLDEST entries. Nothing here is
// worth blocking the UI thread or the user's shutdown for.
class PhiTelemetry {
 public:
  static PhiTelemetry& Get();

  PhiTelemetry(const PhiTelemetry&) = delete;
  PhiTelemetry& operator=(const PhiTelemetry&) = delete;

  // Reads the config, reloads any spool left by the previous run, and starts
  // the uploader — but only when an analytics endpoint is configured.
  // Idempotent; called once the framework hands over the user data dir.
  void Start(const std::wstring& user_data_dir);

  // Queues one event. Safe from any thread, never blocks on the network: the
  // caller is the UI thread delivering a framework callback. A no-op while
  // unconfigured, so an unconfigured build writes no behavioural data to disk.
  void RecordEvent(const std::string& module,
                   const std::string& name,
                   const std::string& properties_json);

  // Stops the uploader and flushes the spool to disk. Best-effort: whatever
  // has not uploaded stays spooled for the next run.
  void Stop();

  // What the pipeline is doing, for the harness and for support:
  // {"configured":bool,"endpoint":"...","queued":n,"sent":n,"dropped":n,
  //  "failures":n,"lastStatus":n,"lastError":"..."}
  // The endpoint is reported, the KEY never is.
  std::string StatusJson();

 private:
  PhiTelemetry();
  ~PhiTelemetry();

  struct Event {
    std::string module;
    std::string name;
    std::string properties_json;
    int64_t unix_ms = 0;
  };

  static DWORD WINAPI UploadThread(void* self);
  void RunUploadLoop();
  // Takes up to kBatchEvents off the front of the queue. Empty when idle.
  std::deque<Event> TakeBatch();
  // Puts a failed batch back at the FRONT, preserving capture order.
  void ReturnBatch(std::deque<Event> batch);
  // The request body for one batch. THE ONE PLACE a collector's ingest shape
  // lives: a JSON object with a "batch" array of {module,event,properties,ts}.
  std::string BuildBatchBody(const std::deque<Event>& batch) const;
  bool PostBatch(const std::string& body, int* http_status, std::string* err);
  void LoadSpool();
  void WriteSpool();
  // Reads the install id beside the spool, generating and persisting one on
  // first run.
  void EnsureDistinctId(const std::wstring& user_data_dir);

  // Guards every member below except the thread handle and the config, which
  // are written once by Start() before the thread exists.
  CRITICAL_SECTION lock_;
  HANDLE wake_ = nullptr;    // Signalled on a new event and on Stop().
  HANDLE thread_ = nullptr;
  bool stopping_ = false;
  bool started_ = false;

  std::string endpoint_;   // Empty == unconfigured == the whole thing is off.
  std::string api_key_;
  // "posthog" shapes the batch for PostHog's capture API; anything else (the
  // default) sends the generic envelope. This is the ONLY vendor knowledge in
  // the pipeline — see BuildBatchBody.
  std::string format_;
  // Stable per install, generated once and kept beside the spool. PostHog
  // requires a distinct_id per event; anonymous-but-stable is the honest
  // default, and nothing here ever ties it to an account.
  std::string distinct_id_;
  std::wstring spool_path_;

  std::deque<Event> queue_;
  uint64_t sent_ = 0;
  uint64_t dropped_ = 0;
  uint64_t failures_ = 0;
  int last_status_ = 0;
  std::string last_error_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_TELEMETRY_H_
