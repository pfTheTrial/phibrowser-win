// Settings-pane capture rig (untracked). Runs ON the box from
// C:\Users\jdxin\phitest:  node set-shot.mjs [tag] [pane...]
//
// Switches the settings window to each pane over the control socket, then
// fires the PhiSetShot scheduled task (the capture must run in the
// INTERACTIVE session — an ssh session's window station sees no client
// windows). PNGs land in C:\Users\jdxin\setshot\<tag>\<pane>.png.
import { openControl, sleep } from '../phi-test.mjs'
import { execFileSync } from 'node:child_process'
import { existsSync, readFileSync, rmSync, mkdirSync } from 'node:fs'

const tag = process.argv[2] || 'shot'
const panes = process.argv.slice(3).length
  ? process.argv.slice(3)
  : ['account', 'general', 'profiles', 'spaces', 'navigation', 'ai',
     'shortcuts', 'developer']
const dir = `C:\\Users\\jdxin\\setshot\\${tag}`
const done = 'C:\\Users\\jdxin\\setshot\\done.txt'
mkdirSync(dir, { recursive: true })

function ps(...args) {
  return execFileSync('powershell', ['-NoProfile', '-Command', ...args],
                      { encoding: 'utf8' })
}

const ctl = await openControl()
try {
  for (const pane of panes) {
    await ctl.send('agentSpace.settings.open')
    const r = await ctl.send('agentSpace.settings.set', { section: pane })
    await sleep(500)
    rmSync(done, { force: true })
    ps(`[Environment]::SetEnvironmentVariable('PHI_SETSHOT_OUT','${dir}\\${pane}.png','User'); Start-ScheduledTask -TaskName PhiSetShot`)
    let text = null
    for (let i = 0; i < 40 && text === null; i++) {
      await sleep(400)
      if (existsSync(done)) text = readFileSync(done, 'utf8').trim()
    }
    console.log(pane, JSON.stringify(r), '->', text ?? 'TIMEOUT')
  }
} finally {
  ctl.close()
}
