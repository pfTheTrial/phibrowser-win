// Real Chromium Browser-domain routing, not agentSpace.window.setBounds.
// browser_handler.cc resolves BrowserWindow and calls GetBounds/SetBounds,
// IsMinimized/IsMaximized/IsFullscreen and ExclusiveAccessContext. Minimized
// readback deliberately uses GetRestoredBounds. Page.close dispatches
// beforeunload and acknowledges without waiting for the dialog to finish.
// Run separately from other UI suites. No external URLs or existing-tab edits.
// Fresh-launch isolation: --case beforeunload (or bounds / minimize).
import { randomUUID } from 'node:crypto'
import { test as registerTest, run, assert, assertEq, openControl, cdpHttp, openCdp } from './phi-test.mjs'
import { waitFor, waitSpaces } from './lib/wait.mjs'

const caseArg = process.argv.indexOf('--case')
const caseFilter = caseArg === -1 ? '' : process.argv[caseArg + 1]
if (caseArg !== -1 && (!caseFilter || caseFilter.startsWith('--'))) {
  throw new Error('--case requires a nonempty substring of the test name')
}
const availableCases = []
let selectedCases = 0
function test(name, body) {
  availableCases.push(name)
  if (!name.toLowerCase().includes(caseFilter.toLowerCase())) return
  registerTest(name, body)
  selectedCases++
}

const rectKeys = ['left', 'top', 'width', 'height']
const rect = (b) => Object.fromEntries(rectKeys.map((k) => [k, b[k]]))
const near = (a, b, label, tolerance = 2) =>
  assert(Number.isFinite(a) && Number.isFinite(b) && Math.abs(a - b) <= tolerance,
         `${label}: got ${a}, want ${b} (+/-${tolerance})`)
const sameRect = (actual, expected, label) => {
  for (const k of rectKeys) near(actual[k], expected[k], `${label}.${k}`)
}
async function ok(ctl, method, params = {}) {
  const r = await ctl.send(method, params)
  assertEq(r.ok, true, `${method}: ${JSON.stringify(r)}`)
  return r
}
async function evaluate(page, expression, userGesture = false) {
  const r = await page.send('Runtime.evaluate', {
    expression, userGesture, returnByValue: true,
  })
  assert(!r.exceptionDetails, `evaluate: ${JSON.stringify(r.exceptionDetails)}`)
  return r.result.value
}

// Each case owns a new target/window, with an exact marker and target ID.
// Existing shells/tabs/Spaces are never resized, navigated, or closed.
async function withShell(body) {
  const ctl = await openControl()
  let root, page, targetId, windowId, initial, failure
  const spaces = []
  const cleanupErrors = []
  const cleanup = async (label, fn) => {
    try { await fn() } catch (e) { cleanupErrors.push(`${label}: ${e.message}`) }
  }
  try {
    const version = await cdpHttp('/json/version')
    const path = new URL(version.webSocketDebuggerUrl).pathname
    assert(path.startsWith('/devtools/browser/'), 'root browser CDP endpoint')
    root = await openCdp(path)
    const marker = `phi-hosted-shell-${randomUUID()}`
    const url = `data:text/html,${encodeURIComponent(`<title>${marker}</title><p>${marker}</p>`)}`
    ;({ targetId } = await root.send('Target.createTarget', { url, newWindow: true }))
    assert(targetId, 'owned target created')
    const target = await waitFor(async () =>
      (await cdpHttp('/json/list')).find((t) => t.id === targetId && t.title === marker),
    { what: 'owned marked page to load' })
    page = await openCdp(new URL(target.webSocketDebuggerUrl).pathname)
    ;({ windowId } = await root.send('Browser.getWindowForTarget', { targetId }))
    assert(Number.isInteger(windowId), 'CDP returned an integer Browser session ID')
    const get = async () => (await root.send('Browser.getWindowBounds', { windowId })).bounds
    const set = (bounds) => root.send('Browser.setWindowBounds', { windowId, bounds })
    const state = async (want) => {
      await set({ windowState: want })
      return waitFor(async () => {
        const b = await get()
        assertEq(b.windowState, want, `window state: ${JSON.stringify(b)}`)
        return b
      }, { what: `CDP window state ${want}` })
    }
    // Exiting fullscreen can restore maximized state; normalization is two
    // distinct commands because BrowserHandler ignores geometry while restoring.
    const normal = async () => {
      for (let i = 0; i < 3; ++i) {
        if ((await get()).windowState === 'normal') return get()
        await set({ windowState: 'normal' })
        await waitFor(async () => (await get()).windowState !== 'fullscreen',
                      { what: 'fullscreen exit' })
      }
      assertEq((await get()).windowState, 'normal', 'normalized owned shell')
      return get()
    }
    await ok(ctl, 'agentSpace.window.bounds', { windowId })
    initial = { bounds: await get(), get, set, state, normal }
    await body({ ctl, root, page, targetId, windowId, marker, spaces,
                 get, set, state, normal })
  } catch (e) { failure = e }
  finally {
    // Clear a possible pending beforeunload even when the presentation assertion
    // failed. Do not wait on Runtime.evaluate until the modal dialog is gone.
    if (page) {
      try { await page.send('Page.handleJavaScriptDialog', { accept: false }) }
      catch (e) {
        if (!/No (JavaScript )?dialog|No dialog is showing|not found|closed/i.test(e.message))
          cleanupErrors.push(`cancel dialog: ${e.message}`)
      }
      await cleanup('clear own beforeunload', () => evaluate(page, 'window.onbeforeunload = null; true'))
    }
    if (initial) await cleanup('restore owned window', async () => {
      await initial.normal()
      await initial.set(rect(initial.bounds))
      if (initial.bounds.windowState !== 'normal') await initial.state(initial.bounds.windowState)
    })
    if (windowId) await cleanup('close owned shell', async () => {
      await ok(ctl, 'agentSpace.close', { windowId })
      await waitFor(async () => !(await cdpHttp('/json/list')).some((t) => t.id === targetId),
                    { what: 'owned target to close' })
    })
    else if (targetId && root) await cleanup('close unbound owned target', async () => {
      const r = await root.send('Target.closeTarget', { targetId })
      assertEq(r.success, true, 'owned target closed')
    })
    for (const spaceId of spaces) await cleanup(`delete own Space ${spaceId}`, async () => {
      await ok(ctl, 'agentSpace.spaces.delete', { spaceId })
      await waitFor(async () => !(await ok(ctl, 'agentSpace.spaces.list')).spaces
        .some((s) => s.id === spaceId), { what: 'owned Space deletion' })
    })
    page?.close()
    root?.close()
    ctl.close()
  }
  if (failure || cleanupErrors.length) throw new Error(
    [failure?.message, ...cleanupErrors.map((e) => `cleanup: ${e}`)].filter(Boolean).join('\n'))
}

