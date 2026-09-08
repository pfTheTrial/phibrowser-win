// B15: give Space 1 a catalog icon so the pip draws the ported artwork.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const { windows } = await ctl.send('agentSpace.list')
const w = windows[0]
const sp = await ctl.send('agentSpace.spaces.list', { windowId: w })
const one = sp.spaces[0]
const icon = process.argv[2] || 'phi:phi-icon-beaker'
const r = await ctl.send('agentSpace.spaces.update',
                         { windowId: w, spaceId: one.id, iconName: icon })
console.log('update ok:', r.ok !== false, 'icon now:', icon)
ctl.close()
