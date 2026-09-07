# Lane 2 — the framework/client contract (read-only review)

Scope: `/home/jx/Phi/chromuim/src/chrome/browser/phinomenon/phi_framework/phi_framework_api.h` (1059 lines: `PhiFrameworkDelegate` 28 virtuals, `PhiFramework` 108 virtuals) and its consumers under `/home/jx/Phi/phibrowser-win/client` (63,043 lines; 155 `JSONReader::Read*` sites). Paths below are relative to those two roots unless absolute. `docs/sidebar-architecture.md` §4/§7 was read first and is treated as the house style for proposals.

---

## 0. Executive summary

1. **The contract is a string bus, not an API.** 27 of 108 framework methods return `const char*` JSON or take JSON in; every state query the UI needs (`ListTabsJson`, `NavStateJson`, `SplitViewsJson`, `TabGroupsJson`, `ListProfilesJson`, `WindowProfileJson`, …) is a scratch-buffer JSON string "valid until the next call on this interface" (`phi_framework_api.h:326-327`). The client parses `ListTabsJson` alone at **29 distinct sites** in 7 files, with at least **4 different row structs** (`StripTab` `phi_window_controller.cc:2254`, `TabRow` `phi_window_controller.h:891`, `SidebarLiveTab` `phi_sidebar_model.h:85`, `LiveTabInfo` `phi_coordinator.h:551`) and 5 named mini-parsers (`ReadStrip`, `TabRowsOf`, `LiveTab`, `TabUrlAtIndex`, `IndexOfTagged`, plus the `tabs_of` lambda at `phi_coordinator.cc:8330`). Five row fields the client depends on (`unloaded`, `surface`, `originTabId`, plus `incognito` on `WindowProfileJson` and `danger` on `ListDownloadsJson`) are **not in the header at all** — the schema lives in the fork's serialiser and in the client's `FindX` calls.
2. **Creation intent leaks through four side channels** that are only readable "while `Browser::Create` is still on the stack" (`RestoredFromWindowId` :601-611, `CreatedWindowIsKiosk` :613-618, `CreatedWindowPeekOpener` :620-625, `CreatedWindowReaderOrigin` :1009-1014) plus three **client-side** pending-globals that do the same job in the other direction (`pending_agent_task_`, `pending_shadow_task_`, `pending_slot_`/`pending_space_`/`pending_open_url_` — `phi_coordinator.h:749-771,714`, consumed at `phi_coordinator.cc:7601-7613,7639-7674`). `OnBrowserWindowCreated` (`phi_coordinator.cc:7529-7692`) is a 160-line dispatcher over seven creation kinds reconstructed from these markers.
3. **Threading is documented in one line and violated in spirit by the callbacks themselves.** The header says "Everything runs on the browser UI thread, synchronously" (:32-33) and never says whether the client may call back in. The client has learned the hard way that it must not: `OnTabsChanged` fires *inside* the strip mutation (`phi_coordinator.cc:144-158`, `phi_window_controller.h:498-502,351-354`), reading-and-closing inline "recursed until the stack gave out", and closing a browser from an observer walk trips `!observers_.is_notifying_observers()`. The client now owns **five** posted-turn messages (`WM_PHI_READER_SWEEP/EXPAND/RETIRE`, `kMsgPhiRebuildRows`, `kMsgPhiSyncNativePages`) that exist only to escape the framework's re-entrancy window — and `CreateAgentBrowser`/`CreateShadowBrowser` *rely* on re-entrancy (:539-543, :913-915).
4. **Id spaces are untyped and inconsistently sized.** Tab ids are `int` in 12 header signatures and `int64_t` in 6; window ids are `int64_t` everywhere except `RestoredFromWindowId()`→`int`, `MaterializeGhostWindow(int)`, `DropGhostWindow(int)`, `OnColdStartRestorePlan(std::vector<int>*)`, `RestorePreviousSession(const int*)`. The client mirrors this with `static_cast<int>(tab_id)` at the delegate boundary (`phi_coordinator.cc:2596,2602,2607`) and reads the same JSON `windowId` via `FindDouble` in 28 places and `FindInt` in 1 (`phi_coordinator.cc`). There are **seven** id spaces (slot, browser/window, tab session id, split token, group token, CDP target id, profile basename) plus guid/space id, none typed, and the client's own `slot ≠ browser` distinction (`phi_coordinator.h:395-397`, `ResolveBrowser` :4656) is invisible to the framework.
5. **Recommendation (§5): option (b)+(c)+(d)+(e) together, with (a) limited to the snapshot.** One versioned `PhiWindowSnapshot` struct pushed on `OnWindowChanged(window_id, const PhiWindowSnapshot&, reason)` and pullable via `Snapshot(window_id, PhiWindowSnapshot*)`; a `PhiWindowCreation` struct carried *in* `OnBrowserWindowCreated`; `struct WindowId/TabId/…` newtypes in a shared `phi_ids.h`; and a written re-entrancy contract ("delegate callbacks are notifications: no framework mutation inside them, the framework posts the callback after the strip settles"). JSON stays for the agent dialect, analytics, extension messages and receipts. Migration cost is bounded because every parser in the client already funnels through ~6 helpers on the Windows side and the Mac twin already carries typed Swift models.

---

## 1. Catalogue

### 1.1 `PhiFrameworkDelegate` (framework → client; 28 virtuals)

Legend — *Payload*: T = typed scalars, S = `const char*` opaque string (id/url/text), J = `const char*` JSON. *Mode*: push (framework informs) / ask (framework needs an answer). *Sync*: all delegate callbacks are synchronous on the UI thread by header contract (:32-33); "re-entrant" marks the ones the header or client documents as arriving *inside* a framework mutation. *Consumers*: client files that implement or forward it (from grep; `phi_coordinator.cc` is the sole `PhiFrameworkDelegate` subclass — `phi_coordinator.h`, `phi_client_main_win.cc`).

