// settings-open.mjs <section>: opens the Settings window on a page.
import { openControl } from '../phi-test.mjs'
const section = process.argv[2] || 'general'
const ctl = await openControl()
try {
  console.log(JSON.stringify(await ctl.send('agentSpace.settings.open', {})))
  await new Promise((r) => setTimeout(r, 400))
  console.log(JSON.stringify(await ctl.send('agentSpace.settings.set', { section })))
} finally { ctl.close() }
