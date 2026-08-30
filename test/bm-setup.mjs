// Bookmark manager: seed a small tree, then navigate a tab to the manager
// route so the native page mounts over the content card.
import { openControl, sleep } from './phi-test.mjs'

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
await ctl.send('agentSpace.window.setBounds', {
  windowId: w, x: 100, y: 60, width: 1500, height: 960,
})
await sleep(500)

// Start from an empty tree so the shot is deterministic.
const wipe = async () => {
  const { bookmarks = [] } = await ctl.send('agentSpace.bookmarks.list', {})
  for (const row of bookmarks) {
    await ctl.send('agentSpace.bookmarks.remove', { guid: row.guid })
    await sleep(100)
  }
}
await wipe()

const folder = async (title, icon, parentGuid) => {
  const made = await ctl.send('agentSpace.bookmarks.addFolder',
    parentGuid ? { title, parentGuid } : { title })
  const guid = made.guid || made.bookmark?.guid
  if (guid && icon) await ctl.send('agentSpace.bookmarks.update', { guid, icon })
  await sleep(150)
  return guid
}
const mark = async (title, url, parentGuid) => {
  await ctl.send('agentSpace.bookmarks.add',
    parentGuid ? { title, url, parentGuid } : { title, url })
  await sleep(150)
}

const dev = await folder('Development', 'github')
await mark('Chromium Source', 'https://source.chromium.org/', dev)
await mark('Rust Book', 'https://doc.rust-lang.org/book/', dev)
const read = await folder('Reading', 'bookmark')
await mark('Hacker News', 'https://news.ycombinator.com/', read)
await folder('Archive', 'fire')
await mark('Example Domain', 'https://example.com/')
await mark('Phi Browser', 'https://phibrowser.com/')

console.log('tree:', JSON.stringify(await ctl.send('agentSpace.bookmarks.list', {})).slice(0, 700))

// Mount the page: a tab on the manager route.
await ctl.send('agentSpace.tabs.new', { windowId: w, url: 'chrome://bookmarks/' })
await sleep(1500)
const tabs = await ctl.send('agentSpace.tabs.list', { windowId: w })
console.log('tabs:', JSON.stringify(tabs.tabs.map(t => ({ i: t.index, a: t.active, u: t.url }))))
const layout = await ctl.send('agentSpace.window.layout', { windowId: w })
console.log('layout:', JSON.stringify(layout))
ctl.close()
