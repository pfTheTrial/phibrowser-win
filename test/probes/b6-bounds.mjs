// Position both windows side by side and dump tab geometry for the drag.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
console.log('slots:', JSON.stringify(windows))
if (windows.length < 2) { console.log('NEED 2 WINDOWS'); ctl.close(); process.exit(1) }
const [a, b] = windows
await ctl.send('agentSpace.window.setBounds', { windowId: a, x: 100, y: 100, width: 900, height: 700 })
await ctl.send('agentSpace.window.setBounds', { windowId: b, x: 1400, y: 100, width: 900, height: 700 })
for (const w of [a, b]) {
  const l = await ctl.send('agentSpace.window.layout', { windowId: w })
  const t = await ctl.send('agentSpace.tabs.list', { windowId: w })
  console.log(`slot ${w}: tabs=${t.tabs.length} layout=${JSON.stringify(l)}`)
}
ctl.close()
