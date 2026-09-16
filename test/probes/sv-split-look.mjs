// Drives the split-view chrome look check on the box (the Mac SplitPaneHostView
// port: pane cards, focus ring, transparent divider, snap points). Run from
// phitest\probes: `node sv-split-look.mjs <step> [arg]`
//   form [vertical|horizontal]   two data: pages, split them, park the window
//   ratio <0..1>                 drag the divider (splitView.update)
//   layout <vertical|horizontal> flip the axis
//   active <primary|secondary>   activate that pane's tab (the focus ring)
//   mode <performance|balanced|comfortable>
//   appearance <light|dark|system> / theme <id>
//   unsplit                      dissolve the split
//   state                        just report
import { openControl, sleep } from '../phi-test.mjs'
const ctl = await openControl()
const win = (await ctl.send('agentSpace.list', {})).windows[0]
const tabs = async () => (await ctl.send('agentSpace.tabs.list', { windowId: win })).tabs || []
const splits = async () => (await ctl.send('agentSpace.splitView.list', { windowId: win })).splits || []
const brief = (t) => ({ id: t.id, idx: t.index, url: (t.url || '').slice(0, 28), split: !!t.splitId, active: t.active })
const page = (label, bg) =>
  `data:text/html,<body style="margin:0;background:${bg};font:32px sans-serif;padding:40px">${label}</body>`
const [step, arg] = process.argv.slice(2)
const out = (o) => console.log(step, JSON.stringify(o))
if (step === 'form') {
  await ctl.send('agentSpace.window.setBounds', { windowId: win, x: 69, y: 40, width: 1400, height: 900 })
  const a = await ctl.send('agentSpace.tabs.new', { windowId: win, url: page('Left pane', '%23ffffff') })
  await sleep(600)
  const b = await ctl.send('agentSpace.tabs.new', { windowId: win, url: page('Right pane', '%23f7f7f7') })
  await sleep(600)
  out(await ctl.send('agentSpace.splitView.create', {
    windowId: win, primaryTabId: a.id ?? a.tabId, secondaryTabId: b.id ?? b.tabId, layout: arg || 'vertical' }))
} else if (step === 'ratio' || step === 'layout') {
  const s = (await splits())[0]
  const patch = step === 'ratio' ? { ratio: Number(arg) } : { layout: arg }
  out(await ctl.send('agentSpace.splitView.update', { windowId: win, splitId: s.splitId, ...patch }))
} else if (step === 'active') {
  const s = (await splits())[0]
  const id = arg === 'secondary' ? s.secondaryTabId : s.primaryTabId
  const t = (await tabs()).find((x) => x.id === id)
  out(await ctl.send('agentSpace.tabs.activate', { windowId: win, index: t.index }))
} else if (step === 'mode') {
  out(await ctl.send('agentSpace.layout.set', { mode: arg }))
} else if (step === 'appearance') {
  out(await ctl.send('agentSpace.theme.set', { appearance: arg }))
} else if (step === 'theme') {
  out(await ctl.send('agentSpace.theme.set', { themeId: arg }))
} else if (step === 'unsplit') {
  const t = (await tabs()).find((x) => x.splitId)
  out(await ctl.send('agentSpace.tabs.unsplit', { windowId: win, index: t.index }))
}
await sleep(900)
out({ tabs: (await tabs()).map(brief), splits: await splits() })
ctl.close(); process.exit(0)
