// incognito-route.mjs: a URL rule targeting Incognito routes into an
// Incognito Space, creating one when none is live (docs/incognito-space-
// port.md D8). Adds a temporary example.org rule, navigates the regular
// Space's active page there from script (a renderer-initiated navigation,
// which the routing throttle sees — tabs.new is exempt), reports where the
// URL landed, then deletes the rule.
import { openControl, cdpHttp, openCdp, sleep } from '../phi-test.mjs'
import { firstWindow } from '../lib/window.mjs'
const host = 'example.org'
const ctl = await openControl()
try {
  const windowId = await firstWindow(ctl)
  const before = await ctl.send('agentSpace.spaces.focus', { windowId })
  console.log('before', JSON.stringify({ space: before.spaceName, incognito: before.isIncognito }))
  await ctl.send('agentSpace.urlRules.add', { spaceId: 'space.incognito', host })
  await ctl.send('agentSpace.tabs.new', { windowId, url: 'https://example.com/' })
  await sleep(3000)
  const page = (await cdpHttp('/json/list')).find((t) => t.type === 'page' && t.url === 'https://example.com/')
  const cdp = await openCdp(new URL(page.webSocketDebuggerUrl).pathname)
  await cdp.send('Runtime.evaluate', { expression: `location.href = 'https://${host}/'` })
  await sleep(6000)
  const after = await ctl.send('agentSpace.spaces.focus', { windowId })
  console.log('after', JSON.stringify({ space: after.spaceName, incognito: after.isIncognito }))
  const left = (await cdpHttp('/json/list')).filter((t) => t.type === 'page' && t.url.includes(host)).length
  console.log('example.org pages visible to CDP (regular profile):', left)
} finally {
  const list = await ctl.send('agentSpace.urlRules.list', {})
  for (const r of (list.rules || []).filter((r) => r.host === host)) {
    await ctl.send('agentSpace.urlRules.delete', { id: r.id })
  }
  ctl.close()
  process.exit(0)
}
