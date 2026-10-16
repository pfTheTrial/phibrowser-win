# Pre-release review — phibrowser-win, 2026-10-14

Against client tip `000bd92`, build repo `dfc31a7`, fork
`feat/phi-r154-framework-win` `ed8fcb7ea05`. Read-only: nothing built, run
or deployed. Six lanes: release pipeline + installer, security
re-verification, data-loss / coordinator re-verification, the code landed
since `f1faf71` (Peek/Reader in split, Settings Spaces page), UI
re-verification, and the fork's Windows diff. The 2026-10-10 review is the
baseline; every item there was re-read, not trusted.

**Verified** = the reviewer re-read the cited code path and the defect
holds. **Plausible** = the path is real but the consequence was not traced
to the end (needs a box run). Line numbers are the current tree.

## Fix status — 2026-10-14 (same day)

R1, H1, H2 and H3 are fixed in the working tree (uncommitted, fork and
client); the framework and client build clean and are deployed to the box
(`phi-fx-ship`).

- **H1 / H2** — one deferred hop, `WM_PHI_SURFACE_TURN` on the coordinator's
  message window (`PhiSurfaceCoordinator::Host::PostSurfaceTurn`). A Peek
  replaced from inside `OnBrowserWindowCreated` is marked, hidden and closed
  on the next turn (`DeferCloseSurfacesForOrigin`; the show/hide pass skips a
  pending close), and `OpenSurfaceLinkAsSplit` queues its work the same way.
  Box-smoked with `test/probes/pk.mjs` real input: Shift-click on left.html
  → Peek; Shift-click inside the Peek → one Peek showing next.html, the first
  gone; Alt-click inside the Peek → `next.html` + `final.html` split, no
  surface left; Phi alive throughout.
