# Lane 3 — UI substrate review (window classes, panels, popovers, paint)

Read-only review of `/home/jx/Phi/phibrowser-win/client` on 2026-09-07. Every
claim carries a `file:line`; paths are relative to that directory unless
noted. House style per `docs/sidebar-architecture.md` (§2 "Nothing structural
in WM_PAINT"; "no parallel patterns"); AA rule per
`~/.claude/projects/-home-jx-Phi/memory/phi-win-antialiasing.md`.

Method: every `RegisterClass*`, `CreateWindowEx`, `SetTimer`, `SetCapture`,
`GetMessage`, `WM_NCDESTROY`, `WM_DPICHANGED`, GDI object creation and curve
primitive was grepped and its enclosing wndproc read. Numbers below are from
those greps, not estimates.

---

## 0. Executive summary

* **33 window classes in 24 files** (not 24 classes), registered through
  four different idioms, with `this` bound five different ways. No shared
  base; every class re-derives paint framing, hover tracking, DPI, dismissal
  and lifetime by hand. `phi_popup_chrome` (caption band) and `phi_dwrite`
  (AA shapes) are the only two shared substrate pieces, and both are opt-in.
* **Paint discipline is good in the shell's `WM_PAINT` and bad around it**:
  the floating sidebar rebuilds the model inside its own `WM_PAINT`
  (`phi_window_controller.cc:8825`) and *schedules a shell refresh from
  inside paint* (`:8840`); `WM_PRINTCLIENT` rebuilds synchronously
  (`:11406`); Settings runs `Rebuild()` — host reads plus a registry probe —
  inside `WM_PAINT` (`phi_settings_window.cc:3647`, `:2011-2012`).
* **Hover invalidates the whole shell** on every hover-kind change
  (`phi_window_controller.cc:4533`, `:4554`, `:4572`, `:4596`) and
  `ComputeLayout()` — which mutates `dpi_` and rebuilds fonts as a side
  effect (`:662-672`) — runs ~8 times per `WM_MOUSEMOVE`.
* **No accessibility, no RTL, no tooltips, no OS theme-change listener**
  anywhere (all four greps returned nothing).
* **Timers**: no same-HWND id collisions today, but ids are an informal
  0x515–0x542 space spread across files, three files reuse literal `1`, and
  the coordinator uses unnamed literals `1`/`2` with a catch-all `WM_TIMER`
  branch (`phi_coordinator.cc:304-320`, `:4552`, `:6158`).
* **Three nested modal `GetMessage` loops** (`phi_prompt.cc:169`,
  `phi_folder_icon_picker.cc:252`, `phi_space_theme_panel.cc:472`); the two
  panel ones run with no owner disable and with the global accelerator hook
  live, while holding a raw controller pointer in stack state.
* **Recommendation**: a small `PhiWindow` base + three families
  (`PhiPopover`, `PhiPanel`, `PhiSecondaryWindow`), one animator, one
  theme bus, and a UIA provider driven by the rect lists every surface
  already keeps. Route the `phi_dwrite` shape primitives through the
  existing `ID2D1DCRenderTarget` instead of the software coverage loops;
  do **not** move window composition to Direct2D.

---

## 1. Inventory — every RegisterClass / CreateWindowEx site

Legend: **bind** = how the wndproc finds the C++ object. NCC = set at
`WM_NCCREATE` from `lpCreateParams`; POST = `SetWindowLongPtr` after
`CreateWindowEx` returns; SING = static singleton pointer compared to the
HWND; STACK = pointer to a stack struct in `GWLP_USERDATA`; NONE =
`DefWindowProcW` is the class proc. **Destroy** = who calls `DestroyWindow`.

| # | Class name (register site) | Create site | Styles | Owner/parent | Wndproc | Bind | Destroy |
|---|---|---|---|---|---|---|---|
| 1 | `PhiShell` `phi_window_controller.cc:341-360` (CS_HREDRAW\|CS_VREDRAW\|CS_DBLCLKS, no bg brush) | `:1345` | `WS_OVERLAPPEDWINDOW\|WS_CLIPCHILDREN`; agent: `WS_EX_TOOLWINDOW\|WS_EX_NOACTIVATE` | top-level | `ShellWndProc` `:10846` | NCC `:10850` | `~PhiWindowController` `:1412`; `WM_CLOSE`→`RequestClose()` may destroy self `:11473-11484` |
| 2 | `PhiCornerCap` `:2148-2158` ×4 | `:2677` | `WS_EX_LAYERED` child | shell | `CornerCapProc` `:2127-2146` | NCC `:2127` | with shell |
| 3 | `PhiStatusBubble` `:2188-2198` | `:2489` | `WS_EX_LAYERED` child | shell | `StatusBubbleProc` `:2167` | NCC | with shell |
| 4 | `PhiOmniboxPopup` `:2239-2249` | `:1808` | `WS_EX_TOOLWINDOW\|WS_EX_NOACTIVATE`, `WS_POPUP` | shell (owned) | `OmniboxPopupProc` `:2203` | NCC `:2207` | with shell |
| 5 | `PhiFloatingSidebar` `:8736-8745` | `:8748` | `WS_EX_TOOLWINDOW\|WS_EX_NOACTIVATE`, `WS_POPUP`, DWM round `:8758` | shell (owned) | `FloatingSidebarProc` `:8853` | POST `:8756` | controller (`WM_DESTROY` clears field `:8934-8938`) |
| 6 | `PhiSpaceHoverCard` `:10062-10069` | `:10077` | `WS_EX_TOOLWINDOW\|NOACTIVATE\|TRANSPARENT\|LAYERED`, `WS_POPUP` | shell (owned) | `HoverCardPopupProc` `:10154` | NCC `:10158` | with shell |
| 7 | `EDIT` (system) url field | `:1456` | `WS_CHILD\|WS_VISIBLE\|ES_AUTOHSCROLL` | shell | subclass `UrlFieldProc` `:2000` via `GWLP_WNDPROC` `:1484`, base saved in **process global** `g_edit_base_proc` `:300` | POST `:1482` (=controller) | with shell |
| 8 | Chromium browser HWND (foreign) | reparented `:2101` | stripped to `WS_CHILD\|WS_CLIPSIBLINGS` `:2089-2093` | shell | subclass `BrowserSubclassProc` `:314`, base in `BrowserBaseProcs()` map `:309` | map by HWND; erased at `WM_NCDESTROY` `:328-335` | framework; `SetParent(nullptr)` in dtor `:1409` |
| 9 | `PhiCoordinatorMsg` `phi_coordinator.cc:189-196` | `:198` `HWND_MESSAGE` | message-only | — | `MsgWndProc` `:294` | NCC `:298` | coordinator |
| 10 | `PhiAgentFog` `phi_agent_overlay.cc:558-564` | `:68` | `WS_EX_LAYERED\|NOPARENTNOTIFY` child | shell | `FogProc` `:679` (eats input, paints in `WM_ERASEBKGND` `:682`) | none needed | `~Overlay` `:90-94` |
| 11 | `PhiAgentSprite` `:572-577` | `:74` | `WS_EX_LAYERED\|TRANSPARENT\|NOPARENTNOTIFY` child | shell | `SpriteProc` `:711` | SING `Overlay::instance` `:714` | `~Overlay` |
| 12 | `PhiAgentPill` `:565-571` | `:78` | `WS_EX_NOPARENTNOTIFY` child, **SetWindowRgn** `:145-147` | shell | `PillProc` `:724` | SING `:738` | `~Overlay` |
| 13 | `PhiExtensionsPopover` `phi_extensions_popover.cc:214-226` | `:139` | `WS_EX_TOOLWINDOW`, `WS_POPUP`, DWM round `:142` | shell (owned) | `Popover::WndProc` `:369` | NCC `:374-380` | self (`WM_ACTIVATE` inactive `:388`, click `:366`); `delete self` in `WM_DESTROY` `:437-446` |
| 14 | `PhiDownloadsPopover` `phi_downloads_popover.cc:197-209` | `:163` | same as 13 | shell (owned) | `Popover::WndProc` `:634` | NCC `:639-646` | self; `delete self` `:716-725` |
| 15 | `PhiFeedbackWindow` `phi_feedback_window.cc:292-300` | `:307` | `WS_OVERLAPPED\|CAPTION\|SYSMENU\|MINIMIZEBOX\|CLIPCHILDREN`, chrome via `PhiPopupChrome::Attach` `:316` | top-level | `WndProc` `:828` | NCC `:832` | self (`WM_CLOSE`→`DestroyWindow` `:955`; `delete self` in `WM_DESTROY` `:958-966`) |
| 16 | `EDIT/BUTTON/STATIC/LISTBOX` children of 15 | `CreateControl` `:352` | `WS_CHILD\|WS_VISIBLE` | 15 | `SetWindowSubclass(ControlProc)` `:360`, removed at `WM_NCDESTROY` `:759` | subclass data = `this` | with 15 |
| 17 | `PhiCreateSpacePanel` `phi_create_space_panel.cc:235-243` | `:248` | `WS_CHILD\|WS_CLIPCHILDREN` | shell | `PanelProc` `:1118` | POST `:254` | owner object |
| 18 | `EDIT` child of 17 | `:293` | child | 17 | `FieldProc` via `GWLP_WNDPROC` `:309`; `this` via **parent's** `GWLP_USERDATA` `:1321-1322` | indirect | with 17 |
| 19 | `PhiDragImage` `phi_drag_image.cc:457-468` | `:338` | `WS_EX_LAYERED\|TRANSPARENT\|TOPMOST\|TOOLWINDOW\|NOACTIVATE`, `WS_POPUP` | shell (owned) | `Image::WndProc` `:430` | SING `Image::instance` `:435` | `~Image` `:345-348`; **deletes itself inside its own `WM_TIMER`** `:437-439`; `WM_NCDESTROY` guard `:446-450` |
| 20 | `PhiPeekShadow` `phi_peek_panel.cc:95-105` | `:164` | `WS_EX_LAYERED\|TRANSPARENT\|NOPARENTNOTIFY`, `WS_CHILD\|WS_CLIPSIBLINGS` | shell | NONE (`DefWindowProcW` `:99`, pixels via `UpdateLayeredWindow`) | — | `~PhiPeekPanel` `:252-254` |
| 21 | `PhiPeek` `:122-135` (CS_HREDRAW\|CS_VREDRAW) | `:172` | `WS_CHILD\|CLIPCHILDREN\|CLIPSIBLINGS`, **SetWindowRgn** `:302-304` | shell | `WndProc` `:739` | NCC `:743-751` | `~PhiPeekPanel` `:255-258` (clears USERDATA first `:256`) |
| 22 | `PhiPeekControls` `:107-120` | `:200` | `WS_EX_NOPARENTNOTIFY` child, **SetWindowRgn** union of chips `:494-502` | 21 | `ControlsWndProc` `:720` | NCC `:724` | `~PhiPeekPanel` `:249-251` |
| 23 | `PhiGroupOverview` `phi_group_overview.cc:88-98` | `:59` | `WS_CHILD\|WS_CLIPSIBLINGS` | shell | `OverviewProc` `:312` | SING `Overview::instance` `:314` | `~Overview` `:74-81`; `Close()` clears instance before delete `:373-378` because `WM_KILLFOCUS` re-enters `Close()` `:337-341` |
| 24 | `PhiReaderPanel` `phi_reader_panel.cc:30-43` | `:72` | `WS_CHILD\|WS_CLIPCHILDREN`, **SetWindowRgn** `:194-196` | shell | `WndProc` `:321` | NCC `:325-333` | `~PhiReaderPanel` `:133-136` |
| 25 | `PhiTranscriptPanel` `phi_transcript_panel.cc:170-181` | `:90` | `WS_EX_TOOLWINDOW`, `WS_POPUP\|CAPTION\|SYSMENU\|THICKFRAME\|MIN\|MAX\|CLIPCHILDREN`, `PhiPopupChrome` `:100` | shell (owned) | `PanelProc` `:306` | NCC `:308-313` | `~Panel` `:110-125`; **`delete self` inside `WM_CLOSE`** `:385-391`; also `Toggle()` `:426-428` |
| 26 | `EDIT`×2 + `BUTTON` children of 25 | `:198-211` | child | 25 | input subclassed via `GWLP_WNDPROC` `:228`, `InputProc` `:289` | POST `:225` | with 25 |
| 27 | `PhiPrompt` `phi_prompt.cc:73-81` | `:86` | `WS_EX_DLGMODALFRAME`, `WS_POPUP\|CAPTION\|SYSMENU\|CLIPCHILDREN`, `PhiPopupChrome` `:93` | caller's owner | `Alert::WndProc` `:495` | NCC `:499-505`; extra guard `self->hwnd_ != hwnd` `:509` | `~Alert` `:102-106` (clears USERDATA first `:104`) |
| 28 | `PhiTabSwitch` `phi_tab_switcher.cc:113-123` | `:65` | `WS_EX_TOOLWINDOW\|NOACTIVATE\|TOPMOST`, `WS_POPUP` | shell (owned) | `HudProc` `:259` (HTTRANSPARENT) | SING `Hud::instance` `:260` | `~Hud` `:76-83` via `Commit()/Cancel()` `:307-320` |
| 29 | `PhiSpaceThemePanel` `phi_space_theme_panel.cc:408-418` | `:458` | `WS_EX_TOOLWINDOW`, `WS_POPUP`, DWM round `:465` | shell (owned) | `PanelProc` `:305` | STACK `PanelState` `:464` | caller after modal loop `:477-478` |
| 30 | `PhiFolderIconPicker` `phi_folder_icon_picker.cc:192-202` | `:237` | same as 29 | shell (owned) | `PickerProc` `:116` | STACK `PickerState` `:243` | caller `:257-258` |
| 31 | `PhiSplitDropCards` `phi_split_drop.cc:166-177` | `:132` | `WS_EX_LAYERED\|TRANSPARENT\|NOPARENTNOTIFY` child | shell | NONE (`DefWindowProcW` `:172`; `UpdateLayeredWindow`) | SING `Cards::instance` | `~Cards` `:138-142` |
| 32 | `PhiBookmarkManager` `phi_bookmark_manager.cc:159-167` (CS_DBLCLKS) | `:169` | `WS_CHILD\|CLIPCHILDREN\|CLIPSIBLINGS` | shell | `PageProc` `:800` | POST `:178` | owner object |
| 33 | `EDIT` search child of 32 | `:187` | `WS_EX_CLIENTEDGE` child | 32 | none | — | with 32 |
| 34 | `PhiTabPreview` `phi_tab_preview.cc:38-47` | `:142` | `WS_EX_TOOLWINDOW\|NOACTIVATE`, `WS_POPUP`, DWM round `:150-152` | shell (owned) | `WndProc` `:681` (HTTRANSPARENT) | POST `:148` | owner object |
| 35 | `PhiNewTabPage` `phi_new_tab_page.cc:102-110` | `:115` | `WS_CHILD\|CLIPCHILDREN\|CLIPSIBLINGS` | shell | `PageProc` `:389` | POST `:126` | owner object |
| 36 | `EDIT` child of 35 | `:162` | child | 35 | uses controller's `UrlFieldProc` (see `:2029` "hwnd != self->url_field_") | — | with 35 |
| 37 | `PhiKiosk` `phi_kiosk_window.cc:166-182` (CS_HREDRAW\|CS_VREDRAW, icons) | `:204` | `WS_OVERLAPPEDWINDOW\|WS_CLIPCHILDREN`, custom caption in-proc `:919-1001` | top-level | `WndProc` `:1068` | NCC `:1072-1081` | object dtor (USERDATA cleared `:260`) |
| 38 | `PhiSettings` `phi_settings_window.cc:323-333` (CS_HREDRAW\|CS_VREDRAW, **hbrBackground = COLOR_WINDOW+1** `:330` yet `WM_ERASEBKGND` returns 1 `:3628-3632`) | `:336` | `WS_OVERLAPPED\|CAPTION\|SYSMENU`, custom caption in-proc `:3552-3627` | top-level | `WndProc` `:3539` | NCC `:3543` | `g_instance` singleton; `WM_CLOSE` hides `:3765-3767`; `delete g_instance` `:444` |
| 39 | `PhiSearchTabsPanel` `phi_search_tabs_panel.cc:148-158` | `:99` | `WS_EX_TOOLWINDOW`, `WS_POPUP` | shell (owned) | `PanelProc` `:544` | SING `Panel::instance` `:546` (USERDATA also set `:548-552` but unused) | self; `delete self` in `WM_DESTROY` `:616-621` (**USERDATA not cleared**) |
| 40 | `EDIT` child of 39 | `:107` | child | 39 | `FieldProc` via `GWLP_WNDPROC` `:118`, base in **process global** `g_search_edit_base_proc` `:43` | POST `:115` | with 39 |
| 41 | `PhiToast` `phi_toast.cc:128-138` | `:190` | `WS_EX_TOOLWINDOW\|NOACTIVATE\|LAYERED\|TOPMOST`, `WS_POPUP`, `LWA_ALPHA` `:199` | shell (owned) | `ToastProc` `:42` (HTTRANSPARENT) | SING `g_toast` `:43` | self at fade end `:61`; `delete self` in `WM_DESTROY` `:112-123` |
| 42 | `BUTTON`s styled by `phi_popup_chrome.cc:205-220` | callers | `BS_OWNERDRAW` | various | `SetWindowSubclass(ButtonProc)` `:211`, heap `ButtonState` deleted at `WM_NCDESTROY` `:67-70` | subclass data | with parent |

**Registration idioms (four, none checks `ERROR_CLASS_ALREADY_EXISTS`, no
`UnregisterClass` anywhere):** `static const ATOM atom = []{}` (rows 1-4, 9,
15, 20-22, 24, 27, 37, 38, 41); `static bool registered = []{}` (10-12, 17,
23, 25, 28-30, 32, 34, 35, 39); `static bool done; if (done) return;` (13,
14); `static bool registered = false; if (!registered)` (19, 31, 5). Eleven
classes spell the class name as a bare string literal at both the register
and the create site (e.g. `L"PhiExtensionsPopover"` at
`phi_extensions_popover.cc:140` and `:225`).

**Binding idioms (five):** NCC (14 classes), POST (8 — every message
delivered during `CreateWindowEx` sees `self == nullptr`, so the initial
`WM_SIZE`/`WM_CREATE` are silently dropped: `phi_bookmark_manager.cc:178`,
`phi_create_space_panel.cc:254`, `phi_new_tab_page.cc:126`,
`phi_tab_preview.cc:148`, `phi_window_controller.cc:8756`), SING (8),
STACK (2), NONE (2). Two subclass chains keep the base `WNDPROC` in a
process global on the assumption that every `EDIT` shares one class proc
(`phi_window_controller.cc:298-300`, `phi_search_tabs_panel.cc:43`).

---

## 2. What each class hand-rolls (matrix)

Columns: DB = per-paint double buffer; HOV = `TrackMouseEvent` + hover
index; DIS = dismiss-on-deactivate; ESC = Escape handling; SCR = wheel
scrolling; DPI = `WM_DPICHANGED`; CAP = custom caption frame
(`WM_NCCALCSIZE`/`WM_NCHITTEST`/hot buttons); ANIM = own easing loop.
"—" = not needed; "✗" = needed but absent.

| Class | DB | HOV | DIS | ESC | SCR | DPI | CAP | ANIM |
|---|---|---|---|---|---|---|---|---|
| Shell | cached buffer `wc:11429-11441` | `wc:11148-11149`, `:11085-11087` | — | via url field `wc:2021` + hook `co:7800` | `wc:11322-11330` | `wc:11384-11399` (does not set `dpi_`; relies on `ComputeLayout` side effect `:668-670`) | in-proc `wc:10951-11103` | 4 loops: swap `:11365-11376`, drag slide `:11356-11364`, add-space fade `:4582-4609`, tab-preview slide `tp:465-474` |
| Floating sidebar | per-paint `wc:8827-8838` | forwards to shell after **RebuildTabs** `:8861-8868` | — | ✗ | `:8917-8927` | ✗ | — | 100 ms tick `:8763` |
| Downloads popover | per-paint `dl:702-712` | `:574-581` (re-arms TME every move) | `WM_ACTIVATE` + 500 ms grace `:647-658` | ✗ (no `WM_KEYDOWN`) | `:665-675` | ✗ | — | — |
| Extensions popover | per-paint `ex:423-433` | `:341` | `:382-391` | ✗ | ✗ | ✗ | — | — |
| Search tabs | **none** `st:607-614` | mouse → selection `:582-590` | `:554-562` | field only `:529-532` | `:600-606` | ✗ | — | — |
| Folder picker | none `fp:122-129` | `:131-156` | `:179-185` | `:173-178` | ✗ | ✗ | — | — |
| Space theme panel | none `sp:311-318` | `:320-350` | `:393-401` | `:387-392` | ✗ | ✗ | — | — |
| Tab preview | per-paint `tp:705-720` | — (HTTRANSPARENT) | timers on the **shell's** HWND `:164`, `:220`, `:472` | — | — | ✗ | — | slide via shell timer |
| Toast | none `to:75-110` | — | — | — | — | — | — | fade `:47-73` |
| Tab switcher HUD | none `ts:264-271` | — | — | via hook `co:7765` | — | ✗ | — | — |
| Hover card | per-paint `wc:10133-10150` or shaped layer `:10108` | — | — | — | — | — | — | recommitted every shell frame `:10106` |
| Omnibox popup | ? `wc:2216` | `:2225-2228` | closes on url `WM_KILLFOCUS` `:2053-2057` | `:2021-2027` | ✗ | ✗ | — | — |
| Bookmark manager | per-paint `bm:818-834` | `:857-868` | — | `OnKey` `:878-882` | `:869-877` (**Rebuild + erase**) | ✗ | — | — |
| New tab page | per-paint `ntp:407-423` | — | — | via `UrlFieldProc` | — | ✗ | — | — |
| Create-space panel | per-paint `cs:1135-1151` | `:1167-1198` (tracks `tracking_leave_`) | — | `:1278-1289`, `:1327-1335` | `:1259-1277` | ✗ | — | — |
| Group overview | none `go:316-323` | ✗ | `WM_KILLFOCUS` `:337-341` | `:331-336` | ✗ | ✗ | — | — |
| Peek card + controls | none (`Paint()`) | `pk:597-617` | via mouse hook `co:7695-7710` | via hook `co:7800-7806` | — | ✗ | — | — |
| Reader panel | none `rp:284-303` | — | — | — | — | ✗ | — | — |
| Agent overlay (3) | per-paint `ao:734-745` | ✗ | — | — | — | ✗ | — | sprite tick `:285`, `:713-722` (GDI+) |
| Drag image | `UpdateLayeredWindow` | — | — | — | — | — | — | slide `di:416-428` |
| Split-drop cards | `UpdateLayeredWindow` | — | — | — | — | — | — | — |
| Prompt (Alert) | none (`Paint()`) | via `PhiPopupChrome` | modal | `IsDialogMessage` `pr:173` + `IDCANCEL` `:460` | — | via chrome `pc:586-589` | `PhiPopupChrome` | — |
| Feedback | none | via chrome | — | `ControlProc` `fb:775` | — | `:945-954` (no font rebuild) | `PhiPopupChrome` | — |
| Transcript | none | via chrome | — | ✗ | — | `:326-338` (**fonts not rebuilt**, `EnsureFonts` `:184-195` runs once) | `PhiPopupChrome` | — |
| Settings | per-paint `sw:3644-3661` | `:3593-3610`, `:3671-3682` | — | `WM_CHAR` `:3691-3711` | `:3712-3737` | `:3738-3753` | in-proc `:3552-3627` (third copy) | — |
| Kiosk | `Paint()` (has CompatDC) | `:981-1026` | — | — | — | `:1002-1014` (fonts re-made on demand `:455`) | in-proc `:919-1001` (fourth copy) | — |

Cross-cutting absences:

* **Accessibility**: no `WM_GETOBJECT`, `IAccessible`, `IRawElementProvider*`,
  `UiaReturnRawElementProvider` or `NotifyWinEvent` in any file (grep empty).
  Only the raw `EDIT`/`BUTTON`/`LISTBOX` children are visible to UIA.
* **RTL**: no `WS_EX_LAYOUTRTL`, `DT_RTLREADING`, `GetLayout` (grep empty);
  every layout is hand-computed left-to-right.
* **Tooltips**: no `TOOLTIPS_CLASS`/`TTM_*` (grep empty); nav buttons have no
  text alternative.
* **OS theme change**: no `WM_SETTINGCHANGE`/`ImmersiveColorSet`/
  `WM_THEMECHANGED` handler (grep empty). `PhiSystemPrefersDark` reads the
  registry (`phi_theme.cc:106-118`) only when `PhiResolvePalette` runs
  (`:129-131`); `kSystem` appearance therefore follows the OS toggle only
  when something else calls `ApplyTheme` (`phi_coordinator.cc:3796-3804`).
  Popups snapshot `PhiPopupColorsNow()` at construction (`phi_prompt.cc:83`,
  `phi_feedback_window.cc:302`); `PhiSetPopupPalette` just stores a pointer
  (`phi_popup_chrome.cc:154-156`) and `ApplyTheme` notifies only the
  controllers and Settings (`:3800-3803`) — an open Feedback/Transcript
  window keeps the old colours.
* **Custom caption frame** is implemented four times: shell
  `wc:10951-11103`, kiosk `kw:919-1001`, settings `sw:3552-3627`,
  `PhiPopupChrome` `pc:493-594`. Only the last is reusable.
* **Anchor placement with monitor clamping** is copied in
  `phi_downloads_popover.cc:150-162`, `phi_folder_icon_picker.cc:221-235`,
  `phi_space_theme_panel.cc:437-456`, `phi_tab_preview.cc` (`CardOrigin`),
  `phi_search_tabs_panel.cc:93-101`.

(`wc` = phi_window_controller.cc, `co` = phi_coordinator.cc, `pc` =
phi_popup_chrome.cc, etc.)

---

## 3. Paint discipline

### 3.1 Non-paint work inside WM_PAINT / WM_PRINTCLIENT

| Site | What runs | Why it matters |
|---|---|---|
| `phi_window_controller.cc:8818-8850` `PaintFloatingSidebar` (called from `WM_PAINT` `:8872-8880` **and** `WM_PRINTCLIENT` `:8881-8885`) | `RebuildTabs()` `:8825` (two JSON parses: `ReadStrip` `:6366`, `TabGroupsJson` `:6371`), `ComputeLayout()`, a per-paint compatible bitmap `:8827-8828`, then **`ScheduleRefresh()` `:8840`** which posts a rebuild and invalidates the *shell* | Violates §2.7 directly; every `AnimateWindow` frame of the slide re-parses the strip; painting one window invalidates another, so the two windows ping-pong |
| `:8861-8868` `forward` lambda in `FloatingSidebarProc` | `RebuildTabs()` on every `WM_MOUSEMOVE`, click, wheel `:8886-8927`, then `ScheduleRefresh()` + `InvalidateRect(hwnd)` | JSON parse per mouse move while the floating panel is open |
| `:11400-11411` shell `WM_PRINTCLIENT` | `RebuildTabs()` synchronously `:11406` | Same model rebuild that `WM_PAINT` was carefully moved off `:11416-11423` |
| `:11269-11284` `WM_LBUTTONDBLCLK` | `RebuildTabs()` `:11274` | Input handler rebuilds the model instead of reading the finished layout |
| `phi_settings_window.cc:3633-3665` | `self->Rebuild()` `:3647` inside `WM_PAINT`; `Rebuild()` reads `host_->DeveloperMode()` `:2368`, `host_->Palette()`, lazily calls `ThisExeIsDefaultBrowser()` (registry walk `:48`, `:2011-2012`) and arms `kProfilePollTimer` `:1845` | Layout + host reads + registry I/O + timer arming on the paint path; `WM_TIMER` then re-invalidates with erase `:3687` — a paint→timer→paint loop while a profile is pending |
| `phi_bookmark_manager.cc:869-877` | `WM_MOUSEWHEEL` → `Rebuild()` + `InvalidateRect(..., TRUE)` | Wheel rebuilds rows rather than moving a scroll offset over a finished layout |

### 3.2 `ComputeLayout()` is not a pure function

`phi_window_controller.cc:662-672`: every call does `GetClientRect`,
`GetDpiForWindow`, **assigns `dpi_`**, and calls `EnsureFont()` (which can
create up to nine `HFONT`s `:465-605`). It has 57 call sites in the
controller. One `WM_MOUSEMOVE` on the shell calls it at `:11109`, `:11117`,
then inside `OnChromeMouseMove` at `:4436`, `:4475`, `:4501`, `:4509`, and in
`SetUrlFieldHover` `:4545` — roughly eight full layouts per pointer move,
each with a DPI query. This is also the *only* place the shell's DPI is
updated; `WM_DPICHANGED` `:11384-11399` does not set `dpi_` itself.

### 3.3 Invalidate storms

* Any hover-kind change invalidates the **entire shell**
  (`:4526-4535`), as do url-field hover (`:4553-4555`), `ClearHover`
  (`:4571-4573`) and strip-row hover (`:4594-4597`). The shell repaint then
  recomposes all chrome: 48 gradient bands (`:9145-9172`), the bookmark
  bar, the caption band, the sidebar band, and every row. No dirty rects
  are used anywhere in the controller (26 full-window invalidations).
* `SetStripRowHovered` starts a 16 ms timer that invalidates the whole
  window per tick for the "+" fade (`:4595-4596`, `:11345-11355`); the
  space-swap timer does the same plus `RefreshCornerCaps()` (four layered
  commits) and `RepaintUrlField()` per tick (`:11365-11376`).
* The hover card is *recommitted through `PhiCommitShapedLayer` on every
  shell frame* (`:10106-10115`): a `CreateDIBSection`, a `GetDC(nullptr)`,
  a per-pixel `FunctionRef` coverage loop and `UpdateLayeredWindow`
  (`phi_dwrite.cc:1012-1056`) for ~280×200 pixels each time the shell
  repaints while a Space pip is hovered.
* Settings invalidates with `bErase = TRUE` in seven places
  (`:3533`, `:3687`, `:3708`, `:3725`, `:3735`, `:3751`, `:3762`) although
  `WM_ERASEBKGND` returns 1 (`:3628-3632`) and the class has a background
  brush (`:330`) — three contradictory statements about erasing.

### 3.4 Double-buffer boilerplate

The `CreateCompatibleDC / CreateCompatibleBitmap / SelectObject / Paint /
BitBlt / DeleteObject / DeleteDC` block appears verbatim in ten places:
`phi_bookmark_manager.cc:818-834`, `phi_create_space_panel.cc:1135-1151`,
`phi_downloads_popover.cc:702-712`, `phi_extensions_popover.cc:423-433`,
`phi_new_tab_page.cc:407-423`, `phi_settings_window.cc:3644-3661`,
`phi_tab_preview.cc:705-720`, `phi_agent_overlay.cc:734-745`,
`phi_window_controller.cc:10133-10150`, `:8827-8838`. Only the shell caches
its buffer (`:11433-11441`). Nine surfaces paint straight to the window
(toast `:75-110`, group overview `:316-323`, tab switcher `:264-271`, search
tabs `:607-614`, folder picker `:122-129`, space theme `:311-318`, reader
`:284-303`, prompt, feedback, transcript) — the search-tabs and space-theme
panels visibly flash on hover because they invalidate with erase
(`st:587`, `:604`).

### 3.5 GDI object lifetime

No outright handle leaks were found on the normal paths (every `HFONT`/`HBRUSH`
member has a matching `DeleteObject` in a destructor or `WM_DESTROY`). Issues
are of the "wrong lifetime" kind:

* `phi_window_controller.cc:9210-9213`: `static HICON logo` inside
  `PaintChrome` — a process-wide static in an instance method, loaded at the
  *first window's first DPI* (`logo_size = Scaled(18)` `:9207`) and never
  reloaded; every other window and every DPI change reuses it.
* `phi_settings_window.cc:2963-2988`: a font is created and deleted **per
  icon item per paint** (`CreateFontIndirectW` `:2969`, `DeleteObject`
  `:2987`).
* `phi_transcript_panel.cc:184-195` `EnsureFonts` has no DPI guard and is
  only called from the constructor (`:104`); `WM_DPICHANGED` (`:326-338`)
  updates `dpi_` and relayouts but keeps the old-size fonts. Same shape in
  `phi_feedback_window.cc:945-954`.
* `phi_search_tabs_panel.cc:576-578`: `field_brush_` is created once from
  the palette and never compared/refreshed (contrast
  `phi_bookmark_manager.cc:912-919`, `phi_new_tab_page.cc:470-477`,
  `phi_create_space_panel.cc:1300-1306`, which do refresh) — four copies of
  a "cached brush by colour" idiom, one of them wrong.
* Per-paint brush churn: `PhiPopupChrome::PaintCaption` `:608-610`,
  `:666-668`; `PhiReaderPanel::Paint` `:299-301`; controller `:9266`,
  `:9274`, `:9278`, `:9316`, `:9330`, `:10140`, `:10525`, `:10679`, `:10767`
  (18 `CreateSolidBrush` sites in the controller, most on paint paths;
  only the 48 band brushes are cached `:9147-9167`).
* Four independent `GdiplusStartup` tokens, never shut down:
  `phi_agent_overlay.cc:49-51`, `phi_drag_image.cc:43-45`,
  `phi_split_drop.cc:60-62`, `phi_window_capture.cc:27-29`.

### 3.6 Curve drawing outside `phi_dwrite`

Sanctioned by the memory note (hosts a browser child): peek card
`phi_peek_panel.cc:302-304`, reader `phi_reader_panel.cc:194-196`. Sanctioned
fallbacks (taken only when the layer commit fails): status bubble
`wc:2533-2540`, corner caps `wc:2701-2713`, hover card `wc:10093-10098`,
`:10116-10122`.

Violations / unjustified:

1. **`phi_space_theme_panel.cc:257-278`**: the saturation slider's gradient
   is clipped with `CreateRoundRectRgn` + `ExtSelectClipRgn` — exactly the
   case `PhiKnockOutRoundRectAA` was written for (`phi_dwrite.h:132-140`).
   The create-space panel's identical slider already does it correctly
   (`phi_create_space_panel.cc:1110-1114` uses AA circles).
2. **`phi_agent_overlay.cc:145-147`**: the agent pill is shaped with
   `SetWindowRgn(CreateRoundRectRgn)`. The memory note excuses this because
   the pill "hosts child controls", but it does not: its two buttons are
   `RECT`s painted by hand (`primary_`/`secondary_` at `:594-607`, hit-tested
   at `:666-676`); there are only three `CreateWindowEx` calls in the file
   (`:68`, `:74`, `:78`). It could be a `PhiCommitShapedLayer` child like the
   status bubble. The pill's rounded background is additionally painted with
   **GDI+** (`:542-552`, `LinearGradientBrush`/`DrawPath`) — a third curve
   rasterizer in the client beside GDI and `phi_dwrite`.
3. **`phi_peek_panel.cc:486-502`**: the peek control chips are a region
   union of three `CreateRoundRectRgn`s on a window that hosts no child;
   known in the memory note as "still region-cut", listed here for
   completeness.
4. `phi_window_controller.cc:8842-8846`: the floating sidebar draws a square
   1 px `Rectangle` border on a popup whose corners DWM rounds
   (`:8758`), so the hairline is clipped at the four corners.

### 3.7 DPI assumptions

* `phi_split_drop.cc:54` `kZoneExitTolerance = 12` (used at `:431`) and
  `:56` `kSplitDividerThickness = 2` (used `:455-465`) are applied in
  physical pixels by free functions that have no DPI; the `Cards` class in
  the same file scales (`:179`).
* `phi_toast.cc`, `phi_tab_switcher.cc`, `phi_group_overview.cc`,
  `phi_tab_preview.cc`, both popovers and the search-tabs panel size their
  fonts from the owner's DPI at creation and never handle `WM_DPICHANGED`
  (§2 matrix). Acceptable for sub-second popups, wrong for the tab preview
  and the search-tabs panel which can be dragged across monitors by their
  owner.
* Nineteen copies of `int Scaled(int) { return MulDiv(design, dpi_, 96); }`
  (grep: `phi_extensions_popover.cc:205`, `phi_agent_overlay.cc:236`,
  `phi_group_overview.cc:86`, `phi_downloads_popover.cc:195`,
  `phi_tab_switcher.cc:111`, `phi_tab_preview.h:117`,
  `phi_search_tabs_panel.cc:146`, `phi_toast.cc:31`, `phi_kiosk_window.h:126`,
  `phi_transcript_panel.cc:168`, `phi_peek_panel.h:123`, and the out-of-line
  ones in bookmark, create-space, popup chrome, prompt, feedback, split drop,
  settings, NTP, controller).

---

## 4. Timers, capture, focus, modal loops

### 4.1 Timer id allocation

| HWND | Ids in use (file:line of constant) |
|---|---|
| shell | `0x515` swap (`wc:199`), `0x516` hover card (`:205`), `0x518` drag slide (`:209`), `0x523` copy-link revert (`:216`), `0x524` add-space fade (`:220`); **set by another object**: `0x540/0x541/0x542` tab preview (`phi_tab_preview.h:99-101`, armed at `tp:164`, `:220`, `:472`) and dispatched first in the shell's `WM_TIMER` (`wc:11332`) |
| floating sidebar | `0x522` (`wc:214`) |
| coordinator msg window | `3` named (`phi_coordinator.h:626`); **`1` and `2` as bare literals** (`co:4552`, `:4596`, `:6158`, `:6810`); the handler treats *any* id other than 2 and 3 as the profile-space poll and kills timer 1 (`:304-320`) |
| downloads popover | `0x521` (`dl:45`) |
| extensions popover | `1` (`ex:37`) |
| feedback | `1` (`fb:63`) |
| kiosk | `1` (`kw:92`) |
| toast | `1`, `2` (`to:15-16`) |
| settings | `0x517` (`sw:249`) |
| agent sprite | `0x531` (`ao:43`) |
| drag image | `1` (`di:28`) |

No two ids collide on the same HWND today. The risk is structural: the
`0x515…0x542` numbers are an unwritten cross-file registry (gaps at
`0x519-0x520`, `0x525-0x530`, `0x532-0x53F` show ids were picked by hand to
avoid each other), while five files also use literal `1`. The tab preview
arming timers on a window it does not own is the concrete hazard: a future
shell timer `0x540` would be swallowed by `tab_preview_->OnTimer`.

### 4.2 Mouse capture

* Shell: `SetCapture` at `wc:3197` (drag) and `:11256` (sidebar resize);
  `ReleaseCapture` `:3405`, `:11290`; `WM_CAPTURECHANGED` `:11300-11310`
  aborts both. Correct.
* Create-space panel: `:1227`/`:1251`, `WM_CAPTURECHANGED` `:1254-1258`.
  Correct.
* **Space theme panel: `SetCapture` `sp:375`, `ReleaseCapture` `:384`, no
  `WM_CAPTURECHANGED`/`WM_CANCELMODE`** (grep shows only two files handle
  it). If capture is lost (Alt+Tab, a menu, another window calling
  `SetCapture`), `state->dragging` stays true: the next `WM_MOUSEMOVE`
  applies saturation with no button down (`:325-329`), and the
  `WM_ACTIVATE` dismissal is suppressed while `dragging` (`:397`), so the
  modal panel stays up under another app.
* Drag image and split-drop cards are `WS_EX_TRANSPARENT` and depend on the
  shell holding capture (`phi_split_drop.cc:130-131`); nothing enforces
  that invariant.

### 4.3 Keyboard focus between the client HWNDs and the Chromium child

* The shell forwards `WM_ACTIVATE`/`WM_SETFOCUS` to the browser child
  (`wc:10888-10914`) and decides `WM_MOUSEACTIVATE` by whether the click is
  inside `ComputeLayout().content` (`:10859-10886`) — with a growing list of
  hand-maintained exceptions (group overview, NTP `:10878-10879`, url field
  `:10902`).
* Because the focused window is Chromium's, **accelerators are a thread
  `WH_KEYBOARD` hook** (`co:205`, `:7756-7834`) that walks `windows_` to find
  the shell (`ShellForFocus` `:7741-7753`) and posts `WM_COMMAND`. Escape is
  handled in the hook for peeks (`:7800-7806`) *and* separately in nine
  wndprocs (§2). There is no `WM_GETDLGCODE`, no `IsDialogMessage` outside
  the prompt (`pr:173`), and tab order exists only in the feedback window
  (`fb:770-774`).
* Group overview dismisses on `WM_KILLFOCUS` (`go:337-341`) while popovers
  dismiss on `WM_ACTIVATE` — two different focus-loss models; the popovers
  need a 500 ms "opening grace" against activation churn from the browser
  child (`dl:649-654`, `ex:383-388`, `st:556-559`).

### 4.4 Nested message loops

| Loop | Owner disabled? | `IsDialogMessage`? | Hook still live? | State lifetime |
|---|---|---|---|---|
| `phi_prompt.cc:169-177` | yes `:151` | yes `:173`, plus a caller filter `:170` | yes | heap `Alert`; `Finish()` posts `WM_NULL` to wake the loop `:121-129` |
| `phi_folder_icon_picker.cc:252-255` | **no** | **no** | yes | **stack** `PickerState` with raw `controller` `:215-216`; cleared before destroy `:257` |
| `phi_space_theme_panel.cc:472-475` | **no** | **no** | yes | **stack** `PanelState` with raw `controller` `:431-432`; cleared `:477` |

While the two panel loops spin, the `WH_KEYBOARD` hook keeps translating
chords into `WM_COMMAND` on the shell (`co:7817-7822`), and the control
server's `SendMessage(msg_hwnd_, WM_PHI_CONTROL)` (`co:388`) is serviced
inside the loop. Either can close the shell that owns the panel; the stack
`controller` pointer is then dangling for the remainder of the loop
(`sp:291`, `:359-362`). The `WM_NULL` kick pattern (six sites:
`fp:170`, `:176`, `:183`, `pr:128`, `sp:390`, `:399`) exists because
`done` is checked only *between* `GetMessage` returns.

---

## 5. Lifetime

* **`delete self` inside the wndproc** (all in `WM_DESTROY`, not
  `WM_NCDESTROY`): downloads `dl:716-725`, extensions `ex:437-446`,
  feedback `fb:958-966`, toast `to:112-123`, search tabs `st:616-621`.
  Children are destroyed *after* `WM_DESTROY`, so their subclass procs run
  against a freed owner: `FieldProc` reads the freed `Panel*` from the EDIT's
  `GWLP_USERDATA` (`st:514-515`) — safe only because it dereferences `self`
  solely on `WM_KEYDOWN`. Search tabs also never zeroes `GWLP_USERDATA`
  (`:616-621`).
* **Delete-self from a non-destroy message**: transcript `WM_CLOSE`
  (`tr:385-391`) and drag image `WM_TIMER` (`di:437-439`); both destroy
  their own HWND from inside its wndproc (legal) and return without touching
  members. Group overview handles the `Close()`→`~Overview`→`DestroyWindow`
  →`WM_KILLFOCUS`→`Close()` re-entrancy by nulling the singleton first
  (`go:373-378`) — a documented landmine rather than a design.
* **Shell teardown**: `WM_CLOSE`→`RequestClose()` "may destroy `self`"
  (`wc:11473-11484`); the destructor then calls `DestroyWindow(shell_hwnd_)`
  (`:1412`) from inside that same `WM_CLOSE` dispatch. `GWLP_USERDATA` is
  never cleared for the shell, corner caps, status bubble, omnibox popup or
  hover card (only four files handle `WM_NCDESTROY` at all: `fb:759`,
  `di:446`, `pc:67`, `wc:328`), so every message delivered during
  `DestroyWindow` (`WM_ACTIVATE`, `WM_SIZE`→`LayoutContents` `:11378-11383`,
  `WM_WINDOWPOSCHANGED` on children) runs member code on an object whose
  destructor body is executing.
* **Browser subclass** keeps its `WNDPROC` map entry until `WM_NCDESTROY`
  (`wc:328-335`) but the controller destructor `SetParent(browser_hwnd_,
  nullptr)` (`:1409`) leaves the subclass installed on a now top-level
  Chromium window; it then posts `WM_PHI_RAISE_CAPS` to `GetParent()==NULL`
  (`:321-326`) — harmless today, undefined by design.
* **Popovers carry hidden static coupling**: `Popover::last_owner` and
  `closed_tick` statics (`dl:719-720`, `ex:440-441`) implement "clicking the
  button that opened it closes it" across two objects' lifetimes.
* **Peek/reader/kiosk/prompt** clear `GWLP_USERDATA` before `DestroyWindow`
  (`pk:256`, `rp:134`, `kw:260`, `pr:104`) — the correct pattern, applied to
  four of the 33 classes.

---

## 6. Proposed target substrate

### 6.1 `PhiWindow` (one base, ~400 lines)

```cpp
class PhiWindow {
 public:
  struct ClassSpec { const wchar_t* name; UINT style; HCURSOR cursor; HICON icon; };
  HWND Create(const ClassSpec&, DWORD ex, DWORD style, RECT, HWND owner_or_parent);
  UINT dpi() const;  int Scaled(int) const;  RECT Scaled(RECT) const;
  const PhiFontSet& fonts() const;           // (face,size,weight) -> HFONT, keyed by dpi
  const PhiPalette& palette() const;         // resolved by the theme bus
  PhiTimerId AllocTimer(); void Release(PhiTimerId);
  Capture BeginCapture();                    // RAII; WM_CAPTURECHANGED/WM_CANCELMODE -> OnCaptureLost()
 protected:
  virtual LRESULT OnMessage(UINT, WPARAM, LPARAM, bool* handled);
  virtual void Paint(HDC back_buffer, const RECT& dirty) = 0;
  virtual void OnDpiChanged(UINT old_dpi) {}
  virtual void OnThemeChanged() {}
  virtual void OnHover(int hit) {}           // driven by a rect list, dirty-rect invalidation
  virtual void OnCaptureLost() {}
  virtual void OnNcDestroy() {}              // GWLP_USERDATA zeroed by the base
  std::vector<PhiHit> hits_;                 // {RECT, key, name, role} — hover, click, UIA, tooltip
};
```

What the base absorbs, with today's copies retired:

| Concern | Base behaviour | Retires |
|---|---|---|
| Class registration | atom cache by name; one `WNDCLASSEXW` builder | 33 registration blocks, 4 idioms |
| `this` binding | NCC on create, zeroed at `WM_NCDESTROY`, static thunk | 5 idioms, 8 POST-bound classes that drop creation messages |
| Paint framing | cached back buffer per window, `WM_ERASEBKGND`=1, `WM_PRINTCLIENT` = same path, dirty-rect `BitBlt` | 10 buffer copies, 9 unbuffered surfaces, the erase contradictions in Settings |
| DPI | `dpi_` set only in `WM_DPICHANGED` (+ initial), `OnDpiChanged` rebuilds the `PhiFontSet` | `ComputeLayout`'s side effects (`wc:662-672`), 19 `Scaled()` copies, stale fonts in transcript/feedback/preview |
| Theme | one `PhiThemeBus` on the coordinator's message window that listens to `WM_SETTINGCHANGE`/`ImmersiveColorSet` and broadcasts `OnThemeChanged` | the registry probe in `PhiResolvePalette`, the stale popup colours, `PhiSetPopupPalette` |
| Hover | `TrackMouseEvent` armed once per enter; hit-test over `hits_`; invalidates the union of old+new hit rects only | 14 `TrackMouseEvent` sites, whole-window hover invalidation |
| Timers | per-window id bitmap; `PhiTimer` RAII | the informal 0x515 space, literal `1`s, cross-object arming |
| Capture | RAII + `OnCaptureLost` | the space-theme stuck-drag bug |
| Focus/keys | `OnKey(VK)` with `Esc`/`Enter` defaults; `WM_GETDLGCODE` | nine Escape handlers |
| Accessibility | `IRawElementProviderSimple`/`FragmentRoot` over `hits_`; `WM_GETOBJECT` → `UiaReturnRawElementProvider`; tooltips from `PhiHit::name` | nothing today — this is net new but nearly free because every surface already keeps the rect list (`tabs_`, `pinned_`, `bookmarks_`, `chips_`, `items_`, `cells`, `rows_`) |

### 6.2 Three families on top

* **`PhiPopover : PhiWindow`** — `WS_POPUP|WS_EX_TOOLWINDOW`, owner, anchor
  placement + monitor clamp, dismiss on `WA_INACTIVE` with the opening grace,
  Escape, DWM corners *or* `PhiShapedLayer`, optional non-activating mode.
  Collapses **11**: downloads, extensions, folder picker, space theme,
  search tabs, omnibox popup, hover card, tab preview, toast, tab switcher
  HUD, floating sidebar. Each keeps only its rows/model and `Paint`.
  The folder picker and space theme panel become *asynchronous* popovers
  with a completion callback — that deletes two of the three nested loops.
* **`PhiPanel : PhiWindow`** — `WS_CHILD|WS_CLIPSIBLINGS` over the content
  card, reads the slot palette, participates in the raise-caps contract
  (`WM_PHI_RAISE_CAPS` `wc:302-327`, `:11197-11221`) through one
  `PhiCardStack` z-order list instead of five ad-hoc `SetWindowPos(HWND_TOP)`
  calls. Collapses **12**: bookmark manager, NTP, create-space, group
  overview, peek card, peek controls, peek shadow, reader, agent fog/pill/
  sprite, split-drop cards. Peek/reader keep their browser-child hosting
  (and therefore their region), everything else becomes a shaped layer.
* **`PhiSecondaryWindow : PhiWindow`** — owns a `PhiPopupChrome`, runs
  `IsDialogMessage` for its control set from the *main* loop via a
  thread-local "active dialog" list (the way MFC/ATL do), colours from the
  theme bus. Collapses **5**: prompt, feedback, transcript, settings, kiosk.
  The prompt keeps a modal `Run()` only if a synchronous answer is truly
  required; the four caption-frame copies become one.
* **Shell** stays its own class but inherits `PhiWindow`; corner caps and
  status bubble become `PhiShapedLayer` children (no wndproc of their own);
  the coordinator's message window becomes `PhiMessageWindow`.

Net: 33 classes → 1 base + 3 families + shell + message window; the paint
bodies, models and command handlers of each surface survive unchanged.

### 6.3 One animator

`PhiAnimator` (per UI thread, one `SetCoalescableTimer` at 16 ms while any
animation is live) with `Animate(duration, easing, on_tick(t) -> dirty RECT,
on_done)`. Replaces seven hand-rolled easing loops: add-space fade
(`wc:4582-4609`, `:11345-11355`), drag slide (`:11356-11364`), space swap
ramp (`:11365-11376`), tab-preview slide (`tp:465-474`), toast fade
(`to:47-73`), drag-image slide-back (`di:416-428`), sprite glide
(`ao:713-722`). Ticks invalidate the animated rect, not the window.

### 6.4 Direct2D — recommendation and cost

**Use Direct2D as the rasterizer inside `phi_dwrite`; keep GDI as the
compositor. Do not move windows to D2D render targets.**

Why the internals should change: `phi_dwrite.cc` already owns an
`ID2D1DCRenderTarget` (`:46`, `:82-91`) for text and bitmaps, but the shape
primitives are software-rendered — `BlendCoverage` allocates a `std::vector`
per call and evaluates a `FunctionRef` per pixel (`:651-673`), the corner
tiles are stitched with three GDI fills (`:783-794`), and
`PhiCommitShapedLayer` runs a second per-pixel loop over the whole surface
(`:1037-1047`) on every commit. Routing `PhiFill/Stroke{RoundRect,Capsule,
Ellipse}AA` and the dashed variants through `BindDC` +
`FillRoundedRectangle`/`DrawRoundedRectangle`/`DrawEllipse` (with a
`D2D1_DASH_STYLE` stroke style) keeps the API and every call site, removes
~400 lines of coverage math, and gives GPU-quality AA with proper gamma.
The GDI fallback stays. Cost: 2–3 engineer-days in `phi_dwrite.cc` plus a
box pass with the PhiCap rigs; risk is low because the boundary is already
the function signature.

Why not full D2D windows: the shell composes a reparented DComp Chromium
child, `WS_EX_LAYERED` children (caps, bubble, fog, sprite, cards) and
owned popups. A D2D `HwndRenderTarget` or swap chain on the shell would
re-open the z-order and flashing fights already solved (`WM_PHI_RAISE_CAPS`,
the no-erase class brush `wc:353-355`), and the AA gain is captured by the
rasterizer swap alone. A later move to DirectComposition visuals for the
whole chrome is possible once `PhiPanel`/`PhiShapedLayer` exist, but it is
a separate project (2–4 weeks) with no user-visible payoff over the
rasterizer swap.

Substrate cost estimate: `PhiWindow` + `PhiPopover` and migrating the 11
popovers 1 week; `PhiPanel` + `PhiShapedLayer` and the 12 panels 1 week;
`PhiSecondaryWindow` and the five windows (including retiring the three
caption-frame copies) 3–4 days; animator 2 days; theme bus 1 day; UIA
provider over `hits_` for the sidebar, popovers and settings 1 week.

---

## 7. Ranked findings

1. **Model rebuilt inside paint, and paint invalidating another window.**
   `PaintFloatingSidebar` calls `RebuildTabs()` and `ScheduleRefresh()` from
   `WM_PAINT`/`WM_PRINTCLIENT` (`phi_window_controller.cc:8825`, `:8840`);
   `FloatingSidebarProc` rebuilds on every mouse move (`:8863`);
   `WM_PRINTCLIENT` rebuilds synchronously (`:11406`); Settings runs
   `Rebuild()`, a registry probe and timer arming in `WM_PAINT`
   (`phi_settings_window.cc:3647`, `:2011-2012`, `:1845`). Direct violation
   of `sidebar-architecture.md` §2.7.
2. **No accessibility at all.** Zero `WM_GETOBJECT`/UIA/MSAA code; the
   sidebar, tab strip, popovers, settings, NTP and bookmark manager are
   invisible to assistive tech. Fix is cheap given the existing rect lists
   (§6.1).
3. **Whole-shell invalidation on every hover change and eight
   `ComputeLayout()` calls per mouse move**, with `ComputeLayout` mutating
   `dpi_` and creating fonts (`:4533`, `:4554`, `:4572`, `:4596`;
   `:662-672`; `:11109`, `:11117`, `:4436`, `:4475`, `:4501`, `:4509`,
   `:4545`). The hover card is additionally re-rasterized through a
   per-pixel layered commit on every shell frame (`:10106-10115`).
4. **Space theme panel loses capture without noticing** — no
   `WM_CAPTURECHANGED`; `dragging` sticks, dismissal is suppressed, and mouse
   moves keep applying saturation (`phi_space_theme_panel.cc:375`, `:384`,
   `:325-329`, `:397`).
5. **Two modal loops with stack state, no owner disable, no dialog
   translation, and the accelerator hook still live**
   (`phi_folder_icon_picker.cc:252-255`, `phi_space_theme_panel.cc:472-475`;
   `phi_coordinator.cc:7817-7822`, `:388`). A hooked chord or a control-server
   command can destroy the controller the stack state points at.
6. **Shell/child `GWLP_USERDATA` never cleared; destructor destroys the
   window from inside its own `WM_CLOSE`** (`:11473-11484`, `:1412`; no
   `WM_NCDESTROY` on shell, caps, bubble, omnibox popup, hover card).
   Teardown messages run member code mid-destruction.
7. **No OS theme-change path.** `PhiSystemPrefersDark` is a registry read
   evaluated only inside `PhiResolvePalette` (`phi_theme.cc:106-131`); no
   `WM_SETTINGCHANGE` listener; open secondary windows keep stale
   `PhiPopupColors` (`phi_prompt.cc:83`, `phi_feedback_window.cc:302`,
   `phi_popup_chrome.cc:154-156`, `phi_coordinator.cc:3796-3804`).
8. **AA-rule violations with no justification**:
   `phi_space_theme_panel.cc:257-278` (region-clipped gradient — use
   `PhiKnockOutRoundRectAA`); `phi_agent_overlay.cc:145-147` (region-cut pill
   that hosts no child controls, plus a GDI+ rounded fill `:542-552`);
   `phi_peek_panel.cc:486-502` (region-cut chips). Four separate
   `GdiplusStartup` tokens (`ao:49-51`, `di:43-45`, `sd:60-62`,
   `phi_window_capture.cc:27-29`).
9. **Timer ids are an unwritten cross-file registry**; the coordinator uses
   bare literals `1`/`2` with a catch-all branch (`phi_coordinator.cc:304-320`,
   `:4552`, `:4596`, `:6158`, `:6810`); the tab preview arms timers on the
   shell's HWND (`phi_tab_preview.cc:164`, `:220`, `:472`) and the shell
   dispatches to it first (`wc:11332`).
10. **DPI drift**: transcript and feedback keep pre-move fonts after
    `WM_DPICHANGED` (`phi_transcript_panel.cc:184-195`, `:326-338`;
    `phi_feedback_window.cc:945-954`); `static HICON logo` in `PaintChrome`
    is loaded once at the first DPI for all windows
    (`phi_window_controller.cc:9210-9213`); `phi_split_drop.cc:54`, `:56`
    constants are unscaled physical pixels; six popup classes have no
    `WM_DPICHANGED`.
11. **Four copies of the custom caption frame** (`wc:10951-11103`,
    `kw:919-1001`, `sw:3552-3627`, `pc:493-594`) and **ten copies of the
    double-buffer block** (§3.4); nine surfaces still paint unbuffered and
    two of them invalidate with erase on hover (`st:587`, `sp` via `:327`).
12. **Per-paint GDI churn**: a font per icon item per paint in Settings
    (`sw:2963-2988`); brushes created/deleted per paint in
    `PhiPopupChrome::PaintCaption` (`:608-610`, `:666-668`), reader
    (`rp:299-301`) and ~13 controller sites; search-tabs `field_brush_` never
    refreshed on palette change (`st:576-578`).
13. **Process-global `WNDPROC` bases** for subclassed EDITs
    (`wc:298-300`, `st:43`) and the NTP field sharing the url field's proc
    (`wc:2029`) — one more subclass on any EDIT breaks the chain.
14. **Two focus-loss models** (`WM_KILLFOCUS` in group overview `go:337`,
    `WM_ACTIVATE`+grace in popovers `dl:647-658`) and nine Escape handlers;
    `WM_MOUSEACTIVATE` exceptions grow by hand (`wc:10878-10879`).
15. **No RTL support and no tooltips** anywhere (grep empty); becomes a
    property of `PhiWindow::hits_` once the base exists.

Everything above is fixable incrementally in the order of §6 — popovers
first (largest count, smallest risk), then panels, then the secondary
windows and the shell — without touching the sidebar model, the store or
the framework boundary.
