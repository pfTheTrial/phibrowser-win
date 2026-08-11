// Tab groups over Chromium's native model: create/rename/color/collapse via
// verbs, membership via groupId in tabs.list, and the Phi-managed rule —
// a tagged (bound) tab refuses grouping.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

const tabs = async (ctl, windowId) =>
  (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
const groups = async (ctl, windowId) =>
  (await ctl.send('agentSpace.groups.list', { windowId })).groups

async function ensureTabs(ctl, windowId, count) {
  let t = await tabs(ctl, windowId)
  while (t.length < count) {
    await ctl.send('agentSpace.tabs.new',
                   { windowId, url: `data:text/html,<title>Grp ${t.length}</title>` })
    await sleep(600)
    t = await tabs(ctl, windowId)
  }
  return t
}

async function cleanup(ctl, windowId) {
  for (const g of await groups(ctl, windowId)) {
    await ctl.send('agentSpace.groups.ungroup', { windowId, groupId: g.id })
    await sleep(300)
  }
  let t = await tabs(ctl, windowId)
  for (let i = t.length - 1; i >= 0 && t.length > 1; --i) {
    if ((t[i].url || '').includes('data:text/html')) {
      await ctl.send('agentSpace.tabs.close', { windowId, index: t[i].index })
      await sleep(250)
      t = await tabs(ctl, windowId)
    }
  }
}

test('groups: create marks the tab; rename and color round-trip', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await ensureTabs(ctl, windowId, 2)
    assertEq((await ctl.send('agentSpace.tabs.group', { windowId, index: 0 })).ok,
             true, 'group created')
    await sleep(500)
    let gs = await groups(ctl, windowId)
    assertEq(gs.length, 1, 'one group listed')
    const gid = gs[0].id
    assert((await tabs(ctl, windowId)).some((t) => t.groupId === gid),
           'member row carries the groupId')
    assertEq((await ctl.send('agentSpace.groups.rename',
                             { windowId, groupId: gid, title: 'Suite' })).ok,
             true, 'rename ok')
    assertEq((await ctl.send('agentSpace.groups.color',
                             { windowId, groupId: gid, color: 'cyan' })).ok,
             true, 'color ok')
    await sleep(300)
    gs = await groups(ctl, windowId)
    assertEq(gs[0].title, 'Suite', 'title round-trips')
    assertEq(gs[0].color, 'cyan', 'color round-trips')
    assertEq((await ctl.send('agentSpace.groups.color',
                             { windowId, groupId: gid, color: 'plaid' })).ok,
             false, 'unknown color refused')
    await cleanup(ctl, windowId)
  } finally { ctl.close() }
})

test('groups: collapsing evacuates the active tab out of the group', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await ensureTabs(ctl, windowId, 3)
    let t = await tabs(ctl, windowId)
    const active = t.find((x) => x.active)
    await ctl.send('agentSpace.tabs.group', { windowId, index: active.index })
    await sleep(500)
    const gid = (await groups(ctl, windowId))[0].id
    assertEq((await ctl.send('agentSpace.groups.collapse',
                             { windowId, groupId: gid, collapsed: true })).ok,
             true, 'collapse ok')
    await sleep(400)
    t = await tabs(ctl, windowId)
    const nowActive = t.find((x) => x.active)
    assert(nowActive.groupId !== gid, 'active tab moved outside the group')
    assertEq((await groups(ctl, windowId))[0].collapsed, true, 'group collapsed')
    await cleanup(ctl, windowId)
  } finally { ctl.close() }
})

test('groups: a bound (tagged) tab refuses grouping', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const pin = await ctl.send('agentSpace.pinned.add',
        { windowId, title: 'GrpPin', url: 'https://example.com/grppin' })
    await ctl.send('agentSpace.pinned.open', { windowId, guid: pin.guid })
    await sleep(900)
    const t = await tabs(ctl, windowId)
    const bound = t.find((x) => x.tag === pin.guid)
    assert(bound, 'bound tab exists')
    assertEq((await ctl.send('agentSpace.tabs.group',
                             { windowId, index: bound.index })).ok,
             false, 'Phi-managed tab refused by grouping')
    await ctl.send('agentSpace.pinned.remove', { guid: pin.guid })
    await sleep(400)
    const t2 = await tabs(ctl, windowId)
    const row = t2.find((x) => (x.url || '').includes('grppin'))
    if (row) {
      await ctl.send('agentSpace.tabs.close', { windowId, index: row.index })
    }
  } finally { ctl.close() }
})

run()
