// C4: whole-window capture of an agent Space (chrome + CDP web composite).
import { openControl, cdpHttp, openCdp, sleep } from './phi-test.mjs'
import { existsSync, statSync, writeFileSync, unlinkSync } from 'node:fs'
const ctl = await openControl()
const taskId = 'c4probe'
const created = await ctl.send('agentSpace.create', { taskId })
console.log('create:', created.ok, 'windowId:', created.windowId)
if (!created.ok) { ctl.close(); process.exit(1) }
await ctl.send('agentSpace.navigate', { windowId: created.windowId, url: 'https://example.com' })
await sleep(2500)

// Capture the page over CDP (the client cannot read the DComp surface).
const webFile = 'C:\\Users\\jdxin\\c4-web.png'
let webPath = ''
try {
  const targets = await cdpHttp('/json')
  const page = targets.find(t => t.type === 'page' && /example\.com/.test(t.url))
  if (page) {
    const wsPath = new URL(page.webSocketDebuggerUrl.replace('ws://phi', 'http://phi')).pathname
    const cdp = await openCdp(wsPath)
    const shot = await cdp.send('Page.captureScreenshot', { format: 'png' })
    writeFileSync(webFile, Buffer.from(shot.data, 'base64'))
    webPath = webFile
    cdp.close()
    console.log('web capture bytes:', statSync(webFile).size)
  } else {
    console.log('no agent page target found')
  }
} catch (e) { console.log('cdp capture failed:', e.message) }

const outFile = 'C:\\Users\\jdxin\\c4-window.png'
const r = await ctl.send('agentSpace.captureWindow', { taskId, outPath: outFile, webPath })
console.log('captureWindow:', JSON.stringify(r))
console.log('file exists:', existsSync(outFile), existsSync(outFile) ? statSync(outFile).size + ' bytes' : '')
await ctl.send('agentSpace.complete', { taskId, keep: false })
if (webPath) { try { unlinkSync(webFile) } catch {} }
ctl.close()
