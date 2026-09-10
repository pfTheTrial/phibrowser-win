# Send Feedback — how the Mac does it, and where Windows stood on 2026-09-10

Status: research pass after the first real sign-in on the Windows box. The
one blocking bug it found (the presign 400) is fixed and box-verified; the
rest is a parity list.

## 1. The Mac pipeline (phibrowser-mac)

Sources: `Sources/UserInterface/Feedback/{FeedbackViewController,
FeedbackView, FeedbackOutbox}.swift`, `Sources/Networking/Models/
FeedbackV2.swift`, `Sources/Networking/APIClient.swift` (feedback parts).

**Entry points** all funnel into `MainBrowserWindowController.
showFeedbackWindow(crashContextTab:)`: Chromium's `IDC_FEEDBACK` (Help ▸
Report an Issue, page context menus, the sad tab), the bridge's
`showFeedbackDialog`, the sidebar bottom-bar capsule, the floating sidebar,
the web-content header button, the renderer-crash page (passing the
crashed tab), and ⌘⌥⇧I. Guests never see the entry points; there is no
sign-in prompt in the form. One app-global 520×580 window, "Send Feedback
to Phi".

**The form**: a description (≤ 4096, counter from 3000), an editable URL
prefilled from the active or crashed tab, attachments (file picker, ⌘V
image paste; 10 MB per picked file; regular files only), the privacy notice
with Privacy Policy / Terms links, and a single Send button. Send is enabled
iff the trimmed description is non-empty. Send awaits the Chromium system
logs (no timeout), writes the job to the outbox synchronously, schedules
the uploader, and closes the window. No success toast, no analytics event.

**The outbox** lives per account at `…/users/<sub>/feedbackOutbox/<jobUUID>/`
with `manifest.json` (`id, createdAt, description, contactEmail, metadata,
sourceImages[], sourceFiles[], chromiumSystemLogs, preparedAttachments[],
archiveStrategyVersion=6, status, retryCount, nextAttemptAt, lastError`),
`images/`, `files/`, `logs/system_logs.txt`, `prepared/`. Job states
`queued → preparing → uploading → submitting → submitted` plus
`failedWaitingRetry`; the directory is deleted after submit. Per-call
retries: presign ×3, PUT ×2 (+ one fresh-presign retry), submit ×3, sleeping
2 s then 4 s. Per-job backoff `min(3600, 2^min(retry,6) × 30)` s; dropped
when `retryCount > 5`. Runs at launch, on account change, on enqueue; one
run per account; jobs sequential; attachments in presign batches of 5,
required first.

**The wire** (base `https://account[.stag].phibrowser.com`, bearer =
`getAccessTokenSyncly`):

- `POST /api/auth/feedback/v2/attachments/presign`
  `{"attachments":[{"filename","mime_type","size":<Int>,"attachment_type":
  "screenshot|log|other"}]}` → `{code, message, data:{attachments:[{filename,
  object_key, upload_url, expires_in, headers{}}]}}`, matched positionally.
- `PUT <upload_url>` with the presigned headers verbatim (Content-Type from
  the attachment when absent).
- `POST /api/auth/feedback/v2/submit`
  `{description, contact_email, metadata:{browser{name,version,channel,
  revision}, page{url,title}, client_context{category:"issue-report",
  user_agent, locale, trace_id=jobId}, components[{id,name,type,version}],
  extra{build_number, model_identifier, os_version}}, attachments:[{
  object_key, filename, mime_type, size, attachment_type}]}` →
  `data:{feedback_id, unique_report_identifier, attachment_count}`.
- Errors: no 401 handling, no 4xx/5xx distinction — every non-2xx is retried
  the same way. No `platform` field anywhere; the backend can only tell
  platforms apart by `extra.model_identifier` / `os_version`.