| Callback (header line) | Payload | Mode | Re-entrant? | Client consumer(s) |
|---|---|---|---|---|
| `OnFrameworkReady(PhiFramework*, user_data_dir)` :54 | T + S | push | no (first call) | `phi_coordinator.cc:175` |
| `OnBrowserWindowCreated(window_id, hwnd)` :60 | T | push | **yes** — inside `Browser::Create`; the 4 `Created*`/`RestoredFromWindowId` queries are only valid here; also re-entrant inside `CreateAgentBrowser`/`CreateShadowBrowser` (:539-543, :913-915) | `phi_coordinator.cc:7529` |
| `OnBrowserWindowClosing(window_id)` :63 | T | push | not documented; client closes *other* browsers from inside it (`:8097-8101`) | `phi_coordinator.cc:8062`, forwarded to `phi_window_controller.cc`, `phi_kiosk_window.cc` |
| `OnTabsChanged(window_id)` :69 | T (no payload; client re-pulls) | push | **yes** — fires "from INSIDE the tab-strip change it reports" (`phi_coordinator.cc:145-148`, `:8055-8057`) | `phi_coordinator.cc:8030` → `ScheduleRefresh` |
| `OnOpenLinkInSpace(src_window, space_id, url)` :75 | T + S + S | push | no | `phi_coordinator.cc`, `phi_kiosk_window.cc` |
| `OnAskSpaceForURL(url, default_space, src_window, is_new_tab)` :87 | S + S + T + T | push (client re-opens via Bypass*) | fires from a cancelled navigation | `phi_coordinator.cc` |
| `OnRouteURLToSpace(space_id, url, src_window)` :97 | S + S + T | push | same | `phi_coordinator.cc` |
| `OnRefreshNewTab(window_id)` :105 | T | push | — | no-op forward (`phi_window_controller.h`) |
| `OnTargetURLChanged(window_id, url)` :110 | T + S | push | — | `phi_coordinator.cc:7901` → `phi_window_controller.cc` |
| `OnOmniboxResults(window_id, json)` :119 | T + **J** (`{input, matches[{line,type,contents,description,url,fillIntoEdit,inline,isDefault,deletable,hasTabMatch}]}`) | push | — | `phi_coordinator.cc:7912` → `phi_window_controller.cc:1756` |
| `OnImportCompleted(source, success)` :124 | S + T | push | — | `phi_coordinator.cc:7922` |
| `OnPlaceholderModeChanged(window_id, entered)` :129 | T | push | — | `phi_coordinator.cc` → `phi_window_controller.cc` |
| `OnContentFullscreenChanged(window_id, fs)` :133 | T | push | — | `phi_coordinator.cc:7890` |
| `OnExtensionsChanged(window_id)` :137 | T (re-pull) | push | — | `phi_coordinator.cc:7881` → popover + field icons |
| `OnProfileCreated(profile_id)` :141 | S | push | — | `phi_coordinator.cc:422` (signals an I/O-thread event) |
| `OnAgentDroveUserSpaceTab(tab, window, session)` :154 | T (int64 ×3) | push | — | `phi_coordinator.cc:2593` (casts tab to `int`) |
| `OnAgentObservedUserSpaceTab` :162 | T | push | — | `:2599` |
| `OnAgentStoppedUserSpaceTab(tab, session)` :167 | T | push | — | `:2605` |
| `OnTabDistillable(window_id, int tab_id, bool)` :192 | T | push | — | `phi_coordinator.cc:2153` |
| `OnSessionRestoreSettled(receipt_json)` :196 | **J** (`{restoredAny, eagerFilterMatchedNothing, parked[{profile, windowIds[]}]}`) | push | — | `phi_coordinator.cc:2574` → `phi_ghost_spaces.cc:116` |
| `OnColdStartRestorePlan(vector<int>* eager, bool* armed)` :212 | T out-params | **ask** (sync answer) | "called from inside the replay" (:206) | `phi_coordinator.cc:2113` |
| `OnColdStartParkedWindows(parked_json, profile)` :218 | **J** (bare `parked` list) + S | push | — | `phi_coordinator.cc:2133` (re-wraps to receipt shape and re-serialises, :2137-2149) |
| `OnAnalyticsEventCaptured(module, event, props_json)` :237 | S + S + **J** | push | — | `phi_coordinator.cc:2550` |
| `OnReaderArticleExtracted(request_id, json)` :247 | T + **J** (`{ok,title,sourceURL,contentHTML,htmlLength,pageCount}` / `{ok:false,error}`) | push (async answer) | — | `phi_coordinator.cc:547` |
| `OnExtensionMessage(request_id, sender_id, type, payload_json)` :261 | S ×3 + **J** | **ask** (must answer via `RespondToExtensionMessage`/`Fail…`) | — | `phi_coordinator.cc` → `phi_reader.cc:223` |
| `OnOpenReaderView(window_id, int64 tab_id)` :269 | T | push | — | `phi_coordinator.cc` |
| `OnShowFeedbackRequested()` :275 | — | push | — | `phi_coordinator.cc` |
| `OnFeedbackSystemLogs(request_id, text)` :281 | T + S | push (async answer) | — | `phi_coordinator.cc` |

Observations on the delegate:
* Only two callbacks are *ask* shaped (`OnColdStartRestorePlan`, `OnExtensionMessage`); the header itself notes the awkwardness of an out-param-with-armed-flag because "the do-nothing body has to stay empty" (:208-211).
* 6 of 28 carry JSON; the remaining 22 are typed. The delegate side is already mostly typed — the problem is concentrated in the *pull* side.
* Three callbacks are async answers to earlier requests correlated by an `int request_id` the client mints (`OnReaderArticleExtracted`, `OnFeedbackSystemLogs`) or a framework-minted string (`OnExtensionMessage`); `OnProfileCreated` has no correlation id at all, so the client keeps a single `profile_create_pending_` slot and refuses concurrent creates (`phi_coordinator.cc:4956-4958`) and also polls `ListProfilesJson` on a 500 ms timer diffing against a snapshot (`:4534-4553`, `:4289-4300`) — i.e. the same fact is discovered by two mechanisms.

### 1.2 `PhiFramework` (client → framework; 108 virtuals)

Grouped as the header groups them. *Addr* = how the target is named: idx = strip index, id = session tab id, tok = string token, tgt = CDP target id, prof = profile basename. *Ret* = return payload. Consumer counts are call-site counts from grep (declaration lines excluded).

