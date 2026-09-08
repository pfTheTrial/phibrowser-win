// A dense pixel fingerprint of the shell, for comparing a UI refactor
// against the build before it (docs/architecture-review.md phase 5: the
// substrate rewrite must not move a pixel). Samples a grid over the whole
// window through the PhiUiCap rig and prints "x,y r,g,b" lines, sorted, so
// two runs diff with plain text tools.
//
//   node uifingerprint.mjs [step]     (default step 40 px)
import { execFileSync } from 'node:child_process'
import { openControl, sleep } from './phi-test.mjs'

const STEP = Number(process.argv[2] || 40)
const ctl = await openControl()
const w = (await ctl.send('agentSpace.list', {})).windows[0]
await ctl.send('agentSpace.layout.set', { mode: 'performance' })
await ctl.send('agentSpace.window.setBounds',
               { windowId: w, x: 60, y: 60, width: 1200, height: 900 })
await sleep(800)
const h = await ctl.send('agentSpace.window.handle', { windowId: w })
const geo = await ctl.send('agentSpace.window.layout', { windowId: w })
ctl.close()

const pts = []
for (let y = 8; y < 860; y += STEP) {
  for (let x = 8; x < 1180; x += STEP) {
    pts.push([x + (geo.frameLeft || 0), y + (geo.frameTop || 0)])
  }
}
// The rig takes one env var, so sample in batches it can hold.
const out = []
for (let i = 0; i < pts.length; i += 120) {
  const batch = pts.slice(i, i + 120)
  const arg = batch.map(([x, y]) => `${x},${y}`).join(';')
  const ps = [
    `[Environment]::SetEnvironmentVariable('PHI_UICAP_POINTS','${arg}','User')`,
    `[Environment]::SetEnvironmentVariable('PHI_UICAP_HWND','${h.hwnd}','User')`,
    `Remove-Item C:\\Users\\jdxin\\uicap.txt -ErrorAction SilentlyContinue`,
    `Start-ScheduledTask -TaskName PhiUiCap`,
    `$deadline = (Get-Date).AddSeconds(25)`,
    `do { Start-Sleep -Milliseconds 250; $t = (Get-Content C:\\Users\\jdxin\\uicap.txt -ErrorAction SilentlyContinue) } ` +
      `until (($t -and ($t -contains 'DONE')) -or (Get-Date) -gt $deadline)`,
    `$t`,
  ].join('; ')
  const res = execFileSync('powershell.exe',
    ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', ps],
    { encoding: 'utf8' })
  for (const line of res.split(/\r?\n/)) {
    const m = /^PX (\S+) (\d+,\d+,\d+)$/.exec(line.trim())
    if (m) out.push(`${m[1]} ${m[2]}`)
  }
}
out.sort()
console.log(out.join('\n'))
console.error(`${out.length} samples`)