**Diagnostics**: `logs.zip` (required, type log) = `system_logs.txt`
(Chromium's scrubbed system logs, `FeedbackCommon::IncludeInSystemLogs`
filter) + the single newest `PhiLogs/*` file; `sentinel-logs.zip` with the
newest `boot.log` / `runner.log` / `ai-gateway.log`; images over 20 MB
re-encoded down a JPEG ladder; zips over 20 MB bisected. No
`chrome_debug.log`, no crash dumps.

## 2. Windows on 2026-09-10 (phibrowser-win)

The 2026-09-03 port (`phi_feedback_window`, `phi_feedback_outbox`,
`phi_feedback_zip`, `phi_feedback_image`, `PhiAccountAndFeedback`) mirrors
the above closely: same form fields and rules, same outbox layout under
`<user-data>\PhiFeedbackOutbox\<sanitized sub>\<jobId>\`, same states,
retries, backoff and drop rule, same three endpoints and JSON field names,
same slot arithmetic, an own zip writer, WIC for images. Differences by
design: `logs.zip` carries the tail of `chrome_debug.log` instead of
`PhiLogs`; no Sentinel logs; a 15 s cap on the system-logs wait; the
sign-in prompt on Send when signed out (interactive only — a driven Send
answers `no_account`); 401 invalidates the cached bearer so the retry
refreshes.

**What was never exercised**: an upload with a real bearer. The status log
said so since 2026-09-03; `test/suite-feedback.mjs` asserted failure by
design.

## 3. The bug: presign → HTTP 400

With the real staging session the outbox logged only `HTTP error 400.`
and discarded the job after six retries. Logging the service's body gave
`{"code":400,"message":"code=400, message=Invalid request body"}`.

The only wire difference from the Mac in that body: `size` was a
`base::Value` **double**, and `base::JSONWriter` preserves the type
(`json_writer.cc:92-98` appends `.0`), so the service received
`"size": 4614.0` where the Mac sends `4614`. Fixed by `WireSize()` in
`phi_feedback_outbox.cc` (int on the presign and submit bodies; the manifest
keeps doubles). Verified: the re-sent report was accepted in three seconds
(feedback id 19), the earlier stuck job drained on the worker's 60 s rescan
(id 20), and the suite's Send test submits for real (id 21).

## 4. Other findings, fixed in the same pass

- A reauthentication-required session (`PhiAuth::reauth_required`) counted
  as signed in for feedback, so the form filed jobs that burned their
  retries on "no account". `SyncFeedbackAccount` and `FeedbackAccount` now
  require a live session.
- `suite-feedback.mjs` now branches on `agentSpace.account.status
  signedIn`: a real session expects `submitted` and the job directory
  removed; the stand-in path keeps the old assertions.
- The outbox logs the account service's error body on any HTTP failure.
- A driven `attachFiles` with a refused file raised the modal "Could Not
  Add Attachment" notice, which wedges the control socket (the suite's
  last test hung on it); `AddFiles(paths, interactive)` now records the
  message in the form state (`attachmentError` in `feedback.status`) on
  driven calls. `suite-feedback.mjs` passes 4/4 against the real session
  (feedback id 22 filed by its Send test).

## 5. Parity gaps left (not blocking)

- Crash page: the Mac's sad-tab "Send feedback" carries the crashed tab;
  Windows has no crash surface, so Chromium's IDC_FEEDBACK arrives
  context-free (the focused window's active tab).
- The Mac hides feedback for guests; Windows prompts to sign in and does
  not resume the Send afterwards. With the account optional on Windows this
  prompt is deliberate; a resume is a small follow-up.
- `client_context.user_agent` is never sent (optional on the Mac); presign
  `expires_in` is ignored (the Mac ignores it too).
- Stand-in jobs (`agentSpace.feedback.setAccount`) live under their own
  account directory and are neither processed nor reported once a real
  session is active.
- `PhiHttpRequest` sets no WinHTTP timeouts; a stalled PUT holds the
  outbox worker until WinHTTP's defaults give up.
