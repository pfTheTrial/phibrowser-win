// The reader button in every layout: it must appear on an article and stay
// away on a page with nothing to read, whichever chrome is drawn.
import { openControl, sleep } from './phi-test.mjs'

const ARTICLE = 'https://en.wikipedia.org/wiki/Readability'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const layout = async () => ctl.send('agentSpace.window.layout', { windowId: w })
const rect = (r) => (r && r.width ? `${r.x},${r.y} ${r.width}x${r.height}` : 'none')

async function goto(url) {
  let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
  while (tabs.length > 1) {
    await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
    await sleep(300); ({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
  }
  await ctl.send('agentSpace.navigate', { windowId: w, url })
  for (let i = 0; i < 20; i++) {
    await sleep(1000)
    const l = await layout()
    if (l.readerProbe || l.readerDistillable) break
  }
}

await ctl.send('agentSpace.window.setBounds', { windowId: w, x: 60, y: 40, width: 1700, height: 1040 })
await sleep(500)
await goto(ARTICLE)
for (const mode of ['Performance', 'Balanced', 'Comfortable']) {
  await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', mode] })
  await sleep(1200)
  const l = await layout()
  console.log(`${l.mode.padEnd(12)} article    reader=${rect(l.reader).padEnd(18)} field=${rect(l.urlField)}`)
}
await goto('https://example.com/')
for (const mode of ['Performance', 'Balanced', 'Comfortable']) {
  await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', mode] })
  await sleep(1200)
  const l = await layout()
  console.log(`${l.mode.padEnd(12)} non-article reader=${rect(l.reader)}`)
}
ctl.close(); process.exit(0)
