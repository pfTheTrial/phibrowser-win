// The real lazy-restore scenario: TWO Spaces, so a cold start can rebuild
// the active one eagerly and PARK the other. One window has nothing to park.
import { openControl, sleep } from './phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const show = (l, v) => console.log(l, JSON.stringify(v))

let list = await ctl.send('agentSpace.spaces.list', { windowId: w })
show('spaces:', (list.spaces || []).map(s => s.id))
if ((list.spaces || []).length < 2) {
  show('create:', await ctl.send('agentSpace.spaces.create', { windowId: w, name: 'Second' }))
  await sleep(3000)
  list = await ctl.send('agentSpace.spaces.list', { windowId: w })
  show('spaces now:', (list.spaces || []).map(s => s.id))
}
const second = (list.spaces || [])[1]
if (second) {
  // Visit it so its browser exists and lands in the session file...
  show('switch->second:', await ctl.send('agentSpace.spaces.switch',
    { windowId: w, spaceId: second.id }))
  await sleep(3000)
  // ...then go back, so Space 1 is the ACTIVE (eager) one at quit.
  show('switch->first:', await ctl.send('agentSpace.spaces.switch',
    { windowId: w, spaceId: (list.spaces || [])[0].id }))
  await sleep(2500)
}
show('ghost store:', await ctl.send('agentSpace.lazyRestore.ghosts', {}))
ctl.close()
