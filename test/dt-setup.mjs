// Dormant tabs: turn session restore on and leave several tabs open, so the
// NEXT launch restores them. Phi never loads a restored background tab on
// its own, so those rows come back dormant — the steady state the dimming is
// for, not a transient one.
import { openControl, sleep } from './phi-test.mjs'

const PAGES = [
  ['ALPHA', '%23c0392b'], ['BRAVO', '%23196f3d'], ['CHARLIE', '%231f4e79'],
]
const page = ([name, color]) =>
  `data:text/html,<title>${name}</title><body style="margin:0;background:${color};` +
  `display:flex;align-items:center;justify-content:center;height:100vh">` +
  `<h1 style="font:700 96px system-ui;color:white">${name}</h1></body>`

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
await ctl.send('agentSpace.window.setBounds', {
  windowId: w, x: 100, y: 60, width: 1500, height: 960,
})
await sleep(400)

console.log('set:', JSON.stringify(
  await ctl.send('agentSpace.settings.set', { restoreLastSession: true })))

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
  await sleep(800)
}
await ctl.send('agentSpace.tabs.activate', { windowId: w, index: 0 })
await sleep(600)
;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
console.log('before restart:', JSON.stringify(
  tabs.map(t => ({ i: t.index, a: t.active, u: t.unloaded, t: t.title }))))
ctl.close()
