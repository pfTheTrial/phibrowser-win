// Seeds enough Spaces that the strip's pip row overflows, parks the shell
// and activates the FIRST Space, so a rig can wheel the row until the
// active pip scrolls out and then switch (§ the pill's slide-in origin).
import { openControl, sleep } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const windowId = windows[0]
const kWant = 10
let { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
while (spaces.length < kWant) {
  await ctl.send('agentSpace.spaces.create', { windowId, name: 'S' + (spaces.length + 1) })
  await sleep(300)
  spaces = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
}
console.log('spaces', spaces.map((s, i) => i + ':' + s.name).join(' '))
await ctl.send('agentSpace.window.setBounds', { windowId, x: 69, y: 40, width: 1400, height: 900 })
await sleep(300)
await ctl.send('agentSpace.spaces.activate', { windowId, spaceId: spaces[0].id })
await sleep(600)
const l = await ctl.send('agentSpace.window.layout', { windowId })
console.log('dpi', l.dpi, 'sidebarWidth', l.sidebarWidth, 'titleBar', l.titleBar, 'tabsTop', l.tabsTop, 'mode', l.mode)
ctl.close()
