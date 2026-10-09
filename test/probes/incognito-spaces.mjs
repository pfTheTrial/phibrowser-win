// incognito-spaces.mjs: prints the app menu's Spaces submenu (names and
// chords) and the root's New incognito Space row — what an Incognito Space
// port check reads without pixels (docs/incognito-space-port.md).
import { openControl } from '../phi-test.mjs'
import { firstWindow } from '../lib/window.mjs'
const ctl = await openControl()
try {
  const windowId = await firstWindow(ctl)
  const r = await ctl.send('agentSpace.menu.list', { windowId })
  const items = r.menus || []
  const row = (list, title) => list.find((i) => i.title === title)
  const spaces = row(items, 'Spaces')
  console.log(JSON.stringify({
    newIncognitoSpace: row(items, 'New incognito Space'),
    spaces: (spaces?.items || []).map((i) => [i.title, i.shortcut, i.checked].filter((x) => x !== undefined)),
  }))
  const focus = await ctl.send('agentSpace.spaces.focus', { windowId })
  console.log(JSON.stringify(focus))
} finally { ctl.close() }
