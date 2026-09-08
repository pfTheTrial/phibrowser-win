// First-frame placement probe (the layouts canvas's one open check): after a
// fresh launch, Chromium's window bounds must equal the placement it saved
// last session — the shell anchors its content region there, per layout mode
// (the `wr.right > wr.left` conversion in phi_window_controller.cc). Run it
// right after PhiFxLaunch; prints saved vs live and the delta, all DIP.
//
//   node sv-placement.mjs <mode-label>
import { cdpHttp, openCdp } from './phi-test.mjs'
import { readFileSync } from 'node:fs'

const prefs = JSON.parse(
  readFileSync('C:\\phi-fx-test\\Default\\Preferences', 'utf8'))
const p = prefs.browser.window_placement
const saved = { left: p.left, top: p.top,
                width: p.right - p.left, height: p.bottom - p.top }

const list = await cdpHttp('/json/list')
const page = list.find((t) => t.type === 'page')
if (!page) throw new Error('no page target')
const cdp = await openCdp(new URL(page.webSocketDebuggerUrl).pathname)
const { bounds } = await cdp.send('Browser.getWindowForTarget',
                                  { targetId: page.id })
cdp.close()

const live = { left: bounds.left, top: bounds.top,
               width: bounds.width, height: bounds.height }
const delta = { left: live.left - saved.left, top: live.top - saved.top,
                width: live.width - saved.width,
                height: live.height - saved.height }
const ok = Object.values(delta).every((d) => Math.abs(d) <= 2)
console.log(JSON.stringify(
  { layout: process.argv[2] || '?', ok, saved, live, delta }))
process.exit(ok ? 0 : 1)
