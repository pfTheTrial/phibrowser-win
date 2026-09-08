// The reader HUD's "Reset to Defaults", through the client: the page writes
// the default style back over reader.setStyle and the client's store returns
// to what a fresh install reads at. Also leaves the box tidy after a style run.
import { openControl, cdpHttp, openCdp, sleep } from '../phi-test.mjs'
import { readFileSync } from 'node:fs'

const PREFS = 'C:\\phi-fx-test\\PhiPrefs.json'
const READER_PREFIX = 'chrome-extension://ickhcgejficcoofnjnnobadfdnfbilnm/reader.html'
const readerPrefs = () => {
  try { return JSON.parse(readFileSync(PREFS, 'utf8')).reader ?? null }
  catch (e) { return `unreadable: ${e.message}` }
}

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]

let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
while (tabs.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
  await sleep(300); ({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
}
await ctl.send('agentSpace.navigate', { windowId: w, url: 'https://en.wikipedia.org/wiki/Readability' })
await sleep(5000)
await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
await sleep(6000)

const targets = await cdpHttp('/json/list')
const page = targets.find((t) => (t.url || '').startsWith(READER_PREFIX))
if (!page) { console.log('FAIL: no reader page'); process.exit(1) }
const cdp = await openCdp(page.webSocketDebuggerUrl.replace(/^ws:\/\/[^/]+/, ''))
const evaluate = async (expression) => (await cdp.send('Runtime.evaluate',
    { expression, returnByValue: true, awaitPromise: true })).result?.value

console.log('prefs before reset:', JSON.stringify(readerPrefs()))
console.log('reset:', JSON.stringify(await evaluate(`(() => {
  const aa = [...document.querySelectorAll('.phi-hud *')]
      .find(e => (e.getAttribute('aria-label') || '') === 'Reading Style')
  if (!aa) return 'no Aa button'
  aa.click()
  const row = [...document.querySelectorAll('.phi-menu-item')]
      .find(r => /Reset to Defaults/.test(r.textContent))
  if (!row) return 'no reset row'
  row.click()
  return 'clicked'
})()`)))
await sleep(1500)
console.log('prefs after reset:', JSON.stringify(readerPrefs()))

cdp.close()
await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
await sleep(2000)
ctl.close()
process.exit(0)
