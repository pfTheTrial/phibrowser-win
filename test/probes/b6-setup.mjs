// B6 setup: give window A a second tab and report the shell handle.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
await ctl.send('agentSpace.tabs.new', { windowId: w, url: 'https://example.org/' })
const t = await ctl.send('agentSpace.tabs.list', { windowId: w })
const h = await ctl.send('agentSpace.window.handle', { windowId: w })
console.log('slotA:', w, 'tabs:', t.tabs.length, 'hwnd:', JSON.stringify(h))
ctl.close()
