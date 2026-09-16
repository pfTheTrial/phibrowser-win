# Split view chrome (the Mac look inside the content card)

Since 2026-09-16 a split in the Windows client looks like the Mac's
`SplitPaneHostView`: two rounded pane cards on the window ground, a 1 DIP
hairline on each, a 2 DIP accent ring on the focused pane, a transparent
4 DIP divider between them, 4 DIP of ground around the cards (0 at the top
under a top address bar), snap points at 1/3, 1/2 and 2/3, no mini toolbars,
no attention dimming.

## Who draws what

Chromium's `MultiContentsView` still lays the panes out and paints the
chrome — the Windows client drives the native side-by-side through
`PhiFramework` and reads geometry back (`TabContentBoundsById`,
`OnContentBoundsChanged`). The fork restyles that chrome under
`IS_WIN_PHI_CLIENT` with the Mac's numbers, all in one Phi-owned header,
`chrome/browser/ui/views/frame/phi_split_view_style.h`:

| Mac (SplitPaneHostView.swift) | Windows |
| --- | --- |
| `paneInset` 4, top 0 in Comfortable/Balanced, 4 in Performance | `kPhiSplitPaneInset`, top from the pushed style |
| `dividerThickness` 4, transparent | resize area 4 DIP, handle layer never shown |
| card radius 10 (macOS 26), 1 pt `border` inside it | outline radius 10, thickness 1; content clip 9 |
| `FocusRingView` 2 pt `themeColor` | outline thickness 2 in the pushed accent on the active pane |
| `magneticSnapRatios` 1/3, 1/2, 2/3 within 18 pt | `snap_points_` + `kSnapDistance` 18 |
| no mini toolbar, no dimming | mini toolbar hidden, no `SplitTabHighlightController` |

Colours are the client's: `PhiShellWindow::PushSplitViewStyle` sends
`PhiFramework::SetSplitViewStyle(window_id, {background, border, focus,
top_inset})` for every hosted browser on its own Space's settled palette
(`window_bg`, `border`, `accent`), from `LayoutContents` and `ApplyPalette`,
only when a value moved. Until the first push Chromium paints its own
colours.

## Drop cards

`phi_split_drop.cc`'s replace-mode cards trace the panes Chromium actually
laid out: `PhiSplitDropHints` carries the two pane rects from
`PhiSidebarShell::SidebarSplitPaneRects` (the framework's
`TabContentBoundsById`, mapped into the shell). The ratio estimate (4 DIP
inset + 4 DIP divider over the page area) only stands in before the first
layout.

## Model notifications

A layout flip or a settled divider drag now marks the window snapshot
(`WindowChangeReason::kSplits`): `PhiBridgeWrapper::OnSplitVisualsChanged`
was a no-op on Windows before, so the sidebar's "Convert to … Split" title
and the drop cards could read a stale layout or ratio. Mid-drag ratio steps
(`is_intermediate`) stay in the view.

## Checking it on the box

`test/probes/sv-split-look.mjs` (run from `phitest\probes`): `form
[vertical|horizontal]`, `ratio <r>`, `layout <axis>`, `active
<primary|secondary>`, `mode <layout mode>`, `appearance <light|dark>`,
`theme <id>`, `unsplit`, `state`. Shoot with PhiRm (a page click moves the
ring), drag the divider or a strip tab with PhiDragCap.

Known, unrelated: after `agentSpace.theme.set {appearance}` the content
card's corner caps keep the old tint until the next relayout.
