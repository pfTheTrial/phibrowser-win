// Spaces suite: Spaces are a CLIENT concept (as on Mac) — a named workspace
// with its own Browser (own tab set). A slot shows one at a time; switching
// swaps which browser is visible in the shell window.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'
import { firstWindow } from './lib/window.mjs'
import { waitWindows, waitSpaces } from './lib/wait.mjs'

test('spaces: list starts with the default space, active', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const r = await ctl.send('agentSpace.spaces.list', { windowId })
    assertEq(r.ok, true, 'list ok')
    assert(r.spaces.length >= 1, 'at least one space')
    const active = r.spaces.filter((s) => s.active)
    assertEq(active.length, 1, 'exactly one active space')
    assertEq(active[0].open, true, 'the active space is open in this slot')
    for (const k of ['id', 'name', 'active', 'open']) {
      assert(k in r.spaces[0], `space has ${k}`)
    }
  } finally { ctl.close() }
})

test('spaces: create adds a space (not yet open in the slot)', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const before = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    const created = await ctl.send('agentSpace.spaces.create', { name: 'Work' })
    assertEq(created.ok, true, 'create ok')
    assertEq(created.name, 'Work', 'name echoed')
    const after = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    assertEq(after.length, before.length + 1, 'one more space')
    const mine = after.find((s) => s.id === created.id)
    assert(mine, 'created space is listed')
    assertEq(mine.open, false, 'not open until switched to')
    assertEq(mine.active, false, 'not active until switched to')
  } finally { ctl.close() }
})

test('spaces: switching opens the space and isolates its tabs', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    // Mark the current space's page so we can tell the two apart.
    const homeMark = `home-${Date.now()}`
    await ctl.send('agentSpace.navigate',
                   { windowId, url: `data:text/html,<title>${homeMark}</title>` })
    await sleep(600)
    const homeTabs = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs

    const created = await ctl.send('agentSpace.spaces.create', { name: 'Research' })
    const sw = await ctl.send('agentSpace.spaces.switch',
                              { windowId, spaceId: created.id })
    assertEq(sw.ok, true, 'switch ok')
    await waitSpaces(ctl, windowId,
                     (sp) => sp.some((x) => x.id === created.id && x.active),
                     { timeout: 8000, what: 'the new Space to become active' })

    const listed = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    const now = listed.find((s) => s.id === created.id)
    assertEq(now.active, true, 'new space is active')
    assertEq(now.open, true, 'new space is open')

    // The new Space has its OWN tab set: none of the home space's pages.
    const spaceTabs = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
    assert(spaceTabs.length >= 1, 'new space has a tab')
    assert(!spaceTabs.some((t) => (t.title || '').includes(homeMark)),
           `new space does not show the home page (${JSON.stringify(spaceTabs.map((t) => t.title))})`)
    assert(homeTabs.some((t) => (t.title || '').includes(homeMark)),
           'home space did have the marked page')
  } finally { ctl.close() }
})

test('spaces: switching back restores the original space tabs', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const spaces = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    const home = spaces.find((s) => s.isDefault)  // the default ROLE, not id "1"
    assert(home, 'default space exists')

    // Put a distinctive page in the CURRENT (non-default) space.
    const otherMark = `other-${Date.now()}`
    await ctl.send('agentSpace.navigate',
                   { windowId, url: `data:text/html,<title>${otherMark}</title>` })
    await sleep(600)

    assertEq((await ctl.send('agentSpace.spaces.switch',
                             { windowId, spaceId: home.id })).ok, true, 'switch home ok')
    await waitSpaces(ctl, windowId,
                     (sp) => sp.some((x) => x.id === home.id && x.active),
                     { timeout: 8000, what: 'the home Space to become active' })
    const homeTabs = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
    assert(!homeTabs.some((t) => (t.title || '').includes(otherMark)),
           'home space does not show the other space page')

    const listed = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    assertEq(listed.find((s) => s.id === home.id).active, true, 'home is active again')
  } finally { ctl.close() }
})

test('spaces: a window that visited two spaces still closes', async () => {
  const ctl = await openControl()
  try {
    // A fresh slot, so closing it cannot disturb the window the other tests use.
    const before = (await ctl.send('agentSpace.list')).windows
    await ctl.send('agentSpace.newWindow')
    const windows = await waitWindows(ctl, (w) => w.length > before.length,
                                      { timeout: 6000,
                                        what: 'the new window to be listed' })
    const slot = windows.find((w) => !before.includes(w))
    assert(slot, 'new window created')

    // Visiting a second Space gives the slot a SECOND browser; closing only
    // the first would leave the window on screen (the bug this covers).
    const created = await ctl.send('agentSpace.spaces.create', { name: 'Closable' })
    assertEq((await ctl.send('agentSpace.spaces.switch',
                             { windowId: slot, spaceId: created.id })).ok,
             true, 'switched the new window to a second space')
    await waitSpaces(ctl, slot,
                     (sp) => sp.some((x) => x.id === created.id && x.active),
                     { timeout: 8000, what: 'the slot to enter the Space' })

    assertEq((await ctl.send('agentSpace.close', { windowId: slot })).ok, true,
             'close accepted')
    let live = windows
    try {
      live = await waitWindows(ctl, (w) => !w.includes(slot),
                               { timeout: 6000,
                                 what: 'the closed window to leave the list' })
    } catch { live = (await ctl.send('agentSpace.list')).windows }
    assert(!live.includes(slot),
           `the window went away (still listed: ${JSON.stringify(live)})`)
  } finally { ctl.close() }
})

