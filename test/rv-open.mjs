// Opens Reader View on the active tab and reports that it mounted. For
// staging a screenshot: the capture graft's synthetic click is less reliable
// than asking the client directly.
import { openControl, sleep } from './phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
await sleep(8000)
console.log('readerActive', (await ctl.send('agentSpace.window.layout', { windowId: w })).readerActive)
ctl.close()
process.exit(0)
