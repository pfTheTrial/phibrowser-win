import { openControl } from './phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const r = await ctl.send('agentSpace.navigate', { windowId: windows[0], url: 'https://example.com' })
console.log('restore:', JSON.stringify(r))
ctl.close()
