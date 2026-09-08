import { openControl, sleep, cdpHttp, openCdp } from './phi-test.mjs'
const ctl = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v).slice(0, 300))
const c = await ctl.send('agentSpace.create', { taskId: 'sv-zombie', agentName: 'sv' })
show('create', c)
await sleep(1500)
const targets = (await cdpHttp('/json/list')).filter(t => t.type === 'page')
const cdp = await openCdp(new URL(targets[0].webSocketDebuggerUrl).pathname)
const mine = []
for (const t of targets) {
  const w = await cdp.send('Browser.getWindowForTarget', { targetId: t.id })
  const wid = (w.result || w).windowId
  if (wid === c.windowId) mine.push(t)
}
show('targets in agent window', mine.map(t => t.url))
for (const t of mine) show('closeTarget', await cdp.send('Target.closeTarget', { targetId: t.id }))
await sleep(1500)
const { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: c.windowId })
show('tabs after closing all', tabs.map(t => t.url))
show('windows', (await ctl.send('agentSpace.list', {})).windows)
const c2 = await ctl.send('agentSpace.create', { taskId: 'sv-zombie', agentName: 'sv' })
show('re-create', c2)
console.log('healed with new space:', c2.spaceId !== c.spaceId, 'new window:', c2.windowId !== c.windowId)
await sleep(800)
const t2 = await ctl.send('agentSpace.tabs.list', { windowId: c2.windowId })
show('tabs in healed window', (t2.tabs || []).map(t => t.url))
show('complete', await ctl.send('agentSpace.complete', { taskId: 'sv-zombie', status: 'success' }))
try { cdp.close() } catch {}
ctl.close(); process.exit(0)
