// Probe: removes every bookmark the bookmark-bar staging (b11-setup) left.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const list = await ctl.send('agentSpace.bookmarks.list', { windowId: w })
for (const row of list.bookmarks ?? list.rows ?? []) {
  await ctl.send('agentSpace.bookmarks.remove', { guid: row.guid })
}
const after = await ctl.send('agentSpace.bookmarks.list', { windowId: w })
console.log('bookmarks left:', JSON.stringify(after))
// Close the IANA-bound tab if it opened as an extra, keep index 0.
const t = await ctl.send('agentSpace.tabs.list', { windowId: w })
for (let i = t.tabs.length - 1; i >= 1; i--) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: i })
}
await ctl.send('agentSpace.navigate', { windowId: w, url: 'https://example.com' })
ctl.close()
