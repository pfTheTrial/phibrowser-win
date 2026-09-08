// Agent operating mask: drive a tab in the USER's own Space over CDP and
// watch the browser report it, then confirm a takeover is actually enforced.
//
// The point of the mask is that this client cannot see page automation — the
// frames go straight through its relay — so the browser reports it instead.
// Driving here means Input.dispatchKeyEvent, the same command class the
// browser classifies as a drive.
import { openControl, sleep, openCdp, cdpHttp } from '../phi-test.mjs'

const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
await ctl.send('agentSpace.window.setBounds', {
  windowId: w, x: 100, y: 60, width: 1500, height: 960,
})
await sleep(400)

let { tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w })
while (tabs.length > 1) {
  await ctl.send('agentSpace.tabs.close', { windowId: w, index: tabs.length - 1 })
  await sleep(200)
  ;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
}
await ctl.send('agentSpace.navigate', {
  windowId: w,
  url: 'data:text/html,<title>USERTAB</title><body style="margin:0;background:%23444;' +
       'display:flex;align-items:center;justify-content:center;height:100vh">' +
       '<h1 style="font:700 72px system-ui;color:white">USER TAB</h1></body>',
})
await sleep(900)
;({ tabs } = await ctl.send('agentSpace.tabs.list', { windowId: w }))
console.log('tab:', JSON.stringify(tabs.map(t => ({ i: t.index, id: t.id, t: t.title }))))
console.log('DRIVETAB', tabs[0].id)

// Drive the page over CDP: Input.dispatchKeyEvent is exactly the command
// class the browser classifies as a DRIVE, so this should arm the mask.
const targets = await cdpHttp('/json/list')
const page = targets.find((t) => t.type === 'page' && /USERTAB|data:/.test(t.title + t.url))
if (!page) { console.log('NO PAGE TARGET', JSON.stringify(targets).slice(0, 300)); ctl.close(); process.exit(1) }
const cdp = await openCdp(new URL(page.webSocketDebuggerUrl).pathname)

const drive = async (label) => {
  try {
    const r = await cdp.send('Input.dispatchKeyEvent', {
      type: 'keyDown', windowsVirtualKeyCode: 65, key: 'a', text: 'a',
    })
    console.log(label, 'ACCEPTED', JSON.stringify(r).slice(0, 160))
  } catch (e) {
    console.log(label, 'REFUSED', String(e).slice(0, 200))
  }
}

await drive('drive#1')
await sleep(600)
await drive('drive#2')
await sleep(600)
console.log('MASK SHOULD BE ARMED NOW — screenshot, then take control')
// Keep driving in the background so the mask does not idle out during the
// screenshot round (the browser re-reports at most once a second).
for (let i = 0; i < 60; i++) { await drive('keepalive'); await sleep(1500) }
cdp.close(); ctl.close()
