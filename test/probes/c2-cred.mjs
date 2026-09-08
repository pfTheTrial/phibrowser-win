// C2: exercise a credential REVEAL request (the approval dialog path).
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
console.log('status:', JSON.stringify(await ctl.send('credentials.status', {})))
const r = await ctl.send('credentials.get', { domain: 'example.com', purpose: 'C2 probe' })
// Never print the secret itself — just the shape of the answer.
console.log('get ok:', r.ok, 'error:', r.error ?? '-',
            'fields:', r.credential ? Object.keys(r.credential).join(',') : '-')
ctl.close()
