// B13: open settings on the Shortcuts pane.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const o = await ctl.send('agentSpace.settings.open')
console.log('open ok:', o.ok, 'hwnd:', o.hwnd)
const s = await ctl.send('agentSpace.settings.set', { section: 'shortcuts' })
console.log('set ok:', s.ok)
const g = await ctl.send('agentSpace.settings.get')
console.log('section:', g.section)
ctl.close()
