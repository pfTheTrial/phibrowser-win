// After the cold start: which Space came back eager, which parked, and does
// switching materialize the parked one with its tabs.
import { openControl, sleep } from '../phi-test.mjs'
const ctl = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v).slice(0, 500))
let { spaces } = await ctl.send('agentSpace.spaces.list', {})
show('spaces', spaces.map(s => ({ id: s.spaceId, name: s.name, isDefault: s.isDefault, open: s.open, windowIds: s.windowIds })))
show('ghosts', await ctl.send('agentSpace.lazyRestore.ghosts', {}))
show('windows', (await ctl.send('agentSpace.list', {})).windows)
const parked = spaces.find(s => !s.isDefault)
if (parked) {
  show('activate parked', await ctl.send('agentSpace.spaces.activate', { spaceId: parked.spaceId }))
  await sleep(4000)
  const t = await ctl.send('agentSpace.spaces.listTabs', { spaceId: parked.spaceId })
  show('parked tabs after switch', (t.tabs || []).map(x => x.url))
  show('ghosts after', await ctl.send('agentSpace.lazyRestore.ghosts', {}))
  ;({ spaces } = await ctl.send('agentSpace.spaces.list', {}))
  show('spaces after', spaces.map(s => ({ id: s.spaceId, open: s.open, windowIds: s.windowIds })))
}
ctl.close(); process.exit(0)
