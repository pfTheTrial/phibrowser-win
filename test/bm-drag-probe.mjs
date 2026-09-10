// Bookmark -> tab list drag seeding: performance layout, one sidebar bookmark.
import { openControl, sleep } from './phi-test.mjs'
const ctl = await openControl()
const say = (l, v) => console.log(l, JSON.stringify(v))
const { windows } = await ctl.send('agentSpace.list')
const windowId = windows[0]
try {
  say('layout', await ctl.send('agentSpace.layout.set', { mode: 'performance' }))
  await sleep(500)
  const { bookmarks = [] } = await ctl.send('agentSpace.bookmarks.list', {})
  if (!bookmarks.some((b) => (b.url || '').includes('langfuse'))) {
    say('add', await ctl.send('agentSpace.bookmarks.add', { title: 'Langfuse', url: 'https://langfuse.com/' }))
    await sleep(500)
  }
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  for (const r of l.rows || []) if (r.band === 'bookmarks' || r.band === 'tabs') say('row', { band: r.band, key: r.key, top: r.top, bottom: r.bottom, left: r.left, right: r.right })
} finally { ctl.close?.(); process.exit(0) }
