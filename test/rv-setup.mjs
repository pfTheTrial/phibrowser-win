// Reader View: load a page with a real article so upstream's distillability
// verdict fires, and report where the address capsule is for the shot.
import { openControl, sleep } from './phi-test.mjs'
// Upstream's distillability service does NOT run on data: URLs, so the
// verdict only ever fires for a real http(s) article.
const ARTICLE = 'https://en.wikipedia.org/wiki/Readability'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
await ctl.send('agentSpace.window.setBounds', { windowId: w, x: 100, y: 60, width: 1500, height: 960 })
await sleep(500)
let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
while (tabs.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
  await sleep(200); ({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
}
await ctl.send('agentSpace.navigate', { windowId: w, url: ARTICLE })
await sleep(3000)
const l = await ctl.send('agentSpace.window.layout', { windowId: w })
console.log('urlField:', JSON.stringify(l.urlField), 'frameLeft', l.frameLeft, 'frameTop', l.frameTop)
console.log('CAPSULE_RIGHT', 100 + l.frameLeft + l.urlField.x + l.urlField.width,
            'MID', 60 + l.frameTop + l.urlField.y + Math.round(l.urlField.height / 2))
ctl.close()
