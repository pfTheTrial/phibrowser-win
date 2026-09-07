# Lane 4 — State, persistence, source of truth (PhiBrowser Windows client)

Read-only review, 2026-09-07. Paths are relative to `/home/jx/Phi/phibrowser-win/client/` unless noted; `api.h` = `/home/jx/Phi/chromuim/src/chrome/browser/phinomenon/phi_framework/phi_framework_api.h`. House style = `docs/sidebar-architecture.md` §2–§3 (three axes / three owners, one reconciliation boundary, ids at every boundary, stale-safe by construction).

Headline: the client has **eleven persisted stores written by five different mechanisms**. Exactly three (tab store, telemetry spool, feedback manifests) are crash-safe; the other eight are `CREATE_ALWAYS` + one `WriteFile`, unchecked, written synchronously on the UI thread on every mutation. Only one store carries a schema number. Every store follows "corrupt on read → silently default → next Save overwrites the corrupt file with the defaults", which converts a single torn write into permanent loss of that domain. Identity between runs (Space ↔ window) rests on one client-side map that is overwritten by heuristics ("the first shell backs the default Space", "the next created window is mine"). No component has a shutdown hook.

---

## 1. Persisted-state inventory

All files live in the Chromium user-data-dir (`OnFrameworkReady`, `phi_coordinator.cc:230-239`) except where noted. "In-place" = `CreateFileW(CREATE_ALWAYS)` + one `WriteFile`, return value unchecked, no flush, no rename.

