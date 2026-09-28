// Runs ON Windows, after the main runner explicitly hands over the box.
// Never import app.mjs: this suite neither relaunches nor kills the browser.
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { test, run, assert, assertEq, sleep, cdpHttp, openCdp } from './phi-test.mjs'
import { waitFor } from './lib/wait.mjs'

const here = path.dirname(fileURLToPath(import.meta.url))
const fixture = path.resolve(here, 'fixtures', 'hosted-shell-extension')
const fixtureName = 'Phi Hosted Shell Isolated Audit Fixture'
const normalize = p => path.win32.normalize(p).toLowerCase()
let browser, driver, extensionId, driverTarget, ready = false, baseline, bounds
const wsPath = url => new URL(url).pathname

// The shared CDP client is serial-only and discards events. Do not issue
// concurrent sends or await popup events on this connection.
async function evaluate(expression) {
  assert(driver, 'driver connected')
  const result = await driver.send('Runtime.evaluate', {
    expression, awaitPromise: true, returnByValue: true,
  })
  if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails))
  return result.result.value
}
async function api(method, ...args) {
  assert(ready, 'fixture setup completed')
  return evaluate(`hostedShellAudit[${JSON.stringify(method)}](...${JSON.stringify(args)})`)
}
async function state(id, expected) {
  return waitFor(async () => {
    const w = await api('get', id)
    return w.state === expected ? w : null
  }, { what: `window ${id} state ${expected}`, timeout: 8000 })
}
async function focusBaseline() {
  await api('focus', baseline.id)
  await waitFor(async () => (await api('get', baseline.id)).focused,
                { what: 'owned baseline focused' })
}
function expectBounds(window, expected, label) {
  for (const key of Object.keys(expected)) assertEq(window[key], expected[key], `${label}.${key}`)
}

test('hosted extension: install fresh fixture and connect driver', async () => {
  assertEq(process.platform, 'win32', 'run on Windows; no remote operations from this suite')
  const version = await cdpHttp('/json/version')
  browser = await openCdp(wsPath(version.webSocketDebuggerUrl))
  const { extensions } = await browser.send('Extensions.getExtensions')
  assert(!extensions.some(e => normalize(e.path) === normalize(fixture) || e.name === fixtureName),
         'fixture already installed: refuse to reload/uninstall a pre-existing extension')
  ;({ id: extensionId } = await browser.send('Extensions.loadUnpacked', { path: fixture }))
  assert(extensionId, 'new fixture ID returned')
  console.log('OWNED extension', extensionId, 'path', fixture)
  const url = `chrome-extension://${extensionId}/driver.html?run=${Date.now()}`
  ;({ targetId: driverTarget } = await browser.send('Target.createTarget', { url }))
  const target = await waitFor(async () =>
    (await cdpHttp('/json/list')).find(t => t.url === url && t.webSocketDebuggerUrl),
    { what: 'fixture driver page discovery' })
  driver = await openCdp(wsPath(target.webSocketDebuggerUrl))
  await waitFor(() => evaluate('!!globalThis.hostedShellAudit'), { what: 'driver script ready' })
  const screen = await evaluate('({x:screen.availLeft,y:screen.availTop,w:screen.availWidth,h:screen.availHeight})')
  bounds = { left: screen.x + 20, top: screen.y + 20,
             width: Math.min(1000, screen.w - 40), height: Math.min(720, screen.h - 40) }
  assert(bounds.width >= 900 && bounds.height >= 600, 'work area supports test bounds without minimum-size clamping')
  ready = true
  baseline = await api('create', { ...bounds, focused: true, type: 'normal' })
  console.log('OWNED baseline', baseline.id)
  await focusBaseline()
})

