// Which sites offer the reader, and which of the two signals said so. Answers
// "the button only shows on some sites": probe = the extension's in-page
// readerability test (or a site rule), distillable = upstream's own verdict.
import { openControl, sleep } from './phi-test.mjs'

const SITES = process.argv.slice(2).length ? process.argv.slice(2) : [
  'https://en.wikipedia.org/wiki/Readability',
  'https://zh.wikipedia.org/wiki/%E7%BD%91%E9%A1%B5',
  'https://baike.baidu.com/item/%E6%B5%8F%E8%A7%88%E5%99%A8',
  'https://www.163.com/',
  'https://www.zhihu.com/',
  'https://example.com/',
]
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]

for (const url of SITES) {
  let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
  while (tabs.length > 1) {
    await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
    await sleep(250); ({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
  }
  await ctl.send('agentSpace.navigate', { windowId: w, url })
  // WAIT FOR THE COMMIT. Without this an unreachable host leaves the previous
  // page up and every row after it reports that page's verdicts instead.
  const host = new URL(url).host
  let t = null
  let committed = false
  for (let i = 0; i < 25; i++) {
    await sleep(1000)
    t = (await ctl.send('agentSpace.tabs.list', { windowId: w })).tabs[0]
    if (t && t.url && new URL(t.url).host === host) { committed = true; break }
  }
  if (!committed) {
    console.log(`SKIP     never committed (unreachable?)  ${url}`)
    continue
  }
  let l = null
  for (let i = 0; i < 12; i++) {
    await sleep(1000)
    l = await ctl.send('agentSpace.window.layout', { windowId: w })
    if (l.readerProbe && l.readerDistillable) break
  }
  console.log(
    (l.reader && l.reader.width ? 'BUTTON  ' : 'none    ') +
    `probe=${String(l.readerProbe).padEnd(5)} distillable=${String(l.readerDistillable).padEnd(5)} ` +
    `${url}\n         committed: ${t.url}`)
}
ctl.close(); process.exit(0)
