// Restarting the app under test — one copy, was four.
//
// suite-persist, suite-emoji, suite-pinned and suite-settings each carried
// the same PowerShell here-string (lane 6 §2). The order matters and is the
// reason it is worth having once: flush the coalesced store writes FIRST
// (phase 3 saves land on a 250 ms timer, so a kill before the flush loses
// the very change the test is about to assert survived), then kill, then
// WAIT for every process to go — a survivor keeps a stale shell window on
// the desktop that pixel captures then read instead of the new one.

import { execFileSync } from 'node:child_process'
import { openControl, waitForSaves } from '../phi-test.mjs'
import { waitForPing, sleep } from './wait.mjs'

const KILL = `
Get-Process Phi -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
$deadline = (Get-Date).AddSeconds(20)
while (((Get-Process Phi -ErrorAction SilentlyContinue) | Measure-Object).Count -gt 0 -and (Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 400
}
`

/** Runs a PowerShell snippet on the box (the suites run ON the box). */
export function powershell(script, opts = {}) {
  return execFileSync('powershell.exe', ['-NoProfile', '-Command', script],
                      { encoding: 'utf8', ...opts })
}

/** Kills every Phi process and waits for them to be gone. */
export async function killApp({ flush = true } = {}) {
  if (flush) { try { await waitForSaves() } catch { /* already down */ } }
  powershell(KILL, { stdio: 'ignore' })
}

/**
 * Restarts Phi and returns once its control socket answers again.
 * Throws (rather than sleeping on) when it never comes back.
 */
export async function restartApp() {
  await killApp()
  powershell('Start-Sleep 1; Start-ScheduledTask -TaskName PhiFxLaunch',
             { stdio: 'ignore' })
  await sleep(1000)     // The task's own start-up, before the first probe.
  await waitForPing(openControl)
}
