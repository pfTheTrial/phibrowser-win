// The row drag image for a pinned cell (2026-09-10): a cell dragged over
// the tab list or the bookmark band shows as the ROW it becomes. Two
// mid-drag shots (r1 over the list, r2 over the outline / its zone), then
// released back home so nothing moves. Runs ON the box against the OOBE
// profile (PHI_USER_DATA_DIR=C:\phi-fx-oobe); adds a pin of its own when
// the grid is empty and removes it after.
import { openControl } from './phi-test.mjs'
import { check, show, finish, failureCount } from './lib/check.mjs'
import { park, mid, runSteps } from './lib/dragrig.mjs'
import { activeWindow } from './lib/window.mjs'

const ctl = await openControl()
const wid = await activeWindow(ctl)
const layout = async () => await ctl.send('agentSpace.window.layout', { windowId: wid })
const pins = async () => (await ctl.send('agentSpace.pinned.list', { windowId: wid })).pinned || []
const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

await park()
let L = await layout()
check(L.ok && L.mode !== 'comfortable', `vertical layout (${L.mode})`)
const sc = (dip) => Math.round(dip * L.dpi / 96)
const rowsOf = (band) => L.rows.filter((r) => r.band === band && r.right > r.left)
const pinsBefore = new Set((await pins()).map((p) => p.guid))
if (!pinsBefore.size) {
  await ctl.send('agentSpace.pinned.add', { windowId: wid, url: 'https://example.com/?row', title: 'Row probe' })
  await sleep(400)
  L = await layout()
}
const grid = rowsOf('pinned')
const list = rowsOf('tabs').filter((r) => r.key.startsWith('tab:'))
check(grid.length >= 1 && list.length >= 1, `a cell (${grid.length}) and a tab row (${list.length})`)
const countBefore = (await pins()).length
if (grid.length && list.length) {
  const from = mid(grid[grid.length - 1])
  const overList = [mid(list[0])[0], list[list.length - 1].bottom + sc(10)]
  // The outline band: its rows when it has them, else the one-row zone the
  // pin drag reserves 3 under the grid's band.
  const outline = rowsOf('bookmarks')
  const overOutline = outline.length
      ? mid(outline[0])
      : [mid(grid[0])[0], grid[grid.length - 1].bottom + sc(8) + sc(3) + sc(18)]
  show('drag', from, '->', overList, '->', overOutline)
  await runSteps(['park', `down,${from[0]},${from[1]}`, `move,${from[0]},${from[1] + 12}`,
                  `move,${overList[0]},${overList[1]}`, 'sleep,400', 'shot,r1',
                  `move,${overOutline[0]},${overOutline[1]}`, 'sleep,400', 'shot,r2',
                  `move,${from[0]},${from[1]}`, 'sleep,300', 'up', 'sleep,600'])
  check((await pins()).length === countBefore, `released back home: ${countBefore} pin(s) still`)
}
for (const p of await pins()) {
  if (!pinsBefore.has(p.guid)) await ctl.send('agentSpace.pinned.remove', { windowId: wid, guid: p.guid })
}
check((await pins()).length === pinsBefore.size, 'cleanup: only the pins that were there')
finish()
ctl.close()
process.exit(failureCount() ? 1 : 0)
