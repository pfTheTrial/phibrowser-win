// Bounded waits — the replacement for `await sleep(600)`.
//
// The suites carried 98 fixed sleeps against 34 poll loops (lane 6 §6 item
// 4). A fixed sleep is a bet on how fast the box is today; a poll with a
// predicate states what the test is actually waiting FOR, finishes as soon
// as it is true, and fails with that sentence when it never becomes true.
//
// Every helper here takes the same options: {timeout, interval, what}.

export const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

/**
 * Polls `pred` until it returns a truthy value, and returns that value.
 * Throws `waited <timeout>ms for <what>` when it never does. A predicate
 * that throws is treated as "not yet" (the app may be mid-restart); the last
 * error is quoted in the timeout message so the cause is not swallowed.
 */
export async function waitFor(pred, { timeout = 10000, interval = 150,
                                      what = 'condition' } = {}) {
  const deadline = Date.now() + timeout
  let last = null
  for (;;) {
    try {
      const v = await pred()
      if (v) return v
      last = null
    } catch (e) {
      last = e
    }
    if (Date.now() >= deadline) {
      throw new Error(`waited ${timeout}ms for ${what}` +
                      (last ? ` (last error: ${last.message})` : ''))
    }
    await sleep(interval)
  }
}

/** The window's tab list, once `pred(tabs)` holds. */
export async function waitTabs(ctl, windowId, pred, opts = {}) {
  return waitFor(async () => {
    const r = await ctl.send('agentSpace.tabs.list', { windowId })
    const tabs = r.tabs || []
    return pred(tabs) ? tabs : null
  }, { what: 'the tab list to settle', ...opts })
}

/** The window list, once `pred(windows)` holds. */
export async function waitWindows(ctl, pred, opts = {}) {
  return waitFor(async () => {
    const { windows } = await ctl.send('agentSpace.list')
    return pred(windows || []) ? windows : null
  }, { what: 'the window list to settle', ...opts })
}

/** The Space list, once `pred(spaces)` holds. */
export async function waitSpaces(ctl, windowId, pred, opts = {}) {
  return waitFor(async () => {
    const r = await ctl.send('agentSpace.spaces.list',
                             windowId ? { windowId } : {})
    const spaces = r.spaces || []
    return pred(spaces) ? spaces : null
  }, { what: 'the Space list to settle', ...opts })
}

/** Navigates the active tab and waits until the URL the app reports matches.
 *  `match` is a string (substring) or a predicate over the reported URL. */
export async function navigateAndSettle(ctl, windowId, url, match = url,
                                        opts = {}) {
  await ctl.send('agentSpace.navigate', { windowId, url })
  const ok = typeof match === 'function' ? match : (u) => u.includes(match)
  return waitFor(async () => {
    const r = await ctl.send('agentSpace.nav.state', { windowId })
    return r && r.url && ok(r.url) && !r.loading ? r : null
  }, { timeout: 20000, what: `${url} to finish loading`, ...opts })
}

/** Waits until the app answers agentSpace.ping — i.e. its control socket is
 *  up. Used after a restart and by the runner's readiness gate. */
export async function waitForPing(openControl, opts = {}) {
  return waitFor(async () => {
    const ctl = await openControl()
    try {
      const r = await ctl.send('agentSpace.ping')
      return r && r.ok ? r : null
    } finally { ctl.close() }
  }, { timeout: 45000, interval: 1000, what: 'the control socket', ...opts })
}
