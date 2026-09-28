// Run on Windows: node probes/omnibox-pending-navigation.mjs
// Hold HTTP responses until the framework reports the pending address. This
// catches the new-tab URL blanking before commit, without network timing.
import assert from 'node:assert/strict'
import http from 'node:http'
import { openControl, sleep } from '../phi-test.mjs'

const held = new Map()
const server = http.createServer((req, res) => {
  if (req.url === '/redirect') {
    res.writeHead(302, { Location: '/held/redirect' }).end()
  } else {
    held.set(req.url, res)
  }
})
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve))
const base = `http://127.0.0.1:${server.address().port}`
const ctl = await openControl()
const created = []
let windowId
let originalActive
const waitFor = async (read, accept) => {
  const deadline = Date.now() + 5000
  let value
  do {
    value = await read()
    if (accept(value)) return value
    await sleep(30)
  } while (Date.now() < deadline)
  assert.fail(`Timed out: ${JSON.stringify(value)}`)
}
const state = () => ctl.send('agentSpace.nav.state', { windowId })
const tabs = async () => (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
const release = path => {
  held.get(path).writeHead(200, { 'Content-Type': 'text/html', 'Cache-Control': 'no-store' })
    .end('<title>Pending address test</title><body>Committed</body>')
  held.delete(path)
}
try {
  windowId = (await ctl.send('agentSpace.spaces.focus', {})).windowId
  assert.ok(windowId)
  originalActive = (await tabs()).find(t => t.active)?.id
  const tab = await ctl.send('agentSpace.tabs.new', { windowId, url: 'chrome://newtab/' })
  created.push(tab.id ?? tab.tabId)
  await waitFor(state, s => !s.loading && s.url.startsWith('chrome://newtab'))
  for (const [request, destination] of [
    ['/held/from-new-tab', '/held/from-new-tab'],
    ['/held/from-page', '/held/from-page'],
    ['/redirect', '/held/redirect'],
  ]) {
    await ctl.send('agentSpace.navigate', { windowId, url: base + request })
    await waitFor(async () => held.has(destination), Boolean)
    const pending = await waitFor(state,
      s => s.loading && [base + request, base + destination].includes(s.url))
    assert.ok(pending.url, 'Pending URL must not be blank')
    release(destination)
    await waitFor(state, s => !s.loading && s.url === base + destination)
    console.log('PASS pending and committed:', request)
  }
  // A pending display must not become sticky when a load is stopped.
  await ctl.send('agentSpace.navigate', { windowId, url: base + '/held/cancel' })
  await waitFor(state, s => s.loading && s.url === base + '/held/cancel')
  await ctl.send('agentSpace.nav.stop', { windowId })
  const stopped = await waitFor(state, s => !s.loading)
  assert.equal(stopped.url, base + '/held/redirect')
  console.log('PASS stopped navigation:', stopped.url)
} finally {
  if (windowId) {
    for (const id of created) {
      const tab = (await tabs()).find(t => t.id === id)
      if (tab) await ctl.send('agentSpace.tabs.close', { windowId, index: tab.index })
    }
    const original = (await tabs()).find(t => t.id === originalActive)
    if (original) await ctl.send('agentSpace.tabs.activate', { windowId, index: original.index })
  }
  ctl.close()
  server.closeAllConnections()
  await new Promise(resolve => server.close(resolve))
}
