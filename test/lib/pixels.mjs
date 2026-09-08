// The PhiUiCap pixel graft — one copy, was five.
//
// Only the interactive session can enumerate and capture desktop windows, so
// a suite cannot PrintWindow directly: it parks the request in two user-scope
// environment variables, kicks the PhiUiCap scheduled task, and polls the
// task's output file for its DONE sentinel. suite-layout, suite-settings,
// suite-tabui, suite-theme and suite-toolbar each spelled that out inline
// (lane 6 §2); the differences between the copies were the timeout and the
// buffer size, which are options here.

import { execFileSync } from 'node:child_process'
import { boxPath } from './box.mjs'

const OUT_FILE = boxPath('uicap.txt')

/**
 * Samples pixels from one window. `points` is [[x, y], …] in the window's
 * client space; the answer is ["r,g,b", …] in the same order.
 *
 * PrintWindow(PW_CLIENTONLY) renders from the WINDOW origin, so a client
 * coordinate lands `frame` further in — pass the insets the app publishes in
 * agentSpace.window.layout (frameLeft/frameTop) as `frame`.
 */
export function capturePixels(hwnd, points,
                              { frame = { x: 0, y: 0 }, timeoutSec = 25,
                                maxBuffer = 8 * 1024 * 1024 } = {}) {
  const arg = points
    .map(([x, y]) => `${x + (frame.x || 0)},${y + (frame.y || 0)}`)
    .join(';')
  const ps = [
    `[Environment]::SetEnvironmentVariable('PHI_UICAP_POINTS','${arg}','User')`,
    `[Environment]::SetEnvironmentVariable('PHI_UICAP_HWND','${hwnd || 0}','User')`,
    `Remove-Item ${OUT_FILE} -ErrorAction SilentlyContinue`,
    `Start-ScheduledTask -TaskName PhiUiCap`,
    `$deadline = (Get-Date).AddSeconds(${timeoutSec})`,
    `do { Start-Sleep -Milliseconds 300; $t = (Get-Content ${OUT_FILE} -ErrorAction SilentlyContinue) } ` +
      `until (($t -and ($t -contains 'DONE')) -or (Get-Date) -gt $deadline)`,
    `$t`,
  ].join('; ')
  const out = execFileSync('powershell.exe',
    ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', ps],
    { encoding: 'utf8', maxBuffer })
  const px = []
  for (const line of out.split(/\r?\n/)) {
    const m = /^PX \S+ (\d+,\d+,\d+)$/.exec(line.trim())
    if (m) px.push(m[1])
  }
  if (px.length !== points.length) {
    throw new Error(
      `capture failed: ${out.trim().split(/\r?\n/)[0] || 'no output'}`)
  }
  return px
}

/** One point, one "r,g,b". */
export function capturePixel(hwnd, x, y, opts = {}) {
  return capturePixels(hwnd, [[x, y]], opts)[0]
}

/** Colour predicates the pixel suites share. */
export const rgb = (s) => s.split(',').map(Number)
export const isLight = (s) => {
  const [r, g, b] = rgb(s)
  return r > 200 && g > 200 && b > 200
}
/** "carries a hue" — the ground paints the theme tint, the content card does
 *  not, so a coloured sample is the shell and a grey one is not. Testing for
 *  a particular hue only works for whichever theme the test was written on. */
export const isColoured = (s) => {
  const c = rgb(s)
  return Math.max(...c) - Math.min(...c) > 15
}
