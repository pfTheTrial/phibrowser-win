// Probe: prints the Space list, one line, truncated — the quickest state dump.
import { openControl } from '../phi-test.mjs'
const c = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v).slice(0, 400))
const { spaces } = await c.send('agentSpace.spaces.list', {})
show('spaces', spaces.map(s => ({ id: s.spaceId, name: s.name, isDefault: s.isDefault, open: s.open, windowIds: s.windowIds })))
show('ghosts', await c.send('agentSpace.lazyRestore.ghosts', {}))
const { windows } = await c.send('agentSpace.list', {})
for (const w of windows) { const t = await c.send('agentSpace.tabs.list', { windowId: w }); show('window ' + w + ' tabs', (t.tabs || []).map(x => x.url)) }
c.close(); process.exit(0)
