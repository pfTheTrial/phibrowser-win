// Drives one step of the two-NTP split check: `active` reports which tab
// is active, `ratio` drags the divider, `web` navigates the active pane to
// a web page, `unsplit` dissolves the split.
import { openControl, sleep } from '../phi-test.mjs'
const ctl = await openControl()
const win = (await ctl.send('agentSpace.list', {})).windows[0]
const tabs = async () => (await ctl.send('agentSpace.tabs.list', { windowId: win })).tabs || []
const brief = (t) => ({ id: t.id, idx: t.index, url: (t.url || '').slice(0, 30), split: !!t.splitId, active: t.active })
const step = process.argv[2]
if (step === 'ratio') {
  const s = (await ctl.send('agentSpace.splitView.list', { windowId: win })).splits[0]
  console.log(JSON.stringify(await ctl.send('agentSpace.splitView.update', { windowId: win, splitId: s.splitId, ratio: 0.3 })))
} else if (step === 'web') {
  console.log(JSON.stringify(await ctl.send('agentSpace.navigate', { windowId: win, url: 'https://example.com/' })))
} else if (step === 'unsplit') {
  const t = (await tabs()).find((x) => x.splitId)
  console.log(JSON.stringify(await ctl.send('agentSpace.tabs.unsplit', { windowId: win, index: t.index })))
}
await sleep(900)
console.log(step, JSON.stringify((await tabs()).map(brief)))
ctl.close(); process.exit(0)