- **R1** — crash-upload consent follows the user's data-sharing switch,
  the Mac's way: the coordinator pushes `analyticsEnabled` through a new
  `PhiFramework::SetMetricsReportingEnabled` (Chromium's
  `ChangeMetricsReportingState`, the chrome://settings toggle's path), which
  writes the Local State pref and `HKCU\Software\Phi\usagestats` and
  starts/stops UMA + Crashpad uploads live. Fork: three `IS_PHI_BROWSER`
  guards make the Windows consent per user whatever the install level
  (`install_static::GetCollectStatsConsent`,
  `GoogleUpdateSettings::GetCollectStatsConsentImpl` /
  `SetCollectStatsConsent`), and `kMetricsReportingEnabled` defaults OFF
  under `IS_WIN_PHI_CLIENT` (it was the Mac's opt-out `true`, which is why
  UMA and crash dumps uploaded with no consent on Windows at all — the
  installer's `usagestats=1` was only half of it; Chromium's own
  `SetupMetrics` re-wrote that value from the pref). Installer writes no
  consent and deletes the HKLM value older installers wrote. Settings
  toggle + hint strings now say "and crash reports". Box-verified with the
  deployed build: `analyticsEnabled` false → Local State
  `reporting_enabled` false, `usagestats` 0, Crashpad `upload_enabled` 0;
  true → all three on. The live toggle (no relaunch) rides Chromium's own
  path and was not separately exercised. Fork change is uncommitted in the
  working tree and must be folded into `feat/phi-r154-framework-win`.
- **H3** — `PhiSessionRestore::Settle` lifts the `PrimaryAwaited` guards only
  for a verdict settle (receipt or fallback timer, `SettleLiftsGuards`); the
  first user shell's "window" settle leaves `unreported_profiles_` and
  `awaiting_eager_` in place until each profile's park report lands. Host
  test added; code-only, not box-tested (needs a two-profile cold start with
  a racing switch).

## Verdict

**Almost there. NO-GO until two items land; GO for the first official build
once they do.** The 2026-10-10 blockers B1, B2, B3, B5 and B6 are fixed and
hold up under re-reading. B4 (crash dumps uploaded with no consent) is
still open, and the new release pipeline has one hole: the test workflow
can publish to the public update feed. Both are small changes. Everything
else below is High-and-under and can ride the first stable or the first
point release.

## Status of the 2026-10-10 blockers

| # | Defect | Status | Evidence |
|---|--------|--------|----------|
| B1 | Channel hard-coded canary → staging tenant | **Fixed in the official pipeline.** Source still says `"canary"`; `ci/release.sh official` stamps `PHI_WIN_CHANNEL "stable"`, and `phi_auth.cc:48-53` selects the filled-in production tenant, with sync and account URLs following the domain. Stampers dry-run on scratch copies: regexes match, output correct. | Verified |
| B2 | TEST signing only | **Fixed in design.** `sign-pe.sh` Azure backend via jsign; `release.sh:104` refuses anything but the Azure identity for official. Cannot be exercised until the Trusted Signing profile exists (jsign is not installed here). | Plausible |
| B3 | Feed hard-wired win-nightly | **Fixed.** `stamp-channel.py` writes the win-public feed; `release-gate.py --release` pairs channel, feed and signer. | Verified |
| B4 | Crash upload without consent | **STILL OPEN.** `phi-installer.nsi:1210-1213` writes `usagestats=1` HKLM(+HKCU) whenever `phi_telemetry.json` ships (the default). The fork's `GetCollectStatsConsent` returns exactly that value; no client code reads or writes it; the Settings analytics switch only touches PostHog. | Verified |
| B5 | CDP scope bypass | **Fixed (client).** `FilterTargetList` on `/json` + `/json/list`, scope check before `OpenDevToolsSession`, `/devtools/browser/*` forced to the browser target, one generic `targetId` → `TargetInScope` check in `DispatchOnUi` covering all five targetId verbs. Fork-side wall still missing (see F4). | Verified |
| B6 | Cold-start restore deletes default Space's tabs | **Fixed (core).** Restored window hosted on its own Space (`AttachRestoredGhost`), per-(Space, window) rows, settle drops only windows no row claims. Four neighbouring parked-window gaps remain (M1–M4). | Verified |

## Blockers

### R1 — Crash dumps upload with no consent and no off switch (B4)
`installer/phi-installer.nsi:1210-1213`; fork `chrome_crash_reporter_client_win.cc:159`.
A stable public install sends every minidump (process memory, which can
include cookies and tokens) to Sentry. The onboarding analytics choice does
not touch it. Fix: do not write `usagestats` in the installer; have the
client write it (HKCU) from the onboarding choice and a Settings toggle,
or at minimum tie it to the existing analytics switch. Verified.

### R2 — The test workflow can overwrite the public update feed
`.github/workflows/test-release.yml:39-43`, `ci/release.sh:29`,
`ci/publish-artifacts.sh:24-25`.
`test-release.yml` has no `environment:` yet receives the same S3 secrets as
the official job; `release.sh test` honours `PHI_OTA_CHANNEL` from the
environment and `publish-artifacts.sh` accepts `kind=test` on any channel.
Anyone with repo write can dispatch the test workflow from a branch with a
one-line change and push a TEST-signed, EdDSA-validly-signed build to
`win-public`; stable clients will install it (WinSparkle checks EdDSA, not
Authenticode). Fix: refuse `win-public` unless `kind=official` in both
scripts, and scope the test workflow's R2 token to `win-nightly/` (or move
the win-public S3 secrets into the production environment). Verified.

## High — fix before release

### Crashes and data loss
- **H1. Nested Peek closes the gesture's own browser synchronously inside
  `OnBrowserWindowCreated`.** `phi_coordinator.cc:4656-4694`: a Peek opened
  from inside a Peek is remapped to the origin tab, then
  `CloseSurfacesForOrigin(..., "replaced")` → `CloseBrowserWindow(source)`.
  The announcement is delivered inside `Browser::Create`, which contract
  rule 2 (`phi_framework_api.h:48-53`) forbids closing browsers from, and
  the source WebContents is the opener of the navigation still on the
  stack. Trigger: Shift-click any link inside a Peek. Two lanes found this
  independently. Fix: post the replaced-peek close to `msg_hwnd_` like
  `WM_PHI_READER_EXPAND`. Rule violation verified, crash plausible.
