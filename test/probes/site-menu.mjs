// site-menu.mjs [balanced|comfortable] [url]: switches the layout, parks the
// window at the rigs' origin, optionally opens `url` in a new tab, and
// prints the address field with its in-field slots (Copy link, the site
// menu, the star) in CLIENT px — what PhiRm aims at.
import { openControl } from '../phi-test.mjs'
import { firstWindow } from '../lib/window.mjs'
const [mode, url] = process.argv.slice(2)
const ctl = await openControl()
try {
  const windowId = await firstWindow(ctl)
  if (mode) await ctl.send('agentSpace.layout.set', { mode })
  if (url) await ctl.send('agentSpace.tabs.new', { windowId, url })
  await ctl.send('agentSpace.window.setBounds', { windowId, x: 69, y: 40, width: 1400, height: 900 })
  await new Promise((r) => setTimeout(r, url ? 2500 : 500))
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  console.log(JSON.stringify({ mode: l.mode, dpi: l.dpi, urlField: l.urlField, reader: l.reader,
    copyLink: l.copyLink, siteMenu: l.siteMenu, appMenu: l.appMenu }))
} finally { ctl.close() }