| Method (line) | Addr | Args | Ret | Sync/async | Push/pull | Consumers |
|---|---|---|---|---|---|---|
| **Window lifecycle** | | | | | | |
| `FocusBrowserWindow` :296 | win | T | void | sync | cmd | wc(3) |
| `NewWindow(source)` :301 | win | T | void (window arrives via callback) | async-ish | cmd | menu(1) coord(1) |
| `CloseBrowserWindow` :307 | win | T | void (closing arrives via callback) | async | cmd | coord(9) wc(2) kiosk(2) peek(1) |
| `Navigate(win, url)` :311 | win | S | void | sync | cmd | wc(3) dl(1) coord(1) |
| **Tabs (index-addressed)** :313-357 | | | | | | |
| `NewTab(win, url)` :320 | win | S | `int` index | sync | cmd | **31** (wc 15, coord 9, peek 3, menu 3, grp 1) |
| `ListTabsJson(win)` :328 | win | — | **J** scratch ptr | sync | **pull** | **32** (coord 17, wc 7, grp 2, others 1 each) |
| `ActivateTab(win, idx)` :331 | idx | T | bool | sync | cmd | 14 |
| `CloseTab(win, idx)` :334 | idx | T | bool | sync | cmd | 6 |
| `MoveTab(win, from, to)` :339 | idx | T | bool | sync | cmd | menu(2) coord(1) |
| `MoveTabsToNewWindow(win, idx_a, idx_b)` :347 | idx | T | bool | sync (window arrives via callback) | cmd | wc(2) |
| `MoveTabToWindow(src, idx, dst)` :355 | idx | T | bool | sync | cmd | coord(2) |
| **Split view** :359-412 | | | | | | |
| `SplitActiveWith(win, idx)` :365 | idx | T | bool | sync | cmd | wc(4) peek(1) coord(1) |
| `Unsplit(win, idx)` :369 | idx | T | bool | sync | cmd | coord(1) |
| `SplitViewsJson(win)` :377 | win | — | **J** | sync | pull | wc(4) coord(2) |
| `CreateSplitViewForTabs(win, id, id, layout)` :382 | id | T + S | `const char*` split id (scratch) | sync | cmd | coord(3) wc(1) |
| `UpdateSplitView(win, split, layout, ratio)` :389 | tok | S + T | bool | sync | cmd | coord(2) wc(1) |
| `SwapSplitView(win, split)` :395 | tok | S | bool | sync | cmd | coord(2) wc(1) |
| `RemoveSplitById(win, split)` :398 | tok | S | bool | sync | cmd | coord(2) |
| `SwapTabIntoSplit(win, split, slot, idx, keep)` :408 | tok+idx | S + T | bool | sync | cmd | wc(1) |
| **Tab groups** :414-475 | | | | | | |
| `TabGroupsJson(win)` :423 | win | — | **J** | sync | pull | coord(2) wc(1) |
| `AddTabToNewGroup(win, idx)` :427 | idx | T | bool | sync | cmd | coord(1) |
| `RemoveTabFromGroup(win, idx)` :430 | idx | T | bool | sync | cmd | coord(1) |
| `SetTabTag(win, idx, tag)` :438 | idx | S | bool | sync | cmd | coord(6) wc(4) |
| `RenameTabGroup/SetTabGroupColor/SetTabGroupCollapsed/UngroupTabGroup/CloseTabGroup` :445-455 | tok | S (+S/T) | bool | sync | cmd | 3-4 each |
| `CreateTabGroupForTabs(win, ids_json)` :465 | id via **J** | J in | `const char*` group id | sync | cmd | coord(2) |
| `AddTabsToGroup(win, group, ids_json)` :469 | tok + id via **J** | S + J | bool | sync | cmd | coord(2) grp(1) |
| `RemoveTabsFromGroupByIds(win, ids_json)` :474 | id via **J** | J | bool | sync | cmd | coord(2) |
| **Nav state & pixels** :477-516 | | | | | | |
| `TabFaviconBgra(win, idx, w*, h*)` :486 | idx | T | pixel scratch ptr | sync | pull | 11 |
| `TabThumbnailBgra(win, id, …)` :496 | id | T | pixel ptr | sync | pull | 6 |
| `FaviconForUrlBgra(win, url, …)` :507 | url | S | pixel ptr; **null schedules async fetch and later fires `OnTabsChanged`** (:502-504) | sync+async side-effect | pull | wc(2) coord(2) |
| `NavStateJson(win)` :516 | win | — | **J** `{url,title,canGoBack,canGoForward,loading}` | sync | pull | coord(5) wc(1) peek(1) kiosk(1) |
| **Spaces / profiles** :518-580 | | | | | | |
| `CreateSpaceBrowser(src)` :527 | win | T | void | async (callback) | cmd | **0 — unused** |
| `CreateSpaceBrowserInProfile(prof)` :534 | prof | S | bool | async (callback; client keeps `pending_slot_`) | cmd | coord(2) |
| `CreateAgentBrowser(prof)` :544 | prof | S | `int64_t` window id | **sync, re-entrant callback** | cmd | coord(2) |
| `ListProfilesJson()` :548 | — | — | **J** `[{id,name,loaded}]` | sync | pull | coord(9) wc(1) menu(1) |
| `CreateProfile(name)` :552 | — | S | void; id arrives via `OnProfileCreated` **or** by re-listing (:550-551 says "later ListProfilesJson") | async | cmd | coord(4) menu(1) |
| `DeleteProfile/RenameProfile` :556-560 | prof | S | void (fire-and-forget; client pre-validates by re-listing, `phi_coordinator.cc:3545-3562`) | async | cmd | coord(1 each) |
| `WindowProfileJson(win)` :563 | win | — | **J** `{id,name}` (+ undocumented `incognito`) | sync | pull | coord(2) kiosk(1) |
| `ListSearchEnginesJson(prof)` :570 / `SetDefaultSearchEngine` :573 | prof | S | J / bool | sync | pull/cmd | coord(1 each) |
| `DownloadDirectoryUtf8(prof)` :577 / `SetDownloadDirectory` :579 | prof | S | S / bool | sync | pull/cmd | coord(1) / coord(3)+settings |
| **Lazy restore** :582-662 | | | | | | |
| `RestorePreviousSession(prof, armed, const int* ids, n)` :592 | prof + **int** window ids | S + T array | void; settles via `OnSessionRestoreSettled` | async | cmd | coord(1) |
| `DistillTab(win, idx)` :599 | idx | T | bool | sync | cmd | **0 — unused** |
| `RestoredFromWindowId()` :611 | — | — | **int** (>0 / -1 / 0 tri-state) — **valid only inside `OnBrowserWindowCreated`** | sync | side-channel pull | coord(1) |
| `CreatedWindowIsKiosk()` :618 | — | — | bool — same validity window | sync | side-channel | coord(1) |
| `CreatedWindowPeekOpener(int64* win, int64* tab)` :624 | — | out-params | — same | sync | side-channel | coord(1) |
| `SetPeekPreferences(enabled, auto)` :640 | — | T | void | sync | **push (client→fw)** | coord(1) |
| `GhostWindowsJson()` :645 | — | — | **J** (same shape as receipt `parked`) | sync | pull | coord(1) |
| `MaterializeGhostWindow(prof, int win)` :654 | prof + **int** | S + T | `int` tri-state (0/1/2) | sync (window arrives via callback with `RestoredFromWindowId`) | cmd | coord(2) |
| `DropGhostWindow(prof, int win)` :661 | prof + **int** | S + T | bool | sync | cmd | coord(2) |
| `SetUserReclaimedTabs(const int* ids, n)` :670 | id | T array | void | sync | push | coord(1) |
| **Downloads** :672-704 | | | | | | |
| `ListDownloadsJson(win)` :679 | win | — | **J** (13 documented fields; client also reads `danger`) | sync | pull | dl(1) |
| `DownloadAction(win, uint32 id, action)` :683 | uint32 | T + S enum-as-string | void | sync | cmd | dl(2) |
| `DownloadsJsonForAgent(win)` :697 | win | — | **J** (second, different shape: guid identity, 18 fields) | sync | pull | coord(2) |
| `DownloadActionByGuid(win, guid, action)` :702 | guid | S + S | bool | sync | cmd | coord(1) |
| **Space routing** :706-745 | | | | | | |
| `SetWindowSpace(win, space)` :707 | win | S | void | sync | push | wc(2) |
| `SetSpaceMenu(json)` :716 | — | **J** `[{id,name,windowId}]` | void | sync | push | coord(1) |
| `SetSpaceRoutingTable(json)` :730 | — | **J** `{rules[…], spaceWindows{}}` | void | sync | push | coord(1) |
| `OpenTabBypassingSpaceRouting(win, url)` :737 | win | S | void | sync | cmd (one-shot exemption) | coord(6) |
| `NavigateActiveTabBypassingSpaceRouting(win, url)` :744 | win | S | void | sync | cmd (one-shot) | coord(1) |
| `GoBack/GoForward/Reload/StopLoading` :747-750 | win | T | bool | sync | cmd | 2-5 each |
| **Credential autofill** :752-775 | | | | | | |
| `TargetUrlForCdpTarget(tgt)` :757 | tgt | S | S scratch | sync | pull | coord(1) |
| `TabIdForCdpTarget(tgt)` :762 | tgt | S | `int` tab id / -1 | sync | pull | coord(2) |
| `FillTaggedField(tgt, token, field, value)` :772 | tgt | S ×4 | bool | sync | cmd | coord(2) |
| **Chromium commands** :777-790 | | | | | | |
| `ExecuteChromiumCommand(win, IDC)` :785 / `IsChromiumCommandEnabled` :789 | win | T | bool | sync | cmd / pull | menu(1 each) |
| **Startup pref** :792-800 | | | | | | |
| `GetRestoreOnStartup(prof)` :796 | prof | S | bool | sync | pull | **0 — unused** (the setter has 1 caller) |
| `SetRestoreOnStartup(prof, bool)` :800 | prof | S + T | void | sync | cmd | coord(1) |
| **Omnibox** :802-819 | | | | | | |
| `StartOmniboxQuery/StopOmniboxQuery` :807-808 | win | S | void; results stream via `OnOmniboxResults` | async | cmd | wc(1 each) |
| `OpenOmniboxMatch(win, line, new_tab)` :814 | line (index into last result set) | T | bool | sync | cmd | wc(1) |
| `DeleteOmniboxMatch(win, line)` :819 | line | T | bool | sync | cmd | **0 — unused** |
| **Import / bookmarks** :821-834 | | | | | | |
| `StartChromeImport(win)` :827 | win | T | bool; completes via `OnImportCompleted` | async | cmd | menu(1) |
| `AllBookmarksJson(win)` :834 | win | — | **J** tree | sync | pull | coord(1) |
| **Tab search** :836-853 | | | | | | |
| `SearchTabsDataJson(win)` :845 | win | — | **J** `{openTabs[{tabId,windowId,index,…}], recentlyClosed[{sessionId,…}]}` | sync | pull | search(1) |
| `ActivateSearchTab(int64 tab)` :849 | id (**int64**) | T | bool | sync | cmd | search(1) |
| `OpenRecentlyClosedEntry(win, int64 session)` :852 | session id | T | bool | sync | cmd | search(1) |
| **Extensions** :855-887 | | | | | | |
| `ListExtensionsJson(win)` :863 | win | — | **J**; first call may return `[]` and kick an async refresh signalled by `OnExtensionsChanged` | sync + async side-effect | pull | wc(1) ext(1) coord(1) |
| `ExtensionIconBgra(ext_id, …)` :868 | ext id | S | pixel ptr | sync | pull | wc(1) ext(1) |
| `TriggerExtensionAction(win, ext, x,y,w,h)` :875 | ext id | S + T | void | sync | cmd | wc(1) ext(1) |
| `SetExtensionPinned(win, ext, bool)` :885 | ext id | S + T | void | sync | cmd | ext(1) |
| **JS dialogs** :889-909 | | | | | | |
| `JavaScriptDialogStateJson(tgt)` :900 | tgt | S | **J** `{found,open,type,message}` | sync | pull | coord(1) |
| `HandleJavaScriptDialog(tgt, accept, prompt, bool* handled)` :906 | tgt | S + T + out | bool | sync | cmd | coord(1) |
| **Shadow / reader / misc** :911-1022 | | | | | | |
| `CreateShadowBrowser(prof, incognito)` :920 | prof | S + T | `int64_t` window id; **re-entrant callback** | sync | cmd | coord(2) |
| `StartReaderExtraction(win, tgt, request_id)` :929 | win/tgt | T + S + T | bool; answers via `OnReaderArticleExtracted` | async | cmd | coord(1) |
| `SetDisplayLanguage(locale)` :952 | — | S | S (applied locale, scratch) | sync (+async reload) | cmd | coord(1) |
| `RequestDefaultBrowser()` :969 | — | — | void | async | cmd | coord(2)+settings |
| `RespondToExtensionMessage/FailExtensionMessage` :977-980 | request id | S + **J**/S | void | sync | answer | coord(1 each) |
| `BroadcastToExtensions(type, payload_json)` :988 | — | S + **J** | bool | sync | push | coord(2) reader(1) |
| `MoveTabToReaderPanel(win, int surface, int origin)` :1005 | id (**int**) | T | `int64_t` panel window id; **callback fires first** with `CreatedWindowReaderOrigin` set | sync, re-entrant | cmd | coord(1) reader_panel.h(1) |
| `CreatedWindowReaderOrigin(int64* win, int64* tab)` :1013 | — | out | — valid only inside `OnBrowserWindowCreated` | sync | side-channel | coord(1) |
| `FetchFeedbackSystemLogs(win, request_id)` :1022 | win | T | bool; answers via `OnFeedbackSystemLogs` | async | cmd | coord(2)+feedback |
| **Id-addressed (§7 additions)** :1024-1050 | | | | | | |
| `ActivateTabById(win, int id)` :1028 | id | T | bool | sync | cmd | coord(4) |
| `CloseTabById` :1029 | id | T | bool | sync | cmd | coord(1) |
| `SetTabTagById` :1030 | id | T + S | bool | sync | cmd | coord(9) |
| `TabFaviconBgraById` :1031 | id | T | pixel ptr | sync | pull | coord(2) |
| `MoveTabBefore(win, id, anchor, group_id)` :1043 | id + tok (nullptr/""/id tri-state) | T + S | bool | sync | cmd | coord(2) |
| `MoveTabAfter` :1047 | id | T + S | bool | sync | cmd | **0 — unused** |

