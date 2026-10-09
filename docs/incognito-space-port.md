# Incognito Space — porting the Mac feature to phibrowser-win

2026-10-09. Research of phibrowser-mac HEAD (8beec86c) and phibrowser-win
(0e480f2), plus the plan this port follows.

## 1. What the Mac does

An **Incognito Space** is a Space in the window's own Space strip whose
browser runs in a dedicated off-the-record profile. It is NOT the standalone
incognito window (Cmd+Shift+N), which both clients keep.

* **Created** from File ▸ New Incognito Space (⌃⇧N, rebindable), the
  profile menu, AppleScript, or on demand when a URL rule / the ask chooser
  targets Incognito and none is live (`SpaceManager.createIncognitoSpace`,
  `resolveIncognitoRouteTarget`). The new Space joins the FOCUSED window's
  strip and that window switches to it. Not offered in the create-Space
  panel.
* **Many**: id `space.incognito.<UUID>`, named "Incognito" while it is the
  only one, else "Incognito N" (lowest free ordinal, stable). Icon 🥷
  (`emoji:1F977`), colour #5F6368. Placed after the user Spaces, before
  agent pips. Rename and delete are not offered (delete = close).
* **One OTR session**: every Incognito Space shares the named OTR profile
  "PhiIncognitoSpace" of the initial ("Default") profile
  (`phi_profile_manager.cc`), separate from the primary OTR the standalone
  incognito window uses. The OTR profile is destroyed when its last browser
  closes.
* **Runtime only**: never stored, synced, listed in Settings/Library or the
  agent API, never a landing/default/retreat target; a slot showing one is
  persisted as its last regular Space. Nothing restores after quit.
* **Closing**: Cmd+W / ✕ on the LAST tab, or "Close Incognito Space" in the
  tab-area context menu, asks "This will also close this Incognito Space,
  are you sure?" — Close (default) / Cancel, "Do not ask again" checkbox
  (`suppressCloseIncognitoSpaceWarning`, synced). Close retreats every slot
  showing it to a regular Space, closes its windows and drops the entry.
* **Look**: fixed dark incognito theme regardless of app theme; native
  incognito NTP ("Incognito" artwork, tab title "New Incognito Tab"); no
  pinned / bookmark bands, no bookmark bar, no AI/memory, no Peek, no
  content-blocking row, bookmark/import commands refused.
* **Routing**: the generic rule target `space.incognito` resolves to the
  first live Incognito Space (creating one if none); navigation is never
  routed OUT of an OTR profile. "Move to Space" from a regular Space lists
  Incognito Spaces (a re-open in the OTR profile + close).

## 2. What Windows has (0e480f2)

