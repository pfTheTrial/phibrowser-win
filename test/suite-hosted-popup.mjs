// Exclusive Windows test-box handoff required. No relaunch/deployment here.
import http from 'node:http'
import crypto from 'node:crypto'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { test, run, assert, assertEq, sleep, openControl, openCdp, cdpHttp } from './phi-test.mjs'
import { waitFor } from './lib/wait.mjs'

const fixture = path.resolve(path.dirname(fileURLToPath(import.meta.url)), 'fixtures', 'hosted-popup-extension')
const fixtureName = 'Phi Hosted Popup Isolated Audit Fixture'
const token = crypto.randomBytes(16).toString('hex')
const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+jRZkAAAAASUVORK5CYII=', 'base64')
const normalize = value => path.win32.normalize(value).toLowerCase()
const wsPath = value => new URL(value).pathname
const cases = new Map()
const ownedSpaces = []
let ctl, browser, driver, driverTarget, extensionId, server, port, shellHwnd, originalSpace, profileId, spaceA, spaceB, ready = false
let positivePassed = false

function respondImage(response) {
  if (response.destroyed || response.writableEnded) return false
  response.writeHead(200, { 'Content-Type': 'image/png', 'Cache-Control': 'no-store', 'Content-Length': png.length })
  response.end(png)
  return true
}
async function startServer() {
  server = http.createServer((request, response) => {
    const parts = new URL(request.url, 'http://127.0.0.1').pathname.split('/').filter(Boolean)
    const entry = parts[0] === token ? cases.get(parts[1]) : null
    if (!entry || !['image', 'loaded'].includes(parts[2]) || parts.length !== 3) {
      response.writeHead(404); response.end(); return
    }
    if (parts[2] === 'loaded') {
      entry.loaded++
      respondImage(response)
    } else {
      entry.requested++
      entry.pending.push(response)
      response.on('close', () => { entry.closed++ })
      if (!entry.hold) respondImage(response)
    }
  })
  await new Promise((resolve, reject) => {
    server.once('error', reject)
    server.listen(0, '127.0.0.1', resolve)
  })
  port = server.address().port
}
// Re-resolve the control slot using the stable shell HWND after every switch.
// Do not confuse a Space's Chromium browser ID with the client slot ID.
async function currentWindow() {
  const { windows } = await ctl.send('agentSpace.list')
  for (const windowId of windows || []) {
    const result = await ctl.send('agentSpace.window.handle', { windowId })
    if (result.ok && String(result.hwnd) === shellHwnd) return windowId
  }
  throw new Error(`test shell ${shellHwnd} no longer available`)
}
async function activeSpace() {
  const result = await ctl.send('agentSpace.spaces.list', { windowId: await currentWindow() })
  assertEq(result.ok, true, 'spaces.list')
  return result.spaces.find(s => s.active)
}
async function switchTo(spaceId) {
  assert(spaceId === originalSpace || spaceId === spaceA || ownedSpaces.includes(spaceId), 'switch only to original/driver/owned Space')
  const result = await ctl.send('agentSpace.spaces.switch', { windowId: await currentWindow(), spaceId })
  assertEq(result.ok, true, 'Space switch accepted')
  await waitFor(async () => (await activeSpace())?.id === spaceId, { what: `Space ${spaceId} active` })
}
async function evaluate(expression) {
  const result = await driver.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true })
  if (result.exceptionDetails) throw new Error(JSON.stringify(result.exceptionDetails))
  return result.result.value
}
async function verifyDriverRealm() {
  const binding = await evaluate(`({
    href: location.href,
    readyState: document.readyState,
    runtimeId: globalThis.chrome?.runtime?.id ?? null,
    hasManifestAction: !!globalThis.chrome?.runtime?.getManifest?.().action,
    canSetPopup: typeof globalThis.chrome?.action?.setPopup === 'function'
  })`)
  const details = JSON.stringify(binding)
  assertEq(binding?.runtimeId, extensionId, `driver belongs to loaded fixture: ${details}`)
  assertEq(binding.readyState, 'complete', `driver document loaded: ${details}`)
  assertEq(binding.canSetPopup, true, `driver action API ready: ${details}`)
  return binding
}
async function popupTargets() {
  // Discovery only. Never attach to popup targets: that changes inspection
  // state and can keep the popup alive during ordinary dismissal.
  return (await cdpHttp('/json/list')).filter(t =>
    t.url?.startsWith(`chrome-extension://${extensionId}/popup.html?`))
}
async function noPopupFor(duration) {
  const until = Date.now() + duration
  do {
    assertEq((await popupTargets()).length, 0, 'no late popup reappearance')
    await sleep(100)
  } while (Date.now() < until)
}
async function prepare(caseId, hold) {
  assert(ready, 'setup completed')
  await switchTo(spaceA)
  await verifyDriverRealm()
  const focusedWindowId = await evaluate(`(async () => {
    const owner = await chrome.windows.getCurrent();
    await chrome.windows.update(owner.id, {focused: true});
    return owner.id;
  })()`)
  console.log('FOCUSED driver Browser', focusedWindowId)
  await waitFor(async () => !(await popupTargets()).length, { what: 'previous popup destroyed' })
  // Let the incoming Space finish its normal transition before opening.
  await sleep(600)
  const popup = `popup.html?port=${port}&token=${token}&case=${caseId}`
  await verifyDriverRealm()
  await evaluate(`chrome.action.setPopup({popup:${JSON.stringify(popup)}})`)
  const entry = { hold, requested: 0, loaded: 0, closed: 0, pending: [] }
  cases.set(caseId, entry)
  const result = await ctl.send('agentSpace.menu.click', {
    windowId: await currentWindow(), path: ['Extensions', fixtureName],
  })
  assertEq(result.ok, true, 'native Extensions menu action dispatched')
  await waitFor(() => entry.requested > 0, { what: `${caseId} popup image requested` })
  const targets = await waitFor(async () => {
    const found = await popupTargets()
    return found.length ? found : null
  }, { what: `${caseId} popup target appears` })
  assertEq(targets.length, 1, 'exactly one fixture popup target')
  console.log('POPUP', caseId, targets[0].id)
  return entry
}

