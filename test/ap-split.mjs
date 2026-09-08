// Does the peek's split chip put the preview beside its opener? Reports the
// opener window's tabs (splitId marks members of a split).
import { openControl, sleep } from './phi-test.mjs'
import { appendFileSync } from 'node:fs'
const LOG = 'C:\\Users\\jdxin\\phitest\\ap-split.log'
const say = (...a) => appendFileSync(LOG, a.join(' ') + '\n')
const ctl = await openControl()
const win = (await ctl.send('agentSpace.list', {})).windows[0]
const tabs = (await ctl.send('agentSpace.tabs.list', { windowId: win })).tabs || []
say(JSON.stringify(tabs.map(t => ({ id: t.id, url: (t.url||'').slice(0,44), split: t.splitId, active: t.active }))))
ctl.close()
process.exit(0)
