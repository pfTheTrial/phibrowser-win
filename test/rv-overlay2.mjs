// The rules AROUND the overlay, the ones the Mac spells out:
//   1 a link clicked inside the reader leaves the reading surface and becomes
//     a normal tab in the origin window;
//   2 the origin closing takes its reader with it;
//   3 Back reads as "leave the reader", not as a navigation under it;
//   4 an address-bar navigation on the origin leaves the reader too, or what
//     arrives would load unseen behind it;
//   5 two origins can each carry their own reader.
import { openControl, cdpHttp, openCdp, sleep } from './phi-test.mjs'

const ARTICLE = 'https://en.wikipedia.org/wiki/Readability'
const ARTICLE2 = 'https://en.wikipedia.org/wiki/Domain_Name_System'
const READER_PREFIX = 'chrome-extension://ickhcgejficcoofnjnnobadfdnfbilnm/reader.html'

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const log = (...a) => console.log(...a)

const tabs = async () =>
  (await ctl.send('agentSpace.tabs.list', { windowId: w })).tabs || []
const urls = async () => (await tabs()).map((t) => t.url.slice(0, 55))
const layout = async () => ctl.send('agentSpace.window.layout', { windowId: w })
const readerTargets = async () =>
  (await cdpHttp('/json/list')).filter((t) => (t.url || '').startsWith(READER_PREFIX))

// The menu item is a TOGGLE, so every run must start from a known state or
// each step does the opposite of what it says.
async function ensureClosed() {
  if ((await ctl.send('agentSpace.window.layout', { windowId: w })).readerActive) {
    await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
    await sleep(3000)
  }
}
await ensureClosed()

async function reset(url = ARTICLE) {
  let list = await tabs()
  while (list.length > 1) {
    await ctl.send('agentSpace.tabs.close', { windowId: w, index: list.length - 1 })
    await sleep(300); list = await tabs()
  }
  await ctl.send('agentSpace.navigate', { windowId: w, url })
  await sleep(6000)
}
const openReader = async () => {
  await ensureClosed()
  await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
  // Poll rather than sleep: extraction takes as long as it takes, and a fixed
  // wait samples too early on a slow run.
  for (let i = 0; i < 25 && !(await layout()).readerActive; i++) await sleep(600)
}

// --- 1. link click inside the reader ---------------------------------------
await reset()
await openReader()
let target = (await readerTargets())[0]
if (!target) { log('FAIL: no reader surface'); process.exit(1) }
let cdp = await openCdp(target.webSocketDebuggerUrl.replace(/^ws:\/\/[^/]+/, ''))
log('1 clicking a link inside the reader:', JSON.stringify(
    (await cdp.send('Runtime.evaluate', {
      expression: `(() => {
        const a = [...document.querySelectorAll('#phi-article a[href^="http"]')]
            .find(x => !x.href.includes('#'))
        if (!a) return 'no link'
        const href = a.href
        a.click()
        return href
      })()`, returnByValue: true })).result?.value))
cdp.close()
await sleep(6000)
log('1 strip:', JSON.stringify(await urls()), '| readerActive',
    (await layout()).readerActive, '| surfaces', (await readerTargets()).length)

// --- 2. closing the origin takes its reader ---------------------------------
await reset()
await openReader()
log('2 before:', (await readerTargets()).length, 'surface(s)')
await ctl.send('agentSpace.tabs.close', { windowId: w, index: 0 })
await sleep(4000)
log('2 after closing the origin:', (await readerTargets()).length, 'surface(s), strip',
    JSON.stringify(await urls()))

// --- 3. Back leaves the reader ---------------------------------------------
await reset()
await ctl.send('agentSpace.navigate', { windowId: w, url: ARTICLE2 })
await sleep(6000)
await openReader()
log('3 reader up:', (await layout()).readerActive)
await ctl.send('agentSpace.menu.click', { windowId: w, path: ['History', 'Back'] })
await sleep(3000)
const afterBack = await layout()
log('3 after Back: readerActive', afterBack.readerActive, '| url',
    JSON.stringify((await urls())[0]), '(the page must NOT have gone back yet)')

// --- 4. address-bar navigation ----------------------------------------------
await reset()
await openReader()
log('4 reader up:', (await layout()).readerActive)
await ctl.send('agentSpace.navigate', { windowId: w, url: 'https://example.com/' })
await sleep(4000)
log('4 after navigating:', 'readerActive', (await layout()).readerActive,
    '| surfaces', (await readerTargets()).length, '| strip', JSON.stringify(await urls()))

// --- 5. two origins, two readers -------------------------------------------
await reset()
await openReader()
await ctl.send('agentSpace.tabs.new', { windowId: w, url: ARTICLE2 })
await sleep(6000)
await openReader()
log('5 surfaces:', (await readerTargets()).map((t) => t.title))
log('5 strip:', JSON.stringify(await urls()))

ctl.close()
process.exit(0)
