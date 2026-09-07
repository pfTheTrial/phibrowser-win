# Lane 1 — The god objects: `PhiWindowController`, `PhiCoordinator`, `PhiSettingsWindow`

Read-only review, 2026-09-07. Files read in full: `client/phi_window_controller.{h,cc}` (1,607 + 11,489 lines), `client/phi_coordinator.{h,cc}` (893 + 9,199), `client/phi_settings_window.{h,cc}` (353 + 3,772), `phi_framework_api.h`, `docs/sidebar-architecture.md`. Satellite files (`phi_menu_bar.cc`, `phi_agent_overlay.cc`, `phi_group_overview.cc`, `phi_tab_switcher.cc`, `phi_search_tabs_panel.cc`, `phi_peek_panel.*`, `phi_reader_panel.*`, `phi_surface_registry.h`, `phi_prompt.cc`, `phi_control_server.cc`, `phi_client_main_win.cc`) were grepped/spot-read only for coupling and lifetime evidence. All paths below are relative to `/home/jx/Phi/phibrowser-win/client/` unless stated; `ctrl:` = `phi_window_controller.cc`, `ctrl.h:` = `phi_window_controller.h`, `coord:` = `phi_coordinator.cc`, `coord.h:` = `phi_coordinator.h`, `settings:` = `phi_settings_window.cc`.

The sidebar rebuild (`docs/sidebar-architecture.md` §2, §6) is the house style used as the yardstick: one reconciliation boundary, stable keys, pure functions, nothing structural in `WM_PAINT`, rebuild on a posted turn.

---

## 1. Responsibility inventory

### 1.1 `PhiWindowController` (11,489 lines, 14 window procedures across the client, 8 in this file)

Line counts are approximate; ranges are contiguous where the code is, listed where it is scattered.

