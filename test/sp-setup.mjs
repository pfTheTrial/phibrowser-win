// Stacked splits: put two tabs into a side-by-side split, then report the
// merged sidebar row's screen position so a graft can drive its menu.
import { openControl, sleep } from './phi-test.mjs'

const PAGES = [['LEFT', '%231f4e79'], ['RIGHT', '%23196f3d']]
const page = ([name, color]) =>
  `data:text/html,<title>${name}</title><body style="margin:0;background:${color};` +
  `display:flex;align-items:center;justify-content:center;height:100vh">` +
  `<h1 style="font:700 96px system-ui;color:white">${name}</h1></body>`

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const X = 100
const Y = 60
await ctl.send('agentSpace.window.setBounds', {
  windowId: w, x: X, y: Y, width: 1500, height: 960,
})
await sleep(500)

let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
while (tabs.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
  await sleep(200)
  ;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
}
await ctl.send('agentSpace.navigate', { windowId: w, url: page(PAGES[0]) })
await sleep(700)
await ctl.send('agentSpace.tabs.new', { windowId: w, url: page(PAGES[1]) })
await sleep(900)

;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
const [a, b] = tabs
// splitView.* is the SKILL dialect: it scopes by spaceId/taskId, not windowId.
const made = await ctl.send('agentSpace.splitView.create', {
  spaceId: '1', primaryTabId: a.id, secondaryTabId: b.id,
})
console.log('split:', JSON.stringify(made))
await sleep(900)
console.log('splits:', JSON.stringify(await ctl.send('agentSpace.splitView.list', { spaceId: '1' })))

const layout = await ctl.send('agentSpace.window.layout', { windowId: w })
const rowY = Y + layout.frameTop + layout.tabsTop + Math.round(layout.rowHeight / 2)
console.log('MERGEDROW', X + layout.frameLeft + Math.round(layout.sidebarWidth / 2), rowY)
console.log('layout:', JSON.stringify(layout))
ctl.close()
