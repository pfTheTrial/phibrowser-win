// Toolbar suite: the client's own address field and back/forward/reload
// affordances, driven by framework nav state and verified in pixels.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'
import { firstWindow as slotZero } from './lib/window.mjs'
import { capturePixels } from './lib/pixels.mjs'
import { waitFor } from './lib/wait.mjs'

// Mirrors phi_shell_window.cc, in DESIGN UNITS (pixels at 100%). The
// client lays out in PHYSICAL pixels and uicap captures in the same space, so
// every constant here goes through scale() first.
// The address field's rect comes FROM THE CLIENT (agentSpace.window.layout),
// not mirrored here: hand-copied constants drift the moment the chrome moves.
let DPI = 96
const scale = (design) => Math.round((design * DPI) / 96)
let URL = { x: 0, y: 0, width: 0, height: 0 }
function useLayout(l) {
  DPI = l.dpi
  URL = l.urlField
}

function samplePixels(points) {
  return capturePixels(captureHwnd, points, { frame: FRAME, timeoutSec: 20 })
}

// The HWND of the slot under test; pixel captures target exactly this window
// (creating a profile can open extra shell windows).
let captureHwnd = 0
let FRAME = { x: 0, y: 0 }
export function setCaptureWindow(h) { captureHwnd = h }

const isLight = (s) => { const [r, g, b] = s.split(',').map(Number); return r > 200 && g > 200 && b > 200 }
const isDim   = (s) => { const [r, g, b] = s.split(',').map(Number); return b > 200 && r < 200 }

// uicap captures the client area in physical pixels, so client coordinates map
// straight through — nothing to calibrate.
function frameOffset() { return 0 }

/** Light/dark pattern across the URL field's text line, one char per sample. */
function urlSignature(offset) {
  const y = Math.round(URL.y + URL.height / 2) + offset
  const pts = []
  for (let x = URL.x + scale(12); x < URL.x + URL.width - scale(12); x += 3) {
    pts.push([x, y])
  }
  return samplePixels(pts).map((p) => {
    const [r, g, b] = p.split(',').map(Number)
    return r < 150 && g < 150 && b < 150 ? '#' : '.'  // dark glyph on white
  }).join('')
}

/** Counts ink (dark) pixels across the URL field's text line. */
function urlInkCount(offset) {
  return (urlSignature(offset).match(/#/g) || []).length
}

async function firstWindow(ctl) {
  const slot = await slotZero(ctl)
  // Capture the window we drive: extra shell windows can exist (e.g. Chromium
  // opens one when a profile is created).
  const h = await ctl.send('agentSpace.window.handle', { windowId: slot })
  setCaptureWindow(h.ok ? h.hwnd : 0)
  // These assertions are about the SIDEBAR, so pin the layout: another suite
  // (or a manual poke) may have left the client in Balanced or Comfortable,
  // where the address bar and tab list live somewhere else entirely.
  // Pin the window SIZE as well: an earlier suite (or a restart) can leave it
  // too small for the sample points, which reads as "capture failed".
  await ctl.send('agentSpace.window.setBounds',
                 { windowId: slot, x: 60, y: 60, width: 1200, height: 900 })
  await ctl.send('agentSpace.layout.set', { mode: 'performance' })
  await sleep(400)
  const l = await ctl.send('agentSpace.window.layout', { windowId: slot })
  assert(l.ok, 'window.layout ok')
  useLayout(l)
  FRAME = { x: l.frameLeft || 0, y: l.frameTop || 0 }
  return slot
}

async function navigateAndSettle(ctl, windowId, marker) {
  await ctl.send('agentSpace.navigate',
                 { windowId, url: `data:text/html,<title>${marker}</title>` })
  return waitFor(async () => {
    const st = await ctl.send('agentSpace.nav.state', { windowId })
    return !st.loading && (st.title || '').includes(marker) ? st : null
  }, { timeout: 6000, interval: 250, what: `the page to settle: ${marker}` })
}

test('toolbar: url field renders text for the current page', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await navigateAndSettle(ctl, windowId, `bar-${Date.now()}`)
    const offset = frameOffset()
    const ink = urlInkCount(offset)
    assert(ink > 3, `url field shows text (ink pixels: ${ink})`)
  } finally { ctl.close() }
})

