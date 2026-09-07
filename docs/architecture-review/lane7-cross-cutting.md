# Lane 7 — Cross-cutting concerns, PhiBrowser Windows client

Scope: `/home/jx/Phi/phibrowser-win/client` (116 files, 63,043 lines of .cc/.h). Read-only review; every claim cites `file:line` in that directory unless another path is given. House style read first: `docs/sidebar-architecture.md` (principles §2, "no parallel patterns" rule at docs/sidebar-architecture.md:91) and `docs/i18n.md`.

## 0. Ranked findings (highest first)

| # | Sev | Finding | Evidence |
|---|-----|---------|----------|
| 1 | **Release-blocking** | Updater trusts two environment variables: `PHI_APPCAST_URL` replaces the appcast and `PHI_APPCAST_ED25519` replaces the **signing key**. Any process that can set the user's environment can point the updater at its own feed with its own key. Not gated by build type or developer mode. | phi_updater.cc:113-133, used at 141-147 |
| 2 | **Release-blocking** | Credential vault falls back to a local "dev vault" whenever the helper is absent, and `PHI_VAULT_HELPER` env var overrides the helper binary. The dev vault accepts a **plaintext** `PhiCredentials.dat`. Unconditional in release. | phi_coordinator.cc:250-260; phi_credentials.cc:76-82, phi_credentials.h:29 |
| 3 | **Release-blocking** | Browser process always adds `--remote-debugging-port=0`; the control pipe (103 `agentSpace.*` verbs) always starts, including harness-only verbs such as `agentSpace.feedback.setAccount`. No release gate distinguishes a test box from a customer machine. | phi_client_main_win.cc:385-389; phi_coordinator.cc:278; phi_coordinator.cc:5637-5643 |
| 4 | High | Analytics has **no user consent/opt-in surface**. The only gate is a deployment file (`phi_telemetry.json`). Event properties from the framework are forwarded verbatim with no client-side scrub; a per-install `distinct_id` is generated and attached to every PostHog event. | phi_telemetry.h:21-33, 96-100; phi_telemetry.cc:449-465; no hits for consent/opt-in in any settings code |
| 5 | High | i18n coverage is ~35%: 181 of 521 catalog keys are called; Settings uses 5 keys and ~130 English literals; every context menu in `phi_window_controller.cc` is literal; sentences are concatenated in six files; there is no plural or locale-aware number/date formatting anywhere; zero RTL handling. | §1 below |
| 6 | High | `strings-source.json` was mechanically harvested: it carries an SDDL descriptor, HTTP headers, a registry value name, a MIME type and a company name as translatable strings. Translators will translate `AppsUseLightTheme`. | resources/strings-source.json:111-118, 171-174, 195-198, 239-242, 259-262, 339-342 |
| 7 | High | "System" appearance is only re-resolved when a pref changes; there is no `WM_SETTINGCHANGE`/`ImmersiveColorSet` listener, so a live Windows dark/light toggle is not picked up until the user touches a theme setting. | phi_coordinator.cc:2940-2947, 3912-3928 (the only `ApplyTheme()` callers); grep for `WM_SETTINGCHANGE` returns nothing |
| 8 | High | Framework calls that return `bool` are fired-and-forgotten 187 times (e.g. `ActivateTab` ×9, `SetTabTag` ×10, `CloseTab` ×3, `SplitActiveWith` ×4). A refused action leaves store and strip out of sync with no log. | grep of `^\s+framework_->X(` where X is one of the 50 `virtual bool` methods in phi_framework_api.h |
| 9 | High | 155 `JSONReader::Read*` sites; only 3 log on failure. `PhiPrefs.json` is written non-atomically (`CREATE_ALWAYS` + `WriteFile`, both unchecked) while three other files in the same tree implement atomic replace. | phi_coordinator.cc:2884-2894 vs phi_telemetry.cc:199, phi_feedback_outbox.cc:121 |
| 10 | Med | `LOG(INFO)` leaks the Auth0 subject id and the sign-in redirect URI; `[Reader]` logs page URLs; `[PhiControl]` logs the pipe name. No PII policy exists. | phi_auth.cc:394, 472, 580; phi_reader.cc:258-259; phi_control_server.cc:325 |
| 11 | Med | Nine file-scope mutable globals and two lifecycle methods never called (`PhiTelemetry::Stop`, `PhiUpdater::Shutdown`) — shutdown flush of the analytics spool depends on the batch loop having run. | §5.2 |
| 12 | Med | 13 dead functions, one declared but never defined (`CloseStoredContent`). | §5.6 |
| 13 | Med | Duplicated helpers: 5 file-read loops, 4 file-write helpers, 4 `Widen/Narrow` pairs (two different implementations), 6 colour-blend functions, 3 `Trim`, 3 `Lower`. | §5.5 |
| 14 | Med | 143 hard-coded `RGB(` outside `phi_theme.cc`; three surfaces (Kiosk, Peek, Reader) fall back to `pure/light` regardless of appearance; Settings and popup chrome define their own parallel palettes. | §2 |
| 15 | Low | `kPhiAiEnabled` is a `constexpr bool` — the only feature flag; no BUILDFLAG, no official-build gating anywhere in the client. | phi_features.h:23; only `DCHECK_IS_ON()` use is phi_client_main_win.cc:289 |

---

## 1. Internationalisation

### 1.1 How strings reach the UI

`phi_i18n.h` is a clean facade: `PhiStr(key)` returns a process-lifetime `const wchar_t*` (phi_i18n.h:57-60), `PhiStrF(key, args)` does positional `{0}` substitution (phi_i18n.h:62-68, impl phi_i18n.cc:322-332). Lookup order is active locale → `en` → key itself, with a `LOG(WARNING)` on a miss (phi_i18n.cc:296-320). State is a `NoDestructor<State>` singleton (phi_i18n.cc:41-44) holding all locales' maps, never erased, so pointers stay valid across a switch (comment phi_i18n.cc:24-27). Locale resolution: stored pref → `GetUserPreferredUILanguages` → `en`, with script-aware Chinese matching (phi_i18n.cc:120-171). The catalog is read with raw Win32 I/O to avoid the UI-thread blocking-call check (phi_i18n.cc:46-63, rationale phi_i18n.h:32-35). This is sound.

