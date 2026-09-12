// Modifier-click link gestures, REAL input: Shift → Peek, Alt → split view,
// Ctrl+Alt → Kiosk (the Mac's Shift / Option / Cmd+Opt), on a plain anchor
// page and on a preventDefault()+window.open() page, then both switches off
// (Peek off → stock new window, Kiosk off → stock background tab). Prints
// one verdict line per case.
//
// Needs the PhiModClick scheduled task on the box: clone PhiDragCap's
// definition onto modclick.ps1 (this directory), the way mktask.ps1 clones
// PhiRm. The pages come from mc-pages/ beside this file (file: links, so no
// network), which `test-box.sh --probes` uploads. Run on the box from
// PHI_TEST_DIR: node probes\mc-matrix.mjs
import { openControl, sleep } from '../phi-test.mjs'
import { firstWindow, windowLayout, closeMatching } from '../lib/window.mjs'
import { execSync } from 'node:child_process'
import { writeFileSync, existsSync, unlinkSync, readFileSync } from 'node:fs'
import { boxPath, TEST_DIR } from '../lib/box.mjs'
async function runSteps(steps) {
  try { unlinkSync(boxPath('mod.done')) } catch {}
  writeFileSync(boxPath('mod.txt'), steps.join('\r\n') + '\r\n')
  execSync('schtasks /Run /TN PhiModClick', { stdio: 'ignore' })
  const deadline = Date.now() + 40000
  while (Date.now() < deadline && !existsSync(boxPath('mod.done'))) await sleep(300)
  await sleep(500)
  return readFileSync(boxPath('modprobe.txt'), 'utf8').replace(/^\uFEFF/, '').trim()
}
const VK = { shift: 0x10, ctrl: 0x11, alt: 0x12 }
const ctl = await openControl()
try {
  const windowId = await firstWindow(ctl)
  const tabs = async () => (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
  const kill = async (cls) => execSync(`powershell -NoProfile -Command "Add-Type -Name W -Namespace X -MemberDefinition '[DllImport(\\"user32.dll\\", CharSet=CharSet.Unicode)] public static extern System.IntPtr FindWindowW(string c, string t); [DllImport(\\"user32.dll\\")] public static extern bool PostMessageW(System.IntPtr h, uint m, int w, int l);'; $h=[X.W]::FindWindowW('${cls}', [NullString]::Value); while ($h -ne 0) { [X.W]::PostMessageW($h, 0x10, 0, 0); Start-Sleep -Milliseconds 400; $h=[X.W]::FindWindowW('${cls}', [NullString]::Value) }"`, { stdio: 'ignore' })
  async function reset() {
    // Close any kiosk, unsplit, and leave exactly one fresh source tab.
    await kill('PhiKioskShell'); await sleep(600)
    for (const t of await tabs()) if (t.splitId) { await ctl.send('agentSpace.tabs.unsplit', { windowId, index: t.index }); await sleep(400); break }
    await ctl.send('agentSpace.tabs.new', { windowId, url: 'about:blank' }); await sleep(500)
    await closeMatching(ctl, windowId, (t) => !(t.url || '').includes('about:blank'), { keep: 1 })
    const { windows } = await ctl.send('agentSpace.list')
    for (const w of windows) if (w !== windowId) await ctl.send('agentSpace.close', { windowId: w })
  }
  async function run(name, page, mods, settings) {
    await reset()
    await ctl.send('agentSpace.settings.set', settings)
    await ctl.send('agentSpace.tabs.new', { windowId, url: `file:///${TEST_DIR.replace(/\\/g, '/')}/probes/mc-pages/${page}` })
    await sleep(1200)
    await closeMatching(ctl, windowId, (t) => !(t.url || '').includes(page), { keep: 1 })
    await runSteps(['park'])
    const c = (await windowLayout(ctl, windowId)).content
    const x = Math.round(c.x + c.width / 2), y = Math.round(c.y + c.height / 2)
    const steps = ['park', ...mods.map((m) => `keydown,${VK[m]}`), `click,${x},${y}`,
                   ...[...mods].reverse().map((m) => `keyup,${VK[m]}`), 'sleep,2500', 'list', `shot,${name}`]
    const log = await runSteps(steps)
    const after = await tabs()
    const surfaces = (await windowLayout(ctl, windowId)).surfaces
    const { windows } = await ctl.send('agentSpace.list')
    const kiosk = (log.match(/PhiKioskShell hwnd=\d+ vis=1/g) || []).length
    const split = after.filter((t) => t.splitId).length === 2 && after.some((t) => t.splitId && t.url.includes('mc-target'))
    const peek = Object.values(surfaces).includes('peek')
    const newTab = after.some((t) => t.url.includes('mc-target')) && !split
    const v = { name, kiosk, split, peek, newTab, windows: windows.length, tabs: after.length }
    console.log(JSON.stringify(v))
  }
  const on = { peekViewEnabled: true, openKioskOnCtrlAltClick: true }
  for (const page of ['mc-source.html', 'mc-source-js.html']) {
    const tag = page.includes('-js') ? '-js' : ''
    await run(`shift${tag}`, page, ['shift'], on)
    await run(`alt${tag}`, page, ['alt'], on)
    await run(`ctrlalt${tag}`, page, ['ctrl', 'alt'], on)
  }
  await run('shift-peekoff', 'mc-source.html', ['shift'], { peekViewEnabled: false })
  await run('ctrlalt-kioskoff', 'mc-source.html', ['ctrl', 'alt'], { openKioskOnCtrlAltClick: false })
  await reset()
  await ctl.send('agentSpace.settings.set', on)
} finally { ctl.close() }
