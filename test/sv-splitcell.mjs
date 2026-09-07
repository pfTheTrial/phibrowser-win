// Phase 1 of docs/sidebar-architecture.md: ONE stored row per split cell.
// Opens every pinned cell that carries a pane B through the same path a
// sidebar click takes (agentSpace.pinned.open -> OpenOrFocusPinned ->
// OpenStoredSplit) and asserts that two live tabs come up tagged
// "<guid>" and "<guid>#b" and share one live split.
import { openControl } from './phi-test.mjs'
const ctl = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v))
let failures = 0
const check = (ok, what) => { console.log((ok ? 'PASS ' : 'FAIL ') + what); if (!ok) failures++ }

const { spaces } = await ctl.send('agentSpace.spaces.list', {})
const active = spaces.find(s => s.isActive && s.windowIds.length) || spaces.find(s => s.windowIds.length)
const wid = active.windowIds[0]
const pinned = await ctl.send('agentSpace.pinned.list', { windowId: wid })
show('pinned:', (pinned.pinned || []).map(p => ({ guid: p.guid.slice(0, 8), url: p.url, secondaryUrl: p.secondaryUrl, layout: p.layout })))
check(pinned.ok && !pinned.pinned.some(p => 'splitPartnerGuid' in p), 'pinned.list carries no splitPartnerGuid')
const cells = (pinned.pinned || []).filter(p => p.secondaryUrl)
check(cells.length > 0, `at least one split cell in the store (${cells.length})`)
const known = new Set((pinned.pinned || []).map(p => p.guid))
// Bookmarks bind by tag too: walk the whole tree so every stored guid is known.
const walk = async (parentGuid) => {
  const r = await ctl.send('agentSpace.bookmarks.list', { windowId: wid, parentGuid })
  for (const b of (r.bookmarks || [])) { known.add(b.guid); if (b.folder) await walk(b.guid) }
}
await walk('')
// Optional: guids folded away by the schema 1 -> 2 migration (argv). No live
// tab may still carry one — the coordinator retags them on arrival.
const folded = process.argv.slice(2)
{
  const { tabs } = await ctl.send('agentSpace.spaces.listTabs', { windowId: wid })
  show('tags before:', tabs.filter(t => t.tag).map(t => ({ index: t.index, tag: t.tag.slice(0, 12), splitId: t.splitId && t.splitId.slice(0, 6) })))
  for (const g of folded) check(!tabs.some(t => t.tag && t.tag.startsWith(g)), `no live tab still tagged with folded guid ${g.slice(0, 8)}`)
  // A tag naming no row of THIS Space is not a failure: rows of other Spaces
  // bind too, and a stale tag simply leaves its tab in the plain list.
  const stray = tabs.filter(t => t.tag && !known.has(t.tag.replace(/#b$/, '')))
  if (stray.length) console.log('WARN tags naming no row of this Space:', JSON.stringify(stray.map(t => t.tag)))
}
// Start each cell from CLOSED: close its live panes (highest index first).
const closePanes = async (guid) => {
  const { tabs } = await ctl.send('agentSpace.spaces.listTabs', { windowId: wid })
  const mine = tabs.filter(t => t.tag === guid || t.tag === guid + '#b').sort((x, y) => y.index - x.index)
  for (const t of mine) await ctl.send('agentSpace.tabs.close', { windowId: wid, index: t.index })
  await new Promise(r => setTimeout(r, 800))
}

for (const cell of cells) {
  await closePanes(cell.guid)
  const open = await ctl.send('agentSpace.pinned.open', { windowId: wid, guid: cell.guid })
  check(open.ok, `pinned.open ${cell.guid.slice(0, 8)}`)
  await new Promise(r => setTimeout(r, 1500))
  const { tabs } = await ctl.send('agentSpace.spaces.listTabs', { windowId: wid })
  const a = tabs.find(t => t.tag === cell.guid)
  const b = tabs.find(t => t.tag === cell.guid + '#b')
  show('panes:', { a: a && { tabId: a.tabId, url: a.url, splitId: a.splitId }, b: b && { tabId: b.tabId, url: b.url, splitId: b.splitId } })
  check(!!a, 'pane A live and tagged <guid>')
  check(!!b, 'pane B live and tagged <guid>#b')
  check(a && b && a.splitId && a.splitId === b.splitId, 'both panes share one live split')
  const splits = await ctl.send('agentSpace.splitView.list', { spaceId: active.spaceId })
  const s = (splits.splits || []).find(x => a && x.splitId === a.splitId)
  show('split:', s || splits)
  check(!!s && s.layout === (cell.layout || 'vertical'), 'split reopened with the stored layout')

  // Second open must FOCUS, not duplicate.
  const again = await ctl.send('agentSpace.pinned.open', { windowId: wid, guid: cell.guid })
  await new Promise(r => setTimeout(r, 800))
  const after = await ctl.send('agentSpace.spaces.listTabs', { windowId: wid })
  const tagged = after.tabs.filter(t => t.tag === cell.guid || t.tag === cell.guid + '#b')
  check(again.ok && tagged.length === 2, `re-open focuses instead of duplicating (${tagged.length} tagged tabs)`)
  check(tagged.some(t => t.active), 'a pane of the cell is the active tab after re-open')
}
console.log(failures ? `${failures} FAILED` : 'ALL PASS')
ctl.close()
process.exit(failures ? 1 : 0)
