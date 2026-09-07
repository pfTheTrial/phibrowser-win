# Lane 6 — Tests, build, tooling (phibrowser-win)

Read-only review, 2026-09-07. Paths are relative to `/home/jx/Phi/phibrowser-win` unless absolute. Nothing in the repo was modified; the only side effect was running `test/model/run.sh` (writes to `/tmp/phi_sidebar_*_test`) to confirm the host tests build and pass.

Headline numbers: `test/` holds 104 files (85 tracked, 18 untracked, 1 dir). 19 `suite-*.mjs` files carry 115 `test()` cases; 64 other `.mjs`/`.ps1` files are probes/setup/cleanup/rigs, of which 26 end in an unconditional `process.exit(0)` and cannot fail. Three host-compiled C++ tests cover four pure sidebar modules. There is no lint, no format config, no `.gitignore`, no CI entry point in the repo; the box runners live outside it (`/home/jx/Phi/run-fx-all.sh`, `run-fx-test.sh`).

---

## 1. Inventory of `test/`

Legend. **Kind**: S = suite (uses `test()/run()` from the harness), P = probe (prints, no assertions), Q = probe with its own PASS/FAIL counter, SU = setup for a manual screenshot, CL = cleanup, R = rig script (PowerShell run by a box scheduled task), M = host model test, H = harness, MT = maintenance. **Asserts**: Y = throws/exits non-zero on failure, N = prints only, `exit0` = ends with unconditional `process.exit(0)`. **Idem**: idempotent/self-cleaning (Y / partial / N). **Ref**: referenced by a runner or doc (runner = `/home/jx/Phi/run-fx-all.sh:8` globs `suite-*.mjs`; doc = `docs/sidebar-architecture.md`; mem = memory note). **Tracked**: git.

