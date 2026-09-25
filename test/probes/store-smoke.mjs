// The per-account local store end to end (docs/user-data-parity.md, S1):
// Spaces with UUID ids, pinned tabs, a bookmark tree, a URL rule with a
// stable id, a pin-scope round trip, and a Space delete — all through the
// verbs the skill uses. Prints what it saw; the store file itself is read
// back on the Linux side (LocalStore.sqlite under
// <user-data-dir>\Phi\users\<account>\localDB).
import { openControl, sleep } from '../phi-test.mjs'

const ctl = await openControl()
const out = {}
const note = (k, v) => { out[k] = v; console.log(k, JSON.stringify(v)) }

const before = await ctl.send('agentSpace.spaces.list')
note('spaces.before', before.spaces.map(s => s.id))

const made = await ctl.send('agentSpace.spaces.create', { name: 'Store Smoke' })
const spaceId = made.spaceId || made.id || made.space?.id
note('space.created', spaceId)
note('space.id.is.uuid', /^[0-9A-F]{8}-[0-9A-F]{4}-4[0-9A-F]{3}-[89AB][0-9A-F]{3}-[0-9A-F]{12}$/.test(spaceId || ''))

// Pins in the current window's collection.
const p1 = await ctl.send('agentSpace.pinned.add', { title: 'One', url: 'https://one.example/' })
const p2 = await ctl.send('agentSpace.pinned.add', { title: 'Two', url: 'https://two.example/' })
note('pinned.added', [p1.guid, p2.guid])
note('pinned.list', (await ctl.send('agentSpace.pinned.list')).pinned?.map(p => p.title))

// A bookmark tree.
const folder = await ctl.send('agentSpace.bookmarks.addFolder', { title: 'Folder' })
const fguid = folder.guid || folder.bookmark?.guid
await ctl.send('agentSpace.bookmarks.add', { title: 'In folder', url: 'https://in.example/', parentGuid: fguid })
await ctl.send('agentSpace.bookmarks.add', { title: 'At root', url: 'https://root.example/' })
note('bookmarks.list', (await ctl.send('agentSpace.bookmarks.list', {})).bookmarks?.map(b => [b.title, b.parentGuid || '']))

// A rule: its id must survive an unrelated second write.
await ctl.send('agentSpace.urlRules.add', { spaceId, host: ' GitHub.COM. ', pathPrefix: '/' })
const r1 = (await ctl.send('agentSpace.urlRules.list')).rules
await ctl.send('agentSpace.urlRules.add', { spaceId, host: '*.figma.com' })
await sleep(600)
const r2 = (await ctl.send('agentSpace.urlRules.list')).rules
note('rules', r2.map(r => [r.id, r.host, r.pathPrefix]))
note('rule.id.stable', r1[0] && r2.some(r => r.id === r1[0].id))

// Scope round trip: narrow then widen back, the pins survive once each.
await ctl.send('agentSpace.pinned.scope.set', { scope: 'space' })
note('pinned.after.space', (await ctl.send('agentSpace.pinned.list')).pinned?.map(p => p.title))
await ctl.send('agentSpace.pinned.scope.set', { scope: 'profile' })
note('pinned.after.profile', (await ctl.send('agentSpace.pinned.list')).pinned?.map(p => p.title))

// Delete the smoke Space: its rules go with it.
await ctl.send('agentSpace.spaces.delete', { spaceId })
await sleep(800)
note('spaces.after', (await ctl.send('agentSpace.spaces.list')).spaces.map(s => s.id))
note('rules.after', (await ctl.send('agentSpace.urlRules.list')).rules.map(r => r.host))

// Leave the pins and bookmarks for the store read-back; remove them after.
console.log('RESULT', JSON.stringify(out))
process.exit(0)
