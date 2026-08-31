import { openControl } from './phi-test.mjs'
const ctl = await openControl()
const r = await ctl.send('agentSpace.getOwnership', { taskId: 'c3watch' })
console.log('ownership now:', JSON.stringify(r))
ctl.close()