Totals: 108 methods; **27 return or take JSON**; **5 return raw pixel scratch buffers**; **5 have zero client callers** (`CreateSpaceBrowser`, `DistillTab`, `GetRestoreOnStartup`, `DeleteOmniboxMatch`, `MoveTabAfter`); **4 are creation side-channels**; **3 are "sync with a re-entrant callback"** (`CreateAgentBrowser`, `CreateShadowBrowser`, `MoveTabToReaderPanel`); **2 have hidden async side-effects that later fire an unrelated notification** (`FaviconForUrlBgra` → `OnTabsChanged`, `ListExtensionsJson` → `OnExtensionsChanged`).

The index-vs-id split by caller (grep at `phi_coordinator.cc`, `phi_window_controller.cc`, `phi_menu_bar.cc`, `phi_group_overview.cc`, `phi_tab_switcher.cc`): index-addressed `ActivateTab/CloseTab/SetTabTag/TabFaviconBgra` still have **~40 call sites** across 6 files (e.g. `phi_window_controller.cc:6793,6804,6851,6859,6865,6870,8106,8135,8181,8248,8428`; `phi_menu_bar.cc:810,900,1206`; `phi_group_overview.cc:146,279,289`), while the `ById` twins are used only inside `phi_coordinator.cc` (`:1410,1453-1499,1641-1712`). The window controller — the file that most needs stability under churn — has **zero** `ById` calls.

---

## 2. JSON usage

### 2.1 Where the same shape is parsed more than once

| Shape (producer) | Parse sites | Files | Distinct row structs / decoders |
|---|---|---|---|
| **`ListTabsJson` row** | **29** — `phi_coordinator.cc:1222,1360,1478,1855,1913,1958,2038,2247,2379,2473,3116,3468,4093,5777,6095,8334`; `phi_window_controller.cc:2275,4783,4809,6824,6884,7692,8162`; `phi_group_overview.cc:119,300`; `phi_tab_switcher.cc:142`; `phi_peek_panel.cc:658`; `phi_reader_panel.cc:231`; `phi_menu_bar.cc:1127` | 7 | `StripTab` (`phi_window_controller.cc:2254`, via `ReadStrip` :2268), `TabRow` (`phi_window_controller.h:891`), `SidebarLiveTab` (`phi_sidebar_model.h:85`), `LiveTabInfo` (`phi_coordinator.h:551`, via `LiveTab` :1353), `TabRowsOf` (raw `DictValue` clones, `phi_coordinator.cc:2240`), `TabUrlAtIndex` (`phi_window_controller.cc:7685`), `IndexOfTagged` (`:6878`), `SplitIdForTab` (`:4777`), `tabs_of` lambda (`phi_coordinator.cc:8330`), `Entry` (`phi_tab_switcher.cc:135`), `Card` (`phi_group_overview.cc:130`) |
| `NavStateJson` | 5 — `phi_coordinator.cc:1204,2763`; `phi_window_controller.cc:5784`; `phi_kiosk_window.cc:276`; `phi_peek_panel.cc:423` | 4 | none; ad-hoc `FindString("url")` each time |
| `SplitViewsJson` | 5 — `phi_coordinator.cc:1928,8607`; `phi_window_controller.cc:4032,4078,4757` | 2 | none |
| `TabGroupsJson` | 4 — `phi_window_controller.cc:6371`; `phi_coordinator.cc:5961,8420` (+`GroupHeader` :6379) | 2 | `GroupHeader` + raw |
| `ListProfilesJson` | 7 — `phi_coordinator.cc:3547,4289,4535,4580,4942`; `phi_window_controller.cc:5316`; `phi_menu_bar.cc:588` | 3 | none; three of these exist only to *diff* the list before/after `CreateProfile` (`:4534-4545`, `:4579-4590`, `:4289-4300`) |
| `WindowProfileJson` | 3 — `phi_coordinator.cc:4977,7679`; `phi_kiosk_window.cc:303` | 2 | none |
| `ListExtensionsJson` | 3 — `phi_extensions_popover.cc:172`; `phi_window_controller.cc:5808`; `phi_coordinator.cc:3991` | 3 | `ExtensionRow` + 2 ad-hoc |
| Downloads (two producers, two shapes) | `ListDownloadsJson` → `phi_downloads_popover.cc:235`; `DownloadsJsonForAgent` → `phi_coordinator.cc:8682,8695` | 2 | `DownloadRow` + pass-through |
| `parked` receipt shape | 3 — `phi_ghost_spaces.cc:123` (receipt), `phi_coordinator.cc:2138` (cold-start list, re-wrapped and **re-serialised** to feed the receipt parser :2137-2149), `:8536` (`GhostWindowsJson` passed through) | 2 | `Remembered` |
| `OnOmniboxResults` | 1 — `phi_window_controller.cc:1762` | 1 | `OmniboxRow` (clean) |
| `SearchTabsDataJson` | 1 — `phi_search_tabs_panel.cc:185` | 1 | `Row` (clean) |

Beyond these, the remaining ~95 `JSONReader` sites in `phi_coordinator.cc:4674-5976` and `:8321-8710` are the **agent control dialect** (`agentSpace.*` verbs arriving over the named pipe) — each verb re-parses `payload_json` into a `DictValue`, then frequently parses a framework JSON string, then writes a `DictValue` reply. That is a different bus (client ↔ skill) and is fine to keep as JSON, but it is where most of the re-serialisation lives.

### 2.2 Re-serialisation of what was just parsed

