// pc-space-switch.mjs: the address row's page colour across a Space switch
// and back (docs: phi-page-color). Two throwaway Spaces: A shows a coloured
// page, B a new tab; after A -> B -> A the row must report A's colour again.
// Prints window.layout's pageColor at each step, then switches back to the
// Space it started in and deletes both.
// Usage: node pc-space-switch.mjs [coloured-page-url [b-page-url]]
// Screens land in the cwd as a-before/b/a-after-*.png (PhiShot).
import { openControl } from '../phi-test.mjs'
import { firstWindow, windowLayout } from '../lib/window.mjs'
import { execFileSync } from 'node:child_process'
import { sleep, waitFor, waitSpaces } from '../lib/wait.mjs'

const PAGE = process.argv[2] || 'http://192.168.100.21:8765/red.html'
// B's page: a new tab by default, or a second coloured page.
const B_PAGE = process.argv[3] || ''
const ctl = await openControl()
const made = []
let windowId = 0
let home = null
const switchTo = async (id) => {
  await ctl.send('agentSpace.spaces.switch', { windowId, spaceId: id })
  await waitSpaces(ctl, windowId, (sp) => sp.some((s) => s.id === id && s.active))
}
try {
  windowId = await firstWindow(ctl)
  const color = async () => (await windowLayout(ctl, windowId)).pageColor
  // A plain screen grab through PhiShot (no forced repaint, unlike PhiUiCap:
  // a stale band must show as stale), copied next to this probe as <name>.png.
  const px = async (name) => {
    const ps = [
      `Remove-Item C:\\Users\\jdxin\\shot.done -ErrorAction SilentlyContinue`,
      `Start-ScheduledTask -TaskName PhiShot`,
      `$d = (Get-Date).AddSeconds(20)`,
      `do { Start-Sleep -Milliseconds 200 } until ((Test-Path C:\\Users\\jdxin\\shot.done) -or (Get-Date) -gt $d)`,
      `Copy-Item C:\\Users\\jdxin\\shot.png ${name}.png -Force`,
    ].join('; ')
    execFileSync('powershell.exe', ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', ps])
    return name
  }
  const url = async () => (await ctl.send('agentSpace.nav.state', { windowId })).url
  home = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    .find((s) => s.active)
  for (const name of ['PcSwitch A', 'PcSwitch B']) {
    made.push((await ctl.send('agentSpace.spaces.create', { name })).id)
  }
  const [a, b] = made

  await switchTo(a)
  await sleep(1000)
  await ctl.send('agentSpace.navigate', { windowId, url: PAGE })
  await waitFor(async () => (await url()) === PAGE || null,
                { timeout: 15000, what: 'A to show the page' })
  const a0 = await waitFor(async () => (await color()) || null,
                           { timeout: 10000, what: 'the page colour' })
  console.log(JSON.stringify({ step: 'A before', mode: (await windowLayout(ctl, windowId)).mode,
                               url: await url(), pageColor: a0, px: await px('a-before') }))

  await switchTo(b)
  await sleep(1000)
  if (B_PAGE) {
    await ctl.send('agentSpace.navigate', { windowId, url: B_PAGE })
    await waitFor(async () => ((await url()) === B_PAGE && (await color())) || null,
                  { timeout: 15000, what: 'B to show its page colour' })
  }
  console.log(JSON.stringify({ step: 'B', url: await url(), pageColor: await color(), px: await px('b') }))

  await switchTo(a)
  const now = await px('a-after-now')
  const samples = []
  for (let i = 0; i < 8; i++) {
    samples.push(await color())
    await sleep(250)
  }
  const a1 = await color()
  const pixels = []
  for (let i = 0; i < 3; i++) {
    pixels.push(await px(`a-after-${i}`))
    await sleep(1000)
  }
  console.log(JSON.stringify({ step: 'A after', url: await url(), pageColor: a1, now, samples, pixels }))
  console.log(a1 === a0 ? 'PASS' : `FAIL: want ${a0} got ${JSON.stringify(a1)}`)
} finally {
  // Let the slide home land first: a Space closed mid-swap can survive.
  if (home) await switchTo(home.id).then(() => sleep(1500)).catch(() => {})
  for (const id of made) await ctl.send('agentSpace.spaces.delete', { spaceId: id })
  ctl.close()
}
