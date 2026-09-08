// Shadow window round 2: does the relay's /json/list survive a shadow tab,
// and does the shadow tab COMPOSITE (Page.captureScreenshot answers)?
import { openControl, sleep, cdpHttp, openCdp } from './phi-test.mjs'
const ctl = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v).slice(0, 300))
const timed = async (label, fn) => { const t = Date.now(); try { const r = await fn(); console.log(label, 'ok', Date.now() - t, 'ms'); return r } catch (e) { console.log(label, 'FAIL', Date.now() - t, 'ms', String(e).slice(0, 120)); return null } }
await timed('/json/list before', () => cdpHttp('/json/list'))
const r = await ctl.send('agentSpace.shadow.create', { taskId: 'sv-shadow-2', incognito: false })
show('create:', r)
await sleep(800)
let list = await timed('/json/list after create', () => cdpHttp('/json/list'))
console.log('targets:', (list || []).map(t => t.type + ':' + t.url.slice(0, 40)).join(' | '))
show('openTab:', await ctl.send('agentSpace.shadow.openTab', { taskId: 'sv-shadow-2', url: 'https://example.com/?shadow2' }))
await sleep(2500)
list = await timed('/json/list after openTab', () => cdpHttp('/json/list'))
console.log('targets:', (list || []).map(t => t.type + ':' + t.url.slice(0, 40)).join(' | '))
const page = (list || []).find(t => t.type === 'page' && /shadow2/.test(t.url))
if (page) {
  const cdp = await openCdp(new URL(page.webSocketDebuggerUrl).pathname)
  const win = await timed('Browser.getWindowForTarget', () => cdp.send('Browser.getWindowForTarget', { targetId: page.id }))
  console.log('windowId match:', JSON.stringify(win).slice(0, 120), 'expect', r.windowId)
  const shot = await Promise.race([
    timed('Page.captureScreenshot', () => cdp.send('Page.captureScreenshot', { format: 'png' })),
    sleep(12000).then(() => { console.log('Page.captureScreenshot TIMED OUT (12s)'); return null }),
  ])
  if (shot) console.log('screenshot bytes:', ((shot.result || shot).data || '').length)
  const ev = await timed('Runtime.evaluate', () => cdp.send('Runtime.evaluate', { expression: 'document.title', returnByValue: true }))
  show('title:', ev)
  try { cdp.close() } catch {}
} else {
  console.log('NO SHADOW PAGE TARGET')
}
show('close:', await ctl.send('agentSpace.shadow.close', { taskId: 'sv-shadow-2' }))
await sleep(1000)
show('list after close:', await ctl.send('agentSpace.shadow.list', {}))
ctl.close()
