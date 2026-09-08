// The emoji catalog loads lazily on the UI thread; before the raw-read fix
// this DCHECK-crashed the browser (thread_restrictions.cc:62).
import { openControl } from './phi-test.mjs'
const ctl = await openControl()
const g = await ctl.send('agentSpace.emoji.groups', {})
console.log('groups:', JSON.stringify(g).slice(0, 160))
const a = await ctl.send('agentSpace.list', {})
console.log('still alive; transcriptMirror =', a.transcriptMirror, 'windows =', JSON.stringify(a.windows))
ctl.close()
