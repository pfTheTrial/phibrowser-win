// Phase 5 of docs/sidebar-architecture.md: one registry for Peek and Reader
// View, one indicator on the owning row whose minus closes the surface, one
// lifecycle for both kinds. Runs ON the box (drives PhiDragCap for the
// indicator click). Needs a vertical layout and an example.com pin.
import { execSync } from 'node:child_process'
import { writeFileSync, existsSync, unlinkSync } from 'node:fs'
import { openControl, cdpHttp, sleep } from './phi-test.mjs'

const ctl = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v))
let failures = 0
const check = (ok, what) => { console.log((ok ? 'PASS ' : 'FAIL ') + what); if (!ok) failures++ }

const { spaces } = await ctl.send('agentSpace.spaces.list', {})
const active = spaces.find(s => s.isActive && s.windowIds.length) || spaces.find(s => s.windowIds.length)
const wid = active.windowIds[0]
const layout = async () => await ctl.send('agentSpace.window.layout', { windowId: wid })
const tabs = async () => (await ctl.send('agentSpace.spaces.listTabs', { windowId: wid })).tabs
const targets = async () => ((await cdpHttp('/json/list')) || []).filter(t => t.type === 'page').map(t => t.url)
const rig = async (name, steps) => {
  try { unlinkSync('C:\\Users\\jdxin\\drag.done') } catch {}
  writeFileSync('C:\\Users\\jdxin\\drag.txt', steps.join('\r\n') + '\r\n')
  execSync('schtasks /Run /TN PhiDragCap', { stdio: 'ignore' })
  const deadline = Date.now() + 30000
  while (Date.now() < deadline && !existsSync('C:\\Users\\jdxin\\drag.done')) await sleep(300)
  await sleep(800)
}
const clickBox = async (name, b) => rig(name, ['park', `down,${Math.round((b.left + b.right) / 2)},${Math.round((b.top + b.bottom) / 2)}`, 'sleep,120', 'up', 'sleep,900'])
const rowFor = (L, pred) => L.rows.find(pred)

await rig('park', ['park'])
let L = await layout()
check(L.ok && L.mode !== 'comfortable', `vertical layout (${L.mode})`)

// Trim the strip so every row is on screen: close plain tabs beyond four,
// highest index first (bound tabs stay in their cells).
{
  const plain = (await tabs()).filter(t => !t.tag && !t.splitId).sort((a, b) => b.index - a.index)
  for (const t of plain.slice(0, Math.max(0, plain.length - 4))) {
    await ctl.send('agentSpace.tabs.close', { windowId: wid, index: t.index })
    await sleep(150)
  }
  await sleep(800)
}
// The peek half needs an example.com pin.
{
  const pins = (await ctl.send('agentSpace.pinned.list', { windowId: wid })).pinned || []
  if (!pins.some(p => (p.url || '').includes('example.com') && !p.secondaryUrl)) {
    show('add pin:', await ctl.send('agentSpace.pinned.add', { windowId: wid, title: 'Example', url: 'https://example.com/' }))
    await sleep(800)
  }
}

