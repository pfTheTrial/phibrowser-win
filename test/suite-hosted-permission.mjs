// Permission-confirmation variant ONLY: not pre-install/post-install coverage.
// permissions_api.cc uses PERMISSIONS_PROMPT + default ShowDialog callback.
// ExtensionInstallDialogView observes uninstall and closes only a matching ID.
// ExtensionBrowserWindowHelper closes ordinary extension tabs on unload.
// Run after exclusive box handoff; no relaunch, native automation or network.
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { randomUUID } from 'node:crypto'
import { test, run, assert, assertEq, sleep, openControl, openCdp, cdpHttp } from './phi-test.mjs'
import { waitFor, waitSpaces } from './lib/wait.mjs'

const fixture = path.resolve(path.dirname(fileURLToPath(import.meta.url)),
                             'fixtures', 'hosted-permission-extension')
const fixtureName = 'Phi Hosted Optional Permission Isolated Fixture'
const normalize = p => path.win32.normalize(p || '').toLowerCase()
async function ok(ctl, method, params = {}) {
  const result = await ctl.send(method, params)
  assertEq(result.ok, true, `${method}: ${JSON.stringify(result)}`)
  return result
}
async function evaluate(driver, expression, options = {}) {
  const result = await driver.send('Runtime.evaluate', {
    expression, returnByValue: true, ...options,
  })
  assert(!result.exceptionDetails, `evaluate: ${JSON.stringify(result.exceptionDetails)}`)
  return result.result.value
}

