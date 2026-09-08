// B11: seed two bar bookmarks, then flip the "always show" pref over the
// settings channel is not exposed — the menu click does it; here we only add
// the rows the bar will render.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const a = await ctl.send('agentSpace.bookmarks.add',
    { windowId: w, title: 'Example', url: 'https://example.com/' })
const b = await ctl.send('agentSpace.bookmarks.add',
    { windowId: w, title: 'IANA', url: 'https://www.iana.org/' })
console.log('added:', a.ok, b.ok)
ctl.close()
