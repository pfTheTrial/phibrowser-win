// Copyright 2026 The Fyde Innovations Limited. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_WINDOW_H_
#define PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_WINDOW_H_

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "base/memory/raw_ptr_exclusion.h"
#include "phibrowser-win/client/phi_feedback_outbox.h"
#include "phibrowser-win/client/phi_popup_chrome.h"

namespace phi_client {

// What a report is about: the window it was invoked from and its focused
// tab — the Mac FeedbackViewController's host window + focusingTab (or the
// crashed tab's snapshot when opened from a crash page).
struct PhiFeedbackContext {
  int64_t window_id = 0;  // The browser whose profile supplies system logs.
  std::string url;
  std::string title;
};

// What the form needs from the coordinator.
class PhiFeedbackHost {
 public:
  virtual ~PhiFeedbackHost() = default;
  // True with `email` when an account is signed in.
  virtual bool FeedbackAccount(std::string* email) = 0;
  virtual void RequestFeedbackSignIn() = 0;
  // Opens `url` in a new foreground tab of `window_id` and raises it (the
  // Privacy Policy / Terms of Service links).
  virtual void OpenFeedbackLink(int64_t window_id, const std::string& url) = 0;
  // Starts the framework's system-logs collection; the answer arrives at
  // PhiFeedbackWindow::OnSystemLogs. False when nothing will answer.
  virtual bool FetchFeedbackSystemLogs(int64_t window_id, int request_id) = 0;
  // Phi's own extensions in `window_id`'s profile, name -> version.
  virtual std::map<std::string, std::string> PhiExtensionVersions(
      int64_t window_id) = 0;
};

// The Mac's "Send Feedback to Phi" window (FeedbackViewController +
// FeedbackView): app-global — one window, rebound to whichever browser
// window reopens it — with the description, the URL it is about, picked or
// pasted attachments, the privacy notice, and Send. Send collects Chromium's
// system logs through the framework, files the report in the outbox and
// closes; the upload happens in the background from there.
//
// Native fields on the shared popup chrome (the caption band with the
// Windows minimize / close pair), laid out as the Mac FeedbackView: the
// description well, the "Additional info" card with its divided rows, the
// legal notice with inline links, and the footer's Send. Modeless, like the
// Mac's — the user can go back to the page to check a detail while
// composing.
class PhiFeedbackWindow {
 public:
  static void Open(PhiFeedbackHost* host,
                   const PhiFeedbackContext& context,
                   HWND anchor);
  // The framework answered a FetchFeedbackSystemLogs (or the request timed
  // out — `text` null): the pending Send proceeds.
  static void OnSystemLogs(int request_id, const char* text);
  // The open window, or null.
  static HWND Handle();
  // The form's state for the harness: {"open","windowId","url","title",
  // "description","attachments":[{filename,size,image}],"submitting"}.
  static std::string StateJson();
  // Drives the form from the control socket: sets what is given
  // (description / url), adds `attachFiles` (paths), and presses Send when
  // `send` is true. Empty on success, else why not: not_open,
  // empty_description, no_account (the driven Send never raises the
  // interactive sign-in prompt — a modal box would wedge the caller).
  static std::string Drive(const std::string& description,
                    bool set_description,
                    const std::string& url,
                    bool set_url,
                    const std::vector<std::wstring>& attach_files,
                    bool send);

  ~PhiFeedbackWindow();

 private:
  struct Attachment {
    PhiFeedbackDraftAttachment draft;
    std::wstring preview_path;  // A pasted image written out for Preview.
  };

  PhiFeedbackWindow(PhiFeedbackHost* host, const PhiFeedbackContext& context);
  static LRESULT CALLBACK WndProc(HWND hwnd,
                                  UINT msg,
                                  WPARAM wparam,
                                  LPARAM lparam);
  static LRESULT CALLBACK ControlProc(HWND hwnd,
                                      UINT msg,
                                      WPARAM wparam,
                                      LPARAM lparam,
                                      UINT_PTR id,
                                      DWORD_PTR data);

  void Build();
  void Layout();
  void Paint();
  void Rebind(const PhiFeedbackContext& context);
  void RefreshAttachmentList();
  void RefreshSendState();
  void RefreshCounter();
  bool AddFiles(const std::vector<std::wstring>& paths);
  void ChooseFiles();
  void PasteImage();
  void RemoveSelected();
  void PreviewSelected();
  // False when nothing was sent: blank, already sending, or (with
  // `interactive` false) no account.
  bool Send(bool interactive);
  // The second half of Send: the logs arrived (or were given up on).
  void CompleteSend(const char* logs);
  std::string DescriptionText() const;
  std::string UrlText() const;
  int Scaled(int design) const;
  HWND Child(const wchar_t* cls,
             const wchar_t* text,
             DWORD style,
             UINT_PTR id,
             DWORD ex_style = 0);

  RAW_PTR_EXCLUSION PhiFeedbackHost* const host_;
  PhiFeedbackContext context_;
  HWND hwnd_ = nullptr;
  UINT dpi_ = 96;
  PhiPopupChrome chrome_;
  PhiPopupColors colors_;
  HBRUSH bg_brush_ = nullptr;
  HBRUSH card_brush_ = nullptr;
  HFONT font_ = nullptr;
  HFONT headline_font_ = nullptr;
  HFONT small_font_ = nullptr;
  HWND description_ = nullptr;
  HWND counter_ = nullptr;
  HWND url_ = nullptr;
  HWND choose_ = nullptr;
  HWND list_ = nullptr;
  HWND preview_ = nullptr;
  HWND remove_ = nullptr;
  HWND send_ = nullptr;
  std::vector<Attachment> attachments_;
  bool submitting_ = false;
  int pending_request_ = 0;
};

}  // namespace phi_client

#endif  // PHIBROWSER_WIN_CLIENT_PHI_FEEDBACK_WINDOW_H_