`tools/gen-strings-catalog.py` joins `strings-source.json` (keys + English) with the Mac `Localizable.xcstrings` by key, else by exact English match, rewriting printf placeholders to `{n}` (header comment lines 5-27; `_PRINTF` regex line 52-53). Nine entries are `verbatim` (menu titles with language-specific mnemonics), 66 carry local `translations` blocks.

### 1.2 Coverage — measured

Script over all `.cc`: 521 keys in source and catalog, 9 locales; **181 keys referenced by `PhiStr`/`PhiStrF`, 341 never referenced**. Per-file `PhiStr` counts: `phi_menu_bar.cc` 134, `phi_feedback_window.cc` 20, `phi_window_controller.cc` 20, `phi_create_space_panel.cc` 18, `phi_settings_window.cc` 10 (5 distinct keys: `settings.displayLanguage`, `settings.general.language.sectionTitle`, `settings.languageHint`, `settings.languageSystemDefault`, `settings.account.defaultBrowser.setDefaultButton`). Files with user-visible text APIs and **zero** `PhiStr`: `phi_agent_overlay.cc`, `phi_downloads_popover.cc`, `phi_extensions_popover.cc`, `phi_group_overview.cc`, `phi_prompt.cc`, `phi_search_tabs_panel.cc`, `phi_tab_preview.cc`, `phi_transcript_panel.cc`, `phi_reader_panel.cc`, `phi_coordinator.cc`.

docs/i18n.md:143-148 ("Status") is honest that everything but the menu bar and two Settings rows is literal. The source file already has 146 `settings.*`, 62 `sidebar.*`, 20 `downloads.*`, 20 `prompt.*`, 15 `common.*` keys waiting — so the remaining work is mechanical but large.

### 1.3 Hard-coded user-visible literals (representative, not exhaustive)

* Settings pane: phi_settings_window.cc:429 (`SetWindowTextW(hwnd_, L"URL Rules")`), 672-677, 683, 691, 720, 754, 762, 783-792, 802-809, 832-837, 859-867, 975, 1256-1276, 1303-1338, 1350, 1379-1383, 1544-1562, 1611, 1641 — ~130 English literals in one file.
* Context menus: phi_window_controller.cc:7425-7430 (New Space), 7792-7797 (group menu), 7883-7910, 7939-7988, 8016-8048, 8072, 8202-8223, 8260-8272, 8306-8309 — every `AppendMenuW` label; also `PhiPromptText` titles/prompts at 7813, 8072, 8260-8261, 8272.
* Consent dialogs (security-relevant copy the user must understand): phi_coordinator.cc:645-651 (`L"Agent access request"`, `{L"Allow Once", L"Always Allow", L"Deny"}`) and 7128-7136 (`L"Credential request"`, `{L"Allow once", L"Allow for 10 minutes", L"Always allow", L"Deny"}`) — while the catalog already carries `agentControl.connectionApproval.allowOnceButton/alwaysAllowButton/denyButton` and `app.allowFor10Minutes` unused.
* Alert primitives: phi_prompt.cc:571 (`{L"OK", ...}`), 584 (`L"Cancel"`), so every `PhiNotice`/`PhiConfirm` caller inherits English buttons.
* Downloads: phi_downloads_popover.cc:86-108 (`L"Just now"`, `L"Today"`, `L"Yesterday"`, and the concatenations below).
* Space fallback name: phi_window_controller.cc:5896 (`L"Space " + id`).

### 1.4 Concatenated sentences (the rule docs/i18n.md:60-63 forbids)

phi_bookmark_manager.cc:703-707 (`L"Delete the " + n + L" selected items?"`), phi_bookmark_manager.cc:486 (`child_count == 1 ? L" item" : L" items"`), phi_downloads_popover.cc:93, 96, 106 (`n + L" min ago"`, `L" hour ago"`, `L" days ago"`), phi_downloads_popover.cc:460 (`received + L" of " + total`), 486 (`L"From " + host`), phi_transcript_panel.cc:88, phi_settings_window.cc:1985 (`L"Signed in as " + email`), 2167 (`L"Go to Space “" + name`), 3150, phi_coordinator.cc:634, 650, 7134.

### 1.5 Plurals, numbers, dates, RTL

* Plural: `PhiStrF` has no plural-form support (phi_i18n.cc:322-332). The only plural handling in the tree is the English ternary at phi_bookmark_manager.cc:486; downloads emit `"1 days ago"`-class bugs (phi_downloads_popover.cc:96 `L" hour ago"` for 1–11 hours).
* Numbers: 25 `std::to_wstring` sites; no `GetNumberFormatEx`, `GetDateFormatEx`, `GetTimeFormatEx`, `base::i18n` anywhere (grep returned no files). Digit grouping and decimal separators are C-locale.
* RTL: no `WS_EX_LAYOUTRTL`, `WS_EX_RTLREADING`, `DT_RTLREADING`, or text-direction query anywhere. The nine shipping locales are all LTR, so this is a latent rather than a current defect, but the sidebar layout module (`phi_sidebar_layout.cc`) has no mirroring hook to add later.
* Fonts: `L"Segoe UI"`, `L"Segoe UI Emoji"`, `L"Segoe MDL2 Assets"` are literal in 21 files (e.g. phi_window_controller.cc:504-554, phi_settings_window.cc:406-414). CJK fallback is left to GDI/DirectWrite defaults with no per-locale face selection.

### 1.6 Catalog hygiene

