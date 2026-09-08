// Forms a split from two fresh new-tab pages and reports what the model
// sees afterwards: tabs (url/split/active), the split list, and the layout.
import { openControl, sleep } from '../phi-test.mjs'
import { appendFileSync, writeFileSync } from 'node:fs'
const LOG = 'C:\\Users\\jdxin\\phitest\\ntp-split.log'
writeFileSync(LOG, '')
const say = (...a) => appendFileSync(LOG, a.join(' ') + '\n')
const ctl = await openControl()
const win = (await ctl.send('agentSpace.list', {})).windows[0]
await ctl.send('agentSpace.window.setBounds', { windowId: win, x: 69, y: 40, width: 1400, height: 900 })
const tabs = async () => (await ctl.send('agentSpace.tabs.list', { windowId: win })).tabs || []
const brief = (t) => ({ id: t.id, idx: t.index, url: (t.url || '').slice(0, 40), split: t.splitId, active: t.active, tag: t.tag })
say('before', JSON.stringify((await tabs()).map(brief)))
const a = await ctl.send('agentSpace.tabs.new', { windowId: win, url: 'chrome://newtab' })
await sleep(700)
const b = await ctl.send('agentSpace.tabs.new', { windowId: win, url: 'chrome://newtab' })
await sleep(700)
say('new a', JSON.stringify(a), 'new b', JSON.stringify(b))
say('after new', JSON.stringify((await tabs()).map(brief)))
const s = await ctl.send('agentSpace.splitView.create', { windowId: win, primaryTabId: a.id ?? a.tabId, secondaryTabId: b.id ?? b.tabId, layout: 'vertical' })
say('split', JSON.stringify(s))
await sleep(1200)
say('after split', JSON.stringify((await tabs()).map(brief)))
say('splits', JSON.stringify(await ctl.send('agentSpace.splitView.list', { windowId: win })))
const l = await ctl.send('agentSpace.window.layout', { windowId: win })
say('layout', JSON.stringify({ mode: l.mode, content: l.content, sidebar: l.sidebar, ntp: l.ntp, keys: Object.keys(l) }))
ctl.close()
process.exit(0)
