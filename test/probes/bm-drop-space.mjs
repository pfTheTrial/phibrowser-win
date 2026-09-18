// Bookmark folder drop space (docs/windows-native-ui.md §4.8): seeds the
// outline (Work > two bookmarks + Specs; Reading, empty; two roots), opens
// Work with a real click, then drags a tab row over the outline with the
// PhiDragCap rig and shoots four resting points:
//   a  lower half of Work's last child   -> the gap opens at Work's end, Work's
//                                          whole area (incl. the gap) washed
//   b  Reading (collapsed) upper half, 1.2 s -> hover-expands; the gap under its
//                                          header is the space, Reading washed
//   c  a root bookmark's upper half      -> plain gap, no folder lit
//   d  below the last root row           -> the root's end: the list below parts
// The release lands at Work's end (the tab becomes Work's last bookmark).
// Runs ON the box.
import { readFileSync, existsSync, unlinkSync } from 'node:fs'
import { openControl, sleep } from '../phi-test.mjs'
import { runSteps } from '../lib/dragrig.mjs'
import { boxPath } from '../lib/box.mjs'

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const windowId = windows[0]
for (const extra of windows.slice(1)) {
  await ctl.send('agentSpace.close', { windowId: extra }).catch(() => {})
}
const { bookmarks = [] } = await ctl.send('agentSpace.bookmarks.list', {})
for (const row of bookmarks) {
  await ctl.send('agentSpace.bookmarks.remove', { guid: row.guid })
  await sleep(80)
}
const folder = async (title, parentGuid) => {
  const made = await ctl.send('agentSpace.bookmarks.addFolder',
    parentGuid ? { title, parentGuid } : { title })
  await sleep(120)
  return made.guid || made.bookmark?.guid
}
const mark = async (title, url, parentGuid) => {
  await ctl.send('agentSpace.bookmarks.add',
    parentGuid ? { title, url, parentGuid } : { title, url })
  await sleep(120)
}
const work = await folder('Work')
await mark('phibrowser-cli · GitHub', 'https://github.com/', work)
await mark('Chromium code search', 'https://source.chromium.org/', work)
await folder('Specs', work)
await folder('Reading')
await mark('Design system notes', 'https://example.com/design')
await mark('LinkedIn', 'https://www.linkedin.com/')
// Two tabs, so a row can be dragged while another keeps the window.
await ctl.send('agentSpace.tabs.new', { windowId, url: 'https://example.com/' })
await sleep(1500)

await runSteps(['park'])
const probe = readFileSync(boxPath('dragprobe.txt'), 'utf8')
const origin = /origin=(-?\d+),(-?\d+)/.exec(probe)
const ox = Number(origin[1]); const oy = Number(origin[2])
console.log('origin', ox, oy)
// The rig adds the parked window's client origin to every point: points
// are client-relative, physical.
const dx = 0, dy = 0
const rows = async () => (await ctl.send('agentSpace.window.layout', { windowId })).rows || []
const mid = (r) => [Math.round((r.left + r.right) / 2) + dx, Math.round((r.top + r.bottom) / 2) + dy]
const at = (r, frac) => [Math.round((r.left + r.right) / 2) + dx, Math.round(r.top + (r.bottom - r.top) * frac) + dy]
let all = await rows()
const workRow = all.find((r) => r.band === 'bookmarks' && r.folder && r.depth === 0)
const [wx, wy] = mid(workRow)
await runSteps([`down,${wx},${wy}`, 'sleep,120', 'up', 'sleep,900'])
all = await rows()
const bm = all.filter((r) => r.band === 'bookmarks')
console.log(JSON.stringify(bm.map((r) => [r.key.slice(0, 12), r.depth, r.expanded, r.top, r.bottom])))
const specs = bm.find((r) => r.folder && r.depth === 1)
const reading = bm.find((r) => r.folder && r.depth === 0 && r.key !== workRow.key)
const design = bm.find((r) => !r.folder && r.depth === 0)
const linkedin = bm.filter((r) => !r.folder && r.depth === 0).at(-1)
const tabRows = all.filter((r) => r.band === 'tabs' && r.key.startsWith('tab:') && !r.groupId)
const tab = tabRows.find((r) => !r.active) || tabRows[0]
console.log('tab', JSON.stringify(tab))
const [tx, ty] = mid(tab)
const pitch = specs.bottom - specs.top + 2
for (const n of 'abcd') { try { unlinkSync(boxPath(`drag-tail-${n}.png`)) } catch { /* none */ } }
const steps = runSteps([
  `down,${tx},${ty}`, `move,${tx},${ty + 14}`, `move,${tx},${ty + 30}`, 'sleep,300',
  `move,${at(specs, 0.8)[0]},${at(specs, 0.8)[1]}`, 'sleep,900', 'shot,tail-a', 'sleep,900',
  // Reading rests one pitch lower while Work's end gap is open; on arrival the
  // gap closes and Reading slides back up under the pointer, so aim at its
  // resting upper half plus a little.
  `move,${at(reading, 0.3)[0]},${at(reading, 0.35)[1]}`, 'sleep,1600', 'shot,tail-b', 'sleep,900',
  // Reading is open now (a gap under its header): the root rows sit a pitch lower.
  `move,${at(design, 0.3)[0]},${at(design, 0.3)[1] + pitch}`, 'sleep,900', 'shot,tail-c', 'sleep,900',
  `move,${at(linkedin, 0.5)[0]},${at(linkedin, 0.85)[1]}`, 'sleep,2500', 'shot,tail-d', 'sleep,900',
  `move,${at(specs, 0.8)[0]},${at(specs, 0.8)[1]}`, 'sleep,700', 'up', 'sleep,900',
], { timeout: 120000 })
const snap = async (label) => {
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  const marks = (l.rows || []).filter((r) => r.band === 'bookmarks')
  console.log(label, JSON.stringify(l.drag), JSON.stringify(marks.map((r) => [r.key.slice(0, 12), r.depth, r.expanded, r.top, r.bottom])))
}
for (const n of 'abcd') {
  const file = boxPath(`drag-tail-${n}.png`)
  const deadline = Date.now() + 30000
  while (!existsSync(file) && Date.now() < deadline) await sleep(100)
  await sleep(200)
  await snap(n)
}
await steps
console.log('after', JSON.stringify((await rows()).filter((r) => r.band === 'bookmarks').map((r) => [r.key.slice(0, 12), r.depth])))
ctl.close()
