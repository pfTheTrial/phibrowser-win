// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_OUTBOX_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_OUTBOX_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"

namespace phi_client {

class PhiAuth;

// One attachment the user added to the form: a file they picked, or an image
// they pasted (already PNG-encoded).
struct PhiFeedbackDraftAttachment {
  std::wstring filename;
  int64_t size = 0;
  bool is_image = false;
  std::wstring file_path;  // A picked file, or empty for...
  std::string pasted_png;  // ...a pasted image's bytes.
};

// What the form hands over on Send — the Mac's FeedbackDraft.
struct PhiFeedbackDraft {
  std::string description;
  std::string page_url;
  std::string page_title;
  // Phi's own extensions, name -> version (the Mac's phiExtensionVersions).
  std::map<std::string, std::string> extension_versions;
  // Chromium's scrubbed system logs, when the framework produced them.
  std::optional<std::string> chromium_system_logs;
  std::vector<PhiFeedbackDraftAttachment> attachments;
};

// The Windows twin of the Mac's FeedbackOutbox + FeedbackOutboxUploader
// (UserInterface/Feedback/FeedbackOutbox.swift): a feedback report is first
// written to disk as a JOB — its description, metadata, copies of the
// attachments — and a worker then prepares the archives, uploads them
// through the account service's presigned URLs and submits the report,
// retrying with backoff across restarts, so a report survives the network
// being down when the user hit Send.
//
// Layout, under the user data dir:
//   PhiFeedbackOutbox\<account>\<jobId>\manifest.json
//                                       images\  files\  logs\  prepared\
// The manifest carries the job's state machine (queued → preparing →
// uploading → submitting → submitted | failedWaitingRetry) and every
// prepared attachment's upload state, so an interrupted job resumes where
// it stopped instead of re-uploading.
//
// Jobs are per ACCOUNT (the bearer token that submits them), as on the Mac:
// another account's jobs wait untouched until that account signs in again.
class PhiFeedbackOutbox {
 public:
  // The submit endpoint takes at most five attachments; a picked file may be
  // 10 MB, any single prepared attachment 20 MB.
  static constexpr int kMaxSubmitAttachments = 5;
  static constexpr int64_t kMaxSelectedAttachmentBytes = 10 * 1024 * 1024;
  static constexpr int64_t kMaxAttachmentBytes = 20 * 1024 * 1024;
  // The planning budget for one zip's INPUT bytes; the real output is
  // measured afterwards and split when it still exceeds the limit.
  static constexpr int64_t kZipPlanningBytes =
      kMaxAttachmentBytes - 512 * 1024;
  static constexpr int kMaxJobRetryCount = 5;
  // Bumped when the archive layout changes: older jobs are re-prepared.
  static constexpr int kArchiveStrategyVersion = 6;

  static PhiFeedbackOutbox& Get();

  PhiFeedbackOutbox(const PhiFeedbackOutbox&) = delete;
  PhiFeedbackOutbox& operator=(const PhiFeedbackOutbox&) = delete;

  // Binds the outbox root and the token source and starts the worker.
  // `auth` must outlive the process (it does: the coordinator owns it).
  void Start(const std::wstring& user_data_dir, PhiAuth* auth);
  // The signed-in account, or empty ids when signed out. Jobs are filed and
  // processed under `user_id`; a change wakes the worker (the Mac's
  // mainAccountChanged observer).
  void SetAccount(const std::string& user_id, const std::string& email);
  // Files `draft` as a job for the current account (copying the attachments
  // in) and wakes the worker. UI thread. False with `error` set when there
  // is no account, the description is blank, or a copy failed.
  bool Enqueue(const PhiFeedbackDraft& draft, std::string* error);
  // Wakes the worker to scan the outbox now.
  void Kick();
  // Stops the worker; a job mid-flight resumes on the next run.
  void Stop();

  // The outbox as the harness sees it: the account, the root, and every job
  // with its state — {"account","root","jobs":[{id,status,retryCount,
  // lastError,nextAttemptAtMs,sourceImages,sourceFiles,hasSystemLogs,
  // prepared,uploaded}]}.
  std::string StatusJson();

  // Validates a file the user picked (the Mac's selectedAttachmentInfo):
  // regular files only (no directories, no symlinks), at most 10 MB.
  static bool SelectedAttachmentInfo(const std::wstring& path,
                                     int64_t* size,
                                     std::string* mime_type,
                                     bool* is_image,
                                     std::string* error);
  // The registry's Content Type for the path's extension, with a built-in
  // fallback for the common image types; application/octet-stream when
  // unknown.
  static std::string MimeTypeForPath(const std::wstring& path);
  // "debug" for a DCHECK build, else PHI_WIN_CHANNEL.
  static std::string BrowserChannelName();
  // One of Phi's own extensions (the Mac's phiExtensionIds) — the ones
  // whose versions a report carries as components.
  static bool IsPhiExtensionId(const std::string& id);
  static bool ShouldDiscardFailedJob(int retry_count) {
    return retry_count > kMaxJobRetryCount;
  }

 private:
  PhiFeedbackOutbox();
  ~PhiFeedbackOutbox();

  static DWORD WINAPI WorkerThread(void* self);
  void RunLoop();
  // One pass over the current account's jobs. True when some job wants
  // another attempt later.
  bool ProcessOutbox();
  bool ProcessJob(const std::wstring& job_root, const std::string& user_id);
  // The account this thread should be working for, or empty.
  std::string CurrentUserId();
  std::wstring AccountRoot(const std::string& user_id) const;

  CRITICAL_SECTION lock_;
  HANDLE wake_ = nullptr;
  HANDLE thread_ = nullptr;
  bool stopping_ = false;
  bool started_ = false;
  std::wstring root_;
  std::wstring user_data_dir_;
  // Process-lifetime, owned by the coordinator.
  RAW_PTR_EXCLUSION PhiAuth* auth_ = nullptr;
  std::string user_id_;
  std::string email_;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_OUTBOX_H_