| # | File | Format / schema | Writer (fn:line) | Reader | When written | Atomicity | Corrupt / missing | Secrets |
|---|---|---|---|---|---|---|---|---|
| 1 | `PhiTabData.json` | JSON `{schema:2, pinScope, legacyTagAliases, rows[]}` — **only versioned store** (`phi_tab_store.h:23-26`, `.cc:92,292`) | `PhiTabStore::Save` `phi_tab_store.cc:263-328`; called from 14 mutators (`:174,259,413,463,493,515,527,542,557,574,589,607,648,675,753`) | `Load` `:64-184`; coordinator `SidebarStoredItems` `:1309-1330`, `PinnedRows`/`BookmarkRows` via `StoredRowView` `:1251-1275` | **Every mutation, synchronously**, with `FlushFileBuffers` (`:316`) | **Yes**: `.tmp` + `ReplaceFileW`, `MoveFileEx` fallback (`:305-327`) | Missing → first run (`:71`). Unparseable → `return` with `rows_` empty (`:82`) — **next mutation overwrites the file with the empty set**. Schema 1→2 fold at `:172-175` | none |
| 2 | `PhiSpaces.json` | JSON `{spaces[], defaultSpaceId}`; no schema | `SaveSpaces` `phi_coordinator.cc:839-869` (12 call sites: `:2751,3296,3322,3342,3414,3499,3889,4637,4843,5328,8959,8967`) | `LoadSpaces` `:768-837` | every Space change | **In-place** `:858-864` | Missing → built-in default Space. Unparseable → `return` (`:783`) → in-memory default only → **first SaveSpaces rewrites the file with one Space**; rows in #1 and rules in #3 that name the lost Spaces are stranded | none |
| 3 | `PhiUrlRules.json` | JSON array; no schema | `SaveUrlRules` `:985-1007` | `LoadUrlRules` `:944-984` | every rule edit, and DeleteSpace `:3409-3412` | In-place `:999-1005` | Missing → none. Unparseable → keep in-memory (empty) → overwritten on next write | none |
| 4 | `PhiLayout.json` | `{layoutMode, sidebarWidth}` | `SaveLayoutMode` `:1163-1180` | `LoadLayoutMode` `:1135-1161` | each layout/width change | In-place `:1173-1179` | defaults | none |
| 5 | `PhiTheme.json` | `{themeId, appearance}` | `SaveTheme` `:3780-3794` (from `ApplyTheme` `:3796-3799`) | `LoadTheme` `:3745-3778` | each theme change | In-place `:3786-3792` | defaults (unknown id normalised `:3765`) | none |
| 6 | `PhiPrefs.json` | flat dict + `reader` + `shortcuts` sub-dicts; no schema | `SavePrefs` `:2851-2893`; also `OnReaderStyleChanged` `:2233-2235` | `LoadPrefs` `:2788-2849` | every setting change | In-place `:2886-2892` | missing keys keep defaults (`value_or`) — the only store that is field-tolerant | none |
| 7 | `PhiAgentGrants.json` | `{always:[lowercased image paths]}` | `SaveAgentGrants` `:745-765` | `LoadAgentGrants` `:715-743` | each "Always allow" | In-place `:756-763` | silently empty | authorization decisions (paths only) |
| 8 | `PhiCredentialGrants.json` | `{grants:{scope:"fill"/"reveal"}, audit:[tsv lines]}` | `SaveCredentialGrants` `:7226-7251` (also from every audit append `:7185`) | `LoadCredentialGrants` `:7190-7224` | every grant / every audit line | In-place `:7242-7249` | silently empty | plaintext audit of which agent touched which credential scope |
| 9 | `PhiGhostSpaces.json` | `{spaces:[{spaceId, profile, windowId}]}` | `PhiGhostSpaces::Save` `phi_ghost_spaces.cc:62-86` via `NoteLiveWindow` `:88-104` | `Load` `:21-60`; `EagerWindowIds`/`SpaceForWindowId`/`Parked` | whenever a Space's live window id changes | In-place `:77-84` | Missing/corrupt → `remembered_` empty → cold start **not armed** (`phi_coordinator.cc:2119`) — safe fallback | none |
| 10 | `PhiAuth.dat` | DPAPI(`{refreshToken, user{sub,name,email,picture}}`) | `PhiAuth::Persist` `phi_auth.cc:646-660`; `SignOut` deletes `:636-643` | `PhiAuth::Load` `:355-398` | on sign-in, on refresh-token rotation, on id_token refresh — **from worker threads** (`:577-579`, `:623-631`) | In-place `:170-181` | undecryptable → ignored (`:367-370`) | refresh token — DPAPI-protected (current user), OK |
| 11 | `PhiCredentials.dat` | DPAPI(JSON array) **or plaintext JSON** | none in client (dev seed / helper) | `PhiCredentialVault::Load` `phi_credentials.cc:66-113`; only when the Bitwarden helper is absent (`phi_coordinator.cc:266-269`) | — | — | plaintext accepted silently (`:78-84`) | **passwords; plaintext is accepted as a first-class format** |
| 12 | `phi_telemetry.json` (config) | `{analytics_endpoint, analytics_key, analytics_format}` | external | `PhiTelemetry::Start` `phi_telemetry.cc:245-262` | — | — | invalid → pipeline off | API key plaintext (config) |
| 13 | `phi_telemetry_spool.json` | `{batch:[…]}` | `WriteSpool` `:634-668`, called only from the upload loop `:399,406,410` and `Stop` `:338` | `LoadSpool` `:580-632` | after each batch attempt; **`Stop()` has no caller** (grep) | Atomic (`WriteFileAtomic` `:199-217`) | 8 MB cap `:187` | none |
| 14 | `phi_telemetry_id` | bare GUID | `EnsureDistinctId` `:483-505` | same | once | atomic | regenerated | pseudonymous id |
| 15 | `PhiFeedbackOutbox/<sub>/<job>/{manifest.json, images/, files/, logs/, prepared/}` | pretty JSON manifest with `status/retryCount/nextAttemptAtMs`, `archiveStrategyVersion` (`phi_feedback_outbox.cc:395-417`) | `Enqueue` `phi_feedback_outbox.cc:1418-1520` (**UI thread**), worker `:1528+` | `ReadManifest` `:419+`, worker scan | on submit; each attempt | Atomic manifest (`:114-137`); attachments copied with `CopyFileW` | "uploading" left by a crash is retried (only `submitted` skipped, `:1576`) | description + contact e-mail + system logs, plaintext under user-data-dir |
| 16 | `%TEMP%\PhiFeedbackPreviews\<uuid>-name` | PNG | `WritePreviewFile` `phi_feedback_window.cc:142-168` | previewer | on paste | in-place | deleted on close `:338,658` (not on crash) | pasted screenshots in TEMP |
| 17 | `CDPAgentSocket` (pointer file) | one line: pipe name | `PhiControlServer::Start` `phi_control_server.cc:305-315` | agents | at start | in-place | stale after crash (pid-scoped pipe) | none |
| 18 | `chrome_debug.log` | Chromium log | Chromium | outbox attaches it `:39,799` | — | — | — | URLs/paths |
| 19 | Chromium-owned: session files (tabs, `custom_value` tags, ghost parked windows), `browser.window_placement`, `SessionStartupPref`, locale | Chromium prefs/session | framework | client via `ListTabsJson`, `RestoredFromWindowId`, `GhostWindowsJson` | Chromium's cadence | Chromium's | Chromium's | — |
| — | WinSparkle updater | registry (`HKCU\Software\<app>\WinSparkle`) is written **by WinSparkle**, not the client (`phi_updater.cc:91-102,139-159`) | — | — | — | — | — | none |

