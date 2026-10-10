// Prints the Update reminder pill's rect (CLIENT px) for a layout, after
// parking the window at the rigs' origin — what PhiRm aims at.
//   node upd-pill.mjs [balanced|comfortable] [sidebarWidth]
import { openControl } from '../phi-test.mjs'
import { firstWindow } from '../lib/window.mjs'
const [mode, width] = process.argv.slice(2)
const ctl = await openControl()
try {
  if (mode) {
    const set = { mode }
    if (width) set.sidebarWidth = Number(width)
    await ctl.send('agentSpace.layout.set', set)
  }
  const windowId = await firstWindow(ctl)
  await ctl.send('agentSpace.window.setBounds', { windowId, x: 69, y: 40, width: 1400, height: 900 })
  await new Promise((r) => setTimeout(r, 600))
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  console.log(JSON.stringify({ mode: l.mode, dpi: l.dpi, update: l.update, appMenu: l.appMenu,
    sidebarToggle: l.sidebarToggle, sidebarCollapsed: l.sidebarCollapsed, sidebarWidth: l.sidebarWidth,
    urlField: l.urlField, frameLeft: l.frameLeft, frameTop: l.frameTop }))
} finally { ctl.close() }
