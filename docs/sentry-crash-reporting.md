# Sentry crash reporting for phibrowser-win

Set up 2026-09-11. The Windows client sends Crashpad minidumps to Sentry
Cloud; nothing Sentry-specific is compiled into the client.

## How it works

- Crash capture is Chromium's Crashpad, launched by `phi_client_main_win.cc`.
- The upload URL comes from `crash_endpoint` in `phi_telemetry.json` in the
  user data dir, read once at startup by
  `ChromeCrashReporterClient::GetUploadUrl` (fork,
  `chrome/app/chrome_crash_reporter_client_win.cc`). Empty = dumps stay local.
- Sentry accepts Crashpad's multipart POST directly on its Minidump ingest:
  `https://<ingest host>/api/<project id>/minidump/?sentry_key=<public key>`.
  Crashpad annotations (`ptype`, `ver`, switches, GPU, origin) arrive as
  event context.

## Sentry side

- Org `phinomenon`, project `phibrowser-win`, platform **Minidump**. The
  sibling Mac project is `phibrowser-mac`.
- Client Keys page: nothing to configure. OTLP / security header tabs are
  for SDK-based apps.
- Symbols are matched by debug ID, which every build re-stamps, so each
  deployed build needs its own upload.

## Tooling (outside this repo, in /home/jx/Phi)

| file | role |
|---|---|
| `phi-telemetry.env` | not in git. Sentry block: ingest host, project id, public key, `PHI_SENTRY_API_URL` for sentry-cli, `PHI_SENTRY_AUTH_TOKEN` (org token, scope `org:ci`, SECRET; file is chmod 600). LAN self-host lines kept commented. |
| `push-telemetry-config.sh` | renders `phi_telemetry.json` and scp's it to `C:/phi-fx-test` on the box. Restart Phi afterwards (`PhiQuit`, then `PhiFxLaunch`). |
| `upload-symbols.sh` | `sentry-cli debug-files upload` of PE + PDB from `out/PhiFxWin`. Default is CLIENT ONLY (`Phi.exe`). Name modules to add framework DLLs (`upload-symbols.sh chrome.dll`); `--all` uploads every DLL with a PDB (~8.5 GB for the component build); `--dry-run` sends nothing. Empty token or missing sentry-cli = one line and exit 0. |
| `sync-fx-win.sh` | after a push that included `Phi.exe`, calls `upload-symbols.sh Phi.exe`. A sync that pushed nothing uploads nothing. |

sentry-cli is a global npm install (`npm i -g @sentry/cli`).

## Verified on the test box (2026-09-11)

- Crashpad handler runs with `--url=<Sentry minidump endpoint>` (visible in
  the process command line); upload consent on.
- `chrome://crash` (renderer) via `agentSpace.navigate` produced a dump in
  `C:\phi-fx-test\Crashpad\reports` and reached Sentry as PHIBROWSER-WIN-1;
  later crashes grouped as PHIBROWSER-WIN-2.
- Re-pushing `Phi.exe` triggered the symbol step; sentry-cli deduped the
  already-present files.

## Gotchas

- **Crashpad throttles uploads to one per hour on Windows** and marks the
  rest skipped for good (`crash_report_upload_thread.cc`,
  `kUploadAttemptIntervalSeconds`; Chromium passes `--no-rate-limit` only
  on Linux). To retest within the hour: stop Phi, zero the int64 at byte 16
  of `Crashpad\settings.dat`, restart. Or POST a dump by hand:
  `curl -F upload_file_minidump=@x.dmp "$PHI_CRASH_ENDPOINT"`.
- **Windows Crashpad DB layout** is `reports/*.dmp` + `metadata` +
  `settings.dat` (byte 8 = upload_enabled). No pending/completed dirs.
- **Component build.** `out/PhiFxWin` has 537 DLLs with PDBs (1.3 GB PE +
  7.2 GB PDB). A renderer crash sits in `blink_common.dll` and every stack
  walks `base.dll` / `content.dll`, so with only Phi.exe (+ chrome.dll)
  uploaded Sentry names those modules' frames and stack-scans the rest. A
  non-component release build collapses this to Phi.exe + chrome.dll.
- `chrome://inducebrowsercrashforrealz` is ignored from `agentSpace.navigate`:
  content only honours browser debug URLs on explicit (typed) navigations.
- **Renderer crashes show almost no client frames.** The unnamed frames
  on the test events are in `content.dll` / `base.dll` / `blink_*.dll`
  (visible through `stack.package`), which are not uploaded on purpose;
  Sentry names only what it has symbols for (Microsoft DLLs via its symbol
  server, Phi.exe, chrome.dll). A renderer's stack reaches Phi.exe only at
  the very bottom, and unwinding breaks at the first DLL without unwind
  info, so those two frames are never reached. Verified 2026-09-11 with a
  fresh debug ID (no cache involved) and with Sentry's own `symbolic`
  library resolving addresses from both PDBs. To see client frames named,
  crash the browser process in client code.

## What an install ships (2026-09-12)

Two things make an install report crashes, and `phi-installer.nsi` writes
both when built with `/DPHI_TELEMETRY_JSON=<file>` (`make-installer.sh`
passes it by default, rendering the file with the sibling
`push-telemetry-config.sh --print`; `--telemetry <file>` overrides,
`--no-telemetry` omits both):

1. `phi_telemetry.json` in the DEFAULT user data dir
   (`%LOCALAPPDATA%\Phi\User Data`). The installer owns it — every install
   or update rewrites it, so an endpoint change ships with the next
   version. A `--user-data-dir` profile does not see it (the test box's
   `C:\phi-fx-test` still needs the push script).
2. Upload consent: `install_static::GetCollectStatsConsent()` reads
   `HKCU\Software\Phi\usagestats == 1` — `Software\Phi` itself
   (`GetUnregisteredKeyPathForProduct`; there is no Google Update
   integration, so no `ClientState` sub-key). Absent = dumps captured,
   never uploaded. The installer writes it; uninstall deletes it. The
   client's `analyticsEnabled` pref does not touch it — analytics consent
   and crash-upload consent are separate switches today.

Still open: a compiled-in default for builds that ship without the file;
consent following the Phi analytics pref (a fork change); Crashpad `prod`
(empty, Phi.exe has no product name in its version resource) and a release
tag, so Sentry cannot group by build yet.
