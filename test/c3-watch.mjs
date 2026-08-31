// C3: set up a live agent Space with a caption, ready for watch mode.
import { openControl, sleep } from './phi-test.mjs'
const ctl = await openControl()
const taskId = 'c3watch'
const created = await ctl.send('agentSpace.create', { taskId, agentName: 'Claude Code' })
console.log('create:', created.ok, 'windowId:', created.windowId)
await ctl.send('agentSpace.navigate', { windowId: created.windowId, url: 'https://example.com' })
await ctl.send('agentSpace.setState', { taskId, caption: 'Reading the page…' })
await sleep(500)
const list = await ctl.send('agentSpace.list', {})
console.log('tasks:', JSON.stringify((list.tasks || []).map(t => ({ taskId: t.taskId, ownership: t.ownership }))))
ctl.close()
