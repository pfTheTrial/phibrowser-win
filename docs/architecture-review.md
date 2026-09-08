# Windows client architecture review

Status: REVIEW (2026-09-07); PHASE 0 LANDED the same day (see §5 "Phase 0
status"). The review text below describes the tree as it was before phase 0.
Scope: `phibrowser-win/client` (116 files, 63,043 lines) and the shared
framework header `phi_framework_api.h`. The Chromium side of the framework
and the Mac client were not read; the header itself is fair game because the
product is unreleased and the Mac twin would be updated in step. The Mac is
not the target: where Win32 favours a different shape, this document takes
it.

Seven reviewers read the tree in parallel, one lane each. Their full reports,
with every claim cited by `file:line`, are the appendices in
`docs/architecture-review/`:

| Lane | Appendix | Read in full |
|---|---|---|
| 1 God objects | `lane1-god-objects.md` | `phi_window_controller`, `phi_coordinator`, `phi_settings_window` |
| 2 Framework contract | `lane2-framework-contract.md` | `phi_framework_api.h` and every consumer |
| 3 UI substrate | `lane3-ui-substrate.md` | every `RegisterClass`, wndproc, paint path |
| 4 State and persistence | `lane4-state-persistence.md` | every file the client reads or writes |
| 5 Control plane and agent trust | `lane5-control-agent.md` | control server, verb dispatch, credentials, the skill |
| 6 Tests and tooling | `lane6-tests-tooling.md` | `test/`, `tools/`, `build-client.py`, `installer/` |
| 7 Cross-cutting | `lane7-cross-cutting.md` | i18n, theme, logging, telemetry, errors, hygiene, gating |

This document is the synthesis: what the lanes agree on, the one target
architecture they add up to, the merged ranked findings, and a migration
order. The nine highest-severity claims were re-verified at the cited lines
before this was written. `docs/sidebar-architecture.md` is the house style
and the yardstick throughout; the sidebar is the one subsystem that already
has the shape this document asks the rest of the client to take.

---

## 0. Summary

Five structural facts explain almost every finding:

1. **Two god objects hold the application.** `PhiWindowController` (11,489
   lines, 19 responsibility clusters, 8 window procedures) and
   `PhiCoordinator` (9,199 lines, 19 clusters, six interfaces, a 1,355-line
   verb ladder) own state, policy, I/O, painting and dispatch together. The
   crash-class bugs live at their seams: references held across modal loops,
   controllers destroyed under their own stack frame by pipe verbs, singletons
   pointing at dead shells, and one-slot "pending" globals that bind the
   wrong window (lane 1 §3, §5).
2. **The framework contract is a string bus.** 27 of 108 framework methods
   carry JSON; `ListTabsJson` alone is parsed at 29 sites in 7 files into four
   different row structs; five fields the client depends on are absent from
   the header; window-creation intent leaks through four validity-windowed
   getters plus three client globals; the threading contract is one sentence
   and the client owns five posted-turn messages to work around it (lane 2).
3. **Every window is hand-rolled.** 33 window classes in 24 files, four
   registration idioms, five ways of binding `this`, ten copies of the
   double-buffer block, four copies of the custom caption frame, seven easing
   loops, zero accessibility, zero RTL, no OS theme-change listener. Model
   rebuilds still run inside `WM_PAINT` in three places (lane 3).
4. **Persistence is nineteen files, five mechanisms, three of them safe.**
   Eight client stores are `CREATE_ALWAYS` plus one unchecked `WriteFile` on
   the UI thread per mutation; every loader silently defaults on a corrupt
   file and the next save overwrites it. One torn write of `PhiSpaces.json`
   deletes every Space. The last-active Space is never persisted. There is no
   shutdown hook anywhere (lane 4).
5. **The agent surface has a consent gate with a second, open door.**
   Chromium is started with `--remote-debugging-port=0`, so any local process
   gets full DevTools over every profile with no prompt. A standing credential
   grant plus the agent-supplied `allowCrossOrigin` flag types a password into
   any page silently. The user-space toggle the skill documents does not exist
   on Windows; developer mode gates nothing (lane 5).

Everything else (i18n at 35 % coverage, 143 hard-coded colours, no telemetry
consent, environment variables that replace the update signing key, a test
folder where 60 % of scripts cannot fail, an installer whose shortcuts point at
a binary that no longer exists) is real but follows from the same lack of a
spine: no application model, no service layer, no UI base class, no
persistence layer, no verb registry.

**Release blockers** (§4, items 1 to 8) total roughly one engineer-week and
must land before any external build. The structural work (§5) is roughly
twelve to sixteen engineer-weeks, phased so that every step ships.

---

## 1. The client today

| Measure | Value | Where |
|---|---|---|
| Client source | 63,043 lines, 116 files | `client/` |
| Largest two files | 11,489 and 9,199 lines | `phi_window_controller.cc`, `phi_coordinator.cc` |
| Framework virtuals | 28 delegate + 108 framework; 27 carry JSON; 5 have no caller | `phi_framework_api.h` |
| `JSONReader::Read*` sites | 155; 107 in the coordinator; 3 log on failure | lane 2 §2, lane 7 §4 |
| `ListTabsJson` parse sites | 29 in 7 files, 4 row structs, 6 mini-parsers | lane 2 §2.1 |
| Window classes | 33 in 24 files; `this` bound five ways | lane 3 §1 |
| Files handling `WM_NCDESTROY` | 4 of 24 | lane 3 §5 |
| Persisted files | 19; 3 written atomically; 1 carries a schema number | lane 4 §1 |
| Control verbs | 107 string-compared, four layered handlers, 45 payload re-parses | lane 5 §2 |
| `RefreshTabs()` call sites | 88 synchronous vs 12 posted | lane 1 §2.1 |
| Framework `bool` results ignored | 187 fire-and-forget calls | lane 7 §4.1 |
| i18n keys used | 181 of 521 | lane 7 §1.2 |
| Hard-coded `RGB(` outside the theme | 143 | lane 7 §2.2 |
| Test scripts that can fail | 19 suites + 3 model tests + 3 probes of 104 files | lane 6 §1 |
| Accessibility, RTL, tooltips, `WM_SETTINGCHANGE` | none | lane 3 §2 |

The sidebar rebuild is the counter-example: one store, one reconciliation
boundary (`BuildSidebar`), pure layout, a command type for every mutation,
rebuild on a posted turn, host tests. It took the same code base from "works
on the box" to "has a model". This review asks for that pass across the
rest.

---

## 2. Findings by lane

Each subsection is the lane's conclusion in a page. Evidence and the full
argument are in the appendix.

### 2.1 God objects (lane 1)

`PhiWindowController` is nineteen things: shell lifecycle and browser
hosting (~1,250 lines), drag and drop (~1,700), painting (~1,600), every
context menu (~1,450), the sidebar pipeline (~1,300), omnibox (~720), hover
and cards (~650), layout (~560), caption band (~420), plus split helpers,
native pages, reader affordance, Space-switch animation, the floating
sidebar. `PhiCoordinator` is nineteen more: the control-verb dispatch alone
is ~2,300 lines, stored rows ~1,100, agent Spaces ~1,050, credentials ~700,
Spaces and prefs ~600 each. `PhiSettingsWindow` is a sound immediate-mode UI
whose only debt is its seam: a 67-virtual host passing tab-separated string
rows, and a static singleton the model pokes 36 times.

Coupling: `PhiMenuBar` is a `friend` touching 17 private members and calling
the private rebuild from `WM_INITMENUPOPUP`; 13 satellites hold a raw
`PhiWindowController*`, four of them process-wide `static instance`
singletons; slot id and first-browser id are the same number, patched at 32
sites with `active_browser_ ? active_browser_ : window_id_`; the coordinator
reads the controller's paint cache (`tabs_`) as if it were the model, which
is why Ctrl+W is a silent no-op on a pin-bound tab.

Re-entrancy: the posted-rebuild rule from the sidebar doc is applied in
exactly one place (`OnTabsChanged`). Elsewhere `RefreshTabs()` is synchronous
at 88 sites; `RebuildTabs()` runs from input handlers, from paint of the
floating sidebar, and from the menu bar's friend access; `ComputeLayout()` is
a side-effecting getter (writes `dpi_`, creates fonts) called six times per
mouse move.

Crash class, verified:

- Context menus hold `const TabRow&` across `TrackPopupMenu`
  (`phi_window_controller.cc:7831`, `:8051-8060`). A posted rebuild
  dispatched inside the menu loop clears `tabs_` and the reference dangles.
- Pipe verbs arrive by cross-thread `SendMessage`
  (`phi_coordinator.cc:379-389`), are serviced inside any modal loop, and
  `agentSpace.close` or `spaces.delete` can erase the controller whose
  context-menu frame is on the stack.
- `PhiAgentOverlay`, `PhiGroupOverview` and `PhiTabSwitcher` singletons keep
  a controller pointer after the shell dies; `PhiAgentOverlay::Reposition()`
  runs from every shell's `LayoutContents`.
- One global set of `pending_slot_/pending_space_/pending_open_url(s)_`
  is consumed by whichever browser is created next.

Decomposition: `PhiShellWindow` (~1.5k lines) owning `PhiBrowserHost`,
`PhiOmniboxSession`, `PhiCaptionBand`+`PhiMenuBar` (no friend),
`PhiSpacesStrip`, `PhiSidebarView`+`PhiSidebarDragSession`,
`PhiNativePagesHost`, `PhiFloatingSidebar`, and pure `phi_shell_menus`
(`MenuSpec` in, `MenuAction` by key out). `PhiCoordinator` becomes a
composition root over `PhiFrameworkDispatcher`, `PhiWindowRegistry`,
`PhiSpaceManager`, `PhiUrlRouter`, `PhiPreferences`, `PhiSidebarActions`,
`PhiSurfaceCoordinator`, `PhiSessionRestore`, `PhiAgentSpaceManager`,
`PhiCredentialService`, `PhiProfileService`, `PhiControlDispatcher`.
`PhiSlotHost` (73 virtuals) splits into five narrow host interfaces.

### 2.2 Framework contract (lane 2)

The delegate side is mostly typed (22 of 28); the pull side is not. Every
state query returns a scratch JSON `const char*` valid until the next call.
The client depends on `unloaded`, `surface`, `originTabId`, `incognito` (on
`WindowProfileJson`, the only incognito signal) and `danger` (downloads), none
of which are in the header. There are seven id spaces (slot, window, tab,
split, group, CDP target, profile) with no newtypes; tab ids are `int` in 12
signatures and `int64_t` in 6; window ids are `int` in five places.

`OnBrowserWindowCreated(window_id, hwnd)` carries no intent. The client
reconstructs seven window kinds from `CreatedWindowIsKiosk`,
`CreatedWindowPeekOpener`, `CreatedWindowReaderOrigin`, `RestoredFromWindowId`
(each valid only while `Browser::Create` is on the stack), two client
globals that depend on synchronous re-entrant delivery, one undocumented JSON
field, and the absence of all of the above (`phi_coordinator.cc:7529-7692`).

Threading: the header says "everything on the UI thread, synchronously" and
nothing else. The client has learned that `OnTabsChanged` fires mid-mutation
and navigation callbacks arrive inside observer walks, and owns five
posted-turn messages to escape; the agent-window path depends on the
opposite. `FaviconForUrlBgra`'s async completion arrives as an anonymous
`OnTabsChanged` that triggers a full sidebar rebuild.

Recommendation: one versioned `WindowSnapshot` struct (tabs, splits, groups,
nav, profile) pulled via `Snapshot()` and pushed via
`OnWindowChanged(snapshot, reason)` after commit; a `WindowCreation` struct
with a `WindowKind` enum and a request token replacing the `Created*`
getters; `WindowId/TabId/SplitId/GroupId/TargetId/ProfileId` newtypes in a
shared header; a four-rule threading contract. JSON stays for the agent
dialect, analytics, extension messages, receipts, omnibox and tab search.
Migration order: ids, threading, snapshot, creation; each step ships.

### 2.3 UI substrate (lane 3)

Inventory: 42 create sites, 33 registered classes. Registration is done four
ways, none checks `ERROR_CLASS_ALREADY_EXISTS`, none unregisters; eleven
classes spell the class name twice as a bare literal. `this` is bound at
`WM_NCCREATE` (14), after `CreateWindowEx` returns (8, so the initial
`WM_SIZE`/`WM_CREATE` are dropped), via static singleton (8), via a stack
struct (2), or not at all (2).

Paint: `PaintFloatingSidebar` runs `RebuildTabs()` inside `WM_PAINT` and
then invalidates the shell from inside paint
(`phi_window_controller.cc:8825`, `:8840`); `WM_PRINTCLIENT` rebuilds
synchronously (`:11406`); Settings runs `Rebuild()`, a registry probe and
timer arming in `WM_PAINT` (`phi_settings_window.cc:3647`, `:2011-2012`,
`:1845`). Every hover change invalidates the whole shell; the hover card is
re-rasterised through a per-pixel layered commit on every shell frame. The
double-buffer block is copied ten times; nine surfaces paint unbuffered and
two flash on hover.

Input and lifetime: the Space theme panel takes capture with no
`WM_CAPTURECHANGED`, so a lost capture leaves `dragging` stuck and dismissal
suppressed. Two of the three nested `GetMessage` loops run with no owner
disable, no `IsDialogMessage`, the accelerator hook live, and a raw
controller pointer in stack state. Five classes `delete self` in
`WM_DESTROY` so child subclass procs then run against a freed owner. The
shell destroys itself from inside its own `WM_CLOSE` with `GWLP_USERDATA`
never cleared. Timer ids form an unwritten 0x515 to 0x542 cross-file
registry, five files use literal `1`, and the tab preview arms timers on the
shell's HWND.

AA rule violations: region-clipped slider gradient
(`phi_space_theme_panel.cc:257-278`), region-cut agent pill that hosts no
child controls plus a GDI+ rounded fill (`phi_agent_overlay.cc:145-147`,
`:542-552`), region-cut peek chips. Four separate `GdiplusStartup` tokens.

Target: a `PhiWindow` base (atom cache, NCC binding zeroed at
`WM_NCDESTROY`, cached back buffer, DPI only in `WM_DPICHANGED` with a font
set, theme bus on `WM_SETTINGCHANGE`, rect-list hover with dirty-rect
invalidation, per-window timer allocation, RAII capture, UIA provider over
the same rect list). Three families: `PhiPopover` (collapses 11 classes),
`PhiPanel` (12), `PhiSecondaryWindow` (5, retiring three caption-frame
copies). One `PhiAnimator` for the seven easing loops. Direct2D: route the
`phi_dwrite` shape primitives through the `ID2D1DCRenderTarget` it already
owns (two to three days, no call-site changes); do not move window
composition to D2D because of the DComp browser child and the layered-child
z-order contracts.

### 2.4 State and persistence (lane 4)

Nineteen persisted artefacts. Crash-safe: the tab store, the telemetry spool,
the feedback manifests. Not crash-safe, written in place with an unchecked
`WriteFile` on the UI thread per mutation: Spaces, URL rules, layout, theme,
prefs, agent grants, credential grants, ghost Spaces, `PhiAuth.dat`. Only
`PhiTabData.json` has a schema number and a migration. No loader quarantines
a corrupt file; all of them default silently, and the tab store's read side
has the same clobber (`phi_tab_store.cc:82`).

Truth ownership is mixed and partly heuristic. The Space-to-window binding
is client-only and is overwritten by "the first shell backs the default
Space" (`phi_window_controller.cc:1373-1385`); any second shell rebinds the
ghost record so the next cold start parks the user's real window. The
last-active Space is never persisted and `LastActiveSpaceId()` returns the
default at cold start (`phi_coordinator.cc:2100-2111`). `DeleteSpace` never
clears the ghost record or drops its parked window. Cold start never drops
orphan ghosts and never reads the `eagerFilterMatchedNothing` or
`restoredAny` receipt flags. Pinned rows are two truths (client row plus
Chromium tag) written non-atomically. Profile creation is reconciled by a
500 ms poll and set-difference although `OnProfileCreated` exists.

Concurrency: `PhiAuth` fields and `PhiAuth.dat` are mutated from three
threads with one partial lock, contrary to the ownership comment in its own
header (`phi_auth.cc:574-579`, `:613-643` vs `phi_auth.h:102-108`);
`Persist()` can race `SignOut()`'s `DeleteFileW`.

Shutdown: none. `~PhiCoordinator` unhooks; no `WM_ENDSESSION` handler
anywhere; `PhiTelemetry::Stop` and `PhiFeedbackOutbox::Stop` have no
callers.

Target: one `AppModel` (settings, spaces+rules, rows, restore incl.
last-active Space, consent, non-persisted runtime); one `PhiStore<T>` with
the tab store's atomic writer hoisted, a schema field and migration hook in
every file, quarantine of corrupt files, one coalesced write per domain per
turn; an explicit truth-owner table; a `Booting → StoresLoaded →
RestorePlanned → Restoring → Settled → Running → Closing → Flushed` state
machine that finally gives telemetry, outbox and saves a shutdown hook.

### 2.5 Control plane and agent trust (lane 5)

Correction to the brief: there is a pipe SDDL (owner, SYSTEM, admins, remote
rejected), a per-connection consent prompt keyed by peer image path, and a
fill-vs-reveal grant store. The stale header comment says otherwise. The
findings are about what those do not cover.

- **Consent bypass (critical).** `phi_client_main_win.cc:385-389` starts
  Chromium with `--remote-debugging-port=0`. Any local process reads
  `DevToolsActivePort` and gets full CDP over every profile: cookies, DOM,
  navigation, screenshots. The skill's claim that the pipe is the only door
  (`cdp.mjs:14-15`) is false. The operating mask arms only for drive
  commands; reads never arm it.
- **Silent cross-origin password fill (critical).** The origin check
  (`phi_coordinator.cc:7494-7501`) is skipped by the agent-supplied
  `allowCrossOrigin`; the grant check (`:7104-7109`) is keyed on item only. A
  standing or ten-minute fill grant for one site fills any page without a
  prompt.
- **No user-space gate.** `user_space_operations_disabled` exists nowhere in
  the client; `agentSpacesAllowed` is hard-coded true; `profiles.delete`,
  `spaces.delete`, URL-rule rewrites (cookie-jar routing), download
  directory, `menu.click`, and `captureWindow{outPath}` (arbitrary file
  write) are all served to any consented peer. Developer mode gates nothing
  and is agent-settable.
- **Consent identity is the interpreter path.** "Always Allow" persists
  `node.exe`; the `name:` fallback matches any exe so named; the default
  button is Allow; connection threads are unbounded so prompts stack. The
  skill's `X-Phi-Agent-Pid/Capability` headers are never parsed on Windows.
- **Ownership is cooperative.** `agentSpace.takeover` flips user takeover
  back; client verbs (`agentSpace.navigate {windowId:<user window>}`) never
  arm the mask; `taskId` is a free string so any connection completes,
  hands off, or forges "You:" lines in any task's transcript.
- **`credentials.getTotp` serves 2FA codes**; "secret-free fill" is the
  agent's own scrubbing, not something Phi enforces; the plaintext vault has
  no dev gate.
- Pipe squatting (no `FILE_FLAG_FIRST_PIPE_INSTANCE`, predictable name,
  pointer file written before the pipe exists, listener dies on first
  failure); relay back-thread use-after-free on a slow peer; UI thread
  blocked up to 30 s by the vault helper on every `credentials.status`.
- Dispatch: 107 verbs, name collisions resolved by payload shape, five
  response shapes, and an unknown verb resolves as success through the
  skill's shim.

Target: per-launch token in a restricted-ACL pointer file plus peer identity
plus a TTL'd agent capability; a `Principal` per connection with an
agent-space-only default and explicit `kUserSpaceRead/Write`,
`kDestructive`, `kAppSettings`, `kHost` capabilities; one typed verb registry
(name, scopes, capability, param schema, prompts flag, handler); a uniform
`{ok, result | error{code}}` envelope; in-process DevTools transport from the
framework with per-connection target filtering, or per-task browser
contexts; credential grants keyed by (kind, item, page host, principal) with
cross-origin never covered by a standing grant.

### 2.6 Tests and tooling (lane 6)

`test/` holds 104 files: 19 suites with 115 cases (all run by
`run-fx-all.sh`), 3 host model tests, 64 probes and rigs of which 26 end in
unconditional `process.exit(0)`; the entire Reader View probe set has no
assertions; 18 files are untracked scratch and two cannot run as written.
The harness abstracts pipe discovery and a hand-rolled WebSocket client and
nothing else: no timeouts, hooks, filter, or report. Suites re-implement
`firstWindow` 15 times, the pixel-capture graft 5 times, app restart 4 times,
and the close-to-one-tab loop ~21 times. 98 fixed sleeps vs 34 polls. All
suites share one mutable profile; `suite-persist` says "must run last" and
the runner runs alphabetically.

Host-testable candidates, ranked: the tab-store codec and migration (the
sidebar doc promises a `phi_tab_store_codec.h` that does not exist), the
WebSocket decoder and accept key, the verb table, URL rules and
`HostMatches`, i18n resolution, theme palettes, split geometry, the zip
writer, RLE mask integrity, ghost-space receipt rules. Two trivial wins:
`phi_agent_drive_registry.h` includes `<windows.h>` and uses nothing from
it; `phi_surface_registry.h` exposes `HWND` on an otherwise pure interface.

Build: `build-client.py` inherits Chromium's `/W4 /WX -Wshadow` flags but
has no dependency tracking, one configuration, no test target. No
`.clang-format`, `.clang-tidy`, `.gitignore`, or CI config. `phi_client.rc`
has no `VERSIONINFO`. `phi_icon_masks.cc` cites a generator that is not in
the repo. **The installer creates shortcuts to `phi_client.exe`
(`installer/phi-installer.nsi:60-61,70`) while the build has produced
`Phi.exe` since `build-client.py:179`.**

Target: a three-tier pyramid (host units with `test-host.sh`, a box runner
`test-box.sh` that owns fresh-profile isolation, ordering and artifact
capture, and `test/probes/` outside the suite path), plus source rules that
let the pure subset compile on Linux.

### 2.7 Cross-cutting (lane 7)

- **Release-blocking:** `PHI_APPCAST_URL` and `PHI_APPCAST_ED25519`
  environment variables replace the update feed and its signing key with no
  build or mode gate (`phi_updater.cc:113-133`). `PHI_VAULT_HELPER` overrides
  the helper and the client falls back to a plaintext-capable dev vault when
  the helper is missing. The DevTools port and the control pipe always start,
  including harness-only verbs.
- Telemetry has no user consent surface; properties are forwarded
  unscrubbed with a persistent per-install id; `Stop()` is never called.
- i18n: 181 of 521 keys used; Settings has ~130 literals against 5 keys;
  every context menu and both security consent dialogs are English literals
  although the catalog carries the keys; concatenated sentences in six
  files; no plural, number or date formatting; the harvested catalog contains
  an SDDL string, HTTP headers and a registry value name as translatable
  text.
- Theme: 143 hard-coded `RGB(`, eight duplicate blend functions, parallel
  palettes in Settings, popup chrome, transcript and overlay, three surfaces
  frozen on a `pure/light` fallback, no `WM_SETTINGCHANGE`.
- Errors: 187 fire-and-forget framework calls (~60 return `bool`); 155 JSON
  parse sites of which 3 log; zero `CHECK`/`DCHECK` in client code; SDDL
  conversion failure silently falls back to the default pipe DACL.
- Hygiene: `RAW_PTR_EXCLUSION` in 30 files with one written justification;
  nine file-scope mutable globals; 13 dead functions and one declared but
  never defined; five copies of the file-read loop, four writers, two
  divergent `Widen/Narrow`; `<windows.h>` in 37 of 56 headers.
  `phi_client_main_win.cc` diverges from Chromium's copy by 60 lines in four
  blocks with no script to re-apply them.
- Logs carry the Auth0 subject id and page URLs at INFO, and every log line
  ships in the feedback zip with no redaction pass.

---

## 3. Target architecture

### 3.1 Principles

The sidebar doc's §2 principles, extended to the whole client:

1. **One identity type per axis.** `SlotId` and `BrowserId` are distinct
   strong types. No "slot 0 means first window", no
   `active ? active : window_id_`. Framework ids arrive typed.
2. **Delegate callbacks record, post, return.** No framework call, no modal
   UI, no teardown, no synchronous refresh inside a delegate method. One
   `Defer(fn)` on the message window.
3. **Mutations are commands; views are invalidated, never refreshed.**
   Services call `Invalidate(SlotId | All, Reason)`; the shell rebuilds once
   per turn. Synchronous `RefreshTabs()` survives only for tests.
4. **No reference survives a modal loop.** Menus are data (`MenuSpec`) built
   from a snapshot; the result is a `MenuAction` re-resolved by key.
5. **Every HWND has one C++ owner, and that owner is torn down by the shell
   that created it.** No process-wide singleton points at a shell.
6. **Layout is a value.** Pure functions of (rect, dpi, mode, counts), cached
   per turn, host-testable.
7. **Nothing structural in paint.** Paint reads finished rows.
8. **One persistence mechanism.** Atomic replace, schema number, migration
   hook, corrupt-file quarantine, coalesced writes.
9. **Every store has one written truth owner.** Framework, client model, or
   Chromium; never both.
10. **The control plane is a registry with a principal.** No string ladder,
    no anonymous peer, no verb that acts on a user window without a
    capability.
11. **Dev hooks compile out.** A `PHI_DEV_HOOKS` define gates every
    environment override, plaintext path and harness verb; a release-gate
    script asserts they are absent from the binary.

### 3.2 Layers

```
Chromium fork (phi_framework)
   │  typed ids · WindowSnapshot push/pull · WindowCreation with token
   │  four-rule threading contract · JSON only for extensible payloads
   ▼
PhiFrameworkDispatcher  (record + post; the only code that sees raw callbacks)
   ▼
AppModel + services     (coordinator = composition root, ~300 lines)
   WindowRegistry · SpaceManager · UrlRouter · Preferences(+observers)
   SidebarActions · SurfaceCoordinator · SessionRestore · AgentSpaceManager
   CredentialService · ProfileService · AccountAndFeedback · Accelerators
   PhiStore<T> per domain · startup/shutdown state machine
   ▼                                              ▲
PhiShellWindow (per slot)                  PhiControlDispatcher
   BrowserHost · OmniboxSession               Principal per connection
   CaptionBand+MenuBar · SpacesStrip          typed verb registry
   SidebarView+DragSession · NativePagesHost  uniform envelope
   FloatingSidebar · phi_shell_menus (pure)   token + identity + capability
   ▼
UI substrate
   PhiWindow base · PhiPopover · PhiPanel · PhiSecondaryWindow
   PhiAnimator · theme bus · UIA over hit rects · phi_dwrite on D2D
```

### 3.3 Framework contract

Adopt lane 2 §5 in full. The header changes are additive and gated on
`IS_WIN_PHI_CLIENT` until the Mac migrates; the fork fills the struct and
serialises the legacy JSON from the same struct so they cannot drift.

- `phi_ids.h`: `WindowId`, `TabId` (both `int32_t`, matching Chromium's
  `SessionID`), `SplitId`, `GroupId`, `TargetId`, `ProfileId`. Explicit
  constructors; no implicit int conversion.
- `WindowSnapshot { window, version, profile, incognito, nav, tabs[],
  splits[], groups[] }` with `TabSnapshot` carrying `unloaded`, `surface`,
  `origin` as first-class fields. `Snapshot(WindowId, WindowSnapshot*)` to
  pull; `OnWindowChanged(const WindowSnapshot&, TabsChangeReason)` to push,
  delivered after commit, coalesced per window per task. `kFaviconReady`
  replaces the anonymous `OnTabsChanged` from `FaviconForUrlBgra`.
- `WindowCreation { window, hwnd, kind, profile, incognito, opener_window,
  opener_tab, restored_from, request_token }` delivered to
  `OnBrowserWindowCreated`; `RequestBrowser(CreateRequest) → token` replaces
  `NewWindow`, `CreateSpaceBrowser(InProfile)`, `CreateAgentBrowser`,
  `CreateShadowBrowser`. The four `Created*` getters and the re-entrant
  delivery contract are retired.
- Threading contract in the header: every call on the UI thread; delegate
  callbacks are notifications dispatched after commit from a fresh task,
  never inside an observer walk, so any framework method may be called from
  any callback; ASK-shaped callbacks are marked; no callback is delivered
  re-entrantly from inside a framework call.
- Retire the five callerless methods; give every async request a
  correlation token (`CreateProfile` included); make the ~40 index-addressed
  gesture-path calls use the `ById` twins.

Client side: `PhiWindowModel` keyed by `WindowId` holds the last snapshot
plus derived indexes and replaces all 29 parsers; `SidebarInputs` is built
from it in one place; popovers take `const WindowSnapshot&`, not a
`PhiFramework*`.

ABI note: structs containing `std::string`/`std::vector` already cross the
exe/dll boundary (`OnColdStartRestorePlan` takes a `std::vector<int>*`), and
both sides are built by the same toolchain in the same tree, so this is not
a new risk. The Mac bridge marshals to Swift regardless. If a C-only header
is preferred for the Mac, the same structs can be expressed with
`const char*` plus lengths; the important property is one shape parsed
once, not the container type.

### 3.4 Application model and services

The coordinator becomes a composition root. Each service owns one domain,
one store, and one set of invariants (lane 1 §4.3, lane 4 §5):

| Service | Owns | Invariant it enforces |
|---|---|---|
| `PhiFrameworkDispatcher` | nothing | every override ≤ 10 lines and ends in a post; captures `WindowCreation` into a value |
| `PhiWindowRegistry` | `SlotId → PhiShellWindow`, `BrowserId → SlotId`, kiosks, pending creations as `{token, slot, space, urls}` | slot ids are allocated, never a browser id; pending creations are matched by token, never "next window" |
| `PhiSpaceManager` | `spaces_`, default role, per-Space palettes, `PhiSpaces.json` v2 (rules inside) | only writer of the list; publishes `SpacesChanged`; `PushSpaceMenu`/`PushRoutingTable` once per turn |
| `PhiUrlRouter` | rules, routing decisions, the ask prompt (deferred), the two action targets (Incognito, Kiosk) | rule ids regenerated on write; every write stores the canonical pattern; the editor (`PhiUrlRulesWindow`) writes the whole list at once on Save |
| `PhiPreferences` | layout, theme, prefs, shortcuts, restore, peek, language; one `PhiSettings.json` | observers replace the 36 `PhiSettingsWindow::Refresh()` calls; framework mirrors are pushed on `OnProfileLoaded`, not by "the paths that load them" |
| `PhiSidebarActions` | the existing command table plus `tab_store_` | row first, tag second; id-addressed framework calls only |
| `PhiSurfaceCoordinator` | Peek/Reader registry, sweep, dismissal | panels hold `SlotId`/`BrowserId`, not controller pointers |
| `PhiSessionRestore` | ghosts, cold-start plan, receipts, **last-active Space** | Space↔window written only from an explicit attach or a restore match; `DeleteSpace` erases the record and drops the ghost; orphans dropped on `Settled` |
| `PhiAgentSpaceManager` | agent/shadow windows, overlay, transcript, ownership, drive mask | overlay is owned per shell; ownership transitions are enforced, not cooperative |
| `PhiCredentialService` | vault helper, grants, audit, prompts | grant key = (kind, item, page host, principal); helper I/O off the UI thread |
| `PhiProfileService` | async create with tokens, rename/delete, engines, downloads dir | no polling |
| `PhiAccountAndFeedback` | `PhiAuth` (UI-thread single writer; workers post results), outbox | one writer of `PhiAuth.dat` |
| `PhiControlDispatcher` | verb registry, principals | see §3.8 |

`PhiStore<T>`: `Load(path) → {T, migrated}`, `Save(T)` = serialise → `.tmp`
→ `FlushFileBuffers` → `ReplaceFileW` (the tab store's body, hoisted); every
file carries `{"schema": N}`; parse failure renames the file to
`<name>.corrupt-<timestamp>` and logs; `ScheduleSave(domain)` coalesces to
one write per domain per posted turn. Store mapping: tab store → rows;
Spaces + URL rules → one file; layout + theme + prefs → one file; agent
grants + credential grants → consent (DPAPI-wrapped, HMAC'd); ghost Spaces +
last-active Space → restore; `PhiAuth.dat` stays DPAPI, single writer;
`PhiCredentials.dat` rejects plaintext unless `PHI_DEV_HOOKS`.

Lifecycle: `Booting → StoresLoaded → RestorePlanned → Restoring → Settled →
Running → Closing → Flushed`, logged on each transition. `Closing` handles
`WM_QUERYENDSESSION`/`WM_ENDSESSION` on the message window and flushes
coalesced saves, telemetry and the outbox.

### 3.5 Shell window

`PhiShellWindow` keeps the WndProc, DPI, fonts, the per-turn `Layout` value,
focus routing and component dispatch (`OnLayout`, `Paint`, `HitTest`,
`Mouse`, `Timer`, `Command`). It is the only writer of `GWLP_USERDATA` (set
at `WM_NCCREATE`, cleared at `WM_NCDESTROY`), the only `DestroyWindow`
caller for its tree, and it keeps a `modal_depth_` so a `RequestClose`
arriving under a modal loop is deferred to the next turn. Components:

- `PhiBrowserHost`: adoption, per-Space browser HWNDs, placeholder,
  fullscreen, corner caps and status bubble as shaped layers, z-order.
  Exposes the active `BrowserId`; there is no fallback to the slot id.
- `PhiOmniboxSession`: the EDIT, popup, results, nav state; the NTP's field
  attaches here.
- `PhiCaptionBand` + `PhiMenuBar` (no friend; reads the sidebar tree and
  calls `ShellServices`).
- `PhiSpacesStrip`: chips, hover card, add-Space, create-Space form host,
  swap and ramp animation via `PhiAnimator`.
- `PhiSidebarView`: `SidebarInputs → BuildSidebar → LayoutSidebar → rows`,
  paint, hit-test by `SidebarKey`, hover, scroll, MRU. `rows_` written in one
  function on the posted rebuild turn. Answers "which window shows tab X"
  from the tree, `owner_of` included.
- `PhiSidebarDragSession`: exists only between `ArmDrag` and commit/cancel;
  its destructor is `AbortDrag`; owns the drag image and split-drop overlay.
- `PhiNativePagesHost`, `PhiFloatingSidebar` (a second viewport, no rebuild
  in paint).
- `phi_shell_menus.{h,cc}`: pure `MenuSpec` builders and a `MenuAction`
  variant for every context menu; the shell runs a spec through one
  `RunPopupMenu` and applies the action by key against fresh state.

`PhiSlotHost` (73 virtuals) becomes a `ShellServices` struct of five narrow
interfaces: `SidebarHost`, `SpacesHost`, `ChromePrefsHost`, `AgentHost`,
`WindowHost`. `PhiSettingsHost` splits the same way and its tab-separated
rows become structs; Settings rebuilds on invalidation, not in paint.

### 3.6 UI substrate

Adopt lane 3 §6. `PhiWindow` absorbs class registration, `this` binding,
paint framing with a cached back buffer and dirty rects, DPI, theme,
hover over a hit-rect list, timer allocation, RAII capture, Escape/Enter
defaults, `WM_GETDLGCODE`, and a UIA provider over the same hit-rect list
(which also yields tooltips and, later, RTL mirroring). `PhiPopover`,
`PhiPanel` and `PhiSecondaryWindow` collapse 28 of the 33 classes; the
folder picker and Space theme panel become asynchronous popovers with a
completion callback, deleting two of the three nested loops. `PhiAnimator`
replaces seven easing loops with one coalescable timer that invalidates the
animated rect. A `PhiThemeBus` on the message window listens for
`WM_SETTINGCHANGE` and broadcasts `OnThemeChanged`; `PhiPopupColors`
becomes a view over `PhiPalette`; one `PhiBlend`; a `PhiMetrics` token table
with one `PhiDip` scaler.

`phi_dwrite` routes its shape primitives through the `ID2D1DCRenderTarget`
it already creates, keeping every call site and the GDI fallback. Window
composition stays GDI plus layered children; a later DirectComposition
project is possible once `PhiPanel`/`PhiShapedLayer` exist but has no
user-visible payoff over the rasteriser swap.

### 3.7 Control plane and trust

Adopt lane 5 §4. Order of importance:

1. **Close the second door.** Preferred: a framework-provided in-process
   DevTools transport (`PhiFramework::OpenDevToolsSession(filter)` wrapping
   `DevToolsAgentHost` and a `DevToolsAgentHostClient`), which also enables
   per-connection target filtering. Interim if that slips: keep TCP but have
   the fork's socket factory accept only the client's own PID.
2. **Token + identity + capability.** Per-launch 32-byte token in the
   pointer file (restricted ACL, written after the first pipe instance
   exists); `FILE_FLAG_FIRST_PIPE_INSTANCE` and a random suffix; identity =
   image path plus publisher or hash; never persist "Always Allow" for a
   known interpreter; on first consented upgrade mint a TTL'd
   `X-Phi-Agent-Capability` (the skill already parses it) and persist that
   instead; prompt default Deny, coalesced, connection cap, denial memory.
3. **Principal per connection.** `{peer, capability_id, caps, tasks,
   granted_spaces, granted_windows}`. Default after consent is `kAgentSpace`
   only. `kUserSpaceRead`, `kUserSpaceWrite` (the toggle the skill already
   documents), `kDestructive` (per-request prompt, never standing),
   `kAppSettings` (developer mode plus toggle; `developerMode` itself never
   agent-settable), `kHost` (test rig only, refused without a switch),
   `kCredentialFill/Reveal/Totp`. `ResolveScope(principal, payload) →
   {browser, kind, task}` in one place; a verb declared `scope: agent` given
   a user window fails before its handler runs. User-window drive from a
   client verb goes through a framework call that raises the same
   `OnAgentDroveUserSpaceTab` report as CDP does. Lifecycle verbs on a task
   the connection did not create fail `not_owner`; `userMessage` leaves the
   agent-callable set.
4. **Typed verb registry.** `VerbSpec { name, scopes, capability, params
   schema (with aliases), prompts, deferred, handler }` in one table;
   dispatcher = parse envelope → spec → capability → scope → params →
   (prompt mutex) → handler → wrap. Generates `references/verbs.md` and an
   `agentSpace.capabilities` verb. Uniform envelope `{id, ok, result |
   error{code}}` with a closed error-code enum; unknown verb is `ok:false`;
   v1 shape accepted for one release behind `X-Phi-Protocol`.
5. **Credential grants** keyed by (kind, item, page host, principal);
   cross-origin always prompts and is never remembered; `reveal` and `totp`
   never "always"; post-approval origin re-check; grant files DPAPI-wrapped
   and HMAC'd; `getTotp` removed or behind an off-by-default setting; the
   fill prompt says what is true ("any automation attached to that page can
   read it") or the fill becomes an auto-submit the agent cannot read.

### 3.8 Tests and tooling

Adopt lane 6 §5. Tier 0: `test-host.sh` builds every `test/host/*_test.cc`
against `test/host/pure_sources.txt` with `-Wall -Wextra -Werror
-fsanitize=address,undefined`, runs the JS host tests (`selftest-mirror.mjs`
already runs on Linux and is wired to nothing), checks the strings catalog
is regenerated and every `PhiStr` key exists, and enforces that pure headers
include only STL. Tier 1: `test-box.sh` owns kill/sync/verify/launch with a
fresh `--user-data-dir` per run, an ordered suite list, per-test timeouts,
`beforeEach/afterEach`, a shared `test/lib/` for the fifteen-times-copied
helpers, `waitFor` instead of `sleep`, a JSON report, and artifact capture
on failure. Tier 2: `test/probes/` for everything that prints. Add
`.clang-format`, `.clang-tidy` fed by a `compile_commands.json` emitted from
`build-client.py`, `.gitignore`, `VERSIONINFO` with git sha, and
`tools/check-main-diff.sh` for `phi_client_main_win.cc`. Version the box
rig task definitions in the repo.

### 3.9 Cross-cutting conventions

- **Strings:** `PhiStr`/`PhiStrF` as the only path, plus `PhiStrN` for
  plurals and `PhiFormat{Number,Date}`; a literal lint in CI; purge the
  harvested non-UI `app.*` keys; convert `phi_prompt.cc`'s buttons first
  (fixes every alert), then Settings, then the context menus; the two
  security consent dialogs before release.
- **Theme:** `PhiPalette` as the single token set with the roles files
  invent today; per-Space overlay computed in one place; delete the three
  frozen `kFallback`s.
- **Logging:** `docs/logging.md`: INFO for lifecycle only; ERROR carries
  `GetLastError()`; `PhiRedact()` for URLs, ids, emails; one `[PhiModule]`
  tag per module; the feedback zip passes through the same redaction.
- **Telemetry:** `analytics.enabled` pref, default off, surfaced in
  Settings; property allow-list per module; `Stop()` wired to `Closing`.
- **Errors:** `PhiFw::Must(bool, verb, ids...)` around the ~60
  bool-returning framework methods; `CHECK` on ownership pointers; `DCHECK`
  on model invariants inside `BuildSidebar`; `PhiWin32Check` for the handful
  of calls whose failure changes semantics (SDDL, file writes,
  `RegisterClassExW`).
- **Globals:** the coordinator owns the six UI singletons and the popup
  palette; services get explicit `Start/Stop`; `phi_util.h` for the
  duplicated helpers; `RAW_PTR_EXCLUSION` gets one written justification in
  one header or is replaced by `raw_ptr` where it links.
- **Gating:** `PHI_DEV_HOOKS` define; `tools/release-gate.py` asserts no
  `PHI_APPCAST`/`PHI_VAULT_HELPER` strings in the binary, no plaintext vault,
  DevTools port and pipe only when developer mode is on or a grant exists,
  analytics consent present, strings regenerated, main-diff green, no
  URL/sub/email in `LOG(INFO)` arguments.

---

## 4. Ranked findings

Merged across lanes, deduplicated, most severe first. Severity: **R** =
release blocker, **C** = crash or data loss, **S** = security, **D** =
design debt, **Q** = quality. Evidence is one representative citation; the
appendix section has the rest.

| # | Sev | Finding | Evidence | Lane |
|---|---|---|---|---|
| 1 | R/S | Loopback DevTools port bypasses consent; full CDP over every profile to any local process | `phi_client_main_win.cc:385-389`; `phi_control_server.cc:598-623` | 5 §1.2, 7 §6 |
| 2 | R/S | Standing fill grant + agent-supplied `allowCrossOrigin` fills a password into any page unprompted | `phi_coordinator.cc:7434-7436`, `:7494-7501`, `:7104-7109` | 5 §3.2 |
| 3 | R/S | Update feed and signing key replaceable by environment variables in release | `phi_updater.cc:113-133` | 7 §0 |
| 4 | R/S | Plaintext vault accepted; dev-vault fallback when helper absent; `PHI_VAULT_HELPER` override | `phi_credentials.cc:76-82`; `phi_coordinator.cc:250-260` | 5 §3.2, 7 §6 |
| 5 | R/S | No user-space gate; destructive verbs unconfirmed; developer mode gates nothing and is agent-settable | `phi_coordinator.cc:6423`, `:3572-3578`, `:5457-5460`, `:4892-4914` | 5 §3.1 |
| 6 | R/S | Consent identity = interpreter path; default Allow; unbounded prompt spam; claim headers ignored | `phi_coordinator.cc:620-623`, `:645-647`; `phi_control_server.cc:337-391` | 5 §1.3 |
| 7 | R/S | `credentials.getTotp` serves 2FA; "secret-free" fill unenforced | `phi_coordinator.cc:7285-7346` | 5 §3.2 |
| 8 | R | Installer shortcuts target `phi_client.exe`; build produces `Phi.exe` | `installer/phi-installer.nsi:60-61,70`; `build-client.py:179` | 6 §6 |
| 9 | C | Eight stores non-atomic and self-clobbering on corruption; tab store read side too | `phi_coordinator.cc:858-864` and six siblings; `phi_ghost_spaces.cc:77-84`; `phi_tab_store.cc:82` | 4 §6, 7 §4 |
| 10 | C | Context-menu references held across `TrackPopupMenu`; posted rebuild clears the vector | `phi_window_controller.cc:7831`, `:8051-8060`, `:8230`, `:8370` | 1 §3.2 |
| 11 | C | Pipe verbs via cross-thread `SendMessage` serviced inside modal loops can destroy the controller under its own frame | `phi_coordinator.cc:379-389`, `:4732-4737`, `:3402-3404` | 1 §3.3, 5 §1.7 |
| 12 | C | Singletons keep a dangling controller pointer; `PhiAgentOverlay::Reposition` from every shell | `phi_agent_overlay.cc:62-66`, `:124-127`; `phi_window_controller.cc:2657` | 1 §2.2 |
| 13 | C | `PhiAuth` mutated from three threads with one partial lock; `Persist` races `SignOut`'s delete | `phi_auth.cc:574-579`, `:613-643` vs `phi_auth.h:102-108` | 4 §4 |
| 14 | C | Space↔window binding overwritten by "first shell = default Space"; next cold start parks the wrong window | `phi_window_controller.cc:1373-1385`; `phi_ghost_spaces.cc:96-103` | 4 §6 |
| 15 | C | Single global pending-attach slot binds the wrong Space; agent adoption relies on re-entrancy | `phi_coordinator.cc:7639-7674`, `:6134-6141` | 1 §2.2, 2 §3.3, 4 §6 |
| 16 | C | Shell and children never clear `GWLP_USERDATA`; shell destroys itself inside its own `WM_CLOSE`; five classes `delete self` in `WM_DESTROY` | `phi_window_controller.cc:1412`, `:11473-11484`; `phi_search_tabs_panel.cc:616-621` | 1 §2.2, 3 §5 |
| 17 | C | Two modal loops with stack state, no owner disable, accelerator hook live | `phi_folder_icon_picker.cc:252-255`; `phi_space_theme_panel.cc:472-475` | 3 §4.4 |
| 18 | C | Space theme panel loses capture silently; stuck drag | `phi_space_theme_panel.cc:375-397` | 3 §7 |
| 19 | C | Framework re-entered inside delegate callbacks; browser teardown inside a `WH_MOUSE` hook | `phi_coordinator.cc:8073`, `:7652`, `:7695-7737` | 1 §3.1 |
| 20 | C | Relay back-thread use-after-free on a slow peer; listener dies permanently on first pipe failure; pipe squatting | `phi_control_server.cc:680-701`, `:345-349`, `:302-315` | 5 §1.4-1.5 |
| 21 | C | Last-active Space never persisted; `DeleteSpace` leaks ghost identity; orphans never dropped; receipt flags unread | `phi_coordinator.cc:2100-2111`, `:3352-3432`, `:2133-2151` | 4 §6 |
| 22 | C | No shutdown path: no `WM_ENDSESSION`, `Stop()` never called on telemetry, outbox, updater | `phi_coordinator.cc:128-136` | 4 §3, 7 §5.2 |
| 23 | D | `ListTabsJson` parsed 29 times into 4 structs; five depended-on fields absent from the header | lane 2 §2.1 table | 2 |
| 24 | D | Creation intent via four validity-windowed getters plus three client globals; seven kinds reconstructed | `phi_framework_api.h:601-625,1009-1014`; `phi_coordinator.cc:7529-7692` | 2 §3.3 |
| 25 | D | Threading contract one sentence; five posted-turn workarounds; untyped mixed-width ids across seven spaces | `phi_framework_api.h:32-33`; `phi_coordinator.cc:144-158` | 2 §4 |
| 26 | D | Slot id = first browser id, 32 fallback expressions; shell activates the dead original browser | `phi_window_controller.cc:10883`, `:10911`; `phi_coordinator.cc:3842` | 1 §5 D1 |
| 27 | D | Two refresh disciplines (88 sync vs 12 posted); `RebuildTabs` from paint, input, menus; `ComputeLayout` side effects ×6 per mouse move | `phi_window_controller.cc:8825`, `:8863`, `:11406`, `:662-672` | 1 §3.4, 3 §3 |
| 28 | D | Coordinator and menu bar read the paint cache as the model | `phi_coordinator.cc:2224`; `phi_menu_bar.cc:897-904` | 1 §5 D2 |
| 29 | D | Verb ladder: 107 string compares, 45 re-parses, collisions by payload shape, five reply shapes, unknown verb succeeds via shim; the UI calls itself over the agent wire | `phi_coordinator.cc:4664-6018`, `:4314-4322`; `cdp.mjs:566-575` | 5 §2, 1 §5 D8, 2 §2.2 |
| 30 | D | 33 hand-rolled window classes; no `PhiWindow` base; 10 double-buffer copies; 4 caption-frame copies; 7 easing loops; timer ids an unwritten registry | lane 3 §1-§2, §4.1 | 3 |
| 31 | D | Zero accessibility, RTL, tooltips; no OS theme-change listener; open secondary windows keep stale colours | lane 3 §2; `phi_theme.cc:106-131` | 3, 7 |
| 32 | D | Telemetry has no consent surface; PII in `LOG(INFO)` ships in the feedback zip | `phi_telemetry.h:21-33`; `phi_auth.cc:394,580`; `phi_reader.cc:258-259` | 7 §3 |
| 33 | D | `PhiSlotHost` 73 virtuals, `PhiSettingsHost` 67 with tab-separated rows; `friend class PhiMenuBar`; 36 static `Refresh()` calls | `phi_window_controller.h:105-324`, `:650` | 1 §5 D7, D9, D11 |
| 34 | D | Profile creation by 500 ms poll and set-difference; no correlation ids for async framework requests | `phi_coordinator.cc:4279-4312` | 2 §1.1, 4 §6 |
| 35 | D | Test suites share one polluted profile; runner ignores ordering; 98 sleeps vs 34 polls; 60 % of `test/` cannot fail | lane 6 §2, §6 | 6 |
| 36 | D | i18n 35 % wired; consent dialogs and every context menu literal; catalog contains SDDL/HTTP headers as translatable text | lane 7 §1 | 7 |
| 37 | D | 187 ignored framework results; 155 JSON parses with 3 logging; zero `CHECK`; SDDL failure falls back to default DACL | `phi_control_server.cc:333-342` | 7 §4 |
| 38 | Q | AA violations: region-clipped slider, region-cut agent pill with GDI+ fill, four `GdiplusStartup` tokens | `phi_space_theme_panel.cc:257-278`; `phi_agent_overlay.cc:145-147`, `:542-552` | 3 §3.6 |
| 39 | Q | Whole-shell invalidation per hover; hover card re-rasterised per frame; per-paint font/brush churn; DPI drift in transcript, feedback, preview, split-drop constants | `phi_window_controller.cc:4533`, `:10106-10115`; `phi_settings_window.cc:2963-2988`; `phi_split_drop.cc:54,56` | 3 §3 |
| 40 | Q | 143 hard-coded colours; eight blend functions; parallel palettes; 266 unscaled pixel constants | lane 7 §2 | 7 |
| 41 | Q | Hygiene: 13 dead functions, one declared never defined, 9 mutable globals, 5 file-read loops, 4 writers, `RAW_PTR_EXCLUSION` in 30 files with one justification, `<windows.h>` in 37 of 56 headers | lane 7 §5 | 7 |
| 42 | Q | Build: no dependency tracking, one configuration, no lint/format/ignore, no `VERSIONINFO`, irreproducible `phi_icon_masks.cc`, unversioned box rigs | lane 6 §4 | 6 |
| 43 | Q | MRU filter bug drops pane-B ids (`row.id == id` where `row.b_id` was meant) | `phi_window_controller.cc:6760` | 1 §5 C1 |
| 44 | Q | Unchecked `*FindString("name")` deref in downloads popover; missing `index` acts on tab 0 at two mutation sites | `phi_downloads_popover.cc:244`; `phi_coordinator.cc:2263`, `:2443` | 2 §2.3 |

---

## 5. Migration order

Dependency-ordered. Every phase leaves the client building and box-usable.
Sizes are engineer-time estimates from the lanes, rounded.

### Phase 0 — release blockers and crash hotfixes (~1.5 weeks)

Security (lane 5 P0, lane 7 gating):
kill the TCP DevTools door (in-process transport in the fork, or the interim
PID gate); per-launch token in a restricted-ACL pointer file;
`FILE_FLAG_FIRST_PIPE_INSTANCE` + random name + connection cap + listener
retry; prompt default Deny and coalescing; cross-origin never covered by a
standing grant; remove or gate `getTotp`; refuse plaintext vault and dev-vault
fallback outside `PHI_DEV_HOOKS`; `PHI_APPCAST_*` and `PHI_VAULT_HELPER`
behind `PHI_DEV_HOOKS`; a real "Allow agents to operate your Spaces" toggle in
front of the existing ladder; `developerMode` not agent-settable; fix the
installer exe name; add `VERSIONINFO`.

Crash (lane 1 P0, lane 3):
copy row fields to locals before every `TrackPopupMenu`/`PhiPrompt*`;
`HandleControlCommand` → `PostMessage` + the existing `PendingReply`, plus a
`modal_depth_` on the controller that defers `RequestClose`; close every
singleton bound to a shell in its destructor and hide the overlay in the
agent branch of `OnBrowserWindowClosing`; `WM_NCDESTROY` clears
`GWLP_USERDATA` everywhere; `WM_CAPTURECHANGED` in the theme panel; make
`PhiAuth` single-writer (workers post results); persist last-active Space;
`DeleteSpace` clears the ghost; use the active browser at
`phi_window_controller.cc:10883/10911` and `phi_coordinator.cc:3842`; fix the
MRU filter and the downloads deref.

Persistence quick win: hoist the tab store's atomic writer into
`PhiWriteJsonFileAtomic` and use it for all eight in-place stores; quarantine
on parse failure. (Full `PhiStore<T>` comes in phase 3.)

**Phase 0 status (2026-09-07, uncommitted).** Everything listed above
landed, plus: the in-process DevTools transport in the fork
(`phi_devtools_transport.{h,cc}`, four `PhiFramework` virtuals), the control
server rewritten around it with a per-launch token (`X-Phi-Token`, line 2
of the pointer file), `phi_file_io.{h,cc}` and `phi_modal_scope.{h,cc}`,
`kPhiDevHooks` (`build-client.py --dev-hooks`), `tools/release-gate.py`
(passes), a `cancel_is_default` mode on `PhiPromptChoose`, and
`NoteSlotActiveSpace` so chip clicks persist the last-active Space. Fill is
treated as reveal for grants; `profiles.delete` is test-rig-only and needs
`confirm:true`. Box sweep (18 suites, feedback skipped): 13 pass; the
remaining failures (menu placeholders enabled, layout caption offset at
125 % DPI, cross-profile Space switch, emoji's hard-coded Space id) were
reproduced byte-for-byte on the pre-phase-0 client and are profile-state or
pre-existing, not regressions. The box runner now seeds the test-rig prefs
(`seed-fx-prefs.sh`), caps each suite at 300 s and dumps the client log on
a kill. The lane 5 P1 items (principal per connection, capability bitset,
typed registry) start in phase 4.

Phase 0 consequence to carry forward: because pipe verbs are now parked
while any modal loop runs, an unattended dialog freezes the agent surface
until a person dismisses it. That is the right behaviour for a user's
machine and the wrong one for the box: the feedback suite drives the form
into its "only regular files can be attached" notice on purpose and the
sweep stalled there (2026-09-07). `run-fx-all.sh` skips that suite for now.
Phase 1 should give the rig a dismissal path (a `PhiPrompt` verb served
outside the deferred queue, or notices that are not modal) rather than
letting suites depend on someone at the desk.

### Phase 1 — identity and contract foundations (~2 weeks)

`phi_ids.h` on both sides (mechanical); `PhiWindowRegistry` with allocated
`SlotId`s and token-matched pending creations (fixes the pending-attach
class); the four-rule threading contract written into the header and the
fork posting `OnTabsChanged` after commit; `WindowCreation` +
`RequestBrowser(token)` added alongside the old getters. Verify on the box
against the reader sweep and drag paths that today assume mid-mutation
delivery.

**Phase 1 status (2026-09-07, uncommitted).** Landed: `phi_ids.h` on both
sides (`WindowId`, `TabId`, `RequestToken`; client `SlotId`/`BrowserId`);
the threading contract written into the header (rules 1 to 5) with the
fork delivering `OnTabsChanged` after commit from a posted task, coalesced
per window; `WindowKind`/`WindowCreation`/`CreateRequest` and
`RequestBrowser(…) → token`, never announcing before it returns; the client
`OnBrowserWindowCreated` is a switch on kind, every creation goes through
`RequestBrowser` with a token-keyed `PendingCreation` (no legacy creator has
a caller left), and `OnProfileCreated` replaces the 500 ms poll;
`PhiWindowRegistry` (`phi_window_registry.{h,cc}`, host-tested) owns
slots, browser bindings, kiosks and pending creations, with one `Resolve`
for wire `windowId`s (slot id, bound browser id, or 0 = first user slot) and
one `FirstUserSlot`; `PhiWindowController` takes `SlotId` + `BrowserId`,
`window_id()` is gone and `active_browser_id()` never falls back to the
slot number (32 controller sites, the menu bar and four satellites);
the shell is registered before its first rebuild (D12). Verb timing changes:
`agentSpace.create`, `shadow.create` and `profiles.create` answer through
the deferred-reply path; cold `spaces.switch`/`activate`/`newWindow` are
always `pending:true`. Verified on the box: sweep identical to the phase-0
baseline (13 of 18 pass, the same pre-existing failures), `sv-surface`
(reader and peek lifecycle under the posted `OnTabsChanged`) and `sv-drag`
(T1 to T3) pass; `sv-splitcell` needs a stored split cell seeded first.
Left for phase 2: the legacy `int64_t`/`int` signatures, the
`WM_PHI_READER_*` deferrals (now coalescing only), and `PhiSlotHost`'s
`int64_t slot` parameters.

### Phase 2 — refresh discipline and the snapshot model (~2 to 3 weeks)

`Snapshot()`/`OnWindowChanged` in the fork; `PhiWindowModel` in the client;
migrate `ReadStrip` first, then the coordinator helpers, then popovers, and
delete the 29 parsers and ~40 index-addressed calls; replace every
`for (windows_) RefreshTabs()` with a posted `Invalidate`; remove
`RebuildTabs()` from paint, input and menu paths; `ComputeLayout` becomes a
pure `phi_shell_layout` value cached per turn (host-tested); the menu bar
reads the tree. The `WM_PHI_READER_*` deferrals become direct calls once
rule 2 holds.

**Phase 2 status (2026-09-07, uncommitted).** Landed: the typed
`WindowSnapshot` (tabs, splits, groups, nav, profile, incognito, version)
built by ONE builder in the fork that also serialises the legacy JSON
getters, pushed as `OnWindowChanged(snapshot, reason)` after commit with a
per-window version and a change reason (tabs / navigation / groups /
splits / favicon-ready / bulk), pullable via `Snapshot()`; id-addressed
twins for every remaining index operation (`OpenTab → TabId`,
`MoveTabToWindowById`, `MoveTabsToNewWindowByIds`, `UnsplitById`,
`AddTabToNewGroupById`, `RemoveTabFromGroupById`, `SwapTabIntoSplitById`);
the client's `PhiWindowModel` (`phi_window_model.{h,cc}`, host-tested)
owned by the coordinator and read by every controller and panel — all 29
tab-list parsers are gone (two wire pass-throughs remain by design) and
zero index-addressed framework calls remain; every refresh is a posted
`ScheduleRefresh` (the coordinator's 46 synchronous loops and the
controller's ~35 are gone, rebuilds never run from paint, input or menus,
the rebuild invalidates only changed bands, hover invalidates old∪new
rects); `ComputeLayout` is a pure `phi_shell_layout.{h,cc}` value cached
per turn and host-tested, `dpi_` written only at construction and
`WM_DPICHANGED`; `WM_PHI_READER_SWEEP` removed (`EXPAND`/`RETIRE` stay for
rule-2 reasons). Two contract details found on the box: a tag write is
snapshot state, so the fork's `SetTabTag` now marks the window dirty and
the coordinator pulls the model after every tag write (the gesture's own
posted rebuild can precede the push); and the `tabs.move` verb keeps its
"split member refused" contract on top of the anchored move. Verified:
sweep identical to the baseline (13 of 18, same pre-existing failures),
`sv-surface` and `sv-drag` ALL PASS, `sv-splitcell` needs a stored split
seeded. Decision §6 item 2 is settled: the snapshot crosses the exe/dll
boundary with `std::string`/`std::vector`, same toolchain, and the Chromium
style plugin accepts the aggregates. Left for later phases: the legacy
`int64_t`/`int` signatures and JSON getters themselves (remove when the
agent dialect is rebuilt in phase 4), `PhiSlotHost`'s `int64_t`
parameters, `RefreshFieldExtensions`' extension-list JSON (no typed twin).

### Phase 3 — persistence layer, app model, lifecycle (~1 week)

`PhiStore<T>` with schema numbers and migrations; the store consolidation in
§3.4; `AppModel`; the startup/shutdown state machine with
`WM_QUERYENDSESSION`/`WM_ENDSESSION`; orphan-ghost drop and receipt flags on
`Settled`; telemetry consent pref and `Stop()` wiring.

**Phase 3 status (2026-09-07, uncommitted).** Landed: `phi_store.h`
(`PhiStore<T>` over `phi_file_io`: load with quarantine, one-shot import of
the legacy files, schema migration, atomic save; sealed mode = DPAPI +
HMAC-SHA256 keyed by a DPAPI-protected `PhiConsent.key`, any seal failure
fails closed); `phi_app_model.{h,cc}` (`PhiSettings`, `PhiSpacesModel` with
the URL rules inside, `PhiRestoreModel` with the last-active Space,
`PhiConsentModel`; truth owner stated on each; host-tested); the
coordinator owns the four models and stores and nothing else persists —
the fourteen `Load*/Save*` bodies and ~30 scattered members are gone, saves
are coalesced (250 ms, one write per dirty domain, flushed on close);
`PhiGhostSpaces` is a view over the restore model; the lifecycle machine
(`booting → storesLoaded → restorePlanned → restoring → settled → running →
closing → flushed`, logged, one `SettleRestore` for both restore paths,
`agentSpace.ping` reports `phase` and `pendingSaves`); `OnProfileLoaded`
(fork: after `OnFrameworkReady`, before the restore decision and the first
window) carries the per-profile mirror pushes; telemetry is opt-in
(`analyticsEnabled`, default off, Settings › General, never agent-settable);
and a real bug from the fork read: the Windows "restore last session"
setting wrote a per-profile pref that Phi's startup never reads — the
setter now flips the app-wide Local State switch too. Verified on the box:
first launch over the old profile imported all seven legacy files
(renamed `.imported`), migrated the Spaces file to schema 2 and created the
four stores plus the seal key; the lifecycle log shows the transitions; the
sweep is identical to the baseline; `sv-surface` and `sv-drag` ALL PASS.
The harness gained `waitForSaves()` and the four restarting suites call it
before killing the process. Rig note: a wire `windowId` may be a slot id, a
bound browser id or 0 — every verb resolves through `PhiWindowRegistry::
Resolve`; a strict slot lookup crept back into two verbs during phase 2 and
was caught by the rigs.

### Phase 4 — coordinator split and control plane (~3 to 4 weeks)

Extract the services in §3.4 as file moves plus interfaces, no behaviour
change; `PhiPreferences` observers kill the 36 `Refresh()` calls;
`PhiSlotHost` → `ShellServices`; `PhiSettingsHost` typed. In parallel, lane
5 P1 and P2: `Principal` per connection, task ownership, capability bitset,
unified `ResolveScope`, mask-reporting framework call for user-window
drive; then the typed verb registry, uniform envelope, protocol v2 with the
v1 shim, regenerated skill references, and the fix for silent unknown-verb
success in the skill's shim. CDP target filtering or per-task browser
contexts land here if the in-process transport was chosen in phase 0.

**Phase 4 status (2026-09-08, uncommitted).** Landed, control plane first:
`PhiControlDispatcher` took the whole verb surface out of the coordinator,
then the four `if (type == …)` ladders became ONE typed registry —
`phi_verb_registry.{h,cc}`, 142 specs, handlers split across four
`phi_verbs_*.cc` — with one payload parse per request against a declared
`ParamSchema` (aliases declared once), name collisions settled by required
fields rather than payload sniffing, and a uniform envelope: protocol v2
(`{id, ok, result|error{code}}`, closed error enum) selected by
`X-Phi-Protocol: 2`, with a byte-compatible v1 shim whose only change is
that an unknown verb now answers `ok:false` instead of succeeding silently.
`references/verbs.md` is generated from the table and the release gate
fails on a stale one. The trust model underneath: a `PhiPrincipal` per
connection with a capability bitset, an agent capability token minted at
consent and presented by later connections, task ownership (`not_owner`),
ONE `ResolveScope` for every verb, and — new in the fork —
`SetAgentDriveContext` so a verb driving a USER window arms the same
operating mask and reclaim refusal CDP driving has, plus
`SetDevToolsSessionScope` so a session sees only its own windows (default
empty; the rig is unrestricted). Scope enforcement is live:
`scope_not_allowed` before the handler runs. The fork also retired the 24
legacy methods and the two legacy callbacks the client no longer calls.
Services: `PhiPreferences` (observers replaced all 34 static settings
refreshes), `PhiSpaceManager` (coalesced menu/routing pushes),
`PhiUrlRouter` (the ask prompt now deferred off the navigation throttle),
`PhiFrameworkDispatcher` (every delegate override ≤ 10 lines, forward
only), `PhiProfileService`, `PhiSessionRestore` (ghost store folded in),
`PhiSurfaceCoordinator`, `PhiAgentSpaceManager`, `PhiCredentialService`,
`PhiAccountAndFeedback`, `PhiKeyboardAccelerators`; `PhiSlotHost` split
into the five `ShellServices` interfaces (the controller still sees the
union, so its `.cc` is untouched). `phi_coordinator.cc` 9,366 → 3,512.
Verified: sweep identical to the baseline, `sv-surface` and `sv-drag` ALL
PASS, release gate and host tests green. One regression the rigs caught and
fixed: five handlers built a slot from the raw payload `windowId` instead
of the resolved scope, so a Space's own browser id listed no pins.
Left for later: the coordinator's remaining ~2,000 lines are the sidebar
mutator (`PhiSidebarActions`, phase 6's neighbour) and the creation switch;
the controller's `ShellServices` adoption and the typed `PhiSettingsHost`;
vault helper I/O still on the UI thread.

### Phase 5 — UI substrate (~4 weeks)

`PhiWindow` + `PhiPopover` and the 11 popovers (1 week); `PhiPanel` +
`PhiShapedLayer` and the 12 panels (1 week); `PhiSecondaryWindow` and the
five windows, retiring three caption frames (3 to 4 days); `PhiAnimator`
(2 days); theme bus (1 day); `phi_dwrite` on D2D (2 to 3 days); UIA provider
over hit rects for the sidebar, popovers and Settings (1 week). Popovers
first: largest count, smallest risk.

### Phase 6 — controller split (~2 to 3 weeks)

In order of seam cleanliness: `PhiOmniboxSession`, `PhiBrowserHost`,
`PhiCaptionBand` + de-friended `PhiMenuBar`, `PhiNativePagesHost`,
`PhiSpacesStrip`, then `PhiSidebarView` + `PhiSidebarDragSession` +
`phi_shell_menus`. `PhiShellWindow` ends as WndProc plus component dispatch.
Settings rebuilds off-paint with typed host structs.

**Phase 5 status (2026-09-08, committed).** Landed: `PhiWindow` — one
atom-cached registration path, `this` bound at `WM_NCCREATE` and cleared at
`WM_NCDESTROY`, a cached back buffer, DPI written only in `WM_DPICHANGED`
with a font set, palette from a theme bus that listens for the OS
light/dark switch, a hit-rect list driving hover with old∪new invalidation,
per-window timer allocation, RAII capture, and `DeleteWhenDestroyed`. On it:
`PhiPopover` (one anchor-placement implementation replacing five copies, and
the eight standalone popovers), `PhiPanel` (a card-stack z-order list and the
ten panels), `PhiSecondaryWindow` (one caption frame replacing three copies,
and the five windows). Plus `PhiAnimator` for the easing loops, one GDI+
initializer for four, and a Direct2D rasterizer inside `phi_dwrite` behind a
runtime probe that falls back to the software field. Two of the three nested
message loops became async popovers with completion callbacks; the prompt's
is the one that remains, documented. Settings rebuilds off the paint path.
**Accessibility, the review's second-ranked finding, is closed**: a UI
Automation provider reads each surface's hit list, so no surface carries
accessibility code of its own, and it takes a `PhiHitSource` interface that
both `PhiWindow` and the hand-registered shell implement — which is how the
sidebar's rows reach a screen reader.

**Phase 6 status (2026-09-08, committed).** `PhiWindowController` is now
`PhiShellWindow` and went from 13,142 lines to ~5,200: out of it came
`PhiOmniboxSession`, `PhiBrowserHost`, `PhiCaptionBand`, pure
`phi_shell_menus` (a `MenuSpec` built from a snapshot, a `MenuAction`
re-resolved by key — so no reference can survive a modal loop by
construction), `PhiNativePagesHost`, `PhiSpacesStrip`, `PhiSidebarView` and
`PhiSidebarDragSession` (whose destructor IS `AbortDrag`, so a drag comes
down by ownership). `friend class PhiMenuBar` is retired, the settings host
passes typed structs instead of tab-separated rows (75 virtuals down to 46,
setters addressed by identity rather than list position), and the combined
`PhiSlotHost` is replaced by the five `ShellServices` interfaces, each
component holding only what it calls. Verified on the box (2026-09-08): the sweep matches the
baseline exactly (13 of 18; the four long-standing failures are suites
fighting over one profile, not product bugs), `sv-surface` and `sv-drag`
both report ALL PASS, and the 660-point fingerprint is identical to phase 4
at 655 points — the five that differ all sit on one live tab row whose
content differs between captures, so the deliberate antialiasing changes
moved nothing the grid samples. The pixel suites (theme, tab UI, settings
saturation, toolbar) pass, which is the real check on the Direct2D swap.

### Continuous — tests, tooling, cross-cutting (alongside phases 1 to 6)

`test-host.sh` with the codec, WebSocket, verb-table, URL-rule, i18n, theme,
geometry, zip and RLE tests as each seam is created; `test-box.sh` with fresh
profiles, ordering, `test/lib/`, artifacts; `test/probes/`; `.clang-format`,
`.clang-tidy`, `.gitignore`, `compile_commands.json`; `check-main-diff.sh`;
`release-gate.py`; the string facade lint and the i18n conversion (prompt
buttons, consent dialogs, Settings, menus); `PhiPalette` tokens and
`PhiBlend`; `docs/logging.md` and `PhiRedact`; `PhiFw::Must`; `phi_util.h`.

Total: roughly 15 to 18 engineer-weeks after phase 0, of which the
framework-side work (transport, snapshot, creation token, posted callbacks)
is about 2 weeks and must be scheduled with whoever owns the fork.

---

## 6. Decisions needed

1. **DevTools transport.** In-process transport in the fork (recommended,
   2 to 5 days, enables filtering) or the interim PID gate (1 day, weaker,
   still a loopback bind). Phase 0 cannot close without one of them.
2. **Snapshot container types across the exe/dll boundary.** `std::string`/
   `std::vector` (already crosses, same toolchain) or a C-shaped struct with
   `const char*` plus lengths for the Mac bridge's sake. This affects only
   the header sketch, not the design.
3. **Credential fill semantics.** Treat fill as reveal for grant purposes
   (honest, simple), or invest in an auto-submit fill the agent cannot read
   (renderer work). The prompt wording depends on this.
4. **Control protocol v2.** The envelope change touches `cdp.mjs`, which is
   shared with the Mac. Ship v2 behind `X-Phi-Protocol` with a one-release
   v1 shim, or keep v1's double-encoded envelope and fix only the error
   channel.
5. **Direct2D.** Rasteriser swap inside `phi_dwrite` (recommended, 2 to 3
   days) versus leaving the software coverage loops. Full D2D windows are
   not recommended.
6. **Telemetry consent.** An OOBE screen or a Settings row with default off;
   either way the pref must exist before release.
7. **Phase order.** The proposed order puts the framework contract before
   the coordinator split because the split is much cheaper once the client
   holds one typed model. If fork scheduling makes that impossible, phases 3
   and 4 can precede 1 and 2 at the cost of migrating the parsers twice.

---

## 7. What to keep

The lanes also found things that are right and should survive the
migration untouched: the sidebar model and its four pure modules; the tab
store's atomic writer and schema fold; the feedback outbox (atomic
manifests, size caps, retry of `uploading`); the telemetry spool's bounded,
atomic design and https-only endpoint rule; the pipe SDDL with
`PIPE_REJECT_REMOTE_CLIENTS`; consent before both pipe families; the
browser-side reclaim refusal (the one hard enforcement point in the agent
model); fill-vs-reveal separation; the vault helper's process isolation;
bounded pushes with a write timeout; the `WM_NULL` wake in the prompt's
modal loop; the immediate-mode design of Settings; `phi_dwrite` as the one
AA layer; the i18n facade and its generator; the Chromium warning set the
kit inherits; and the memory notes and status logs that made this review
possible.
