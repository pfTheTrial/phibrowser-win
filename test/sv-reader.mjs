// readerArticle() / saveArticle() through the skill's own helpers, in an
// agent Space, against a real article and against a non-article.
import * as H from 'file:///C:/Users/jdxin/phi-browser-skill/scripts/lib/helpers.mjs'
const out = []
const log = (...a) => { const l = a.map(x => typeof x === 'string' ? x : JSON.stringify(x)).join(' '); out.push(l); console.log(l) }
try {
  const ctx = await H.enterContext({ kind: 'agent', name: 'sv-reader' })
  log('context', { spaceId: ctx.spaceId, windowId: ctx.windowId })
  await H.openTab('https://en.wikipedia.org/wiki/Domain_Name_System')
  await H.wait(4)
  const t0 = Date.now()
  const a = await H.readerArticle({ html: false })
  log('readerArticle', Date.now() - t0, 'ms', { title: a.title, rung: a.rung, htmlLength: a.htmlLength, pageCount: a.pageCount, isComplete: a.isComplete, hasHTML: 'contentHTML' in a })
  const b = await H.readerArticle({ html: true })
  log('with html: length', (b.contentHTML || '').length, 'starts', (b.contentHTML || '').slice(0, 80))
  const s = await H.saveArticle('C:\\Users\\jdxin\\sv-article.html')
  log('saveArticle', s)
  await H.goto('https://example.com/?reader')
  await H.wait(1.5)
  try { const n = await H.readerArticle({ html: false }); log('non-article unexpectedly ok', n) }
  catch (e) { log('non-article ->', String(e.message).slice(0, 140)) }
  await H.complete()
} catch (e) {
  log('ERROR', String(e && e.stack || e).slice(0, 600))
}
process.exit(0)
