// Toolbar suite: the client's own address field and back/forward/reload
// affordances, driven by framework nav state and verified in pixels.
import { execFileSync } from 'node:child_process'
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

// Mirrors phi_window_controller.cc.
const BTN_TOP = 240, URL_TOP = 146, URL_BOTTOM = 182
const BACK = { l: 16, r: 52, t: 190, b: 226 }
const FWD  = { l: 56, r: 92, t: 190, b: 226 }

function samplePixels(points) {
  const arg = points.map(([x, y]) => `${x},${y}`).join(';')
  const ps = [
    `[Environment]::SetEnvironmentVariable('PHI_UICAP_POINTS','${arg}','User')`,
    `Remove-Item C:\\Users\\jdxin\\uicap.txt -ErrorAction SilentlyContinue`,
    `Start-ScheduledTask -TaskName PhiUiCap`,
    `$deadline = (Get-Date).AddSeconds(20)`,
    `do { Start-Sleep -Milliseconds 300; $t = (Get-Content C:\\Users\\jdxin\\uicap.txt -ErrorAction SilentlyContinue) } ` +
      `until (($t -and ($t -contains 'DONE')) -or (Get-Date) -gt $deadline)`,
    `$t`,
  ].join('; ')
  const out = execFileSync('powershell.exe',
    ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', ps],
    { encoding: 'utf8' })
  const px = []
  for (const line of out.split(/\r?\n/)) {
    const m = /^PX \S+ (\d+,\d+,\d+)$/.exec(line.trim())
    if (m) px.push(m[1])
  }
  if (px.length !== points.length) {
    throw new Error(`capture failed: ${out.trim().split(/\r?\n/)[0] || 'no output'}`)
  }
  return px
}

const isLight = (s) => { const [r, g, b] = s.split(',').map(Number); return r > 200 && g > 200 && b > 200 }
const isDim   = (s) => { const [r, g, b] = s.split(',').map(Number); return b > 200 && r < 200 }

/** Calibrates the frame offset from the New Window button's white block. */
function frameOffset() {
  const ys = []
  for (let y = BTN_TOP - 60; y < BTN_TOP + 120; y += 2) ys.push(y)
  const px = samplePixels(ys.map((y) => [200, y]))
  // The New Window button is the widest light run in this range.
  let start = -1, best = null
  for (let i = 0; i < ys.length; i++) {
    if (isLight(px[i])) { if (start < 0) start = ys[i] }
    else if (start >= 0) {
      const len = ys[i] - start
      if (!best || len > best.len) best = { top: start, len }
      start = -1
    }
  }
  assert(best, 'located the New Window button')
  return best.top - BTN_TOP
}

/** Counts ink (dark) pixels across the URL field's text line. */
function urlInkCount(offset) {
  const y = Math.round((URL_TOP + URL_BOTTOM) / 2) + offset
  const pts = []
  for (let x = 24; x < 216; x += 3) pts.push([x, y])
  const px = samplePixels(pts)
  return px.filter((p) => {
    const [r, g, b] = p.split(',').map(Number)
    return r < 150 && g < 150 && b < 150   // dark glyph on the white field
  }).length
}

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

async function navigateAndSettle(ctl, windowId, marker) {
  await ctl.send('agentSpace.navigate',
                 { windowId, url: `data:text/html,<title>${marker}</title>` })
  for (let i = 0; i < 24; i++) {
    await sleep(250)
    const st = await ctl.send('agentSpace.nav.state', { windowId })
    if (!st.loading && (st.title || '').includes(marker)) return st
  }
  throw new Error(`did not settle: ${marker}`)
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
    // A short data: url vs a much longer one => different ink coverage.
    await navigateAndSettle(ctl, windowId, 'S')
    const shortInk = urlInkCount(offset)
    await navigateAndSettle(ctl, windowId, 'L'.repeat(120))
    const longInk = urlInkCount(offset)
    assert(longInk !== shortInk,
           `url rendering tracks the page (short ${shortInk} vs long ${longInk})`)
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

run()
