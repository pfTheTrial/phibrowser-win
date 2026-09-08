// The PhiDragCap rig — one copy, was two (sv-drag, sv-surface).
//
// Real mouse input has to come from the interactive session too, so a rig
// writes a step script to drag.txt, kicks the PhiDragCap scheduled task and
// waits for the task's drag.done marker. Steps are one per line:
//
//   park                 bring the shell forward at a known position
//   down,X,Y             press at a screen point
//   move,X,Y             glide to a screen point
//   up                   release
//   sleep,MS             wait inside the task (so the app sees real timing)
//   shot,NAME            PrintWindow the shell into NAME.png
//
// The stale-marker race is why drag.done is deleted BEFORE the task starts:
// the memory notes record a run that read the previous drag's marker and
// asserted against a window that had not moved yet.

import { execSync } from 'node:child_process'
import { writeFileSync, existsSync, unlinkSync } from 'node:fs'
import { boxPath } from './box.mjs'
import { sleep } from './wait.mjs'

const SCRIPT = boxPath('drag.txt')
const DONE = boxPath('drag.done')

/** Runs one step script and returns when the task has finished it. */
export async function runSteps(steps, { timeout = 30000, settle = 800 } = {}) {
  try { unlinkSync(DONE) } catch { /* first run */ }
  writeFileSync(SCRIPT, steps.join('\r\n') + '\r\n')
  execSync('schtasks /Run /TN PhiDragCap', { stdio: 'ignore' })
  const deadline = Date.now() + timeout
  while (Date.now() < deadline && !existsSync(DONE)) await sleep(300)
  if (!existsSync(DONE)) {
    throw new Error(`PhiDragCap did not finish in ${timeout}ms`)
  }
  await sleep(settle)   // The app's own animation, after the input stops.
}

/** Brings the shell forward at a known position, so rows measured next are
 *  measured on the parked window. */
export const park = () => runSteps(['park'])

/** One real drag: press at `from`, glide to `to`, release. `name` names the
 *  screenshot the task leaves behind for triage. */
export async function drag(name, from, to, opts = {}) {
  return runSteps(['park',
                   `down,${from[0]},${from[1]}`,
                   `move,${from[0]},${from[1] + 12}`,
                   `move,${to[0]},${to[1]}`,
                   'sleep,400',
                   `shot,${name}`,
                   'up', 'sleep,600'], opts)
}

/** The centre of a published row/box rect. */
export const mid = (r) => [Math.round((r.left + r.right) / 2),
                           Math.round((r.top + r.bottom) / 2)]

/** One real click in the middle of a published rect. */
export async function clickBox(name, box, opts = {}) {
  const [x, y] = mid(box)
  return runSteps(['park', `down,${x},${y}`, 'sleep,120', 'up', 'sleep,900'],
                  opts)
}