The `app.*` bucket (100 keys, comment "used in phi_X.cc") is a mechanical harvest of `L"..."` literals, including non-UI strings: `app.dPAGaSy` = `D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)` (SDDL, resources/strings-source.json:195-198), `app.authorizationBearer` = `"\r\nAuthorization: Bearer "` (115-118), `app.contentTypeApplicationJson` (171-174), `app.appsuselighttheme` (111-114, a registry value name), `app.imagePng` (259-262), `app.fydeInnovations` (239-242), `app.phibrowserWin` (339-342, a User-Agent). Keys such as `app.espaOl`/`app.franAis` (219-222) are mangled endonyms that duplicate `PhiLocaleDisplayName`'s table (phi_i18n.cc:241-256). A dynamic key is assembled at phi_menu_bar.cc:833 and phi_window_controller.cc:8620 (`"app.changeSpaceProfileConfirmation." + ...`), which the coverage script cannot see and the generator cannot verify.

### 1.7 Target

* One facade, already present: `PhiStr`/`PhiStrF`. Add `PhiStrN(key, n, args)` selecting `key.one`/`key.other` (CLDR categories per locale later) and `PhiFormatNumber/Date` wrappers over `GetNumberFormatEx`/`GetDateFormatEx` keyed on `PhiActiveLocale()`.
* Lint: a `tools/check-literals.py` that fails CI on any `L"` containing a space passed to `AppendMenuW`, `SetWindowTextW`, `MessageBoxW`, `PhiDrawText*`, `PhiAlert*`, `PhiPrompt*`, `Child(L"STATIC"/"BUTTON", ...)`. Allow-list font faces and class names.
* Purge the harvested `app.*` non-UI entries before any translator sees the file; forbid dynamic key assembly (make the profile-scoped variants two explicit keys).
* Convert `phi_prompt.cc:571/584` first — it fixes every alert's buttons at once — then Settings, then the context menus.

---

## 2. Theme

### 2.1 Model

`phi_theme.h` ports the Mac model: eight `PhiTheme`s with light/dark `PhiThemeColors` (phi_theme.h:26-38), a three-state `PhiAppearance` (:20), and a resolved `PhiPalette` of 15 `COLORREF` roles plus `dark` (:52-77) produced by `PhiResolvePalette(theme, appearance, saturation)` (:82-84). Per-space overlays: `PhiCoordinator::RebuildSpacePalettes()` (phi_coordinator.cc:3806-3833) builds `palette_by_space_` from a space's `theme_id`/`saturation`/`color_hex`, synthesising a custom theme when only a hex is present (3817-3828). Windows read `PaletteForSlot`. The popup chrome has a separate `PhiPopupColors` struct with its own 19 literal defaults (phi_popup_chrome.h:26-44) fed from the live palette through a global pointer `g_live_palette` (phi_popup_chrome.cc:146-157).

### 2.2 Hard-coded colours outside the theme

143 `RGB(` outside `phi_theme.cc`. Concentrations:

* phi_settings_window.cc:2441-2446 defines a full parallel palette (`bg`, `ink`, `label`, `secondary`, `tertiary`) from literals, justified as the Mac's `fixedWindowBackground`; then 2491 (close-button red `C4,2B,1C`), 2657-2661 (`kAgentPill = RGB(0x3A,0xA4,0xD5)` duplicating `PhiThemeColors::accent` default at phi_theme.h:30), 2724-2726, 2996-2997.
* phi_window_controller.cc:239-247 (tab-group colour table), 5283 (accent literal again), 9278 (same close-red), 9768, 9833-9848.
* phi_agent_overlay.cc:614-619 (own ground/ink/dim), 686 (`RGB(0x10,0x12,0x18)`).
* phi_transcript_panel.cc:47-51 (own 4-colour light/dark palettes).
* phi_split_drop.cc:352-360, phi_new_tab_page.cc:352-372, phi_create_space_panel.cc:892-1113, phi_popup_chrome.cc:152-153 (a second dark accent `1E,70,99` that exists nowhere in `phi_theme`).

The recurring "white on accent / ink at N%" computations are re-derived per file via six different blend helpers (§5.5).

### 2.3 Metrics

266 `constexpr` pixel constants matching `k*Width/Height/Pad/Gap/Radius/…` across 20 files (phi_window_controller.cc 54, phi_create_space_panel.cc 31, phi_settings_window.cc 27, …). DPI scaling is done by two private `Scaled(state, design)` helpers (phi_folder_icon_picker.cc:48, phi_space_theme_panel.cc:56) and ad-hoc `MulDiv`/`GetDpiForWindow` in ten other files. No shared metric token table; a design change to, say, row height is a multi-file edit.

### 2.4 Dark/light switching path

`PhiSystemPrefersDark()` reads `AppsUseLightTheme` from the registry (phi_theme.cc:106-116) and is consulted inside `PhiResolvePalette` when appearance is `kSystem` (phi_theme.cc:131). `ApplyTheme()` (phi_coordinator.cc:3796) re-resolves and pushes `ApplyPalette()` to every window (3800-3803), but its only callers are the saturation/theme/appearance setters (2945, 3917, 3925). There is no `WM_SETTINGCHANGE`/`WM_THEMECHANGED` handler in any window procedure, so an OS-level switch while the app runs is ignored. `PhiPopupColorsNow()` polls the registry on every call when no live palette is set (phi_popup_chrome.cc:149), so popups can disagree with the main window after such a switch. There is also no `DWMWA_USE_IMMERSIVE_DARK_MODE` call anywhere, so any native frame/menu Windows still draws stays light in dark mode.

### 2.5 Surfaces that ignore the theme

Kiosk (phi_kiosk_window.cc:342-344), Peek (phi_peek_panel.cc:351-354, 517, 534) and Reader (phi_reader_panel.cc:292-295) each hold a `static const PhiPalette kFallback = PhiResolvePalette("pure", kLight)` used whenever `coordinator_` is null. Because the static is resolved once, it also freezes the *system* answer. `phi_space_theme_panel.cc:184-187` uses `kSystem` for its fallback — a third convention. `phi_folder_icon_picker.cc:81` a fourth.

