// Reader View end-to-end, through the client's own control channel.
//
// What each step proves, in the order the bridge has to work:
//   1 the View menu carries the item at all (client build is the new one);
//   2 clicking it broadcasts reader.open, the Mirage extension extracts and
//     creates its surface tab — app -> extension direction;
//   3 the item comes back TICKED, which only happens once reader.state has
//     arrived and been believed — extension -> app direction;
//   4 clicking again closes the surface;
//   5 a page with no article produces no surface tab and no tick (the refusal
//     path; its toast needs a screenshot to see).
import { openControl, sleep } from './phi-test.mjs'

const ARTICLE = 'https://en.wikipedia.org/wiki/Readability'
const NOT_AN_ARTICLE = 'https://example.com/'
const READER_PREFIX = 'chrome-extension://ickhcgejficcoofnjnnobadfdnfbilnm/reader.html'

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const log = (...a) => console.log(...a)

async function viewItem() {
  const { menus, ok } = await ctl.send('agentSpace.menu.list', { windowId: w })
  if (!ok) return null
  const view = (menus || []).find((m) => /View/i.test(m.title))
  if (!view) return null
  return (view.items || []).find((i) => /Reader/i.test(i.title || '')) || null
}

async function tabs() {
  const { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
  return tabs || []
}

async function readerTabs() {
  return (await tabs()).filter((t) => (t.url || '').startsWith(READER_PREFIX))
}

async function goto(url) {
  await ctl.send('agentSpace.navigate', { windowId: w, url })
  await sleep(4000)
}

async function closeExtraTabs() {
  let list = await tabs()
  while (list.length > 1) {
    await ctl.send('agentSpace.tabs.close', { windowId: w, index: list.length - 1 })
    await sleep(300)
    list = await tabs()
  }
}

// --- 1. the item exists -----------------------------------------------------
await closeExtraTabs()
await goto(ARTICLE)
let item = await viewItem()
log('1 menu item:', JSON.stringify(item))
if (!item) { log('FAIL: no Reader View item in the View menu'); process.exit(1) }

// --- 2. open ----------------------------------------------------------------
log('2 clicking...', JSON.stringify(await ctl.send('agentSpace.menu.click',
    { windowId: w, path: ['View', item.title.replace(/\t.*$/, '').replace(/&/g, '')] })))
await sleep(6000)
let surfaces = await readerTabs()
log('2 surface tabs:', JSON.stringify(surfaces.map((t) => t.url)))
log('2 all tabs:', JSON.stringify((await tabs()).map((t) => t.url.slice(0, 90))))

// --- 3. the tick, i.e. reader.state came back -------------------------------
item = await viewItem()
log('3 item after open:', JSON.stringify(item))

// --- 4. close ---------------------------------------------------------------
if (surfaces.length) {
  await ctl.send('agentSpace.menu.click',
      { windowId: w, path: ['View', item.title.replace(/\t.*$/, '').replace(/&/g, '')] })
  await sleep(3000)
  log('4 surface tabs after close:', JSON.stringify((await readerTabs()).map((t) => t.url)))
  log('4 item after close:', JSON.stringify(await viewItem()))
}

// --- 5. refusal -------------------------------------------------------------
await closeExtraTabs()
await goto(NOT_AN_ARTICLE)
const before = (await tabs()).length
item = await viewItem()
log('5 item on a non-article:', JSON.stringify(item))
await ctl.send('agentSpace.menu.click',
    { windowId: w, path: ['View', item.title.replace(/\t.*$/, '').replace(/&/g, '')] })
await sleep(6000)
log('5 surface tabs:', JSON.stringify((await readerTabs()).map((t) => t.url)),
    'tab count', before, '->', (await tabs()).length)
log('5 item after refusal:', JSON.stringify(await viewItem()))

ctl.close()
process.exit(0)
