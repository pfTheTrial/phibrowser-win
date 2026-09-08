// One-shot readout of what Reader View is doing right now: the strip, and the
// three verdicts the address capsule lays its button out from.
import { openControl } from '../phi-test.mjs'

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
console.log('tabs:', JSON.stringify((tabs || []).map((t) => t.url.slice(0, 100)), null, 1))
const l = await ctl.send('agentSpace.window.layout', { windowId: w })
console.log('probe', l.readerProbe, '| distillable', l.readerDistillable,
            '| readerActive', l.readerActive, '| button', JSON.stringify(l.reader))
ctl.close()
process.exit(0)