### 2.6 Per-space vs global interaction

`RebuildSpacePalettes` is correct in intent but has an ordering hazard: it reads `appearance_`/`saturation_` and `theme_id_` (3814, 3830-3831) — so it must run after every `ApplyTheme()`; `ApplyTheme` at 3796-3803 does not call it in the excerpt seen, and the Settings "Spaces" pane computes its own preview palette at phi_settings_window.cc:636 and 1199 with a literal white for "pure" (640, 1201), duplicating the "pure = white swatch" rule from phi_create_space_panel.cc:978. Rule lives in three places.

### 2.7 Target

* `PhiPalette` becomes the single token set, extended with the roles files currently invent (`ground_fixed`, `destructive`, `destructive_hover`, `on_accent`, `group.<name>`, `overlay_scrim`). `PhiPopupColors` becomes a view over `PhiPalette`, not a second struct.
* One `PhiBlend(over, base, alpha255)` in `phi_theme.h`; delete the six copies.
* A `PhiMetrics` struct (design units) + one `PhiDip(hwnd, design)` scaler; per-file constants move behind it incrementally.
* Appearance source of truth: coordinator subscribes to `WM_SETTINGCHANGE` (`lParam == L"ImmersiveColorSet"`) on its message-only window (phi_coordinator.cc:186 creates one) and calls `ApplyTheme()` → `RebuildSpacePalettes()` → windows. Remove the three `static const kFallback`s; a surface with no coordinator asks `PhiCoordinator::Get()->PaletteForSlot()`.

---

## 3. Logging, diagnostics, telemetry

### 3.1 Logging inventory

Totals across the client: `LOG(INFO)` 49, `LOG(WARNING)` 25, `LOG(ERROR)` 36, `VLOG(1)` 4, `DLOG`/`DVLOG`/`PLOG`/`LOG_IF` 0. All 110 lines carry a bracketed tag; the tag set is inconsistent in case and granularity: `[PhiClient]` 49, `[Feedback]` 25, `[PhiAuth]` 10, `[telemetry]` 5, `[Reader]` 5, `[PhiVault]` 5, `[PhiUpdater]` 5, `[PhiAgent]` 4, `[PhiControl]` 2, plus `[analytics]` on phi_coordinator.cc:2567. `phi_window_controller.cc` (≈11k lines) has four log lines (1198, 1350, 1395, 1460); `phi_sidebar_*`, `phi_tab_store.cc` (3), and all drag/drop code are effectively silent. `GetLastError()` is attached to only 20 of the error logs.

Level policy is implicit and uneven: cold-start restore plans log at INFO (phi_coordinator.cc:2120-2147); a missing string catalog — a packaging failure — logs ERROR (phi_i18n.cc:186); every unknown key logs WARNING once (phi_i18n.cc:314); a rejected analytics batch logs ERROR (phi_telemetry.cc:403). No `LOG(FATAL)`, no `CHECK` outside main.

### 3.2 PII in logs

* Auth0 subject id at INFO: phi_auth.cc:394 (`session restored for <sub>`), 580 (`signed in as <sub>`); the loopback redirect URI at 472.
* Page URL at INFO: phi_reader.cc:258-259 (`offerable granted tab=… url=…`); a context-menu reader event at phi_coordinator.cc:2202 logs window/tab ids only.
* The named-pipe path (contains the PID) at phi_control_server.cc:325; the peer's image path is embedded in the consent dialog text (phi_coordinator.cc:634) rather than logged.
* Analytics event module/name at VLOG(1) (phi_coordinator.cc:2567) — not properties, good.
* `[PhiUpdater] appcast override: <url>` (phi_updater.cc:118) discloses the override in the log, which is at least useful evidence of tampering.
* `chrome_debug.log` and Chromium's system logs are zipped into every feedback report (phi_feedback_outbox.cc:777-830); therefore anything the client logs reaches the vendor with the report. No redaction pass exists (grep for redact/scrub/anonymi: none).

### 3.3 Crash-time and support diagnostics

Feedback: the form (phi_feedback_window.cc:372-403) sends description, optional URL (page URL captured into the draft at phi_feedback_window.cc:730 → body at phi_feedback_outbox.cc:577-578), contact email (phi_feedback_outbox.cc:383-384, 1294-1295), screenshots/picked files (886, 910), and `logs.zip` = Chromium system logs + `chrome_debug.log` tail (777-830). The privacy notice string exists (strings-source.json:773) and is shown at phi_feedback_window.cc:398. Jobs are spooled per account, uploaded with retry (1707), and the upload bearer token is built by concatenation at phi_feedback_outbox.cc:1145. Good separation; the outbox is the best-engineered persistence in the tree (atomic writes at 121, size caps at 747/939).

Crash reporting itself is Chromium's (`crash_endpoint` in the same config file, phi_telemetry.h:30) — nothing client-side attaches the client's state (active Space, layout mode, pref snapshot) to a minidump. `agentSpace.analytics.list` exposes the last 256 events over the pipe (phi_coordinator.cc:5659-5665, ring at phi_coordinator.h:380), which is the closest thing to a breadcrumb trail.

### 3.4 Telemetry (`phi_telemetry.cc`)

