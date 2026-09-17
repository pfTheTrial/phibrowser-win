# Windows-native UI — implementation log

Status: **in progress** (started 2026-09-17). The design is
`docs/windows-native-ui.md`; this file is the build log Jixiang reviews:
which phase landed, what each one changed, and every decision that was
taken without asking (§D — check these). Phases follow the design's §10
order; each leaves the box build usable.

Jixiang's brief (2026-09-17): "make a plan to impl the ui one by one; if
there is a decision you need me to make, decide yourself first, document
it; after you have implemented the full UI, show me the questions and
decisions and I will check." The box is mine for testing.

## Phase status

| Phase | What | State |
|---|---|---|
| P0 | tokens, type ramp, icon font, `PaintIconButton`, palette roles | done 2026-09-17 |
| P1 | app menu (⋯) replaces the menu bar; Fluent owner-drawn menus; Alt / F10 | done 2026-09-17, box-verified |
| P2 | the two layouts, flush page pane, Performance removed, floating panel | done 2026-09-17, box-verified (split-time row sheet, Esc → P6) |
| P3 | Spaces: pips, chip + switcher, two-line hover flyout | done 2026-09-17, box-verified |
| P4 | dialogs as ContentDialogs | done 2026-09-17, box-verified (scrim not done) |
| P5 | Settings as a Windows Settings window | done 2026-09-17, box-verified (expander cards, rail search, remembered size: round B) |
| P6 | secondary windows, Kiosk, dark pass, polish | done 2026-09-17 (partial, see notes) |

## D. Decisions taken without asking (review these)

* **D2 — dialog button order: Windows** (affirmative left, Cancel right).
  The design's recommendation; dialogs are where Windows habit is
  strongest. Keyboard: Enter = default (never on destructive), Esc =
  Cancel.
* **D4 — floating panel trigger: edge hover + the toggle's right-click**
  menu (Show sidebar / Auto-hide sidebar). Left-edge hover slides the
  tucked panel in; the toggle button (ClosePane/OpenPane) docks it.
* **D5 — no Mica.** Flat Space tint. Not revisited.
* **D6 — menus stay HMENUs, owner-drawn.** Every menu (the app menu, its
  submenus, every context menu) is a Win32 popup menu with
  `MFT_OWNERDRAW` rows and a `MENUINFO` background brush, painted to the
  Fluent look (34 dip rows, 16 dip icon column, right-aligned chords in
  the tertiary ink, danger rows, accent check). Keyboard navigation,
  mnemonics, submenus, `TrackPopupMenu`'s modal loop and the remote
  `menu.list` / `menu.invoke` surface come for free; the popup frame
  (rounded, shadowed on Windows 11) is the OS's. A custom popup window
  would have cost all of that for a 1 px border colour.
* **D7 — the zoom row is a submenu**, `Zoom ▸ Zoom in · Zoom out · Actual
  size · ─ · Full screen`, with the current level in the parent row's
  chord column ("100 %"). The design allowed this fallback; an inline
  three-target owner-drawn row cannot take keyboard focus per target.
* **D8 — icon font chosen at start-up**: Segoe Fluent Icons when the
  face is installed, Segoe MDL2 Assets otherwise. No third fallback. The
  Space icon catalogue masks and the agent brand masks stay compiled.
* **D9 — Performance maps to Balanced at load** and on the wire:
  `PhiLayoutModeFromName("performance")` answers `kBalanced`; the enum
  keeps the value so the Mac's stored setting still parses. The Settings
  radio, the app menu's Layout submenu and the onboarding page show two
  choices. `agentSpace.layout.set {mode:"performance"}` is accepted and
  lands on balanced.
* **D10 — the remote menu paths use the app menu's English labels**
  (`["More tools","Task manager"]`, `["New tab"]`), as the design says;
  `test/suite-menu.mjs` is rewritten to them.
* **D11 — Chat stays out** (`kPhiAiEnabled` paths untouched, no menu rows
  for it). The Feedback pill stays in the sidebar bottom bar.
