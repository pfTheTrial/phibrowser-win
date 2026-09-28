// Layout suite: the two Windows-native chrome layouts
// (docs/windows-native-ui.md §4). The wire names are the Mac's:
//   balanced     vertical tabs — the sidebar beside a flush page pane whose
//                first row is the address row (the default)
//   comfortable  horizontal tabs — the strip on the caption row, a toolbar,
//                the page edge to edge, no sidebar
//   performance  RETIRED: accepted on the wire and lands on balanced (D9)
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'
import { firstWindow } from './lib/window.mjs'
import { capturePixel } from './lib/pixels.mjs'

// kSidebarWidth in phi_shell_window.cc, in DESIGN UNITS. The client lays
// out in physical pixels, so every expectation here scales by the window's DPI.
const SIDEBAR_DESIGN = 240
const ADDRESS_ROW = 44   // The vertical layout's address row.
const STRIP_ROW = 40     // The horizontal layout's strip row...
const TOOLBAR_ROW = 40   // ...and the toolbar under it.
const scaled = (design, dpi) => Math.round((design * dpi) / 96)

/** Sets the layout and returns the slot's geometry once it has applied. */
async function useLayout(ctl, windowId, mode, expect = mode) {
  const r = await ctl.send('agentSpace.layout.set', { mode })
  assertEq(r.ok, true, `layout.set ${mode} accepted`)
  assertEq(r.mode, expect, `layout.set echoes ${expect}`)
  await sleep(400)
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  assertEq(l.ok, true, 'window.layout ok')
  assertEq(l.mode, expect, `window reports ${expect}`)
  assert(l.dpi >= 96, `window reports its dpi (${l.dpi})`)
  return { ...l.content, sidebar: scaled(SIDEBAR_DESIGN, l.dpi), dpi: l.dpi,
           titleBar: l.titleBar || 0, urlField: l.urlField, appMenu: l.appMenu,
           sidebarToggle: l.sidebarToggle }
}

test('layout: the mode is one of the two', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.layout.get')
    assertEq(r.ok, true, 'layout.get ok')
    assert(['balanced', 'comfortable'].includes(r.mode),
           `a known mode (${r.mode})`)
  } finally { ctl.close() }
})

test('layout: an unknown mode is rejected and changes nothing', async () => {
  const ctl = await openControl()
  try {
    const before = (await ctl.send('agentSpace.layout.get')).mode
    const r = await ctl.send('agentSpace.layout.set', { mode: 'cosy' })
    assertEq(r.ok, false, 'unknown mode rejected')
    assertEq(r.mode, before, 'mode unchanged')
  } finally { ctl.close() }
})

test('layout: vertical tabs — sidebar beside a flush pane with the address row', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const v = await useLayout(ctl, windowId, 'balanced')
    // The pane sits FLUSH against the sidebar and starts under the 44 dip
    // address row; it runs to the window's right and bottom edges.
    assertEq(v.x, v.sidebar, 'pane flush against the sidebar')
    assertEq(v.y, scaled(ADDRESS_ROW, v.dpi), 'the address row above the pane')
    assertEq(v.titleBar, scaled(ADDRESS_ROW, v.dpi), 'the caption row IS the address row')
    assert(v.width > 0 && v.height > 0, 'content is non-empty')
    // The toggle trails the sidebar header; navigation and the omnibox
    // occupy the adjacent address row.
    assertEq(v.sidebarToggle.x, v.sidebar - scaled(8, v.dpi) - scaled(32, v.dpi),
             'toggle at the trailing edge of the sidebar header')
    assert(v.urlField.y >= 0 && v.urlField.y + v.urlField.height <= v.y,
           'the omnibox lives in the address row')
    assert(v.appMenu.x > v.urlField.x + v.urlField.width, 'the app menu trails the omnibox')
  } finally { ctl.close() }
})

