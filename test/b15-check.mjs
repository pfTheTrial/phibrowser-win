import { openControl } from './phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const sp = await ctl.send('agentSpace.spaces.list', { windowId: windows[0] })
console.log(JSON.stringify(sp.spaces))
ctl.close()
