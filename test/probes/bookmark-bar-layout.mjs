// The bookmark bar is a comfortable-layout feature (Mac: bookmarkBarSupported
// = traditionalLayout && ...). The two View items exist only in comfortable,
// and a bar the prefs would show is NOT laid out in the vertical layouts.
import { test, run, assert, assertEq, sleep, openControl } from '../phi-test.mjs'
import { firstWindow } from '../lib/window.mjs'

const BAR_DESIGN = 32  // kBookmarkBarHeight
const scaled = (design, dpi) => Math.round((design * dpi) / 96)
const ALWAYS = 'Always Show Bookmark Bar'
const ON_NTP = 'Show Bookmark Bar on New Tab'

async function setLayout(ctl, windowId, mode) {
  const r = await ctl.send('agentSpace.layout.set', { mode })
  assertEq(r.ok, true, `layout.set ${mode}`)
  await sleep(400)
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  assertEq(l.mode, mode, `window reports ${mode}`)
  return l
}

async function viewMenu(ctl, windowId) {
  const r = await ctl.send('agentSpace.menu.list', { windowId })
  assertEq(r.ok, true, 'menu.list ok')
  return r.menus.find((m) => m.title === 'View')
}

const find = (menu, title) => menu.items.find((i) => i.title === title)
const doubledSeparator = (menu) =>
  menu.items.some((it, i) => it.separator && i > 0 && menu.items[i - 1].separator)

test('bookmark bar: View items exist only in comfortable', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    for (const mode of ['performance', 'balanced']) {
      await setLayout(ctl, windowId, mode)
      const view = await viewMenu(ctl, windowId)
      assertEq(find(view, ALWAYS), undefined, `${mode}: no "${ALWAYS}"`)
      assertEq(find(view, ON_NTP), undefined, `${mode}: no "${ON_NTP}"`)
      assertEq(doubledSeparator(view), false, `${mode}: separators collapse`)
      assert(find(view, 'Toggle Sidebar'), `${mode}: Toggle Sidebar still there`)
    }
    await setLayout(ctl, windowId, 'comfortable')
    const view = await viewMenu(ctl, windowId)
    assert(find(view, ALWAYS), `comfortable: "${ALWAYS}" back`)
    assert(find(view, ON_NTP), `comfortable: "${ON_NTP}" back`)
    assertEq(doubledSeparator(view), false, 'comfortable: separators single')
    const idx = view.items.findIndex((i) => i.title === ALWAYS)
    assertEq(view.items[idx - 1].separator, true, 'separator before the pair')
    assertEq(view.items[idx + 1].title, ON_NTP, 'pair adjacent')
    assertEq(view.items[idx + 2].separator, true, 'separator after the pair')
    assertEq(view.items[idx + 3].title, 'Toggle Sidebar', 'then Toggle Sidebar')
  } finally { ctl.close() }
})

test('bookmark bar: the bar shows in comfortable only, whatever the pref', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const perf0 = (await setLayout(ctl, windowId, 'performance')).content.y
    const bal0 = (await setLayout(ctl, windowId, 'balanced')).content.y
    const comfy = await setLayout(ctl, windowId, 'comfortable')
    const bar = scaled(BAR_DESIGN, comfy.dpi)
    let view = await viewMenu(ctl, windowId)
    assertEq(find(view, ALWAYS).checked, false, 'pref starts off')
    const click = await ctl.send('agentSpace.menu.click',
                                 { windowId, path: ['View', ALWAYS] })
    assertEq(click.ok, true, 'Always Show clicked')
    await sleep(400)
    view = await viewMenu(ctl, windowId)
    assertEq(find(view, ALWAYS).checked, true, 'pref now on (ticked)')
    const comfy1 = (await ctl.send('agentSpace.window.layout', { windowId })).content.y
    assertEq(comfy1, comfy.content.y + bar, 'comfortable: bar carved off the content')

    const bal1 = (await setLayout(ctl, windowId, 'balanced')).content.y
    assertEq(bal1, bal0, 'balanced: no bar although the pref is on')
    const perf1 = (await setLayout(ctl, windowId, 'performance')).content.y
    assertEq(perf1, perf0, 'performance: no bar although the pref is on')

    const back = await setLayout(ctl, windowId, 'comfortable')
    assertEq(back.content.y, comfy1, 'comfortable: bar returns with the mode')
    view = await viewMenu(ctl, windowId)
    assertEq(find(view, ALWAYS).checked, true, 'still ticked after the round trip')
    assertEq((await ctl.send('agentSpace.menu.click',
                             { windowId, path: ['View', ALWAYS] })).ok, true,
             'Always Show clicked off')
    await sleep(400)
    const restored = (await ctl.send('agentSpace.window.layout', { windowId })).content.y
    assertEq(restored, comfy.content.y, 'bar gone again')
    await setLayout(ctl, windowId, 'performance')
  } finally { ctl.close() }
})

await run()
