// Menu-bar suite: the Mac menu bar (AppController+Menu.swift over Chromium's
// main_menu_builder) as a native Win32 menu on the shell window. The control
// socket reads the bar as the user would see it (dynamic rows included) and
// clicks items by their title path.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'
import { firstWindow } from './lib/window.mjs'

async function bar(ctl, windowId) {
  const r = await ctl.send('agentSpace.menu.list', { windowId })
  assertEq(r.ok, true, 'menu.list ok')
  return r.menus
}

const topMenu = (menus, title) => menus.find((m) => m.title === title)
const item = (menu, title) => menu.items.find((i) => i.title === title)

async function tabCount(ctl, windowId) {
  const r = await ctl.send('agentSpace.tabs.list', { windowId })
  return (r.tabs || []).length
}

test('menu: the Mac top-level menus, in the Mac order', async () => {
  const ctl = await openControl()
  try {
    const menus = await bar(ctl, await firstWindow(ctl))
    assertEq(menus.map((m) => m.title).join('|'),
             'Phi|File|Edit|View|Spaces|History|Bookmarks|Tab|Window|Help',
             'same bar as the Mac (minus Mac-only surfaces)')
  } finally { ctl.close() }
})

test('menu: implemented items enabled, placeholders disabled', async () => {
  const ctl = await openControl()
  try {
    const menus = await bar(ctl, await firstWindow(ctl))
    const file = topMenu(menus, 'File')
    assertEq(item(file, 'New Tab').enabled, true, 'New Tab works')
    assertEq(item(file, 'New Window').enabled, true, 'New Window works')
    // The Mac strips Save Page As's shortcut; here the whole surface waits.
    assertEq(item(file, 'Save Page As...').enabled, false,
             'Save Page As drawn but disabled')
    assertEq(item(file, 'Print...').enabled, false, 'Print drawn but disabled')
    const phi = topMenu(menus, 'Phi')
    assertEq(item(phi, 'Preferences...').enabled, true, 'Preferences works')
    assertEq(item(phi, 'Check for Update...').enabled, false,
             'Check for Update waits')
  } finally { ctl.close() }
})

test('menu: a disabled item refuses the click', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const r = await ctl.send('agentSpace.menu.click',
                             { windowId, path: ['File', 'Print...'] })
    assertEq(r.ok, false, 'Print... refuses, as a real click would')
    const missing = await ctl.send('agentSpace.menu.click',
                                   { windowId, path: ['File', 'Frobnicate'] })
    assertEq(missing.ok, false, 'an unknown item refuses too')
  } finally { ctl.close() }
})

test('menu: File > New Tab opens a tab; Close Tab closes it', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const before = await tabCount(ctl, windowId)
    assertEq((await ctl.send('agentSpace.menu.click',
                             { windowId, path: ['File', 'New Tab'] })).ok,
             true, 'New Tab clicked')
    let after = before
    for (let i = 0; i < 20 && after !== before + 1; i++) {
      await sleep(250)
      after = await tabCount(ctl, windowId)
    }
    assertEq(after, before + 1, 'one more tab')
    assertEq((await ctl.send('agentSpace.menu.click',
                             { windowId, path: ['File', 'Close Tab'] })).ok,
             true, 'Close Tab clicked')
    for (let i = 0; i < 20 && after !== before; i++) {
      await sleep(250)
      after = await tabCount(ctl, windowId)
    }
    assertEq(after, before, 'back to where we started')
  } finally { ctl.close() }
})

test('menu: View switches the layout with a radio check', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    assertEq((await ctl.send('agentSpace.menu.click',
                             { windowId, path: ['View', 'Balanced'] })).ok,
             true, 'Balanced clicked')
    await sleep(300)
    assertEq((await ctl.send('agentSpace.layout.get')).mode, 'balanced',
             'layout followed the menu')
    const view = topMenu(await bar(ctl, windowId), 'View')
    assertEq(item(view, 'Balanced').checked, true, 'Balanced checked')
    assertEq(item(view, 'Performance').checked, false,
             'Performance unchecked')
    assertEq((await ctl.send('agentSpace.menu.click',
                             { windowId, path: ['View', 'Performance'] })).ok,
             true, 'Performance restored')
    await sleep(300)
    assertEq((await ctl.send('agentSpace.layout.get')).mode, 'performance',
             'back to the default')
  } finally { ctl.close() }
})

test('menu: Spaces lists every Space and marks the active one', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const spaces = (await ctl.send('agentSpace.spaces.list',
                                   { windowId })).spaces
    const menu = topMenu(await bar(ctl, windowId), 'Spaces')
    // Space rows are the checked/checkable entries after Previous Space; they
    // carry ids in the dynamic range, so count them by exclusion.
    const fixed = ['New Space...', 'Rename Space...', 'Change Icon...',
                   'Edit Theme', 'Change Profile', 'Delete Space...',
                   'Next Space', 'Previous Space', 'URL Rules...',
                   'New Profile...', 'Delete Profile']
    const rows = menu.items.filter((i) => i.title && !fixed.includes(i.title))
    // The client caps the menu at 100 Space rows: the test box accumulates
    // junk Spaces faster than anyone can delete them (there IS no delete
    // yet), and a several-hundred-row menu serves nobody.
    const expected = Math.min(spaces.length, 100)
    assertEq(rows.length, expected, 'one row per Space (capped at 100)')
    const activeIndex = spaces.findIndex((s) => s.active)
    assert(activeIndex >= 0, 'one Space is active')
    if (activeIndex < expected) {
      const checked = rows.filter((i) => i.checked)
      assertEq(checked.length, 1, 'exactly one row checked')
      assert(checked[0].title.includes(spaces[activeIndex].name),
             `the checked row is the active Space (${checked[0].title})`)
    }
  } finally { ctl.close() }
})

test('menu: Edit Theme pins the active Space theme', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    assertEq((await ctl.send('agentSpace.menu.click',
                             { windowId,
                               path: ['Spaces', 'Edit Theme', 'Mint'] })).ok,
             true, 'Mint clicked')
    await sleep(300)
    let spaces = (await ctl.send('agentSpace.spaces.list',
                                 { windowId })).spaces
    assertEq(spaces.find((s) => s.active).themeId, 'mint',
             'the active Space pinned Mint')
    assertEq((await ctl.send('agentSpace.menu.click',
                             { windowId,
                               path: ['Spaces', 'Edit Theme',
                                      'Follow app theme'] })).ok,
             true, 'Follow app theme clicked')
    await sleep(300)
    spaces = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    assertEq(spaces.find((s) => s.active).themeId, '',
             'back to following the app')
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
      const menu = topMenu(await bar(ctl, windowId), 'Bookmarks')
      assert(item(menu, marker), 'the bookmark is a menu row')
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
