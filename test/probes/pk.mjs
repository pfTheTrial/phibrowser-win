// Step driver for the Peek / Reader View in split view checks (phibrowser-mac
// 89dd8a48 port). Commands run in order from argv, e.g.
//   node probes\pk.mjs reset open left.html,right.html split 0 1 state
//   node probes\pk.mjs rig "park;keydown,16;click,500,500;keyup,16;sleep,2000;list;shot,c1"
// Commands:
//   serve | unserve      start/stop pk-serve.mjs (http://127.0.0.1:8765/)
//   reset                close kiosks + extra windows, unsplit, one tab on left.html
//   open a,b,...         new tabs (bare names resolve against the server)
//   activate <i>         select the tab at strip index i
//   split <i> <j>        split the tabs at indices i (left) and j (right)
//   unsplit              dissolve every split in the window
//   ratio <r>            set the split's divider ratio
//   reader               toggle Reader View on the active tab (app menu)
//   menu A>B             click an app-menu path
//   verb <name> <k=v&..> any control verb, printed
//   rig "s1;s2;..."      PhiModClick steps (modclick.ps1), log printed
//   wait <ms>
//   state                tabs, splits, surfaces, content rect, readerActive
import { openControl, sleep } from '../phi-test.mjs'
import { firstWindow, windowLayout } from '../lib/window.mjs'
import { execSync } from 'node:child_process'
import { writeFileSync, existsSync, unlinkSync, readFileSync } from 'node:fs'
import { boxPath, TEST_DIR } from '../lib/box.mjs'

const BASE = 'http://127.0.0.1:8765/'
const url = (u) => (/^[a-z]+:/.test(u) ? u : BASE + u)

async function runSteps(steps) {
  try { unlinkSync(boxPath('mod.done')) } catch {}
  writeFileSync(boxPath('mod.txt'), steps.join('\r\n') + '\r\n')
  execSync('schtasks /Run /TN PhiModClick', { stdio: 'ignore' })
  const deadline = Date.now() + 60000
  while (Date.now() < deadline && !existsSync(boxPath('mod.done'))) await sleep(300)
  await sleep(300)
  return readFileSync(boxPath('modprobe.txt'), 'utf8').replace(/^﻿/, '').trim()
}