* Source: framework `OnAnalyticsEventCaptured(module, name, properties_json)` (phi_framework_api.h:221-237) → `PhiCoordinator::OnAnalyticsEventCaptured` (phi_coordinator.cc:2550-2570) → `PhiTelemetry::RecordEvent`.
* Gate: `phi_telemetry.json` in the user data dir with `analytics_endpoint`/`analytics_key` (phi_telemetry.cc:247-287). Unconfigured → events dropped after the ring (280-283). Endpoint must be https, or http only to loopback/LAN (51-147, refusal at 147/273-275). Started from `OnFrameworkReady` (phi_coordinator.cc:180-184).
* **Consent**: none. No settings row, no first-run prompt, no `analytics_enabled` pref (grep across all files). The design comment "BACKEND-AGNOSTIC BY DESIGN" (phi_telemetry.h:21) treats the file as the switch; that is an operator switch, not a user one.
* PII: properties are parsed and forwarded unmodified (phi_telemetry.cc:449-465); a `distinct_id` GUID is generated on first run and persisted beside the spool (491-494, `EnsureDistinctId`), attached to every PostHog event (459). The header claims "nothing here ever ties it to an account" (phi_telemetry.h:98-100) — true for this file, but the account id is logged elsewhere (§3.2) and the feedback pipeline ships `chrome_debug.log`, so the join is possible offline.
* Failure handling: 4xx drops the batch and counts it (381-393, 403), 5xx/transport retries with capped backoff (37-40), spool bounded with oldest-first drop (30-35, 312-314, 434-436), spool written atomically (199-212). `StatusJson` deliberately omits the key (344-345). `Stop()` (phi_telemetry.cc:320-338) is **never called** by any file (grep), so the final flush and the 2 s join at 335 never happen; whatever is queued since the last batch write is lost on exit.

### 3.5 Target

* A logging policy file (`docs/logging.md`) with four rules: (1) INFO is for lifecycle transitions only, never per-navigation; (2) ERROR only when the user will see a degraded result and `GetLastError()`/HTTP status is attached; (3) never log a URL, title, email, `sub`, token or path under the profile without passing through `PhiRedact()` (`url` → scheme+host, ids → last 4); (4) one tag per module, `[Phi<Module>]`, enforced by a grep in CI.
* Telemetry: add `analytics.enabled` to `PhiPrefs.json` (default off until an OOBE screen sets it), surface it in Settings › General, make `RecordEvent` a no-op when false, and add an allow-list of property keys per module in `BuildBatchBody`.
* Call `PhiTelemetry::Get().Stop()` and `PhiUpdater::Shutdown()` from the framework's shutdown callback.

---

## 4. Error handling

### 4.1 Patterns observed

* **Win32 calls**: return values are mostly ignored. Bare-statement counts (no `if`/assignment on the same line): `SetWindowPos` 65, `CloseHandle` 62, `DeleteFileW` 13/13, `WriteFile` 11 of 19, `DwmSetWindowAttribute` 10/10, `FlushFileBuffers` 3/3, `SetLayeredWindowAttributes` 3/3, `RegisterClassExW` 6 of 23. Most are benign (`CloseHandle`, `SetWindowPos`), but `WriteFile` unchecked on the prefs file (phi_coordinator.cc:2892-2893) and on the pipe pointer file (phi_control_server.cc:311-312) is not. `ConvertStringSecurityDescriptorToSecurityDescriptorW` failure at phi_control_server.cc:333-335 silently falls back to the default DACL (`sd ? &sa : nullptr` at 342), which is exactly the case the comment at 331-332 says must not happen.
* **Framework calls**: 50 of 108 virtuals return `bool`; 187 bare `framework_->…(` statements, of which the bool-returning ones are `ActivateTab` 9, `SetTabTag` 10, `SetTabTagById` 7, `SplitActiveWith` 4, `CloseTab` 3, `MoveTabsToNewWindow` 2, `RenameTabGroup` 2, `SetTabGroupColor` 2, `GoBack` 2, `DropGhostWindow` 2, `CreateSpaceBrowserInProfile` 2, and one each of `MoveTab`, `UpdateSplitView`, `SwapSplitView`, `RemoveSplitById`, `SwapTabIntoSplit`, `SetTabGroupCollapsed`, `UngroupTabGroup`, `CloseTabGroup`, `SetDefaultSearchEngine`, `SetDownloadDirectory`, `GoForward`, `Reload`, `OpenOmniboxMatch`, `FetchFeedbackSystemLogs`, `MoveTabBefore`. The sidebar doc's principle 6 ("stale-safe by construction") covers the read side; nothing covers a refused write.
* **`const char*` results**: the framework returns 108 `const char*` values "valid until the next call on this interface" (phi_framework_api.h:326-327, 376, 422, 464, 514-515). Call sites store the pointer in a local then parse immediately (phi_peek_panel.cc:421, phi_kiosk_window.cc:274, 301, phi_coordinator.cc:3989) — safe today, with no type-level protection; `ListTabsJson` alone is fetched and re-parsed at 29 sites (phi_coordinator.cc 16, phi_window_controller.cc 7, others 6).
* **JSON**: 155 `base::JSONReader::Read*` sites (107 in `phi_coordinator.cc`). The dominant pattern is `if (!root) return;` (phi_coordinator.cc:729-733, 783-787, 1149-1153) or `value_or(default)`; only three parse sites log (phi_i18n.cc:190, phi_telemetry.cc:263, and one in the outbox). A corrupt `PhiSpaces.json` or `PhiTabData.json` therefore degrades to an empty model with no trace. Some sites paper over invalid input by design: phi_coordinator.cc:548 parses `json ? json : "{}"`.
* **CHECK/DCHECK**: four in `phi_client_main_win.cc` (161, 260, 308, 341), all inherited from Chromium; **zero** in the 62k lines of client code. No `NOTREACHED()`. Invariants the sidebar doc states (one row per cell, dense indices) are not asserted anywhere.
* **Exceptions**: no `try`/`catch`/`__try`/`SetUnhandledExceptionFilter` in the tree; the build presumably runs `-fno-exceptions` like Chromium. Nothing is swallowed, which is good; the risk is instead unchecked returns.
* **Silent-empty-block** near-miss: phi_control_server.cc:345-349 does log and `return` on `CreateNamedPipe` failure — but `return` exits `ListenLoop` permanently, so a transient failure kills the control surface for the session.

### 4.2 Target — one convention

