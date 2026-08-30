// Tab hover previews: stage a window with snapshot-bearing tabs and print
// the sidebar row geometry the capture graft needs to aim the cursor.
//
// Chromium only caches a hover-card thumbnail for a tab that has been SEEN
// and then hidden, so every tab is activated once before we settle back on
// the first one.
import { openControl, sleep } from './phi-test.mjs'

const PAGES = [
  ['ALPHA', '%23c0392b'],
  ['BRAVO', '%23196f3d'],
  ['CHARLIE', '%231f4e79'],
]
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
await sleep(600)

// One tab per page, starting from whatever the launch task left behind.
let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
while (tabs.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
  await sleep(200)
  ;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
}
await ctl.send('agentSpace.navigate', { windowId: w, url: page(PAGES[0]) })
await sleep(700)
for (const p of PAGES.slice(1)) {
  await ctl.send('agentSpace.tabs.new', { windowId: w, url: page(p) })
  await sleep(900)
}

// Walk every tab so each one records a snapshot on its way out, then rest
// on the first: tabs 2 and 3 are then non-foreground WITH thumbnails.
;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
for (const t of tabs) {
  await ctl.send('agentSpace.tabs.activate', { windowId: w, index: t.index })
  await sleep(800)
}
await ctl.send('agentSpace.tabs.activate', { windowId: w, index: 0 })
await sleep(800)

const layout = await ctl.send('agentSpace.window.layout', { windowId: w })
;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))

// Row i's centre in SCREEN pixels: the window origin, plus the client-area
// offset, plus the strip's own stacking.
const rowY = (i) =>
  Y + layout.frameTop + layout.tabsTop +
  i * (layout.rowHeight + layout.rowGap) + Math.round(layout.rowHeight / 2)
const rowX = X + layout.frameLeft + Math.round(layout.sidebarWidth / 2)

console.log('layout:', JSON.stringify(layout))
console.log('tabs:', JSON.stringify(tabs.map(t => ({ i: t.index, a: t.active, t: t.title }))))
console.log('ROWX', rowX)
tabs.forEach((t, i) => console.log('ROWY', i, rowY(i), t.title, t.active ? 'ACTIVE' : ''))
ctl.close()
