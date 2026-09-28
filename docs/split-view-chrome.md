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
(`ground_top`, `border`, `accent`), from `LayoutContents` and `ApplyPalette`,
only when a value moved. Until the first push Chromium paints its own
colours.

When the Windows backdrop is available, the ground uses the sidebar's
theme-preserving tint, converted from premultiplied DIB pixels to straight
ARGB for Skia (`PhiBackdropColor`). The shell extends Acrylic under the
whole content area and leaves its backing pixels clear; Chromium paints the tint
once, with the page surfaces remaining opaque. Split activation/dismissal,
sidebar collapse and system backdrop changes refresh this contract.
Without the backdrop, the pushed ground stays fully opaque.

The framework must preserve alpha at every layer: the normal browser
widget requests native translucency, its desktop host keeps full DWM
frame margins and a transparent compositor root, and the split background
layer advertises the pushed color's actual opacity. The hosted browser
does not paint Chromium's hidden caption beneath the top gutter. These
changes live in `browser_widget.cc`, `browser_desktop_window_tree_host_win.cc`,
`browser_frame_view_win.cc` and `multi_contents_background_view.cc` in the
Chromium fork; deployment needs both the matching client and framework.

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

Transparency verification uses desktop captures, because `PrintWindow`
does not include the composed Windows backdrop. Put a black, then white,
window behind Phi: the top split gap should match the sidebar, the divider
should change with the backdrop (with its pane shadows), and opaque page
pixels must remain unchanged. Check both split axes and a hidden sidebar.
The host layout test checks that Skia's straight-alpha tint reconstructs
the same premultiplied color as the sidebar in light and dark appearances.

Rounded corners use the same ground in normal tabs and splits. A normal
page is clipped at its lower-left corner by Chromium's compositor; the
background view stays visible behind it. Native new-tab and bookmark
overlays use a matching window region. Opaque corner-cap windows are
removed while the backdrop is active (and always in split view). The
shell preserves alpha outside the address row's rounded corners, while
known card coverage keeps the row itself opaque even when its RGB matches
the ground. Live palette changes therefore recolor the exposed ground
without waiting for a resize. Fullscreen removes the single-page clip.
Ordinary native surfaces use an opaque 32-bit GDI buffer (`PhiWindow`)
so extending glass beneath them does not make the new-tab/bookmark page
itself translucent. Shaped panels keep their existing per-pixel alpha path.

Include the bottom-left page corner and the address row's upper-left
corner in desktop pixel probes. In a split, also check the row's rounded
lower-left corner and the pane corners beside the divider. Change theme
without relayout and check an ordinary web page and a native new-tab page.
