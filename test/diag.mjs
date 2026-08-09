import { openControl } from './phi-test.mjs'
import { execFileSync } from 'node:child_process'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
console.log('slots:', JSON.stringify(windows))
for (const w of windows) {
  const sp = await ctl.send('agentSpace.spaces.list', { windowId: w })
  const t = await ctl.send('agentSpace.tabs.list', { windowId: w })
  const p = await ctl.send('agentSpace.window.profile', { windowId: w })
  console.log(` slot ${w}: active space=${sp.spaces.find(s=>s.active)?.id} profile=${p.id} tabs=${t.tabs.length} activeTab=${t.tabs.findIndex(x=>x.active)}`)
}
ctl.close()
const out = execFileSync('powershell.exe', ['-NoProfile','-Command',
  '(Get-Process phi_client | Where-Object MainWindowHandle -ne 0 | Select-Object -ExpandProperty MainWindowHandle) -join ","'],
  {encoding:'utf8'})
console.log('phi_client main windows:', out.trim())
