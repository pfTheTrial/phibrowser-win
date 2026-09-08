// Probe: slot/space/profile readout plus an activate repro.
import { openControl, sleep } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
console.log('slot', w)
// normalize like suite-tabui
let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
while (tabs.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
  await sleep(200); ({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
}
await ctl.send('agentSpace.navigate', { windowId: w, url: 'data:text/html,<title>A</title>' })
await sleep(400)
await ctl.send('agentSpace.tabs.new', { windowId: w, url: 'data:text/html,<title>B</title>' })
await sleep(600)
await ctl.send('agentSpace.tabs.activate', { windowId: w, index: 0 })
await sleep(500)
;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
console.log('after normalize:', JSON.stringify(tabs.map(t => ({ i: t.index, a: t.active, t: t.title }))))
// the failing step: activate the other tab
const target = tabs.find(t => !t.active)
await ctl.send('agentSpace.tabs.activate', { windowId: w, index: target.index })
await sleep(800)
;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
console.log('after activate:', JSON.stringify(tabs.map(t => ({ i: t.index, a: t.active, t: t.title }))))
const sp = await ctl.send('agentSpace.spaces.list', { windowId: w })
console.log('spaces:', JSON.stringify(sp.spaces.map(s => ({ id: s.id, a: s.active, o: s.open }))))
ctl.close()
