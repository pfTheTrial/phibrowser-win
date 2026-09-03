// Second half of the style round-trip: a STRING axis through the HUD menu,
// then a fresh reader surface, which must come up wearing what the client
// stored (reader.getStyle at mount) rather than the extension's own defaults.
import { openControl, cdpHttp, openCdp, sleep } from './phi-test.mjs'
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

async function readerPage() {
  const targets = await cdpHttp('/json/list')
  const page = targets.find((t) => (t.url || '').startsWith(READER_PREFIX))
  if (!page) return null
  const cdp = await openCdp(page.webSocketDebuggerUrl.replace(/^ws:\/\/[^/]+/, ''))
  return {
    cdp,
    async eval(expression) {
      const r = await cdp.send('Runtime.evaluate',
          { expression, returnByValue: true, awaitPromise: true })
      return r.result?.value
    },
  }
}

// The article's own rendered look, which is what the style actually controls.
const LOOK = `(() => {
  const a = document.querySelector('#phi-article') || document.body
  const s = getComputedStyle(a)
  return { fontSize: s.fontSize, family: s.fontFamily.slice(0, 24),
           bg: getComputedStyle(document.body).backgroundColor }
})()`

let page = await readerPage()
if (!page) { console.log('FAIL: no reader page open — run rv-style.mjs first'); process.exit(1) }
console.log('look now:', JSON.stringify(await page.eval(LOOK)))

// Reading Style ▸ Background ▸ Sepia. Rows carry a leading check span, so
// match on containing the label rather than equalling it.
console.log('sepia:', JSON.stringify(await page.eval(`(() => {
  const aa = [...document.querySelectorAll('.phi-hud *')]
      .find(e => (e.getAttribute('aria-label') || '') === 'Reading Style')
  if (!aa) return 'no Aa button'
  aa.click()
  const row = [...document.querySelectorAll('.phi-submenu .phi-menu-item')]
      .find(r => r.textContent.replace(/[^A-Za-z ]/g, '').trim() === 'Sepia')
  if (!row) return 'no Sepia row'
  row.click()
  return 'clicked'
})()`)))
await sleep(1500)
console.log('look after sepia:', JSON.stringify(await page.eval(LOOK)))
console.log('prefs after sepia:', JSON.stringify(readerPrefs()))
page.cdp.close()

// Close the surface and open a new one: it must mount wearing the stored
// style, which it can only know by asking the client for it.
await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
await sleep(2500)
await ctl.send('agentSpace.navigate', { windowId: w, url: ARTICLE })
await sleep(4000)
await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
await sleep(6000)
page = await readerPage()
if (!page) { console.log('FAIL: second reader surface never opened'); process.exit(1) }
console.log('FRESH surface look:', JSON.stringify(await page.eval(LOOK)))
page.cdp.close()
ctl.close()
process.exit(0)
