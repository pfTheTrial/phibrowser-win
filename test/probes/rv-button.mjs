// The reader BUTTON: which signal offers it, and does the slot appear.
// Runs the article and a non-article past the same probe, waiting for the
// verdicts rather than assuming they have landed (upstream's distillability
// service can take the better part of ten seconds on a long page).
import { openControl, sleep } from '../phi-test.mjs'

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]

async function layout() { return ctl.send('agentSpace.window.layout', { windowId: w }) }
const shape = (l) => ({
  reader: l.reader && l.reader.width ? `${l.reader.x},${l.reader.y} ${l.reader.width}x${l.reader.height}` : 'none',
  probe: l.readerProbe, distillable: l.readerDistillable, active: l.readerActive,
})

async function stage(url) {
  let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
  while (tabs.length > 1) {
    await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
    await sleep(300); ({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
  }
  await ctl.send('agentSpace.navigate', { windowId: w, url })
  // Poll: the extension's probe reports at document_idle, upstream's
  // distillability verdict lands later still.
  for (let i = 0; i < 20; i++) {
    await sleep(1000)
    const l = await layout()
    console.log(`  t+${i + 1}s`, JSON.stringify(shape(l)))
    if (l.readerProbe && l.readerDistillable) break
  }
}

await ctl.send('agentSpace.window.setBounds', { windowId: w, x: 60, y: 40, width: 1700, height: 1040 })
await sleep(500)
await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Performance'] })
await sleep(800)

console.log('ARTICLE https://en.wikipedia.org/wiki/Readability')
await stage('https://en.wikipedia.org/wiki/Readability')
const article = await layout()
console.log('article final:', JSON.stringify(shape(article)))
if (article.reader && article.reader.width) {
  console.log('BUTTON CLICK POINT',
      `${Math.round(article.reader.x + article.reader.width / 2)},${Math.round(article.reader.y + article.reader.height / 2)}`)
}

console.log('NON-ARTICLE https://example.com/')
await stage('https://example.com/')
console.log('non-article final:', JSON.stringify(shape(await layout())))

ctl.close()
process.exit(0)
