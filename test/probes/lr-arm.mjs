// Stage the ARMED replay: turn cold-start restore OFF so the session file is
// left unconsumed, and leave several tabs behind for the next launch to find.
import { openControl, sleep } from '../phi-test.mjs'
const PAGES = [['ALPHA','%23c0392b'],['BRAVO','%23196f3d'],['CHARLIE','%231f4e79']]
const page = ([n,c]) =>
  `data:text/html,<title>${n}</title><body style="margin:0;background:${c};height:100vh">` +
  `<h1 style="font:700 96px system-ui;color:white">${n}</h1></body>`
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
console.log('restore-off:', JSON.stringify(
  await ctl.send('agentSpace.settings.set', { restoreLastSession: false })))
let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
while (tabs.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
  await sleep(200); ({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
}
await ctl.send('agentSpace.navigate', { windowId: w, url: page(PAGES[0]) })
await sleep(700)
for (const p of PAGES.slice(1)) {
  await ctl.send('agentSpace.tabs.new', { windowId: w, url: page(p) }); await sleep(800)
}
;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
console.log('staged:', JSON.stringify(tabs.map(t => t.title.slice(0, 24))))
ctl.close()
