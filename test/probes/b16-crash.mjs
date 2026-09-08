// B16: drive the active tab into chrome://crash — the sad tab check.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const r = await ctl.send('agentSpace.navigate', { windowId: w, url: 'chrome://crash' })
console.log('navigate:', JSON.stringify(r))
await new Promise(res => setTimeout(res, 2500))
const t = await ctl.send('agentSpace.tabs.list', { windowId: w })
console.log('tabs:', JSON.stringify(t.tabs.map(x => ({ title: x.title, url: x.url }))))
ctl.close()
