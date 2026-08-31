// The armed replay itself: an eager set of size 0 must park EVERY window of
// the previous session — the case a bool-shaped API could not express.
import { openControl, sleep } from './phi-test.mjs'
const ctl = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v))
show('tabs now:', (await ctl.send('agentSpace.tabs.list',
  { windowId: (await ctl.send('agentSpace.list')).windows[0] })).tabs.map(t => t.title.slice(0, 20)))
show('ghosts before:', await ctl.send('agentSpace.lazyRestore.ghosts', {}))
await ctl.send('agentSpace.lazyRestore.restore',
  { profile: '', armed: true, eagerWindowIds: [] })
await sleep(4000)
show('ghosts after armed-empty:', await ctl.send('agentSpace.lazyRestore.ghosts', {}))
ctl.close()
