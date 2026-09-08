// Post-cleanup state check: groups + spaces + settings window.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const g = await ctl.send('agentSpace.tabGroups.list', { windowId: w })
console.log('groups:', JSON.stringify(g.groups ?? g))
const sp = await ctl.send('agentSpace.spaces.list', { windowId: w })
console.log('spaces:', sp.spaces.map(s => s.name).join(','))
const t = await ctl.send('agentSpace.tabs.list', { windowId: w })
console.log('tabs:', t.tabs.map(x => x.title).join(' | '))
ctl.close()
