// Window pinning for user-Space bindings: spaces.list windowIds, listTabs /
// openTab by windowId (with the Space derived from the window), and the
// window_not_open refusals.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v))
const { spaces } = await ctl.send('agentSpace.spaces.list', {})
show('spaces:', spaces.map(s => ({ id: s.spaceId, name: s.name, isDefault: s.isDefault, active: s.isActive, windowIds: s.windowIds })))
const active = spaces.find(s => s.isActive && s.windowIds.length) || spaces.find(s => s.windowIds.length)
const wid = active.windowIds[0]
const a = await ctl.send('agentSpace.spaces.listTabs', { spaceId: active.spaceId })
console.log('listTabs by space:', a.ok, a.spaceId, a.windowId, (a.tabs || []).length)
const b = await ctl.send('agentSpace.spaces.listTabs', { windowId: wid })
console.log('listTabs by window: ok', b.ok, 'space derived', b.spaceId === active.spaceId, 'window echoed', b.windowId === wid)
show('stale window ->', await ctl.send('agentSpace.spaces.listTabs', { spaceId: active.spaceId, windowId: 424242 }))
const other = spaces.find(s => s.spaceId !== active.spaceId)
if (other) show('window shows another space ->', await ctl.send('agentSpace.spaces.listTabs', { spaceId: other.spaceId, windowId: wid }))
show('openTab by window:', await ctl.send('agentSpace.spaces.openTab', { windowId: wid, url: 'https://example.com/?pinned', activate: false }))
ctl.close()
