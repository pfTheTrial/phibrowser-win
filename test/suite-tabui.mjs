// Tab-bar UI suite: the client's sidebar rows are driven by live framework
// state, verified in PIXELS from a PrintWindow capture of the shell window.
//
// The capture includes the window frame, so painted content sits at an offset
// from client coords. Rather than hardcode it (DPI/theme dependent), each test
// CALIBRATES against the "+ New Window" button — a known white block at a known
// client-y — and derives row positions from that.
import { execFileSync } from 'node:child_process'
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

// Mirrors phi_window_controller.cc.
const BTN_TOP = 240, BTN_BOTTOM = 284
const TABS_TOP = 304, ROW_H = 30, ROW_GAP = 4, SAMPLE_X = 42

/** Samples pixels on the box (scheduled task: only the interactive session can
 *  enumerate/capture desktop windows). [[x,y],…] -> ["r,g,b",…] */
function samplePixels(points) {
  const arg = points.map(([x, y]) => `${x},${y}`).join(';')
  // Poll for the DONE marker: the task writes the file once when finished,
  // so a read before that would race (or hit a sharing violation).
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
const isBand  = (s) => { const [r, g, b] = s.split(',').map(Number); return b > r + 30 && b > 200 }

// x=200 is inside a row but right of typical title text and left of the close
// box, so a vertical scan there sees chip fill, not glyphs.
const SCAN_X = 200

/** Vertical scan of the sidebar: [{y, px}] every 2px. */
function scanColumn() {
  const ys = []
  for (let y = BTN_TOP - 40; y < TABS_TOP + 9 * (ROW_H + ROW_GAP); y += 2) ys.push(y)
  const px = samplePixels(ys.map((y) => [SCAN_X, y]))
  return ys.map((y, i) => ({ y, px: px[i] }))
}

/** Contiguous runs of light pixels, as {top, bottom, len}. */
function lightRuns(scan) {
  const runs = []
  let start = -1
  for (let i = 0; i < scan.length; i++) {
    if (isLight(scan[i].px)) {
      if (start < 0) start = scan[i].y
    } else if (start >= 0) {
      runs.push({ top: start, bottom: scan[i].y, len: scan[i].y - start })
      start = -1
    }
  }
  if (start >= 0) {
    const last = scan[scan.length - 1].y
    runs.push({ top: start, bottom: last, len: last - start })
  }
  return runs
}

/** Calibrates the frame offset from the New Window button, then returns the
 *  row indices that paint an active chip. */
function chipRows() {
  const scan = scanColumn()
  const runs = lightRuns(scan)
  assert(runs.length >= 1, `saw the New Window button (runs: ${JSON.stringify(runs)})`)
  const button = runs[0]  // Topmost light block is the button.
  const offset = button.top - BTN_TOP
  const rows = []
  for (const run of runs.slice(1)) {
    if (run.len < ROW_H - 12) continue  // Ignore stray light pixels.
    const rowTop = run.top - offset
    const idx = Math.round((rowTop - TABS_TOP) / (ROW_H + ROW_GAP))
    if (idx >= 0) rows.push(idx)
  }
  return { offset, rows, runs }
}

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

test('tabui: exactly one row paints the active chip', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId })
    if (tabs.length < 2) {
      await ctl.send('agentSpace.tabs.new',
                     { windowId, url: 'data:text/html,<title>UI-B</title>' })
      await sleep(800)
      ;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId }))
    }
    assert(tabs.length >= 2, 'two or more tabs')
    const activeIdx = tabs.findIndex((t) => t.active)

    const { offset, rows, runs } = chipRows()
    assert(Math.abs(offset) < 200, `plausible frame offset (${offset})`)
    assertEq(rows.length, 1, `exactly one chip painted (runs: ${JSON.stringify(runs)})`)
    assertEq(rows[0], activeIdx, 'chip is on the active row')
  } finally { ctl.close() }
})

test('tabui: chip follows the active tab after activate', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId })
    assert(tabs.length >= 2, 'two or more tabs')
    const target = tabs.find((t) => !t.active)
    await ctl.send('agentSpace.tabs.activate', { windowId, index: target.index })
    await sleep(700)
    ;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId }))
    const activeIdx = tabs.findIndex((t) => t.active)
    assertEq(tabs[activeIdx].index, target.index, 'target active in state')

    const { rows, runs } = chipRows()
    assertEq(rows.length, 1, `still exactly one chip (runs: ${JSON.stringify(runs)})`)
    assertEq(rows[0], activeIdx, 'chip moved to the newly active row')
  } finally { ctl.close() }
})

test('tabui: closing a tab shrinks the row list', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId })
    const before = tabs.length
    assert(before >= 2, 'at least two tabs')
    await ctl.send('agentSpace.tabs.close', { windowId, index: tabs[before - 1].index })
    await sleep(700)
    ;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId }))
    assertEq(tabs.length, before - 1, 'one fewer tab in state')

    // The chip must be within the surviving rows, never in the vacated slot.
    const { rows, runs } = chipRows()
    assertEq(rows.length, 1, `one chip after close (runs: ${JSON.stringify(runs)})`)
    assert(rows[0] <= before - 2,
           `chip is within the surviving rows (row ${rows[0]}, ${before - 1} remain)`)
  } finally { ctl.close() }
})

run()
