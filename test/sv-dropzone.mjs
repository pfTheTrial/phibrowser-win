// The EMPTY stored bands' drop zones and the outline's end (2026-09-10):
// while a tab drags, a profile with no pins reserves the Mac's 100pt pinned
// zone and one with no bookmarks a one-row bookmark zone, and the outline's
// band runs through its separator row so a drop below the last bookmark
// lands at the root's end. Runs ON the box, against a profile whose stored
// bands are EMPTY (PHI_USER_DATA_DIR=C:\phi-fx-oobe); drives real drags
// through PhiDragCap and leaves drag-z1/z2/z3.png mid-drag shots behind.
import { openControl } from './phi-test.mjs'
import { check, show, finish, failureCount } from './lib/check.mjs'
import { drag, park, mid } from './lib/dragrig.mjs'
import { activeWindow } from './lib/window.mjs'

const ctl = await openControl()
const wid = await activeWindow(ctl)
const layout = async () => await ctl.send('agentSpace.window.layout', { windowId: wid })
const tabs = async () => (await ctl.send('agentSpace.spaces.listTabs', { windowId: wid })).tabs
const pins = async () => (await ctl.send('agentSpace.pinned.list', { windowId: wid })).pinned || []
const roots = async () => (await ctl.send('agentSpace.bookmarks.list', { windowId: wid })).bookmarks || []
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

await park()
let L = await layout()
check(L.ok && L.mode !== 'comfortable', `vertical layout (${L.mode})`)
const sc = (dip) => Math.round(dip * L.dpi / 96)
const rowsOf = (band) => L.rows.filter((r) => r.band === band && r.right > r.left)
const tabRows = () => rowsOf('tabs').filter((r) => r.key.startsWith('tab:'))
const sidebarMid = Math.round(L.sidebarWidth / 2)

check((await pins()).length === 0, 'no pins to begin with')
check((await roots()).length === 0, 'no bookmarks to begin with')
for (const url of ['https://example.org/', 'https://example.net/', 'https://example.com/?z']) {
  if (tabRows().length >= 4) break
  await ctl.send('agentSpace.spaces.openTab', { windowId: wid, url })
  await sleep(700)
  L = await layout()
}
check(tabRows().length >= 4, `enough tab rows (${tabRows().length})`)
show('rows:', L.rows.map((r) => `${r.band}:${r.key.slice(0, 12)}@${r.top}`))

// With both stored bands empty, "+ New Tab" sits where the pinned band
// would start: favorites.top = tabsTop - (36 + 2) dips.
const favoritesTop = L.tabsTop - sc(36) - sc(2)

// ---- Z1: a tab dropped on the EMPTY pinned band's reserved zone ----------
{
  const victim = tabRows()[tabRows().length - 1]
  const target = [sidebarMid, favoritesTop + Math.round(sc(100) / 2)]
  show('Z1 drag', mid(victim), '->', target)
  await drag('z1', mid(victim), target)
  const after = await pins()
  const bound = (await tabs()).find((t) => t.tabId === victim.tabId)
  check(after.length === 1, `Z1 the zone made the first pin (${after.length})`)
  check(bound && bound.tag && after.some((p) => p.guid === bound.tag), 'Z1 the tab is bound to it')
  L = await layout()
}

// ---- Z2: a tab dropped on the EMPTY bookmark band's reserved zone --------
{
  const grid = rowsOf('pinned')
  check(grid.length === 1, `Z2 one grid cell placed (${grid.length})`)
  if (grid.length) {
    // favorites.bottom = the cell's bottom + the 8 inset; the one-row zone
    // opens 3 under it.
    const zoneTop = grid[0].bottom + sc(8) + sc(3)
    const victim = tabRows()[tabRows().length - 1]
    const target = [sidebarMid, zoneTop + sc(18)]
    show('Z2 drag', mid(victim), '->', target)
    await drag('z2', mid(victim), target)
    const after = await roots()
    const bound = (await tabs()).find((t) => t.tabId === victim.tabId)
    check(after.length === 1, `Z2 the zone made the first bookmark (${after.length})`)
    check(bound && bound.tag && after.some((b) => b.guid === bound.tag), 'Z2 the tab is bound to it')
    L = await layout()
  }
}

// ---- Z3: a tab dropped BELOW the last bookmark (the separator row) -------
{
  const outline = rowsOf('bookmarks')
  check(outline.length === 1, `Z3 one outline row placed (${outline.length})`)
  if (outline.length) {
    const last = outline[outline.length - 1]
    const victim = tabRows()[tabRows().length - 1]
    const before = await roots()
    const target = [sidebarMid, last.bottom + sc(8)]
    show('Z3 drag', mid(victim), '->', target)
    await drag('z3', mid(victim), target)
    const after = await roots()
    const bound = (await tabs()).find((t) => t.tabId === victim.tabId)
    check(after.length === before.length + 1, `Z3 a bookmark landed past the last row (${after.length})`)
    const mine = bound && after.find((b) => b.guid === bound.tag)
    check(!!mine, 'Z3 the tab is bound to it')
    check(mine && after.every((b) => b.index <= mine.index), `Z3 and it is the root's LAST (index ${mine?.index})`)
  }
}

// Clean up: the profile's bands go back to empty; the tabs stay, plain.
for (const p of await pins()) await ctl.send('agentSpace.pinned.remove', { windowId: wid, guid: p.guid })
for (const b of await roots()) await ctl.send('agentSpace.bookmarks.remove', { windowId: wid, guid: b.guid })
await sleep(600)
check((await pins()).length === 0 && (await roots()).length === 0, 'cleanup: both bands empty again')

finish()
ctl.close()
process.exit(failureCount() ? 1 : 0)