const args = process.argv.slice(2)
const ctl = await openControl()
try {
  const windowId = await firstWindow(ctl)
  const tabs = async () => (await ctl.send('agentSpace.tabs.list', { windowId })).tabs || []
  const short = (u) => (u || '').replace(BASE, '').replace(/^chrome:\/\//, 'chrome:').slice(0, 40)
  async function unsplitAll() {
    for (let g = 0; g < 6; g++) {
      const t = (await tabs()).find((x) => x.splitId)
      if (!t) return
      await ctl.send('agentSpace.tabs.unsplit', { windowId, index: t.index }); await sleep(500)
    }
  }
  async function state() {
    const list = await tabs()
    const L = await windowLayout(ctl, windowId)
    const { windows } = await ctl.send('agentSpace.list')
    console.log('windows', JSON.stringify(windows), 'mode', L.mode, 'dpi', L.dpi,
                'content', JSON.stringify(L.content), 'readerActive', L.readerActive,
                'surfaces', JSON.stringify(L.surfaces))
    for (const t of list) {
      console.log(`  [${t.index}] id=${t.id ?? t.tabId} ${t.active ? 'ACTIVE' : '      '} split=${t.splitId || '-'} ${short(t.url)} | ${(t.title || '').slice(0, 30)}`)
    }
  }
  for (let i = 0; i < args.length; i++) {
    const cmd = args[i]
    if (cmd === 'serve') {
      const js = `${TEST_DIR}\\probes\\pk-serve.mjs`
      execSync(`powershell -NoProfile -Command "Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine='node ${js.replace(/\\/g, '\\\\')}'} | Out-Null"`, { stdio: 'inherit' })
      await sleep(1000)
      console.log('serve: started')
    } else if (cmd === 'unserve') {
      execSync(`powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \\"Name='node.exe'\\" | Where-Object { $_.CommandLine -like '*pk-serve*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"`, { stdio: 'inherit' })
      console.log('unserve: done')
    } else if (cmd === 'reset') {
      const { windows } = await ctl.send('agentSpace.list')
      for (const w of windows) if (w !== windowId) await ctl.send('agentSpace.close', { windowId: w })
      await unsplitAll()
      await ctl.send('agentSpace.tabs.new', { windowId, url: url('left.html') }); await sleep(600)
      for (let g = 0; g < 40; g++) {
        const list = await tabs()
        if (list.length <= 1) break
        const victim = [...list].reverse().find((t) => !t.active) || list[list.length - 1]
        await ctl.send('agentSpace.tabs.close', { windowId, index: victim.index, destructive: true })
        await sleep(300)
      }
      console.log('reset: done')
    } else if (cmd === 'open') {
      for (const u of args[++i].split(',')) {
        const r = await ctl.send('agentSpace.tabs.new', { windowId, url: url(u) })
        console.log('open', u, JSON.stringify(r))
        await sleep(700)
      }
    } else if (cmd === 'activate') {
      const r = await ctl.send('agentSpace.tabs.activate', { windowId, index: Number(args[++i]) })
      console.log('activate', JSON.stringify(r)); await sleep(500)
    } else if (cmd === 'split') {
      const a = Number(args[++i]), b = Number(args[++i])
      const list = await tabs()
      const id = (t) => t.id ?? t.tabId
      const r = await ctl.send('agentSpace.splitView.create', { windowId, primaryTabId: id(list[a]), secondaryTabId: id(list[b]), layout: 'vertical' })
      console.log('split', JSON.stringify(r)); await sleep(1200)
    } else if (cmd === 'unsplit') {
      await unsplitAll(); console.log('unsplit: done')
    } else if (cmd === 'ratio') {
      const r0 = Number(args[++i])
      const { splits } = await ctl.send('agentSpace.splitView.list', { windowId })
      const r = await ctl.send('agentSpace.splitView.update', { windowId, splitId: splits[0].splitId ?? splits[0].id, ratio: r0 })
      console.log('ratio', JSON.stringify(r)); await sleep(800)
    } else if (cmd === 'reader') {
      const r = await ctl.send('agentSpace.menu.list', { windowId })
      const rows = JSON.stringify(r)
      const m = rows.match(/"title":"(Reader[^"]*|[^"]*[Rr]eader[^"]*)"/)
      const title = m ? m[1] : 'Reader view'
      const c = await ctl.send('agentSpace.menu.click', { windowId, path: [title] })
      console.log('reader', title, JSON.stringify(c)); await sleep(2500)
    } else if (cmd === 'menu') {
      const c = await ctl.send('agentSpace.menu.click', { windowId, path: args[++i].split('>') })
      console.log('menu', JSON.stringify(c)); await sleep(800)
    } else if (cmd === 'verb') {
      // Params as k=v&k=v (ssh -> PowerShell strips JSON's double quotes);
      // numbers and true/false are coerced, url values resolve like `open`.
      const name = args[++i]; const p = {}
      for (const kv of (args[++i] || '').split('&').filter(Boolean)) {
        const [k, ...rest] = kv.split('='); const v = rest.join('=')
        p[k] = /^-?\d+(\.\d+)?$/.test(v) ? Number(v) : v === 'true' ? true : v === 'false' ? false : k === 'url' ? url(v) : v
      }
      const r = await ctl.send(name, { windowId, ...p })
      console.log(name, JSON.stringify(r).slice(0, 4000))
    } else if (cmd === 'rig') {
      const log = await runSteps(args[++i].split(';'))
      console.log('rig:', log)
    } else if (cmd === 'wait') {
      await sleep(Number(args[++i]))
    } else if (cmd === 'state') {
      await state()
    } else {
      console.log('unknown command', cmd)
    }
  }
} finally { ctl.close() }
process.exit(0)
