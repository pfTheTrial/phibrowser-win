// C4: pushed agentSpace.ownershipChanged events.
import { openControl, sleep } from './phi-test.mjs'
const ctl = await openControl()
const events = []
ctl.onEvent((type, payload) => { if (type === 'agentSpace.ownershipChanged') events.push(payload) })
const taskId = 'c4own'
console.log('create:', (await ctl.send('agentSpace.create', { taskId })).ok)
console.log('handoff:', (await ctl.send('agentSpace.handoff', { taskId })).ok)
await ctl.pump(700)
console.log('takeover:', (await ctl.send('agentSpace.takeover', { taskId })).ok)
await ctl.pump(700)
console.log('events:', JSON.stringify(events))
await ctl.send('agentSpace.complete', { taskId, keep: false })
ctl.close()
