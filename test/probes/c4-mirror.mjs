// C4: session-mirror wire round-trip (log in, user messages out).
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const taskId = 'c4mirror'
console.log('create:', (await ctl.send('agentSpace.create', { taskId })).ok)
// The mirror's batched forward shape, verbatim.
const log = await ctl.send('agentSpace.log', {
  taskId, touch: false,
  entries: [
    { kind: 'assistant', text: 'Mirror line one', ts: Date.now() },
    { kind: 'tool', text: 'Mirror line two', detail: 'detail text', agent: 'claude' },
  ],
})
console.log('log:', JSON.stringify(log))
// The console-command direction: inject, then drain as the daemon does.
console.log('userMessage:', JSON.stringify(await ctl.send('agentSpace.userMessage', { taskId, text: 'stop please' })))
const drained = await ctl.send('agentSpace.readUserMessages', { taskId })
console.log('readUserMessages:', JSON.stringify(drained))
const again = await ctl.send('agentSpace.readUserMessages', { taskId })
console.log('drained twice is empty:', (again.messages || []).length === 0)
const list = await ctl.send('agentSpace.list', {})
const task = (list.tasks || []).find(t => t.taskId === taskId)
console.log('task in list:', task ? JSON.stringify({ status: task.status }) : 'none')
await ctl.send('agentSpace.complete', { taskId, keep: false })
ctl.close()
