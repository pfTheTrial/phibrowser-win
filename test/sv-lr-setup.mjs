// Stage a two-Space session for the lazy-restore cold start: a second Space
// with its own tabs, then return to the default Space so IT is the active
// one at quit (the other should park on the next cold start).
import { openControl, sleep } from './phi-test.mjs'
const ctl = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v).slice(0, 400))
let { spaces } = await ctl.send('agentSpace.spaces.list', {})
const def = spaces.find(s => s.isDefault)
let other = spaces.find(s => !s.isDefault)
if (!other) { const mk = await ctl.send('agentSpace.spaces.create', { name: 'LR Parked' }); show('created', mk); other = { spaceId: mk.spaceId } }
show('activate other', await ctl.send('agentSpace.spaces.activate', { spaceId: other.spaceId }))
await sleep(2500)
show('openTab A', await ctl.send('agentSpace.spaces.openTab', { spaceId: other.spaceId, url: 'https://example.com/?parked-a' }))
show('openTab B', await ctl.send('agentSpace.spaces.openTab', { spaceId: other.spaceId, url: 'https://example.org/?parked-b' }))
await sleep(2500)
show('activate default', await ctl.send('agentSpace.spaces.activate', { spaceId: def.spaceId }))
await sleep(1500)
show('default openTab', await ctl.send('agentSpace.spaces.openTab', { spaceId: def.spaceId, url: 'https://example.net/?eager' }))
await sleep(2000)
;({ spaces } = await ctl.send('agentSpace.spaces.list', {}))
show('spaces', spaces.map(s => ({ id: s.spaceId, name: s.name, isDefault: s.isDefault, open: s.open, windowIds: s.windowIds })))
show('ghosts', await ctl.send('agentSpace.lazyRestore.ghosts', {}))
ctl.close(); process.exit(0)