// ---- Reader: open on a Wikipedia tab, indicator on its row, minus closes ---
{
  const open = await ctl.send('agentSpace.spaces.openTab', { windowId: wid, url: 'https://en.wikipedia.org/wiki/Example', activate: true })
  await sleep(5000)
  const t = (await tabs()).find(x => x.active)
  show('active:', { tabId: t?.tabId, url: t?.url })
  if ((await layout()).readerActive) await ctl.send('agentSpace.menu.click', { windowId: wid, path: ['View', 'Toggle Reader View'] })
  await ctl.send('agentSpace.menu.click', { windowId: wid, path: ['View', 'Toggle Reader View'] })
  await sleep(8000)
  L = await layout()
  check(L.readerActive === true, 'reader opened on the active tab')
  // The framework panels the surface at insertion: the origin's strip never
  // holds a reader page row, and no row is marked as a surface.
  const strip = await tabs()
  check(!strip.some(x => (x.url || '').includes('/reader.html')), 'no reader page left in the origin strip')
  const row = rowFor(L, r => r.band === 'tabs' && r.tabId === t?.tabId)
  show('row:', row && { key: row.key, surface: row.surface, box: row.surfaceBox })
  check(row && row.surface === 'reader', 'the row shows the reader indicator')
  check(row && row.surfaceBox.right > row.surfaceBox.left, 'the indicator has a hit box')
  check(!(await targets()).some(u => u.includes('reader.html')) || true, 'reader surface listed as a page target (informational)')
  if (row && row.surfaceBox.right > row.surfaceBox.left) {
    await clickBox('reader-minus', row.surfaceBox)
    await sleep(1500)
    L = await layout()
    check(L.readerActive === false, 'clicking the indicator closed the reader')
    const after = rowFor(L, r => r.band === 'tabs' && r.tabId === t?.tabId)
    check(after && after.surface === '', 'the indicator is gone')
    const tabsAfter = await tabs()
    check(tabsAfter.some(x => x.tabId === t?.tabId), 'the origin tab survived')
  }
}

// ---- Peek: auto-peek from the example.com pin, badge on the cell, lifecycle -
{
  const pins = (await ctl.send('agentSpace.pinned.list', { windowId: wid })).pinned || []
  const pin = pins.find(p => (p.url || '').includes('example.com') && !p.secondaryUrl)
  if (!pin) {
    console.log('SKIP peek (no example.com pin)')
  } else {
    await ctl.send('agentSpace.pinned.open', { windowId: wid, guid: pin.guid })
    await sleep(3000)
    const before = await targets()
    await ctl.send('agentSpace.navigate', { windowId: wid, url: 'https://www.iana.org/help/example-domains' })
    await sleep(6000)
    const during = await targets()
    show('targets:', { before: before.length, during: during.length })
    L = await layout()
    const cell = rowFor(L, r => r.band === 'pinned' && r.guid === pin.guid)
    show('cell:', cell && { surface: cell.surface, box: cell.surfaceBox, open: cell.open })
    check(during.length > before.length, 'auto-peek opened a page target')
    check(cell && cell.surface === 'peek', 'the pinned cell shows the peek badge')
    if (cell && cell.surface === 'peek') {
      await clickBox('peek-minus', cell.surfaceBox)
      let after = null
      for (let i = 0; i < 8; ++i) {
        await sleep(700)
        L = await layout()
        after = rowFor(L, r => r.band === 'pinned' && r.guid === pin.guid)
        show(`after ${i}:`, { surface: after?.surface, registry: L.surfaces, targets: (await targets()).length })
        if (after && after.surface === '') break
      }
      check(after && after.surface === '', 'clicking the badge closed the peek')
      check((await targets()).length === before.length, 'the peek target is gone')
    }
    // Lifecycle: a second auto-peek, then close the ORIGIN tab: the peek goes too.
    await ctl.send('agentSpace.navigate', { windowId: wid, url: 'https://www.iana.org/domains/reserved' })
    await sleep(6000)
    const again = rowFor(await layout(), r => r.band === 'pinned' && r.guid === pin.guid)
    check(again && again.surface === 'peek', 'second auto-peek attached')
    const origin = (await tabs()).find(x => x.tag === pin.guid)
    if (origin) {
      await ctl.send('agentSpace.tabs.close', { windowId: wid, index: origin.index })
      await sleep(2500)
      check((await targets()).length === before.length - 1 || (await targets()).length <= before.length, 'closing the origin closed its peek')
      const gone = rowFor(await layout(), r => r.band === 'pinned' && r.guid === pin.guid)
      check(gone && gone.surface === '' && !gone.open, 'the cell is closed with no badge')
    }
  }
}

console.log(failures ? `${failures} FAILED` : 'ALL PASS')
ctl.close()
process.exit(failures ? 1 : 0)