test('spaces: switching to an unknown space is rejected', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const r = await ctl.send('agentSpace.spaces.switch',
                             { windowId, spaceId: 'no-such-space' })
    assertEq(r.ok, false, 'unknown space rejected')
  } finally { ctl.close() }
})

test('spaces: rename changes the name everywhere', async () => {
  const ctl = await openControl()
  try {
    const windowId = (await ctl.send('agentSpace.list')).windows[0]
    const created = await ctl.send('agentSpace.spaces.create',
                                   { name: 'Renamable' })
    assertEq(created.ok, true, 'space created')
    try {
      const renamed = await ctl.send('agentSpace.spaces.rename',
                                     { spaceId: created.id, name: 'Renamed' })
      assertEq(renamed.ok, true, 'rename accepted')
      const { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
      assertEq(spaces.find((s) => s.id === created.id).name, 'Renamed',
               'the list shows the new name')
      assertEq((await ctl.send('agentSpace.spaces.rename',
                               { spaceId: created.id, name: '' })).ok, false,
               'an empty name is refused')
    } finally {
      await ctl.send('agentSpace.spaces.delete', { spaceId: created.id })
    }
  } finally { ctl.close() }
})

test('spaces: delete removes the space, its data, and its browser', async () => {
  const ctl = await openControl()
  try {
    const windowId = (await ctl.send('agentSpace.list')).windows[0]
    const created = await ctl.send('agentSpace.spaces.create',
                                   { name: 'Doomed' })
    assertEq(created.ok, true, 'space created')
    // Open it in the slot so the delete also has a live browser to close.
    await ctl.send('agentSpace.spaces.switch',
                   { windowId, spaceId: created.id })
    try {
      await waitSpaces(ctl, windowId,
                       (sp) => sp.some((x) => x.id === created.id && x.active &&
                                              x.open),
                       { timeout: 10000,
                         what: 'the Space to open in the slot' })
    } catch { /* delete is asserted below either way */ }
    const gone = await ctl.send('agentSpace.spaces.delete',
                                { spaceId: created.id })
    assertEq(gone.ok, true, 'delete accepted')
    let spaces = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    assertEq(spaces.some((s) => s.id === created.id), false,
             'the space is gone from the list')
    // The window fell back to another open space rather than a dead one.
    try {
      spaces = await waitSpaces(ctl, windowId, (sp) => {
        const a = sp.find((x) => x.active)
        return a && a.id !== created.id
      }, { timeout: 5000, what: 'another Space to take over' })
    } catch { spaces = (await ctl.send('agentSpace.spaces.list',
                                       { windowId })).spaces }
    const active = spaces.find((s) => s.active)
    assert(active && active.id !== created.id,
           `another space is active (${active && active.id})`)
  } finally { ctl.close() }
})

test('spaces: only the LAST space refuses deletion; the default hands its role on', async () => {
  // The Mac's last-Space rule (139edbb2): everything is deletable except the
  // last remaining Space; deleting the default passes the role to the first
  // remaining one, persisted with the list.
  const ctl = await openControl()
  try {
    const windowId = (await ctl.send('agentSpace.list')).windows[0]
    const originalShell = await ctl.send('agentSpace.window.handle', { windowId })
    assertEq(originalShell.ok, true, 'original shell handle available')
    let { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
    const def = spaces.find((s) => s.isDefault)
    assert(def, 'a default space exists')
    const spare = await ctl.send('agentSpace.spaces.create', { name: 'Successor' })
    assertEq(spare.ok, true, 'spare created')
    const gone = await ctl.send('agentSpace.spaces.delete', { spaceId: def.id })
    assertEq(gone.ok, true, 'the default is deletable while another space exists')
    ;({ spaces } = await ctl.send('agentSpace.spaces.list', { windowId }))
    assertEq(spaces.some((s) => s.id === def.id), false, 'the old default is gone')
    const next = spaces.find((s) => s.isDefault)
    assert(next && next.id === spaces[0].id, 'the first remaining space holds the role')
    // Strip down to one, then the last must be refused.
    for (const s of spaces.slice(1)) {
      assertEq((await ctl.send('agentSpace.spaces.delete', { spaceId: s.id })).ok,
               true, `delete ${s.id}`)
    }
    ;({ spaces } = await ctl.send('agentSpace.spaces.list', { windowId }))
    assertEq(spaces.length, 1, 'one space left')
    assertEq((await ctl.send('agentSpace.spaces.delete',
                             { spaceId: spaces[0].id })).ok, false,
             'the last space is permanent')
    assertEq(spaces[0].isDefault, true, 'and it holds the default role')
    const survivingSpace = spaces[0].id
    await waitSpaces(ctl, windowId,
      current => current.some(space => space.id === survivingSpace &&
                                      space.active && space.open),
      { timeout: 10000, what: 'surviving Space to attach in the original shell' })
    const survivingShell = await ctl.send('agentSpace.window.handle', { windowId })
    assertEq(survivingShell.ok, true, 'original shell remains live')
    assertEq(String(survivingShell.hwnd), String(originalShell.hwnd),
             'default-role handoff preserves the native shell')
  } finally { ctl.close() }
})

test('spaces: reorder moves a Space in the list', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const created = await ctl.send('agentSpace.spaces.create', { name: 'ReorderSuite' })
    assertEq(created.ok, true, 'created')
    await sleep(400)
    assertEq((await ctl.send('agentSpace.spaces.reorder',
                             { spaceId: created.id, index: 0 })).ok,
             true, 'reorder ok')
    await sleep(300)
    const spaces = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    assertEq(spaces[0].name, 'ReorderSuite', 'moved to the front')
    await ctl.send('agentSpace.spaces.delete', { spaceId: created.id })
  } finally { ctl.close() }
})

run()
