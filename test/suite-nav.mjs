// Navigation suite: agentSpace.nav.* (state, back, forward, reload, stop).
import {
  test, run, assert, assertEq, sleep, openControl,
} from './phi-test.mjs'

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

/** Navigates and waits until the committed url/title reflects the marker. */
async function navigateAndSettle(ctl, windowId, marker) {
  const url = `data:text/html,<title>${marker}</title><h1>${marker}</h1>`
  await ctl.send('agentSpace.navigate', { windowId, url })
  for (let i = 0; i < 24; i++) {
    await sleep(250)
    const st = await ctl.send('agentSpace.nav.state', { windowId })
    if ((st.title || '').includes(marker) && !st.loading) return st
  }
  throw new Error(`page did not settle: ${marker}`)
}

/** Navigates to raw typed text and waits for whatever it resolves to. */
async function typeAndSettle(ctl, windowId, typed) {
  // Park on a known page first: the window may already be showing whatever the
  // typed text resolves to (the launcher opens example.com), and then "the url
  // changed" would never be true.
  const was = (await navigateAndSettle(ctl, windowId, `blank-${Date.now()}`)).url
  await ctl.send('agentSpace.navigate', { windowId, url: typed })
  for (let i = 0; i < 40; i++) {
    await sleep(250)
    const st = await ctl.send('agentSpace.nav.state', { windowId })
    // An unreachable host still COMMITS (an error page at the requested url),
    // so this settles with or without a network.
    if (!st.loading && st.url && st.url !== was) return st
  }
  throw new Error(`typed text did not resolve: ${typed}`)
}

// The client's address field hands raw text to the framework, which resolves
// it the way the omnibox would.
test('nav: a bare host typed in the address field becomes a url', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const st = await typeAndSettle(ctl, windowId, 'example.com')
    assert(/^https?:\/\/example\.com\//.test(st.url),
           `example.com resolved to a url (${st.url})`)
  } finally { ctl.close() }
})

test('nav: free text typed in the address field becomes a search', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const typed = 'phinomenon address field query'
    const st = await typeAndSettle(ctl, windowId, typed)
    assert(/^https?:\/\//.test(st.url), `went somewhere real (${st.url})`)
    assert(st.url !== typed, 'not treated as a literal url')
    assert(/phinomenon/i.test(st.url), `carries the query terms (${st.url})`)
  } finally { ctl.close() }
})

test('nav: state shape', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const st = await ctl.send('agentSpace.nav.state', { windowId })
    for (const k of ['url', 'title', 'canGoBack', 'canGoForward', 'loading']) {
      assert(k in st, `state has ${k}`)
    }
    assertEq(typeof st.canGoBack, 'boolean', 'canGoBack is boolean')
  } finally { ctl.close() }
})

test('nav: back and forward traverse history', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const one = `hist-one-${Date.now()}`
    const two = `hist-two-${Date.now()}`
    await navigateAndSettle(ctl, windowId, one)
    const afterFirst = await ctl.send('agentSpace.nav.state', { windowId })
    await navigateAndSettle(ctl, windowId, two)

    let st = await ctl.send('agentSpace.nav.state', { windowId })
    assertEq(st.canGoBack, true, 'canGoBack after two navigations')

    assertEq((await ctl.send('agentSpace.nav.back', { windowId })).ok, true, 'back ok')
    for (let i = 0; i < 24; i++) {
      await sleep(250)
      st = await ctl.send('agentSpace.nav.state', { windowId })
      if ((st.title || '').includes(one)) break
    }
    assert((st.title || '').includes(one), `back landed on first page (got ${st.title})`)
    assertEq(st.canGoForward, true, 'canGoForward after going back')

    assertEq((await ctl.send('agentSpace.nav.forward', { windowId })).ok, true, 'forward ok')
    for (let i = 0; i < 24; i++) {
      await sleep(250)
      st = await ctl.send('agentSpace.nav.state', { windowId })
      if ((st.title || '').includes(two)) break
    }
    assert((st.title || '').includes(two), `forward returned to second page (got ${st.title})`)
  } finally { ctl.close() }
})

test('nav: back is rejected with no history', async () => {
  const ctl = await openControl()
  try {
    // A fresh window has no back history.
    const before = (await ctl.send('agentSpace.list')).windows
    await ctl.send('agentSpace.newWindow')
    let windows = before
    for (let i = 0; i < 24; i++) {
      await sleep(250)
      windows = (await ctl.send('agentSpace.list')).windows
      if (windows.length > before.length) break
    }
    const fresh = windows.find((w) => !before.includes(w))
    assert(fresh, 'new window created')
    try {
      const st = await ctl.send('agentSpace.nav.state', { windowId: fresh })
      assertEq(st.canGoBack, false, 'fresh window cannot go back')
      const r = await ctl.send('agentSpace.nav.back', { windowId: fresh })
      assertEq(r.ok, false, 'back rejected with no history')
    } finally {
      await ctl.send('agentSpace.close', { windowId: fresh })
    }
  } finally { ctl.close() }
})

test('nav: reload keeps the same page', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const marker = `reload-${Date.now()}`
    const before = await navigateAndSettle(ctl, windowId, marker)
    assertEq((await ctl.send('agentSpace.nav.reload', { windowId })).ok, true, 'reload ok')
    let st
    for (let i = 0; i < 24; i++) {
      await sleep(250)
      st = await ctl.send('agentSpace.nav.state', { windowId })
      if (!st.loading && (st.title || '').includes(marker)) break
    }
    assert((st.title || '').includes(marker), 'same page after reload')
    assertEq(st.url, before.url, 'url unchanged by reload')
  } finally { ctl.close() }
})

test('nav: stop is accepted', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const r = await ctl.send('agentSpace.nav.stop', { windowId })
    assertEq(r.ok, true, 'stop accepted')
  } finally { ctl.close() }
})

test('nav: unknown window is rejected', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.nav.back', { windowId: 999999999 })
    assertEq(r.ok, false, 'unknown window rejected')
  } finally { ctl.close() }
})

run()
