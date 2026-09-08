// Window helpers the suites used to carry a copy of each.
//
// `firstWindow` alone existed fifteen times, byte-identical in thirteen of
// them (lane 6 §2). The two pixel suites wrap it with their own layout
// pinning; that wrapper now calls this one.

import { waitFor, waitTabs } from './wait.mjs'

/** The window slot every suite drives: the first one the app reports. */
export async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

/** The window of the ACTIVE Space — what the sidebar rigs drive, because a
 *  rig aims at rows that only the active Space renders. */
export async function activeWindow(ctl) {
  const { spaces } = await ctl.send('agentSpace.spaces.list', {})
  const active = (spaces || []).find((s) => s.isActive && s.windowIds &&
                                            s.windowIds.length) ||
                 (spaces || []).find((s) => s.windowIds && s.windowIds.length)
  if (!active) throw new Error('no Space has a window')
  return active.windowIds[0]
}

/** The window's published geometry (rows, urlField, dpi, frame insets). */
export async function windowLayout(ctl, windowId) {
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  if (!l.ok) throw new Error('window.layout refused')
  return l
}

/** The window's HWND, or 0 on a build that refuses the verb. */
export async function windowHandle(ctl, windowId) {
  const h = await ctl.send('agentSpace.window.handle', { windowId })
  return h && h.ok ? h.hwnd : 0
}

/**
 * Closes every tab `pred(tab)` accepts, newest first, never going below
 * `keep` tabs. The suites each carried this loop with a fixed sleep after
 * every close; the wait here is for the list to actually shrink, which is
 * what the sleep was standing in for. Closing shifts the list, so the list
 * is re-read every round rather than indexed into.
 */
export async function closeMatching(ctl, windowId, pred, { keep = 1 } = {}) {
  for (let guard = 0; guard < 60; guard++) {
    const { tabs } = await ctl.send('agentSpace.tabs.list', { windowId })
    const list = tabs || []
    if (list.length <= keep) return list
    let victim = null
    for (let i = list.length - 1; i >= 0; --i) {
      if (pred(list[i])) { victim = list[i]; break }
    }
    if (!victim) return list
    await ctl.send('agentSpace.tabs.close', { windowId, index: victim.index })
    await waitTabs(ctl, windowId, (t) => t.length < list.length,
                   { timeout: 5000,
                     what: 'the closed tab to leave the list' })
  }
  throw new Error('closeMatching: tabs kept coming back')
}

/**
 * Closes tabs until `keep` are left — the "close down to one, re-list after
 * each close" loop that appeared about twenty-one times. Closing shifts the
 * list, so the list is re-read every round rather than indexed into.
 * Returns the surviving tab list.
 */
export async function closeToOneTab(ctl, windowId, { keep = 1 } = {}) {
  for (let guard = 0; guard < 60; guard++) {
    const { tabs } = await ctl.send('agentSpace.tabs.list', { windowId })
    if (!tabs || tabs.length <= keep) return tabs || []
    const last = tabs[tabs.length - 1]
    await ctl.send('agentSpace.tabs.close',
                   last.index !== undefined ? { windowId, index: last.index }
                                            : { windowId, tabId: last.tabId })
    await waitTabs(ctl, windowId, (t) => t.length < tabs.length,
                   { timeout: 5000,
                     what: 'the closed tab to leave the list' })
  }
  throw new Error('closeToOneTab: tabs kept coming back')
}

/**
 * The state the pixel suites need before they can read anything: one known
 * window size, one known layout mode, and (optionally) one known theme.
 * Every pixel suite pinned these inline with the same comment about another
 * suite having left the client in Balanced.
 */
export async function normalizeWindow(
    ctl, windowId,
    { bounds = { x: 60, y: 60, width: 1200, height: 900 },
      mode = 'performance', theme = null } = {}) {
  if (bounds) await ctl.send('agentSpace.window.setBounds',
                             { windowId, ...bounds })
  if (mode) await ctl.send('agentSpace.layout.set', { mode })
  if (theme) await ctl.send('agentSpace.theme.set', theme)
  // The relayout is asynchronous; wait for the geometry to report the mode
  // we asked for rather than sleeping a guessed 400 ms.
  return waitFor(async () => {
    const l = await ctl.send('agentSpace.window.layout', { windowId })
    return l.ok && (!mode || l.mode === mode) ? l : null
  }, { timeout: 8000,
       what: `the window to settle into ${mode || 'its layout'}` })
}

/** The frame insets a PrintWindow capture needs: client (0,0) sits this far
 *  into the WINDOW bitmap. */
export const frameInsets = (layout) => ({ x: layout.frameLeft || 0,
                                          y: layout.frameTop || 0 })