- **H2. "Open as split" from a surface destroys the surface browser under
  its own caller.** `phi_surface_coordinator.cc` `PairPeekPageWith`
  (~470-505) ends with `CloseBrowserWindow(peek)`; it is reached
  synchronously from the fork's context-menu `ExecuteCommand` and from
  `OpenURLFromTab` for Alt-click. Trigger: inside a Peek, right-click a link
  → Open in Split View, or Alt-click. Fix: defer the whole operation via a
  posted message, as `PostReaderExpand` does. Verified path, crash plausible.
- **H3. The restore "awaited" guard is lifted before every profile has
  reported.** `phi_session_restore.cc:160-166` `Settle` clears
  `unreported_profiles_`, and is reached from the first user shell's first
  snapshot (`phi_coordinator.cc:5289-5290`), which the code itself says can
  beat the park report. In that gap a fresh request for a still-unreported
  Space (launch URL via a rule, `spaces.switch`, a quick pip click)
  overwrites the row → the park report lands → the real window is an orphan
  → `DropOrphanGhosts` deletes its tabs. Same shape for a profile not loaded
  at startup. Fix: keep a profile in `unreported_profiles_` until its own
  `OnColdStartParkedWindows` arrives. Plausible (framework ordering).

### Security
- **H4. Crash endpoint read from the user-writable profile directory.**
  Fork `chrome_crash_reporter_client_win.cc:163-200` reads
  `phi_telemetry.json` from the **user data dir**, not the install dir. Any
  same-user process can drop a `crash_endpoint` and receive Phi's minidumps.
  Also any `--user-data-dir` launch silently has no upload. The 2026-10-10
  fix list already asked for the install-dir read. Verified.
- **H5. Google API key and OAuth client secret committed in both args
  files** (`phibrowser-win-build/args/PhiFxWinShip.gn:14-16`, Arm64 twin;
  pushed). They are Chromium's well-known default keys, not ours; shipping
  them in a stable product is a Google ToS problem and the quota can be
  revoked under us. `release-gate.py` does not check for them. Decide:
  register Phi's own project keys, or build without keys and accept that
  Google-backed features (geolocation, translate, safe browsing lookups
  that need a key) are off. Verified.

### Release mechanics
- **H6. Downgrade trap for everyone on win-nightly.** Official build numbers
  come from the official workflow's own `run_number`, so the first stable is
  `0.0.4.1`-ish while nightly testers run `0.0.4.7+`; the installer refuses
  without `/ALLOWDOWNGRADE` and a nightly client never sees the stable feed.
  Sentry releases are named `full_version` in one project, so official and
  test `0.0.4.N` collide. Fix: bump `PHI_WIN_VERSION_PATCH` (or go to a real
  release version, e.g. `0.1.0` or `1.0.0`) before the first official build,
  and namespace the Sentry release by ring. Verified.

## Medium

### Coordinator / Spaces / restore
- **M1.** Changing the profile of a cold (parked) Space loses its parked
  tabs: `RehomeSpaceData` only `ClearParked`s in memory; next run the
  restored window is closed via the pending-path `WM_CLOSE`
  (`phi_coordinator.cc:2762-2771`, `:4918-4925`). Also reachable from
  `SyncRebind`, i.e. from another device. Verified path.
- **M2.** `NoteSpaceWindow` records the Space's profile, not the browser's
  (`:2009-2013`); the fallback host path can put a profile-P browser on
  another profile's default Space, whose row then never matches the receipt
  → orphan → dropped. Plausible.
- **M3.** Sync-hidden Space's parked secondary window is dropped (hidden is
  not deleted; `DropOrphanGhosts` drops strays whose Space `!HasSpace`,
  `phi_session_restore.cc:271-273`); `SyncPurge` never forgets ghost data,
  so a remotely deleted Space's parked window leaks forever. Verified.