test('hosted popup: isolated fixture in driver Space and disposable exit Space', async () => {
  assertEq(process.platform, 'win32', 'Windows only')
  ctl = await openControl()
  const version = await cdpHttp('/json/version')
  browser = await openCdp(wsPath(version.webSocketDebuggerUrl))
  const { extensions } = await browser.send('Extensions.getExtensions')
  assert(!extensions.some(e => e.name === fixtureName || normalize(e.path) === normalize(fixture)),
         'refuse to reload or clean up a pre-existing popup fixture')
  await startServer()
  ;({ id: extensionId } = await browser.send('Extensions.loadUnpacked', { path: fixture }))
  assert(extensionId, 'new fixture ID returned')
  console.log('OWNED extension', extensionId, 'loopback port', port)
  // Root CDP creates the driver in the same default browser context used by
  // Extensions.loadUnpacked. An arbitrary shell's active Space may not match.
  const url = `chrome-extension://${extensionId}/driver.html?run=${token}`
  ;({ targetId: driverTarget } = await browser.send('Target.createTarget', { url }))
  assert(driverTarget, 'own driver target created')
  const target = await waitFor(async () =>
    (await cdpHttp('/json/list')).find(t => t.id === driverTarget && t.url === url && t.webSocketDebuggerUrl),
    { what: 'own driver target' })
  driver = await openCdp(wsPath(target.webSocketDebuggerUrl))
  // Target discovery can precede the committed extension execution context.
  const binding = await waitFor(verifyDriverRealm, {
    what: `committed fixture realm ${extensionId} with action.setPopup`, timeout: 10000,
  })
  console.log('DRIVER BINDING', JSON.stringify(binding))
  const { windowId: driverWindowId } = await browser.send('Browser.getWindowForTarget', { targetId: driverTarget })
  const owner = await waitFor(async () => {
    const result = await ctl.send('agentSpace.spaces.list', { windowId: driverWindowId })
    assertEq(result.ok, true, 'driver shell Spaces listed')
    return result.spaces.find(s => s.windowIds?.some(id => String(id) === String(driverWindowId)))
  }, { what: 'driver Browser mapped to owning Space profile' })
  assert(typeof owner.profileId === 'string' && owner.profileId.length, 'driver profile ID available')
  profileId = owner.profileId
  const handle = await ctl.send('agentSpace.window.handle', { windowId: driverWindowId })
  assert(handle.ok && handle.hwnd, 'driver shell HWND available')
  shellHwnd = String(handle.hwnd)
  const active = await activeSpace()
  assert(active, 'original active Space found in driver shell')
  originalSpace = active.id
  // The model profile label need not identify this resident Browser's real
  // context after default-Space reassignment. Open the popup in the verified
  // driver's existing Browser, never a newly created Browser inferred from it.
  assertEq(owner.id, originalSpace, 'original Space contains verified driver Browser')
  spaceA = originalSpace
  console.log('DRIVER OWNER', JSON.stringify({ driverWindowId, shellHwnd, profileId, originalSpace }))
  for (const suffix of ['B']) {
    const result = await ctl.send('agentSpace.spaces.create', {
      name: `Popup audit ${token.slice(0, 8)} ${suffix}`, profileId,
    })
    assertEq(result.ok, true, 'own Space created')
    assert(result.id && result.id !== originalSpace, 'new Space has distinct ID')
    ownedSpaces.push(result.id)
    console.log('OWNED Space', result.id)
  }
  ;[spaceB] = ownedSpaces
  await switchTo(spaceA)
  await waitFor(async () => {
    const menu = await ctl.send('agentSpace.menu.list', { windowId: await currentWindow() })
    return menu.menus?.find(row => row.title === 'Extensions')?.items?.some(row => row.title === fixtureName && row.enabled)
  }, { what: 'fixture in current profile Extensions menu' })
  ready = true
})

