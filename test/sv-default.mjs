// The default-Space role: the default is deletable once another Space
// exists, the role moves to the first remaining Space, the window survives
// (retreat before close), and the LAST Space is refused.
import { openControl, sleep } from './phi-test.mjs'
const ctl = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v))
let { spaces } = await ctl.send('agentSpace.spaces.list', {})
const def = spaces.find(s => s.isDefault)
console.log('default before:', def && def.spaceId, def && def.name, 'count', spaces.length)
const mk = await ctl.send('agentSpace.spaces.create', { name: 'SV Successor' })
show('created:', mk)
show('delete default:', await ctl.send('agentSpace.spaces.delete', { spaceId: def.spaceId }))
await sleep(2000)
;({ spaces } = await ctl.send('agentSpace.spaces.list', {}))
show('after:', spaces.map(s => ({ id: s.spaceId, name: s.name, isDefault: s.isDefault, active: s.isActive, open: s.open, windowIds: s.windowIds })))
show('windows alive:', (await ctl.send('agentSpace.list', {})).windows)
// Last-Space rule: strip down to one and try to delete it.
for (const s of spaces.slice(1)) { show('delete ' + s.spaceId + ':', await ctl.send('agentSpace.spaces.delete', { spaceId: s.spaceId })); await sleep(800) }
;({ spaces } = await ctl.send('agentSpace.spaces.list', {}))
show('remaining:', spaces.map(s => ({ id: s.spaceId, name: s.name, isDefault: s.isDefault })))
show('delete last (refused):', await ctl.send('agentSpace.spaces.delete', { spaceId: spaces[0].spaceId }))
show('windows alive:', (await ctl.send('agentSpace.list', {})).windows)
ctl.close()
