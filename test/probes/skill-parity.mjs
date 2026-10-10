// skill-parity.mjs: the phi-browser skill's Mac-parity verbs on Windows —
// Space theme/icon normalization (spaces.create/update), spaces.openTab on
// a windowless Space (windowOpened, background keeps focus), and agent
// notification cards (notification.show → notification.response). Leaves
// one card up for a screenshot when given `hold`.
import { openControl, sleep } from '../phi-test.mjs'

const hold = process.argv.includes('hold')
const name = `parity ${Date.now() % 100000}`
const ctl = await openControl()
const events = []
ctl.onEvent((type, payload) => {
  if (type === 'notification.response') events.push(payload)
})
const out = (label, value) => console.log(label, JSON.stringify(value))
let spaceId = null
try {
  // --- Themes and icons ----------------------------------------------------
  const created = await ctl.send('agentSpace.spaces.create',
                                 { name, themeId: 'Coral', iconName: 'mail' })
  spaceId = created.spaceId
  out('create', created)
  let row = (await ctl.send('agentSpace.spaces.list', {})).spaces
    .find((s) => s.spaceId === spaceId)
  out('listed', { themeId: row?.themeId, iconName: row?.iconName,
                  colorHex: row?.colorHex, windowIds: row?.windowIds })
  out('update hex+emoji', await ctl.send('agentSpace.spaces.update',
    { spaceId, colorHex: '#3A6FF8', iconName: '🚀' }))
  out('update legacy', await ctl.send('agentSpace.spaces.update',
    { spaceId, iconName: 'phi:22' }))
  for (const bad of [{ iconName: 'not-an-icon' }, { themeId: 'chartreuse' },
                     { colorHex: '#12' }]) {
    out(`refuse ${Object.keys(bad)[0]}`,
        await ctl.send('agentSpace.spaces.update', { spaceId, ...bad }))
  }
  row = (await ctl.send('agentSpace.spaces.list', {})).spaces
    .find((s) => s.spaceId === spaceId)
  out('after refusals', { themeId: row?.themeId, iconName: row?.iconName })

  // --- A windowless Space gets a window ------------------------------------
  const before = await ctl.send('agentSpace.spaces.focus', {})
  out('focus before', { space: before.spaceName, windowId: before.windowId })
  const opened = await ctl.send('agentSpace.spaces.openTab',
    { spaceId, url: 'https://example.org/', activate: false })
  out('openTab (background)', opened)
  await sleep(3000)
  const after = await ctl.send('agentSpace.spaces.focus', {})
  out('focus after', { space: after.spaceName, windowId: after.windowId,
                       unchanged: after.windowId === before.windowId })
  out('new window tabs', ((await ctl.send('agentSpace.spaces.listTabs',
    { spaceId })).tabs || []).map((t) => t.url))
  out('openTab again (window exists)', await ctl.send(
    'agentSpace.spaces.openTab',
    { spaceId, url: 'https://example.com/', activate: false }))

  // --- Notification cards -------------------------------------------------
  out('notify invalid', await ctl.send('notification.show',
                                       { title: ' ', message: 'x' }))
  out('notify bad expiry', await ctl.send('notification.show',
    { title: 't', message: 'm', expiresInSeconds: 1.5 }))
  const quick = await ctl.send('notification.show',
    { title: 'Expires fast', message: 'This card times out in 2 s.',
      expiresInSeconds: 2 })
  out('notify quick', quick)
  await ctl.pump(4000)
  out('events', events)
  if (hold) {
    const kept = await ctl.send('notification.show', {
      title: 'Review ready',
      message: 'The agent finished comparing the three quotes. Open the ' +
               'summary to approve the cheapest one, or dismiss to keep ' +
               'looking.',
      buttonTitle: 'Review', expiresInSeconds: 120 })
    await ctl.send('notification.show', { title: 'Second card',
      message: 'Paging shows when more than one card waits.',
      expiresInSeconds: 120 })
    out('notify held', kept)
  }
} finally {
  ctl.close()
  process.exit(0)
}
