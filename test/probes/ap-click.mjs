// Auto-Peek from a synthetic renderer click. CDP input does NOT go through the
// Windows message queue, so this exercises the navigation path WITHOUT the
// client's WH_MOUSE click-outside monitor — which is the discriminator: if the
// peek survives here but dies under a real click, the monitor is the cause.
import { appendFileSync } from 'node:fs'
import { openControl, cdpHttp, openCdp, sleep } from '../phi-test.mjs'
const LOG = 'C:\\Users\\jdxin\\phitest\\ap-click.log'
const say = (...a) => appendFileSync(LOG, a.join(' ') + '\n')
try {
  say('start')
  const ctl = await openControl()
  const ianas = async () => (await cdpHttp('/json/list'))
    .filter(t => t.type === 'page' && /iana\.org/.test(t.url)).length
  const win = (await ctl.send('agentSpace.list', {})).windows[0]
  const pins = (await ctl.send('agentSpace.pinned.list', { windowId: win })).pinned || []
  const pin = pins.find(p => (p.url || '').includes('example.com'))
  say('pin', JSON.stringify(pin))
  await ctl.send('agentSpace.pinned.open', { windowId: win, guid: pin.guid })
  await sleep(3000)
  const page = (await cdpHttp('/json/list'))
    .find(t => t.type === 'page' && t.url === 'https://example.com/')
  say('page', JSON.stringify(page && page.webSocketDebuggerUrl))
  const cdp = await openCdp(new URL(page.webSocketDebuggerUrl).pathname)
  say('cdp attached')
  const box = await cdp.send('Runtime.evaluate', {
    expression: `(() => { const a = document.querySelector('a[href*="iana.org"]');
      const r = a.getBoundingClientRect();
      return JSON.stringify({x: r.x + r.width/2, y: r.y + r.height/2}); })()`,
    returnByValue: true,
  })
  const at = JSON.parse(box.result.value)
  say('link at', JSON.stringify(at), 'ianas before', await ianas())
  for (const type of ['mousePressed', 'mouseReleased']) {
    await cdp.send('Input.dispatchMouseEvent',
      { type, x: Math.round(at.x), y: Math.round(at.y), button: 'left', clickCount: 1 })
  }
  say('clicked')
  for (let i = 1; i <= 8; i++) { await sleep(1000); say(`ianas @${i}s`, await ianas()) }
  ctl.close()
  say('done')
} catch (e) { say('ERROR ' + (e && e.stack || e)) }
process.exit(0)
