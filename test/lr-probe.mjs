// Lazy Space restore, phase 2: exercise the framework surface directly —
// no client state machine yet, on purpose. What matters here is that the
// RECEIPT arrives and that the three materialize outcomes stay distinct.
import { openControl, sleep } from './phi-test.mjs'

const ctl = await openControl()
const show = (label, v) => console.log(label, JSON.stringify(v))

// 1. Nothing is parked on a normal run.
show('ghosts@start:', await ctl.send('agentSpace.lazyRestore.ghosts', {}))

// 2. A ghost that was never parked must report noSuchGhost — NOT
//    refusedForNow. A caller holding a park record treats those opposite
//    ways (give up vs retry), so collapsing them is a real bug.
show('materialize(bogus):', await ctl.send('agentSpace.lazyRestore.materialize',
  { profile: 'Default', windowId: 999999 }))
show('drop(bogus):', await ctl.send('agentSpace.lazyRestore.drop',
  { profile: 'Default', windowId: 999999 }))

// 3. An UNARMED replay restores everything and parks nothing; the receipt
//    should still arrive.
await ctl.send('agentSpace.lazyRestore.restore', { profile: '', armed: false })
await sleep(2500)
show('after unarmed restore:', await ctl.send('agentSpace.lazyRestore.ghosts', {}))

// 4. An ARMED replay with an eager set that matches nothing parks the whole
//    session — restoredAny stays true (a replay STARTED) while
//    eagerFilterMatchedNothing reports the stale set. The two are separate
//    on purpose.
await ctl.send('agentSpace.lazyRestore.restore',
  { profile: '', armed: true, eagerWindowIds: [424242] })
await sleep(3000)
show('after armed-stale restore:', await ctl.send('agentSpace.lazyRestore.ghosts', {}))
ctl.close()