async function sample(f) {
  const bounds = await f.get()
  const mapped = await f.root.send('Browser.getWindowForTarget', { targetId: f.targetId })
  assertEq(mapped.windowId, f.windowId, 'target still belongs to owned Browser')
  assertEq(mapped.bounds.windowState, bounds.windowState, 'Browser getters agree on state')
  sameRect(mapped.bounds, bounds, 'Browser getters agree on bounds')
  const native = await ok(f.ctl, 'agentSpace.window.bounds', { windowId: f.windowId })
  const layout = await ok(f.ctl, 'agentSpace.window.layout', { windowId: f.windowId })
  assert(layout.dpi > 0, 'native shell DPI reported')
  return { bounds, native, scale: layout.dpi / 96 }
}
async function shellMatches(f, state, origin = null, expected = null) {
  return waitFor(async () => {
    const s = await sample(f)
    assertEq(s.bounds.windowState, state, 'CDP state')
    // Native bounds are physical pixels; CDP bounds are screen DIPs.
    near(s.native.width, s.bounds.width * s.scale, 'native shell width', 3)
    near(s.native.height, s.bounds.height * s.scale, 'native shell height', 3)
    if (expected) sameRect(s.bounds, expected, 'requested CDP bounds')
    if (origin) {
      near(s.scale, origin.scale, 'test stays on its initial monitor', 0)
      // Mixed-DPI desktops have monitor-relative DIP origins. Compare movement
      // deltas, not global physical x divided by DPI, which is invalid there.
      near(s.native.x - origin.native.x,
           (s.bounds.left - origin.bounds.left) * s.scale, 'native shell move x', 3)
      near(s.native.y - origin.native.y,
           (s.bounds.top - origin.bounds.top) * s.scale, 'native shell move y', 3)
    }
    return s
  }, { what: `${state} CDP bounds to match the actual native shell` })
}

test('hosted-shell: root Browser bounds route to native shell, including partial updates', () =>
  withShell(async (f) => {
    await f.normal()
    const before = await shellMatches(f, 'normal')
    const mapped = await f.root.send('Browser.getWindowForTarget', { targetId: f.targetId })
    assertEq(mapped.windowId, f.windowId, 'target maps to owned Browser')
    sameRect(mapped.bounds, before.bounds, 'getWindowForTarget/getWindowBounds')
    const want = { left: before.bounds.left + 8, top: before.bounds.top + 6,
                  width: Math.max(800, before.bounds.width - 40),
                  height: Math.max(600, before.bounds.height - 30) }
    await f.set(want)
    await shellMatches(f, 'normal', before, want)
    const partial = { ...want, width: want.width + 24 }
    await f.set({ width: partial.width })
    await shellMatches(f, 'normal', before, partial)
    await f.set(rect(before.bounds))
    await shellMatches(f, 'normal', before, before.bounds)
  }))

