// Targeted probe for the flicker fix: exercise Space switch + new tab, then
// leave the window in a steady state for a screen capture.
import { openControl, sleep } from './phi-test.mjs'
const ctl = await openControl()
try {
  const { windows } = await ctl.send('agentSpace.list')
  const windowId = windows[0]
  const created = await ctl.send('agentSpace.spaces.create', { name: 'BlinkProbe' })
  console.log('created', created.ok)
  const sw = await ctl.send('agentSpace.spaces.switch', { windowId, spaceId: created.id })
  console.log('switched', sw.ok)
  await sleep(1200)
  const nt = await ctl.send('agentSpace.tabs.new', {
    windowId,
    url: 'data:text/html,<title>blinkprobe</title><body style="background:%23336699">',
  })
  console.log('newtab', nt.ok)
  await sleep(1200)
  const tabs = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
  console.log('tabs in probe space:', tabs.length)
} finally { ctl.close() }