- **M4.** `phi_local_store.cc:84-101, 497-526` bypasses `PhiReadFile`: a
  locked manifest reads as "no manifest", the `kRequiresNewerApp` guard is
  skipped and a newer Mac-written store is stamped V13. Plausible.
- **M5.** Surfaces hide/show during the Space slide
  (`phi_browser_host.cc:797` fires while the swap runs): the entering
  Space's Peek/Reader shows over the still-visible leaving page. Plausible.

### Peek / Reader / split (new code)
- **M6.** Divider drag re-shows and re-raises every surface per frame:
  `OnContentBoundsChanged` → `NoteContentLayoutChanged` →
  `SetSurfaceShown(true)` unconditionally for every live panel
  (`phi_coordinator.cc:5102-5107`). Z-order churn per mouse move. Only call
  when the state changes. Verified chain.
- **M7.** Reader View inside a Peek is unhostable: `HostSurfacePanel` for
  `kReaderPanel` resolves the controller with the peek browser as opener →
  null → falls through to a stray top-level window hosting reader.html
  (`phi_coordinator.cc:4518-4521`). Either remap like the Peek case or
  refuse. Verified path.
- **M8.** Transient full-card reader over both panes when a split forms
  (`phi_reader_panel.cc:140-143` returns early without updating `in_pane_`).
  Cosmetic.

### Security / agents
- **M9.** Whole-profile cookie read survives B5 for agents sharing the
  regular profile: the fork's browser-target filter refuses
  `Storage.getCookies` only when `browserContextId` is absent
  (`phi_devtools_transport.cc:417-419`). An agent in the Default profile
  learns the context id from its own in-scope target and reads every cookie
  of the profile. Same on Mac; inherent to a shared profile. Mitigation:
  default agent/shadow windows to a dedicated agent profile, or have the
  fork check the context id against in-scope windows. Verified at the filter.
- **M10.** Middle-click closes a tab while the agent holds input
  (`phi_sidebar_view.cc:575-587` has no `agent_holds_input_` check; the ✕
  and Delete paths do). Still open from 2026-10-10. One line. Verified.

### Release pipeline
- **M11.** Stale sibling `sign-pe.sh` fails hours late: `release.sh:100`
  checks the Actions checkout's copy, but `make-installer.sh:184` calls the
  **runner's** `../../phibrowser-win-build/sign-pe.sh`, and `:188` swallows
  a failing `--info` into "signing: none". Both arches build, then `--verify`
  fails. Preflight should `cmp` the two or make-installer should take the
  tool path. Verified.
- **M12.** `sign-pe.sh --info` proves nothing for Azure: it echoes the
  account/profile from env (`:126-129`); a wrong tenant or secret is first
  exercised inside make-installer after the x64 build. Add a token mint
  (and ideally a jsign dry call) to preflight. Verified.
- **M13.** `release-gate.py --release` does not assert `is_official_build`
  / `dcheck_always_on` from the out dir's `args.gn`, version ≠ `0.0.x`,
  absence of the Google keys, that `phi_telemetry.json` is staged, or that
  every stage PE is signed (only the two installers are verified). Verified.
- **M14.** First official run dies in preflight because `copy-to-s3.py`
  `check()` needs ≥1 object under `win-public/` (CI.md documents the
  pre-create). Fails safe, but will fail. Verified.
- **M15.** Admin per-machine install still writes `phi_telemetry.json` and
  HKCU keys into the installing admin's own profile when the session user
  *is* the admin; only over-the-shoulder UAC is detected
  (`phi-installer.nsi:343-355`). Other users get no telemetry config.
  Verified.

## Low

- `PhiSetupUI.dll` is unsigned and extracted to `%TEMP%`; WDAC/AppLocker
  PCs may block the installer UI. Sign it in the stage pass.
- `is_signed` skips any pre-signed PE (WinSparkle.dll ships with its
  third-party signature); no post-sign `--verify` of the stage.
- Onboarding links use `http://phibrowser.com/privacy/` and `/terms/`
  (`phi_onboarding_window.cc:82-83`); the server 308s to https, but the
  feedback window already uses https. Make them https.
