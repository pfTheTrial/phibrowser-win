// Smoke for the account surface (docs/login-port.md §5): the session
// restores without a crash, the You pane opens, a sign-in opens the hosted
// login in a tab with the Mac's callback as redirect_uri (the tenant
// answers), cancel takes the pane back, a synthetic callback navigation is
// intercepted by the framework throttle and refused for its stale state,
// and the harness verbs answer. Run on the box from phitest with
// PHI_USER_DATA_DIR.
import { openControl, sleep } from '../phi-test.mjs'

const ctl = await openControl()
const show = (label, v) => console.log(label + ': ' + JSON.stringify(v))

show('status#1', await ctl.send('agentSpace.account.status'))
show('settings.open', await ctl.send('agentSpace.settings.open'))
show('settings.set account', await ctl.send('agentSpace.settings.set', { section: 'account' }))
await sleep(500)

const started = await ctl.send('agentSpace.account.signIn')
show('signIn', started)
await sleep(4000)
const { windows } = await ctl.send('agentSpace.list')
const tabs = await ctl.send('agentSpace.tabs.list', { windowId: windows[0] })
show('tabs after signIn', tabs.tabs.map((t) => t.url))
show('status#2 (signing in)', await ctl.send('agentSpace.account.status'))

show('cancelSignIn', await ctl.send('agentSpace.account.cancelSignIn'))
await sleep(1000)
show('status#3 (cancelled)', await ctl.send('agentSpace.account.status'))

// The intercept: a navigation to the callback scheme must be taken by the
// client (no external-protocol dialog) and the tab it landed in retired by
// the framework. With no sign-in pending the bare callback is the
// logout-return shape.
const callback = 'com.phibrowser.Mac://auth.stag.phibrowser.com/macos/com.phibrowser.Mac/callback'
show('open callback', await ctl.send('agentSpace.tabs.open', { windowId: windows[0], url: callback }))
await sleep(2500)
const after = await ctl.send('agentSpace.tabs.list', { windowId: windows[0] })
show('tabs after callback', after.tabs.map((t) => t.url))
show('status#4', await ctl.send('agentSpace.account.status'))
ctl.close()
