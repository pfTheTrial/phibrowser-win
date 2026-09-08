// Stages a reader on a Wikipedia tab for a screenshot (no assertions).
import { openControl, sleep } from '../phi-test.mjs'
const ctl = await openControl()
const { spaces } = await ctl.send('agentSpace.spaces.list', {})
const active = spaces.find(s => s.isActive && s.windowIds.length) || spaces.find(s => s.windowIds.length)
const wid = active.windowIds[0]
await ctl.send('agentSpace.spaces.openTab', { windowId: wid, url: 'https://en.wikipedia.org/wiki/Example', activate: true })
await sleep(9000)
if (!(await ctl.send('agentSpace.window.layout', { windowId: wid })).readerActive) {
  await ctl.send('agentSpace.menu.click', { windowId: wid, path: ['View', 'Toggle Reader View'] })
  await sleep(8000)
}
const L = await ctl.send('agentSpace.window.layout', { windowId: wid })
console.log('readerActive', L.readerActive, 'surfaces', JSON.stringify(L.surfaces))
ctl.close()
process.exit(0)
