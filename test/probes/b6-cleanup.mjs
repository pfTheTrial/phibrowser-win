// Probe: closes the second window the b6 drag staging opened.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const b = windows[1]
const h = await ctl.send('agentSpace.window.handle', { windowId: b })
console.log('closing slot', b, 'hwnd', h.hwnd)
ctl.close()