test('hosted popup: positive control fully loads and stays open', async () => {
  const entry = await prepare('positive', false)
  await waitFor(() => entry.loaded > 0, { what: 'positive popup window.load beacon' })
  for (let i = 0; i < 6; i++) {
    assertEq((await popupTargets()).length, 1, 'loaded popup remains alive')
    await sleep(100)
  }
  await switchTo(spaceB)
  await waitFor(async () => !(await popupTargets()).length, { what: 'visible popup destroyed on Space exit' })
  positivePassed = true
})

test('hosted popup: shell movement dismisses an anchored popup', async () => {
  assert(positivePassed, 'positive popup control must pass before testing dismissal')
  const entry = await prepare('moved', false)
  await waitFor(() => entry.loaded > 0, { what: 'movable popup window.load beacon' })
  const windowId = await currentWindow()
  const before = await ctl.send('agentSpace.window.bounds', { windowId })
  assertEq(before.ok, true, 'shell bounds available')
  try {
    const moved = await ctl.send('agentSpace.window.setBounds', {
      windowId, x: before.x + 12, y: before.y + 12,
      width: before.width, height: before.height,
    })
    assertEq(moved.ok, true, 'shell move accepted')
    await waitFor(async () => !(await popupTargets()).length,
                  { what: 'anchored popup destroyed on shell move' })
    await noPopupFor(500)
  } finally {
    const restored = await ctl.send('agentSpace.window.setBounds', {
      windowId, x: before.x, y: before.y,
      width: before.width, height: before.height,
    })
    assertEq(restored.ok, true, 'original shell bounds restored')
  }
})

test('hosted popup: pending load cancelled on Space exit without resurrection', async () => {
  assert(positivePassed, 'positive popup control must pass before interpreting cancellation')
  const entry = await prepare('delayed', true)
  assertEq(entry.loaded, 0, 'load remains blocked on held image')
  await switchTo(spaceB)
  assertEq(entry.loaded, 0, 'no load completed before release')
  entry.hold = false
  const sent = entry.pending.filter(respondImage).length
  console.log('RELEASE', JSON.stringify({ requested: entry.requested, responsesSent: sent,
                                        alreadyCancelled: entry.closed, loaded: entry.loaded }))
  // Cancellation may close the HTTP connection before release, which is a
  // valid success, provided the target existed and the positive control passed.
  await waitFor(async () => !(await popupTargets()).length, { what: 'pending popup target destroyed' })
  await noPopupFor(1800)
  assertEq((await activeSpace()).id, spaceB, 'Space B remains presented')
  await switchTo(spaceA)
  await noPopupFor(800)
})

test('hosted popup: restore original Space and remove only owned resources', async () => {
  const errors = []
  for (const entry of cases.values()) for (const response of entry.pending) respondImage(response)
  if (ctl && originalSpace && ownedSpaces.length) {
    try { await switchTo(originalSpace) } catch (e) { errors.push(`restore: ${e.message}`) }
  }
  driver?.close(); driver = null
  // Retain owned Spaces if restoration failed; don't let delete choose an
  // unrelated fallback Space for the user.
  if (!errors.length) {
    for (const spaceId of [...ownedSpaces].reverse()) {
      try {
        assertEq((await ctl.send('agentSpace.spaces.delete', { spaceId })).ok, true, 'owned Space deleted')
      } catch (e) { errors.push(`Space ${spaceId}: ${e.message}`) }
    }
  }
  if (browser && driverTarget && !errors.length) {
    try {
      const result = await browser.send('Target.closeTarget', { targetId: driverTarget })
      assertEq(result.success, true, 'own driver target closed')
      driverTarget = null
    } catch (e) { errors.push(`driver ${driverTarget}: ${e.message}`) }
  }
  if (browser && extensionId && !errors.length) {
    try {
      await browser.send('Extensions.uninstall', { id: extensionId })
      const result = await browser.send('Extensions.getExtensions')
      assert(!result.extensions.some(e => e.id === extensionId), 'own fixture uninstalled')
    } catch (e) { errors.push(`extension ${extensionId}: ${e.message}`) }
  }
  assertEq(errors.length, 0, `cleanup errors, retained IDs above: ${errors.join('; ')}`)
})

try { await run() } finally {
  driver?.close(); browser?.close(); ctl?.close()
  if (server) { server.closeAllConnections(); server.close() }
}