* `agentSpace.tabs.list` (`phi_coordinator.cc:5776-5780`), `agentSpace.groups.list` (`:5961-5964`), `lazyRestore.ghosts` (`:8536-8539`), `downloads.list` (`:8682-8686`): parse the framework string only to `std::move` it into the reply dict, which `JSONWriter::Write` re-encodes (`:6016`). A pass-through of the original `const char*` would do if the reply envelope allowed raw fragments.
* `groups.list` / `splits.list` in the skill dialect (`:8420-8445`, `:8607-8628`) parse `TabGroupsJson`/`SplitViewsJson` and *rename* keys (`id`→`token`, `id`→`splitId`) row by row — a second schema for the same data.
* `OnColdStartParkedWindows` (`:2137-2149`) parses the bare list, wraps it in `{"parked":…}`, `JSONWriter::Write`s it, then hands the string to `ReplaceFromReceipt`, which parses it again (`phi_ghost_spaces.cc:122`).
* `HandleControlCommand` (`:398-404`) parses the reply that `ExecuteControlOnUi` just serialised to splice in `profileId`, and again at `:410-414` to look for `pendingReply` — after first doing a `std::string::find("\"pendingReply\"")` on the serialised text (`:410`).
* `RequestCreateSpace*` (`:4314-4322`, `:4556-4563`, `:4606-4623`) build a `DictValue`, serialise it, call the *client's own* `ExecuteControlOnUi("agentSpace.spaces.create", json)`, and parse the JSON reply — the UI uses the agent wire to talk to itself.
* Tab ids in the skill dialect are re-encoded to JSON (`:8414-8416`) because `CreateTabGroupForTabs`/`AddTabsToGroup`/`RemoveTabsFromGroupByIds` take `tab_ids_json` (header :457-475) — the only three methods in the interface that take a JSON *argument* for what is a `const int*, int` elsewhere (`SetUserReclaimedTabs` :670, `RestorePreviousSession` :594).

### 2.3 Ad-hoc field access without validation

All parse sites use `FindX(...).value_or(default)` with silent fallbacks. Consequences observed:

* `phi_downloads_popover.cc:244`: `base::UTF8ToWide(*dict->FindString("name"))` — **unchecked deref**; a row without `name` crashes the popover.
* `id` decoded as `FindInt` at 27 sites but `FindDouble` at `phi_peek_panel.cc:664` and `phi_search_tabs_panel.cc:205-208`; `windowId` in the control payload decoded via `FindDouble` 28× vs `FindInt` 1× (`phi_coordinator.cc`). Both work because `base::Value` stores ints as doubles-compatible, but it means there is no single "what is a tab id in JSON" answer.
* `index` defaults to `-1` at some sites and `0` at others: `phi_coordinator.cc:2263` and `:2443` call `ActivateTab`/`CloseTab` with `FindInt("index").value_or(0)` — a missing index acts on **tab 0**.
* `phi_group_overview.cc:126-127`: a row with no `groupId` is skipped by pointer check; `phi_window_controller.cc:6383-6385`: a group with no `color` silently becomes `"grey"`.
* `OnOmniboxResults` `line` defaults to the row's position (`phi_window_controller.cc:1775-1776`) — plausible but invents a value the framework then uses to `OpenOmniboxMatch`.

### 2.4 The undocumented schemas (what the client actually depends on)

**`ListTabsJson` row** — header documents `index,id,title,url,active,loading` (:323) and mentions `splitId` (:361), `groupId` (:417), `tag` (:435) elsewhere. Client additionally reads: `unloaded` (`phi_window_controller.cc:2300`, `.h:919`), `surface` (`:2306`, `phi_reader_panel.cc:239`, `phi_coordinator.cc:2430`), `originTabId` (`:2309`, `phi_reader_panel.cc:245`, `phi_coordinator.cc:2433`). **None of `unloaded`/`surface`/`originTabId` appear in the header** (verified by grep). `loading` is documented but never read from tab rows by the client. `SidebarLiveTab` (`phi_sidebar_model.h:85-96`) is the de-facto typed schema: `{id, index, tag, split_id, group_id, active, surface, origin_id}`.

**`NavStateJson`** — `{url, title, canGoBack, canGoForward, loading}`; matches header. `phi_window_controller.cc:5794` additionally applies a `chrome://newtab` prefix rule that belongs in the producer.

**`SplitViewsJson`** — `{id, layout, ratio, primaryTabId, secondaryTabId}`; matches.

**`TabGroupsJson`** — `{id, title, color, collapsed}`; matches. Color is a 9-name string enum re-decoded via `GroupColorByName` (`phi_window_controller.cc:6385`).

**`ListProfilesJson`** — `{id, name, loaded}`; client reads `id`, `name` only.

**`WindowProfileJson`** — header `{id, name}` (:562); client reads **`incognito`** (`phi_coordinator.cc:7681`) — undocumented, and it is the *only* signal by which an incognito window is recognised at creation.

**`ListDownloadsJson`** — 13 documented fields; client reads **`danger`** (`phi_downloads_popover.cc:263`, drives the keep/discard action set at `:282-286`) — undocumented.

**`OnSessionRestoreSettled` / `OnColdStartParkedWindows` / `GhostWindowsJson`** — `{parked:[{profile, windowIds:[int]}]}`; three producers, one consumer struct (`Remembered`, `phi_ghost_spaces.cc:56`).

**`OnOmniboxResults`** — client reads `line,type,contents,description,fillIntoEdit,hasTabMatch` of the 10 documented match fields.

**`SearchTabsDataJson`** — client reads `title,url,lastActiveText,tabId,windowId` and `sessionId` for closed entries.

---

## 3. Consistency

### 3.1 Naming

* **Index vs id doublets** without a naming rule: `ActivateTab`/`ActivateTabById`, `CloseTab`/`CloseTabById`, `SetTabTag`/`SetTabTagById`, `TabFaviconBgra`/`TabFaviconBgraById`, `RemoveTabFromGroup`(idx)/`RemoveTabsFromGroupByIds`(json), `AddTabToNewGroup`(idx)/`CreateTabGroupForTabs`(json ids), `RemoveSplitById`(split token, not tab id), `DownloadAction`(uint32)/`DownloadActionByGuid`. `TabThumbnailBgra` takes an *id* without the suffix (:496-497); `CreateSplitViewForTabs` takes ids without the suffix (:382); `ActivateSearchTab` takes an `int64_t` id (:849) while `ActivateTabById` takes `int` (:1028). The suffix therefore does not reliably tell the reader which address space a method uses.
* **Move family**: `MoveTab(from,to)` (index/index), `MoveTabsToNewWindow(idx,idx)`, `MoveTabToWindow(idx,…)`, `MoveTabBefore/After(id, anchor_id, group)`, `MoveTabToReaderPanel(surface_id, origin_id)` — five verbs, three address spaces, and `MoveGroupBefore/After` still "open" per the design doc §7. The client's own `PhiCoordinator::ActivateTabById` (`phi_coordinator.cc:2260-2267`) re-implements the framework's `ActivateTabById` by parsing `ListTabsJson` and calling index `ActivateTab` — an un-migrated duplicate with the same name as the framework method.
* **`*Json` suffix** marks 15 pull methods but not the JSON-*taking* ones (`SetSpaceMenu`, `SetSpaceRoutingTable`, `BroadcastToExtensions`, `CreateTabGroupForTabs`), nor the JSON-carrying delegate callbacks (`OnOmniboxResults`, `OnSessionRestoreSettled`).
* **Bool-vs-void** for commands is arbitrary: `Navigate` void, `GoBack` bool, `CreateProfile` void, `CreateSpaceBrowserInProfile` bool, `DownloadAction` void, `DownloadActionByGuid` bool.
* **String enums**: `action` ("open|show|pause|…"), `layout` ("vertical|horizontal"), `color` (9 names), `field` ("password|username"), `state`, `source` ("chrome") — all `const char*`, all re-compared in the client (`phi_downloads_popover.cc:282-288`, `phi_window_controller.cc:4772,4804`).

### 3.2 Identifier spaces