* **D13 — dark menu frames through uxtheme's undocumented
  `SetPreferredAppMode` / `FlushMenuThemes`** (ordinals 135 / 136,
  Windows 10 1903+), called when a themed menu opens. The documented API
  cannot recolour a popup menu's frame or its submenu arrows; every
  dark-mode Win32 app (Notepad++, Explorer++) uses this pair. A missing
  export leaves the frame light and the rows still ours.
* **D14 — the app menu's translations are authored in this repo**
  (`strings-source.json` `translations` blocks for de / es / fr / ja / ko
  / nl / zh-Hans / zh-Hant): the Mac catalogue has no sentence-case
  counterparts to join against. The translation repo can overwrite them.
* **D15 — the incognito row stays live** through Chromium's
  IDC_NEW_INCOGNITO_WINDOW route, as the menu bar's was (the design
  said "disabled until supported"; the route already works).
* **D17 — RETIRED (round B).** Settings › Spaces / Profiles are the §7.1
  / §7.2 expander cards now (see the round B notes). The one liberty:
  the expander hint reads "profile · Ctrl+n" — the settings host has no
  per-Space tab count, so "N tabs" is left out rather than plumbed.
* **D18 — RETIRED (round B).** The rail has "Find a setting": typing on
  any page but Shortcuts filters every page's rows by title and
  description into one results page, under per-page headings.
* **D21 — the Settings size is remembered as a preference**
  (`settingsWindowWidth` / `settingsWindowHeight` in PhiSettings.json,
  written on WM_EXITSIZEMOVE, never for a maximized window); no
  position, which Windows Settings also does not keep.
* **D23 — the omnibox's context menu is the Fluent flyout**, replacing
  the EDIT control's stock menu rather than adding one row to it (§3.5
  said "the native EDIT menu plus one Phi row"): one menu family
  everywhere, and the stock menu cannot be themed dark.
* **D22 — a Space's Profile is a ComboBox in its expander** backed by
  the coordinator's existing `SetSpaceProfile` (the Space's browser
  respawns in the new profile); the default Space refuses with a
  notice, as the coordinator does.
* **D19 — the You page keeps the id `account`** on the remote surface
  (verbs, suites, rigs aim at it); only its label is "You".
* **D20 — the rail has no account block**; the "You" row is the way to
  the account page.