| # | Cluster | Where (ctrl:) | ~Lines | State members (ctrl.h:) |
|---|---|---|---|---|
| A | Process-wide statics & metrics | 61–362 constants; `g_edit_base_proc` 300; `BrowserBaseProcs()` NoDestructor 309–312; `BrowserSubclassProc` 314–339; `EnsureShellClass` 341–360; `PhiAppIcon`/`PhiLogoBgra256` NoDestructor 364–429 | 370 | none (process globals) |
| B | DPI & GDI fonts | `Scaled` 456–463; `EnsureFont` 465–605 | 150 | `ui_font_`…`ui_semibold_font_`, `*_dpi_`, `dpi_` (842–867) |
| C | Caption band + menu-bar host | `RebuildMenuBar` 607–616; `EnsureMenuTitleRects` 618–660; `CaptionHitTest`…`SetMenuHot` 6898–7047; `WM_NCCALCSIZE`/`WM_NCHITTEST`/`WM_NC*` 10951–11103; `kMsgPhiToggleMenuReveal` 11180 | 420 | `hot_caption_`, `pressed_caption_`, `menu_revealed_`, `menu_alt_revealed_`, `menu_open_`, `hot_menu_`, `menu_titles_`, `menu_titles_dpi_` (872–888); `menu_bar_` (1472) |
| D | Layout geometry | `Layout` struct (ctrl.h 666–743); `ComputeLayout` 662–1046; accessors 1048–1170; `LayoutContents` 2601–2716; `card_rect`/`CardRoundedCorners` 5013–5039 | 560 | `layout_mode_` (1576), `sidebar_dip_`, `sidebar_resizing_` (1158–1159), `sidebar_collapsed_` (1583) |
| E | Shell window lifecycle & browser-child hosting (slot = N Space browsers) | ctor 1185–1398; dtor 1400–1450; `AdoptBrowserAsChild` 2084–2113; `OnContentFullscreenChanged` 1626–1671; `OnPlaceholderModeChanged` 1673–1695; `SetAgentVisible` 5337–5363; `AttachSpaceBrowser` 5416–5463; `FocusActiveBrowser` 5465–5483; `ShowSpace` 5645–5684; `CloseSpaceBrowser` 5686–5706; `ForgetBrowser` 5708–5727; `RequestClose` 5729–5753; `Get/SetBounds` 5755–5776; `ShellWndProc` 10846–11487 | 1,250 | `window_id_`, `browser_hwnd_`, `space_browsers_`, `browser_hwnds_`, `space_order_`, `active_space_`, `active_browser_`, `shell_hwnd_` (1463–1471); `placeholder_browsers_`, `content_fullscreen_`, `fullscreen_browser_`, `pre_fullscreen_*` (1502–1508); `agent_`, `incognito_`, `agent_visible_` (1585–1589) |
| F | Address field / omnibox session (shared with the native NTP's EDIT) | `EnsureUrlField`…`CommitOmniboxField` 1452–1624; `StartNewTab` 1718–1733; `OnOmniboxFieldChanged`…`PaintOmniboxPopup` 1735–1997; `UrlFieldProc` 2000–2082; `OmniboxPopupProc` 2203–2249; `SetUrlFieldHover` 4538–4560; `RebuildNavState` 5778–5801; `RepaintUrlField` 5638; `WM_CTLCOLOREDIT` 10915–10950 | 720 | `url_field_`, `url_brush_`, `url_brush_color_` (834–839); `nav_url_`, `can_go_*`, `nav_loading_`, `copy_link_flash_`, `url_field_hover_` (1424–1438); `omnibox_rows_`, `omnibox_selected_`, `omnibox_popup_`, `url_field_programmatic_`, `omnibox_new_tab_pending_` (1487–1500) |
| G | Content-card overlays: corner caps, status bubble, z-order | `CornerCapProc`/`StatusBubbleProc` 2115–2198; `GroundTintAt` 2370–2386; `CommitCornerCap`…`PaintCornerCap` 2388–2458; `OnTargetURLChanged`/`SetStatusUrl`/`PaintStatusBubble` 2460–2568; `RaiseCornerCaps` 5041; `WM_PHI_RAISE_CAPS` 11197–11221 | 310 | `corner_caps_`, `status_bubble_`, `status_url_` (1475–1480) |
| H | Sidebar model → paint rows (the §6 pipeline) | `ReadStrip`/`StripTab` helpers 2251–2367; `RefreshTabs`/`ScheduleRefresh` 5395–5414; `RefreshFieldExtensions` 5803–5867; `RebuildChips`/`ChipHit`/`StepChipsViewport` 5869–6047; `DrawFavicon` 6067–6129; `GridMetrics`/`PinnedGridWalk`/`VisibleBookmarkCells` 6131–6177; `RebuildShortcuts` 6179–6348; `RebuildTabs` 6350–6769; `OpenOrFocusPinned`/`OpenStoredSplit`/`IndexOfTagged` 6771–6896; `LayoutRowsJson` 1069–1134 | 1,300 | `TabRow`/`tabs_`, `sidebar_tree_`, `add_tab_button_`, `GroupHeader`/`group_headers_`, `rows_dirty_`, `rebuild_posted_`, `back_buffer_*`, `band_brushes_`, `scroll_offset_`, `tabs_scroll_max_` (891–981); `ShortcutRow`/`pinned_`/`bookmarks_`, `BarItem`/`bar_items_`, `active_tab_url_`, `active_tab_id_`, `ntp_commit_tab_id_`, `pinned_row_count_`, `bookmark_row_count_`, `expanded_bookmarks_` (1270–1356); `tab_mru_` (1511); `SpaceChip`/`chips_*`, `add_space_button_`, `chips_overflow_` (1525–1554); `FieldExtension`/`field_extensions_`, `field_ext_count_` (1443–1452) |
| I | Drag & drop: reorder, conversions, tear-off, page split-drop, drag image, slide animation | `InSidebarResizeBand` 2718; `ArmDrag`…`DrawDragIndicator` 2728–4429 (contiguous); of which `RetargetDragSlide` 2894–3150, drag image 3422–3823, `TearOffStoredRow` 3825–3881, split drop 3883–4284 | 1,700 | ~45 members: `drag_kind_`…`drag_folder_key_` (989–1011), split-drop state (1019–1027), slide maps (1079–1100), drag image (1118–1122) |
| J | Hover, Space hover card, tab preview | `OnChromeMouseMove` 4431–4536; `ClearHover` 4562; `SetStripRowHovered`/`AddSpaceRevealProgress` 4577–4609; `UpdateTabPreview` 5093–5201; `UpdateHoverCard`…`ResolveHoverCard` 5203–5335; `MeasureHoverCard`…`HoverCardPopupProc` 9872–10184 | 650 | `hover_kind_`, `hover_index_` (1178–1179); `hover_card_space_`…`card_shortcut_` (1189–1196); `HoverCardGeometry`, `hover_card_popup_*` (1207–1217); `tab_preview_` (1231); `strip_row_hovered_`, `add_space_fade_start_` (1561–1562) |
| K | Create-Space form host + live theme preview | 4611–4748 | 140 | `create_space_panel_` (1241); `creating_space_`, `creating_space_icon_text_`, `creating_pip_bounds_`, `preview_palette_`, `preview_until_switch_` (1570–1574) |
| L | Split helpers (framework queries) | `FormSplitOrdered`/`UnbindStoredTab`/`SplitActiveWithNormalized` 4062–4131; `SplitIsStacked`/`SplitIdForTab`/`ApplySplitLayout` 4752–4826 | 150 | — |
| M | Native pages (bookmark manager, NTP) mount/unmount | 4832–4943; `kMsgPhiSyncNativePages` 11165–11170 | 120 | `bookmark_manager_`, `new_tab_page_` (1234–1240) |
| N | Reader View affordance | 4945–5011; `ToggleReaderView`/`LeaveReaderViewForNavigation` 5056–5091; `ShowsTab` 5047 | 130 | `distillable_tabs_` (1433) |
| O | Space-switch animation (band slide + palette ramp) | `LerpPalette` 5489–5518; `SwapEasedProgress`…`CancelSpaceSwap` 5520–5636; `PaintBandOrSwipe`/`SwapBandModel` 10802–10844; `kSpaceSwapTimer` 11365–11376 | 200 | `BandModel leaving_band_`, `swap_reveal_`, `swap_conceal_`, `swap_forward_`, `swap_chip_from_`, `swap_start_`, `swap_source_palette_`, `ramp_start_` (1376–1396) |
| P | Gesture → command mapping: clicks and every context menu | `OnChromeClick` 7049–7496; `OnChromeMiddleClick` 7498–7513; `OnChromeContextMenu` 7752–8706; `ShowBookmarkBarMenu` 9058–9107 | 1,450 | — (reads every cache above) |
| Q | Painting | helpers 7515–7706; `DrawFolderIcon` 7710–7750; `BookmarkBarVisible`/`PaintBookmarkBar` 8944–9056; `PaintChrome` 9109–9865; `DrawSurfaceBadge` 10187–10212; `PaintSidebarBand` 10214–10800; `WM_PRINTCLIENT`/`WM_PAINT` 11400–11468 | 1,600 | — |
| R | Floating (collapsed) sidebar | `ToggleSidebar` 2574–2584; 8708–8942 incl. `FloatingSidebarProc` | 250 | `floating_layout_`, `floating_sidebar_`, `floating_offset_y_`, `floating_*_tick_` (1590–1594) |
| S | Misc | `OpenFeedback` 1697–1716; `ToggleDownloadsPopover` 2586–2599; `OnSidebarWheel` 5365–5388; `BackgroundFill` 5390 | 80 | — |

Observations on the header itself: `PhiSlotHost` (ctrl.h 105–324) is a 73-pure-virtual grab bag spanning Spaces, agent Spaces, reader, surfaces, store rows, layout prefs, theme, profiles, feedback, drag, tear-off, bookmark-bar prefs, settings and the create-Space form. The class has three `public:` sections (361, 1454, and the `friend class PhiMenuBar` escape hatch at 650).

### 1.2 `PhiCoordinator` (9,199 lines; implements 6 interfaces: `PhiFrameworkDelegate`, `PhiControlDelegate`, `PhiSlotHost`, `PhiSettingsHost`, `PhiTranscriptHost`, `PhiFeedbackHost` — coord.h 50–55)

| # | Cluster | Where (coord:) | ~Lines | State (coord.h:) |
|---|---|---|---|---|
| 1 | Bootstrap, message-only window, hooks | ctor/dtor 122–136; `OnFrameworkReady` 175–291; `MsgWndProc` 294–377; `g_coordinator` 47 | 200 | `framework_`, `control_server_`, `msg_hwnd_`, `keyboard_hook_`, `mouse_hook_` (426–439); all `*_path_` |
| 2 | Control-socket marshaling & deferred replies (cross-thread) | `HandleControlCommand` 379–420; `OnProfileCreated` 422–427; `Register/Complete/WaitPendingReply` 429–474; `AuthorizeAgentPeer` 600–610 | 110 | `profile_create_done_`, `created_profile_id_`, `profile_create_pending_` (653–654, 678); `pending_replies_lock_`, `pending_replies_`, `next_pending_reply_` (668–670) |
| 3 | Reader-article reply shaping | 476–598 | 120 | — |
| 4 | Agent-peer consent + grants | `AuthorizeAgentPeerOnUi` 612–669; rows 671–687; `Load/SaveAgentGrants` 715–766 | 130 | `agent_grants_`, `session_allowed_peers_`, `session_denied_peers_` (632–634) |
| 5 | Credentials (vault helper, dev vault, grants, audit, prompts, verbs) | 689–713; 6876–7527 | 700 | `vault_helper_`, `credentials_`, `credentials_dir_`, `credential_grants_`, `credential_timed_grants_`, `credential_audit_` (826–846) |
| 6 | Spaces model + persistence + push to framework | `LoadSpaces`/`SaveSpaces`/`PushSpaceMenu`/`PushRoutingTable` 768–942; `OwnerForSlot` 1183–1198; `LastActiveSpaceId` 2100–2111; `ReorderSpace` 2740–2755; `SpaceSummaries`…`SetSpaceProfile` 3276–3522; `DefaultSpaceId`/`CanDeleteSpace` 3423–3437; `RebuildSpacePalettes` 3806–3833; `KioskTargetSpaceId` 3848; `PaletteForSlot`/`SpaceThemeId`/`SetSpaceTheme` 3860–3897; `Spaces()` 4335–4348; `BrowserForSpace` 8230–8241 | 600 | `Space`/`spaces_`, `next_space_id_`, `default_space_id_`, `spaces_path_`, `palette_by_space_` (486–507, 596) |
| 7 | URL rules & routing | `Load/SaveUrlRules` 944–1007; `OnOpenLinkInSpace`/`OnRouteURLToSpace`/`OnAskSpaceForURL`/`RouteUrlToSpace` 1009–1133; rule host verbs 3684–3743; `urlRules.*` 6520–6638 | 330 | `UrlRule`/`url_rules_`, `url_rules_generation_`, `url_rules_path_` (512–523) |
| 8 | Preferences (layout, theme, prefs file, shortcuts, restore/peek pushes) — the `PhiSettingsHost` body | `Load/SaveLayoutMode` 1135–1181; `SidebarWidth` 2722–2738; `Load/SavePrefs` 2788–2894; language 2896–2932; saturation/new-tab 2934–2963; shortcuts 2965–3072; restore/full-url/previews/peek/pin-scope 3162–3274; developer/settings pages 3568–3591; theme 3745–3804, 3899–3926; layout mode 4262–4277; bookmark bar 4190–4221 | 650 | `sidebar_width_`, `layout_mode_`, `theme_id_`, `appearance_`, `palette_`, `saturation_`, `new_tab_behavior_`, `always_show_full_url_`, `show_tab_previews_`, `peek_*`, `restore_last_session_`, `always_show_bookmark_bar_`, `bookmark_bar_on_new_tab_`, `shortcut_overrides_`, `developer_mode_`, `prefs_path_` (479, 586–610, 629, 680–691) |
| 9 | Stored rows (tab store) + the sidebar mutator (`Apply`, transition table) | `LiveUrl`/`CaptureFavicon` 1200–1245; `StoredRowView` 1247–1280; row queries 1282–1337; `LiveTab` 1353–1382; `StoreTabsById` 1384–1462; `OpenCell` 1464–1505; `ApplyMove` 1507–1592; `Apply` 1594–1812; `UpgradeFavicon` 1814–1837; `StoreTabInternal`/`LiveSplitLayout`/`StoreSplitInternal` 1845–2028; `RemoveStoredRow`…`SetStoredRowUrl` 2030–2082; `ApplyLegacyTagAliases` 2373–2407; `SetStoredRowLayout`…`MoveStoredRowToSpace` 2610–2720; `RequestBookmarkActiveTab` 2757–2786; `RequestTabToSpace` 3099–3160; `ConvertStoredRow` 4060–4146; `RequestDropTabOnWindow` 4148–4188 | 1,100 | `tab_store_` (701) |
| 10 | Surfaces (Peek/Reader) + reader-bridge glue + click-outside hook | 2153–2231, 2239–2371, 2409–2535; `MouseHookProc`/`DismissPeeksForShell` 7695–7739 | 450 | `surfaces_`, `pending_reader_expands_`, `reader_` (475–478, 690) |
| 11 | Lazy Space restore (ghosts) | 2113–2151; `NoteSpaceWindow` 2537–2548; `OnSessionRestoreSettled` 2574–2587; `lazyRestore.*` verbs 8525–8593; half of `SwitchSlotToSpace` 8175–8213 | 150 | `ghost_spaces_`, `last_restore_receipt_` (617–620) |
| 12 | Agent operating mask relay | 2084–2098; 2589–2608; callbacks 214–228 | 40 | `agent_drives_` (613) |
| 13 | Analytics ring | 2550–2572 | 25 | `analytics_events_` (375–381) |
| 14 | Profiles (async create, rename/delete, engines, downloads dir) | 3524–3566; 3593–3670; `TryFinishProfileSpace` 4279–4333; `RequestCreateSpace` 4516–4569; `RequestCreateProfileForSpaceForm` 4571–4598 | 260 | `pending_profile_slot_`, `pending_profile_for_form_`, `known_profiles_before_`, `pending_profile_ticks_` (729–735) |
| 15 | Feedback, account/auth, settings-window opening | 3928–4020; 4223–4260 | 150 | `auth_`, `test_feedback_user_id_`, `test_feedback_email_` (829–838) |
| 16 | Slot/window registry + framework-delegate dispatch | `FocusWindowForBrowser` 3074–3097; `OpenUrlInMainWindow` 3835–3846; `SlotAtScreenPoint`/tear-off 4022–4058; `RequestSwitchSpace` 4506–4514; `RequestCreateSpaceWithOptions` 4600–4646; `ControllerForSlot`/`ResolveBrowser` 4648–4662; `OnBrowserWindowCreated` 7529–7692; `ShellForFocus` 7741–7753; `OnExtensionsChanged`…`OnRefreshNewTab` 7881–8028; `OnImportCompleted` 7922–7992; `OnTabsChanged` 8030–8060; `OnBrowserWindowClosing` 8062–8149; `SwitchSlotToSpace` 8151–8228; `FocusedUserController` 8272–8287 | 700 | `windows_`, `kiosks_`, `browser_to_slot_` (469–481); `pending_slot_`, `pending_space_`, `pending_open_url_`, `pending_open_urls_`, `tear_off_drop_*` (714–726) |
| 17 | Keyboard accelerators (thread hook) | `kRebindableShortcuts` 66–119; `EffectiveChord`/`MatchRebindableChord` 2965–2990; `KeyboardHookProc` 7756–7879 | 200 | `alt_solo_` (448) |
| 18 | Agent Spaces, shadow windows, overlay, transcript, ownership | 4350–4504; `HandleAgentSpaceVerb` 6020–6641; `CloseAgentSpace` 6643–6664; shadows 6666–6852; `SweepAgentSpaces` 6854–6874; `PushOwnershipChanged` 7069–7082 | 1,050 | `agent_spaces_`, `watched_agent_task_`, `pending_agent_task_`, `last_agent_window_id_`, `ShadowWindow`/`shadow_windows_`, `pending_shadow_task_` (739–771) |
| 19 | Control-verb dispatch: client-UI dialect + skill dialect | `ExecuteControlOnUi` 4664–6018 (1,355 lines, one if/else chain of ~60 verbs); `ResolveScopeBrowser`/`DialectOwner` 8243–8319; `HandleSkillDialectVerb` 8321–9197 (875 lines) | 2,300 | — |

### 1.3 `PhiSettingsWindow` (3,772 lines)

| Cluster | Where (settings:) | ~Lines |
|---|---|---|
| Statics & helpers (default-browser probe, colour math, `PickFolder`, `SplitTabs`, `g_instance` 161) | 33–296 | 265 |
| Singleton lifecycle (`Open`/`Handle`/`Refresh`/`CurrentSection`/`ShowSection`/`ShowSpacePicker`, ctor/dtor) | 298–510 | 210 |
| Layout DSL (`BeginSection`…`AddToggleRow`) | 512–590, 872–925 | 130 |
| Pane builders (General 594–871, Spaces 926–1300, Navigation 1301–1355, URL-rule codec 1356–1530, UrlRules 1532–1649, Profiles 1650–1786, ProfileDetail 1787–1929, Account 1930–2039, AI 2040–2088, Shortcuts 2089–2266, Developer 2267–2357), `Rebuild` 2359–2433 | 594–2433 | 1,840 |
| Paint | 2435–3102 | 670 |
| Action dispatch (`Apply`, string-keyed) + `OnClick` | 3104–3536 | 430 |
| `WndProc` | 3539–3770 | 230 |

It is a self-contained immediate-mode UI (rebuild item list → paint → hit-test from the same list), which is fine. Its problems are all at the seam: the 67-pure-virtual `PhiSettingsHost` with tab-separated string rows (`phi_settings_window.h` 61–64, 81–91, 100, 107, 116–121), and being a static singleton the model layer pokes directly (§2).

---

## 2. Coupling map

### 2.1 Who reaches into whom

```
                 g_coordinator (coord:47)          PhiSettingsWindow::g_instance (settings:161)
                       ▲ hooks                              ▲ 36 static Refresh() calls from coordinator setters
                       │                                    │ + CurrentSection/ShowSection/Handle/ShowSpacePicker
PhiFramework ──▶ PhiCoordinator ──owns unique_ptr──▶ PhiWindowController ◀── friend PhiMenuBar (17 private members)
   (chrome.dll)   │  ▲ PhiSlotHost* host_ (73 virtuals)        │  ▲
                  │  │                                          │  │ raw PhiWindowController* held by 13 satellites
                  │  └──── PhiSettingsHost / PhiTranscriptHost / PhiFeedbackHost / PhiControlDelegate
                  │
                  ├─ PhiSurfaceRegistry (owns PhiPeekPanel/PhiReaderPanel; each holds raw opener_/origin_ controller AND coordinator_)
                  ├─ PhiAgentSpaces, PhiGhostSpaces, PhiTabStore, PhiAgentDriveRegistry, PhiReaderBridge(this), PhiAuth, PhiVaultHelper, PhiCredentialVault, PhiControlServer
                  └─ statics: PhiTelemetry::Get(), PhiFeedbackOutbox::Get(), PhiTranscriptPanel::*, PhiAgentOverlay::*, PhiExtensionsPopover::RefreshOpen, PhiFeedbackWindow::*, PhiSetPopupPalette(&palette_) (coord:125)
```

Concrete edges, with evidence:

1. **Coordinator → controller (public surface)**: `windows_` iterated in 69 `for (... : windows_)` loops in coord.cc; ~30 controller methods called (`RefreshTabs`, `ScheduleRefresh`, `ShowSpace`, `HasSpace`, `AttachSpaceBrowser`, `ForgetBrowser`, `RequestClose`, `CloseSpaceBrowser`, `BrowserIdForSpace`, `SpaceForBrowser`, `active_space`, `active_browser`, `shell_hwnd`, `LayoutContents`, `ApplyPalette`, `ApplyLayoutMode`, `RebuildMenuBar`, `OpenFeedback`, `OpenOrFocusPinned`, `ShowsTab`, `NoteTabDistillable`, `OnTargetURLChanged`, `OnOmniboxResults`, `OnPlaceholderModeChanged`, `OnContentFullscreenChanged`, `ResetNativeNtp`, `RefreshFieldExtensions`, `SetAgentVisible`, `OnSpaceFormProfileCreated`, `content_rect`/`url_field_rect`/`reader_button_rect`/`LayoutRowsJson`/`menu_bar()`…).
2. **Controller → coordinator**: only through `host_` (`PhiSlotHost`), plus the `framework_` pointer it also drives directly (index-addressed ops, see §5 D6).
3. **`PhiMenuBar` is a `friend`** (ctrl.h 650) and touches private state directly: `host_` ×25, `framework_` ×8, `tabs_` ×6, `window_id_` ×4, `url_field_` ×4, `active_browser_` ×2, `nav_url_` ×2, `active_space_`, `can_go_*`, `nav_loading_`, and calls the private `RebuildTabs()` (`phi_menu_bar.cc:465`, `:747`) from inside `WM_INITMENUPOPUP` and `RebuildNavState()` ×3. `OnCommand` recomputes the slot→browser fallback itself (`phi_menu_bar.cc:772–774`).
4. **Coordinator reads controller *paint caches* as if they were the model**: `WindowShowingTab` (coord:2224) → `ShowsTab` (ctrl:5047) walks `tabs_`, which by design excludes tabs hidden in pinned/bookmark cells (`owner_of`). Used by `PanelizeReaderSurface` (coord:2273) and `OnReaderOfferabilityChanged` (coord:2489). `PhiMenuBar::OnCommand(kCmdCloseTab)` walks `controller_->tabs_` for the active row (`phi_menu_bar.cc:897–904`). `OpenFeedback` (ctrl:1705) does the same.
5. **Satellites holding raw `PhiWindowController*`** (grep, all `RAW_PTR_EXCLUSION`): owned/unique_ptr and safe — `PhiTabPreview`, `PhiBookmarkManager`, `PhiNewTabPage`, `PhiCreateSpacePanel`, `PhiMenuBar`. Process-wide singletons with `static X* instance` — `PhiAgentOverlay` (`phi_agent_overlay.cc:62–66`, `:805–809`), `PhiGroupOverview` (`phi_group_overview.cc:82`, `:373–378`), `PhiTabSwitcher` (`phi_tab_switcher.cc:285–310`), `PhiSearchTabsPanel` (`phi_search_tabs_panel.cc:616–621`), `PhiSplitDropOverlay` (`phi_split_drop.cc:401`, `:500`), plus modal-loop locals in `phi_space_theme_panel.cc:46/472` and `phi_folder_icon_picker.cc:39/252`. `PhiPeekPanel::opener_` / `PhiReaderPanel::origin_` (`phi_peek_panel.h:137`, `phi_reader_panel.h:104`) are dereferenced without an `IsWindow`-style guard in `Reposition`/`RefreshState` (`phi_reader_panel.cc:148,162,183,217`; `phi_peek_panel.cc:265`).
6. **Static/global state**: `g_coordinator` (coord:47) for the two thread hooks; `g_edit_base_proc` (ctrl:300); `BrowserBaseProcs()` NoDestructor map keyed by HWND (ctrl:309); `PhiLogoBgra256` NoDestructor (ctrl:383); `static HICON logo` inside `PaintChrome` (ctrl:9210); `PhiSetPopupPalette(&palette_)` hands every popup window a pointer to a coordinator member (coord:125); `g_search_edit_base_proc` (`phi_search_tabs_panel.cc:43`); `g_instance` (settings:161); `g_window` (`phi_feedback_window.cc:88`).
7. **Model → view static dependencies**: 36 `PhiSettingsWindow::Refresh()` calls scattered through coordinator setters (e.g. coord:1580, 2056, 2068, 2080, 2629, 2647, 2667, 2718, 2931, 2962, 3173…); `PhiTranscriptPanel::RefreshOpen/CloseFor` (coord:6315, 6337, 6376, 6650, 8121); `PhiExtensionsPopover::RefreshOpen` (coord:7882); `PhiAgentOverlay::Show/Hide/Reposition` (coord:4398–4424; ctrl:2657).
8. **Identity conflation (slot id == first browser id)**: `windows_` keyed by the original browser id (coord:7610, 7687); `browser_to_slot_` (coord:481); the expression `active_browser_ ? active_browser_ : window_id_` appears 32× in ctrl.cc and again in `phi_menu_bar.cc:773`; `ResolveBrowser` (coord:4656–4662) and `ControllerForSlot` (4648) exist to undo the conflation; `ShellWndProc` drives Chromium activation with `window_id_` rather than the active browser (ctrl:10883, 10911); `OpenUrlInMainWindow` opens a tab on `main->window_id()` (coord:3842) — the trap the coordinator itself documents at coord:4713–4717 for `agentSpace.newWindow`.
9. **Strip re-parsing**: 24 `ListTabsJson` parse sites (17 coord, 7 ctrl) across at least seven ad-hoc readers: `ReadStrip` (ctrl:2268), `IndexOfTagged` (6878), `SplitIdForTab` (4777), `ApplySplitLayout` (4802), `TabUrlAtIndex` (7685), the lambda in `OpenStoredSplit` (6816); `LiveTab` (coord:1353), `TabRowsOf` (2240), `ActiveTabId` (2468), `CaptureFavicon` (1221), `StoreTabInternal` (1854), `LiveSplitLayout` (1912), `RemoveStoredRow` (2037), `ConvertStoredRow` (4092), `RequestTabToSpace` (3115), `SetSpaceProfile` (3467), `tabs_of` (8330). `phi_sidebar_model.h` already defines the `LiveTab` snapshot type used by `BuildSidebar`; none of these use it.
10. **Refresh fan-out**: `RefreshTabs()` (= `RebuildTabs` + `InvalidateRect` + synchronous `UpdateWindow`, ctrl:5395–5401) is called from 88 sites in five files (46 coord, 42 ctrl, 13 menu bar, 1 switcher, 1 overview). Almost every coordinator mutator ends with `for (windows_) controller->RefreshTabs()` — including hidden agent windows — and several gestures run it two or three times (`SidebarMove` → `ApplyMove` → `RemoveStoredRow` (refresh all, coord:2053) → `MoveTabBefore` → `refresh()` (1555); `MoveStoredRowToSpace` → `RemoveStoredRow` (refresh all) → refresh all again (2715)).

### 2.2 Hidden shared state and lifetime hazards

| Hazard | Evidence |
|---|---|
| **Singletons outlive the controller they point at.** `~PhiWindowController` (ctrl:1400–1450) takes down only `PhiSplitDropOverlay` and `PhiDragImage`. `PhiAgentOverlay::Overlay::instance` keeps `controller_`; its child HWNDs die with the shell, but `PhiAgentOverlay::Reposition()` — called from **every** controller's `LayoutContents` (ctrl:2657) — dereferences `controller_->shell_hwnd()` first (`phi_agent_overlay.cc:124–127`). The agent-window branch of `OnBrowserWindowClosing` erases the controller without `PhiAgentOverlay::Hide()` (coord:8118–8131; only `RequestAgentFinish` hides it, 4453). Same shape for `PhiGroupOverview::Overview::instance` (`IsOpen()` is consulted in `WM_MOUSEACTIVATE` of *other* shells, ctrl:10878) and `PhiTabSwitcher::Hud::instance` (`Cycle(self)` reuses a stale `Hud` bound to a dead controller, `phi_tab_switcher.cc:296–303`). |
| **Peek/Reader panels hold a raw origin controller.** Ordering in `OnBrowserWindowClosing` (surfaces detached at coord:8093–8102 before `windows_.erase` at 8145–8147) makes it safe today; the invariant is implicit and enforced by nothing. |
| **`GWLP_USERDATA` is never cleared.** `ShellWndProc` has no `WM_DESTROY`/`WM_NCDESTROY` case (10846–11487; the only `WM_DESTROY` in the file is the floating sidebar's, 8934). `~PhiWindowController` calls `DestroyWindow(shell_hwnd_)` (1412) from the destructor body, so `WM_ACTIVATE`/`WM_CAPTURECHANGED`/`WM_KILLFOCUS`/child `WM_PAINT`s run `ComputeLayout()`/`AbortDrag()` on an object mid-destruction. It works because the `unique_ptr` members are destroyed *after* the body; it is one member-reorder away from a UAF. |
| **Single-flight "pending" globals.** `pending_slot_`/`pending_space_`/`pending_open_url_`/`pending_open_urls_` are process-wide (coord.h 714–721) and written by three independent flows: URL-rule routing (coord:1129–1132), user/verb switch (8185–8186, 8220–8221), re-profiling respawn (3513–3515). Whichever `OnBrowserWindowCreated` arrives first consumes them (7639–7674). A second cold switch while the first profile is still loading mis-attaches the browser and then records the wrong `SetWindowSpace` binding (ctrl:5453). |
| **Timer id 1 on `msg_hwnd_` is shared** by "New Space with new profile" and the create-form profile flow (coord:4552, 4596), with one `pending_profile_slot_`/`known_profiles_before_` set; a Settings "New Profile" during either poll (3616) is claimed as the pending one (4294–4331). |
| **Cross-thread state without the lock.** `profile_create_pending_` is a `volatile bool` written on both threads (coord:390–396 I/O; 4956–4966 UI); `created_profile_id_` is written on the UI thread (423) and read on the I/O thread (397–401) ordered only by the event. Benign today, brittle. |
| **`PhiSetPopupPalette(&palette_)`** (coord:125) publishes the address of a coordinator member to every secondary window; `palette_` is reassigned (not re-pointed) so this holds, but it is invisible coupling. |
| **`BrowserBaseProcs()`** subclasses Chromium's HWND process-wide (ctrl:2104–2109) and unhooks on `WM_NCDESTROY` (328–335) — correct, but it is the one place client code lives inside a Chromium window's message chain. |

---

## 3. Re-entrancy and ordering hazards

### 3.1 Framework callbacks arriving mid-mutation

The contract (`phi_framework_api.h:32–33, 66–70`) is synchronous delivery on the UI thread; `OnTabsChanged` fires from *inside* the tab-strip change (`phi_framework_impl.cc:3349–3358`; coordinator comment coord:144–149). The client handles this correctly in exactly one place — `OnTabsChanged` → `ScheduleRefresh` (posted `kMsgPhiRebuildRows`, ctrl:5403–5414) and a posted `WM_PHI_READER_SWEEP` (coord:8042–8058). Elsewhere the same callbacks trigger synchronous work:

- **`OnBrowserWindowCreated` runs inside `Browser::Create`** (`phi_framework_impl.cc:3331–3346`; api 59–60, 542–544). From there the client: constructs the shell and calls `RefreshTabs()` before the controller is even registered in `windows_` (coord:7607–7611, 7683–7688 — so `OwnerForSlot(window_id_)` fails during the first rebuild and pins/bookmarks/palette are wrong for one frame); reads `ListTabsJson` of the half-built browser (ctrl:6366 via 5462); calls back into the framework with `SetWindowSpace` (ctrl:1391, 5453), `OpenTabBypassingSpaceRouting` on the browser being created (coord:7652, 7661), and `SetSpaceMenu`/`SetSpaceRoutingTable` (7634, 7667, 7690). The box tests prove the common paths work; the design has no rule that says which of these are allowed.
- **`OnBrowserWindowClosing`** (a "will close" notification, api 62–63) calls *into* the framework: `ActivateTabById` → `ActivateTab` (coord:8073), and `ForgetBrowser` → `ShowSpace` → `LayoutContents`/`RefreshTabs`/`FocusActiveBrowser` → `FocusBrowserWindow` (ctrl:5708–5727 → 5645–5684).
- **`OnAgentDrove/Observed/Stopped`** → `agent_drives_` changed-callback → synchronous `RefreshTabs()` on every window (coord:214–218).
- **`OnAskSpaceForURL`** runs a modal `PhiPromptChoose` (coord:1066) *inside a navigation-throttle callback*; anything can happen under that loop (§3.3).

### 3.2 Modal loops holding references into rebuildable caches (crash)

`OnChromeContextMenu` iterates `tabs_`/`pinned_`/`bookmarks_`/`group_headers_` by reference and runs `::TrackPopupMenu` (a nested message loop) while the reference is live, then reads it afterwards:

- tab row: loop at ctrl:7831, menu at 8051–8054, then `row.key/row.title/row.index/row.b_index/row.id` at 8058–8184;
- pinned cell: menu at 8230, values copied *after* it at 8234–8238;
- bookmark: menu at 8370, copies at 8374–8381;
- group header: menu at 7800, copies at 7804–7805.

Any `kMsgPhiRebuildRows` posted before or during the loop (a page finishing a load fires `OnTabsChanged` for a title/favicon update, api 66–70 → `ScheduleRefresh`) is dispatched by the menu's loop → `RebuildTabs()` → `tabs_.clear()` (6354), `pinned_.clear()`/`bookmarks_.clear()` (6180–6181), `group_headers_.clear()` (6355) → dangling reference. Same pattern with `PhiPromptText` (7813, 8072, 8260, 8272, 8396, 8440, 8466, 8633) and `PhiPickFolderIcon` (8460), which run their own `GetMessage` loops (`phi_prompt.cc:169–175`, `phi_folder_icon_picker.cc:252`). The bookmark-folder click path shows the authors know the rule — `const std::string guid = bookmark.guid;` before `RefreshTabs()` (7255–7259) — it just is not applied to the menus.

### 3.3 Cross-thread `SendMessage` into modal loops (crash)

`HandleControlCommand` marshals every pipe verb with `SendMessageW(msg_hwnd_)` from the I/O thread (coord:379–389). A cross-thread `SendMessage` is serviced whenever the UI thread pumps — including inside `TrackPopupMenu`, `PhiPrompt*`, `MessageBox`, the theme panel (`phi_space_theme_panel.cc:472`) and the icon picker. So `agentSpace.close` → `RequestClose()` (4732–4737), `agentSpace.spaces.delete` → `DeleteSpace` → `CloseSpaceBrowser` (3402–3404), or `spaces.switch` → `ShowSpace` → `RefreshTabs` can execute *under* an `OnChromeContextMenu`/`OnChromeClick` frame of the very controller they mutate or destroy (`windows_.erase`, 8145–8147). The client is written on the assumption that `CloseBrowserWindow` reaches `OnBrowserWindowClosing` synchronously (ctrl:5735–5737; coord:4736 "the call below may erase `it`"), so the frame returns into freed memory. The test rigs drive menus and pipe verbs concurrently (memory notes), so this is reachable in practice. There is no modal-depth guard and no deferred-destroy.

### 3.4 Structural work in paint / posted-turn discipline not applied consistently

The sidebar doc's rule ("nothing structural in `WM_PAINT`; rebuild on a posted turn") holds for `WM_PAINT` of the shell (ctrl:11412–11424) but not elsewhere:

- `PaintChrome` moves a child window (`PositionUrlField`, 9389) and creates/moves/commits a layered popup (`SyncHoverCardPopup`, 9860) from inside the paint.
- `PaintFloatingSidebar` runs `RebuildTabs()` inside `WM_PAINT` (8825) and then `ScheduleRefresh()` (8840) — every 100 ms tick while the panel is up (8763, 8786).
- `RebuildTabs()` is called synchronously from input handlers (`OnChromeClick` 7283, `OnChromeContextMenu` 7756, `WM_LBUTTONDBLCLK` 11274, the floating panel's `forward` 8863) and from `WM_INITMENUPOPUP` via the friend (`phi_menu_bar.cc:465, 747`).
- `ComputeLayout()` is a side-effecting getter: it writes `dpi_` and `sidebar_dip_` and creates fonts + `SendMessage(WM_SETFONT)` (662–672, 489–491), and is called from 57 sites — six times per `WM_MOUSEMOVE` (11109, 11117, 4436, 4475, 4501, 4509), each re-querying `host_->SidebarWidth()`, `BookmarkBarVisible()` (→ two host calls) and `ActiveTabReaderOfferable()` (→ host + reader bridge).
- Two refresh disciplines coexist: 12 `ScheduleRefresh` sites vs 88 synchronous `RefreshTabs` sites; the coordinator never uses the posted path for its own mutations.

### 3.5 Other ordering notes

- `MouseHookProc` destroys browsers from inside a `WH_MOUSE` hook, before the click is dispatched to the target (coord:7695–7737). Chromium window teardown inside a hook callback is legal but fragile.
- `RequestClose` (ctrl:5729–5753) is the only place that snapshots locals because "this may be destroyed"; `CloseSpaceBrowser` (5686–5706) and `ForgetBrowser` do not, and both are reachable from `DeleteSpace` while iterating `windows_` (coord:3402–3404) — the loop holds an iterator into the map that `OnBrowserWindowClosing` erases from (8145–8147). Today the retreat logic (3383–3399) guarantees the slot survives; again an invariant enforced by nothing.

---

## 4. Proposed decomposition

### 4.1 Principles (extending the sidebar doc's §2 to the whole client)

1. **One identity type per axis.** `SlotId` (client shell) and `BrowserId` (framework window id) become distinct strong types; there is no "slot 0 means first window" convention outside the control dispatcher, and no `active ? active : window_id_` expression anywhere.
2. **Delegate callbacks record, post, return.** No framework call, no controller construction beyond adoption, no `RefreshTabs`, no modal UI, no teardown inside a `PhiFrameworkDelegate` method. Every consequence runs on a posted turn through one `Defer(fn)` on the message-only window.
3. **Mutations go through commands; views are invalidated, never refreshed.** Services call `Invalidate(SlotId|All, Reason)`; the shell rebuilds once per turn. `RefreshTabs()` (synchronous) survives only as `FlushRebuildForTest()`.
4. **No reference survives a modal loop.** Context menus are built as data (`MenuSpec`) from a snapshot and return a `MenuAction` that is re-resolved by key against fresh state.
5. **Every top-level HWND owned by the client has one C++ owner, and that owner is torn down by the shell that created it.** Process-wide singletons that point at a shell (`Overlay`, `Overview`, `Hud`, search panel) become owned components of `PhiShellWindow` or hold a `SlotId` and look the shell up.
6. **Layout is a value.** `ComputeLayout` becomes a pure function of `(client rect, dpi, mode, flags, counts)` in `phi_shell_layout.{h,cc}` (host-testable like the sidebar layout), cached per turn.

### 4.2 The window controller becomes `PhiShellWindow` (~1.5k lines) owning components

```
PhiShellWindow                          // HWND, WndProc, DPI, fonts (PhiChromeFonts), Layout cache, focus routing,
  ├─ PhiCaptionBand                     //   component dispatch: OnLayout / Paint / HitTest / Mouse / Timer / Command
  │    └─ PhiMenuBar (no friend; talks to ShellServices + reads the SidebarTree)          [C]
  ├─ PhiBrowserHost                     // content-child adoption, per-Space browser HWNDs, placeholder, fullscreen,
  │                                     //   corner caps, status bubble, z-order (WM_PHI_RAISE_CAPS)               [E, G]
  ├─ PhiOmniboxSession                  // EDIT + popup + results + nav state; the NTP's EDIT attaches here        [F]
  ├─ PhiSpacesStrip                     // chips/viewport/hover card/add-space, create-Space form host,
  │                                     //   swap+ramp animation (owns the BandModel snapshot)                     [chips of H, J-card, K, O]
  ├─ PhiSidebarView                     // SidebarInputs → BuildSidebar → LayoutSidebar → rows; paint; hit-test;
  │    │                                //   hover, scroll, expanded set, MRU; emits SidebarCommand / ShellGesture  [H, J-hover, Q-band, P-sidebar]
  │    └─ PhiSidebarDragSession         // created on ArmDrag, destroyed on commit/cancel; keys only; slides,
  │                                     //   drag image, drop resolution, split-drop, tear-off                     [I]
  ├─ PhiNativePagesHost                 // bookmark manager + NTP mount by URL                                      [M]
  ├─ PhiFloatingSidebar                 // a second viewport over PhiSidebarView's rows; no rebuild in paint       [R]
  └─ phi_shell_menus.{h,cc}             // pure MenuSpec builders + MenuAction variant for every context menu      [P]
```

Cluster → component mapping is in the bracketed letters (from §1.1). Cluster L (split helpers that call the framework) moves out of the window entirely into `PhiSidebarActions` (`FormSplit` already exists as a command); cluster N (reader affordance) becomes a small `ReaderAffordance` value the omnibox session reads from `SidebarTree` + `SurfaceRegistry`.

Invariants each unit enforces:

- `PhiShellWindow`: the only writer of `GWLP_USERDATA` (set on `WM_NCCREATE`, cleared on `WM_NCDESTROY`); the only `DestroyWindow` caller for its HWND tree; runs `ModalScope` counting so a `RequestClose` arriving while `modal_depth_ > 0` is deferred to the next turn; components receive `OnDestroy()` before the HWND goes.
- `PhiBrowserHost`: owns `BrowserId → HWND` for the slot; the shell's "active browser" is a `BrowserId` the host exposes — no `window_id_` fallback. `SetParent`/subclassing happen here only.
- `PhiSidebarView`: `rows_` are written in one function, on the posted rebuild turn, never from paint, input, or menus; hit-tests return `SidebarKey`, never indices; `WindowShowingTab`-style queries answer from the tree (`owner_of` included), not from `rows_`.
- `PhiSidebarDragSession`: exists only between `ArmDrag` and commit/cancel; `AbortDrag` is the destructor; the shell's dtor destroys it first (so `PhiSplitDropOverlay`/`PhiDragImage` are torn down by ownership, not by two ad-hoc calls).
- `PhiOmniboxSession`: the single owner of `url_field_`, `omnibox_popup_`, the NTP field subclass and `nav_*`; palette changes repaint through it.
- `phi_shell_menus`: functions of `(snapshot rows, tree, prefs) → MenuSpec`; the shell shows a spec through one `RunPopupMenu(spec)` that snapshots nothing else and returns an action applied by key.

### 4.3 The coordinator becomes a composition root over services

```
PhiCoordinator (composition root; ~300 lines)
  ├─ PhiFrameworkDispatcher : PhiFrameworkDelegate      // record + post; owns Defer(); the only place that sees raw callbacks
  ├─ PhiWindowRegistry                                   // SlotId/BrowserId maps, kiosks, pending attaches as a token queue
  ├─ PhiSpaceManager                                     // spaces_, default role, per-Space palettes, persistence, SetSpaceMenu   [6]
  ├─ PhiUrlRouter                                        // rules, routing decisions, ask-prompt (deferred), SetSpaceRoutingTable [7]
  ├─ PhiPreferences (+ observer list)                    // layout/theme/prefs/shortcuts; pushes restore/peek/language           [8, 17 tables]
  ├─ PhiSidebarActions (already designed)                // Apply/ApplyMove/StoreTabsById/OpenCell/Convert/MoveToSpace + tab_store_ [9]
  ├─ PhiSurfaceCoordinator                               // registry, reader bridge glue, sweep, peek dismissal                   [10]
  ├─ PhiSessionRestore                                   // ghosts, cold-start plan, receipts                                    [11]
  ├─ PhiAgentSpaceManager                                // agent/shadow windows, overlay, transcript, ownership, drive mask      [12, 18]
  ├─ PhiCredentialService                                // vault helper, dev vault, grants, audit, prompts                       [5]
  ├─ PhiAgentPeerConsent                                 // [4]
  ├─ PhiProfileService                                   // async create with request tokens, rename/delete, engines, downloads   [14]
  ├─ PhiAccountAndFeedback                               // [15]
  ├─ PhiKeyboardAccelerators                             // hooks + tables, posts commands to SlotId                             [17]
  └─ PhiControlDispatcher : PhiControlDelegate           // verb → handler tables over the services; three dialects as three tables [19]
```

`PhiSlotHost` (73 virtuals) is replaced by a `ShellServices` struct of five narrow interfaces the shell receives at construction: `SidebarHost` (stored items, rows, surfaces, `Apply`), `SpacesHost` (list, switch, create, theme, default role), `ChromePrefsHost` (layout, palette, previews, bookmark-bar flags, sidebar width), `AgentHost` (pips, watch, reclaim/release), `WindowHost` (slot-at-point, cross-window drop, tear-off, feedback, settings). `PhiSettingsHost` splits along the same seams and its `"a\tb\tc"` rows become structs.

Data flow after the split:

```
framework ─▶ PhiFrameworkDispatcher ─(record)─▶ WindowRegistry / SurfaceCoordinator / AgentSpaceManager
                                     └(post)──▶ Invalidate(SlotId, reason) ─▶ PhiShellWindow rebuild turn
                                                                                 └─ PhiSidebarView pulls SlotSnapshot
                                                                                    {strip, groups, splits, stored items,
                                                                                     surfaces, policy} from SidebarHost
gesture ─▶ SidebarCommand / ShellCommand ─▶ PhiSidebarActions / services ─▶ framework + store
                                                                          └─ (framework fires OnTabsChanged) ─▶ posted rebuild
pipe (I/O thread) ─▶ PhiControlDispatcher via Defer() (posted, not SendMessage) ─▶ services; reply through PendingReply
```

Invariants per service:

- `PhiFrameworkDispatcher`: every override is ≤ 10 lines and ends in a post; it holds no business state. The `RestoredFromWindowId`/`CreatedWindowIsKiosk`/`CreatedWindowPeekOpener`/`CreatedWindowReaderOrigin` "read only during the callback" markers are captured into a `CreatedWindowInfo` value here and handed on.
- `PhiWindowRegistry`: `windows_` keyed by `SlotId` allocated by the registry (not the first browser id); `browser_to_slot_` is the only map; pending attaches are `{token, SlotId, space_id, urls}` entries matched by the profile/space the arriving browser reports, so two cold switches cannot cross.
- `PhiSpaceManager`: the only writer of `spaces_`/`default_space_id_`; publishes `SpacesChanged` to observers (settings window, strips, menu bars) instead of calling `PhiSettingsWindow::Refresh()`; `PushSpaceMenu`/`PushRoutingTable` run once per turn, coalesced.
- `PhiSidebarActions`: unchanged contract; gains the remaining framework-index paths (D6) rewritten by id.
- `PhiSurfaceCoordinator`: owns the deferral messages (`READER_SWEEP/EXPAND/RETIRE`); panels hold `SlotId`/`BrowserId`, not controller pointers, and resolve through the registry.
- `PhiControlDispatcher`: internal callers never dispatch verbs by string (`RequestSwitchSpace`, `RequestCreateSpace`, `TryFinishProfileSpace`, `CreateSpaceNamed`, `RequestCreateSpaceWithOptions` currently do — coord:3681, 4320, 4513, 4561, 4620); `spaces.create` becomes `PhiSpaceManager::Create(spec)` and the verb calls it.
- `PhiPreferences`: one `PhiJsonFile` helper with atomic write and coalesced save replaces the seven copy-pasted `CreateFileW`/`ReadFile` loops (coord:715–766, 768–869, 944–1007, 1135–1181, 2788–2894, 3745–3794, 7190–7252).

### 4.4 Settings window

Keep the immediate-mode design; change the seam: typed `PhiSettingsHost` (structs, not `\t` rows), observer-driven `Refresh` (subscribe to `PhiPreferences`/`PhiSpaceManager` change events), and build the pane's item list on `Rebuild()` triggered by invalidation rather than inside `WM_PAINT` (settings:3647) — today every paint re-queries the framework (`ListProfilesJson`, `ListSearchEnginesJson`, `DownloadDirectoryUtf8` through `SearchEngines`/`DownloadDirectory`, coord:3622–3670) and the registry (`ThisExeIsDefaultBrowser`, cached in `is_default_browser_`).

---

## 5. Ranked findings

### Crash / data-loss risk

**F1 — Dangling row references across modal menus (high probability).** `OnChromeContextMenu` holds `const TabRow& row` (ctrl:7831) across `::TrackPopupMenu` (8051) and reads it afterwards (8058–8184); pinned (8230 → 8234–8238), bookmark (8370 → 8374–8381) and group-header (7800 → 7804–7805) branches copy fields *after* the loop. A posted `kMsgPhiRebuildRows` (any `OnTabsChanged`, e.g. a title/favicon update of a loading page → `ScheduleRefresh` 5403–5414) is dispatched inside the menu loop and clears the vectors (6354–6355, 6180–6181). Same for `PhiPromptText`/`PhiPickFolderIcon` sites (7813, 8072, 8260, 8272, 8396, 8440, 8460, 8466). Fix: snapshot by value before the loop, or (target design) build a `MenuSpec` and apply the action by `SidebarKey`.

**F2 — Pipe verbs destroy the controller under its own stack frame.** `HandleControlCommand` uses cross-thread `SendMessage` (coord:379–389), serviced inside any UI modal loop (§3.3). `agentSpace.close` (4732–4737), `spaces.delete` (3402–3404 → ctrl:5686–5706), `spaces.switch` can erase `windows_` (8145–8147) while `OnChromeContextMenu`/`OnChromeClick`/`OnAskSpaceForURL` of that controller is on the stack. Fix: marshal with `PostMessage` + `PendingReply` (the mechanism already exists, 429–474) and add a modal-depth guard that defers `RequestClose`.

**F3 — Satellite singletons keep a dangling `PhiWindowController*`.** `PhiAgentOverlay::Overlay::instance` (`phi_agent_overlay.cc:62`) is not hidden when an agent window is destroyed (coord:8118–8131 has no `PhiAgentOverlay::Hide()`; ctrl:1400–1450 hides only split-drop and drag image) and `PhiAgentOverlay::Reposition()` derefs `controller_` from every shell's `LayoutContents` (ctrl:2657; overlay 124–127). `PhiGroupOverview::Overview::instance` (`phi_group_overview.cc:373–378`) and `PhiTabSwitcher::Hud::instance` (`phi_tab_switcher.cc:296–303`) likewise survive their shell (their popups are owner-destroyed, the objects are not). Fix: `~PhiWindowController` (or `PhiShellWindow::OnDestroy`) closes every singleton bound to it; longer term, own them.

**F4 — Shell teardown runs the window procedure on a half-destroyed object.** `~PhiWindowController` calls `DestroyWindow(shell_hwnd_)` (ctrl:1412) with `GWLP_USERDATA` still pointing at `this` and no `WM_NCDESTROY` handler (10846–11487). Safe only because member destructors run after the body. Fix: clear `USERDATA` on `WM_NCDESTROY`; destroy components before the HWND.

**F5 — Single-flight pending-attach globals mis-bind Space browsers.** `pending_slot_/pending_space_/pending_open_url_/pending_open_urls_` (coord.h 714–721) are overwritten by concurrent cold switches (coord:1129–1132, 8185–8186, 8220–8221, 3513–3515) and consumed by the first `OnBrowserWindowCreated` (7639–7674), which then records the wrong `SetWindowSpace` (ctrl:5453) → routing/profile mismatch persisted via `NoteSpaceWindow`. Fix: a queue of tokens matched by `WindowProfileJson`/requested space.

**F6 — Framework re-entered from inside delegate callbacks.** `OnBrowserWindowClosing` → `ActivateTab` (coord:8073) and `ForgetBrowser` → `ShowSpace` → `FocusBrowserWindow` (ctrl:5708–5727, 5679); `OnBrowserWindowCreated` → `OpenTabBypassingSpaceRouting` on the browser inside its own `Browser::Create` (coord:7652, 7661). Works on the box today; the fork's `CHECK_IS_TEST`/observer-notify landmines (memory notes; coord:151–154) show how thin the margin is. Fix: post.

**F7 — Browser teardown from inside a `WH_MOUSE` hook.** `MouseHookProc` → `DismissPeeksForShell` → `CloseBrowserWindow` (coord:7695–7737) before the click is dispatched; the click target may be a child of the window being destroyed. Fix: post the dismissal.

**F8 — Cross-thread fields without synchronisation.** `profile_create_pending_` (coord.h 678; coord:390–396 vs 4956–4966) and `created_profile_id_` (423 vs 397–401). Fold into the `pending_replies_` mechanism.

### Design debt

**D1 — Slot identity is the first browser's id.** `windows_` keyed by browser id (coord:7610, 7687); 32 `active_browser_ ? active_browser_ : window_id_` in ctrl.cc + `phi_menu_bar.cc:773`; `ResolveBrowser`/`ControllerForSlot` as patches; actual bugs: `ShellWndProc` activates `window_id_` instead of the active browser (ctrl:10883, 10911) so Chromium activation is driven at a dead browser once the original Space browser has closed; `OpenUrlInMainWindow` (coord:3842) opens on `window_id()`; the coordinator documents the trap itself (4713–4717).

**D2 — The coordinator and menu bar read the paint cache as the model.** `WindowShowingTab` → `ShowsTab` → `tabs_` (coord:2224; ctrl:5047) misses cell-bound tabs → `PanelizeReaderSurface` returns false for a pin-bound origin (coord:2273–2276) and `OnReaderOfferabilityChanged` never repaints (2489); `PhiMenuBar::OnCommand(kCmdCloseTab)` (`phi_menu_bar.cc:897–904`) finds no active row for a pin/bookmark-bound tab so Ctrl+W is a silent no-op there; `OpenFeedback` loses the title (ctrl:1705–1713). The tree (`sidebar_tree_.owner_of`, `active_tab`) is the model and should answer.

**D3 — Two refresh disciplines.** 12 posted `ScheduleRefresh` vs 88 synchronous `RefreshTabs` (each = strip + groups + store + favicon copy + `UpdateWindow` paint, for every window incl. hidden agent ones), 69 all-window loops in coord.cc, multiple rebuilds per gesture (coord:2053 + 1555; 2715 + 2053), and direct `RebuildTabs()` from input/paint/menu paths (ctrl:7283, 7756, 11274, 8825, 8863; `phi_menu_bar.cc:465, 747`).

**D4 — `ComputeLayout` is a side-effecting getter** (ctrl:662–672 writes `dpi_`, `sidebar_dip_`, builds fonts, `SendMessage(WM_SETFONT)` 489–491) called from 57 sites, ~6× per `WM_MOUSEMOVE`, each with 3–5 host round-trips. `PaintChrome` does structural work (`PositionUrlField` 9389, `SyncHoverCardPopup` 9860).

**D5 — Seven strip readers, 24 JSON parses** (§2.1 item 9) while `phi_sidebar_model.h` already defines `LiveTab`.

**D6 — Index-addressed framework ops on gesture paths despite id-addressed API** (`phi_framework_api.h:1024–1050`): `SetTabTag` by index in `UnbindStoredTab` (ctrl:4109), `OpenOrFocusPinned` (6793–6804), `OpenStoredSplit` (6849–6872), `StoreTabInternal` (coord:1897), `StoreSplitInternal` (2021–2022), `ConvertStoredRow` (4136), `RemoveStoredRow` (2047), `ApplyLegacyTagAliases` (2394); `CloseTab` by index in "Close Other Tabs" (ctrl:8181) and `kCmdCloseTab`; `ActivateTab`/`SplitActiveWith` by index in Duplicate/Open-as-Split (8103–8137). Each re-reads the strip between calls to survive shifting indices.

**D7 — One class, six interfaces, two 70-method hosts** (coord.h 50–55; `PhiSlotHost` 73 pure virtuals ctrl.h 105–324; `PhiSettingsHost` 67 in `phi_settings_window.h`), with stringly-typed rows (`"name\ticon\tprofile\tspace\ttheme\tsat"`, `phi_settings_window.h:61–64, 81, 100, 107, 116–121`; `PhiSlotHost::ProfileSummaries` ctrl.h 311) and `SplitTabs` duplicated (settings:282, coord:51).

**D8 — `ExecuteControlOnUi` is the internal API.** `RequestSwitchSpace`, `RequestCreateSpace`, `CreateSpaceNamed`, `TryFinishProfileSpace`, `RequestCreateSpaceWithOptions` serialize JSON to call their own string dispatcher (coord:3681, 4320, 4513, 4561, 4620); Space creation exists only inside the verb (4805–4857). The 1,355-line if/else chain (4664–6018) and the 875-line skill dialect (8321–9197) re-parse `payload_json` per branch.

**D9 — Model → view static singletons.** 36 `PhiSettingsWindow::Refresh()` calls from setters; `PhiTranscriptPanel`, `PhiExtensionsPopover`, `PhiAgentOverlay` statics (§2.1 item 7). The settings window rebuilds with framework round-trips inside `WM_PAINT` (settings:3647 → 594–2357 → coord:3622–3670).

**D10 — Persistence.** Seven hand-rolled JSON files (§4.3), synchronous writes on the UI thread on every mutation — `SaveSpaces` on chip-drag commit (coord:2751), `SavePrefs` on every toggle, `SaveCredentialGrants` on every approval prompt via `AppendCredentialAudit` (7186), `SaveUrlRules` per rule edit; no atomic write.

**D11 — `friend class PhiMenuBar`** (ctrl.h 650) reaching 17 private members and the private rebuild (§2.1 item 3).

**D12 — Registration after first render.** `OnBrowserWindowCreated` calls `RefreshTabs()` before inserting into `windows_` (coord:7607–7611, 7683–7688), so the first rebuild runs with `OwnerForSlot` failing (1183–1187) and `PaletteForSlot` falling back (3860–3868).

**D13 — Magic timer ids on `msg_hwnd_`** (1, 2, 3: coord:317, 4552, 4596, 6158, 6661, 6707, 6810, 6872, 8111, 8128 vs the named `kAgentDriveSweepTimer` 626), with id 1 shared by two flows (§2.2).

**D14 — Panels hold `PhiCoordinator*` and `PhiWindowController*` both** (`phi_peek_panel.h:137–139`, `phi_reader_panel.h:104–106`; `phi_kiosk_window.h:132`), so `PhiKioskWindow` calls `coordinator_->OnOpenLinkInSpace` — a *delegate callback* — as an API (`phi_kiosk_window.cc:806`).

### Cosmetic

**C1 — MRU filter bug**: `return row.id == id || (row.b_index >= 0 && row.id == id);` (ctrl:6760) — second clause should test `row.b_id`; pane-B tab ids drop out of `tab_mru_`.
**C2 — Stale comment** at coord:2156–2158 (the original browser *is* in `browser_to_slot_`, 7688).
**C3 — Misplaced doc comment** in `PhiSlotHost` (ctrl.h 219–231: "Retitles a stored row" sits above `SetStoredRowIcon`).
**C4 — `static HICON logo`** in `PaintChrome` (ctrl:9210) never destroyed.
**C5 — Untranslated menu strings** throughout `OnChromeContextMenu` (ctrl:7792–7797, 7887–8048, 8200–8227, 8306–8367, 8514–8590) while an i18n catalog exists (known WIP per memory).

---

## 6. Migration order (each phase leaves the client building and box-usable)

| Phase | Scope | Size |
|---|---|---|
| **P0 — hotfixes** | F1: copy `TabRow`/`ShortcutRow`/`GroupHeader` fields into locals before every `TrackPopupMenu`/`PhiPrompt*` in `OnChromeContextMenu`/`OnChromeClick`. F3: in `~PhiWindowController` close `PhiAgentOverlay` (if bound to this), `PhiGroupOverview`, `PhiTabSwitcher`, `PhiSearchTabsPanel`; add `PhiAgentOverlay::Hide()` to the agent branch of `OnBrowserWindowClosing`. F2 (minimal): switch `HandleControlCommand` to `PostMessage` + `PendingReply` and add a `modal_depth_` counter on the controller that defers `RequestClose`. F4: `WM_NCDESTROY` clears `USERDATA`. D1 quick fix: ctrl:10883/10911 and coord:3842 use the active browser. C1. | 1–2 days |
| **P1 — identity + registry** | Introduce `SlotId`/`BrowserId` strong types; extract `PhiWindowRegistry` (cluster 16's maps, kiosks, pending attaches as a token queue — fixes F5/D12/D13); rewrite the 32 fallback expressions to `browser_host.active()`. Mechanical, high-churn, low-risk. | ~1 week |
| **P2 — refresh discipline** | Replace every coordinator `for (windows_) RefreshTabs()` with `registry.Invalidate(...)` (posted); remove `RebuildTabs()` from paint/input/menu paths (menu bar reads `sidebar_tree_`); cache `Layout` per turn and make `ComputeLayout` pure; one `StripSnapshot` reader shared by both files (D3/D4/D5); D6 id-addressed ops. Ships the F6/F7 posts. | 1–2 weeks |
| **P3 — coordinator split** | `PhiFrameworkDispatcher`, `PhiPreferences` (+observers, kills the 36 `Refresh()` calls), `PhiSpaceManager`, `PhiUrlRouter`, `PhiProfileService`, `PhiSurfaceCoordinator`, `PhiAgentSpaceManager`, `PhiCredentialService`, `PhiControlDispatcher` tables (D7/D8/D9). `PhiSidebarActions` moves out with `tab_store_`. `PhiSlotHost` → `ShellServices`. Each service is a file move plus an interface; no behaviour change. | 2 weeks |
| **P4 — controller split** | Extract in order of seam cleanliness: `PhiOmniboxSession` (F), `PhiBrowserHost` (E+G), `PhiCaptionBand`+`PhiMenuBar` de-friended (C), `PhiNativePagesHost` (M), `PhiSpacesStrip` (K+O+chips), then `PhiSidebarView` + `PhiSidebarDragSession` + `phi_shell_menus` (H/I/J/P/Q). `PhiShellWindow` ends as the WndProc + component dispatch. | 2–3 weeks |
| **P5 — settings window** | Typed host structs, observer-driven refresh, rebuild off-paint. | ~1 week |
| **P6 — persistence** | `PhiJsonFile` (atomic replace, coalesced save on a posted turn) for the seven stores (D10). | 2–3 days |

Test scaffolding to add alongside P1–P2: `test/model/` gains `phi_shell_layout` and the menu-spec builders (pure); a pipe-driven lifetime test that opens a context menu (`PhiRm` right-click rig) and issues `agentSpace.close` mid-menu should fail before P0 and pass after.
