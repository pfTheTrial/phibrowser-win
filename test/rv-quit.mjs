// Asks the running client to quit, through its own close path.
//
// A deploy cannot overwrite Phi.exe while it is running (Windows locks the
// image), and the working log's "dest open ... Failure" is exactly that. This
// closes every shell window the way the close box does — agentSpace.close
// routes to PhiWindowController::RequestClose — so the browser shuts itself
// down instead of being killed out from under its own cleanup.
import { openControl, cdpHttp, openCdp, sleep } from './phi-test.mjs'

// Reader panels are browsers of their own and are NOT listed as client
// windows, so close their pages first — a surviving panel keeps the process
// alive after the last real window is gone.
const READER_PREFIX = 'chrome-extension://ickhcgejficcoofnjnnobadfdnfbilnm/reader.html'
for (const t of (await cdpHttp('/json/list')).filter((x) => (x.url || '').startsWith(READER_PREFIX))) {
  try {
    const cdp = await openCdp(t.webSocketDebuggerUrl.replace(/^ws:\/\/[^/]+/, ''))
    await cdp.send('Page.close')
    cdp.close()
    console.log('closed reader surface', t.id)
    await sleep(500)
  } catch (e) { console.log('reader close failed', String(e.message)) }
}

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
console.log('windows', JSON.stringify(windows))
for (const w of windows) {
  const r = await ctl.send('agentSpace.close', { windowId: w })
  console.log('close', w, JSON.stringify(r))
  await sleep(400)
}
ctl.close()
process.exit(0)
