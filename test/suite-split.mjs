// Split view over Chromium's native side-by-side: tabs.split pairs the ACTIVE
// tab with another (shared splitId in tabs.list), tabs.unsplit dissolves it.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

const tabs = async (ctl, windowId) =>
  (await ctl.send('agentSpace.tabs.list', { windowId })).tabs

async function ensureTabs(ctl, windowId, count) {
  let t = await tabs(ctl, windowId)
  while (t.length < count) {
    await ctl.send('agentSpace.tabs.new',
                   { windowId, url: `data:text/html,<title>Split ${t.length}</title>` })
    await sleep(600)
    t = await tabs(ctl, windowId)
  }
  return t
}

async function cleanup(ctl, windowId) {
  let t = await tabs(ctl, windowId)
  const member = t.find((x) => x.splitId)
  if (member) {
    await ctl.send('agentSpace.tabs.unsplit', { windowId, index: member.index })
    await sleep(400)
  }
  t = await tabs(ctl, windowId)
  for (let i = t.length - 1; i >= 0 && t.length > 1; --i) {
    if ((t[i].url || '').includes('data:text/html')) {
      await ctl.send('agentSpace.tabs.close', { windowId, index: t[i].index })
      await sleep(250)
      t = await tabs(ctl, windowId)
    }
  }
}

test('split: pairing marks both tabs with one shared splitId', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const t = await ensureTabs(ctl, windowId, 2)
    const other = t.find((x) => !x.active)
    assertEq((await ctl.send('agentSpace.tabs.split',
                             { windowId, index: other.index })).ok,
             true, 'split ok')
    await sleep(700)
    const after = await tabs(ctl, windowId)
    const members = after.filter((x) => x.splitId)
    assertEq(members.length, 2, 'exactly two members')
    assertEq(members[0].splitId, members[1].splitId, 'one shared id')
    await cleanup(ctl, windowId)
  } finally { ctl.close() }
})

test('split: the active tab itself and split members are refused', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    let t = await ensureTabs(ctl, windowId, 3)
    const active = t.find((x) => x.active)
    assertEq((await ctl.send('agentSpace.tabs.split',
                             { windowId, index: active.index })).ok,
             false, 'splitting the active tab with itself is refused')
    const other = t.find((x) => !x.active)
    await ctl.send('agentSpace.tabs.split', { windowId, index: other.index })
    await sleep(700)
    t = await tabs(ctl, windowId)
    const outsider = t.find((x) => !x.splitId)
    assert(outsider, 'a tab outside the split exists')
    assertEq((await ctl.send('agentSpace.tabs.split',
                             { windowId, index: outsider.index })).ok,
             false, 'active tab already split: refused')
    await cleanup(ctl, windowId)
  } finally { ctl.close() }
})

test('split: unsplit dissolves the pair, both tabs stay open', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    let t = await ensureTabs(ctl, windowId, 2)
    const other = t.find((x) => !x.active)
    await ctl.send('agentSpace.tabs.split', { windowId, index: other.index })
    await sleep(700)
    t = await tabs(ctl, windowId)
    const count = t.length
    const member = t.find((x) => x.splitId)
    assertEq((await ctl.send('agentSpace.tabs.unsplit',
                             { windowId, index: member.index })).ok,
             true, 'unsplit ok')
    await sleep(500)
    const after = await tabs(ctl, windowId)
    assertEq(after.filter((x) => x.splitId).length, 0, 'no members remain')
    assertEq(after.length, count, 'both tabs still open')
    await cleanup(ctl, windowId)
  } finally { ctl.close() }
})

run()
