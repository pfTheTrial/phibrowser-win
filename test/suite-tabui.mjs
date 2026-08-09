// Tab-bar UI suite: the client's sidebar rows are driven by live framework
// state, verified in PIXELS from a PrintWindow capture of the shell window.
import { execFileSync } from 'node:child_process'
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

// Mirrors phi_window_controller.cc.
const BTN_TOP = 240, TABS_TOP = 374, ROW_H = 30, ROW_GAP = 4  // tabs sit below the Spaces chip row

/** Samples pixels on the box via a scheduled task (only the interactive
 *  session can enumerate/capture desktop windows). [[x,y],…] -> ["r,g,b",…] */
function samplePixels(points) {
  const arg = points.map(([x, y]) => `${x},${y}`).join(';')
  const ps = [
    `[Environment]::SetEnvironmentVariable('PHI_UICAP_POINTS','${arg}','User')`,
    `[Environment]::SetEnvironmentVariable('PHI_UICAP_HWND','${captureHwnd || 0}','User')`,
    `Remove-Item C:\\Users\\jdxin\\uicap.txt -ErrorAction SilentlyContinue`,
    `Start-ScheduledTask -TaskName PhiUiCap`,
    `$deadline = (Get-Date).AddSeconds(25)`,
    `do { Start-Sleep -Milliseconds 300; $t = (Get-Content C:\\Users\\jdxin\\uicap.txt -ErrorAction SilentlyContinue) } ` +
      `until (($t -and ($t -contains 'DONE')) -or (Get-Date) -gt $deadline)`,
    `$t`,
  ].join('; ')
  const out = execFileSync('powershell.exe',
    ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', ps],
    { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 })
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

// The HWND of the slot under test; pixel captures target exactly this window
// (creating a profile can open extra shell windows).
let captureHwnd = 0
export function setCaptureWindow(h) { captureHwnd = h }

const isLight = (s) => { const [r, g, b] = s.split(',').map(Number); return r > 200 && g > 200 && b > 200 }

// The capture includes the window frame (both axes), so absolute client coords
// don't map directly. Approach: sample a GRID over the sidebar, classify each
// scan line by majority across columns, calibrate the vertical offset from the
// New Window button (the tallest light block), then decide each row slot by a
// majority of its own lines. Text can darken individual samples but never a
// majority of a whole row.
const SCAN_XS = [40, 90, 140, 190]
const SCAN_STEP = 3

function gridScan() {
  const ys = []
  for (let y = BTN_TOP - 8; y < TABS_TOP + 8 * (ROW_H + ROW_GAP); y += SCAN_STEP) {
    ys.push(y)
  }
  const pts = []
  for (const y of ys) for (const x of SCAN_XS) pts.push([x, y])
  const px = samplePixels(pts)
  return ys.map((y, i) => {
    const line = px.slice(i * SCAN_XS.length, (i + 1) * SCAN_XS.length)
    return { y, light: line.filter(isLight).length >= 3, sample: line[0] }
  })
}

/** Contiguous runs of light lines: {top, bottom, len}. */
function lightRuns(scan) {
  const runs = []
  let start = -1
  for (let i = 0; i < scan.length; i++) {
    if (scan[i].light) {
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

/** Which row slots paint the active chip, plus the calibrated frame offset. */
function chipRows() {
  const scan = gridScan()
  const runs = lightRuns(scan)
  assert(runs.length >= 1, `saw the New Window button (runs: ${JSON.stringify(runs)})`)
  const button = runs.reduce((a, b) => (b.len > a.len ? b : a), runs[0])
  const offset = button.top - BTN_TOP

  const rows = []
  for (let i = 0; i < 8; i++) {
    const top = TABS_TOP + i * (ROW_H + ROW_GAP) + offset
    const bottom = top + ROW_H
    const lines = scan.filter((l) => l.y >= top + 4 && l.y <= bottom - 4)
    if (lines.length < 3) continue
    const lit = lines.filter((l) => l.light).length
    if (lit >= Math.ceil(lines.length * 0.6)) rows.push(i)
  }
  return { offset, rows, runs }
}

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  const slot = windows[0]
  // Capture the window we drive: extra shell windows can exist (e.g. Chromium
  // opens one when a profile is created).
  const h = await ctl.send('agentSpace.window.handle', { windowId: slot })
  setCaptureWindow(h.ok ? h.hwnd : 0)
  return slot
}

/** Normalizes the window to exactly two tabs with SHORT titles, first active.
 *  Makes these tests independent of whatever earlier suites left behind. */
async function normalizeTabs(ctl, windowId) {
  let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId })
  // Close extras from the end so indices stay valid.
  while (tabs.length > 1) {
    await ctl.send('agentSpace.tabs.close', { windowId, index: tabs.length - 1 })
    await sleep(200)
    ;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId }))
  }
  await ctl.send('agentSpace.navigate',
                 { windowId, url: 'data:text/html,<title>A</title>' })
  await sleep(400)
  await ctl.send('agentSpace.tabs.new',
                 { windowId, url: 'data:text/html,<title>B</title>' })
  await sleep(600)
  await ctl.send('agentSpace.tabs.activate', { windowId, index: 0 })
  await sleep(500)
  ;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId }))
  assertEq(tabs.length, 2, 'normalized to two tabs')
  assertEq(tabs.findIndex((t) => t.active), 0, 'first tab active after normalize')
  return tabs
}

test('tabui: exactly one row paints the active chip', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const tabs = await normalizeTabs(ctl, windowId)
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
    let tabs = await normalizeTabs(ctl, windowId)
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
    let tabs = await normalizeTabs(ctl, windowId)
    const before = tabs.length
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
