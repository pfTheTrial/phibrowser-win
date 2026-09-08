// Phase 4 of docs/sidebar-architecture.md: drags land through ONE verb
// (SidebarMove over a resolved SidebarPlace) and the framework's anchored
// moves. Runs ON the box: drives real mouse drags through the PhiDragCap
// scheduled task (C:\Users\jdxin\drag.txt), aims them with the rows that
// agentSpace.window.layout reports, and asserts the outcome through the
// control pipe. Layout must be a vertical one (performance/balanced).
import { openControl } from './phi-test.mjs'
import { check, show, finish, failureCount } from './lib/check.mjs'
import { drag, park, mid } from './lib/dragrig.mjs'
import { activeWindow } from './lib/window.mjs'

const ctl = await openControl()
const wid = await activeWindow(ctl)
const layout = async () => await ctl.send('agentSpace.window.layout', { windowId: wid })
const tabs = async () => (await ctl.send('agentSpace.spaces.listTabs', { windowId: wid })).tabs
const pins = async () => (await ctl.send('agentSpace.pinned.list', { windowId: wid })).pinned

// Park first so the rows below are measured on the parked window.
await park()
let L = await layout()
check(L.ok && L.mode !== 'comfortable', `vertical layout (${L.mode})`)
const rowsOf = (band) => L.rows.filter(r => r.band === band && r.right > r.left)
show('rows:', L.rows.map(r => `${r.band}:${r.key.slice(0, 12)}@${r.top}`))

// ---- T1: reorder a tab row to the top of the list (MoveTabBefore) --------
{
  const list = rowsOf('tabs').filter(r => r.key.startsWith('tab:'))
  check(list.length >= 3, `enough tab rows to reorder (${list.length})`)
  const last = list[list.length - 1]
  const first = list[0]
  const before = await tabs()
  await drag('t1', mid(last), [mid(first)[0], first.top + 4])
  const after = await tabs()
  const moved = after.find(t => t.tabId === last.tabId)
  const firstAfter = after.find(t => t.tabId === first.tabId)
  check(moved && firstAfter && moved.index < firstAfter.index,
        `T1 last tab row moved above the first (${moved?.index} < ${firstAfter?.index})`)
  check(after.length === before.length, 'T1 nothing opened or closed')
  L = await layout()
}

// ---- T2: a tab row dragged onto the pinned grid becomes a bound pin -------
{
  const list = rowsOf('tabs').filter(r => r.key.startsWith('tab:'))
  const victim = list[list.length - 1]
  const grid = rowsOf('pinned')
  const target = grid.length ? [grid[grid.length - 1].right - 6, mid(grid[grid.length - 1])[1]]
                             : [60, 190]
  const pinsBefore = await pins()
  await drag('t2', mid(victim), target)
  const pinsAfter = await pins()
  const bound = (await tabs()).find(t => t.tabId === victim.tabId)
  check(pinsAfter.length === pinsBefore.length + 1, `T2 one new pin (${pinsBefore.length} -> ${pinsAfter.length})`)
  check(bound && bound.tag && pinsAfter.some(p => p.guid === bound.tag), 'T2 the tab is bound to the new pin')
  L = await layout()
  const cell = rowsOf('pinned').find(r => r.tabId === victim.tabId)
  check(!!cell, 'T2 the new cell shows as open')

  // ---- T3: the pin dragged back into the list is unpinned ---------------
  if (cell) {
    const list2 = rowsOf('tabs').filter(r => r.key.startsWith('tab:'))
    const dest = list2[0]
    await drag('t3', mid(cell), [mid(dest)[0], dest.top + 4])
    const pinsAfter3 = await pins()
    const plain = (await tabs()).find(t => t.tabId === victim.tabId)
    check(pinsAfter3.length === pinsBefore.length, `T3 the pin is gone (${pinsAfter3.length})`)
    check(plain && !plain.tag, 'T3 the tab is plain again')
    const firstAfter = (await tabs()).find(t => t.tabId === dest.tabId)
    check(plain && firstAfter && plain.index < firstAfter.index, 'T3 and landed at the anchor')
    L = await layout()
  }
}

// ---- T4: a tab dropped INTO a collapsed folder's upper half --------------
{
  const folder = rowsOf('bookmarks').find(r => r.folder && !r.expanded)
  const list = rowsOf('tabs').filter(r => r.key.startsWith('tab:'))
  if (!folder || list.length < 2) {
    console.log('SKIP T4 (no collapsed folder or too few tabs)')
  } else {
    const victim = list[list.length - 1]
    const childrenBefore = (await ctl.send('agentSpace.bookmarks.list', { windowId: wid, parentGuid: folder.guid })).bookmarks || []
    await drag('t4', mid(victim), [mid(folder)[0], folder.top + 5])
    const childrenAfter = (await ctl.send('agentSpace.bookmarks.list', { windowId: wid, parentGuid: folder.guid })).bookmarks || []
    const bound = (await tabs()).find(t => t.tabId === victim.tabId)
    check(childrenAfter.length === childrenBefore.length + 1, `T4 folder gained one child (${childrenBefore.length} -> ${childrenAfter.length})`)
    check(bound && bound.tag && childrenAfter.some(b => b.guid === bound.tag), 'T4 the tab is bound to the new bookmark')
    check(!(await layout()).rows.some(r => r.band === 'tabs' && r.tabId === victim.tabId), 'T4 the bound tab left the list (folder collapsed)')
    // Clean up: remove the bookmark so the tab returns plain.
    if (bound?.tag) await ctl.send('agentSpace.bookmarks.remove', { windowId: wid, guid: bound.tag })
  }
}

finish()
ctl.close()
process.exit(failureCount() ? 1 : 0)
