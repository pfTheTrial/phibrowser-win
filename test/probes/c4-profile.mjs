// C4: profiles.create must answer with the new profileId.
import { openControl } from '../phi-test.mjs'
const ctl = await openControl()
const name = 'C4 Probe Profile'
const r = await ctl.send('agentSpace.profiles.create', { displayName: name })
console.log('create:', JSON.stringify(r))
const list = await ctl.send('agentSpace.profiles.list', {})
const row = (list.profiles ?? []).find(p => p.name === name)
console.log('listed:', row ? JSON.stringify(row) : 'NOT FOUND')
console.log('ids match:', !!row && row.id === r.profileId)
if (row) {
  const del = await ctl.send('agentSpace.profiles.delete', { profileId: row.id })
  console.log('cleanup delete:', del.ok)
}
ctl.close()