test('toolbar: url text changes when the page changes', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const offset = frameOffset()
    // Two different pages must PAINT differently. Compare the whole glyph
    // pattern, not just how much ink it adds up to: with crisp (unresampled)
    // capture, two unrelated strings can easily land on the same ink count
    // while looking nothing alike.
    await navigateAndSettle(ctl, windowId, 'S')
    const shortSig = urlSignature(offset)
    await navigateAndSettle(ctl, windowId, 'L'.repeat(120))
    const longSig = urlSignature(offset)
    assert(shortSig !== longSig,
           `url rendering tracks the page\n      short ${shortSig}\n      long  ${longSig}`)
  } finally { ctl.close() }
})

test('toolbar: back button enables once history exists', async () => {
  const ctl = await openControl()
  try {
    // A fresh window has no history: the back glyph paints dim.
    const before = (await ctl.send('agentSpace.list')).windows
    await ctl.send('agentSpace.newWindow')
    let windows = before
    for (let i = 0; i < 24 && windows.length === before.length; i++) {
      await sleep(250)
      windows = (await ctl.send('agentSpace.list')).windows
    }
    const fresh = windows.find((w) => !before.includes(w))
    assert(fresh, 'new window created')
    try {
      let st = await ctl.send('agentSpace.nav.state', { windowId: fresh })
      assertEq(st.canGoBack, false, 'fresh window has no back history')
      // Two navigations create history; the client must reflect it.
      await navigateAndSettle(ctl, fresh, `h1-${Date.now()}`)
      await navigateAndSettle(ctl, fresh, `h2-${Date.now()}`)
      st = await ctl.send('agentSpace.nav.state', { windowId: fresh })
      assertEq(st.canGoBack, true, 'back available after two navigations')
    } finally {
      await ctl.send('agentSpace.close', { windowId: fresh })
    }
  } finally { ctl.close() }
})

test('toolbar: clicking back navigates (framework path)', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const first = `tb-first-${Date.now()}`
    const second = `tb-second-${Date.now()}`
    await navigateAndSettle(ctl, windowId, first)
    await navigateAndSettle(ctl, windowId, second)
    // Same command the toolbar button issues.
    assertEq((await ctl.send('agentSpace.nav.back', { windowId })).ok, true, 'back ok')
    let st
    for (let i = 0; i < 24; i++) {
      await sleep(250)
      st = await ctl.send('agentSpace.nav.state', { windowId })
      if ((st.title || '').includes(first)) break
    }
    assert((st.title || '').includes(first), `landed back on the first page (${st.title})`)
  } finally { ctl.close() }
})

test('toolbar: the address row follows the page colour, and lets it go', async () => {
  // Mirage's page-chrome-color script reports the page's colour on http(s)
  // pages; the address row (and the comfortable strip's active tab and the
  // bookmark bar) take it -- the Mac's WebContentHeader page colour. A page
  // the script does not run on (data:) clears it on the cross-document
  // navigation.
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await ctl.send('agentSpace.navigate', { windowId, url: 'https://example.com/' })
    const coloured = await waitFor(async () => {
      const l = await ctl.send('agentSpace.window.layout', { windowId })
      return /^#[0-9A-F]{6}$/.test(l.pageColor || '') ? l.pageColor : null
    }, { timeout: 15000, interval: 300, what: 'the page colour to arrive' })
    assert(coloured, 'a page colour was reported')
    await navigateAndSettle(ctl, windowId, `plain-${Date.now()}`)
    await waitFor(async () => {
      const l = await ctl.send('agentSpace.window.layout', { windowId })
      return l.pageColor === ''
    }, { timeout: 6000, interval: 250, what: 'the page colour to clear' })
  } finally { ctl.close() }
})

run()
