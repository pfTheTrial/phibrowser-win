// How long the reading surface is visible in the strip before it is moved
// into its panel. The user sees a tab appear and vanish for exactly this long.
import { openControl, sleep } from './phi-test.mjs'
const ARTICLE = 'https://en.wikipedia.org/wiki/Readability'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const tabs = async () =>
  (await ctl.send('agentSpace.tabs.list', { windowId: w })).tabs || []

let list = await tabs()
while (list.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: list.length - 1 })
  await sleep(250); list = await tabs()
}
await ctl.send('agentSpace.navigate', { windowId: w, url: ARTICLE })
await sleep(6000)
console.log('before:', (await tabs()).length, 'tab(s)')
const t0 = Date.now()
await ctl.send('agentSpace.menu.click', { windowId: w, path: ['View', 'Toggle Reader View'] })
// No early exit: the extraction alone takes several seconds, so a loop that
// stops at "one tab" stops before the surface has even been created.
let seen = 0
for (let i = 0; i < 40; i++) {
  const list = await tabs()
  if (list.length > 1) seen++
  console.log(`  +${String(Date.now() - t0).padStart(5)}ms  ${list.length} tab(s): ` +
      JSON.stringify(list.map((t) => t.url.slice(0, 46))))
  if (seen && list.length === 1) break
  await sleep(400)
}
console.log('polls with the surface in the strip:', seen)
ctl.close(); process.exit(0)
