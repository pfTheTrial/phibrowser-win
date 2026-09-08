// Auto-Peek probe: navigate a BOUND (pinned) tab cross-site and ask CDP what
// page targets exist afterwards — a peek is its own browser, so it shows up
// as a target even though it is not in agentSpace.list.
import { openControl, cdpHttp, sleep } from '../phi-test.mjs'
const ctl = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v))
const targets = async (label) => {
  const list = await cdpHttp('/json/list')
  show(label, (list || []).map(t => ({ type: t.type, url: t.url })))
}
const win = (await ctl.send('agentSpace.list', {})).windows[0]
const pins = (await ctl.send('agentSpace.pinned.list', { windowId: win })).pinned || []
const pin = pins.find(p => (p.url || '').includes('example.com'))
show('open pin:', await ctl.send('agentSpace.pinned.open', { windowId: win, guid: pin.guid }))
await sleep(3000)
await targets('targets before:')
show('nav cross-site:', await ctl.send('agentSpace.navigate',
  { windowId: win, url: 'https://www.iana.org/help/example-domains' }))
await sleep(6000)
await targets('targets after:')
show('tabs after:', ((await ctl.send('agentSpace.tabs.list', { windowId: win })).tabs || [])
  .map(t => ({ id: t.id, url: t.url, tag: t.tag, active: t.active })))
ctl.close()
