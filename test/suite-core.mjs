// Core suite: the framework/client boundary, window lifecycle, navigation,
// and CDP — all driven over the client's unified socket.
import {
  test, run, assert, assertEq, sleep, openControl, cdpHttp, openCdp,
} from './phi-test.mjs'

test('control: ping', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.ping')
    assertEq(r.ok, true, 'ping ok')
  } finally { ctl.close() }
})

test('control: list returns at least one window', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.list')
    assert(Array.isArray(r.windows), 'windows is an array')
    assert(r.windows.length >= 1, 'at least one window')
  } finally { ctl.close() }
})

test('control: unknown verb reports an error', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.doesNotExist')
    assert(typeof r.error === 'string' && r.error.includes('unknown'),
           'unknown verb rejected')
  } finally { ctl.close() }
})

test('control: navigate loads a page (verified via CDP)', async () => {
  const ctl = await openControl()
  try {
    const { windows } = await ctl.send('agentSpace.list')
    const windowId = windows[0]
    const marker = `phi-nav-${Date.now()}`
    const url = `data:text/html,<title>${marker}</title><h1>${marker}</h1>`
    const r = await ctl.send('agentSpace.navigate', { windowId, url })
    assertEq(r.ok, true, 'navigate accepted')
    // Verify the page actually loaded, via CDP on the same socket.
    let seen = false
    for (let i = 0; i < 20 && !seen; i++) {
      await sleep(250)
      const targets = await cdpHttp('/json/list')
      seen = targets.some((t) => (t.title || '').includes(marker))
    }
    assert(seen, 'navigated page is visible to CDP')
  } finally { ctl.close() }
})

test('control: newWindow then close', async () => {
  const ctl = await openControl()
  try {
    const before = (await ctl.send('agentSpace.list')).windows
    await ctl.send('agentSpace.newWindow')
    let after = before
    for (let i = 0; i < 20; i++) {
      await sleep(250)
      after = (await ctl.send('agentSpace.list')).windows
      if (after.length > before.length) break
    }
    assertEq(after.length, before.length + 1, 'window count grew by one')
    const created = after.find((w) => !before.includes(w))
    assert(created, 'found the new windowId')
    await ctl.send('agentSpace.close', { windowId: created })
    let final = after
    for (let i = 0; i < 20; i++) {
      await sleep(250)
      final = (await ctl.send('agentSpace.list')).windows
      if (final.length === before.length) break
    }
    assertEq(final.length, before.length, 'window count back to baseline')
  } finally { ctl.close() }
})

test('cdp: /json/version reports the browser', async () => {
  const v = await cdpHttp('/json/version')
  assert(/Chrome\//.test(v.Browser || ''), 'Browser string present')
  assert((v.webSocketDebuggerUrl || '').includes('/devtools/browser/'),
         'browser ws url present')
})

test('cdp: evaluate JS in the page', async () => {
  const targets = await cdpHttp('/json/list')
  const page = targets.find((t) => t.type === 'page')
  assert(page, 'a page target exists')
  const cdp = await openCdp(new URL(page.webSocketDebuggerUrl).pathname)
  try {
    const r = await cdp.send('Runtime.evaluate',
                             { expression: '1+1', returnByValue: true })
    assertEq(r.result.value, 2, 'JS evaluated')
  } finally { cdp.close() }
})

run()