for (const type of ['normal', 'popup']) {
  test(`hosted extension: ${type} explicit bounds and focused:false`, async () => {
    await focusBaseline()
    const created = await api('create', { ...bounds, type, focused: false })
    console.log('OWNED window', created.id)
    try {
      expectBounds(created, bounds, 'create result')
      assertEq(created.focused, false, 'create result stays inactive')
      // Sample over the settling period, rather than only at its end.
      for (let i = 0; i < 8; i++) {
        const current = await api('get', created.id)
        expectBounds(current, bounds, 'get result')
        assertEq(current.focused, false, 'new window remains inactive')
        assertEq((await api('get', baseline.id)).focused, true, 'baseline remains focused')
        await sleep(100)
      }
    } finally { await api('remove', created.id) }
  })
}

test('hosted extension: partial initial bounds', async () => {
  await focusBaseline()
  const expected = { width: bounds.width, height: bounds.height }
  const created = await api('create', { ...expected, type: 'popup', focused: false })
  try {
    expectBounds(created, expected, 'partial create')
    expectBounds(await api('get', created.id), expected, 'partial get')
    assertEq((await api('get', baseline.id)).focused, true, 'partial bounds do not activate')
  } finally { await api('remove', created.id) }
})

for (const options of [
  { state: 'minimized' },
  { state: 'minimized', focused: false },
  { state: 'maximized' },
  { state: 'fullscreen' },
]) {
  test(`hosted extension: initial ${JSON.stringify(options)}`, async () => {
    await focusBaseline()
    const created = await api('create', options)
    try {
      assertEq(created.state, options.state, 'create returns requested state')
      const current = await state(created.id, options.state)
      if (options.state === 'minimized') {
        assertEq(current.focused, false, 'minimized window inactive')
        assertEq((await api('get', baseline.id)).focused, true, 'minimized creation preserves focus')
      } else {
        await waitFor(async () => (await api('get', created.id)).focused,
                      { what: 'maximized/fullscreen window focused' })
      }
    } finally { await api('remove', created.id) }
  })
}

test('hosted extension: invalid state combinations reject without creating windows', async () => {
  for (const options of [
    { state: 'minimized', focused: true },
    { state: 'maximized', focused: false },
    { state: 'fullscreen', focused: false },
    { state: 'minimized', width: 900 },
    { state: 'maximized', width: 900 },
    { state: 'fullscreen', width: 900 },
  ]) {
    const before = (await api('list')).map(w => w.id).sort((a, b) => a - b)
    const result = await evaluate(`(async () => {
      try { return { created: await hostedShellAudit.create(${JSON.stringify(options)}) }; }
      catch (e) { return { error: String(e) }; }
    })()`)
    assert(result.error, `${JSON.stringify(options)} rejected: ${JSON.stringify(result)}`)
    assert(/state/i.test(result.error), `state validation error: ${result.error}`)
    const after = (await api('list')).map(w => w.id).sort((a, b) => a - b)
    assertEq(JSON.stringify(after), JSON.stringify(before), 'invalid request creates no window')
  }
})

test('hosted extension: cleanup only this run’s resources', async () => {
  const errors = []
  if (driver) {
    try {
      const result = await evaluate('hostedShellAudit.cleanup()')
      console.log('CLEANUP', JSON.stringify(result))
      if (result.remaining.length) errors.push(JSON.stringify(result))
    } catch (e) { errors.push(`owned window cleanup: ${e.message}`) }
  }
  // If owned windows could not be cleaned up, retain the driver and fixture
  // for recovery. Never search for and delete other extensions or windows.
  if (!errors.length && driverTarget) {
    try {
      const result = await browser.send('Target.closeTarget', { targetId: driverTarget })
      assertEq(result.success, true, 'driver target closed')
    }
    catch (e) { errors.push(`driver cleanup: ${e.message}`) }
  }
  if (!errors.length && extensionId) {
    try {
      await browser.send('Extensions.uninstall', { id: extensionId })
      const result = await browser.send('Extensions.getExtensions')
      assert(!result.extensions.some(e => e.id === extensionId), 'owned fixture uninstalled')
    }
    catch (e) { errors.push(`fixture cleanup (${extensionId}): ${e.message}`) }
  }
  assertEq(errors.length, 0, `cleanup errors; retain reported IDs for recovery: ${errors.join('; ')}`)
})

try { await run() } finally { driver?.close(); browser?.close() }
