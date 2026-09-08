// B16: content (element) fullscreen in the hosted shell, driven over CDP
// with a user gesture. Enter, report state; leave via exitFullscreen when
// run with "exit".
import { openControl, cdpHttp, openCdp, sleep } from '../phi-test.mjs'
const mode = process.argv[2] || 'enter'
const targets = await cdpHttp('/json')
const page = targets.find(t => t.type === 'page')
if (!page) { console.log('no page target'); process.exit(1) }
console.log('target:', page.url)
const wsPath = new URL(page.webSocketDebuggerUrl.replace('ws://phi', 'http://phi')).pathname
const cdp = await openCdp(wsPath)
if (mode === 'enter') {
  const r = await cdp.send('Runtime.evaluate', {
    expression: `document.documentElement.requestFullscreen()
        .then(() => 'fs-entered')
        .catch(e => 'fs-error: ' + e.message)`,
    awaitPromise: true, userGesture: true,
  })
  console.log('enter:', JSON.stringify(r.result))
  await sleep(1500)
  const s = await cdp.send('Runtime.evaluate', {
    expression: `[!!document.fullscreenElement, innerWidth, innerHeight,
                  screen.width, screen.height].join(',')`,
  })
  console.log('state:', JSON.stringify(s.result))
} else {
  const r = await cdp.send('Runtime.evaluate', {
    expression: `(document.fullscreenElement
        ? document.exitFullscreen().then(() => 'fs-left')
        : Promise.resolve('not-fullscreen'))`,
    awaitPromise: true, userGesture: true,
  })
  console.log('exit:', JSON.stringify(r.result))
}
cdp.close()
