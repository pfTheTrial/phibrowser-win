// Spaces suite: Spaces are a CLIENT concept (as on Mac) — a named workspace
// with its own Browser (own tab set). A slot shows one at a time; switching
// swaps which browser is visible in the shell window.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

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
    await sleep(800)

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
    const home = spaces.find((s) => s.id === '1')
    assert(home, 'default space exists')

    // Put a distinctive page in the CURRENT (non-default) space.
    const otherMark = `other-${Date.now()}`
    await ctl.send('agentSpace.navigate',
                   { windowId, url: `data:text/html,<title>${otherMark}</title>` })
    await sleep(600)

    assertEq((await ctl.send('agentSpace.spaces.switch',
                             { windowId, spaceId: '1' })).ok, true, 'switch home ok')
    await sleep(700)
    const homeTabs = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
    assert(!homeTabs.some((t) => (t.title || '').includes(otherMark)),
           'home space does not show the other space page')

    const listed = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    assertEq(listed.find((s) => s.id === '1').active, true, 'home is active again')
  } finally { ctl.close() }
})

test('spaces: a window that visited two spaces still closes', async () => {
  const ctl = await openControl()
  try {
    // A fresh slot, so closing it cannot disturb the window the other tests use.
    const before = (await ctl.send('agentSpace.list')).windows
    await ctl.send('agentSpace.newWindow')
    let windows = before
    for (let i = 0; i < 24 && windows.length === before.length; i++) {
      await sleep(250)
      windows = (await ctl.send('agentSpace.list')).windows
    }
    const slot = windows.find((w) => !before.includes(w))
    assert(slot, 'new window created')

    // Visiting a second Space gives the slot a SECOND browser; closing only
    // the first would leave the window on screen (the bug this covers).
    const created = await ctl.send('agentSpace.spaces.create', { name: 'Closable' })
    assertEq((await ctl.send('agentSpace.spaces.switch',
                             { windowId: slot, spaceId: created.id })).ok,
             true, 'switched the new window to a second space')
    await sleep(800)

    assertEq((await ctl.send('agentSpace.close', { windowId: slot })).ok, true,
             'close accepted')
    let live = windows
    for (let i = 0; i < 24; i++) {
      await sleep(250)
      live = (await ctl.send('agentSpace.list')).windows
      if (!live.includes(slot)) break
    }
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
    for (let i = 0; i < 40; i++) {
      const { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
      const s = spaces.find((x) => x.id === created.id)
      if (s && s.active && s.open) break
      await sleep(250)
    }
    const gone = await ctl.send('agentSpace.spaces.delete',
                                { spaceId: created.id })
    assertEq(gone.ok, true, 'delete accepted')
    let spaces = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    assertEq(spaces.some((s) => s.id === created.id), false,
             'the space is gone from the list')
    // The window fell back to another open space rather than a dead one.
    for (let i = 0; i < 20; i++) {
      spaces = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
      const active = spaces.find((s) => s.active)
      if (active && active.id !== created.id) break
      await sleep(250)
    }
    const active = spaces.find((s) => s.active)
    assert(active && active.id !== created.id,
           `another space is active (${active && active.id})`)
  } finally { ctl.close() }
})

test('spaces: the default space refuses deletion', async () => {
  const ctl = await openControl()
  try {
    assertEq((await ctl.send('agentSpace.spaces.delete',
                             { spaceId: '1' })).ok, false,
             'the default space is permanent')
  } finally { ctl.close() }
})

run()
