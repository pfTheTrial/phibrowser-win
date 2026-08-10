// Space↔profile binding: every Space has EXACTLY ONE profile, and several
// Spaces may share one (the fork's model; the binding is immutable).
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

/** Switches to a Space, waiting out an async profile load if needed. */
async function switchSpace(ctl, windowId, spaceId) {
  const r = await ctl.send('agentSpace.spaces.switch', { windowId, spaceId })
  assertEq(r.ok, true, `switch to ${spaceId} accepted`)
  for (let i = 0; i < 40; i++) {
    const { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
    const s = spaces.find((x) => x.id === spaceId)
    if (s && s.active && s.open) return s
    await sleep(250)
  }
  throw new Error(`space ${spaceId} never became active`)
}

test('profiles: list reports at least the default profile', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.profiles.list')
    assertEq(r.ok, true, 'list ok')
    assert(r.profiles.length >= 1, 'at least one profile')
    for (const k of ['id', 'name', 'loaded']) {
      assert(k in r.profiles[0], `profile has ${k}`)
    }
    assert(r.profiles.some((p) => p.id === 'Default'), 'Default profile present')
  } finally { ctl.close() }
})

test('spaces: every space reports exactly one profile', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
    for (const s of spaces) {
      assert(typeof s.profileId === 'string' && s.profileId.length > 0,
             `space ${s.id} binds a profile (${s.profileId})`)
    }
  } finally { ctl.close() }
})

test('spaces: two spaces can SHARE one profile', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const a = await ctl.send('agentSpace.spaces.create',
                             { name: 'Shared A', profileId: 'Default' })
    const b = await ctl.send('agentSpace.spaces.create',
                             { name: 'Shared B', profileId: 'Default' })
    assertEq(a.profileId, 'Default', 'A bound to Default')
    assertEq(b.profileId, 'Default', 'B bound to Default')

    await switchSpace(ctl, windowId, a.id)
    const profA = await ctl.send('agentSpace.window.profile', { windowId })
    await switchSpace(ctl, windowId, b.id)
    const profB = await ctl.send('agentSpace.window.profile', { windowId })
    assertEq(profA.id, 'Default', 'A runs in Default')
    assertEq(profB.id, 'Default', 'B runs in Default')

    // Sharing a profile must NOT merge their tabs — Spaces stay separate.
    const mark = `shared-${Date.now()}`
    await ctl.send('agentSpace.navigate',
                   { windowId, url: `data:text/html,<title>${mark}</title>` })
    await sleep(600)
    await switchSpace(ctl, windowId, a.id)
    const tabsA = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
    assert(!tabsA.some((t) => (t.title || '').includes(mark)),
           'space A does not show space B\'s page despite the shared profile')
  } finally { ctl.close() }
})

test('spaces: a space bound to another profile runs in that profile', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    let { profiles } = await ctl.send('agentSpace.profiles.list')
    let other = profiles.find((p) => p.id !== 'Default')
    if (!other) {
      await ctl.send('agentSpace.profiles.create', { name: 'Work' })
      for (let i = 0; i < 40 && !other; i++) {
        await sleep(500)
        ;({ profiles } = await ctl.send('agentSpace.profiles.list'))
        other = profiles.find((p) => p.id !== 'Default')
      }
    }
    assert(other, `a second profile exists (${JSON.stringify(profiles)})`)

    const space = await ctl.send('agentSpace.spaces.create',
                                 { name: 'Work Space', profileId: other.id })
    assertEq(space.profileId, other.id, 'space bound to the second profile')
    await switchSpace(ctl, windowId, space.id)
    const prof = await ctl.send('agentSpace.window.profile', { windowId })
    assertEq(prof.id, other.id,
             `the space's browser runs in its own profile (${prof.id})`)

    // And back to the default space: different profile again.
    await switchSpace(ctl, windowId, '1')
    const home = await ctl.send('agentSpace.window.profile', { windowId })
    assertEq(home.id, 'Default', 'default space is back on Default')
    // Housekeeping: take the created Work Space with us, or the box grows a
    // new one every run.
    await ctl.send('agentSpace.spaces.delete', { spaceId: space.id })
  } finally { ctl.close() }
})

test('profiles: rename updates the display name', async () => {
  const ctl = await openControl()
  try {
    const { profiles } = await ctl.send('agentSpace.profiles.list')
    const other = profiles.find((p) => p.id !== 'Default')
    assert(other, 'a second profile exists')
    const fresh = `W-${Date.now()}`
    assertEq((await ctl.send('agentSpace.profiles.rename',
                             { id: other.id, name: fresh })).ok, true,
             'rename accepted')
    let renamed = null
    for (let i = 0; i < 20 && !renamed; i++) {
      await sleep(250)
      const list = (await ctl.send('agentSpace.profiles.list')).profiles
      renamed = list.find((p) => p.id === other.id && p.name === fresh)
    }
    assert(renamed, 'the list shows the new name')
    await ctl.send('agentSpace.profiles.rename',
                   { id: other.id, name: 'Work' })
  } finally { ctl.close() }
})

test('profiles: Default and in-use profiles refuse deletion', async () => {
  const ctl = await openControl()
  try {
    assertEq((await ctl.send('agentSpace.profiles.delete',
                             { id: 'Default' })).ok, false,
             'Default is permanent')
    // Bind a space to the second profile, then try to delete the profile.
    const { profiles } = await ctl.send('agentSpace.profiles.list')
    const other = profiles.find((p) => p.id !== 'Default')
    assert(other, 'a second profile exists')
    const space = await ctl.send('agentSpace.spaces.create',
                                 { name: 'Binder', profileId: other.id })
    try {
      assertEq((await ctl.send('agentSpace.profiles.delete',
                               { id: other.id })).ok, false,
               'an in-use profile refuses (bindings are immutable)')
    } finally {
      await ctl.send('agentSpace.spaces.delete', { spaceId: space.id })
    }
  } finally { ctl.close() }
})

test('profiles: an unused profile can be deleted', async () => {
  const ctl = await openControl()
  try {
    const doomed = `Doomed-${Date.now()}`
    await ctl.send('agentSpace.profiles.create', { name: doomed })
    let created = null
    for (let i = 0; i < 40 && !created; i++) {
      await sleep(500)
      const list = (await ctl.send('agentSpace.profiles.list')).profiles
      created = list.find((p) => p.name === doomed)
    }
    assert(created, 'the profile appeared')
    assertEq((await ctl.send('agentSpace.profiles.delete',
                             { id: created.id })).ok, true, 'delete accepted')
    let present = true
    for (let i = 0; i < 40 && present; i++) {
      await sleep(500)
      const list = (await ctl.send('agentSpace.profiles.list')).profiles
      present = list.some((p) => p.id === created.id)
    }
    assertEq(present, false, 'the profile is gone')
  } finally { ctl.close() }
})

run()
