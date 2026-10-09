// App-menu suite: the Windows client's ⋯ "Settings and more" flyout
// (docs/windows-native-ui.md §3), which replaced the Mac-style menu bar. The
// control socket reads the menu as the user would see it (dynamic rows
// included) and clicks items by their title path from the root.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'
import { firstWindow } from './lib/window.mjs'

async function menu(ctl, windowId) {
  const r = await ctl.send('agentSpace.menu.list', { windowId })
  assertEq(r.ok, true, 'menu.list ok')
  return r.menus  // The root's rows; submenus nest under `items`.
}

const item = (rows, title) => rows.find((i) => i.title === title)
const sub = (rows, title) => (item(rows, title) || {}).items || []

async function tabCount(ctl, windowId) {
  const r = await ctl.send('agentSpace.tabs.list', { windowId })
  return (r.tabs || []).length
}

test('menu: the app menu, top to bottom', async () => {
  const ctl = await openControl()
  try {
    const rows = await menu(ctl, await firstWindow(ctl))
    const titles = rows.filter((r) => r.title).map((r) => r.title)
    assertEq(titles.join('|'),
             'New tab|New window|New incognito window|New incognito Space|' +
             'Spaces|History|' +
             'Bookmarks|Downloads|Extensions|Zoom|Print…|Find on page…|' +
             'Reader view|Layout|More tools|Settings|Help|Exit',
             'Chrome and Edge\'s order (docs/windows-native-ui.md §3.1)')
    assertEq(item(rows, 'New tab').shortcut, 'Ctrl+T', 'the chord rides the row')
    for (const name of ['Spaces', 'History', 'Bookmarks', 'Extensions',
                        'Zoom', 'Layout', 'More tools', 'Help']) {
      assert(Array.isArray(item(rows, name).items), `${name} is a submenu`)
    }
  } finally { ctl.close() }
})

test('menu: implemented items enabled, placeholders disabled', async () => {
  const ctl = await openControl()
  try {
    const rows = await menu(ctl, await firstWindow(ctl))
    assertEq(item(rows, 'New tab').enabled, true, 'New tab works')
    assertEq(item(rows, 'New window').enabled, true, 'New window works')
    assertEq(item(rows, 'New incognito Space').enabled, true,
             'New incognito Space works')
    assertEq(item(rows, 'New incognito Space').shortcut, 'Ctrl+Alt+N',
             'its chord rides the row')
    assertEq(item(rows, 'Settings').enabled, true, 'Settings works')
    // Chromium's own command state drives the routed rows (incognito is
    // live through IDC_NEW_INCOGNITO_WINDOW); the unrouted placeholders
    // stay off.
    const tools0 = sub(rows, 'More tools')
    assertEq(item(tools0, 'Export user data…').enabled, false,
             'Export user data waits')
    const tools = sub(rows, 'More tools')
    assert(item(tools, 'Task manager'), 'Task manager listed under More tools')
    const help = sub(rows, 'Help')
    assert(item(help, 'About Phi'), 'About lives under Help')
    assertEq(item(help, 'Check for update…').enabled, false,
             'Check for update waits without the updater')
  } finally { ctl.close() }
})

test('menu: a disabled item refuses the click', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const r = await ctl.send('agentSpace.menu.click',
                             { windowId, path: ['More tools', 'Export user data…'] })
    assertEq(r.ok, false, 'a disabled row refuses, as a real click would')
    const missing = await ctl.send('agentSpace.menu.click',
                                   { windowId, path: ['Frobnicate'] })
    assertEq(missing.ok, false, 'an unknown item refuses too')
    const submenu = await ctl.send('agentSpace.menu.click',
                                   { windowId, path: ['More tools'] })
    assertEq(submenu.ok, false, 'a submenu itself cannot be activated')
  } finally { ctl.close() }
})

test('menu: New tab opens a tab', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const before = await tabCount(ctl, windowId)
    assertEq((await ctl.send('agentSpace.menu.click',
                             { windowId, path: ['New tab'] })).ok,
             true, 'New tab clicked')
    let after = before
    for (let i = 0; i < 20 && after !== before + 1; i++) {
      await sleep(250)
      after = await tabCount(ctl, windowId)
    }
    assertEq(after, before + 1, 'one more tab')
    const tabs = (await ctl.send('agentSpace.tabs.list', { windowId })).tabs
    await ctl.send('agentSpace.tabs.close',
                   { windowId, index: tabs[tabs.length - 1].index })
  } finally { ctl.close() }
})

