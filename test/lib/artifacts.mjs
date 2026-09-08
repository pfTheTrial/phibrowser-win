// Artifact capture, run ON the box after a suite fails.
//
//   node lib/artifacts.mjs <out-dir>
//
// Writes state.json (the window list, each window's published layout, its
// tabs, the Space list, the pinned list and settings) into <out-dir>, so a
// failure that says "expected 2 tabs, got 3" can be read the next morning
// without the box still being in that state. test-box.sh runs this and then
// scp's the directory back — lane 6 §6: "no artifact capture on failure".
//
// It never throws: a wedged verb must not turn one failing suite into a
// failing run, so every probe is best-effort and records its own error.

import { mkdirSync, writeFileSync } from 'node:fs'
import { openControl, cdpHttp } from '../phi-test.mjs'

const outDir = process.argv[2] || '.'
mkdirSync(outDir, { recursive: true })

const state = { capturedAt: new Date().toISOString() }
const attempt = async (name, fn) => {
  try { state[name] = await fn() } catch (e) { state[name] = { error: e.message } }
}

let ctl = null
try {
  ctl = await openControl()
} catch (e) {
  state.control = { error: e.message }
}

if (ctl) {
  await attempt('list', () => ctl.send('agentSpace.list'))
  await attempt('spaces', () => ctl.send('agentSpace.spaces.list', {}))
  await attempt('settings', () => ctl.send('agentSpace.settings.get'))
  const windows = (state.list && state.list.windows) || []
  state.windows = {}
  for (const windowId of windows) {
    const per = {}
    for (const [key, verb] of [['layout', 'agentSpace.window.layout'],
                               ['tabs', 'agentSpace.tabs.list'],
                               ['pinned', 'agentSpace.pinned.list'],
                               ['nav', 'agentSpace.nav.state']]) {
      try { per[key] = await ctl.send(verb, { windowId }) }
      catch (e) { per[key] = { error: e.message } }
    }
    state.windows[windowId] = per
  }
  try { ctl.close() } catch { /* already gone */ }
}
await attempt('cdpTargets', () => cdpHttp('/json/list'))

writeFileSync(`${outDir}\\state.json`, JSON.stringify(state, null, 1) + '\n')
console.log(`artifacts: wrote ${outDir}\\state.json`)
process.exit(0)
