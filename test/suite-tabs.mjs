// Tabs suite: agentSpace.tabs.* over the unified socket, cross-checked
// against CDP where it proves the tab really exists in the browser.
import {
  test, run, assert, assertEq, sleep, openControl, cdpHttp, openCdp,
} from './phi-test.mjs'

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  assert(windows.length >= 1, 'a window exists')
  return windows[0]
}

/** Polls tabs.list until `pred` holds, then returns the tabs. */
async function waitTabs(ctl, windowId, pred, what) {
  let tabs = []
  for (let i = 0; i < 24; i++) {
    tabs = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
    if (pred(tabs)) return tabs
    await sleep(250)
  }
  throw new Error(`timed out waiting for: ${what} (last: ${JSON.stringify(tabs)})`)
}

test('tabs: list shape', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const { tabs } = await ctl.send('agentSpace.tabs.list', { windowId })
    assert(Array.isArray(tabs) && tabs.length >= 1, 'at least one tab')
    const t = tabs[0]
    for (const k of ['index', 'title', 'url', 'active', 'loading']) {
      assert(k in t, `tab has ${k}`)
    }
    assertEq(tabs.filter((x) => x.active).length, 1, 'exactly one active tab')
  } finally { ctl.close() }
})

test('tabs: new opens a tab with the requested url', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const before = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
    const marker = `tab-${Date.now()}`
    const url = `data:text/html,<title>${marker}</title>`
    const r = await ctl.send('agentSpace.tabs.new', { windowId, url })
    assertEq(r.ok, true, 'tabs.new ok')
    assert(r.index >= 0, 'returned an index')
    const tabs = await waitTabs(ctl, windowId,
      (t) => t.length === before.length + 1 && t.some((x) => x.title.includes(marker)),
      'new tab present with title')
    const created = tabs.find((x) => x.title.includes(marker))
    assertEq(created.active, true, 'new tab is active (foreground)')
  } finally { ctl.close() }
})

test('tabs: activate switches the active tab', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    // Ensure at least two tabs.
    let tabs = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
    if (tabs.length < 2) {
      await ctl.send('agentSpace.tabs.new', { windowId, url: 'data:text/html,<title>second</title>' })
      tabs = await waitTabs(ctl, windowId, (t) => t.length >= 2, 'two tabs')
    }
    const target = tabs.find((t) => !t.active)
    assert(target, 'an inactive tab exists')
    const r = await ctl.send('agentSpace.tabs.activate', { windowId, index: target.index })
    assertEq(r.ok, true, 'activate ok')
    const after = await waitTabs(ctl, windowId,
      (t) => t.find((x) => x.index === target.index)?.active === true,
      'target tab became active')
    assertEq(after.filter((x) => x.active).length, 1, 'still exactly one active')
  } finally { ctl.close() }
})

test('tabs: close removes the tab', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const marker = `close-${Date.now()}`
    await ctl.send('agentSpace.tabs.new',
                   { windowId, url: `data:text/html,<title>${marker}</title>` })
    let tabs = await waitTabs(ctl, windowId,
      (t) => t.some((x) => x.title.includes(marker)), 'tab to close exists')
    const victim = tabs.find((x) => x.title.includes(marker))
    const countBefore = tabs.length
    const r = await ctl.send('agentSpace.tabs.close', { windowId, index: victim.index })
    assertEq(r.ok, true, 'close ok')
    tabs = await waitTabs(ctl, windowId,
      (t) => t.length === countBefore - 1 && !t.some((x) => x.title.includes(marker)),
      'tab removed')
    assertEq(tabs.filter((x) => x.active).length, 1, 'an active tab remains')
  } finally { ctl.close() }
})

test('tabs: a tab the PAGE opens (target=_blank) joins the same window', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const marker = `opener-${Date.now()}`
    await ctl.send('agentSpace.tabs.new',
                   { windowId, url: `data:text/html,<title>${marker}</title>` })
    await waitTabs(ctl, windowId, (t) => t.some((x) => x.title.includes(marker)),
                   'opener tab exists')
    const before = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
    const windowsBefore = (await ctl.send('agentSpace.list')).windows.length

    // Drive the OPENER PAGE, not the client: this is the path a real
    // target=_blank click takes, and the client only learns about it through
    // the framework's tab notification.
    const targets = await cdpHttp('/json/list')
    const page = targets.find((t) => t.type === 'page' && (t.title || '').includes(marker))
    assert(page, 'found the opener page over CDP')
    const cdp = await openCdp(new URL(page.webSocketDebuggerUrl).pathname)
    try {
      await cdp.send('Runtime.evaluate', {
        expression: `window.open('about:blank?${marker}', '_blank')`,
        userGesture: true,  // Without a gesture the popup blocker eats it.
      })
    } finally { cdp.close() }

    const tabs = await waitTabs(ctl, windowId,
      (t) => t.some((x) => x.url.includes(marker) && x.url.startsWith('about:blank')),
      'the page-opened tab appears in this window')
    assertEq(tabs.length, before.length + 1, 'exactly one tab was added')
    assertEq((await ctl.send('agentSpace.list')).windows.length, windowsBefore,
             'it became a tab here, not a new window')
    // Leave the window as we found it: indices shift on every close, so
    // re-list and close the first survivor each time.
    const mine = (t) => t.url.includes(marker) || t.title.includes(marker)
    for (;;) {
      const live = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
      const victim = live.find(mine)
      if (!victim) break
      await ctl.send('agentSpace.tabs.close', { windowId, index: victim.index })
      await sleep(150)
    }
  } finally { ctl.close() }
})

test('tabs: out-of-range index is rejected', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const a = await ctl.send('agentSpace.tabs.activate', { windowId, index: 9999 })
    assertEq(a.ok, false, 'activate rejects bad index')
    const c = await ctl.send('agentSpace.tabs.close', { windowId, index: 9999 })
    assertEq(c.ok, false, 'close rejects bad index')
  } finally { ctl.close() }
})

test('tabs: CDP sees the same tab count', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const { tabs } = await ctl.send('agentSpace.tabs.list', { windowId })
    const targets = await cdpHttp('/json/list')
    const pages = targets.filter((t) => t.type === 'page')
    // CDP lists pages across all windows, so it must be at least our count.
    assert(pages.length >= tabs.length,
           `cdp pages (${pages.length}) >= window tabs (${tabs.length})`)
  } finally { ctl.close() }
})

run()
