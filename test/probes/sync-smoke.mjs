// Chromium sync's view of the client (docs/user-data-parity.md S2): the
// account state, the server --sync-url named, whether a bearer is at hand,
// and a hand-fed profile key coming and going.
//
// A key handed to a SIGNED-IN account starts Chromium's sync engine against
// the real server (a GetUpdates on /chromium-sync/<uuid>), so the key steps
// only run signed out, or with --live when that is the point.
import { openControl, sleep } from '../phi-test.mjs'

const live = process.argv.includes('--live')
const ctl = await openControl()
const status = await ctl.send('agentSpace.sync.status')
console.log('status', JSON.stringify(status))
if (status.account === 'signedIn' && !live) {
  console.log('signed in: key steps skipped (pass --live to feed the server)')
  process.exit(0)
}
const key = { profileId: 'Default', uuid: 'PROBE-UUID', passphrase: '0'.repeat(64) }
console.log('set', JSON.stringify(await ctl.send('agentSpace.sync.setProfileKey', key)))
console.log('bad', JSON.stringify(
    await ctl.send('agentSpace.sync.setProfileKey', { ...key, passphrase: 'nothex' })))
await sleep(200)
console.log('clear', JSON.stringify(
    await ctl.send('agentSpace.sync.setProfileKey', { profileId: 'Default' })))
process.exit(0)
