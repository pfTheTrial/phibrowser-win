// Prints where the app menu button (⋯) and the sidebar sit, in CLIENT px,
// after parking the window at the rigs' origin — what PhiRm aims at.
import { openControl } from '../phi-test.mjs'
import { firstWindow } from '../lib/window.mjs'
const ctl = await openControl()
try {
  const windowId = await firstWindow(ctl)
  await ctl.send('agentSpace.window.setBounds', { windowId, x: 69, y: 40, width: 1400, height: 900 })
  await new Promise((r) => setTimeout(r, 400))
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  console.log(JSON.stringify({ mode: l.mode, dpi: l.dpi, appMenu: l.appMenu, sidebarToggle: l.sidebarToggle,
    content: l.content, urlField: l.urlField, frameLeft: l.frameLeft, frameTop: l.frameTop, titleBar: l.titleBar }))
} finally { ctl.close() }
