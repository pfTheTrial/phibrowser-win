// The pinned-cell drag image (2026-09-10): a tab row dragged over the
// pinned band shows as the CELL it becomes. Two mid-drag shots — over the
// empty band's zone (c1), then over a grid holding one pin (c2, released
// back where it started). Runs ON the box against a profile with EMPTY
// stored bands (PHI_USER_DATA_DIR=C:\phi-fx-oobe); cleans up after itself.
import { openControl } from './phi-test.mjs'
import { check, show, finish, failureCount } from './lib/check.mjs'
import { drag, park, mid, runSteps } from './lib/dragrig.mjs'
import { activeWindow } from './lib/window.mjs'

const ctl = await openControl()
const wid = await activeWindow(ctl)
const layout = async () => await ctl.send('agentSpace.window.layout', { windowId: wid })
const tabs = async () => (await ctl.send('agentSpace.spaces.listTabs', { windowId: wid })).tabs
const pins = async () => (await ctl.send('agentSpace.pinned.list', { windowId: wid })).pinned || []
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

await park()
let L = await layout()
check(L.ok && L.mode !== 'comfortable', `vertical layout (${L.mode})`)
const sc = (dip) => Math.round(dip * L.dpi / 96)
const rowsOf = (band) => L.rows.filter((r) => r.band === band && r.right > r.left)
const tabRows = () => rowsOf('tabs').filter((r) => r.key.startsWith('tab:'))
const sidebarMid = Math.round(L.sidebarWidth / 2)
// The profile may hold pins already (Jixiang tests here): the probe then
// aims at the grid's end instead of the empty band's zone.
const pinsBefore = new Set((await pins()).map((p) => p.guid))
show('pins to begin with:', pinsBefore.size)
// Spend only tabs the probe opened itself.
const opened = []
for (const url of ['https://example.org/?c1', 'https://example.net/?c2']) {
  const before = new Set((await tabs()).map((t) => t.tabId))
  await ctl.send('agentSpace.spaces.openTab', { windowId: wid, url })
  await sleep(700)
  opened.push(...(await tabs()).filter((t) => !before.has(t.tabId)).map((t) => t.tabId))
}
L = await layout()
const mine = () => tabRows().filter((r) => opened.includes(r.tabId))
check(mine().length === 2, `two probe tabs placed (${mine().length})`)
// The empty pinned band's zone opens where the band starts: at the first
// bookmark row when the outline has rows, else at "+ New Tab".
const outline = rowsOf('bookmarks')
const favoritesTop = outline.length ? outline[0].top : L.tabsTop - sc(36) - sc(2)

// C1: over the empty band's zone (or the end of an existing grid), then
// dropped: one more pin.
{
  const victim = mine()[0]
  const grid = rowsOf('pinned')
  const target = grid.length
      ? [grid[grid.length - 1].right - sc(6), mid(grid[grid.length - 1])[1]]
      : [sidebarMid, favoritesTop + Math.round(sc(100) / 2)]
  await drag('c1', mid(victim), target)
  check((await pins()).length === pinsBefore.size + 1, `C1 the drop made a pin (${(await pins()).length})`)
  L = await layout()
}
// C2: over the grid holding that pin (right of the last cell), shot, back
// home: the cells shrink to open the phantom's slot.
{
  const grid = rowsOf('pinned')
  check(grid.length === pinsBefore.size + 1, `C2 grid cells placed (${grid.length})`)
  const victim = mine()[0]
  const from = mid(victim)
  const over = grid.length ? [grid[grid.length - 1].right - sc(6), mid(grid[grid.length - 1])[1]] : [0, 0]
  if (grid.length && victim) await runSteps(['park', `down,${from[0]},${from[1]}`, `move,${from[0]},${from[1] + 12}`,
                  `move,${over[0]},${over[1]}`, 'sleep,400', 'shot,c2',
                  `move,${from[0]},${from[1]}`, 'sleep,300', 'up', 'sleep,600'])
  check((await pins()).length === pinsBefore.size + 1, 'C2 released back home: no new pin')
}
// C3: a grid padded to a full row (three cells at the default width): the
// phantom for one more WRAPS onto a new row, and the band grows for it —
// the outline, "+ New Tab" and the tabs slide down (shot c3).
{
  while ((await pins()).length < 3) {
    const n = (await pins()).length
    await ctl.send('agentSpace.pinned.add', { windowId: wid, url: `https://example.com/?pad${n}`, title: `Pad ${n}` })
    await sleep(300)
  }
  L = await layout()
  const grid = rowsOf('pinned')
  const victim = mine()[0]  // C1 pinned the other one away.
  check(grid.length >= 3 && !!victim, `C3 a full grid row (${grid.length}) and a probe tab`)
  const newTabBefore = L.tabsTop
  if (grid.length >= 3 && victim) {
    const from = mid(victim)
    const last = grid[grid.length - 1]
    const over = [last.right - sc(6), mid(last)[1]]
    await runSteps(['park', `down,${from[0]},${from[1]}`, `move,${from[0]},${from[1] + 12}`,
                    `move,${over[0]},${over[1]}`, 'sleep,400', 'shot,c3',
                    `move,${from[0]},${from[1]}`, 'sleep,300', 'up', 'sleep,600'])
    check((await pins()).length === grid.length, 'C3 released back home: no new pin')
    check((await layout()).tabsTop === newTabBefore, 'C3 the layout settled back')
  }
}
for (const p of await pins()) {
  if (!pinsBefore.has(p.guid)) await ctl.send('agentSpace.pinned.remove', { windowId: wid, guid: p.guid })
}
for (const id of opened) {
  // tabs.close addresses by INDEX: re-list after every close.
  const t = (await tabs()).find((x) => x.tabId === id)
  if (t) await ctl.send('agentSpace.tabs.close', { windowId: wid, index: t.index })
}
await sleep(500)
check((await pins()).length === pinsBefore.size, 'cleanup: the probe\'s pin is gone')
finish()
ctl.close()
process.exit(failureCount() ? 1 : 0)