Not persisted at all (in-memory only, lost on exit): agent Spaces incl. ones flagged `persistent` (`phi_agent_spaces.h:55`), shadow windows, `expanded_bookmarks_` (`phi_window_controller.cc:6172,6246,7256`), `tab_mru_`, `space_order_`, sidebar width per slot, `credential_timed_grants_` (by design, `phi_coordinator.h:157-159`), `session_allowed_peers_`, the last-active Space (see §3), and window placement (Chromium's).

**Schema versioning:** only #1. **Migrations:** only #1 (`FoldLegacySplitPairs`). **Backups/quarantine of a corrupt file:** none anywhere.

---

## 2. Truth ownership per domain, cached copies, heuristic reconciliation

| Domain | Truth today | Client copy that can go stale | Reconciled by |
|---|---|---|---|
| Windows (browsers) | Framework (session ids) | `windows_` map keyed by **slot** = id of the first browser (`phi_coordinator.h:469`), `browser_to_slot_` (`:481`), per-controller `space_browsers_`, `browser_hwnds_`, `space_order_`, `active_browser_` (`phi_window_controller.h:1466-1470`) | Callbacks by id (good), but "which slot" uses `windows_.begin()` = **lowest window id** at 7 sites (`phi_coordinator.cc:1124,3583,3841,4651,5717,7629,8157`) |
| Space ↔ browser binding | **Client only** (`space_browsers_` + `PhiGhostSpaces.remembered_`) | ghost file | Shell ctor **unconditionally binds the default Space** to any new non-agent, non-incognito browser and records it as that Space's window (`phi_window_controller.cc:1370-1385`); `NoteLiveWindow` overwrites the previous id (`phi_ghost_spaces.cc:88-104`) |
| Tabs / strip order / active | Framework | `tabs_`, `active_tab_url_`, `active_tab_id_`, `sidebar_tree_` rebuilt per `RebuildTabs` from one `ReadStrip` (`phi_window_controller.cc:6365-6422`) — good; but `ListTabsJson` is re-parsed ad hoc in 7 other functions (`:2275,4783,4809,6824,6884,7692,8162`) and in the coordinator (`RemoveStoredRow :2037`, `ApplyLegacyTagAliases :2378`, `TabRowsOf :2243`) | ids |
| Groups / splits | Framework (`TabGroupsJson`, `SplitViewsJson`) | none persisted | ids |
| Pinned rows / bookmarks | **Two truths**: row in `PhiTabData.json` (client) + `custom_value` tag in Chromium's session (`api.h:433-438`) | — | `BuildSidebar` by guid (`phi_sidebar_model`); the two are written **non-atomically**: `StoreTabsById` adds the row then tags (`phi_coordinator.cc:1430-1445+`), `RemoveStoredRow` saves the removal then clears tags by re-listing every browser (`:2030-2050`). A crash between leaves an orphan tag; `legacyTagAliases` (`phi_tab_store.cc:257-261`) exists precisely to patch a previous instance of this |
| Spaces list / default role | Client (`spaces_`, `default_space_id_`) | — | `DefaultSpaceId()` recomputed on each call (`:3423-3431`) |
| Profiles | Framework | `known_profiles_before_` snapshot | **Timer poll**: 500 ms `SetTimer(…,1,500)` diffing `ListProfilesJson` against a before-set, 20 s timeout (`:4279-4312`, `:4534-4552`, `:4579-4597`) even though `OnProfileCreated` exists (`:422-427`, used only to release the harness thread) |
| Restore-on-startup | **Both**: `restore_last_session_` in PhiPrefs *and* each profile's `SessionStartupPref` | — | Client value force-pushed onto every *loaded* profile at start and on change (`:3176-3195`); later-loaded profiles depend on "the paths that load them" remembering to push |
| Display language | **Both**: client catalog + Chromium locale pref | — | reconciled once at startup (`:270-274`) |
| Peek prefs | Client file; framework holds a pushed copy (`api.h:625-640`) | — | push on change |
| Window placement | Chromium `browser.window_placement`; client owns nothing | — | Shell anchored to the restored browser rect with a "<300×200 visible → distrust" heuristic (`phi_window_controller.cc:1215-1236`); the memory note's agent-window poisoning is this seam |
| Lazy restore (parked windows) | **Both**: Chromium session ghost store + client `remembered_/parked_` | `parked_` | Receipt replaces `parked_` (`phi_ghost_spaces.cc:116-152`); `remembered_` is never pruned; `GhostWindowsJson` (`api.h:642-646`, offered so a restarted client can "re-read the store instead of trusting its own records") is used **only by the harness verb** (`phi_coordinator.cc:8536`) |
| Agent tasks / shadow windows | Client memory | — | `last_agent_window_id_` "last created" (`:6135-6141,6792-6798,7612`); `pending_agent_task_` re-entrancy flag |
| Peek / Reader surfaces | Client `PhiSurfaceRegistry` + framework markers readable only inside `OnBrowserWindowCreated` | registry | ids; orphan sweep deferred via `WM_PHI_READER_SWEEP` (`:8043-8048`) |
| Downloads / extensions | Framework | popovers query live | — |
| Auth session | `PhiAuth` (UI-thread-owned per `phi_auth.h:102-108`) | outbox copy via `SetAccount` | see §4 |

**Pending-attach heuristic ("the next window created is the one I asked for").** `pending_slot_/pending_space_/pending_open_url_/pending_open_urls_` is a single global slot set by `OnOpenLinkInSpace` (`:1129-1132`) and `SwitchSlotToSpace` (`:8184-8186`, `:8220-8221`) and consumed by whichever browser `OnBrowserWindowCreated` sees next (`:7639-7672`). A materialized ghost is only matched by `RestoredFromWindowId` when `pending_slot_ == 0` (`:7622`), so a ghost landing while a pending attach is armed is bound to the *pending* Space.

---

## 3. Startup and shutdown

**Cold start** (order established by `api.h:49-55` "OnFrameworkReady … before any other callback"):

1. `OnFrameworkReady` (`phi_coordinator.cc:175-291`): telemetry start; message window; hooks; **all Loads synchronously on the UI thread** (`:232-262`): ghost, spaces, rules, layout, theme, prefs, grants, tab store; push restore pref/peek prefs/language; control server; `PhiUpdater::Init`; `auth_.Load` (spawns `RefreshThread`, `phi_auth.cc:397`); outbox start.
2. `OnColdStartRestorePlan` (`:2113-2131`): armed iff `remembered_` non-empty; eager set = `EagerWindowIds(LastActiveSpaceId())`. **`LastActiveSpaceId()` at cold start is always `DefaultSpaceId()`** (`:2100-2111`) — the header's "the one that was active when the last run ended" (`phi_coordinator.h:622-623`) is not persisted anywhere. Every cold start therefore rebuilds the *default* Space's window and parks the one the user was actually in.
3. `OnColdStartParkedWindows` per profile (`:2133-2151`): wraps the bare list and `ReplaceFromReceipt`. Orphans are **not** dropped here (only the reopen path `OnSessionRestoreSettled :2574-2590` calls `DropGhostWindow`), and the API's `eagerFilterMatchedNothing`/`restoredAny` flags are never read (grep: no hits).
4. `OnBrowserWindowCreated` (`:7529-7681`): kiosk → reader → peek → agent/shadow → ghost (`RestoredFromWindowId > 0 && pending_slot_ == 0`, attached to `windows_.begin()`) → pending attach → new shell. A new shell **claims the default Space and overwrites the ghost record** (`phi_window_controller.cc:1373-1385`), so an unfiltered restore of N windows, a tear-off, or Ctrl+N each rebinds the default Space's remembered window id to the newest shell.
5. `RestoredFromWindowId == -1` (session restore's stand-in fallback, `api.h:606-608`) is treated like 0.

**Shutdown:** there is none. `~PhiCoordinator` only unhooks (`:128-136`); no `WM_ENDSESSION`/`WM_QUERYENDSESSION` handler anywhere (grep across coordinator, controller, kiosk, main); `PhiTelemetry::Stop` and `PhiFeedbackOutbox::Stop` have no callers. `WM_CLOSE` routes every Space browser through Chromium's close (`phi_window_controller.cc:11473-11483`, `RequestClose :5729-5750`); the controller dies from `OnBrowserWindowClosing` (`phi_coordinator.cc:8062-8149`). Because every store is write-through, exit loses only: the telemetry queue since the last batch attempt, and any in-flight in-place write (which loses the *whole* file, §6.1).

**Ordering assumptions / races worth naming**
- Space delete never touches the ghost store (`DeleteSpace :3352-3432` vs the ghost call list `:233-8200`): the deleted Space's `remembered_` entry survives, `SpaceForWindowId` can later match its old window id and `AttachSpaceBrowser` a browser to a Space that no longer exists; its parked window is never `DropGhostWindow`ed.
- `SetSpaceProfile` reads the old browser's URLs into `pending_open_urls_` and relies on the *next* `OnBrowserWindowCreated` to reopen them (`:3455-3470`, `:7657-7663`); a materialized ghost or another window arriving first eats them.
- `DeleteSpace` retreats each slot via `SwitchSlotToSpace`, which may itself arm `pending_slot_` for a cold profile, then closes browsers synchronously — the pending attach can outlive the loop.
- `OnColdStartRestorePlan` is "synchronous, called from inside the replay" (`api.h:210`) yet `NoteLiveWindow` writes the ghost file from inside the same replay's `OnBrowserWindowCreated`.

---

## 4. Concurrency

**File I/O on the UI thread (all of it, by design):** every Load/Save in §1 rows 1–9; the tab store's `FlushFileBuffers` on every pin/bookmark mutation (`phi_tab_store.cc:316`); `Enqueue` copying attachments and writing PNGs (`phi_feedback_outbox.cc:1467-1493`); lazy 200 KB catalog reads (`phi_emoji.cc:28-33`, `phi_i18n.cc:46-49`). The comments say raw Win32 is used *because* `base::ReadFileToString` DCHECKs on the browser UI thread — the assertion is being sidestepped rather than honoured.

**Threads:** control-server listener + one thread per connection (`phi_control_server.cc:317,376,681`) marshalling to the UI via `SendMessage(WM_PHI_CONTROL)` (`phi_coordinator.cc:321-329`, `:388`); telemetry uploader (`phi_telemetry.cc:294`), outbox worker (`phi_feedback_outbox.cc:1362`), auth listener + refresh (`phi_auth.cc:397,463`). Telemetry/outbox/control are correctly locked (`CRITICAL_SECTION`, `base::Lock`).

**Shared without a lock — PhiAuth.** `phi_auth.h:102-108` declares `signed_in_/user_/refresh_token_` "UI-thread-owned; worker threads hand results back through the message window before it changes", but:
- `ListenerThread → CompleteSignIn` writes `user_`, `refresh_token_`, `access_token_`, `signed_in_` and calls `Persist()` (`phi_auth.cc:574-579`) without `refresh_lock_`.
- `RefreshThread → RefreshSession` writes `refresh_token_`/`user_` and `Persist()`s (`:622-631`), then may call `SignOut()` which clears all four and **deletes PhiAuth.dat** (`:613-616`, `:636-643`).
- Meanwhile the UI reads `signed_in()`/`user()` in `SyncFeedbackAccount` (`phi_coordinator.cc:4009-4011`) and the settings pane; the outbox worker calls `AccessTokenBlocking` (`:325-335`).
Two writers of the same file/fields on different threads, one of them a delete: a torn `PhiAuth.dat` or a signed-in UI with an empty token is reachable.

**Other cross-thread state:** `profile_create_pending_`/`created_profile_id_` (event-synchronised, `phi_coordinator.h:660-671`), `pending_replies_` (locked). `volatile bool` is used as a flag in three places (`phi_auth.h:105`, `phi_control_server.h:113`, `phi_coordinator.h:678`) — not a memory barrier.

---

## 5. Proposed target

**One application model.** `PhiAppModel` (client, UI-thread only) = the document the UI renders:

```
AppModel {
  Settings   settings;      // prefs, layout, theme, restore, language, shortcuts (today #4,#5,#6)
  Spaces     spaces;        // list, default role, url rules (#2,#3)
  TabStore   rows;          // pins/bookmarks (#1)
  Restore    restore;       // per-Space {profile, last_window_id, parked}, last_active_space (#9 + the missing field)
  Consent    consent;       // agent grants, credential grants + audit (#7,#8)
  Runtime    runtime;       // NOT persisted: windows/slots, surfaces, agent tasks, pending attaches
}
```

**One persistence layer.** `PhiStore<T>`: `Load(path) → {T, migrated}`; `Save(T)` = serialize → `.tmp` → `FlushFileBuffers` → `ReplaceFileW` (the existing `PhiTabStore::Save` body, `phi_tab_store.cc:305-327`, hoisted); every file gets `{"schema": N, …}`; `Load` runs `Migrate(from, to)` steps and, on parse failure, **renames the bad file to `<name>.corrupt-<ts>` and logs** instead of silently defaulting — so the next Save cannot clobber evidence. Saves are coalesced on a posted turn (`ScheduleSave(domain)` → one write per burst), never inside a callback that runs inside Chromium's replay/observer walk. Writes stay on the UI thread but become at most one per domain per turn (today a split pin is three fsyncs, `phi_coordinator.cc:1430-1445`; alias cleanup is one per alias, `:2398-2400`).

**Explicit truth per domain** (write it in the header of each store):

| Domain | Owner | Rule |
|---|---|---|
| tabs, order, groups, splits, active, window ids | framework | client never persists; reads by id only |
| pins/bookmarks rows | client store | tag in Chromium is a *pointer to* the row; write row first, tag second, and make `BuildSidebar` treat an unknown-guid tag as plain (already does) **and** ask the framework to drop the "excluded from groups" rule for unknown guids |
| Space ↔ window | client `Restore` | written **only** from `AttachSpaceBrowser`-with-a-Space and from `RestoredFromWindowId` matches; a new shell binds the default Space only if no other shell holds it; `DeleteSpace` erases the record and calls `DropGhostWindow` |
| last active Space | client `Restore` | written on every Space switch (coalesced) — the missing input to `OnColdStartRestorePlan` |
| restore-on-startup, language, peek prefs | client `Settings` | framework holds a pushed mirror; push on profile load is the framework's job (`OnProfileLoaded` → client pushes), not "the paths that load them" |
| placement | Chromium | keep; but agent/shadow/kiosk windows never save placement (already fixed in the fork) |
| auth | `PhiAuth` on UI thread | workers return results via `PostMessage`; only the UI thread mutates fields or touches `PhiAuth.dat` |

**Startup/shutdown state machine** (coordinator-owned enum, logged on every transition):
`Booting → StoresLoaded → RestorePlanned(armed?) → Restoring → Settled → Running → Closing(slots pending) → Flushed`.
- `RestorePlanned` requires `StoresLoaded` (today implicit via callback order).
- `Settled` fires on `OnSessionRestoreSettled` **or** after the last `OnColdStartParkedWindows` + first `OnBrowserWindowCreated`; orphan ghosts are dropped in both paths.
- `Closing` handles `WM_QUERYENDSESSION`/`WM_ENDSESSION` on the message window: flush coalesced saves, `PhiTelemetry::Stop`, `PhiFeedbackOutbox::Stop`.

**Mapping of existing stores:** #1 → `rows` (already the model file); #2+#3 → `spaces` (one file `PhiSpaces.json` v2 with rules inside, since DeleteSpace already edits both); #4+#5+#6 → `settings` (one file); #7+#8 → `consent`; #9 → `restore` (+ `lastActiveSpaceId`); #10 stays DPAPI but single-writer; #11 rejects plaintext unless a `--phi-dev-vault` switch is present; #13–#15 keep their own atomic writers (they are already correct and thread-owned).

---

## 6. Ranked findings (data-loss risk first)

1. **Eight stores are non-atomic and self-clobbering on corruption.** In-place `CREATE_ALWAYS` writes with unchecked results: Spaces `phi_coordinator.cc:858-864`, URL rules `:999-1005`, layout `:1173-1179`, theme `:3786-3792`, prefs `:2886-2892`, agent grants `:756-763`, credential grants `:7242-7249`, ghost `phi_ghost_spaces.cc:77-84`, auth `phi_auth.cc:170-181`. A crash/power cut mid-write leaves a truncated file; on the next launch the loader returns silently (`LoadSpaces :783`, `LoadUrlRules :963`, …) and the next Save overwrites the truncated file with defaults. For `PhiSpaces.json` this deletes every Space and strands their rows in #1 and rules in #3. The tab store has the correct writer but the same read-side clobber (`phi_tab_store.cc:82` → `rows_` empty → first mutation `Save()` writes `rows: []`).

2. **PhiAuth is mutated from three threads with one partial lock** (`phi_auth.cc:574-579, 613-616, 622-643` vs the ownership comment `phi_auth.h:102-108`). Reachable outcomes: `Persist()` racing `SignOut()`'s `DeleteFileW` on `PhiAuth.dat`; UI reading a half-updated `user_`; two concurrent refreshes on a rotating tenant invalidating each other (the lock covers `RefreshSession` but not `CompleteSignIn`).

3. **Space ↔ window identity is overwritten by the "first shell = default Space" rule** (`phi_window_controller.cc:1373-1385` + `phi_ghost_spaces.cc:96-103`). Any second shell (unfiltered restore of N windows, tear-off, Ctrl+N) rebinds the default Space's remembered window id; the next cold start arms the wrong eager id and parks the user's real window. Same class as the memory note's placement poisoning: two writers, one slot, last-writer-wins.

4. **Last-active Space is not persisted**; `LastActiveSpaceId()` returns the default Space at cold start (`phi_coordinator.cc:2100-2111`) despite `phi_coordinator.h:622-623`. Every cold start reopens the default Space and parks the one the user quit in.

5. **DeleteSpace leaks ghost identity** (`:3352-3432` never calls `ghost_spaces_.*` or `DropGhostWindow`; `remembered_` has no erase API, `phi_ghost_spaces.h:37-96`). The deleted Space's window id keeps matching in `SpaceForWindowId` (`:7624`) and its parked window is replayed as a loose window by the next unfiltered restore.

6. **Cold start never drops orphans and ignores the receipt flags**: `OnColdStartParkedWindows :2133-2151` only replaces; `eagerFilterMatchedNothing`/`restoredAny` unread (grep). An armed set whose ids no longer exist yields a session with **no window** and no client reaction (`api.h:184-190` describes exactly this case).

7. **Row/tag double truth written non-atomically** (`StoreTabsById :1430-1445`; `RemoveStoredRow :2030-2050` saves then re-lists *every* browser to clear tags). Crash or a browser that is mid-close between the two leaves orphan tags; the fork's "excluded from tab groups" rule keys on the tag, so those tabs silently lose grouping until a manual repair. `legacyTagAliases` (`phi_tab_store.cc:257-261`, `phi_coordinator.cc:2373-2408`) is the scar from the previous occurrence.

8. **Single global pending-attach slot** (`pending_slot_/pending_space_/pending_open_url(s)_`, set `:1129-1132, 8184-8186, 8220-8221`, consumed `:7639-7672`, ghost path gated on it `:7622`). Overlapping Space switches, a materialization landing during a pending attach, or `SetSpaceProfile`'s reopen list (`:3455-3470`) bind to the wrong Space or lose the URLs.

9. **Profile creation reconciled by polling and set-difference** (`:4279-4312`, `SetTimer(…,1,500)` at `:4552,4596`), 20 s timeout, while `OnProfileCreated` (`:422-427`) is already delivered. Two creations in flight are indistinguishable.

10. **Write amplification on the UI thread**: fsync'd tab-store write per mutation (`phi_tab_store.cc:316`), three per split pin, one per alias in a loop; credential-grants file rewritten per audit line (`:7185`); Spaces file rewritten on icon/saturation/rename (`:3296,3322,3342`). Not loss, but each is a synchronous disk round-trip inside a framework callback.

11. **Feedback `Enqueue` does attachment copies on the UI thread** (`phi_feedback_outbox.cc:1467-1493`); large attachments stall the shell. The worker side is correct (atomic manifest, retry of `uploading`).

12. **Telemetry spool written only from the uploader; `Stop()` has no caller** (`phi_telemetry.cc:338,399-410`). Events since the last batch attempt are lost at exit. Low value, but it is the one store whose loss is *guaranteed* by the missing shutdown path.

13. **`PhiCredentials.dat` accepts plaintext silently** (`phi_credentials.cc:78-84`), and `PhiCredentialGrants.json` audit lines / `phi_telemetry.json` API key are plaintext. Acceptable for dev boxes; make the plaintext path opt-in before any external install.

14. **No schema numbers outside #1; no corrupt-file quarantine anywhere; no shutdown handler anywhere** (`~PhiCoordinator :128-136`; no `WM_ENDSESSION` in any window proc). These three are what §5's `PhiStore<T>` and the state machine exist to fix.
