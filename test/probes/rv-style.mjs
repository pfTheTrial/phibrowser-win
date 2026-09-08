// The reading style round-trip: the reader page reads it from the client
// (reader.getStyle) and writes changes back (reader.setStyle), which the
// client persists in PhiPrefs.json. Drives the reader HUD's own buttons over
// CDP so nothing here fakes the message the page would send.
import { openControl, cdpHttp, openCdp, sleep } from '../phi-test.mjs'
import { readFileSync } from 'node:fs'

const ARTICLE = 'https://en.wikipedia.org/wiki/Readability'
const PREFS = 'C:\\phi-fx-test\\PhiPrefs.json'
const READER_PREFIX = 'chrome-extension://ickhcgejficcoofnjnnobadfdnfbilnm/reader.html'

const readerPrefs = () => {
  try { return JSON.parse(readFileSync(PREFS, 'utf8')).reader ?? null }
  catch (e) { return `unreadable: ${e.message}` }
}

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]

console.log('reader prefs before:', JSON.stringify(readerPrefs()))

let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
while (tabs.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
  await sleep(300); ({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
}
await ctl.send('agentSpace.navigate', { windowId: w, url: ARTICLE })
await sleep(4000)
await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
await sleep(6000)

const targets = await cdpHttp('/json/list')
const page = targets.find((t) => (t.url || '').startsWith(READER_PREFIX))
if (!page) { console.log('FAIL: no reader page target'); process.exit(1) }
console.log('reader target:', page.title, '|', page.url.slice(0, 80))

const cdp = await openCdp(page.webSocketDebuggerUrl.replace(/^ws:\/\/[^/]+/, ''))
const evaluate = async (expression) => {
  const r = await cdp.send('Runtime.evaluate', { expression, returnByValue: true, awaitPromise: true })
  return r.result?.value
}

// What the page is rendering right now — it asked the client for this.
console.log('page font-size / family:', JSON.stringify(await evaluate(`(() => {
  const s = getComputedStyle(document.documentElement)
  return { fontSize: s.fontSize, family: s.fontFamily.slice(0, 30) }
})()`)))

// The HUD's own A+ button, twice: 19 -> 23 on the step-2 grid.
console.log('A+ clicks:', JSON.stringify(await evaluate(`(() => {
  const b = [...document.querySelectorAll('button,[role=button],.phi-hud *')]
      .find(e => (e.getAttribute('aria-label') || '') === 'Increase Text Size')
  if (!b) return 'no A+ button'
  b.click(); b.click()
  return 'clicked'
})()`)))
await sleep(1500)
console.log('page font-size after:', JSON.stringify(await evaluate(
    `getComputedStyle(document.documentElement).fontSize`)))
console.log('reader prefs after size:', JSON.stringify(readerPrefs()))

// A second axis through the Aa menu: Reading Style > Theme > Sepia.
console.log('theme pick:', JSON.stringify(await evaluate(`(() => {
  const aa = [...document.querySelectorAll('.phi-hud *')]
      .find(e => (e.getAttribute('aria-label') || '') === 'Reading Style')
  if (!aa) return 'no Aa button'
  aa.click()
  const rows = [...document.querySelectorAll('.phi-menu-item')]
  const sepia = rows.find(r => /^Sepia$/i.test(r.textContent.trim()))
  if (!sepia) return 'no Sepia row; rows: ' + rows.map(r => r.textContent.trim()).join('|').slice(0, 200)
  sepia.click()
  return 'clicked'
})()`)))
await sleep(1500)
console.log('reader prefs after theme:', JSON.stringify(readerPrefs()))

cdp.close(); ctl.close()
process.exit(0)
