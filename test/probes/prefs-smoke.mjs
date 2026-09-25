// The account-scoped preferences (docs/user-data-parity.md S1): the sidebar
// width lands in the account's defaults (lastKnownSidebarWidth), not the
// device-wide PhiPreferences.json. Prints what the verbs report; the files
// are read back from the Linux side.
import { openControl, sleep } from '../phi-test.mjs'

const ctl = await openControl()
await ctl.send('agentSpace.layout.set', { sidebarWidth: 300 })
await sleep(500)
console.log('layout', JSON.stringify(await ctl.send('agentSpace.layout.get')))
await ctl.send('agentSpace.layout.set', { sidebarWidth: 240 })
await sleep(300)
process.exit(0)
