# Pre-release review — phibrowser-win client

Date: 2026-10-10, against client tip `697507d` (plus the build repo and the
installer). Read-only review in eight lanes: shell window, coordinator/Spaces,
sidebar/strip, sync/storage/auth, control pipe/agents, dialogs/settings,
rendering/page UI, release plumbing. Nothing was built, run or deployed.

**Verified** = I re-read the cited code and the defect holds. **Reviewer** =
found and quoted by the lane reviewer, not independently re-checked. Line
numbers are the current tree.

## Fix status — 2026-10-13

B5, B6, every High, and the Medium/Low items above were fixed in the
working tree (uncommitted). The client builds clean (no warnings) and is
deployed to the box (`phi-fx-ship`). There has been no runtime or box-suite
testing beyond the launch. Still open:

- **Needs a decision, or work outside the client.** B1–B4: channel and
  tenant, production signing, stable feed, crash-upload consent. Also:
  - the Google API keys in the args files;
  - who holds the appcast signing key;
  - `is_official_build` / PGO;
  - whether onboarding pre-ticks analytics;
  - migrating existing installs' `allowAgentUserSpaces=true`.
- **Needs a fork change:**
  - Own browser registration names (`Phi`/`PhiHTM`/`PhiPDF`) in
    `chromium_install_modes`. The installer writes the stock `Chromium*`
    names and skips registration when a real Chromium owns them.
  - Read `phi_telemetry.json` from the install directory (client
    `phi_telemetry.cc` and the fork's `chrome_crash_reporter_client_win.cc`).
  - Add `canResume` to `ListDownloadsJson`.
  - Add a Windows `AllowInspectingTarget` wall (defence in depth; the client
    now filters discovery and session opens itself).
- **Deliberately partial:**
  - Vault helper calls still run on the UI thread, with 3–5 s timeouts.
  - No sentinel Space id for incognito/agent shells; the lookups skip them
    instead.
  - High-contrast palettes don't cover hard-coded RGB in onboarding,
    settings and popup chrome.
  - Box harness verbs (testRig) now need a `build-client.py --dev-hooks`
    build.
- **Translations:** 244 keys were machine-translated into `translations`
  blocks and need review by the translation repo. 16 non-UI keys sit in the
  catalog and should be removed from it.

## Verdict

**NO-GO for a public/stable release as configured today.** Fine as the internal
nightly ring. Six items are blockers; four of them are configuration +
infrastructure (channel, signing, feed, consent), two are code (CDP scope,
data loss on restore).

## Blockers

| # | Area | Defect | Where | Status |
|---|------|--------|-------|--------|
| B1 | Release | Channel is hard-coded `"canary"` → release builds sign in on **staging Auth0** and sync to `sync.stag` / `account.stag`. Flipping later moves users to another tenant (`auth0\|…` subjects differ, `Phi\users\<sub>` is a new empty dir), so this must be decided **before the first public build**. | `phi_version.h:38`, `phi_auth.cc:51-53` | Verified |
| B2 | Release | Only release pipeline signs with the **TEST** Authenticode identity (trusted only on the x64 box) and publishes to public `ota.phibrowser.com`. Every other PC: SmartScreen/UAC "unknown publisher", WDAC rejects. | build repo `ci/test-release.sh:78` | Reviewer |
| B3 | Release | Update feed hard-wired to `win-nightly`; does not follow the channel. A stable install takes every nightly. | `phi_updater.cc:23` | Verified |
| B4 | Privacy | Installer writes `usagestats=1` (HKLM+HKCU) → Crashpad uploads minidumps to Sentry with **no consent and no off switch**; the onboarding analytics choice doesn't touch it. | `phi-installer.nsi:850-851` | Verified |
| B5 | Security | **CDP scope bypass**: `/json/list` is returned unfiltered and `/devtools/page/<id>` opens a session on any target with no scope check (the fork's transport header says the client must filter both). Any admitted agent — even with "agents may operate your Spaces" off — lists every tab incl. Incognito Spaces and gets `Runtime.evaluate` / `Network.getAllCookies` on them. The Mac's `AllowInspectingTarget` wall is `IS_MAC_PHI` only. | `phi_control_server.cc:888-901`, `:1082` | Verified |
| B6 | Data loss | Cold-start restore: a restored window whose Space is known but has no slot yet returns `false`, gets hosted as the **default** Space, and the default Space's real parked window is later orphan-dropped (`DropGhostWindow`) → the default Space's tabs are deleted. Same class: a second window overwrites the default Space's single remembered window, so W1's default browser is dropped on next restore (lane4 KNOWN, now destructive). | `phi_coordinator.cc:4474-4487`, `phi_shell_window.cc:1180-1188`, `phi_ghost_spaces.cc:41-61` | Verified (code path) |

## High — fix before release

### Data, privacy, security
- **Ctrl+D in incognito saves a regular, synced bookmark.** The menu row is greyed, but the accelerator reaches `OnCommand` with no OTR check; `OwnerForSlot` has none either (also covers `AddBookmarkRow`, verbs, import). Fix once in `OwnerForSlot`. `phi_app_menu.cc:1111`, `phi_coordinator.cc:678` — Verified
- **Right-click a cold Space pip → Delete/Rename/Change Profile act on the Space currently shown.** `SwitchToSpace` is async for an unvisited Space; the menu opens bound to the old `active_space()`, and the delete confirm doesn't name the Space. `phi_shell_window.cc:5987-5992`, `phi_app_menu.cc:1054-1058` — Verified
- **Settings ▸ Profiles "Clear browsing data / Privacy / Notifications" open in the wrong profile** (`OpenBrowserPage` → `FirstUserSlot`, profile id ignored). User clears the wrong profile. `phi_settings_window.cc:2074`, `phi_coordinator.cc:2650` — Verified
- **`testRig` honoured in release**; it and `PhiCDPAgentAccessEnabled` live in user-writable `PhiPreferences.json` → a same-user process grants itself `kAll` silently. Gate on `kPhiDevHooks`. `phi_control_dispatcher.cc:603` — Verified
- **"Profiles agents can use" bypass**: empty `profileId` skips the check (empty = first profile); `agentSpace.shadow.create` never checks. `phi_verbs_lifecycle.cc:229`, `:729-770` — Verified
- **`targetId` verbs unscoped**: `readerDocument`, `handleJavaScriptDialog`, autofill operate on any user/incognito tab. One `TargetInScope` check in the dispatcher fixes this and B5. — Reviewer
- **Agent defaults too broad**: every pipe principal gets `kCredentialReveal|Fill`; `allow_agent_user_spaces` defaults **true**; consent/credential prompts are plain BUTTON/LISTBOX that local malware can click. `phi_control_dispatcher.cc:606`, `phi_app_model.h:234` — Verified (defaults)
- **Sync duplicates Spaces on a UI stall**: UI-hop accessors answer "doesn't exist" when the hop times out; the engine then drops mappings and re-creates → duplicates propagate to all devices. Needs a yes/no/unknown result. `phi_sync_spaces_win.cc:50,153,241` — Verified
- **Default-browser registration missing**: the installer writes no `StartMenuInternet`/`RegisteredApplications`/ProgIds; for system-level installs Chromium leaves this to the installer, so Phi never appears in Windows' default-apps list. `phi-installer.nsi` — Reviewer (code-traced, not box-tested)
- **Uninstall/update wildcard deletes in a user-picked folder**: Browse doesn't append `\Phi Browser`; uninstall deletes `$INSTDIR\*.exe|*.dll…`, "delete data" `RMDir /r`s a custom data dir. Install to `D:\Tools` → uninstall wipes it. `phi-installer.nsi:512,790-794,956-975` — Reviewer

### UI
- **Onboarding is a fixed 640×800 (+32 caption) window, no scroll/clamp** → the Continue button sits under the taskbar at 1080p@150 % (the 15" laptop default) and at 1366×768@100 %. Onboarding gates the browser. Feedback form has the same problem (~740 dip and growing). `phi_onboarding_window.cc:40-41` — Verified (constants)
- **Omnibox: hovering a suggestion changes what Enter opens.** `PopupHover` sets `selected_`; Enter commits that row, field text unchanged. Popup appearing under a resting cursor triggers it. `phi_omnibox_session.cc:688-696`, `:847` — Verified
- **AltGr chars hijacked**: AltGr = LCtrl+RAlt, so AltGr+N / AltGr+R (Polish ń, US-Intl ñ / ®) open an Incognito Space / toggle Reader instead of typing. `phi_keyboard_accelerators.cc:160-185`, defaults `phi_preferences.cc:44,63-80` — Verified
- **Sidebar drag "cancel band" doesn't cancel**: `kNone` falls into `default:` in `DragInsertSlot`, resolves a real place, and commit moves the bookmark/pin (persisted, no badge). `phi_sidebar_drag_session.cc:722`, `:2401/2437/2452` — Verified
- **Pinned grid + bookmark outline uncapped and unscrollable** → a long outline pushes every tab and "+ New Tab" off screen. `phi_shell_layout.cc:319-332` — Verified
- **Comfortable strip silently drops overflow tabs, including the active one** (~15+ tabs at 1280 px); "future work" comment, not in the D-list. `phi_sidebar_view.cc:1132,1168` — Verified
- **Settings has no keyboard navigation at all** (no focus/Tab/Enter); UIA drops rows scrolled out of view. `phi_settings_window.cc:362` — Reviewer
- **i18n**: ~120 English literals in Settings beside `PhiStr` (rail labels, "Restore last session", "Delete" on destructive confirms); **260 catalog keys are en-only** (whole sync setup, Sync pane, analytics toggle, account errors). — Reviewer

## Medium

**Coordinator / Spaces**
- Incognito tab dragged onto a *cold* regular pip is re-opened in the regular profile and the original closed (`phi_coordinator.cc:2322-2345`).
- Kiosk opened from incognito can "Expand" into a regular Space (`phi_kiosk_window.cc:766-785`).
- `RouteUrlToSpace` / `BrowserForSpace` can pick an incognito or hidden agent shell (they nominally hold `DefaultSpaceId()` via `AdoptFirst`) (`phi_url_router.cc:300-312`).
- Ctrl+Alt+N from a lone incognito window hijacks it; `SwitchSlotToSpace` and the `spaces.switch` verb have no slot-kind guard (`phi_coordinator.cc:3996-4008`).
- `SetSpaceProfile` / `RetreatFromMissingSpaces` close a slot's only browser before the replacement attaches → app can exit and tabs are lost (`phi_space_manager.cc:549-555`, `:277-282`).
- Session end handled on an `HWND_MESSAGE` window (MS docs: message-only windows get no broadcasts — the code comment claims otherwise; **verify on the box**). If it is delivered, the *query* alone stops WinSparkle/telemetry/outbox, so a cancelled logoff leaves a half-shut app and `QuitForUpdate` returns early (`phi_coordinator.cc:273-289`).
- Last-active Space recorded wrong on window-close quit (closes in map order) (`phi_coordinator.cc:4971`).

**Sync / storage / auth**
- Any open/read failure (AV lock, sharing violation) is treated as "missing" → prefs / `key_state.json` overwritten with defaults (`phi_file_io.cc:26-53`) — Verified.
- Protobuf group skipper recursion unbounded (`depth` never incremented) → stack-overflow crash on a malformed sync response (`phi_sync_proto.cc:86-114`) — Verified. Same in the SSE JSON skipper (`phi_sync_invalidation.cc:240`).
- Invalidation stream resets backoff on `ready` → server that sends ready+close causes reconnect + full Chromium refresh ~1/s on every client (`phi_sync_invalidation_win.cc:162`).
- Rotated refresh token persisted only when the UI applies the outcome → exit/crash mid-refresh forces re-login (or family revocation) (`phi_auth.cc:696-739`).
- Undecryptable settings entity rewinds the marker → full replay every round (`phi_sync_engine.cc:876`).

**Agent control**
- Vault enumeration (20 candidates) before any prompt; prompt doesn't list requested fields (`phi_credential_service.cc:138`).
- `BroadcastEvent` blocks the UI thread on a channel write lock (up to 30 s) and races handle teardown (`phi_control_server.cc:1264`, `:1055`).
- Vault helper calls on the UI thread with a 30 s timeout (`phi_credential_service.cc:86`).

**Shell window**
- Alt+Tab between/into Phi windows pops the ⋯ menu (`alt_solo_` not reset on activation).
- `WM_SIZE` lays out when minimized → 0×0 child; close-while-minimized saves a junk placement (`phi_shell_window.cc:8017`) — Verified.
- Floating-sidebar popups (context menu, preview, downloads) offset ≈(−8, −52) dip: panel-local coords mapped through the shell HWND.
- Address-field focus lost on re-activation (Alt+Tab back, paste goes to the page).
- Tear-off onto a different-DPI monitor keeps the source DPI.
- Stale caption-button press can later close/min/max the window (`phi_caption_band.cc:146-171`).
- Dangling `chip.id` reference after `chips_.clear()` (`phi_shell_window.cc:5979`) — copy the id.

**Sidebar / strip**
- Vertical list never scrolls the active tab into view, no scrollbar, one scroll offset shared across Spaces.
- Bookmark manager hit-tests rows hidden under the header → Delete removes an invisible bookmark without confirm.
- Favicon backfill: textual private-host filter (DNS to 127.x, redirects, `0x7f.1`) + unsandboxed WIC decode with no dimension cap; URLs come from sync.
- Middle-click closes tabs while the agent holds input (`phi_sidebar_view.cc:553`).
- Ctrl+Tab HUD stays topmost if focus leaves while Ctrl is held; no Esc; 7 cards overflow 1366 px.
- Group overview can't scroll ("+" unreachable); every sidebar rebuild base64-encodes all stored favicons.

**Dialogs / settings**
- Settings changes a Space's profile with no confirmation (menus confirm).
- Feedback: page title still sent after the user clears the URL; outbox silently deletes a "sent" report after ~30 min of failures; signed-out jobs linger on disk.
- Sync recovery code copied to the clipboard without the exclude-from-history/cloud formats.
- Esc discards a typed feedback report / unsaved URL rules without asking.
- `SW_SHOW` doesn't restore a minimized Settings / URL rules / sync setup / create-Space window.
- Rename prompts accept whitespace-only and unbounded names.

**Rendering / page UI**
- Paste-and-go runs pasted `javascript:` (Chrome strips it) (`phi_omnibox_session.cc:287-345`).
- "All Downloads" replaces the current tab; completed downloads can't be opened from the popover (`phi_downloads_popover.cc:641`) — Verified.
- **No tooltips anywhere** — `EnableTooltips` is never called (`phi_window.h:563`) — Verified.
- Colour emoji erase the rounded hover fill in the icon picker (`phi_dwrite.cc:387`).
- Peek/Reader destructor `SetParent(nullptr)` on a visible `WS_CHILD` → page flashes at desktop top-left.
- Region (stair-step) corners on peek, reader, NTP split, panel — violates the AA house rule.
- NTP search EDIT keeps stale colours after a theme change.

**Release plumbing**
- Uninstall doesn't check Phi is running → orphaned half-install.
- Admin-only per-machine install: HKCU / `%LOCALAPPDATA%` writes land in the *admin's* profile on over-the-shoulder UAC; other users get no telemetry config; standard users can't apply updates.
- Google API key / OAuth secret committed in `args/PhiFxWinShip.gn` (prefix matches Chrome's published keys).
- One Ed25519 appcast key on the test runner; signature doesn't cover the version; installer accepts downgrades.

## Low / polish (condensed)

- `is_official_build = false` (no PGO/ThinLTO); version still 0.0.4.
- Onboarding analytics pre-ticked outside the EU (by user-settable geo); confirm with legal.
- `phi_reader.cc:259` logs full URLs; settings logs an exe path with the username — both land in the feedback zip under `--enable-logging`.
- `release-gate.py` checks none of channel / feed / signing identity.
- High-contrast mode ignored by every custom-painted surface; no `SPI_SETHIGHCONTRAST` re-poll.
- `&` in user labels (AT&T, R&D) treated as a mnemonic in menus/pills.
- Inert placeholder rows in Settings ("Shorten links…", "What's new", "Documentation").
- No sidebar keyboard navigation at all (F6 / arrows / Enter / Shift+F10); drag auto-scroll (D31).
- Folder icon picker failure path never calls back → modal loop hangs and eats `WM_QUIT`.
- URL-rules / create-Space windows leak on close (no `DeleteWhenDestroyed`).
- IDN URL rules never match (Unicode vs punycode).
- No `WM_GETMINMAXINFO`; maximized window hides an auto-hide taskbar.
- Floating sidebar repaints the whole chrome ~33×/s while idle.
- `screenshotBrowser` skill helper broken in release (`outPath` ⇒ `kHost`).
- Download progress unclamped; extension badge width unbounded; square GDI rims inside DWM-rounded popovers.

## Checked and fine

Pipe transport (SDDL owner/SY/BA + NW label, `PIPE_REJECT_REMOTE_CLIENTS`,
first-instance, constant-time token compare, frame caps, depth-limited JSON);
skill installer does no downloads; no secrets in git; appcast over HTTPS with
EdDSA required; dev-hook env overrides compiled out and gate-checked; analytics
default off with allow-listed properties; atomic temp+`ReplaceFileW` writes;
DPAPI for tokens/keys; PKCE S256 + state check; TLS never disabled; random
AES-GCM nonces; D2D device-loss rebuild; COM/GDI lifetimes in the substrate.

## Suggested order

1. **Decide channel + tenant** (B1/B3), wire both from one build input, extend
   `release-gate.py` to assert channel, feed and signing identity.
2. **Production signing cert + stable feed + separate release workflow** (B2).
3. **Crash consent** tied to onboarding + a Settings toggle (B4).
4. **One `TargetInScope` helper** for `/json/list`, session open, and every
   `targetId` verb; gate `testRig` on dev hooks; default user-Spaces off (B5 + highs).
5. **Restore data-loss** (B6): host the ghost under its own Space when no slot;
   remember windows per (Space, slot); never orphan-drop a profile-bound window.
6. **One OTR gate in `OwnerForSlot`** + source-incognito checks on routing / drag / Kiosk.
7. UI highs: onboarding/feedback clamp+scroll, omnibox hover, AltGr, cold-pip menu,
   drag cancel band, sidebar band cap, strip overflow, Settings profile links.
8. Installer: default-browser registration, safe deletes, running-check on uninstall.