* Standalone incognito WINDOWS (Ctrl+Shift+N, "Open link in incognito
  window", the URL-rule Incognito target via `OpenUrlOffTheRecord`), hosted
  as an `incognito_` shell: fixed-dark palette, incognito omnibox glyph and
  NTP, no strip / pins / bookmarks.
* No Incognito Space: `kCmdNewIncognitoSpace` is a dead enum, the
  `app.fileMenu.createNewIncognitoSpace` string is unused, the
  `suppressCloseIncognitoSpaceWarning` pref is stored and synced but read by
  nothing, and there is no close confirmation.
* The fork already builds `phi_profile_manager` / `phi_browser_proxy_factory`
  for Windows, including `ChromiumBrowserType::kIncognitoSpace` and the OTR
  profile's teardown.

## 3. Decisions

* **D1 — runtime entries in the Space list.** An Incognito Space is a
  `model::Space` whose id starts `space.incognito.` held in
  `PhiSpaceManager`'s list like any Space, so pips, switcher, Ctrl+1..9,
  the slide and hover cards work unchanged. `Save`, sync rows, Settings
  summaries, the default role, Kiosk targets and the agent API skip it.
* **D2 — framework.** `CreateRequest{kSpace, incognito=true}` creates a
  `kIncognitoSpace` browser (the fork's shared "PhiIncognitoSpace" OTR of
  the initial profile, loaded first). No new API.
* **D3 — the shell's incognito look follows the ACTIVE Space** as well as
  the window: `ShowsIncognito()` = an incognito window OR an active
  Incognito Space. Strip/Space features stay on in an Incognito Space (it
  is part of the Space world); content features (pins, bookmarks, bookmark
  bar, bookmark/import rows) go.
* **D4 — shortcut Ctrl+Alt+N**, rebindable. Ctrl+Shift+N stays the
  incognito window; Ctrl+Alt follows the existing Ctrl+Alt+R reader chord
  (the Mac ⌃⇧N has no free Windows twin).
* **D5 — entry points**: the ⋯ app menu row "New Incognito Space" under
  "New incognito window", the shortcut, and on-demand creation by the
  `space.incognito` route. "Close Incognito Space" in the tab-area menu.
* **D6 — close confirmation** as a Windows ContentDialog (PhiAlert) with
  the Mac's strings and a "Do not ask again" checkbox writing the existing
  synced pref. Asked on Ctrl+W / ✕ of the last tab and the menu row.
* **D7 — theme**: "Set theme" is not offered for an Incognito Space (the
  Mac offers it but ramps its switch to the incognito theme anyway — an
  inconsistency not ported).
* **D8 — URL rules**: `space.incognito` routes into the first live
  Incognito Space, creating one in the source window when none exists —
  replacing today's standalone-window fallback, as the Mac did in 60acb3d1.

## 4. Phases

1. Framework: incognito kSpace requests (D2).
2. Model: incognito entries, naming, exclusions (D1).
3. Creation: app menu row, shortcut, coordinator `NewIncognitoSpace` (D4,
   D5).
4. Shell look per active Space (D3, D7).
5. Close: last-tab close + menu row + confirmation (D6), slot retreat.
6. Routing + Move to Space (D8).
7. Session restore / quit exclusions; host tests; box verification.

## 5. Status (2026-10-09)

Implemented in one pass (fork + client, uncommitted at the time of writing),
built, deployed to the box and checked there with `test/probes/
incognito-spaces.mjs` / `incognito-route.mjs` and PhiSkillCap captures.

* **Framework** — `CreateRequest{kSpace, incognito}` → `CreateIncognitoSpaceBrowser`
  (fork `kIncognitoSpace`, initial profile loaded first). `MoveTabToWindow`
  now refuses a cross-profile move (a page cannot leave its profile).
* **Model** — `PhiSpaceManager` keeps `incognito_` apart from `model_`:
  `Find` / `Spaces` / palettes / the "Open Link in Space" payload see them,
  Save / sync / Summaries / the default role do not. `model::
  IsIncognitoSpaceId` (prefix `space.incognito.`), naming by lowest free
  ordinal, 🥷, `#5F6368`, the incognito window's fixed-dark palette.
* **Creation** — ⋯ "New incognito Space" (Ctrl+Alt+N, rebindable, row
  under "New incognito window"), `RequestNewIncognitoSpace` (an incognito
  window or agent shell hands it to the first user window; with none, a
  regular shell whose first Space it is — `PhiShellWindow(first_space)`).
* **Look** — `ShowsIncognito()` (window OR active Incognito Space) drives the
  greyed rows, omnibox glyph, incognito NTP, bookmark bar, tab/bookmark
  menus; the sidebar isolates its stored bands; the DWM backdrop's dark flag
  follows the settled palette (`SettledDark`); hover card and switcher hint
  read "Incognito", not the parent profile.
* **Close** — tab-area / pip menu "Close Incognito Space" (rename, theme,
  profile and delete off), Ctrl+W and the row close box on the LAST tab,
  `spaces.delete`, and the coordinator's `DeleteSpace` all end the Space via
  `RequestCloseIncognitoSpace` (slots → last regular Space, browsers
  retired, entry dropped). A windowless Space is reaped when its last
  browser goes another way. `PhiAlertWithCheckbox` is the Mac alert with
  "Do not ask again" writing the synced pref.
* **Routing** — `space.incognito` resolves to the first live Incognito Space
  or a new one (`ResolveIncognitoRouteSpace`), then routes like any Space;
  the routing table aliases the target to an Incognito Space on screen;
  incognito windows are no longer a routing target. A tab moved into an
  Incognito Space is re-opened there and the original closed.
* **Restore** — never noted as the active Space or a Space window.
* **Agent API** — `spaces.list` excludes them (stored list), `spaces.focus`
  reports `isIncognito` and withholds window/tab, `spaces.delete` closes.

Not ported / unverified:

* The close confirmation's pixels: the box account has
  `suppressCloseIncognitoSpaceWarning` on (synced from the Mac), so the
  Ctrl+W path closed without asking — correct, but the dialog itself was
  not seen.
* Mac extras: the account-menu entry (Windows has no account menu), the
  AppleScript/Raycast command (no agent verb was added), the prewarmed
  native tree and the new-pip chip flight.
* Cross-window drag between two Incognito Spaces: the framework's
  cross-profile refusal keeps a drag from a regular Space out; drags between
  Incognito Spaces in two windows were not exercised.