test('hosted permission: optional-permission confirmation reveals hidden owner; uninstall releases prompt', async () => {
  assertEq(process.platform, 'win32', 'Windows-only isolated fixture')
  let ctl, browser, driver, extensionId, targetId, anchorId, windowId, spaceId, failure
  const errors = []
  const attempt = async (label, fn) => {
    try { await fn() } catch (e) { errors.push(`${label}: ${e.message}`) }
  }
  const uninstall = async () => {
    if (!extensionId) return
    await browser.send('Extensions.uninstall', { id: extensionId })
    await waitFor(async () => !(await browser.send('Extensions.getExtensions'))
      .extensions.some(e => e.id === extensionId), { what: 'owned fixture uninstall' })
    extensionId = null
  }
  try {
    ctl = await openControl()
    const version = await cdpHttp('/json/version')
    const rootPath = new URL(version.webSocketDebuggerUrl).pathname
    assert(rootPath.startsWith('/devtools/browser/'), 'root browser CDP')
    browser = await openCdp(rootPath)
    const { extensions } = await browser.send('Extensions.getExtensions')
    assert(!extensions.some(e => normalize(e.path) === normalize(fixture) || e.name === fixtureName),
           'refuse to load or uninstall a pre-existing permission fixture')
    ;({ id: extensionId } = await browser.send('Extensions.loadUnpacked', { path: fixture }))
    assert(extensionId, 'owned extension ID')
    console.log('OWNED permission fixture', extensionId)
    const marker = `phi-permission-${randomUUID()}`
    const anchorUrl = `data:text/html,${encodeURIComponent(`<title>${marker}</title><p>Owned anchor</p>`)}`
    ;({ targetId: anchorId } = await browser.send('Target.createTarget', { url: anchorUrl, newWindow: true }))
    ;({ windowId } = await browser.send('Browser.getWindowForTarget', { targetId: anchorId }))
    console.log('OWNED permission shell', windowId, 'anchor', anchorId)
    const driverUrl = `chrome-extension://${extensionId}/driver.html?run=${marker}`
    await ok(ctl, 'agentSpace.tabs.new', { windowId, url: driverUrl })
    const target = await waitFor(async () => (await cdpHttp('/json/list'))
      .find(t => t.url === driverUrl && t.webSocketDebuggerUrl), { what: 'owned permission driver' })
    targetId = target.id
    driver = await openCdp(new URL(target.webSocketDebuggerUrl).pathname)
    await waitFor(() => evaluate(driver, `
      globalThis.chrome?.runtime?.id === ${JSON.stringify(extensionId)} &&
      typeof globalThis.chrome?.permissions?.request === 'function' &&
      typeof globalThis.chrome?.permissions?.contains === 'function' &&
      typeof globalThis.hostedPermissionAudit?.request === 'function'
    `), { what: 'committed permission fixture context, APIs and driver script ready' })
    const mapped = await browser.send('Browser.getWindowForTarget', { targetId })
    assertEq(mapped.windowId, windowId, 'driver belongs to isolated shell A')
    assertEq(await evaluate(driver, "chrome.permissions.contains({permissions:['tabs']})",
                            { awaitPromise: true }), false, 'tabs not already granted')
    const initial = await ok(ctl, 'agentSpace.spaces.list', { windowId })
    const a = initial.spaces.find(s => s.active && s.windowIds.includes(windowId))
    assert(a, 'driver Space A identified')
    const b = await ok(ctl, 'agentSpace.spaces.create', { name: marker })
    spaceId = b.id
    assert(spaceId, 'owned Space B')
    await ok(ctl, 'agentSpace.spaces.switch', { windowId, spaceId })
    await waitSpaces(ctl, windowId, sp => sp.some(s => s.id === spaceId && s.active && s.open),
                     { what: 'Space B selected before request from hidden A' })
    // Do NOT await the permission Promise: the native prompt stays pending.
    const started = await evaluate(driver, 'hostedPermissionAudit.request()', { userGesture: true })
    console.log('PERMISSION request-start', JSON.stringify(await evaluate(driver,
      '({result:hostedPermissionAudit.result,requestInfo:hostedPermissionAudit.requestInfo})')))
    assertEq(started.state, 'pending', `request started: ${JSON.stringify(started)}`)
    try {
      await waitSpaces(ctl, windowId, sp => sp.some(s => s.id === a.id && s.active && s.open),
                       { timeout: 10000, interval: 200,
                         what: 'permission confirmation to select owning Space A' })
    } catch (error) {
      // Capture BEFORE uninstall destroys the driver. Each helper has its own
      // transport timeout; do not retry request() or await its pending Promise.
      const diagnostic = { extensionId, windowId, targetId, ownerSpace: a.id, otherSpace: spaceId }
      try {
        diagnostic.driver = await evaluate(driver, `({
          result: globalThis.hostedPermissionAudit?.result,
          requestInfo: globalThis.hostedPermissionAudit?.requestInfo,
          runtimeId: globalThis.chrome?.runtime?.id,
          url: location.href, visibility: document.visibilityState,
          focused: document.hasFocus(),
          userActivation: {isActive:navigator.userActivation.isActive,
                           hasBeenActive:navigator.userActivation.hasBeenActive}
        })`)
      } catch (e) { diagnostic.driverError = e.message }
      try {
        diagnostic.spaces = (await ok(ctl, 'agentSpace.spaces.list', { windowId })).spaces
          .map(s => ({id:s.id, active:s.active, open:s.open, windowIds:s.windowIds}))
      } catch (e) { diagnostic.spacesError = e.message }
      console.log('PERMISSION owner-timeout', JSON.stringify(diagnostic))
      throw new Error(`${error.message}; diagnostic=${JSON.stringify(diagnostic)}`)
    }
    for (let i = 0; i < 3; ++i) {
      const result = await evaluate(driver, 'hostedPermissionAudit.result')
      assertEq(result.state, 'pending', `confirmation remains unresolved: ${JSON.stringify(result)}`)
      await sleep(100)
    }
    // Cancellation is uninstall of ONLY this run's returned unpacked ID.
    await uninstall()
    await waitFor(async () => !(await cdpHttp('/json/list')).some(t => t.id === targetId),
                  { what: 'uninstall to close permission driver' })
    await ok(ctl, 'agentSpace.ping')
    await ok(ctl, 'agentSpace.window.bounds', { windowId })
    await ok(ctl, 'agentSpace.spaces.switch', { windowId, spaceId })
    await waitSpaces(ctl, windowId, sp => sp.some(s => s.id === spaceId && s.active),
                     { what: 'shell usable after permission fixture uninstall' })
    await ok(ctl, 'agentSpace.spaces.switch', { windowId, spaceId: a.id })
    await waitSpaces(ctl, windowId, sp => sp.some(s => s.id === a.id && s.active),
                     { what: 'return to anchor Space before cleanup' })
  } catch (e) { failure = e }
  finally {
    // Cancel the prompt before asking the shell to close, including failures.
    if (extensionId && browser) await attempt('uninstall own fixture', uninstall)
    if (!extensionId && windowId && ctl) await attempt('close own shell', async () => {
      await ok(ctl, 'agentSpace.close', { windowId })
      await waitFor(async () => !(await cdpHttp('/json/list')).some(t => t.id === anchorId),
                    { what: 'owned permission shell to close' })
    })
    else if (!extensionId && anchorId && browser) await attempt('close own unbound anchor', async () => {
      assertEq((await browser.send('Target.closeTarget', { targetId: anchorId })).success, true,
               'owned anchor closed')
    })
    if (!extensionId && spaceId && ctl) await attempt('delete own Space B', async () => {
      await ok(ctl, 'agentSpace.spaces.delete', { spaceId })
      await waitFor(async () => !(await ok(ctl, 'agentSpace.spaces.list')).spaces
        .some(s => s.id === spaceId), { what: 'owned permission Space deleted' })
    })
    driver?.close()
    browser?.close()
    ctl?.close()
  }
  if (failure || errors.length) throw new Error([
    failure?.message, ...errors.map(e => `cleanup: ${e}`),
    ...(extensionId ? [`Retained fixture ${extensionId}, shell ${windowId}, Space ${spaceId} for recovery`] : []),
  ].filter(Boolean).join('\n'))
})

run()
