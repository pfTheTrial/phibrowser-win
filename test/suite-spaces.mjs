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

test('spaces: switching to an unknown space is rejected', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const r = await ctl.send('agentSpace.spaces.switch',
                             { windowId, spaceId: 'no-such-space' })
    assertEq(r.ok, false, 'unknown space rejected')
  } finally { ctl.close() }
})

run()
