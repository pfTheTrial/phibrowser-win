// agentSpace.readerArticle / readerDocument now run the READER's pipeline
// (the extension's site rules, ladder and coverage gate) instead of upstream's
// distiller. The tell is the reply: a distiller answer says rung "distiller"
// and carries none of coverage / byline / siteName / codeBlockCount.
import { openControl, sleep } from '../phi-test.mjs'

const ARTICLE = 'https://en.wikipedia.org/wiki/Domain_Name_System'
const NOT_AN_ARTICLE = 'https://example.com/'

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const { spaces } = await ctl.send('agentSpace.spaces.list', {})
const space = (spaces || []).find((s) => (s.windowIds || []).includes(w)) || spaces[0]
console.log('space', space.spaceId, space.name)

let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
while (tabs.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
  await sleep(300); ({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
}
await ctl.send('agentSpace.navigate', { windowId: w, url: ARTICLE })
await sleep(5000)

const t0 = Date.now()
const a = await ctl.send('agentSpace.readerArticle',
    { spaceId: space.spaceId, includeHTML: false })
console.log('readerArticle', Date.now() - t0, 'ms:', JSON.stringify(a).slice(0, 600))

const b = await ctl.send('agentSpace.readerArticle',
    { spaceId: space.spaceId, includeHTML: true })
console.log('with html:', (b.contentHTML || '').length, 'chars, starts:',
    JSON.stringify((b.contentHTML || '').slice(0, 100)))

const d = await ctl.send('agentSpace.readerDocument', { spaceId: space.spaceId })
console.log('readerDocument:', JSON.stringify({
  ok: d.ok, rung: d.rung, suggestedFileName: d.suggestedFileName,
  documentChars: (d.document || '').length,
}))

await ctl.send('agentSpace.navigate', { windowId: w, url: NOT_AN_ARTICLE })
await sleep(4000)
const n = await ctl.send('agentSpace.readerArticle',
    { spaceId: space.spaceId, includeHTML: false })
console.log('non-article:', JSON.stringify(n).slice(0, 300))

ctl.close()
process.exit(0)
