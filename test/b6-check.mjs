import { openControl } from './phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
for (const w of windows) {
  const t = await ctl.send('agentSpace.tabs.list', { windowId: w })
  console.log(`slot ${w}: ${t.tabs.map(x => x.title).join(' | ')}`)
}
ctl.close()
