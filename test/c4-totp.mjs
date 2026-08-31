// C4: credentials.getTotp through the vault helper (approval-gated reveal).
import { openControl } from './phi-test.mjs'
const ctl = await openControl()
console.log('status:', JSON.stringify(await ctl.send('credentials.status', {})))
const r = await ctl.send('credentials.getTotp', { domain: 'example.com' })
console.log('getTotp:', JSON.stringify(r))
ctl.close()