- 16 non-UI keys still in `resources/strings.json` (`app.authorizationBearer`,
  `app.tmp`, `sidebar.chromeNewtab`, …); 501 keys carry machine
  translations, not 244 as the fix status says. Translation-repo review
  burden is ~2× the doc.
- `make-installer.sh:69-75` version grep dies silently under `pipefail` on
  a non-matching line and never validates `X.Y.Z`.
- Task adoption on re-bind overwrites ownership (`RecordTask` unconditional;
  `phi_verbs_lifecycle.cc:792-797`): a connection with a different
  capability can take over another agent's window. Nominal today because
  all connections of a session share one capability id.
- Credential approval dialog omits the requested `fields` list; vault
  helper calls block the UI thread up to 5 s (declared partial).
- Hand-closed primary rows go stale; SessionIDs restart per process, so a
  later run can mis-host a parked window under the wrong Space.
- `phi_telemetry.cc:179-197`, `phi_feedback_outbox.cc:101-118`,
  `phi_credentials.cc:34-48` keep private readers that conflate locked with
  missing.
- `SplitPaneCardCorners` rounds the lower-right pane corner on Windows 10
  and snapped Win11 windows (square window corners).
- Hidden Peek/Reader pages stay VISIBLE to Chromium and keep compositing
  (`SW_HIDE` on the panel only).
- `CreateRoundRectRgn` remains on the status bubble
  (`phi_browser_host.cc:633-635`) and the Space hover card's non-layered
  path (`phi_spaces_strip.cc:1514-1518`).
- Settings Space editor announces English icon names in every locale.
- Fork: `kProductPathName = L"Phi"` is unguarded (any non-Phi build from
  the tree uses `%LOCALAPPDATA%\Phi`); `user_data_dir.cc:57-83` reads HKCU
  before HKLM for the install choice; ~20 reformat-only lines in
  `select_file_dialog_win.cc`; importer tags under `IS_MAC` not a Phi guard.

## Fork items from the 2026-10-10 list

| Item | Status | Evidence |
|------|--------|----------|
| Own registration names Phi / PhiHTM / PhiPDF | **NOT DONE** | `chromium_install_modes.h:65-73` still `Chromium`, `ChromiumHTM`, `ChromiumPDF`; AppUserModelID `Chromium` groups Phi with Chromium's taskbar button. Installer skips registration when a real Chromium owns the names (`phi-installer.nsi:146-154`), so Phi cannot become the default browser beside Chromium. |
| Read `phi_telemetry.json` from the install dir | **NOT DONE** | Reads from the user data dir (H4). |
| `canResume` in `ListDownloadsJson` | **DONE** | `phi_framework_impl.cc:2833`. |
| Windows `AllowInspectingTarget` wall | **NOT DONE** | `chrome_devtools_manager_delegate.cc:424` is `IS_MAC_PHI` only; the Windows stub returns true. The client filter is the only wall; a regression there re-exposes every tab. |

## Still deliberately partial (unchanged, acceptable for 1.0)

Vault helper on the UI thread with 3–5 s timeouts; no sentinel Space id for
incognito/agent shells; high-contrast palettes not covering hard-coded RGB
in onboarding/settings/popups; every pipe principal gets credential
reveal/fill (mitigated by hardware-input prompts with Deny preselected).

## Checked and fine

- **Pipeline:** workflows are `workflow_dispatch` only, `contents: read`,
  `persist-credentials: false`; `environment: production` on official and
  recovery; Azure vars/secret written to a 0600 `$RUNNER_TEMP` file, trap
  removed, unset before build; official refuses the runner's TEST env;
  separate win-public stage root; appcast public key matched against the
  client; `check-pe-version.py` on exe + installers; `verify-appcast.py`
  checks EdDSA over exact bytes and both `sparkle:os`; copy-to-s3 never
  overwrites differing bytes and writes the feed last with `no-cache`;
  stamped sources restored on EXIT; concurrency group + flock. `sign-pe.sh`
  Azure invocation shape matches jsign ≥5 docs; non-zero exits fail the
  build; `!finalize/!uninstfinalize` abort makensis on failure; the
  Microsoft Identity Verification Root 2020 pem is genuine.
