// layout-set.mjs <balanced|comfortable> [sidebar hidden|docked]: switches the
// layout (and the sidebar) over the control pipe, parks the window, prints
// the geometry.
import { openControl } from '../phi-test.mjs'
import { firstWindow } from '../lib/window.mjs'
const [mode, sidebar] = process.argv.slice(2)
const ctl = await openControl()
try {
  const windowId = await firstWindow(ctl)
  if (mode) console.log(JSON.stringify(await ctl.send('agentSpace.layout.set', { mode })))
  if (sidebar) console.log(JSON.stringify(await ctl.send('agentSpace.layout.set', { windowId, sidebar })))
  await ctl.send('agentSpace.window.setBounds', { windowId, x: 69, y: 40, width: 1400, height: 900 })
  await new Promise((r) => setTimeout(r, 500))
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  console.log(JSON.stringify({ mode: l.mode, dpi: l.dpi, content: l.content, urlField: l.urlField, appMenu: l.appMenu, sidebarToggle: l.sidebarToggle, sidebarCollapsed: l.sidebarCollapsed }))
} finally { ctl.close() }
