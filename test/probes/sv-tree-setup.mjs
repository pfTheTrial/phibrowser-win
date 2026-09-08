// Arranges the strip for a visual check of the tree-driven sidebar: one tab
// group (two members), one plain split, the rest plain. Prints listTabs.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const { spaces } = await ctl.send('agentSpace.spaces.list', {})
const active = spaces.find(s => s.isActive && s.windowIds.length) || spaces.find(s => s.windowIds.length)
const wid = active.windowIds[0]
const list = async () => (await ctl.send('agentSpace.spaces.listTabs', { windowId: wid })).tabs
const plain = (tabs) => tabs.filter(t => !t.tag && !t.splitId)
let tabs = await list()
while (plain(tabs).length < 5) {
  await ctl.send('agentSpace.spaces.openTab', { windowId: wid, url: 'https://example.com/?t=' + Date.now(), activate: false })
  await new Promise(r => setTimeout(r, 600))
  tabs = await list()
}
const p = plain(tabs)
console.log('group:', JSON.stringify(await ctl.send('agentSpace.tabs.group', { windowId: wid, index: p[0].index })))
await new Promise(r => setTimeout(r, 600))
const groups = await ctl.send('agentSpace.groups.list', { windowId: wid })
console.log('groups:', JSON.stringify(groups))
const gid = (groups.groups || [])[0]?.id || (groups.groups || [])[0]?.groupId
if (gid) {
  const r = await ctl.send('agentSpace.tabGroup.addTabs', { spaceId: active.spaceId, groupId: gid, tabIds: [p[1].tabId] })
  console.log('addTabs:', JSON.stringify(r))
}
console.log('split:', JSON.stringify(await ctl.send('agentSpace.splitView.create', { spaceId: active.spaceId, primaryTabId: p[2].tabId, secondaryTabId: p[3].tabId, layout: 'vertical' })))
await ctl.send('agentSpace.tabs.activate', { windowId: wid, index: (await list()).find(t => t.tabId === p[4].tabId).index })
await new Promise(r => setTimeout(r, 800))
console.log(JSON.stringify(await list()))
ctl.close()
