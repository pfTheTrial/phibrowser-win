// PhiAgentSpace.getJavaScriptDialogState / handleJavaScriptDialog, answered
// on the control channel (cdp.mjs re-routes the CDP names to it).
import { openControl, sleep, cdpHttp, openCdp } from './phi-test.mjs'
const ctl = await openControl()
const show = (l, v) => console.log(l, JSON.stringify(v))
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
await ctl.send('agentSpace.navigate', { windowId: w, url: 'data:text/html,<title>JSDLG</title><h1>dialog</h1>' })
await sleep(1200)
const targets = await cdpHttp('/json/list')
const page = targets.find(t => t.type === 'page' && /JSDLG/.test(t.title))
if (!page) { console.log('NO PAGE', JSON.stringify(targets).slice(0, 300)); process.exit(1) }
show('state before:', await ctl.send('agentSpace.getJavaScriptDialogState', { targetId: page.id }))
const cdp = await openCdp(new URL(page.webSocketDebuggerUrl).pathname)
cdp.send('Runtime.evaluate', { expression: 'window.__r = confirm("sv confirm?")' }).catch(() => {})
await sleep(1500)
show('state open:', await ctl.send('agentSpace.getJavaScriptDialogState', { targetId: page.id }))
show('handle accept:', await ctl.send('agentSpace.handleJavaScriptDialog', { targetId: page.id, accept: true }))
await sleep(800)
show('state after:', await ctl.send('agentSpace.getJavaScriptDialogState', { targetId: page.id }))
show('confirm result:', await cdp.send('Runtime.evaluate', { expression: 'window.__r', returnByValue: true }))
cdp.send('Runtime.evaluate', { expression: 'window.__p = prompt("name?")' }).catch(() => {})
await sleep(1500)
show('prompt state:', await ctl.send('agentSpace.getJavaScriptDialogState', { targetId: page.id }))
show('handle prompt:', await ctl.send('agentSpace.handleJavaScriptDialog', { targetId: page.id, accept: true, promptText: 'phi' }))
await sleep(800)
show('prompt result:', await cdp.send('Runtime.evaluate', { expression: 'window.__p', returnByValue: true }))
show('handle with nothing open:', await ctl.send('agentSpace.handleJavaScriptDialog', { targetId: page.id, accept: false }))
show('bad target:', await ctl.send('agentSpace.getJavaScriptDialogState', { targetId: 'nope' }))
try { cdp.close() } catch {}
ctl.close()
