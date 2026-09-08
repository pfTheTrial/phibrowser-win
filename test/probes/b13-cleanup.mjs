// B13 cleanup: close the settings window, prune probe tabs down to one.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const t = await ctl.send('agentSpace.tabs.list', { windowId: w })
console.log('tabs before:', t.tabs.length)
for (let i = t.tabs.length - 1; i >= 1; i--) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: i })
}
const after = await ctl.send('agentSpace.tabs.list', { windowId: w })
console.log('tabs after:', after.tabs.length)
ctl.close()
