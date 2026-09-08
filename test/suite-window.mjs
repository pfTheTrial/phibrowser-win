// Window suite: client-owned shell geometry over the control socket.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'
import { firstWindow } from './lib/window.mjs'

test('window: bounds reports a sane rect', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const b = await ctl.send('agentSpace.window.bounds', { windowId })
    assertEq(b.ok, true, 'bounds ok')
    assert(b.width > 100 && b.height > 100, `plausible size (${b.width}x${b.height})`)
  } finally { ctl.close() }
})

test('window: setBounds moves and resizes', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const before = await ctl.send('agentSpace.window.bounds', { windowId })
    const want = { x: 120, y: 90, width: 900, height: 700 }
    assertEq((await ctl.send('agentSpace.window.setBounds', { windowId, ...want })).ok,
             true, 'setBounds ok')
    await sleep(400)
    const after = await ctl.send('agentSpace.window.bounds', { windowId })
    assertEq(after.width, want.width, 'width applied')
    assertEq(after.height, want.height, 'height applied')
    assertEq(after.x, want.x, 'x applied')
    assertEq(after.y, want.y, 'y applied')
    // Restore something reasonable for later suites.
    await ctl.send('agentSpace.window.setBounds',
                   { windowId, x: before.x, y: before.y,
                     width: before.width, height: before.height })
    await sleep(300)
  } finally { ctl.close() }
})

test('window: setBounds rejects a bad size', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const r = await ctl.send('agentSpace.window.setBounds',
                             { windowId, x: 0, y: 0, width: 0, height: 0 })
    assertEq(r.ok, false, 'zero size rejected')
  } finally { ctl.close() }
})

test('window: bounds rejects an unknown window', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.window.bounds', { windowId: 999999999 })
    assertEq(r.ok, false, 'unknown window rejected')
  } finally { ctl.close() }
})

run()