test('menu: Layout switches with a radio check', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const start = (await ctl.send('agentSpace.layout.get')).mode
    assertEq((await ctl.send('agentSpace.menu.click',
                             { windowId, path: ['Layout', 'Horizontal tabs'] })).ok,
             true, 'Horizontal tabs clicked')
    await sleep(300)
    assertEq((await ctl.send('agentSpace.layout.get')).mode, 'comfortable',
             'layout followed the menu')
    let layout = sub(await menu(ctl, windowId), 'Layout')
    assertEq(item(layout, 'Horizontal tabs').checked, true, 'Horizontal checked')
    assertEq(item(layout, 'Vertical tabs').checked, false, 'Vertical unchecked')
    assertEq(item(layout, 'Show sidebar').enabled, false,
             'no sidebar to show in the horizontal layout')
    assertEq((await ctl.send('agentSpace.menu.click',
                             { windowId, path: ['Layout', 'Vertical tabs'] })).ok,
             true, 'Vertical tabs clicked')
    await sleep(300)
    assertEq((await ctl.send('agentSpace.layout.get')).mode, 'balanced',
             'vertical is balanced on the wire')
    layout = sub(await menu(ctl, windowId), 'Layout')
    assertEq(item(layout, 'Show sidebar').enabled, true, 'sidebar row live')
    if (start === 'comfortable') {
      await ctl.send('agentSpace.menu.click',
                     { windowId, path: ['Layout', 'Horizontal tabs'] })
    }
  } finally { ctl.close() }
})

test('menu: Spaces lists every Space and marks the active one', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const spaces = (await ctl.send('agentSpace.spaces.list',
                                   { windowId })).spaces
    const rows = sub(await menu(ctl, windowId), 'Spaces')
    const fixed = ['New Space…', 'Rename Space…', 'Change icon…',
                   'Edit theme…', 'Change profile', 'Delete Space…',
                   'Next Space', 'Previous Space', 'URL rules…',
                   'New profile…', 'Delete profile']
    const space_rows = rows.filter((i) => i.title && !fixed.includes(i.title))
    // The client caps the menu at 100 Space rows.
    const expected = Math.min(spaces.length, 100)
    assertEq(space_rows.length, expected, 'one row per Space (capped at 100)')
    const activeIndex = spaces.findIndex((s) => s.active)
    assert(activeIndex >= 0, 'one Space is active')
    if (activeIndex < expected) {
      const checked = space_rows.filter((i) => i.checked)
      assertEq(checked.length, 1, 'exactly one row checked')
      assert(checked[0].title.includes(spaces[activeIndex].name),
             `the checked row is the active Space (${checked[0].title})`)
      assertEq(space_rows[0].shortcut, 'Ctrl+1', 'the first Space is Ctrl+1')
    }
  } finally { ctl.close() }
})

test('menu: a bookmark shows as a row and opens on click', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const marker = `menu-bm-${Date.now()}`
    const url = `data:text/html,<title>${marker}</title>`
    const added = await ctl.send('agentSpace.bookmarks.add',
                                 { windowId, title: marker, url })
    assertEq(added.ok, true, 'bookmark added')
    try {
      const rows = sub(await menu(ctl, windowId), 'Bookmarks')
      assert(item(rows, marker), 'the bookmark is a menu row')
      assertEq((await ctl.send('agentSpace.menu.click',
                               { windowId, path: ['Bookmarks', marker] })).ok,
               true, 'bookmark clicked')
      let opened = null
      for (let i = 0; i < 24 && !opened; i++) {
        await sleep(250)
        const tabs = (await ctl.send('agentSpace.tabs.list',
                                     { windowId })).tabs || []
        opened = tabs.find((t) => (t.title || '').includes(marker))
      }
      assert(opened, 'the bookmark opened in a tab')
      await ctl.send('agentSpace.tabs.close',
                     { windowId, index: opened.index })
    } finally {
      await ctl.send('agentSpace.bookmarks.remove', { guid: added.guid })
    }
  } finally { ctl.close() }
})

run()