* Every write-side framework call goes through `PhiFw::Must(bool, "verb", ids…)` which logs at WARNING with context and returns the bool; callers that can't recover `return` on false. This is a mechanical wrap of the 50 bool-returning methods.
* Every persistence file (`PhiPrefs.json`, `PhiSpaces.json`, `PhiTabData.json`, `PhiUrlRules.json`, `PhiLayout.json`, `PhiTheme.json`, `PhiGhostSpaces.json`, `PhiAgentGrants.json`, `PhiCredentialGrants.json`) uses a single `PhiReadJsonFile`/`PhiWriteJsonFileAtomic` pair that logs parse failures with the path and renames the corrupt file aside rather than overwriting it on the next save.
* Use `CHECK` for ownership invariants that would otherwise dereference null (`host_`, `framework_`), `DCHECK` for the sidebar model invariants inside `BuildSidebar()`. Keep zero in paint paths.
* Win32: a `PhiWin32Check(expr, "name")` macro for the handful of calls whose failure changes semantics (security descriptors, file writes, `RegisterClassExW`); leave the cosmetic ones bare.

---

## 5. Code hygiene

### 5.1 `RAW_PTR_EXCLUSION`

38 occurrences across **30 files** (17 headers, 13 `.cc`), not 14. The justification appears exactly once (phi_auth.h:116-117: cross-thread handoff cleared by the other thread). The other 37 are back-pointers to owners or framework (`PhiWindowController* const controller_` in phi_bookmark_manager.h:117, phi_new_tab_page.h:92, phi_menu_bar.h:219, phi_create_space_panel.h:107, phi_tab_preview.h:157; `PhiFramework*` in phi_kiosk_window.h:131, phi_coordinator.h:426, phi_window_controller.h:1522; `PhiCoordinator*` in phi_reader.h:139, phi_peek_panel.h:139, …). The likely real reason is that the client is compiled outside Chromium's BackupRefPtr-enabled partition (it links `//base` but allocates the framework object from an exported factory, phi_client_main_win.cc "PhiClientCreateDelegate"), so `raw_ptr<T>` would be either a no-op or a link problem — but that reason is not written down anywhere. Two exclusions wrap non-owning `const std::string*` fields on a marshalling struct (phi_coordinator.cc:169-170) and one an `unsigned char*` bitmap pointer (phi_glyph_bitmap.cc:26). The macro is being used as "I don't want to think about raw_ptr", which defeats the annotation.

### 5.2 Mutable globals and singletons (all of them)

File-scope mutable statics:

| Symbol | File:line | Holds | Hidden app state? |
|---|---|---|---|
| `g_coordinator` | phi_coordinator.cc:47 (set 123, cleared 135) | the app object, for `MouseHookProc`/`KeyboardHookProc` (7698, 7759) | yes — the real app context |
| `g_instance` | phi_settings_window.cc:161 (23 uses) | singleton settings window | yes |
| `g_window`, `g_next_request` | phi_feedback_window.cc:88-89 | singleton feedback window + request counter | yes |
| `g_toast` | phi_toast.cc:34 | the one toast | yes |
| `g_sparkle`, `g_initialized` | phi_updater.cc:50-51 | WinSparkle function table + init flag | process-lifetime service |
| `g_live_palette` | phi_popup_chrome.cc (set 155) | raw pointer to coordinator's palette | yes — dangling-capable |
| `g_edit_base_proc` | phi_window_controller.cc:300 | subclassed EDIT WNDPROC | one-time |
| `g_search_edit_base_proc` | phi_search_tabs_panel.cc:43 | same, second copy | one-time |

Function-local mutable statics: `static HWND last_owner; static ULONGLONG closed_tick;` duplicated in phi_downloads_popover.cc:143-144 and phi_extensions_popover.cc:106-107 (re-open debounce), `static int cached` phi_control_server.cc:599 (DevTools port), `static bool tried` phi_window_controller.cc:385, `static HICON logo` phi_window_controller.cc:9210, plus twelve `static bool registered` window-class guards (phi_agent_overlay.cc:557, phi_control_server.cc:44, phi_drag_image.cc:458, phi_group_overview.cc:89, phi_split_drop.cc:167, phi_tab_switcher.cc:114, phi_search_tabs_panel.cc:149, phi_transcript_panel.cc:171, phi_coordinator.cc:189, phi_window_controller.cc:8736, phi_downloads_popover.cc:198, phi_extensions_popover.cc:215).

`base::NoDestructor` (16): immutable catalogs — emoji (phi_emoji.cc:129), icons (phi_icons.cc:22), folder icons (phi_folder_icons.cc:17), icon-mask tables (phi_icon_masks.cc:2088, phi_folder_icon_masks.cc:993), NTP masks (phi_ntp_art.cc:326, 333), themes (phi_theme.cc:68), create-space cell lists (phi_create_space_panel.cc:341, 349); and mutable ones — i18n `State` (phi_i18n.cc:42) and its unknown-key fallback map (:264), DirectWrite state (phi_dwrite.cc:58), icon ink-box cache (phi_icons.cc:129), and in `phi_window_controller.cc` a `map<HWND, WNDPROC>` of subclassed procs (:310) and a byte-store (:384). `PhiTelemetry::Get()` is a Meyers singleton (phi_telemetry.cc:222).

Net: there *is* an app context object (`PhiCoordinator`), but six UI singletons and the popup palette reach it through globals rather than through it, and two lifecycle methods on services are unreachable (`PhiTelemetry::Stop`, `PhiUpdater::Shutdown` phi_updater.cc:180 — neither has a caller).

### 5.3 Header hygiene

`<windows.h>` is included by 37 of 56 headers, including model/value headers such as phi_theme.h:8 (for `COLORREF`), phi_telemetry.h:8 (for `HANDLE`/`CRITICAL_SECTION`), phi_surface_registry.h. The four pure sidebar modules (`phi_sidebar_model/layout/drop/actions.h`), `phi_tab_store.h`, `phi_i18n.h`, `phi_credentials.h`, `phi_features.h` are clean, which is what lets `test/model/run.sh` compile them on Linux. No `NOMINMAX`/`WIN32_LEAN_AND_MEAN` in the client; `std::min`/`std::max` compile because Chromium's toolchain defines `NOMINMAX` globally — a dependency on the fork's build args that is not stated. No `near`/`far` identifier collisions found.

