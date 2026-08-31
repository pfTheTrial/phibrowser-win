// Return to the default Space and remove the BlinkProbe Space the flicker
// probe created.
import { openControl, sleep } from './phi-test.mjs'
const ctl = await openControl()
try {
  const { windows } = await ctl.send('agentSpace.list')
  const windowId = windows[0]
  const { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
  const home = spaces.find((s) => s.name === 'Space 1')
  const probe = spaces.find((s) => s.name === 'BlinkProbe')
  if (home && !home.active) {
    await ctl.send('agentSpace.spaces.switch', { windowId, spaceId: home.id })
    await sleep(800)
  }
  if (probe) {
    const r = await ctl.send('agentSpace.spaces.delete', { spaceId: probe.id })
    console.log('probe space removed:', r.ok)
  }
  const after = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
  console.log('spaces now:', after.length, 'active:', after.find((s) => s.active)?.name)
} finally { ctl.close() }
