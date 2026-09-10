// Two-window probe for the agent Space switch route (phibrowser-mac 3a9fc111
// port). Runs ON the box from C:\Users\jdxin\phitest:
//   node route-probe.mjs setup|state|handoff|complete|teardown
import { openControl, sleep } from './phi-test.mjs'
const step = process.argv[2]
const ctl = await openControl()
const T = 'route-1'
const say = (l, v) => console.log(l, JSON.stringify(v))
async function userWindows() {
  const { spaces } = await ctl.send('agentSpace.spaces.list', {})
  const ids = []
  for (const s of spaces || []) for (const w of s.windowIds || []) if (!ids.includes(w)) ids.push(w)
  return ids
}
try {
  if (step === 'setup') {
    say('layout', await ctl.send('agentSpace.layout.set', { mode: 'performance' }))
    let wins = await userWindows()
    say('windows0', wins)
    if (wins.length < 2) {
      say('newWindow', await ctl.send('agentSpace.newWindow', {}))
      for (let i = 0; i < 20 && wins.length < 2; i++) { await sleep(500); wins = await userWindows() }
    }
    say('windows1', wins)
    say('boundsA', await ctl.send('agentSpace.window.setBounds', { windowId: wins[0], x: 0, y: 0, width: 1240, height: 900 }))
    say('boundsB', await ctl.send('agentSpace.window.setBounds', { windowId: wins[1], x: 1280, y: 0, width: 1240, height: 900 }))
    const list0 = await ctl.send('agentSpace.list')
    if (!(list0.tasks || []).find(t => t.taskId === T)) {
      say('create', await ctl.send('agentSpace.create', { taskId: T, agentName: 'Claude Code' }))
      const { tasks } = await ctl.send('agentSpace.list')
      const w = tasks.find(t => t.taskId === T).windowId
      say('nav', await ctl.send('agentSpace.navigate', { windowId: w, url: 'https://example.org/' }))
      say('running', await ctl.send('agentSpace.setState', { taskId: T, state: 'running', caption: 'Route probe' }))
      await sleep(2500)
    }
  } else if (step === 'overlap') {
    const wins = await userWindows()
    say('boundsB', await ctl.send('agentSpace.window.setBounds', { windowId: wins[1], x: 800, y: 120, width: 1240, height: 900 }))
  } else if (step === 'handoff') {
    say('handoff', await ctl.send('agentSpace.handoff', { taskId: T, message: 'Please sign in so I can continue.' }))
  } else if (step === 'complete') {
    say('complete', await ctl.send('agentSpace.complete', { taskId: T, status: 'failure', message: 'probe done' }))
  } else if (step === 'teardown') {
    const wins = await userWindows()
    if (wins.length > 1) say('closeB', await ctl.send('agentSpace.close', { windowId: wins[wins.length - 1] }))
    say('layout', await ctl.send('agentSpace.layout.set', { mode: 'comfortable' }))
  }
  const wins = await userWindows()
  for (const w of wins) {
    const l = await ctl.send('agentSpace.window.layout', { windowId: w })
    say('win ' + w, { mode: l.mode, frame: [l.frameLeft, l.frameTop], dpi: l.dpi, sidebarWidth: l.sidebarWidth, tabsTop: l.tabsTop, rows: (l.rows || []).length })
  }
  const list = await ctl.send('agentSpace.list')
  say('tasks', (list.tasks || []).map(t => ({ taskId: t.taskId, number: t.number, status: t.status, ownership: t.ownership, spaceId: t.spaceId, windowId: t.windowId })))
} finally { ctl.close() }