| Space | Header type(s) | Client type(s) | Notes |
|---|---|---|---|
| Window / browser (session id) | `int64_t` (most); **`int`** in `RestoredFromWindowId` :611, `MaterializeGhostWindow` :655, `DropGhostWindow` :662, `OnColdStartRestorePlan` :212, `RestorePreviousSession` :594 | `int64_t`; `int` in `PhiGhostSpaces::Remembered` (`phi_ghost_spaces.cc:52-56`) | Chromium `SessionID` is int32, so the `int64_t` is cosmetic and the mixed widths are pure noise. |
| **Slot** (client shell window keyed by its *first* browser's id) | not a concept | `int64_t` reused; `browser_to_slot_` (`phi_coordinator.h:481`), `ResolveBrowser` (`:4656`) | Every delegate callback does `browser_to_slot_.find(window_id)` with a fallback to the id itself (`phi_coordinator.cc:7892-7894,7902-7904,7913-7915,8050-8052,2159-2161`). The peek-opener bug fixed at `:7575-7582` was exactly this conflation. |
| Tab (session id) | **`int`** ×12 (`OnTabDistillable`, `CreateSplitViewForTabs`, `TabThumbnailBgra`, `TabIdForCdpTarget`, `MoveTabToReaderPanel`, all `*ById`, `MoveTabBefore/After`, `SetUserReclaimedTabs`); **`int64_t`** ×6 (`OnOpenReaderView`, `OnAgent*` ×3, `ActivateSearchTab`, `CreatedWindowPeekOpener/ReaderOrigin` out-params) | `int` (`SidebarLiveTab::id`, `TabRow::id`, `PhiAgentDriveRegistry`), `int64_t` (`PhiSurfacePanel::origin_tab_id()`), `static_cast<int>` bridges at `phi_coordinator.cc:2596,2602,2607,2201,2452,7588`, `phi_surface_registry.cc:30,76` | Same int32 underneath. |
| Tab index | `int` | `int`, `-1`/`0` sentinels mixed | "stable for the duration of a call" (:315-316) — the client's own comments repeatedly say otherwise (`phi_window_controller.cc:2251-2253,6853`). |
| Split token | `const char*` | `std::string` | Opaque; fine. |
| Group token | `const char*`, plus a **tri-state `nullptr`/`""`/id** in `MoveTabBefore` (:1039-1041) | `std::string` + `const char*` | Tri-state pointer semantics are invisible at call sites. |
| CDP target id | `const char*` | `std::string` | Opaque; fine. Converted to tab id via `TabIdForCdpTarget`. |
| Session-restore `sessionId` (recently-closed) | `int64_t` (:852) | `int64_t` | A fourth "session id" meaning (window, tab, CDP session in `OnAgent*`, restore entry). |
| Profile | basename `const char*` | `std::string` | Fine, but `CreateProfile` returns nothing and the id is discovered three ways (§1.1). |
| Extension id, guid (downloads), guid (stored rows), space id, request id (`int` vs `const char*`) | strings / ints | strings / ints | `request_id` is `int` for reader/feedback but `const char*` for extension messages. |

No newtypes exist on either side (`grep "using .*Id ="` in `client/*.h` → 0 hits).

### 3.3 The `Created*` side channels

`OnBrowserWindowCreated(window_id, hwnd)` carries no intent; the client reconstructs it (`phi_coordinator.cc:7529-7692`) in this order: `CreatedWindowIsKiosk()` (:7535) → `CreatedWindowPeekOpener()` (:7546) → `CreatedWindowReaderOrigin()` (:7555) → client-side `pending_agent_task_`/`pending_shadow_task_` (:7601) → `RestoredFromWindowId()` (:7620) → client-side `pending_slot_` (:7639) → `WindowProfileJson().incognito` (:7679) → default shell. That is **seven creation kinds** determined by four framework getters that are UB outside the callback ("asking later returns 0", :604), two client globals that are set around a synchronous call and depend on re-entrancy (:6134-6140, :6791-6797), one undocumented JSON field, and the *absence* of all of the above. The header's own comment concedes the shape: "Like RestoredFromWindowId this is pushed rather than queried" (:615-616) — it is queried, on a getter, during a callback; only the validity window makes it "pushed".

Failure modes already visible: a peek whose opener slot switched Space "finds no opener and falls through into a shell" (:7577-7578, fixed by a lookup); a reader panel without an origin logs and falls through to becoming a normal shell (:7571-7573); an agent create whose callback never fires falls back to `last_agent_window_id_` (:6141).

### 3.4 One-shot / bypass patterns

* `OpenTabBypassingSpaceRouting` / `NavigateActiveTabBypassingSpaceRouting` arm a "(url, window) one-shot pass in the router" (:733-734). The client calls the former at 6 sites (`phi_coordinator.cc:1082,1104,3150,7652,7661,8828`), including from *inside* `OnBrowserWindowCreated` (:7652,7661) because "the table rebuilt below races the open" (:7648-7649). The one-shot is keyed by URL text; two opens of the same URL in flight share one pass.
* The design doc §7 proposes a third one-shot, `NavigateTabInPlace`, mirroring `PhiURLRouter::ExemptNextNavigation`, to end the client's "clear-tag/navigate/restore timer pattern".
* `SetPeekPreferences` is a push of client prefs into the framework "because the menu item decides on the UI thread with no way back to the client" (:628-631) — the same ask-shaped hole `OnAskSpaceForURL` fills for routing; two different mechanisms for "framework needs a client policy answer".
* The `FaviconForUrlBgra`-returns-null-then-fires-`OnTabsChanged` side effect (:502-504) is a one-shot in disguise: a *pull* that later arrives as an unrelated *push*, which the client cannot distinguish from a real strip change (it triggers a full `ScheduleRefresh`).

---

## 4. Threading and re-entrancy

**What the header says.** One sentence: "Everything runs on the browser UI thread, synchronously, no marshaling (the Mac threading contract)" (:32-33). Per-method: `OnColdStartRestorePlan` "Synchronous, on the UI thread, called from inside the replay" (:206); `CreateAgentBrowser` "OnBrowserWindowCreated fires re-entrantly during this call" (:540-541); `CreateShadowBrowser` same (:914-915); `MoveTabToReaderPanel` "arrives at OnBrowserWindowCreated first" (:1001-1002); `OnAnalyticsEventCaptured` "dispatched here on the UI thread" (:223); scratch buffers "valid until the next call on this interface (single-threaded UI use)" (:326-327). **Nothing** says whether a delegate callback may call `PhiFramework` methods, whether `OnTabsChanged` is delivered before or after the strip mutation commits, or whether `OnBrowserWindowClosing` may close other browsers.

**What the client has discovered.**

1. `OnTabsChanged` arrives **mid-mutation**: "a tab being removed is still in the strip" (`phi_coordinator.cc:145-146`, `phi_window_controller.h:499-500`, `:352`). Reading the strip inline renders the pre-change state; reading-and-closing inline "recursed until the stack gave out" (`:147-148`). Fix: `ScheduleRefresh` posts `kMsgPhiRebuildRows` (`phi_window_controller.cc:5403-5414`, handler `:11171-11179`) and the coordinator posts `WM_PHI_READER_SWEEP` (`phi_coordinator.cc:8042-8048`, handler `:347-354`).
2. Navigation callbacks arrive **inside a WebContents observer walk**: opening a tab or closing a browser there trips `!observers_.is_notifying_observers()` (`:150-154`). Fix: `WM_PHI_READER_EXPAND` (`:2342-2352`, handler `:339-345`) and `WM_PHI_READER_RETIRE` (`:156-158`, handler `:330-337`).
3. `WM_PAINT` must never rebuild because a rebuild pulls JSON from the framework (`phi_window_controller.cc:11415-11423`, `.h:504-505`): a paint-time `RefreshTabs` would `InvalidateRect` and recurse.
4. Native-page mounting is posted (`kMsgPhiSyncNativePages`, `.h:347-350`, `:6535`) "rather than inside the rebuild".
5. Conversely the client **depends on** re-entrancy for `CreateAgentBrowser`/`CreateShadowBrowser`: `pending_agent_task_` is set, the sync call made, and the callback is expected to have run by return (`:6134-6141`, `:6791-6798`, `:7601-7613`). If the framework ever posted `OnBrowserWindowCreated` instead, the agent path would break silently (`adopted == 0` → `create_failed`).
6. `OnBrowserWindowClosing` closes *other* browsers from inside itself (`:8097-8101`) and re-activates a tab in a different window (`:8073`). Whether that is legal inside Chromium's `BrowserList` removal notification is not stated anywhere; it works today.
7. `OnBrowserWindowCreated` performs `OpenTabBypassingSpaceRouting` on the just-created browser (`:7652,7661`) and `PushSpaceMenu()` (`:7634,7667,7690`) — a `SetSpaceMenu` push into the framework from inside `Browser::Create`.

**Cross-thread edges the client owns (not framework, but shape the contract).** The control server's I/O thread marshals every command to the UI thread with a blocking `SendMessage` to a message-only HWND (`phi_coordinator.cc:186-200,382-389,321-328`); `OnProfileCreated` signals a Win32 event the I/O thread waits on (`:390-407,422-427`); `RegisterPendingReply`/`WaitPendingReply` let a UI-side async framework answer (`OnReaderArticleExtracted`) release an I/O-thread waiter (`:429-440`). Telemetry uploads on its own thread and is fed from UI-thread callbacks (`phi_telemetry.h:48-49`). All consistent with the header's UI-thread rule; none of it is described by the header.

**Net:** the client carries **five posted-turn messages and two pending-global protocols** whose only purpose is to compensate for an undocumented ordering contract. Every one of them is a place where a future framework change (posting a callback, or delivering it after the mutation) would either fix a bug the client has worked around or break a workaround the client relies on — with no compile-time or documented signal either way.

---

## 5. Target contract

### 5.1 Options evaluated

| Option | What it fixes | What it does not | Cost |
|---|---|---|---|
| (a) typed structs/enums for hot paths, JSON for extensibility | Removes 29 parsers for tabs, 5 for nav, 5 for splits, 4 for groups; makes `unloaded/surface/originTabId` part of the contract; ends `FindInt` vs `FindDouble` | Does nothing for creation intent, ids, re-entrancy | Fork: 6 serialisers → 6 struct fillers. Client: replace `ReadStrip` and friends. Mac: unaffected if the JSON methods stay for it (but then two producers drift). |
| (b) one versioned snapshot (windows→tabs→groups→splits) pushed or pulled; client owns one model | Same as (a) *plus* a single parse per change, coherent groups+splits+tabs in one read (today three reads per rebuild `phi_window_controller.cc:6366,6371` + N more per drag), a `reason`/`version` for animation and staleness | Creation intent, ids (unless combined) | As (a) plus a `version` counter and a `reason` enum in the fork; the client already has `SidebarInputs` (`phi_sidebar_model.h:110`) as the target shape. |
| (c) explicit creation request/announcement object | Ends the four `Created*` getters and the `pending_agent_task_`/`pending_shadow_task_`/`pending_slot_` globals; makes the seven creation kinds a closed enum; lets the framework post the callback later without breaking adoption | Nothing about snapshots | Fork: fill one struct where the four markers are set today. Client: `OnBrowserWindowCreated` becomes a `switch`. Mac: its `PhiChromiumBridgeDelegate` twin gets the same struct. |
| (d) strongly-typed id newtypes | Compile-time separation of slot/window/tab/split/group/target/session; ends the `int`/`int64_t` mix and the `static_cast<int>` bridges | Nothing else | One shared header; mechanical signature changes on both sides; Chromium's own `SessionID` already exists and could be wrapped. |
| (e) documented threading contract | Names the ordering (`OnTabsChanged` after commit, or explicitly "pre-commit"), which callbacks are re-entrant and which methods may be called from which callbacks; lets the client delete posted-turn hacks that are no longer needed and keep the ones that are | Nothing structural | Text in the header plus one behavioural change in the fork (post `OnTabsChanged` after the strip settles, coalesced). |

### 5.2 Recommendation

**Adopt (b) + (c) + (d) + (e), with (a)'s typed-struct principle applied only where (b) already needs it.** Keep JSON for: the agent control dialect (a different bus), `OnAnalyticsEventCaptured`, extension messaging, the restore receipt, omnibox matches (a streaming, framework-shaped list that only one client site reads), tab-search data, and bookmarks import — i.e. things that are either extensible by design or read at exactly one site. Everything read at ≥3 sites or on the paint path becomes a struct.

Reasons for this over (a) alone: the client already has the single-model architecture the design doc §4 describes, and `RebuildTabs` (`phi_window_controller.cc:6350-6423`) *is* "parse once into `SidebarInputs`"; but that guarantee is broken outside the window controller by 16 coordinator parsers that exist because the coordinator cannot get at the controller's snapshot without re-pulling. A pushed snapshot with a version number gives the coordinator and every popover the same object.

### 5.3 Header sketch (house style: additive, `IS_WIN_PHI_CLIENT`, Mac twin updated in step)

```cpp
// phi_ids.h — shared by framework and client, no Chromium includes.
namespace phi_framework {
struct WindowId  { int32_t v = 0; /* Browser::session_id() */ };
struct TabId     { int32_t v = 0; /* SessionID of the WebContents */ };
struct SplitId   { std::string v; };
struct GroupId   { std::string v; };
struct TargetId  { std::string v; /* DevTools target */ };
struct ProfileId { std::string v; /* on-disk basename */ };
// Comparison/hash helpers; explicit constructors so int literals do not convert.
}
```

```cpp
// --- The window snapshot (pull or push; ONE shape) ---------------------
enum class TabSurface : uint8_t { kContent, kReader };   // Peek is a browser, not a tab
struct TabSnapshot {
  TabId id; int index;
  std::string title, url, tag;
  SplitId split; GroupId group;
  bool active, loading, unloaded;
  TabSurface surface; TabId origin;        // origin valid iff surface != kContent
};
struct SplitSnapshot { SplitId id; TabId primary, secondary; enum {kVertical,kHorizontal} layout; double ratio; };
struct GroupSnapshot { GroupId id; std::string title; enum class Color : uint8_t {kGrey,kBlue,…} color; bool collapsed; };
struct NavState { std::string url, title; bool can_go_back, can_go_forward, loading; };
struct WindowSnapshot {
  WindowId window; uint64_t version;       // monotonic per window
  ProfileId profile; bool incognito;
  NavState nav;
  std::vector<TabSnapshot> tabs;           // strip order
  std::vector<SplitSnapshot> splits;
  std::vector<GroupSnapshot> groups;
};
enum class TabsChangeReason : uint8_t { kAdded, kRemoved, kMoved, kActivated, kUpdated, kGroupChanged, kSplitChanged, kFaviconReady, kBulk };

// PhiFramework — pull. Fills *out; returns false for an unknown window.
virtual bool Snapshot(WindowId, WindowSnapshot* out) = 0;

// PhiFrameworkDelegate — push. Delivered AFTER the TabStripModel mutation has
// committed, coalesced per window per task; never from inside a strip observer
// or a WebContents observer walk. The client may call any PhiFramework method
// from inside it (see §threading below).
virtual void OnWindowChanged(const WindowSnapshot&, TabsChangeReason) {}
```

Notes: `FaviconForUrlBgra`'s async completion becomes `kFaviconReady` instead of an anonymous `OnTabsChanged`; `ListExtensionsJson`'s async refresh keeps `OnExtensionsChanged` (a different model). `ListTabsJson`/`NavStateJson`/`SplitViewsJson`/`TabGroupsJson`/`WindowProfileJson` remain for the Mac and the agent dialect until nothing else reads them, exactly as §7 does for index methods.

```cpp
// --- Creation announcement (replaces the four Created* getters) ---------
enum class WindowKind : uint8_t { kShell, kSpace, kKiosk, kPeek, kReaderPanel, kAgent, kShadow, kIncognito };
struct WindowCreation {
  WindowId window; void* hwnd;
  WindowKind kind;
  ProfileId profile; bool incognito;
  // kPeek / kReaderPanel
  WindowId opener_window; TabId opener_tab;
  // restore: previous session's id (>0), -1 stand-in, 0 not restore-driven
  int32_t restored_from;
  // the request token the client passed when it asked for this window
  uint64_t request_token;
};
virtual void OnBrowserWindowCreated(const WindowCreation&) {}

// Every creating call takes/returns a token the announcement echoes:
struct CreateRequest { WindowKind kind; ProfileId profile; bool incognito; /* … */ };
virtual uint64_t RequestBrowser(const CreateRequest&) = 0;   // replaces NewWindow/CreateSpaceBrowser(InProfile)/CreateAgentBrowser/CreateShadowBrowser
```

With a token, `CreateAgentBrowser`'s synchronous-re-entrant contract becomes unnecessary: the client keys `pending_agent_task_` by token instead of by "the callback that fires during this call", and the framework is free to post the announcement.

```cpp
// --- Threading contract (header prose) ------------------------------------
// 1. Every call in both directions is on the browser UI thread.
// 2. Delegate callbacks are NOTIFICATIONS: they are dispatched after the
//    framework state they describe has committed, from a fresh task, never
//    from inside a TabStripModel/BrowserList/WebContents observer walk.
//    Consequently any PhiFramework method may be called from any callback.
//    Exceptions are marked ASK: OnColdStartRestorePlan, OnExtensionMessage.
// 3. Scratch buffers (pixel returns, const char* returns) are valid until the
//    next PhiFramework call — including calls made inside a callback.
// 4. No callback is delivered re-entrantly from inside a PhiFramework call.
//    A creating call returns a token; the announcement follows as a task.
```

### 5.4 Client-side model that consumes it

* `PhiWindowModel` (new, owned by `PhiCoordinator`, keyed by `WindowId`): the last `WindowSnapshot` per browser plus derived indexes (`tab_by_id`, `tabs_by_tag`, `active_tab`). Replaces `ReadStrip`, `TabRowsOf`, `LiveTab`, `TabUrlAtIndex`, `IndexOfTagged`, `SplitIdForTab`, `tabs_of`, `ActiveTabId`, `LiveSplitLayout`, and the 29 raw parses. Popovers (`phi_group_overview.cc`, `phi_tab_switcher.cc`, `phi_menu_bar.cc`, `phi_peek_panel.cc`, `phi_reader_panel.cc`) take `const WindowSnapshot&` instead of a `PhiFramework*`.
* `SidebarInputs` (`phi_sidebar_model.h:110`) is built from the model in one place; `RebuildTabs` no longer touches `framework_` for state, only for pixels.
* `OnWindowChanged` → `model.Update(snapshot)` → `controller->ScheduleRefresh()` stays coalesced but no longer needs to be a *deferral*; the `WM_PHI_READER_SWEEP` handler becomes a direct call once rule 2 holds.
* `OnBrowserWindowCreated(const WindowCreation&)` → `switch (kind)`; `pending_agent_task_`, `pending_shadow_task_`, `pending_slot_`, `pending_space_`, `pending_open_url(s)_` become a `std::map<uint64_t token, PendingCreation>`.
* `browser_to_slot_` stays a client concept but becomes `std::map<WindowId, SlotId>` with `struct SlotId { WindowId v; }` so the seven `find-or-fallback` sites become one `SlotFor(WindowId)`.

### 5.5 Migration cost and order

1. **Ids (d)** — one header, mechanical; can land first with `explicit` constructors and `int` accessors so both sides compile with `TabId{n}`. ~1 day each side. Mac: Swift side already has typed ids; the C bridge header gets the same structs.
2. **Threading (e)** — post `OnTabsChanged` after commit in the fork (one change in the strip observer), write the four rules, then delete `WM_PHI_READER_SWEEP` deferral and keep `kMsgPhiRebuildRows` only as paint coalescing. Must be verified on the box against the reader-panel sweep and drag-drop paths which currently assume mid-mutation delivery (`phi_coordinator.cc:2439,2444` return early "the strip moved under us").
3. **Snapshot (b)** — add `Snapshot()` + `OnWindowChanged` beside the JSON methods; migrate `ReadStrip` first (one function, `phi_window_controller.cc:2268`), then the coordinator helpers, then popovers; remove the JSON pulls from the Windows client last. The Mac keeps `ListTabsJson` until it migrates; the fork fills the struct and serialises the JSON from the *same* struct so they cannot drift.
4. **Creation (c)** — additive `OnBrowserWindowCreated(const WindowCreation&)` overload with the old one defaulting to call it; four getters deprecated; client `switch` replaces the 160-line dispatcher.

Order 1→2→3→4 keeps every step shippable; 3 is the bulk (est. the 29+5+5+4+7 parse sites plus ~40 index-addressed calls that should move to ids at the same time).

---

## 6. Ranked findings

1. **`ListTabsJson` is parsed at 29 sites in 7 files with 4 row structs and 6 mini-parsers; three of the fields the client depends on (`unloaded`, `surface`, `originTabId`) are absent from the header.** `phi_window_controller.cc:2254-2315`, `.h:891-937`; `phi_sidebar_model.h:85-96`; `phi_coordinator.h:551-555`, `.cc:1222,1360,1478,1855,1913,1958,2038,2247,2379,2473,3116,3468,4093,5777,6095,8334`; header `:322-328` (documented shape). Every drag path re-reads the strip several times inside one gesture (`phi_window_controller.cc:3840,3868,3984,4101,4120,4129,4151`).
2. **Creation intent leaks through four validity-windowed getters plus three client pending-globals; `OnBrowserWindowCreated` reconstructs seven kinds.** Header `:601-625,1009-1014`; `phi_coordinator.cc:7529-7692`, `.h:714,749-771`; agent/shadow adoption relies on re-entrancy `:6134-6141,6791-6798,7601-7613`.
3. **The threading contract is one sentence and does not cover re-entrancy; the client owns five posted-turn messages to compensate and one path that depends on the opposite.** Header `:32-33,206,540-541,914-915`; `phi_coordinator.cc:144-158,321-355,2342-2352,8042-8058`; `phi_window_controller.h:347-354,498-505`, `.cc:5403-5414,11171-11179,11415-11423`.
4. **Untyped, mixed-width ids across seven address spaces; the client's slot/browser distinction is invisible to the framework and re-derived at every callback.** Header `int` vs `int64_t` tab ids (`:192,269,382,496,762,849,1005,1028-1050` vs `:154-168,624,1013`); window ids as `int` (`:212,594,611,655,662`); client `static_cast<int>` at `phi_coordinator.cc:2596,2602,2607,2201,2452,7588`; `browser_to_slot_` fallbacks at `:7892,7902,7913,8050,2159`; `FindDouble("windowId")` ×28 vs `FindInt` ×1 in `phi_coordinator.cc`.
5. **Async results are correlated inconsistently: `int request_id` (reader, feedback), `const char*` request id (extensions), nothing (`CreateProfile`, `NewWindow`, `CreateSpaceBrowserInProfile`).** The client discovers a new profile three ways (`phi_coordinator.cc:390-407,422-427` event; `:4289-4300,4534-4553` list-diff on a 500 ms timer; header `:550-551` says "a later ListProfilesJson") and serialises creates (`:4956-4958`). `CreateSpaceBrowserInProfile` is matched by `pending_slot_` (`:7639`).
6. **Two silent async side-effects masquerade as unrelated notifications.** `FaviconForUrlBgra` null → later `OnTabsChanged` (header `:502-504`) triggers a full sidebar rebuild; `ListExtensionsJson` empty-then-refresh (`:856-859`).
7. **Index-addressed methods still dominate the client (≈40 calls in 6 files) although id twins exist; the client's own `PhiCoordinator::ActivateTabById` re-implements the framework's by parsing JSON.** `phi_coordinator.cc:2260-2267`; index calls e.g. `phi_window_controller.cc:6793-6870,8106-8428`, `phi_menu_bar.cc:810,900,1206,1237`, `phi_group_overview.cc:146,279,289`; header `:1024-1050`. Two of the header's own comments contradict each other on index stability (`:315-316` vs the client's `:2251-2253`).
8. **Two download shapes and two group/split shapes for the same data; the skill dialect renames keys row by row.** Header `:672-704`; `phi_coordinator.cc:8420-8445,8607-8628`; `ListDownloadsJson` undocumented `danger` (`phi_downloads_popover.cc:263`); unchecked `*FindString("name")` deref (`:244`).
9. **Re-serialisation churn: parse→move→re-encode for pass-through lists; a receipt is re-wrapped and re-parsed; the UI talks to itself over the agent wire.** `phi_coordinator.cc:2137-2149,5776-5780,5961-5964,8536-8539,8682-8686`; `:398-418` (string `find` on serialised JSON); `:4314-4322,4556-4568,4606-4623`.
10. **Dead surface: five framework methods with no caller (`CreateSpaceBrowser`, `DistillTab`, `GetRestoreOnStartup`, `DeleteOmniboxMatch`, `MoveTabAfter`); `SetRestoreOnStartup` is called without its getter.** Header `:527,599,796,819,1047`.
11. **`WindowProfileJson.incognito` is the sole incognito signal and is undocumented.** `phi_coordinator.cc:7679-7681`; header `:562-563`.
12. **Sentinel drift: missing `index` defaults to `0` (acts on tab 0) at two mutation sites.** `phi_coordinator.cc:2263,2443`.
13. **Tri-state `const char* group_id` (`nullptr`/`""`/id) in `MoveTabBefore/After` is invisible at call sites.** Header `:1039-1041`; `phi_coordinator.cc:1535,1553`.
14. **`OnColdStartRestorePlan`'s out-param-plus-armed shape exists only because delegate defaults must be empty bodies; a `std::optional<std::vector<WindowId>>` return with a documented default would say the same thing.** Header `:198-213`.
