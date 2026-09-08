// Reader View as an OVERLAY: the surface must leave the origin's strip and
// come back as a panel over its page, the way the Mac hosts it.
//
// What each step proves:
//   1 opening leaves the origin window with exactly its own tab — the surface
//     is no longer a second row in the strip;
//   2 the surface is nevertheless live and rendering (a CDP page target on
//     reader.html), i.e. it moved rather than died;
//   3 the client knows the origin is in Reader View (readerActive);
//   4 toggling again takes the panel and its page away together;
//   5 the article underneath was never navigated — leaving the reader reveals
//     the same page, it does not reload it.
import { openControl, cdpHttp, sleep } from '../phi-test.mjs'

const ARTICLE = 'https://en.wikipedia.org/wiki/Readability'
const READER_PREFIX = 'chrome-extension://ickhcgejficcoofnjnnobadfdnfbilnm/reader.html'

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const log = (...a) => console.log(...a)

const tabs = async () =>
  (await ctl.send('agentSpace.tabs.list', { windowId: w })).tabs || []
const layout = async () => ctl.send('agentSpace.window.layout', { windowId: w })
const readerTargets = async () =>
  (await cdpHttp('/json/list')).filter((t) => (t.url || '').startsWith(READER_PREFIX))

// Every checkpoint polls: the extraction, the tab move and the panel teardown
// all take as long as they take, and a fixed sleep turns a slow run into a
// phantom failure.
async function waitFor(what, predicate, tries = 25) {
  for (let i = 0; i < tries; i++) {
    if (await predicate()) return true
    await sleep(600)
  }
  console.log(`  (timed out waiting for ${what})`)
  return false
}

// The menu item is a TOGGLE, so every run must start from a known state or
// each step does the opposite of what it says.
async function ensureClosed() {
  if ((await ctl.send('agentSpace.window.layout', { windowId: w })).readerActive) {
    await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
    await sleep(3000)
  }
}
await ensureClosed()

let list = await tabs()
while (list.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: list.length - 1 })
  await sleep(300); list = await tabs()
}
await ctl.send('agentSpace.navigate', { windowId: w, url: ARTICLE })
await sleep(6000)
log('before:', JSON.stringify((await tabs()).map((t) => t.url.slice(0, 60))))

await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
// Extraction takes as long as it takes — a fixed sleep samples too early on a
// slow run and the whole suite then reads one step out of phase.
await waitFor('the reader to mount', async () => (await layout()).readerActive)
await waitFor('its page target', async () => (await readerTargets()).length > 0)

log('1 strip after open:', JSON.stringify((await tabs()).map((t) => t.url.slice(0, 60))))
log('2 reader page targets:', JSON.stringify((await readerTargets()).map((t) => t.title)))
const l = await layout()
log('3 readerActive', l.readerActive, '| windows still', JSON.stringify(
    (await ctl.send('agentSpace.list')).windows))

await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
await waitFor('the reader to go', async () => !(await layout()).readerActive)
log('4 readerActive after close', (await layout()).readerActive,
    '| reader targets', (await readerTargets()).length)
log('5 strip after close:', JSON.stringify((await tabs()).map((t) => t.url.slice(0, 60))))

ctl.close()
process.exit(0)
