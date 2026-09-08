// Asks for Reader View on a page with no article, so the refusal path runs:
// the extension answers reader.state {active:false, error}, the client turns
// that into the Mac's toast and leaves the strip alone. Pair it with
// rv-toast.ps1, which watches for the toast window while this runs.
import { openControl, sleep } from '../phi-test.mjs'

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]

let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
while (tabs.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
  await sleep(300); ({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
}
await ctl.send('agentSpace.navigate', { windowId: w, url: 'https://example.com/' })
await sleep(4000)
console.log('asking...', JSON.stringify(await ctl.send('agentSpace.menu.click',
    { windowId: w, path: ['View', 'Toggle Reader View'] })))
await sleep(6000)
const after = await ctl.send('agentSpace.tabs.list', { windowId: w })
console.log('tabs after:', JSON.stringify((after.tabs || []).map((t) => t.url.slice(0, 60))))
const l = await ctl.send('agentSpace.window.layout', { windowId: w })
console.log('readerActive', l.readerActive)
ctl.close()
process.exit(0)