test('hosted-shell: minimize/maximize/fullscreen and restore preserve shell geometry', () =>
  withShell(async (f) => {
    await f.normal()
    // Small enough to distinguish maximized from ordinary state on the test box.
    const start = await f.get()
    await f.set({ left: start.left, top: start.top, width: 800, height: 600 })
    const baseline = await shellMatches(f, 'normal')
    await f.state('minimized')
    // BrowserHandler explicitly returns restored bounds while minimized; the
    // native HWND rectangle is iconic and must NOT be equated to those bounds.
    sameRect(await f.get(), baseline.bounds, 'minimized restored bounds')
    await waitFor(async () => {
      const n = await ok(f.ctl, 'agentSpace.window.bounds', { windowId: f.windowId })
      assert(n.width < baseline.native.width && n.height < baseline.native.height,
             `native shell has iconic bounds, not just its child: ${JSON.stringify(n)}`)
      return true
    }, { what: 'native shell minimization' })
    await f.normal()
    await shellMatches(f, 'normal', baseline, baseline.bounds)
    await f.state('maximized')
    const maximized = await shellMatches(f, 'maximized')
    assert(maximized.native.width !== baseline.native.width ||
           maximized.native.height !== baseline.native.height, 'native shell actually maximized')
    await f.normal()
    await shellMatches(f, 'normal', baseline, baseline.bounds)
    await f.state('fullscreen')
    const fullscreen = await shellMatches(f, 'fullscreen')
    assert(fullscreen.native.width !== baseline.native.width ||
           fullscreen.native.height !== baseline.native.height, 'native shell actually entered fullscreen')
    await f.normal()
    await shellMatches(f, 'normal', baseline, baseline.bounds)
    // Also preserve maximized placement across fullscreen, not just normal size.
    await f.state('maximized')
    await f.state('fullscreen')
    await f.set({ windowState: 'normal' })
    await shellMatches(f, 'maximized', null, maximized.bounds)
    await f.normal()
    await shellMatches(f, 'normal', baseline, baseline.bounds)
  }))

test('hosted-shell: hidden Space beforeunload reveals its owner and Stay keeps page alive', () =>
  withShell(async (f) => {
    await f.page.send('Page.enable')
    const listed = await ok(f.ctl, 'agentSpace.spaces.list', { windowId: f.windowId })
    const a = listed.spaces.find((s) => s.active && s.windowIds.includes(f.windowId))
    assert(a, 'marked page belongs to active Space A')
    const armed = await evaluate(f.page, `(() => {
      window.onbeforeunload = e => { e.preventDefault(); e.returnValue = ''; return ''; };
      return navigator.userActivation.hasBeenActive;
    })()`, true)
    assertEq(armed, true, 'beforeunload has sticky user activation')
    const b = await ok(f.ctl, 'agentSpace.spaces.create', { name: f.marker })
    assert(b.id, 'own Space B created')
    f.spaces.push(b.id)
    await ok(f.ctl, 'agentSpace.spaces.switch', { windowId: f.windowId, spaceId: b.id })
    await waitSpaces(f.ctl, f.windowId,
      (sp) => sp.some((s) => s.id === b.id && s.active && s.open),
      { what: 'Space B presented before closing hidden A' })
    // The page session is still A, independent of the shell's selected Space.
    // No event waiter: openCdp intentionally discards pushed CDP events.
    await f.page.send('Page.close')
    await waitSpaces(f.ctl, f.windowId,
      (sp) => sp.some((s) => s.id === a.id && s.active && s.open),
      { what: 'beforeunload to reveal its owning Space A' })
    // Successful handling also proves an actual JS dialog existed; an automatic
    // close or a Space switch without a dialog cannot falsely pass this case.
    await waitFor(async () => {
      await f.page.send('Page.handleJavaScriptDialog', { accept: false })
      return true
    }, { what: 'beforeunload dialog to accept Stay' })
    assertEq(await evaluate(f.page, 'document.title'), f.marker, 'Stay retains the exact page')
    assertEq(await evaluate(f.page, 'window.onbeforeunload = null; true'), true, 'handler cleared')
    const after = await ok(f.ctl, 'agentSpace.spaces.list', { windowId: f.windowId })
    assert(after.spaces.some((s) => s.id === a.id && s.active), 'A remains selected after Stay')
  }))

if (!selectedCases) {
  throw new Error(`No hosted-shell case matches ${JSON.stringify(caseFilter)}. Available:\n${availableCases.join('\n')}`)
}
run()
