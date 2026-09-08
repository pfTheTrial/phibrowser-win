// Probe: turns developer mode back off and prints the window list.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
await ctl.send('agentSpace.settings.set', { developerMode: false })
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const t = await ctl.send('agentSpace.tabs.list', { windowId: w })
for (let i = t.tabs.length - 1; i >= 1; i--) await ctl.send('agentSpace.tabs.close', { windowId: w, index: i })
console.log('reset done, tabs:', (await ctl.send('agentSpace.tabs.list', { windowId: w })).tabs.length)
ctl.close()
