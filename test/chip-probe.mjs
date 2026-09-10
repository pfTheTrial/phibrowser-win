// Strip group-chip probe (Mac TabGroupChipView port). Runs ON the box from
// C:\Users\jdxin\phitest: node chip-probe.mjs setup|rows|title <t>|collapse <0|1>|activate <n>|teardown
import { openControl, sleep } from './phi-test.mjs'
const [step, arg] = process.argv.slice(2)
const ctl = await openControl()
const say = (l, v) => console.log(l, JSON.stringify(v))
const { windows } = await ctl.send('agentSpace.list')
const windowId = windows[0]
const tabs = async () => (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
const groups = async () => (await ctl.send('agentSpace.groups.list', { windowId })).groups
const rows = async () => {
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  say('mode', l.mode)
  for (const r of l.rows || []) if (r.band === 'tabs') say('row', r)
}
try {
  if (step === 'setup') {
    say('layout', await ctl.send('agentSpace.layout.set', { mode: 'comfortable' }))
    await ctl.send('agentSpace.window.setBounds', { windowId, x: 69, y: 40, width: 1400, height: 900 })
    await sleep(400)
    const urls = ['https://example.com/', 'https://example.org/', 'https://www.wikipedia.org/', 'https://example.net/', 'data:text/html,<title>Plain</title>']
    let t = await tabs()
    while (t.length < 5) {
      await ctl.send('agentSpace.tabs.new', { windowId, url: urls[t.length] })
      await sleep(900)
      t = await tabs()
    }
    say('tabs', t.map((x) => [x.index, x.id, x.title]))
    if ((await groups()).length === 0) {
      say('group', await ctl.send('agentSpace.tabs.group', { windowId, index: 1 }))
      await sleep(500)
      const token = (await groups())[0].id
      const { spaces = [] } = await ctl.send('agentSpace.spaces.list', {})
      const space = spaces.find((s) => s.active) || spaces[0]
      t = await tabs()
      const members = t.filter((x) => x.index === 2 || x.index === 3).map((x) => x.id)
      say('addTabs', await ctl.send('agentSpace.tabGroups.addTabs', { spaceId: space.spaceId || space.id, token, tabIds: members }))
      await sleep(400)
      say('color', await ctl.send('agentSpace.groups.color', { windowId, groupId: token, color: 'blue' }))
    }
    say('activate', await ctl.send('agentSpace.tabs.activate', { windowId, index: 0 }))
    await sleep(600)
  } else if (step === 'title') {
    const gid = (await groups())[0].id
    say('rename', await ctl.send('agentSpace.groups.rename', { windowId, groupId: gid, title: arg || '' }))
    await sleep(500)
  } else if (step === 'collapse') {
    const gid = (await groups())[0].id
    say('collapse', await ctl.send('agentSpace.groups.collapse', { windowId, groupId: gid, collapsed: arg === '1' }))
    await sleep(500)
  } else if (step === 'activate') {
    say('activate', await ctl.send('agentSpace.tabs.activate', { windowId, index: Number(arg) }))
    await sleep(600)
  } else if (step === 'teardown') {
    for (const g of await groups()) { await ctl.send('agentSpace.groups.ungroup', { windowId, groupId: g.id }); await sleep(300) }
    let t = await tabs()
    for (const x of t.filter((x) => (x.url || '').startsWith('data:') || (x.url || '').includes('example.') || (x.url || '').includes('wikipedia'))) {
      t = await tabs(); const live = t.find((y) => y.id === x.id); if (!live) continue
      await ctl.send('agentSpace.tabs.close', { windowId, index: live.index }); await sleep(300)
    }
  }
  say('groups', await groups())
  say('tabs', (await tabs()).map((x) => [x.index, x.id, x.title, x.groupId, x.active]))
  await rows()
} finally { ctl.close?.(); process.exit(0) }
