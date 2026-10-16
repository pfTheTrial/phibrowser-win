// analytics-verify.mjs: drives the 2026-10-16 analytics parity on the box and
// prints the harness ring afterwards. Needs a --dev-hooks client (analytics.list
// is test_rig_only) and the PhiModClick scheduled task (modclick.ps1).
//   node analytics-verify.mjs space     "Spaces > New Space…" then Enter → space_created.surface
//   node analytics-verify.mjs card      notification.show, click its button → show/run
//   node analytics-verify.mjs fg        desktop click, then re-raise Phi → no Backgrounded
//   node analytics-verify.mjs list [event...]
import { openControl, sleep } from '../phi-test.mjs'
import { execSync } from 'node:child_process'
import { writeFileSync, existsSync, unlinkSync, readFileSync } from 'node:fs'

const HOME = 'C:\\Users\\jdxin\\'
async function rig(steps) {
  try { unlinkSync(HOME + 'mod.done') } catch {}
  writeFileSync(HOME + 'mod.txt', steps.join('\r\n') + '\r\n')
  execSync('schtasks /Run /TN PhiModClick', { stdio: 'ignore' })
  const deadline = Date.now() + 30000
  while (Date.now() < deadline && !existsSync(HOME + 'mod.done')) await sleep(300)
  try { return readFileSync(HOME + 'modprobe.txt', 'utf8').replace(/^\uFEFF/, '').trim() } catch { return '(no log)' }
}
const ctl = await openControl()
const mode = process.argv[2] || 'list'
async function list(...names) {
  const r = await ctl.send('agentSpace.analytics.list', {})
  const events = r.events || []
  const counts = {}
  for (const e of events) counts[e.event] = (counts[e.event] || 0) + 1
  for (const e of events) {
    if (!names.length || names.includes(e.event)) {
      console.log(`${e.module}\t${e.event}\t${JSON.stringify(e.properties || {})}`)
    }
  }
  console.log('counts', JSON.stringify(counts))
  if (r.error) console.log('error', r.error)
}
try {
  if (mode === 'space') {
    await rig(['park'])
    const clicked = await ctl.send('agentSpace.menu.click', { path: ['Spaces', 'New Space\u2026'] })
    console.log('menu.click', JSON.stringify(clicked))
    await sleep(1500)
    console.log('rig', await rig(['sleep,300', 'vk,0x0D', 'sleep,1500']))
    await sleep(2000)
    console.log('spaces', JSON.stringify((await ctl.send('agentSpace.spaces.list', {})).spaces?.map(s => s.name)))
    await list('space_created', 'first_time_action')
  } else if (mode === 'card') {
    await rig(['park'])
    const layout = await ctl.send('agentSpace.window.layout', {})
    const W = layout.content.x + layout.content.width
    const H = layout.content.y + layout.content.height
    const scale = (layout.dpi || 96) / 96
    const shown = await ctl.send('notification.show', {
      title: 'Parity check', message: 'One line.', buttonTitle: 'Run', expiresInSeconds: 120 })
    console.log('notification.show', JSON.stringify(shown))
    await sleep(1500)
    // The card: 275 wide, inset 16 from the client's bottom-right; with a
    // one-line message its button row is ~24 px tall ending 8 px above the
    // bottom edge, so the button centre is ~20 px up from the card bottom.
    const x = Math.round(W - (16 + 275 / 2) * scale)
    const y = Math.round(H - (16 + 20) * scale)
    console.log('click at', x, y, 'client', W, H)
    const got = []
    ctl.onEvent((t, p) => { if (t === 'notification.response') got.push(p) })
    console.log('rig', await rig([`click,${x},${y}`, 'sleep,800']))
    await ctl.pump(2000)
    console.log('responses', JSON.stringify(got))
    await list('notification_card_action')
  } else if (mode === 'fg') {
    // Take the foreground away with a click on the bare desktop right of the
    // parked shell, then give it back (park = SetForegroundWindow).
    console.log('rig', await rig(['park', 'sleep,500', 'sclick,2200,600', 'sleep,1500', 'park', 'sleep,1000']))
    await sleep(1000)
    await list('Application Opened', 'Application Backgrounded')
  } else if (mode === 'click') {
    // A client-px click on the shell, then the events named after it.
    console.log('rig', await rig([`click,${process.argv[3]},${process.argv[4]}`, 'sleep,1500']))
    await sleep(2000)
    console.log('spaces', JSON.stringify((await ctl.send('agentSpace.spaces.list', {})).spaces?.map(s => s.name)))
    await list(...process.argv.slice(5))
  } else if (mode === 'shot') {
    console.log('rig', await rig(['list', 'shot,screen']))
    const png = readFileSync(HOME + 'mod-screen.png')
    console.log('screen', png.readUInt32BE(16), 'x', png.readUInt32BE(20))
  } else {
    await list(...process.argv.slice(3))
  }
} finally { ctl.close(); process.exit(0) }
