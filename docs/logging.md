# Logging policy — PhiBrowser Windows client

Status: adopted with the Continuous track's cross-cutting phase
(`docs/architecture-review.md` §3.9, lane 7 §3). The code half is
`client/phi_log.h`; the redactors are host-tested in
`test/model/util_test.cc`.

## Why this is a policy and not a preference

Every feedback report zips `chrome_debug.log` and the Chromium system logs
and uploads them to the vendor (`phi_feedback_outbox.cc`, the `logs.zip`
attachment, size-capped at :747/:939). **Whatever the client writes to a log
leaves the machine the moment a user reports a bug.** Today the client logs
an Auth0 subject id and page URLs at INFO, so a support bundle carries an
account identity and a browsing trail nobody agreed to send.

There is no server-side scrub, and the review found no `redact`/`scrub`
anywhere in the tree. The log line is the last gate.

## The four rules

### 1. INFO is for lifecycle transitions only

A service started or stopped, a window class registered, the language
switched, the framework became ready, shutdown began. **Never** per
navigation, per tab, per paint, per poll, per keystroke.

Write it through the macro, so the CI grep has exactly one exemption:

```cpp
constexpr char kTag[] = "[PhiDownloads] ";
PHI_LIFECYCLE(kTag) << "popover attached to browser " << browser_id;
```

Everything that is *not* a lifecycle transition and is worth keeping is
`VLOG(1)`.

### 2. WARNING for recoverable, ERROR for degraded — and ERROR carries a code

`LOG(ERROR)` means the user is about to get a worse result than they asked
for, and the line says *why the OS or the server said no*: `::GetLastError()`
for a Win32 failure, the HTTP status for a request, the parse position for
JSON. Twenty of the thirty-six ERROR lines in the tree carry neither today.

A refused framework verb, a missing optional file, a retryable upload — all
WARNING. `PhiFw::Must` (below) writes these for you.

### 3. Nothing personal reaches a log unredacted

No URL, page title, email address, account id, token, filesystem path under
the user's profile, or search term is passed to `LOG` without going through a
`PhiRedact*` function from `phi_log.h`:

| Kind | Function | `"…"` becomes |
|---|---|---|
| URL | `PhiRedactUrl` | `https://mail.example.com/u/0?q=x` → `https://mail.example.com` |
| Opaque id | `PhiRedactId` | `auth0\|66f0c1d2e3b4a5968778` → `...8778` |
| Email | `PhiRedactEmail` | `ada.lovelace@example.com` → `a***@example.com` |
| Path | `PhiRedactPath` | `C:\Users\ada\…\User Data\Default\PhiPrefs.json` → `...\Default\PhiPrefs.json` |

A page **title** and a **search term** have no redacted form that is worth
anything: do not log them at all.

The four functions are pure and have no failure mode — a redactor that
throws while writing a log line is worse than the leak it prevents — so they
are compiled and tested on the host (`bash test/model/run.sh`).

Anything that must carry a real URL or path for a support engineer belongs in
the crash channel or in the feedback body the user can read before sending,
not in the log the zip picks up silently.

### 4. One tag per module, spelled `[PhiModule]`

Declared once at the top of the file:

```cpp
constexpr char kTag[] = "[PhiDownloads] ";
```

The tree currently carries nine tag spellings in three cases (`[PhiClient]`
×49, `[Feedback]`, `[telemetry]`, `[Reader]`, `[analytics]`, …). One module,
one tag, `PhiUpperCamel` after the `Phi`, a trailing space inside the string.
`[PhiClient]` stays for the process-wide lines (startup, language, catalog);
everything else takes its own.

## Errors from the framework: `PhiFw::Must`

Fifty of the framework's 108 methods return `bool` and 187 call sites ignore
the answer. A refused `ActivateTab` or `SetTabTag` leaves the client's store
and the tab strip disagreeing, with nothing in the log to say so.

```cpp
if (!PhiFw::Must(framework_->ActivateTab(browser, index),
                 "ActivateTab", browser, index)) {
  return;                        // A caller that cannot recover returns.
}
```

`Must` returns the bool it was given and, on `false`, writes one
`[PhiFramework] refused: ActivateTab <ids>` at WARNING. Success allocates
nothing. The ids are **ids** — a URL or a title goes through the redactors
first like anything else.

## CI greps

These are the checks `tools/release-gate.py` runs (§6 of lane 7); each is a
single grep so a failure names the line:

1. `LOG(INFO)` anywhere outside `phi_log.h` — must be `PHI_LIFECYCLE`.
2. A `LOG(` argument matching `url` / `sub` / `email` / `token` / `@` that is
   not inside a `PhiRedact` call.
3. A `LOG(ERROR)` whose line does not mention `GetLastError`, `status`,
   `error` or `hr`.
4. A bracketed tag that is not `[Phi<UpperCamel>]`.
5. A bare `framework_->` statement whose method is one of the fifty
   `virtual bool` ones — must be wrapped in `PhiFw::Must`.

## The sites this policy was written for

Fix these first; they are the ones that reach the vendor today.

| Site | Leak | Fix |
|---|---|---|
| `phi_auth.cc:394` | `session restored for <sub>` at INFO | `PhiRedactId(sub)`, and drop to `VLOG(1)` — a session restore is not a lifecycle transition worth INFO |
| `phi_auth.cc:580` | `signed in as <sub>` at INFO | `PhiRedactId(sub)`; the sign-in itself is lifecycle, the subject is not |
| `phi_auth.cc:472` | the loopback redirect URI at INFO | `PhiRedactUrl`, or drop the line: the port is in the same file three lines up |
| `phi_reader.cc:258-259` | `offerable granted tab=… url=…` at INFO | `PhiRedactUrl` **and** demote — this is per-navigation, which rule 1 forbids outright |
| `phi_control_server.cc:325` | the named-pipe path, which contains the PID | keep, but at `VLOG(1)`: it is a debugging aid, not a lifecycle line |
| `phi_updater.cc:118` | `appcast override: <url>` | keep unredacted. An override is evidence of tampering and the whole URL is the evidence |
| `phi_i18n.cc` catalog-missing ERROR | the full exe-relative path | `PhiRedactPath` |
| `phi_feedback_outbox.cc:777-830` | builds `logs.zip` | the zip is the reason for all of the above; once the sites above are fixed it needs no pass of its own |
