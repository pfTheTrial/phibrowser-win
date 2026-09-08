// Persistence suite: the Space list (id, name, profile) and the chosen
// layout survive a restart.
// This suite RESTARTS the app itself and leaves Spaces behind, so it runs
// LAST — test/suites.txt says so and test-box.sh honours the file's order
// (run-fx-all.sh used to glob alphabetically, which put it ninth of nineteen).
import { test, run, assert, assertEq, openControl } from './phi-test.mjs'
import { restartApp } from './lib/app.mjs'


test('persist: a created space survives a restart', async () => {
  let ctl = await openControl()
  let created
  try {
    const { windows } = await ctl.send('agentSpace.list')
    const name = `Persisted ${Date.now()}`
    created = await ctl.send('agentSpace.spaces.create',
                             { name, profileId: 'Default' })
    assertEq(created.ok, true, 'space created')
    const before = (await ctl.send('agentSpace.spaces.list',
                                   { windowId: windows[0] })).spaces
    assert(before.some((s) => s.id === created.id), 'listed before restart')
  } finally { ctl.close() }

  await restartApp()

  ctl = await openControl()
  try {
    const { windows } = await ctl.send('agentSpace.list')
    assert(windows.length >= 1, 'a slot exists after restart')
    const after = (await ctl.send('agentSpace.spaces.list',
                                  { windowId: windows[0] })).spaces
    const mine = after.find((s) => s.id === created.id)
    assert(mine, `space ${created.id} survived the restart`)
    assertEq(mine.name, created.name, 'name preserved')
    assertEq(mine.profileId, 'Default', 'profile binding preserved')
  } finally { ctl.close() }
})

test('persist: space ids keep increasing after a restart', async () => {
  let ctl = await openControl()
  let first
  try {
    first = await ctl.send('agentSpace.spaces.create', { name: 'Seq A' })
  } finally { ctl.close() }

  await restartApp()

  ctl = await openControl()
  try {
    const second = await ctl.send('agentSpace.spaces.create', { name: 'Seq B' })
    assert(Number(second.id) > Number(first.id),
           `new id ${second.id} continues past ${first.id} (no reuse)`)
    const { windows } = await ctl.send('agentSpace.list')
    const spaces = (await ctl.send('agentSpace.spaces.list',
                                   { windowId: windows[0] })).spaces
    const ids = spaces.map((s) => s.id)
    assertEq(new Set(ids).size, ids.length, 'no duplicate space ids')
  } finally { ctl.close() }
})

test('persist: the chosen layout survives a restart', async () => {
  let ctl = await openControl()
  try {
    // Comfortable is the most distinctive: no sidebar at all.
    assertEq((await ctl.send('agentSpace.layout.set',
                             { mode: 'comfortable' })).ok, true, 'set ok')
  } finally { ctl.close() }

  await restartApp()

  ctl = await openControl()
  try {
    assertEq((await ctl.send('agentSpace.layout.get')).mode, 'comfortable',
             'layout restored')
    const { windows } = await ctl.send('agentSpace.list')
    const l = await ctl.send('agentSpace.window.layout', { windowId: windows[0] })
    assert(l.content.x < 40, 'the restored window really has no sidebar')
  } finally { ctl.close() }

  // Leave the default behind for the suites that assume it.
  ctl = await openControl()
  try {
    await ctl.send('agentSpace.layout.set', { mode: 'performance' })
  } finally { ctl.close() }
})

test('persist: the chosen theme and appearance survive a restart', async () => {
  let ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.theme.set',
                             { themeId: 'iris', appearance: 'dark' })
    assertEq(r.ok, true, 'theme set')
  } finally { ctl.close() }

  await restartApp()

  ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.theme.get')
    assertEq(r.themeId, 'iris', 'theme restored')
    assertEq(r.appearance, 'dark', 'appearance restored')
    assertEq(r.dark, true, 'and still resolves dark')
  } finally { ctl.close() }

  // Leave the default behind for the suites that assume it.
  ctl = await openControl()
  try {
    await ctl.send('agentSpace.theme.set',
                   { themeId: 'pure', appearance: 'light' })
  } finally { ctl.close() }
})

run()
