// The sidebar rigs' assertion counter — one copy, was three.
//
// sv-drag, sv-splitcell and sv-surface each grew their own `check()` +
// `failures` pair (lane 6 §6 item 6). They are top-level-await scripts
// rather than test()-registered suites because each step depends on the
// previous one's real mouse input, so they keep their own shape; what they
// share is this counter and the exit rule.
//
// Output is deliberately the same PASS/FAIL <what> the harness prints, and
// finish() writes the same JSON report run() writes, so the runner's log and
// report read alike whichever style a suite is written in.

import { writeReport } from '../phi-test.mjs'

let failures = 0
let checks = 0
const results = []

/** Records one assertion. Returns `ok` so a caller can branch on it. */
export function check(ok, what) {
  checks++
  if (!ok) failures++
  results.push({ name: what, status: ok ? 'pass' : 'fail', ms: 0 })
  console.log((ok ? 'PASS  ' : 'FAIL  ') + what)
  return !!ok
}

/** Pretty-prints a labelled value (every rig had this as `show`). */
export const show = (label, value) => console.log(label, JSON.stringify(value))

export const failureCount = () => failures

/** Prints the summary, sets the exit code and writes the JSON report. */
export function finish() {
  console.log(`\n${checks - failures} passed, ${failures} failed`)
  process.exitCode = failures === 0 ? 0 : 1
  writeReport({ pass: checks - failures, fail: failures, skip: 0,
                cases: results })
}