### 5.4 Naming

71 `Phi*` classes/structs; 33 un-prefixed types, all in the sidebar model/layout/drop headers (`SidebarTree`, `SidebarKey`, `DropRow`, `ListMetrics`, `StripWidths`, …) — a deliberate second convention introduced by the design doc's §3 module map. File naming is uniform (`phi_<noun>.{h,cc}`). The framework uses a `…Json` suffix for the 13 `const char*` JSON getters (phi_framework_api.h:328, 377, 423); the client mirrors it inconsistently (`StatusJson` phi_telemetry.h:67 yes; `payload_json`/`properties_json` parameters yes; but pref/state serialisers are `SavePrefs`, `SourceToDict` with no suffix). Minor.

### 5.5 Duplicated utility code

* Whole-file read loop: phi_i18n.cc:49, phi_emoji.cc:34, phi_auth.cc:154, phi_credentials.cc:32, phi_feedback_outbox.cc:101 (five copies of the same `CreateFileW`/`ReadFile` loop; the i18n one documents why `base::ReadFileToString` is avoided, the others don't).
* File write: `WriteFileBytes` phi_auth.cc:170, phi_feedback_image.cc:102; `WriteFileAtomic` phi_feedback_outbox.cc:121, phi_telemetry.cc:199; and the non-atomic inline version in `SavePrefs` phi_coordinator.cc:2886-2894 and phi_control_server.cc:307-312. Two atomic and four non-atomic writers for the same directory.
* UTF-8↔UTF-16: `Widen/Narrow` in phi_feedback_window.cc:91-96 and phi_feedback_outbox.cc:180-183 wrap `base::UTF8ToWide`; phi_kiosk_window.cc:94-118 and phi_peek_panel.cc:67-79 reimplement it with `MultiByteToWideChar` (unchecked size 0). Same names, two behaviours.
* Colour blend: `Blend` phi_theme.cc:22, phi_settings_window.cc:186, phi_create_space_panel.cc:85; `BlendOver` phi_window_controller.cc:7651, phi_downloads_popover.cc:47; `BlendInk` phi_new_tab_page.cc:47; `LerpColor` phi_window_controller.cc:5491; `PhiPopupBlend` phi_popup_chrome.cc:159. Eight functions, one formula, three argument orders (`(over, base, a)` vs `(base, over, a)` at phi_downloads_popover.cc:47).
* Strings: `Trim` ×3 (phi_bookmark_manager.cc:58 wide, phi_feedback_window.cc:98, phi_feedback_outbox.cc:195), `Lower/ToLower` ×3 (phi_credentials.cc:24, phi_feedback_outbox.cc:204, phi_search_tabs_panel.cc:61).
* Rect/clamp: `Width/Height` phi_split_drop.cc:67-70 only; 43 manual `std::max(a, std::min(b, x))` idioms vs 13 `std::clamp`. `Lerp` phi_tab_preview.cc:91, `Clamp01` phi_dwrite.cc:500.
* Time: `NowMs` phi_feedback_outbox.cc:51 and `NowUnixMs` phi_telemetry.cc:41.
* Window-class registration and `WNDPROC` subclassing repeated in 24 files (`RegisterClassExW` 33 sites) with twelve `static bool registered` guards — a `PhiWindowClass` helper would remove ~300 lines.

### 5.6 Dead code

Declared and defined, never referenced (verified by grep over all `.cc`/`.h`): `PhiAgentDriveRegistry::MaskedTabs` (phi_agent_drive_registry.h:75 / .cc:122), `PhiAgentOverlay::IsShown` (phi_agent_overlay.h:63 / .cc:812), `PhiCoordinator::OpenUrlInMainWindow` (phi_coordinator.h:187 / .cc:3835), `PhiCoordinator::StoreTabInternal` (phi_coordinator.h:573 / .cc:1845), `PhiGhostSpaces::IsParked` (phi_ghost_spaces.h:68 / .cc:160), `PhiReaderBridge::SourceUrlOfReaderPage` (phi_reader.h:83 / .cc:193), `PhiSettingsWindow::AddChips` (phi_settings_window.h:291 / .cc:574), `SidebarTree::BindingFor` and `IndexOf` (phi_sidebar_model.h:170-171 / .cc:91, 96 — test-only helpers, acceptable if the test dir uses them), `PhiWindowController::CommitUrlField` (phi_window_controller.h:814 / .cc:1578), `PhiWindowController::SplitIdForTab` (phi_window_controller.h:1264 / .cc:4777), `PhiUpdater::Shutdown` (phi_updater.cc:180), `PhiTelemetry::Stop` (phi_telemetry.cc:320). Declared but **never defined**: `PhiWindowController::CloseStoredContent` (phi_window_controller.h:1416).

TODO/FIXME/HACK/XXX: **zero** in the tree. Either discipline or the comments carry the debt in prose — the `docs/sidebar-architecture.md` §9 list is the de-facto TODO file.

### 5.7 `phi_client_main_win.cc` maintenance risk

Header states the contract (phi_client_main_win.cc:5-9: "faithful copy … keep diffable against it on rebases"). Measured `diff` against `/home/jx/Phi/chromuim/src/chrome/app/chrome_exe_main_win.cc`: 60 changed lines, all in four labelled blocks — the exported `PhiClientCreateDelegate` factory, the `profile-directory=Default` pin (284-287), the child-process sandbox note, and the `remote-debugging-port=0` injection (385-389). Risk is moderate: the Chromium file changes every few milestones (fiber/stack setup at 260, `RegisterAbslAbortHook` 266, `DCHECK_IS_ON()` block 289-302), and there is no script that re-applies the four blocks. Recommend a `tools/check-main-diff.sh` that fails when the diff has hunks outside those four markers, and moving the two switch injections into the delegate (`OnFrameworkReady` or a `PhiFrameworkDelegate::AdjustCommandLine` hook) so the copy shrinks to the export alone.

### 5.8 Target — "no globals" rule

`PhiCoordinator` is already the app context. Make the six UI singletons members (`settings_window_`, `feedback_window_`, `toast_`, `popup_palette`), pass `PhiCoordinator&` to hooks via `SetWindowsHookEx`'s thread affinity + a single `g_coordinator` behind an accessor that `CHECK`s non-null, and give `PhiUpdater`/`PhiTelemetry` explicit `Start/Stop` calls from the delegate's shutdown path. Add `phi_util.h` (`ReadFileBytes`, `WriteFileAtomic`, `Widen/Narrow`, `Trim`, `Lower`, `Blend`, `Clamp`, `NowMs`) and forbid new local copies by review.

---

## 6. Feature flags and release gating

* `phi_features.h` has one flag, `inline constexpr bool kPhiAiEnabled = false` (phi_features.h:23), consumed at eight sites. No `BUILDFLAG`, no `OFFICIAL_BUILD`, no `NDEBUG` branches in client code; the only debug-only block is Chromium's own at phi_client_main_win.cc:289-302.
* **Developer mode** is a runtime pref (`developerMode` in `PhiPrefs.json`, phi_coordinator.cc:2818-2819, 2866; toggle at phi_settings_window.cc:859-867, 3134) gating the Developer pane and "agent access / agent password manager" per the hint text. It is not a build gate.
* **Dev-only paths reachable in release** (none are behind developer mode, a build flag, or a signed-config check):
  1. `PHI_APPCAST_URL` / `PHI_APPCAST_ED25519` env overrides of update feed and signing key (phi_updater.cc:113-133). This defeats update signing entirely.
  2. `PHI_VAULT_HELPER` env override of the vault helper executable (phi_coordinator.cc:250-256) and automatic fallback to `credentials_.Load()` when the helper fails to start (:257-259), which accepts a plaintext JSON array in `PhiCredentials.dat` when DPAPI decryption fails (phi_credentials.cc:76-82, header note phi_credentials.h:29).
  3. `--remote-debugging-port=0` injected for every browser process (phi_client_main_win.cc:385-389); the port is proxied over the pipe behind per-peer consent (phi_control_server.cc:394-397, consent dialog phi_coordinator.cc:645-651), but the DevTools socket itself is open to any local process that reads `DevToolsActivePort`.
  4. Control pipe with 103 `agentSpace.*` verbs always started (phi_coordinator.cc:278), including `agentSpace.feedback.setAccount` documented as "Harness only" (phi_coordinator.cc:5637-5639) and `agentSpace.analytics.list`. Pipe DACL is owner/SYSTEM/admins (phi_control_server.cc:331-336) — correct — but silently degrades to default if SDDL conversion fails (342).
  5. Consent falls back to `MessageBoxW(nullptr, …)` with a Yes/No when no owner window exists (phi_coordinator.cc:649-655 grants "Allow once" only — good; but 7134-7139 for credentials likewise). Acceptable as designed; note both dialogs are English literals.
* Plaintext credential handling is at least confined: DPAPI is tried first (phi_credentials.cc:78), auth tokens are DPAPI-protected (phi_auth.cc:188, 201), and the feedback bearer token is never written to disk.

### Target — release-gating checklist (make it a script, `tools/release-gate.py`, run by `build-client.py`)

1. `PHI_APPCAST_*`, `PHI_VAULT_HELPER` overrides compile only under a `PHI_DEV_HOOKS` define that the release recipe never sets; grep the binary's string table for `PHI_APPCAST` as a post-build assertion.
2. Dev-vault fallback becomes an explicit `PhiPrefs` flag that developer mode can set, defaulting to "refuse and show a notice" in release.
3. `remote-debugging-port` and the control pipe start only when developer mode is on **or** an agent grant already exists in `PhiAgentGrants.json`; harness-only verbs (`setAccount`, `analytics.list`) compile out under the same define.
4. Analytics requires `analytics.enabled == true` in prefs (user consent), independent of `phi_telemetry.json`.
5. `strings.json` regenerated with `--check` passing and zero `app.*` non-UI keys; the literal-lint from §1.7 green.
6. `check-main-diff.sh` green against the fork's `chrome_exe_main_win.cc`.
7. No `LOG(INFO)` argument matches `url=`, `sub`, `email` (grep gate from §3.5).

---

## 7. Proposed targets, one line each

| Concern | Target |
|---|---|
| Strings | `PhiStr`/`PhiStrF`/`PhiStrN` + `PhiFormat{Number,Date}` as the only path; CI literal-lint; purge harvested `app.*`. |
| Theme | `PhiPalette` as the single token set with per-space overlay computed in one place; `PhiPopupColors` derived from it; one `PhiBlend`; `PhiMetrics`+`PhiDip`; `WM_SETTINGCHANGE` → `ApplyTheme`. |
| Logging | `docs/logging.md` policy, `PhiRedact()` for URLs/ids, one tag per module, INFO for lifecycle only; feedback zip passes through the same redaction. |
| Telemetry | User opt-in pref + Settings row; property allow-list; `Stop()` wired to shutdown. |
| Errors | `PhiFw::Must()` around bool framework verbs; `PhiReadJsonFile`/`PhiWriteJsonFileAtomic` for every persisted file with logged, quarantined failures; `CHECK` on ownership, `DCHECK` on model invariants. |
| Globals | `PhiCoordinator` owns the six UI singletons and the popup palette; services get explicit lifecycles; `phi_util.h` replaces the duplicated helpers. |
| Gating | `PHI_DEV_HOOKS` define + `tools/release-gate.py` implementing the seven checks above. |
