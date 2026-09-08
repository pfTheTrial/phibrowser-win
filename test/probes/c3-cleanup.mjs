// Probe: tears down whatever the c3 agent-Space watch left behind.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const t = await ctl.send('agentSpace.tabs.list', { windowId: w })
// Keep index 0; drop the stray tabs left by probe misclicks.
for (let i = t.tabs.length - 1; i >= 1; i--) await ctl.send('agentSpace.tabs.close', { windowId: w, index: i })
await ctl.send('agentSpace.navigate', { windowId: w, url: 'https://example.com' })
const after = await ctl.send('agentSpace.tabs.list', { windowId: w })
console.log('tabs now:', after.tabs.map(x => x.title).join(' | '))
ctl.close()
