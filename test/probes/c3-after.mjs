// Probe: prints the agent task list after a Finish, for the c3 lifecycle run.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const list = await ctl.send('agentSpace.list', {})
console.log('tasks after Finish:', JSON.stringify((list.tasks || []).map(t => t.taskId)))
console.log('windows:', JSON.stringify(list.windows))
ctl.close()
