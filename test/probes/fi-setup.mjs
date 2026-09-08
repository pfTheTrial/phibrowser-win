// Bookmark folder icons: make one folder per sample icon and set each one's
// symbol over the wire, so the sidebar can be screenshotted.
import { openControl, sleep } from '../phi-test.mjs'

const PICK = ['github', 'heart', 'fire', 'key', 'shield-check', 'default']

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
await ctl.send('agentSpace.window.setBounds', {
  windowId: w, x: 100, y: 60, width: 1500, height: 960,
})
await sleep(500)

// Clear any folders a previous round left behind.
// The tree dialect claims only payloads WITHOUT windowId.
const before = await ctl.send('agentSpace.bookmarks.list', {})
for (const row of (before.bookmarks || before.rows || [])) {
  if (row.folder || row.isFolder) {
    await ctl.send('agentSpace.bookmarks.remove', { guid: row.guid })
    await sleep(120)
  }
}

for (const icon of PICK) {
  const made = await ctl.send('agentSpace.bookmarks.addFolder', { title: icon })
  const guid = made.guid || made.bookmark?.guid
  if (!guid) { console.log('NO GUID for', icon, JSON.stringify(made)); continue }
  const res = await ctl.send('agentSpace.bookmarks.update', { guid, icon })
  console.log('set', icon, '->', JSON.stringify(res))
  await sleep(200)
}

const after = await ctl.send('agentSpace.bookmarks.list', {})
console.log('rows:', JSON.stringify(after).slice(0, 900))
const layout = await ctl.send('agentSpace.window.layout', { windowId: w })
console.log('layout:', JSON.stringify(layout))
ctl.close()
