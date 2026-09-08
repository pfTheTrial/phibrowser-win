// Seeds the sidebar with the Mac reference look's content: a bookmark folder
// "Untitled" holding two bookmarks (see look-setup in this session) and a
// grey group of two tabs. Runs ON the box from C:\Users\jdxin\phitest.

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const windowId = windows[0]
const tabs = async () => (await ctl.send('agentSpace.tabs.list', { windowId })).tabs

// Ungroup whatever exists, then close down to one tab.
for (const g of (await ctl.send('agentSpace.groups.list', { windowId })).groups || []) {
  await ctl.send('agentSpace.groups.ungroup', { windowId, groupId: g.id })
  await sleep(200)
}
let t = await tabs()
while (t.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId, index: t[t.length - 1].index })
  await sleep(200)
  t = await tabs()
}
const survivor = t[0].id
await ctl.send('agentSpace.tabs.new', { windowId, url: 'https://gemini.google.com/' })
await sleep(2500)
await ctl.send('agentSpace.tabs.new', { windowId, url: 'chrome://newtab' })
await sleep(1200)
t = await tabs()
const gemini = t.find((x) => (x.url || '').includes('gemini.google.com'))
const fresh = t.find((x) => x.id !== survivor && x.id !== gemini?.id)
console.log('tabs:', JSON.stringify(t.map((x) => [x.index, x.id, x.title, x.url])))
const sp = await ctl.send('agentSpace.spaces.list', {})
console.log('spaces:', JSON.stringify(sp).slice(0, 400))
const spaces = sp.spaces || []
const space = spaces.find((s) => s.active || s.isActive) || spaces[0]
const scope = { spaceId: space?.spaceId || space?.id }
const created = await ctl.send('agentSpace.tabGroups.create',
  { ...scope, tabIds: [gemini.id, fresh.id], color: 'grey' })
console.log('create:', JSON.stringify(created))
if (!created.ok) {
  console.log('fallback group:', JSON.stringify(await ctl.send('agentSpace.tabs.group', { windowId, index: gemini.index })))
  await sleep(400)
  const { groups } = await ctl.send('agentSpace.groups.list', { windowId })
  console.log('groups:', JSON.stringify(groups))
  console.log('addTabs:', JSON.stringify(await ctl.send('agentSpace.tabGroups.addTabs',
    { ...scope, token: groups[0].id, tabIds: [fresh.id] })))
}
await sleep(400)
t = await tabs()
const left = t.find((x) => x.id === survivor)
if (left) {
  await ctl.send('agentSpace.tabs.close', { windowId, index: left.index })
  await sleep(300)
}
const layout = await ctl.send('agentSpace.window.layout', { windowId })
console.log('rows:', JSON.stringify((layout.rows || []).map((r) => [r.band, r.key, r.left, r.top, r.right, r.bottom])))
console.log('final:', JSON.stringify((await tabs()).map((x) => [x.index, x.id, x.title, x.groupId])))
process.exit(0)
