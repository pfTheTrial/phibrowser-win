// The sync key layer on the box (docs/user-data-parity.md S3): runs ONE
// agentSpace.sync.keyOp and waits for its outcome, or prints the status.
//
//   node probes/sync-keys.mjs                    status (account, keyLayer)
//   node probes/sync-keys.mjs beginSetup         the setup route (read-only)
//   node probes/sync-keys.mjs refresh            devices + pending requests
//   node probes/sync-keys.mjs requestJoin        a join request + its code
//   node probes/sync-keys.mjs pollJoin '{"requestId":"…"}'
//   node probes/sync-keys.mjs startPairing       profile candidates
//   node probes/sync-keys.mjs applyDecisions '{"decisions":[…]}'
//
// Every op except status, beginSetup, refresh and startPairing WRITES to the
// signed-in account on the sync server (a join request, a device
// registration, a profile key, a revocation). bootstrap additionally needs
// '{"confirm":"bootstrap"}': it creates the account's keys, and prints the
// recovery code exactly once.
import { openControl, sleep } from '../phi-test.mjs'

const [op, json] = [process.argv[2], process.argv[3]]
const ctl = await openControl()
if (!op || op === 'status') {
  console.log(JSON.stringify(await ctl.send('agentSpace.sync.status'), null, 1))
  process.exit(0)
}
const start = await ctl.send('agentSpace.sync.keyOp',
                             { op, ...(json ? JSON.parse(json) : {}) })
if (!start.ok) {
  console.log(JSON.stringify(start))
  process.exit(1)
}
for (let i = 0; i < 240; i++) {
  const s = await ctl.send('agentSpace.sync.status')
  if (s.lastOp && s.lastOp.seq === start.seq && s.lastOp.done) {
    console.log(JSON.stringify({ lastOp: s.lastOp, keyLayer: s.keyLayer }, null, 1))
    process.exit(s.lastOp.status === 'ok' ? 0 : 2)
  }
  await sleep(500)
}
console.log('timeout waiting for', op)
process.exit(3)
