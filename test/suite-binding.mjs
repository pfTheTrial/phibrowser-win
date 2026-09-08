// Live stored-row bindings (the Mac's openOrFocusPinnedTab/bookmarkGuidBacking
// via identity TAGS): a pin or sidebar bookmark opens as a tab CARRYING the
// row's guid as its tag (PhiTabData, session-persisted). Opening again focuses
// instead of duplicating; removing the row clears the tag and releases the tab.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'
import { firstWindow } from './lib/window.mjs'
import { closeMatching } from './lib/window.mjs'

const tabs = async (ctl, windowId) =>
  (await ctl.send('agentSpace.tabs.list', { windowId })).tabs

const closeByUrl = (ctl, windowId, marker) =>
  closeMatching(ctl, windowId, (t) => (t.url || '').includes(marker),
                { keep: 0 })

test('binding: opening a pin creates ONE tagged tab, reopening focuses it', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const pin = await ctl.send('agentSpace.pinned.add',
        { windowId, title: 'BindSuite', url: 'https://example.com/bindsuite' })
    assertEq(pin.ok, true, 'pin added')
    const before = (await tabs(ctl, windowId)).length
    assertEq((await ctl.send('agentSpace.pinned.open', { windowId, guid: pin.guid })).ok,
             true, 'first open ok')
    await sleep(900)
    const afterFirst = await tabs(ctl, windowId)
    assertEq(afterFirst.length, before + 1, 'one tab created')
    const bound = afterFirst.find((t) => t.tag === pin.guid)
    assert(bound, 'the new tab carries the pin guid as its tag')
    assertEq((await ctl.send('agentSpace.pinned.open', { windowId, guid: pin.guid })).ok,
             true, 'second open ok')
    await sleep(500)
    assertEq((await tabs(ctl, windowId)).length, before + 1,
             'second open focused, did not duplicate')
    await ctl.send('agentSpace.pinned.remove', { guid: pin.guid })
    await sleep(400)
    await closeByUrl(ctl, windowId, 'bindsuite')
  } finally { ctl.close() }
})

test('binding: removing the row clears the tag and releases the tab', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const bm = await ctl.send('agentSpace.bookmarks.add',
        { windowId, title: 'BindBm', url: 'https://example.com/bindbm' })
    assertEq(bm.ok, true, 'bookmark added')
    await ctl.send('agentSpace.pinned.open', { windowId, guid: bm.guid })
    await sleep(900)
    assert((await tabs(ctl, windowId)).some((t) => t.tag === bm.guid), 'bound')
    assertEq((await ctl.send('agentSpace.pinned.remove', { guid: bm.guid })).ok,
             true, 'row removed')
    await sleep(500)
    const after = await tabs(ctl, windowId)
    assert(!after.some((t) => t.tag === bm.guid), 'tag cleared on removal')
    assert(after.some((t) => (t.url || '').includes('bindbm')),
           'the tab itself stays open as a normal tab')
    await closeByUrl(ctl, windowId, 'bindbm')
  } finally { ctl.close() }
})

test('binding: unknown guid is refused', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    assertEq((await ctl.send('agentSpace.pinned.open',
                             { windowId, guid: 'no-such-guid' })).ok,
             false, 'refused')
  } finally { ctl.close() }
})

run()