* **D16 — the Space switcher stays a menu.** It is now the Fluent
  owner-drawn menu (icon column, checked row semibold, "New Space…"),
  not the §5.3 custom flyout with 40 dip two-line rows: the design's
  reason for a custom popup (the HMENU look) went away with D6, and a
  menu keeps keyboard navigation. Revisit if the hint line ("profile ·
  N tabs") is wanted in the switcher.
* **D12 — the horizontal layout keeps its Chrome shape** (strip row + full
  toolbar row) and gets the Space chip at the strip's leading edge in
  place of the compact pip; the overflow ⋮ becomes ⋯ (`More` E712) — the
  same button, restyled.

## Phase notes

### P0 — tokens (2026-09-17)

* New `client/phi_fluent.{h,cc}`: the glyph map (`fluent::k*`, Segoe
  Fluent Icons / MDL2 code points), the three icon sizes and the 32 dip
  button, `PhiPaintIconButton` / `PhiPaintButtonWash` / `PhiButtonInk`
  (the §2.2a states), `PhiFluentFont` (Segoe UI Variable Text / Display,
  Segoe UI on Windows 10), `PhiIconFont`.
* `phi_dwrite`: `PhiIconFaceName()` picks Segoe Fluent Icons when
  installed, MDL2 otherwise; every literal `L"Segoe MDL2 Assets"` in the
  client now goes through it (12 files). `PhiDrawTextAA` accepts either
  face.
* `PhiPalette` gains `flyout`, `dialog_footer`, `stroke_strong`,
  `accent_soft`, `control_fill`, `pressed`; the ladder is retuned to §2.3
  (hover 4.5 %, pressed 9 %, stroke 9 %, separator 7 %, label 87 %, dim
  62 %, tertiary 42 %; dark card `#2B2B2B`); `ground_fixed` is the
  Windows Settings ground `#F3F3F3` / `#202020`; `destructive` is
  Windows' `#C42B1C` / `#E0463A`. `util_test.cc` pins them.

### P1 — the app menu (2026-09-17)

* New `client/phi_app_menu.{h,cc}` (from `phi_menu_bar`, which is
  deleted): one popup tree in the §3.1 order with the submenus Spaces /
  History / Bookmarks / Extensions / Zoom / Layout / More tools / Help;
  the command handler, the bookmark exporter and the Chromium command
  routes carried over unchanged. `PhiMenuBarShell` became
  `PhiAppMenuShell` (+ `MenuTriggerExtension`, `MenuSidebarShown`).
  Extensions ▸ lists the framework's `ListExtensionsJson` rows and runs
  an action anchored to the ⋯ button; Manage extensions opens
  chrome://extensions. New ids appended: `kCmdShowAppMenu`,
  `kCmdManageExtensions`.
* New `client/phi_menu_paint.{h,cc}`: the Fluent owner-drawn HMENU rows
  (D6) — `PhiMenuRow` / `PhiMenuRowStore`, `PhiMenuApplyTheme`
  (MENUINFO flyout brush + the uxtheme dark-frame hook),
  `PhiMenuOnMeasureItem` / `PhiMenuOnDrawItem` / `PhiMenuOnMenuChar`.
  The context menus (`phi_shell_menus.cc`'s runner) use the same rows;
  a `kRowDecor` table adds their chords, icons and danger rows by key
  so the pure builders and `shell_menus_test.cc` stay untouched.
* `PhiCaptionBand` keeps the frame and the caption buttons only: the
  titles, the reveal state machine, the Phi button press-as-grip and the
  overflow are gone. `ShellLayout` lost `phi_menu` / `band_limit`,
  `overflow` became `app_menu` and every layout lays one out (the
  vertical band's cluster: search-tabs · ⋯ · divider · caption).
  Nav-hover ordinal 12 (the Phi button) is an empty rect so the rigs'
  ordinals hold.
* Keyboard: F10, Alt+F, Alt+E in the fixed accelerator table and the
  solo Alt tap all post `kCmdShowAppMenu`; `kMsgPhiToggleMenuReveal` is
  gone.
* Strings: 64 `appMenu.*` keys in `strings-source.json` (sentence case,
  chords in the `\t` suffix, translations authored here — D14) and the
  catalogue regenerated.
* Remote surface: `agentSpace.menu.list` returns the root's rows (with
  `shortcut`), `menu.click` takes root-relative paths;
  `agentSpace.window.layout` reports `appMenu`. `test/suite-menu.mjs`
  rewritten (7/7 pass on the box); new `test/probes/app-menu-rects.mjs`.
* Box check: PhiRm shots of the open app menu and of the sidebar's
  context menu at 125 % — the flyout ground, icon column, chords, the
  system's submenu arrows and the disabled row all read as designed.
* Left for later phases: the context-menu labels are still Title Case
  English literals (P6 moves them to `strings.json`, §3.5); the §3.5
  additions (Move to new window, New tab in group, Paste and go, the pip
  right-click Space submenu, the system menu on the band run) are P3/P6;
  the zoom level readout needs a framework query the API lacks.

### P2 — the two layouts (2026-09-17)

* `phi_shell_layout.{h,cc}` rewritten: `shell_metrics` carries the §2.1
  values (44 address row, 40 strip / toolbar rows, 32 buttons 4 apart,
  32 dip r8 omnibox with 24 dip in-field slots, 28 pips, 44 pinned
  cells, 28 bookmarks bar, 40 bottom bar, 28 dip section labels, 32
  drag gutter); the Performance branch, the sidebar address bar, the
  copy-link slot and the card margins are gone. Vertical = sidebar
  (header row of pips only, Pinned / Bookmarks / Tabs labels, bands,
  bottom bar with Feedback) beside a flush pane whose 44 address row
  holds toggle · search · nav trio · omnibox · drag gutter · pinned
  extension slots · extensions · downloads · ⋯ · divider · caption
  buttons. Horizontal = strip row (pip, tabs, +, search, divider,
  caption) + toolbar (trio, omnibox, cluster) + optional bookmarks bar +
  edge-to-edge page. `PhiLayoutModeFromName("performance")` answers
  balanced (D9); `ComputeShellLayout` treats a stale `kPerformance` as
  balanced. `shell_layout_test.cc` rewritten (11 cases).
* Shell paint: flat tint (no gradient); the pane's left corners at r8
  with a 1 px stroke and a three-ring shadow; the address row on `card`
  with the caption buttons painted after it; every toolbar button
  through `PhiPaintIconButton` (Back / Forward / Refresh-or-Stop /
  Search / Add / More / ClosePane-OpenPane / Download / Puzzle /
  ReadingMode / FavoriteStar / Feedback); the omnibox with the resting
  `control_fill`, hover a step darker, focus = card + stroke + 2 px
  accent bottom edge, the globe site slot leading, the star trailing
  (Bookmark this tab, nav-hover ordinal 15 — `kCmdBookmarkThisTab`);
  the Mac masks for downloads / feedback / the toggle are no longer
  drawn. Corner caps: top caps never, the bottom-right cap only when
  the pane is not flush with the window (`PhiContentLayout.right_caps`).
  `phi_split_view_style.h` (fork): pane inset 0, radius 8.
* Floating sidebar (§4.4, D4): the panel is a persistent `WS_POPUP`
  while the sidebar is hidden — TUCKED at x = 8 − width (8 dip
  peeking), y 52 to h − 8, `CS_DROPSHADOW`, DWM-rounded, 1 px
  `stroke_strong`; a 300 ms dwell on the peek or the frame's HTLEFT
  hover slides it in (150 ms ease-out on the tick timer); it tucks 150
  ms after the pointer leaves (min 500 ms shown), on a chrome click, on
  deactivation; it stays while a drag or a modal menu is up; it follows
  `WM_MOVE` / `WM_SIZE` and hides while minimised. Inside: the docked
  sidebar laid out for the panel's own height (`floating_panel_h_`)
  with the header row always present and a **pin** button trailing it
  (`ShellLayout::pin`, `ShellLayoutInputs::floating_panel`); the tucked
  edge and the pin dock the sidebar. `PhiOmniboxSession::HostField /
  UnhostField` deleted (the field never leaves the address row).
  `SetSidebarCollapsed(bool)` is the one setter; `ToggleSidebar` flips it.
* Remote surface: `agentSpace.layout.set {sidebar: "docked"|"hidden"
  [, windowId]}` and `window.layout.sidebarFloating`. `test/suite-
  layout.mjs` rewritten (7/7 on the box); `test/probes/layout-set.mjs`.
* Strings: `sidebar.section.pinned / bookmarks / tabs` (D14).
* Box checks: vertical and horizontal PhiRm shots at 125 %; the panel
  tucked → slid in (pin, labels, rows) → tucked again through PhiDragCap.
* Left for later: the address row as its own sheet while split and the
  split hover chip (§4.6, P6 with the fork), Esc tucking the panel, the
  panel's slide-in fade, the Settings / onboarding layout radios still
  list three (P5 / P6), the PhiCap layout-cycling rig still switches to
  performance (box script, not code).

### P3 — Spaces (2026-09-17)

* Pips: 28 dip in the 44 header row, radius 6 (`kSpaceCardRadius`), the
  active pip a `card` tile with the 1 px stroke, hover ink 4.5 % — the
  strip's existing paint on the new metrics. The header row is part of
  the caption row, so `PhiCaptionBand::HitTest` now asks the shell
  whether a pip / the "+" slot is under the pointer there (HTCLIENT),
  leaving the bare run as HTCAPTION; a right-click on that run opens the
  tab-area menu (§3.5), the address row's run keeps the system menu.
* The Space chip (§5.2, horizontal layout): the shell measures the
  active Space's name in the chip's 12/600 face and passes
  `ShellLayoutInputs::space_chip_dip` (8 + 16 + 6 + name ≤ 120 + 4 + 12
  + 8); the strip lays the one chip out at that width and paints it as
  a `card` tile with icon, name and a ChevronDown. Click = the existing
  switcher (now a Fluent menu, D16), right-click = the Space menu.
* The Space context menu (§5.4): right-click on a user pip or the chip
  opens the Space block alone — `PhiTabAreaMenuInputs::space_only` in
  the pure builder (`shell_menus_test.cc` pins it) — after switching to
  that Space; the actions run through `ShowTabAreaMenu`'s dispatcher.
* The hover flyout (§5.6): `MeasureHoverCard` / `PaintHoverCard`
  rewritten — radius 8, `flyout` ground, 1 px stroke, padding 8 × 10,
  200–280 wide, a 20 dip accent tile with the icon, the name 12/600 with
  a `Ctrl+n` keycap trailing (20 high, r4), the hint line 11 dip `dim`
  ("profile · N tabs"; agent Spaces lead with the status dot and word).
  The strip owns the three fonts per DPI; the popup re-bases every rect
  on its own origin. New shell hook `SpacesTabCount(space_id)` (the
  window model's snapshot of the Space's browser).
* Tab preview (`PhiTabPreview`): `flyout` ground, title 12/600, URL 11.
* Menu painter: a row whose picture is a bare icon-face glyph (the
  switcher's "New Space…") draws through the icon face.
* Strings: `spaces.hoverCard.status.*` (5) and the `spaces.hoverCard.
  tabs` plural family (D14).
* Box checks: pip hover flyout, pip context menu, the horizontal chip and
  its switcher at 125 %. `test/probes/space-hover-setup.mjs` seeds a
  second Space and prints the pip / chip aim points.
* Left for later: the "+" pip's 150 ms fade and the wheel scroll are
  the strip's existing behaviour, unchanged; the 40-dip two-line
  switcher rows of §5.3 (D16 keeps the menu rows); the create-Space
  flyout host (§5.5) is P6.

### P4 — dialogs (2026-09-17)

* `phi_prompt.cc`: every `PhiAlert` / `PhiNotice` / `PhiConfirm` /
  `PhiPromptText` / `PhiPromptShortcut` / `PhiPromptChoose*` renders as
  the §6 ContentDialog — no caption band, close button or Phi mark
  (`caption_height_dip` 0, no buttons), 448 wide, body on `card` with
  padding 24, title 20/600, 12 to the message, 16 to an accessory (32
  dip field / recorder, 32 dip list rows), then a footer band on the
  dialog ground with a 1 px separator, padding 24 and EQUAL-WIDTH 32
  dip buttons 8 apart. Actions run left to right in declaration order:
  affirmative first, Cancel last (D2); `PhiAlertAction::leading` is
  gone. Destructive confirms keep no default button.
* `phi_popup_chrome`: the ladder is §2.3's (Windows Settings ground
  `#F3F3F3` / `#202020`, cards white / `#2B2B2B`, ink percentages,
  standard buttons `#FBFBFB` with a 1 px stroke, danger `#C42B1C` /
  `#E0463A`); button radius 4; `PhiPopupFont` is the Segoe UI Variable
  ramp; a secondary window's caption title sits at the leading edge
  after the 16 dip mark (Windows), no longer centred.
* Titles in Windows voice: `dialog.deleteSpace.title` "Delete this
  Space?" and `dialog.deleteProfile.title` "Delete this profile?" (the
  app menu's and Settings' confirms). Other titles unchanged.
* Box check: PhiCap2 commands 1055 (Delete Space) and 1001 (About) —
  the red Delete leads Cancel, the single OK spans the footer.
* Left for later: the owner scrim (a layered smoke window, optional in
  the design), the remaining title rewrites ("Sign out of Phi?",
  "Replace the installed skill?", "Import from Google Chrome?",
  "Updater unavailable").

### P5 — Settings (2026-09-17)

* `phi_settings_window.cc`: a Windows Settings window — resizable
  (min / max / close, 1120 × 760 default, never under 760 × 560), the
  "Settings" title after the 16 dip mark; a **navigation rail** down
  the left (280, a 48 dip icon rail under 900 wide) with 36 dip items
  (16 icon, 14 label, ink 6 % selection with a 3 × 16 accent indicator,
  ink 4.5 % hover) replacing the Mac toolbar; the page column 40 in from
  the rail (≤ 1000 wide) with a 28/600 **page title**, 14/600 group
  headers and **settings cards** (white / `#2B2B2B`, 1 px stroke,
  radius 4, 14 dip rows, 12 dip descriptions, 14 dip padding);
  ToggleSwitch 40 × 20, ComboBox 32 high min 160 on the control fill
  with a stroke, sliders 200, buttons radius 4; the Windows Settings
  ground `#F3F3F3` / `#202020`. The type is the §2.2 ramp
  (`PhiFluentFont`). Every fixed-width `kWidth` / `kMargin` expression
  became the `content_left_` / `content_right_` column.
* Pages regrouped (§7): **You** (`account`), **General** (language, new
  tab behaviour, restore last session, analytics, the single-profile
  detail), **Appearance** (new: Layout with the TWO layout cards
  "Horizontal tabs" / "Vertical tabs", Color appearance, Space color +
  saturation, the Tabs group: tab previews, open indicators, dim
  unloaded, full URL), Spaces, Profiles, Navigation, Phi & AI,
  Shortcuts, **Advanced** (shorten links, auto PiP, developer mode,
  browser settings), Developer, **About** (new: version card with
  "Check for update", What's new / Documentation placeholders).
* The scrollbar: 2 dip at rest, 6 while dragged, thumb DRAG through
  `WM_LBUTTONDOWN` / `WM_MOUSEMOVE` / capture (the wheel as before).
* Remote surface: `agentSpace.settings.get` lists the eleven pages in
  rail order (`appearance`, `about` new; `account` kept — D19);
  `settings.set {section}` takes them. `test/suite-settings.mjs`
  updated (8/8 on the box); `test/probes/settings-open.mjs`.
* Box check: PhiSetShot of General, Appearance, Spaces and About.
* Round B (same day): **expander cards** for Spaces and Profiles
  (`ItemKind::kExpander`, 56 dip header: 28 tile, name 14, hint 12,
  chevron; group header row with "+ New Space" / "+ New profile"; a
  Space's body = Name + Rename…, Profile ComboBox, Icon cell + Change…
  unfolding the four-line picker in place, Color dots, Saturation,
  Shortcut keycap, Delete Space in the danger red; a profile's body =
  Name, Search engine, Download location, Used by, the three data
  pages, Delete profile with the reason it is disabled; the "Default"
  badge on the header), the **About mark at 48** (`kAppMark`), the
  **rail search** (results page "Results for “x”", AddRow drops
  non-matching rows, EndSection drops emptied cards and trailing
  dividers, General's inline profile block and loose hints stay out),
  the **remembered window size** (D21). Box-verified: suite-settings
  8/8, PhiSetShot of Spaces (folded, Work open, picker open),
  Profiles (Default open), About, "peek" and "tab" results.
* Left for later: the account block at the rail's top (D20 — the "You"
  row stands in), the Shortcuts recorder as a §6 dialog (it already is
  a PhiAlert).

### P6 — secondary windows and polish (2026-09-17)

* Secondary windows (Feedback, URL rules, Create Space, Onboarding, the
  transcript) inherit the P4 chrome: the Windows Settings ground, the
  leading title after the 16 dip mark, radius-4 buttons, the ramp.
* Kiosk (§4.7): a 44 dip row, 32 dip buttons through
  `PhiPaintIconButton`, the read-only address with no fill in `dim`;
  the neutral ground comes from the popup ladder.
* Onboarding: the layout step offers two rows — "Vertical tabs" /
  "Horizontal tabs" (the `appMenu.*` strings); Performance is gone.
* Split view (§4.6): the framework's pane inset 0 / radius 8 with a 4
  dip top gutter, so the address row becomes a sheet of its own while
  the active tab is split — its bottom corners round and its hairline
  goes (`ActiveTabIsSplit`). Box check: a vertical split at 125 %.
* Dark mode pass: the omnibox EDIT now paints on the field's own fill
  (control fill / hover / card) in every layout; the flyout menus, the
  panel and the dialogs read their dark values from the palette. Box
  check: the vertical layout and the app menu in dark.
* The Mac masks the Fluent glyphs replaced (`PhiSidebarToggleIcon`,
  `PhiDownloadIcon`, `PhiFeedbackIcon`) are deleted from
  `phi_mac_icons`; the memory and chat masks stay for the AI build.
* After the first commit (b3bb322), the same day: the split view's
  **hover chip** on the unfocused pane (favicon · host · ⋯ pane menu:
  Swap panes / Convert / Unsplit / Close pane; a click on the chip
  focuses that pane; polled while the active tab is split, 600 ms leave)
  — box-verified; the **dialog scrim** (a 30 % layered popup over the
  owner for the dialog's lifetime) — box-verified; **Esc tucks the
  floating panel** (through the keyboard hook's escape ladder, before
  the Peek cards); the **toggle's right-click menu** Show / Hide sidebar
  (D4) — box-verified; **New tab in group** on the group header; the
  context-menu labels in sentence case (still literals, not
  `strings.json`); the bookmarks bar on the 12 dip face with Fluent
  glyphs.
* Round C1 (same day): the **switcher's two-line rows** (§5.3 —
  `PhiMenuRow::hint`, 40 dip, 300 wide: "profile · N tabs" under the
  name, the Ctrl+n keycap trailing; agent rows lead with the status
  word) — retires D16's "menu, not flyout" caveat (it is still an
  owner-drawn HMENU, now with the designed content); **Move to new
  window** on the tab row (Chrome's; `MoveTabsToNewWindowByIds`, the
  tear-off's own call); the **omnibox field menu** as the Fluent flyout
  with "Paste and go" leading when the clipboard holds text (Undo ·
  Cut · Copy · Paste · Delete · Select all with live states — D23: the
  native EDIT menu is replaced outright, not extended, so every menu
  in the app is one family); the **context-menu labels in
  `strings.json`** (`contextMenu.*`, 63 keys, eight translations
  authored, impl log D14) — the host test's stub echoes keys, so its
  label checks name keys now. Box: suite-menu 7/7; PhiRm shots of the
  omnibox menu and the switcher.
* Left for later: the incognito chrome pass, PhiCap re-baselines.

## Questions for Jixiang

Answered by the D-list above; the ones worth a look, in order of how
much they change what you see:

1. **D2 — dialog button order** is now Windows' (affirmative left,
   Cancel right). Keep, or the Mac order?
2. **D17 retired** — Settings › Spaces / Profiles are the designed
   expander cards now; the hint has no tab count (D17's note). Want the
   count plumbed?
3. **D16 — the Space switcher** is a Fluent menu, not the two-line
   custom flyout. Fine, or do you want the "profile · N tabs" hint in
   it?
4. **D9 / D15** — Performance is gone from every surface (Settings,
   onboarding, the Layout menu; a stored "performance" loads as
   Vertical tabs). The incognito window row is live through Chromium's
   command.
5. **D13** — dark menu frames use uxtheme's undocumented ordinals 135 /
   136 (as every dark Win32 app does). OK to ship?
6. **D14** — 70-odd new strings carry translations I wrote; the
   translation repo may overwrite them.
7. **D4** — the floating panel stays tucked with 8 dip showing; hover
   300 ms slides it in; a click on the edge or its pin docks it. The
   toggle's right-click menu ("Show / Auto-hide") is not done.
8. Not done, listed per phase above: the incognito chrome pass (§4.9)
   and the later rounds' items (icon picker flyout, create-Space
   flyout, Peek header, row tiles, agent cues, drag & drop).


(Collected during the work; answered at the end.)