test('layout: horizontal tabs — strip row, toolbar, page edge to edge', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const v = await useLayout(ctl, windowId, 'balanced')
    const h = await useLayout(ctl, windowId, 'comfortable')
    assertEq(h.x, 0, 'content reaches the left edge — no sidebar')
    // The bookmarks bar (28) may sit under the toolbar on this profile.
    assert(h.y >= scaled(STRIP_ROW + TOOLBAR_ROW, h.dpi) &&
           h.y <= scaled(STRIP_ROW + TOOLBAR_ROW + 28, h.dpi),
           `strip + toolbar stacked above (y=${h.y})`)
    assertEq(h.titleBar, scaled(STRIP_ROW, h.dpi), 'the caption row is the strip row')
    assert(h.width > v.width + v.sidebar - 40,
           `content gains the sidebar width (${h.width} vs ${v.width})`)
    assert(h.height < v.height, 'content is shorter by the second row')
    assertEq(h.sidebarToggle.width, 0, 'no sidebar toggle without a sidebar')
    await useLayout(ctl, windowId, 'balanced')
  } finally { ctl.close() }
})

test('layout: performance is retired — it lands on vertical tabs', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const v = await useLayout(ctl, windowId, 'performance', 'balanced')
    assertEq(v.x, v.sidebar, 'the vertical layout')
  } finally { ctl.close() }
})

test('layout: every window follows the client-wide setting', async () => {
  const ctl = await openControl()
  try {
    const before = (await ctl.send('agentSpace.list')).windows
    await ctl.send('agentSpace.newWindow')
    let windows = before
    for (let i = 0; i < 24 && windows.length === before.length; i++) {
      await sleep(250)
      windows = (await ctl.send('agentSpace.list')).windows
    }
    const fresh = windows.find((w) => !before.includes(w))
    assert(fresh, 'second window created')
    try {
      await useLayout(ctl, before[0], 'comfortable')
      const other = await ctl.send('agentSpace.window.layout', { windowId: fresh })
      assertEq(other.mode, 'comfortable', 'the other window switched too')
      assert(other.content.x < 40, 'and re-laid out its content')
    } finally {
      await ctl.send('agentSpace.close', { windowId: fresh })
      await useLayout(ctl, before[0], 'balanced')
    }
  } finally { ctl.close() }
})

test('layout: the sidebar really disappears in horizontal tabs (pixels)', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const h = await ctl.send('agentSpace.window.handle', { windowId })
    await ctl.send('agentSpace.window.setBounds',
                   { windowId, x: 60, y: 60, width: 1200, height: 900 })
    // A coloured theme: under Pure the ground is white and indistinguishable
    // from the unpainted content region.
    // Full saturation: the flat tint at the default 10 is too pale for the
    // colour probe.
    await ctl.send('agentSpace.theme.set', { themeId: 'mint', appearance: 'light', saturation: 100 })
    await sleep(400)
    const geo = await ctl.send('agentSpace.window.layout', { windowId })
    const frame = { x: geo.frameLeft || 0, y: geo.frameTop || 0 }
    // A point deep in the sidebar band: painted with the tint while a
    // sidebar exists, and part of the (unpainted) page once it is gone.
    const probe = () => capturePixel(h.ok ? h.hwnd : 0, 100, 400, { frame })

    await useLayout(ctl, windowId, 'balanced')
    const withSidebar = probe()
    assert(isSidebarGround(withSidebar), `sidebar paints there (${withSidebar})`)

    await useLayout(ctl, windowId, 'comfortable')
    const without = probe()
    assert(!isSidebarGround(without), `sidebar is gone (${without})`)

    await useLayout(ctl, windowId, 'balanced')
    assert(isSidebarGround(probe()), 'and comes back')
  } finally { ctl.close() }
})

// The ground carries Mint's tint (green above red and blue, however pale
// the saturation); the page is not painted by the shell at all and
// captures neutral, where green is never the top channel.
const isSidebarGround = (s) => {
  const [r, g, b] = s.split(',').map(Number)
  return g > r && g > b
}

run()
