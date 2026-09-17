// space-hover-setup.mjs [mode]: makes sure the window has two Spaces (creates
// "Research" when it has one), switches the layout, parks the window and
// prints where the pips / chip sit (client px) for the capture rigs.
import { openControl } from '../phi-test.mjs'
import { firstWindow } from '../lib/window.mjs'
const mode = process.argv[2] || 'balanced'
const ctl = await openControl()
try {
  const windowId = await firstWindow(ctl)
  let { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
  if ((spaces || []).length < 2) {
    console.log(JSON.stringify(await ctl.send('agentSpace.spaces.create', { windowId, name: 'Research' })))
    await new Promise((r) => setTimeout(r, 600))
    spaces = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
  }
  await ctl.send('agentSpace.layout.set', { mode })
  await ctl.send('agentSpace.window.setBounds', { windowId, x: 69, y: 40, width: 1400, height: 900 })
  await new Promise((r) => setTimeout(r, 500))
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  const s = l.dpi / 96
  console.log(JSON.stringify({ mode: l.mode, dpi: l.dpi, spaces: spaces.map((x) => ({ id: x.id, name: x.name, active: x.active })),
    pip1: [Math.round(22 * s), Math.round(22 * s)], pip2: [Math.round(54 * s), Math.round(22 * s)], chip: [Math.round(40 * s), Math.round(23 * s)] }))
} finally { ctl.close() }