| File | Kind | Exercises | Asserts | Idem | Ref | Tracked / last commit |
|---|---|---|---|---|---|---|
| phi-test.mjs | H | pipe discovery, WS client, control + CDP, mini framework | — | — | runner (sftp'd `run-fx-all.sh:27`) | Y 09-02 |
| uicap.ps1 | R | PrintWindow pixel sampler for PhiUiCap task | — | — | runner `:27` | Y 08-09 |
| model/run.sh + 3 `*_test.cc` | M | BuildSidebar, layout grid/list/strip widths, place helpers + drop resolvers | Y (`CHECK` + exit 1, `sidebar_model_test.cc:15-21,305-310`) | Y | doc §8, `phi_sidebar_model.h:18` | Y 09-07 |
| suite-binding | S (3) | pinned.open tag binding, remove releases tab | Y | partial (closes by URL marker `:15-23`) | runner | Y 08-11 |
| suite-core | S (7) | ping, list, unknown verb, navigate+CDP, newWindow/close, /json/version, Runtime.evaluate | Y | Y | runner | Y 08-09 |
| suite-emoji | S (6) | catalog groups/count (hard-coded 1914 `:26`), spaces.setIcon, **restarts app** `:101-119` | Y | partial (resets icon to 1F310) | runner | Y 09-01 |
| suite-feedback | S (4) | feedback.open/form/status, outbox job prep, backoff | Y | N (jobs left in outbox; expects upload to FAIL `:115-125`) | runner | Y 09-03 |
| suite-groups | S (3) | tabs.group, groups.rename/color/collapse, tagged tab refuses | Y | Y (own `cleanup` `:27-40`) | runner | Y 08-11 |
| suite-layout | S (7) | layout.get/set, window.layout geometry, pixels via PhiUiCap `:164-183` | Y | partial (restores performance; leaves theme mint `:133`) | runner | Y 08-10 |
| suite-menu | S (8) | menu.list/click, radio checks, Spaces rows (capped 100 `:129-133`), bookmark row | Y | Y | runner | Y 08-10 |
| suite-nav | S (8) | nav.state/back/forward/reload/stop; omnibox resolution (network) | Y | Y | runner | Y 08-09 |
| suite-persist | S (4) | space/layout/theme survive **3 restarts** `:10-31` | Y | N (creates `Persisted <ts>`, `Seq A/B` spaces, never deleted `:38,66,73`) | runner (comment says "must run last" `:3-5`; runner is alphabetical) | Y 09-01 |
| suite-pinned | S (10) | pin scopes, add/list, scope merge, bookmarks tree, **restart** `:188-206`, favicon (github.com network `:229`), rows.reorder | Y | Y via `clearAll` `:13-23` (wipes ALL pins/bookmarks of the profile) | runner | Y 09-01 |
| suite-profiles | S (7) | profiles.list/create/rename/delete, space↔profile binding | Y | partial (creates 'Work' profile permanently `:85`, 'Shared A/B' spaces never deleted `:52-55`) | runner | Y 08-10 |
| suite-settings | S (8) | settings.get/set, panes, saturation (pixels `:128-149`), **restart** `:178-196` | Y | partial (restores defaults `:204-207`) | runner | Y 09-03 |
| suite-spaces | S (10) | spaces.list/create/switch/rename/delete/reorder, last-space rule, default-role handoff | Y | N ('Work','Research','Closable' created and left `:32,55,118`; last-space test deletes EVERY other space `:224-227`) | runner | Y 09-02 |
| suite-split | S (3) | tabs.split/unsplit | Y | Y (`cleanup` `:24-39`) | runner | Y 08-11 |
| suite-tabs | S (8) | tabs.list/new/activate/close, target=_blank via CDP, move | Y | Y | runner | Y 09-01 |
| suite-tabui | S (3) | active-row chip in pixels (PhiUiCap) | Y | Y (`normalizeTabs` `:154-174`, pins layout+theme `:137-143`) | runner | Y 09-01 |
| suite-theme | S (8) | theme.get/set, appearance, pixels, per-space theme | Y | Y (restores pure/light) | runner | Y 08-10 |
| suite-toolbar | S (4) | URL field ink in pixels, back button | Y | Y | runner | Y 08-09 |
| suite-window | S (4) | window.bounds/setBounds | Y | Y | runner | Y 08-09 |
| sv-drag | Q | real mouse drags via PhiDragCap; SidebarMove outcomes | Y (own `check` `:13-14`, exit `:187`) | partial (T4 removes the bookmark it made `:109`; T1–T3 leave order changed) | doc L17 | Y 09-07 |
| sv-surface | Q | reader/peek indicator lifecycle via PhiDragCap clicks | Y (`:12`, `:125`) | partial (adds example.com pin `:49`, closes plain tabs beyond four `:38-42`) | doc L17 | Y 09-07 |
| sv-splitcell | Q | one-row-per-split-cell open/focus semantics | Y (`:10`, `:74`) | N (opens every split cell) | — | Y 09-07 |
| sv-placement | Q | first-frame window placement vs saved prefs | Y (`exit ok?0:1` `:328`) | Y | — | untracked |
| sv-tree-setup | SU | seeds 5 plain tabs, a group, a split for a screenshot | N | N | — | Y 09-07 |
| sv-surface-show | SU | stages a reader for a screenshot | N, exit0 | N | — | Y 09-07 |
| sv-sidebar-look | SU | seeds folder + grey group | N, exit0 | N | mem (phi-box-capture-rigs) | untracked; **no import** — `openControl`/`sleep` undefined at `:5,13` |
| sv-reader | P | readerArticle/saveArticle through the skill helpers | N, exit0 | Y (`H.complete()`) | — | untracked; imports `file:///C:/Users/jdxin/phi-browser-skill/...` `:3` |
| sv-default, sv-pin, sv-state, sv-lr-setup, sv-lr-check, sv-shadow, sv-shadow2, sv-jsdialog, sv-zombie, sv-saveas, sv-emoji | P | space default role; window-pinned dialect; lazy restore staging/check; shadow windows; JS dialogs; zombie agent window; Save As DCHECK; emoji lazy-load DCHECK | N (most exit0) | N (sv-default deletes every non-default space `:18`) | — | untracked |
| sv-dialog.ps1 | R | dismiss `#32770` dialogs from the interactive session | N | — | — | untracked |
| rv-check, rv-overlay, rv-overlay2, rv-style, rv-style2, rv-reset, rv-refuse, rv-flash, rv-agent, rv-button, rv-layouts, rv-sites, rv-state, rv-open, rv-setup, rv-shot-setup | P / SU | Reader View: menu item, overlay lifecycle, HUD style round-trip via PhiPrefs.json, refusal, button placement per layout, distillability by site (network: wikipedia, 163.com, zhihu) | N, exit0 (rv-check/overlay2/style/style2/reset exit 1 only on "no reader page") | N | — | Y 09-03 |
| rv-quit | MT | closes all windows through RequestClose so a deploy can overwrite the exe | N, exit0 | — | — | Y 09-03 |
| rv-toast.ps1 | R | watch for `PhiToast` window (interactive session) | N | — | — | Y 09-03 |
| am-drive | P | agent operating mask via CDP key drives; 90 s keepalive loop `:60` | N | N | — | Y 08-31 |
| ap-click, ap-probe, ap-split | P | auto-peek via CDP click / cross-site nav | N (ap-click logs to `C:\Users\jdxin\phitest\ap-click.log` `:7`) | N | — | untracked |
| b6-setup/bounds/check/cleanup | SU/P/CL | two-window drag staging | N | — | — | Y 08-31 |
| b11-setup/cleanup | SU/CL | bookmark-bar seeding | N | CL wipes all bookmarks `:5-8` | — | Y 08-31 |
| b13-open/check/cleanup | SU/P/CL | settings shortcuts pane | N | — | — | Y 08-31 |
| b15-icon/check | SU/P | space catalog icon | N | N | — | Y 08-31 |
| b16-crash/fullscreen/restore | P | chrome://crash sad tab; element fullscreen via CDP | N | b16-restore undoes | — | Y 08-31 |
| blink-probe/cleanup | SU/CL | space-switch flicker | N | CL removes BlinkProbe space | — | Y 08-31 |
| bm-setup, fi-setup, dt-setup, tp-setup, sp-setup | SU | bookmark manager tree, folder icons, dormant tabs, tab previews, stacked splits — each prints row geometry for a capture graft | N | bm/fi wipe bookmarks/folders first `bm-setup:14-21`, `fi-setup:17-23` | — | Y 08-30/31 |
| c1-devmode/reset, c2-cred, c3-watch/owner/after/cleanup, c4-capture/mirror/ownership/profile/totp | P | developer mode; credentials.get/getTotp (approval dialog); agent Space create/setState/ownership events; captureWindow; session mirror; profiles.create id | N (c4-ownership uses `onEvent`/`pump` `:5,9`) | c4-* call `agentSpace.complete` | — | Y 08-31 |
| lr-arm, lr-park, lr-probe, lr-two | P | lazy Space restore staging and framework receipts | N | N (lr-arm flips restoreLastSession off `:12`) | — | Y 08-31 |
| diag, diag2 | P | slot/space/profile readout; activate repro | N | — | — | Y 09-01 |
| sweep-spaces | MT | list/delete junk Spaces by name regex `:6` | N | — (the cleanup the suites lack) | — | Y 08-31 |

**Dead vs living.** Living: the 19 suites (run by `run-fx-all.sh`), the 3 model tests (run by `test/model/run.sh`, cited in the design doc), and the three sidebar `sv-drag/sv-surface/sv-splitcell` probes (cited in `docs/sidebar-architecture.md:16-17`, last commit 09-07). Everything else is a one-off tied to a feature bring-up (`b6…b16` = August bug numbers, `c1…c4` = agent/credential checks on 08-31, `rv-*` = Reader View week of 09-03, `lr-*` = lazy restore) with a single commit each; none is referenced by any runner, README, or doc, and 26 of them cannot fail (`process.exit(0)`). They are useful as verb-usage examples and box-driving recipes, not as tests. The 18 untracked `sv-*`/`ap-*` files are today's scratch and two of them cannot run as checked in (`sv-sidebar-look.mjs:5` uses `openControl` without importing it; `sv-reader.mjs:3` imports from an absolute `C:\Users\jdxin` path).

---

## 2. Harness (`test/phi-test.mjs`) and what suites re-implement

**What it abstracts** (266 lines):

- Discovery: reads `<user-data-dir>\CDPAgentSocket` (`:25-29`); the dir defaults to `C:\phi-fx-test` (`:17-18`) and is the only CLI flag parsed. The usage line `node phi-test.mjs [--user-data-dir DIR] [suite ...]` (`:10`) is aspirational: the harness never loads suites; each suite is its own entry point that imports the harness and calls `run()` (e.g. `suite-binding.mjs:5,83`).
- A hand-rolled RFC 6455 client (`Ws`, `:40-104`). Send caps frames at 64 KiB (`:82`, "frame too large for harness") though the read side handles 64-bit lengths (`:62-66`). Timed-out readers are removed from the waiter queue so they cannot eat the next pushed event (`:93-98`) — a real bug they hit and fixed.
- `openControl()` (`:107-169`): WS upgrade to `/phi-agent`, `{id,type,payloadJson}` envelope matching the server's loop (`client/phi_control_server.cc:513-541`), reply correlation by id with pushed events dispatched to `onEvent` listeners (`:137-148`), `pump(ms)` to drain events (`:154-166`). Default read timeout 10 s per frame (`:87`), handshake 8 s (`:125`).
- `cdpHttp(path)` (`:172-195`) and `openCdp(wsPath)` (`:198-235`) over the same pipe; CDP events are dropped (`:230`). The byte-accurate `Content-Length` handling (`:176-178`) records another bug they hit (zh-CN tab title).
- The framework (`:237-266`): `test(name, fn)`, `sleep`, `assert`, `assertEq`, sequential `run()` that prints `PASS/FAIL name (ms)` and `N passed, M failed`, sets `process.exitCode`. Nothing else: no per-test timeout, no `beforeEach/afterEach`, no fixtures, no skip, no retry, no filter by name, no structured output.

**Server-side contract it relies on.** Framing/handshake in `client/phi_control_server.cc:475-560` (accept key `:490-499`, message loop `:513-556`, bad-json/missing-type errors `:539-543`). Verb dispatch is a 128-arm `if (type == "...")` chain in `PhiCoordinator::HandleControlCommand` (`client/phi_coordinator.cc:4697` onward; `grep -c 'type == "'` = 128) preceded by `HandleSkillDialectVerb` (`:4691`); unknown verbs yield `"unknown type: "` (`:6013`). 111 distinct `agentSpace.*` verb strings exist across `client/*.cc`. There is no verb registry, no schema, and no list-verbs endpoint, so the harness and suites carry the contract implicitly.

**What every suite re-implements by hand** (counts are copies found):

| Helper | Copies | Locations |
|---|---|---|
| `firstWindow(ctl)` | 15 | binding:7, emoji:13, groups:6, layout:15, menu:7, nav:6, pinned:7, profiles:5, spaces:6, split:5, tabs:7, tabui:125, theme:10, toolbar:78, window:4 |
| kill + `PhiFxLaunch` + poll-for-ping restart | 4 | persist:10-31 (function), emoji:101-119, pinned:188-206, settings:178-196 (inline PowerShell) |
| `samplePixel(s)` via PhiUiCap env-var graft | 5 | layout:164-183, settings:128-149, tabui:31-57, theme:104-123, toolbar:19-45 |
| "close tabs down to one, re-list after each close" loop | ~21 | tabui:157-161 and nearly every rv-/setup probe (am-drive:19-23, rv-check:45-52, rv-style:24-27, …) |
| `navigateAndSettle` / `waitTabs` / `waitFor` / `switchSpace` polls | 6 | nav:12-21, toolbar:101-110, tabs:14-22, feedback:17-25, rv-overlay:32-39, profiles:11-21 |
| `ensureTabs` / `cleanup` / `clearAll` / `closeByUrl` | 6 | groups:16-40, split:13-39, pinned:13-23, binding:15-23 |
| PhiDragCap `drag.txt` → `drag.done` rig | 2 | sv-drag:26-35, sv-surface:20-27 |
| A second assertion framework (`check()` + `failures`) | 3 | sv-drag:13-14, sv-splitcell:9-10, sv-surface:11-12 |
| `Get-Process Phi ... Stop-Process` | 10 | suites + both runners |

**Flakiness sources, with evidence:**

1. *Fixed sleeps dominate.* 98 `await sleep(…)` across the 19 suites versus 34 bounded poll loops. Whole suites are sleep-only: binding (6/0), groups (9/0), split (7/0), theme (4/0), window (2/0). Probes are worse (`rv-check.mjs:64` 6 s, `sv-surface.mjs:62` 8 s, `sv-surface-show.mjs:8` 9 s).
2. *One shared live profile, no isolation.* Every suite drives `windows[0]` of the one running instance on `C:\phi-fx-test`. Suites explicitly defend against each other: `suite-tabui.mjs:132-143` pins layout, size and theme "another suite … may have left the client in Balanced"; `suite-persist.mjs:103,128` and `suite-settings.mjs:204-207` "leave the default behind for the suites that assume it". `suite-pinned.mjs:13-23 clearAll` wipes every pin and bookmark in the profile, which breaks any sv-* probe that seeded state. Spaces accumulate across runs (`suite-spaces.mjs:32,55,118`, `suite-persist.mjs:38,66,73`, `suite-profiles.mjs:52-55`) to the point that `suite-menu.mjs:129-133` caps at 100 rows and `sweep-spaces.mjs` exists as manual garbage collection; `suite-spaces.mjs:224-227` (last-space rule) deletes every other Space, which is destructive to whatever preceded it.
3. *Order dependence the runner does not honour.* `suite-persist.mjs:3-5` says it "must run last"; `run-fx-all.sh:8` runs `ls suite-*.mjs` alphabetically, so persist runs 9th of 19. Six `Stop-Process -Force` restarts per full run (emoji, persist×3, pinned, settings) never commit a session; the status log records a bug class caused by exactly that (`windows-framework-status.md:4085`).
4. *Box grafts through global mutable state.* Pixel sampling sets **user-scope environment variables** and runs a scheduled task (`suite-layout.mjs:166-169`), then polls a file for `DONE` (`:170-172`); the memory notes document stale `.done` markers racing the task. The `PhiUiCap`/`PhiDragCap`/`PhiFxLaunch` task definitions live only on the box and are not in the repo. `uicap.ps1:37` still looks for `phi_client` while the exe is `Phi.exe` (`build-client.py:24,179`; suites use `Get-Process Phi`), so it depends on the `FindWindowEx "PhiClientShell"` fallback at `:41`.
5. *Network and third-party sites.* `suite-nav.mjs:42-62` (omnibox → real search), `suite-pinned.mjs:229-237` (github.com favicon), all rv-* (wikipedia, 163.com, zhihu in `rv-sites.mjs:7-12`), `sv-surface.mjs:56,85,96`. `suite-feedback.mjs:115-125` asserts the upload **fails** (`failedWaitingRetry`), so the test inverts if the account service ever becomes reachable.
6. *Hard-coded invariants that mirror source constants.* emoji count `1914` (`suite-emoji.mjs:26`), sidebar width `240` (`suite-layout.mjs:12`), exact menu-bar order (`suite-menu.mjs:31`), pane list (`suite-settings.mjs:10-11`), theme list (`suite-theme.mjs:8`), extension id `ickhcgejficcoofnjnnobadfdnfbilnm` in 7 rv- files, box paths `C:\Users\jdxin\…` in 17 files, `192.168.100.170` in the runners.
7. *Timeouts.* A slow verb (profile creation, space switch with profile load) trips the 10 s frame timeout (`phi-test.mjs:87`) and surfaces as `ws read timeout` rather than a named failure; there is no per-test budget, so a stuck poll loop of `40×1000 ms` (`suite-persist.mjs:21-29`) is the only ceiling.

**How pass/fail is reported.** Suites: `run()` prints one line per test and a summary, exit code 0/1 (`phi-test.mjs:251-266`). Runner: `run-fx-all.sh:44-54` sftp's harness + suites to `C:\Users\jdxin\phitest`, restarts via `PhiFxLaunch`, sleeps 12 s, runs each suite with `node`, ORs `$LASTEXITCODE`, prints `ALL SUITES PASSED`/`SOME SUITES FAILED`. No report file, no JUnit/JSON, no timing beyond per-test ms, no artifact capture on failure (the pixel scripts throw away the bitmap after sampling, `uicap.ps1:70`). Model tests: `CHECK` macro increments `g_failures`, `main` returns 1 with `N FAILED` (`sidebar_model_test.cc:305-310`); `run.sh` uses `set -e` (`:5`) so the first failing binary stops the others. Probes: stdout only.

---

## 3. Host-side testability

The four sidebar modules prove the pattern: `.cc` files that include only their own header plus STL (`phi_sidebar_model.cc`, `_layout.cc`, `_drop.cc`, `_actions.cc` — 1–3 includes each), headers with no `<windows.h>`/`base/`, compiled by host g++ with `-std=c++20 -Wall -Wextra -Werror` (`test/model/run.sh:7`). Everything else in `client/` includes `<windows.h>` in the header (46 of 58 headers) and/or `base/*` in the `.cc`.

Ranked by value (behavioural risk × ease):

| # | Module | State today | What blocks a host build | Refactor size | Value |
|---|---|---|---|---|---|
| 1 | **Tab store serialization + migrations** `phi_tab_store.{h,cc}` (757 lines) | Header is STL-only (`:8-11`); `.cc` uses `base::JSONReader/Writer`, `base::Uuid`, `<windows.h>` (`:7,13-18`). `Load` folds schema-1 split pairs (`:186`) and persists `legacyTagAliases` — exactly the migration logic the design doc wants host-tested, and `docs/sidebar-architecture.md §8:5-6` promises a header-only `phi_tab_store_codec.h` that **does not exist** (`ls` fails). | Split codec: `Serialize(rows)->string` / `Parse(string)->rows` on a tiny JSON shim (or a `base::Value` adapter behind an interface), file I/O + uuid injected. | Medium (1 day) | Highest: the store is the one on-disk format with a schema number and a migration; a regression here loses users' pins. |
| 2 | **Control-server framing + WS handshake** `phi_control_server.cc:115-255,479-499` | `WsTextFrame` (`:115-132`), `RequestPath` (`:150-161`), `WsReadMessage` (`:203-255`) and the accept-key computation (`:490-499`) are pure byte logic but read directly from a `HANDLE` via `OvRead`. | Refactor to a `WsDecoder` that consumes a `std::string_view` and yields frames (state machine), plus `MakeAcceptKey(key)`; the pipe loop feeds it. SHA-1/base64 need a host impl (or `<openssl>`; a 40-line SHA-1 is acceptable for test). | Small–medium | High: this is the security/robustness boundary every agent talks through (pre-release C bucket in the status doc); fragmentation, 64-bit lengths, masked/unmasked, close/ping opcodes are exactly the things a fuzz/unit test catches. |
| 3 | **Verb parsing / dispatch** `phi_coordinator.cc:4691-6013` | A 128-arm string chain inside a 9,199-line file; payload shape validation is ad hoc per arm. | Introduce a verb table `{name, handler, required fields}` in its own TU; a pure `ValidatePayload(verb, dict)` step; host test that every verb in the table appears in `docs`/skill and that unknown/missing-field errors are uniform. Full handler logic stays Win32. | Medium | High: makes the contract enumerable (the suites and the skill currently carry it implicitly) and lets `suite-core "unknown verb"` become a table test. |
| 4 | **URL rules matching** (`phi_coordinator.h:512-525 struct UrlRule`, `LoadUrlRules/SaveUrlRules`; routing table pushed to the framework) | Not a module — a struct and two methods inside the coordinator; the match itself lives in the fork's `PhiURLRouter`. Client-side rule id generation (`g<gen>-<index>`, regenerated on write per memory) and JSON persistence are pure. | Extract `phi_url_rules.{h,cc}`: rules vector, id policy, JSON codec, and the `HostMatches`-style predicate used for credentials (`phi_coordinator.cc:7494`, `PhiCredentialVault::HostMatches`). | Small | Medium–high: user-visible routing surprises are cheap to pin with a table test; `PhiCredentialVault::HostMatches` is a security predicate. |
| 5 | **i18n catalog + locale resolution** `phi_i18n.cc` | Header pure. `TagEquals` (`:102`), `MatchLocale` (`:118`), `ResolveActive` (`:153`), `PhiStrF` placeholder substitution (`:322`) are pure; file read, `GetUserPreferredUILanguages` (`:66`) and `base::JSONReader` are not. | Split `phi_i18n_resolve.{h,cc}` (pure functions over `vector<string>`), inject the loader. Test the zh-Hant/zh-HK/zh-MO → zh-Hant rule documented in `docs/i18n.md:74-77` and `{0}` substitution. Also a host test that `strings.json` == generator output (today verified manually: regenerated == checked-in). | Small | Medium: nine locales, documented edge cases, zero tests. |
| 6 | **Theme derivation** `phi_theme.cc` | `Blend` (`:22`), `PhiResolvePalette` (`:120-167`), `PhiParseColorHex` (`:168`), built-in tables (`:66-97`) are pure over `COLORREF` (a `DWORD`); only `PhiSystemPrefersDark` (`:106`) touches the registry (`base/win/registry.h:10`). | Typedef the colour type, move the registry probe out. Suites currently test this in pixels on the box (`suite-theme.mjs:65-91`); a host test would assert the palettes directly. | Small | Medium |
| 7 | **Split-drop geometry** `phi_split_drop.h:48-66` | `PhiSplitCreateZoneAt`, `PhiSplitReplaceZoneAt`, `PhiSplitReplacePaneRects` are pure over `RECT/POINT`; the header drags `<windows.h>` (`:8`) and the `.cc` drags GDI+ and the window controller (`:10-21`) for the overlay. | Move the three functions to `phi_split_geometry.{h,cc}` with a 4-int rect struct (as `phi_sidebar_layout` already does). | Trivial | Medium: hysteresis ("retained zone") and corner precedence are classic off-by-one territory. |
| 8 | **Feedback zip + bucketing** `phi_feedback_zip.{h,cc}` | Header pure (`:12-46`). `.cc` depends on `third_party/zlib` and one `base/compiler_specific.h` macro (`:12-13`); `PhiBucketArchiveItems` (`:38-44` in .h) is pure arithmetic. | Link host zlib; test the writer's output with Python's `zipfile` (data descriptors, UTF-8 central dir) and the bucketing against the Mac's cases. | Small | Medium: hand-written zip writer — readers are unforgiving; the suite only checks names (`suite-feedback.mjs:111-114`). |
| 9 | **Agent Space lifecycle / TTL sweep** `phi_agent_spaces.{h,cc}` | Header includes `base/values.h` (`:14`); `.cc` uses `base::Uuid`, string conversions. Keep-alive TTLs and ownership transitions are pure state machines. | Replace `base::Value` in the header with plain structs; inject the clock. | Small–medium | Medium |
| 10 | **Registries** `phi_agent_drive_registry.{h,cc}`, `phi_surface_registry.{h,cc}` | Both `.cc` files are already STL-only. `phi_agent_drive_registry.h:8` includes `<windows.h>` and uses **no** Win32 type (grep); `phi_surface_registry.h:47` exposes `HWND hwnd()` on the panel interface. | Delete the include; make `hwnd()` an opaque `void*`/`uintptr_t`. | Trivial | Low–medium (already exercised indirectly by sv-surface) |
| 11 | **Icon mask decode** (`PhiTintedIconBgra` in `phi_icons.cc`; tables in generated `phi_icon_masks.cc`, `phi_folder_icon_masks.cc`, `phi_ntp_art.cc`) | Tables are `constexpr unsigned char[]` RLE with `base::span`/`NoDestructor` wrappers (`phi_icon_masks.cc:16-17`). The RLE decoder is pure. | Generate tables as plain arrays into a header with no includes; test that every RLE expands to exactly N×N bytes (a corrupt table today is a silent buffer overrun caught only by `-fsanitize=array-bounds` trapping at runtime). | Small | Medium: 3,477 lines of generated bytes with no checksum. |
| 12 | **Drag image / geometry** `phi_drag_image.cc` | All GDI+ composition (`:41-260`); little pure geometry to extract. | Not worth it. | — | Low |
| 13 | **Ghost spaces** `phi_ghost_spaces.{h,cc}` | Header pure and the comment at `:20-36` spells out two load-bearing rules (receipt replaces the set; three materialize outcomes); `.cc` is base JSON + `<windows.h>`. | Same codec split as the tab store. | Small | Medium: the rules are documented precisely because they were got wrong once. |

Common prerequisite for all of the above: a tiny host-side JSON shim (or vendoring a header-only JSON) so that codecs do not need `base::Value`, and a house rule that **pure headers include only STL and their own kind** (the sidebar four already obey it; `phi_agent_drive_registry.h:8` shows how easily `<windows.h>` creeps in).

---

## 4. Build and tooling

**`build-client.py`.** Globs `client/*.cc` (`:110-112`); staleness is per-object mtime versus `max(source mtime, newest header mtime)` (`:113-115,129-132`) — touching any header recompiles all 58 TUs, and there is no dependency tracking even though the recipe passes `/showIncludes:user` (the "Note: including file:" lines are filtered out at `:159-161` instead of recorded). Implications: no per-file deps, no test target, no way to build a subset or a second configuration, no hook for clang-tidy/IWYU. On the plus side the client inherits Chromium's flag set from the kit: `/W4 /WX -Wshadow -Wexit-time-destructors -Wthread-safety -fsanitize=array-bounds -fsanitize-trap=array-bounds` (kit `compile_template`), `-DDCHECK_ALWAYS_ON=1` with `-DNDEBUG /clang:-O2` — one configuration only (no debug, no ASan). Compilation is `shell=True` (`:153`) with a thread pool; failures are collected and the script exits 1 before link (`:167-168`). `DEFAULT_OUT` is a personal absolute path (`:39`). Resources copied at `:188-192`; `WinSparkle.dll` staged at `:193-203`.

**Generators in `tools/`.**
- `gen-strings-catalog.py` → `client/resources/strings.json`. Output checked in and **reproducible**: re-running `build()` against the current Mac catalog produces byte-identical content (verified in this review: `regenerated == checked-in: True`, 126,069 bytes). Coverage today: 521 keys, 289 from the Mac, 66 Windows-own, **166 English-only** (`--check` output). Depends on a sibling checkout at `../phibrowser-mac/Resources/Localizable.xcstrings` (`:43-45`, overridable with `--mac-catalog`). Nothing enforces freshness at build or commit time; `build-client.py:42` just copies whatever is checked in.
- `gen-folder-icon-masks.py` → `client/phi_folder_icon_masks.cc` (1,015 lines). Deterministic (no time/random); needs the Mac SVGs. Output and generator are both modified in the working tree, consistently.
- `gen-ntp-art.py` → `client/phi_ntp_art.cc` (339 lines). Deterministic given `pdftocairo` + Pillow (`:20`), i.e. depends on poppler's rasteriser version.
- **Missing generator:** `client/phi_icon_masks.cc:5` says "GENERATED by scratchpad/gen-icon-masks.py" — that script is not in `tools/` nor anywhere under `/home/jx/Phi` (find). 2,123 lines of checked-in bytes are irreproducible.
- `tools/phi-browser-skill/` is a product artefact (the agent skill) that happens to include tests: `scripts/selftest-mirror.mjs` (874 lines) runs entirely against fixtures on any host (`:8` "no Phi and no agents needed"); `selftest.mjs`, `selftest-management.mjs`, `selftest-credentials.mjs`, `selftest-animations.mjs` need a running browser and a local HTTP server (`selftest.mjs:17-20`). None is wired to a runner; the memory notes record "42/42 + 20/20 on the box" as a manual ritual.

**i18n pipeline** (`docs/i18n.md`): well documented (source keys → Mac catalog join → generated catalog; bare labels; `verbatim` titles; runtime resolution order `:74-77`). Gaps: no test for `MatchLocale`; no check that every `PhiStr("key")` in `client/*.cc` exists in `strings-source.json` (a typo falls back to the bare key on screen, `docs/i18n.md:147-149`); no check that `strings.json` is regenerated after `strings-source.json` edits.

**Lint/format/static analysis.** None in the repo: no `.clang-format`, `.clang-tidy`, `.editorconfig`, `package.json`, ESLint, pre-commit, or CI directory (find over the tree). Also **no `.gitignore`**, which is why 18 scratch files sit untracked in `test/`. The JS side has no type checking or lint at all despite ~14k lines in the skill and ~7k in tests.

**Version stamping.** `client/phi_version.h:11-13` is the single source (`0.1.0`, channel `canary` `:18`), consumed by the updater (`phi_updater.cc:146,158`), About (`phi_menu_bar.cc:862`), feedback metadata (`phi_feedback_outbox.cc:572`), and the installer (`installer/make-installer.sh:28-29` greps it). Missing: `client/phi_client.rc` (12 lines) has **no `VERSIONINFO`**, so `Phi.exe` carries no file version for Explorer/crash triage; no build-time stamp (git sha, date) reaches the binary or the feedback metadata.

**Installer (`installer/`).** `make-installer.sh` stages the payload box-side with robocopy (`:54-60`) and runs a portable NSIS on the box (`:64-67`); `make-appcast.sh` emits the WinSparkle feed item with optional ed25519 signature. **Drift bug:** `phi-installer.nsi:60-61` create shortcuts to `$INSTDIR\phi_client.exe` and `:70` uses it as `DisplayIcon`, but the build has produced `Phi.exe` since `build-client.py:179` was changed (Sep 2) and the sync whitelist ships `Phi.exe` (`sync-fx-win.sh:32`). An installer built today produces dead shortcuts. `installer/tools/winsparkle-tool.exe` (423 KB) is a checked-in binary with no provenance note.

**Deploy/run scripts** (outside the repo): `sync-fx-win.sh` (manifest-diff SFTP, hash-verified in the runners `run-fx-all.sh:16-25`), `run-fx-all.sh`, `run-fx-test.sh`. They hard-code the box IP, user, and `C:\Users\jdxin\phitest`; the box-side scheduled tasks (`PhiFxLaunch`, `PhiUiCap`, `PhiDragCap`, `PhiCap`, `PhiRm`) and their `.ps1` bodies are not versioned anywhere except `test/uicap.ps1` — the memory notes exist precisely because they drift.

---

## 5. Proposed target

### 5.1 Test pyramid

**Tier 0 — host unit tests (Linux, seconds).** One runner, one binary per pure module, one assertion macro shared from `test/host/check.h`. Modules in order: the sidebar four (exist), tab-store codec + migrations, WS decoder + accept key, verb table validation, URL rules + `HostMatches`, i18n resolve + `PhiStrF`, theme palettes, split geometry, zip writer + bucketing, RLE mask integrity, ghost-spaces receipt rules. Plus the two JS host tests that already exist and just need wiring: `tools/phi-browser-skill/scripts/selftest-mirror.mjs` and a new `test/host/harness.test.mjs` that runs `phi-test.mjs`'s `Ws` class against a fake socket (fragmentation, 64-bit length, interleaved events).

**Tier 1 — box integration (one command, minutes).** Keep the 19 suites but move ownership of setup/teardown into the runner:
- The runner (not each suite) kills, syncs, verifies the hash, and launches — it already does (`run-fx-all.sh:11-43`). Add: a **fresh `--user-data-dir` per run** (copy a checked-in golden profile or start empty; the harness already takes `--user-data-dir`, `phi-test.mjs:17-22`) so junk Spaces/profiles/pins never accumulate and `sweep-spaces.mjs` becomes unnecessary; a `PhiFxLaunch` variant that accepts the dir.
- Move the 4 restart implementations into one `restartApp()` in the harness and give the harness `beforeEach/afterEach`, a per-test timeout, `test.skip`, a `--grep` filter, and a JSON/JUnit report written next to the log.
- Move `firstWindow`, `waitFor(pred)`, `waitTabs`, `navigateAndSettle`, `normalizeWindow` (close-to-one-tab, park bounds, pin layout/theme), `samplePixels`, and the drag/click rig into `test/lib/` and delete the copies. Replace bare `sleep` with `waitFor` everywhere a predicate exists.
- Explicit ordering: runner takes a list (`suites.txt`) with restart-suites last, instead of `ls`.
- On failure: capture a full PrintWindow PNG plus `agentSpace.window.layout` and `tabs.list` JSON into an artifacts directory and pull it back over scp.
- Promote `sv-drag`, `sv-surface`, `sv-splitcell` to suites (they already assert) once they use the shared rig helper; gate network-dependent tests behind a `PHI_TEST_NETWORK=1` flag.

**Tier 2 — `test/probes/` (ad hoc, out of the suite path).** Everything in the inventory marked P/SU/CL/MT moves here, keeps its name, and gets a one-line header comment; the runner never globs it. Two rules: probes import only from `../lib` and `../phi-test.mjs`, and a probe that grows assertions graduates to a suite.

### 5.2 CI-shaped entry points

- `test-host.sh` (repo root): `test/model/run.sh` generalised — builds every `test/host/*_test.cc` against the listed pure `.cc` files with `-Wall -Wextra -Werror -fsanitize=address,undefined`, runs them all (no `set -e` between binaries; collect exit codes), then `node --test test/host/*.test.mjs tools/phi-browser-skill/scripts/selftest-mirror.mjs`, then `tools/gen-strings-catalog.py --check` and a diff of the regenerated `strings.json` against the checked-in one, and a grep that every `PhiStr("…")` key exists. Zero box, zero network; runnable as a pre-commit hook.
- `test-box.sh [--suite name] [--profile golden|empty] [--network]`: today's `run-fx-all.sh` plus the fresh-profile, ordered list, artifact pull, and JSON report. Exit code = any failure. Host/IP/paths read from one `box.env` file rather than being repeated in four scripts.
- `build-client.py --check-only` (or a separate `lint.sh`): `clang-format --dry-run --Werror` with a checked-in `.clang-format` (Chromium style — the code already follows it), `clang-tidy` on changed files using the kit's compile command as the compilation database (the template is already a full command line, so emitting `compile_commands.json` from `build-client.py` is ~20 lines), and `node --check`/ESLint for `test/` and `tools/`.

### 5.3 Build shape that compiles unit tests on Linux from the same sources

What the sources need:
1. **Pure headers include only STL and other pure headers.** Enforce with a list in `test-host.sh` (`grep -L '<windows.h>\|"base/'` over the pure set) so a stray include fails the host build before it fails a colleague. Fix the two cheap violations now: `phi_agent_drive_registry.h:8` (unused include) and `phi_surface_registry.h:47` (`HWND` → opaque handle).
2. **Codec/IO seams.** Modules that persist JSON (`phi_tab_store`, `phi_ghost_spaces`, URL rules, agent spaces) get a `*_codec.h` that speaks `std::string` in/out and a small JSON shim; the `.cc` that opens files stays Win32.
3. **No `base::` in pure code.** `base/compiler_specific.h` (zip), `base/no_destructor.h` + `base/containers/span.h` (mask tables) are replaceable with STL/`static const`; `base::Uuid` and `base::JSONReader` move behind the seams above.
4. **A tiny compat header** for the handful of Win32 value types that leak into geometry (`RECT`, `POINT`, `COLORREF`) — or, better, the 4-int structs `phi_sidebar_layout.h` already uses.
5. **One source list.** `build-client.py` keeps globbing for the exe, but `test/host/pure_sources.txt` names the host-compilable subset; both `test-host.sh` and a future `compile_commands.json` emitter read it. When the SDK's `clang-cl` is present, `test-host.sh` can additionally compile the pure set with the kit's flags (`/W4 /WX -Wshadow …`) to catch MSVC-only diagnostics on Linux without a link.

---

## 6. Ranked findings

1. **Installer shortcuts point at a binary that no longer exists.** `installer/phi-installer.nsi:60-61,70` reference `phi_client.exe`; the build has produced `Phi.exe` since `build-client.py:179` (docstring `:24`) and the deploy whitelist ships `Phi.exe` (`/home/jx/Phi/sync-fx-win.sh:32`). Same rename drift in `test/uicap.ps1:37` (`Get-Process phi_client`, saved only by the `FindWindowEx` fallback at `:41`). No test or script cross-checks the exe name.
2. **The suites share one mutable profile and pollute it.** `suite-spaces.mjs:32,55,118`, `suite-persist.mjs:38,66,73`, `suite-profiles.mjs:52-55,85` create Spaces/profiles without deleting them; `suite-menu.mjs:129-133` caps at 100 to survive it; `sweep-spaces.mjs` is manual GC; `suite-pinned.mjs:13-23` wipes all pins/bookmarks; `suite-spaces.mjs:224-227` deletes every other Space. The harness already accepts `--user-data-dir` (`phi-test.mjs:17-22`); the runner never uses it.
3. **Runner ignores the one ordering constraint that exists.** `suite-persist.mjs:3-5` "must run last" vs `run-fx-all.sh:8` alphabetical `ls`. Six `Stop-Process -Force` restarts per run (`suite-emoji.mjs:101-119`, `suite-persist.mjs:10-31`, `suite-pinned.mjs:188-206`, `suite-settings.mjs:178-196`).
4. **98 fixed sleeps vs 34 polls in the suites**; five suites poll nothing. Every `sleep(300..900)` after a verb is a latent flake on a slower box (`suite-binding.mjs:35,42,46`, `suite-groups.mjs:49,61,85`, `suite-split.mjs:50,88,95`).
5. **Only ~40% of `test/` can fail.** 26 scripts end in unconditional `process.exit(0)`; the rv-* Reader View work (16 files, 09-03) has no assertions at all, so the reader lifecycle rules spelled out in `rv-overlay2.mjs:1-8` are unprotected. Two untracked probes cannot run as written (`sv-sidebar-look.mjs:5` no import; `sv-reader.mjs:3` absolute `C:\Users\jdxin` import).
6. **Harness duplication.** 15 copies of `firstWindow`, 5 of the PhiUiCap pixel graft, 4 of restart, ~21 of the close-to-one-tab loop, and a second assertion framework in `sv-drag.mjs:13-14`/`sv-splitcell.mjs:9-10`/`sv-surface.mjs:11-12`. The harness has no per-test timeout, hooks, filter, or report format (`phi-test.mjs:237-266`); its usage line `:10` advertises suite loading it does not do.
7. **Verb contract is implicit.** 128-arm `if (type == "…")` chain (`client/phi_coordinator.cc:4697-6013`) in a 9,199-line file; 111 verb strings scattered over `client/*.cc`; no table, schema, or `verbs.list`. Suites, probes, the skill (`tools/phi-browser-skill/scripts/lib/helpers.mjs`, 7,579 lines) and the memory notes each carry their own copy of the contract, including "dialect" quirks (`sp-setup.mjs:34`, `fi-setup.mjs:16`).
8. **Design doc promises a codec header that does not exist.** `docs/sidebar-architecture.md §8:5-6` says the store's JSON codec is split into `phi_tab_store_codec.h` so migrations are testable; there is no such file and the schema 1→2 fold (`phi_tab_store.cc:186`) has no host test.
9. **Irreproducible generated source.** `client/phi_icon_masks.cc:5` cites `scratchpad/gen-icon-masks.py`, which is not in the repo (2,123 lines of bytes). The other two generators are deterministic and their outputs match; nothing in CI would notice if they drifted.
10. **No `VERSIONINFO` in `client/phi_client.rc`** (12 lines, zero matches) — the exe reports no file version; no git sha/date reaches the binary or feedback metadata (`phi_feedback_outbox.cc:572` sends only `0.1.0`).
11. **No lint/format/ignore.** No `.clang-format`, `.clang-tidy`, `.editorconfig`, ESLint, or `.gitignore` (18 untracked files in `test/`). The kit's `/W4 /WX -Wshadow …` flags are the only quality gate and only for the Windows build; host tests use plain `g++ -Wall -Wextra -Werror` (`test/model/run.sh:7`), which catches a different set.
12. **Build has no dependency tracking and one configuration.** `build-client.py:113-115,129-132` recompiles everything on any header touch and discards `/showIncludes` output (`:159-161`); one config (`DCHECK_ALWAYS_ON` + `NDEBUG` + O2) with no ASan/debug variant; `DEFAULT_OUT` is a personal path (`:39`).
13. **Box rig definitions are unversioned.** Scheduled tasks `PhiFxLaunch`/`PhiUiCap`/`PhiDragCap`/`PhiCap`/`PhiRm` and their `.ps1` bodies exist only on the box (only `uicap.ps1` is in the repo); pixel sampling passes parameters through **user-scope environment variables** (`suite-layout.mjs:166-167`) and a `DONE` sentinel file (`:168-172`). The memory notes (`phi-box-capture-rigs.md`) exist because these drift silently.
14. **Network-dependent assertions.** `suite-nav.mjs:42-62`, `suite-pinned.mjs:229-237`, all rv-* (`rv-sites.mjs:7-12` incl. 163.com/zhihu), and `suite-feedback.mjs:115-125`, which asserts the upload *fails* and inverts if the box ever reaches the account service.
15. **Host tests stop at first failing binary.** `test/model/run.sh:5 set -e` with three sequential binaries (`:13-15`); output goes to `/tmp`. Minor, but it is the seed of `test-host.sh` and should aggregate.
16. **Existing host-runnable JS test is orphaned.** `tools/phi-browser-skill/scripts/selftest-mirror.mjs` (874 lines, fixtures only, `:8`) runs on Linux today and is wired to nothing.