- **Installer:** downgrade refused without `/ALLOWDOWNGRADE`; running check
  on install and uninstall; uninstall deletes by compile-time list; wildcard
  sweep guarded; "Delete my data" by name; Browse appends `\Phi Browser`.
- **Secrets:** signing env, telemetry env, appcast private key, signing-test
  gitignored and absent from `git ls-files`; no key material in the client
  repo; Auth0 client ids only.
- **Security (client):** named-pipe SDDL + `PIPE_REJECT_REMOTE_CLIENTS` +
  first-instance; constant-time token compare; 1 MB / 16 MB / 64 MB caps;
  `testRig` and `PhiCDPAgentAccessEnabled` only under `kPhiDevHooks`;
  `ResolveAgentProfile` resolves "" to the first *allowed* profile and
  shadow.create checks; `allow_agent_user_spaces` and `allow_agent_control`
  default false; `OwnerForSlot` OTR gate behind every bookmark/pin writer
  plus the accelerator short-circuit; `BroadcastEvent` no longer blocks the
  UI thread; paste-and-go strips `javascript:`; protobuf/JSON skippers
  depth-bounded; `PhiReadStatus` tri-state with `save_blocked_` on error.
- **Fork:** every shared-Chromium behavioural edit is under
  `IS_WIN_PHI_CLIENT` / `IS_MAC_PHI || IS_WIN_PHI_CLIENT` / `IS_PHI_BROWSER`;
  no `IS_PHI_HOSTED`; no sandbox / site-isolation / Safe Browsing /
  certificate changes; no loopback `--remote-debugging-port`; API header
  passes nothing std:: by value or non-const; TTS all-voices falls back to
  upstream on failure; branding Phinomenon / Phi; no Omaha.
- **UI:** every 2026-10-10 High UI item and 22 of 23 sampled Mediums hold
  (onboarding/feedback fit + scroll, omnibox hover, AltGr, drag cancel band,
  sidebar band cap, strip overflow chevron, Settings keyboard + UIA, Alt+Tab
  menu, caption buttons, tooltips, downloads, Esc confirms, IDN rules, …).
- **New code:** API boundary (`OnOpenLinkAsSplitFromSurface` copies the
  string), AA rule (no GDI RoundRect/region in added code), strings via the
  catalog, container snapshots before closes, focus hand-back through
  `Detach`, Space-switch ordering, Settings Spaces reorder semantics.
- Source-only `release-gate.py` passes; `verbs.md` is current (155 rows);
  `check-main-diff.sh` reports 7 labelled hunks against upstream
  `chrome_exe_main_win.cc`; privacy and terms pages return 200; the
  win-nightly feed is live, win-public does not exist yet (expected).

## Pre-first-official-run checklist

1. Fix R1 (consent) and R2 (win-public gate) — code.
2. Bump the version (H6) and decide the Google keys (H5).
3. Install jsign on the runner; update `/home/fydeos/phibrowser-win-build`
   so the sibling `sign-pe.sh` matches (M11).
4. Create one object under `win-public/` in R2 (M14).
5. Set the production environment vars/secret; run the official workflow
   with `PHI_CI_PREFLIGHT_ONLY=1` once the Trusted Signing profile exists,
   then add a token mint to preflight (M12).
6. Land H1/H2 (Peek closes) and H4 (install-dir telemetry read) — H4 is a
   fork change, so squash it into `feat/phi-r154-framework-win` first.
7. Box-test the official installer on a clean, non-trusting PC:
   SmartScreen, UAC publisher, default-browser registration, update from a
   prior stable, uninstall.

## Suggested order after release

H3 → M1–M4 (parked-window loss cases, one coherent restore pass) → fork
registration names + Windows DevTools wall → M6–M8 (surface polish) → M9
(agent profile isolation) → Lows.
