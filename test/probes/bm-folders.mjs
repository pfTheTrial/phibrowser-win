// Seeds the sidebar bookmark band with the design's folder rows (Work with
// two children + a nested folder, Reading, two root bookmarks), parks the
// window and prints the folder rows' client rects so a rig can click one.
import { openControl, sleep } from '../phi-test.mjs'

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const windowId = windows[0]
const { bookmarks = [] } = await ctl.send('agentSpace.bookmarks.list', {})
for (const row of bookmarks) {
  await ctl.send('agentSpace.bookmarks.remove', { guid: row.guid })
  await sleep(80)
}
const folder = async (title, parentGuid) => {
  const made = await ctl.send('agentSpace.bookmarks.addFolder',
    parentGuid ? { title, parentGuid } : { title })
  await sleep(120)
  return made.guid || made.bookmark?.guid
}
const mark = async (title, url, parentGuid) => {
  await ctl.send('agentSpace.bookmarks.add',
    parentGuid ? { title, url, parentGuid } : { title, url })
  await sleep(120)
}
const work = await folder('Work')
await mark('phibrowser-cli · GitHub', 'https://github.com/', work)
await mark('Chromium code search', 'https://source.chromium.org/', work)
await folder('Specs', work)
await folder('Reading')
await mark('Design system notes', 'https://example.com/design')
await mark('LinkedIn', 'https://www.linkedin.com/')
await ctl.send('agentSpace.window.setBounds', { windowId, x: 69, y: 40, width: 1400, height: 900 })
await sleep(600)
const layout = await ctl.send('agentSpace.window.layout', { windowId })
console.log(JSON.stringify({ frameLeft: layout.frameLeft, frameTop: layout.frameTop, dpi: layout.dpi }))
for (const r of layout.rows || []) {
  if (r.band === 'bookmarks') console.log(JSON.stringify([r.key, r.folder, r.expanded, r.depth, r.left, r.top, r.right, r.bottom]))
}
ctl.close()
