// Pinned tabs + bookmarks, stored client-side — the Windows twin of the Mac's
// LocalStore (Sources/LocalStorage). Same model: ONE row kind told apart by
// type, and the Mac's THREE pinned-tab scopes (PinnedTabScope: space, profile,
// app), which decide who owns a pinned row.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

/** Clears whatever earlier tests left, so each case starts from a known store. */
async function clearAll(ctl, windowId) {
  for (const verb of ['agentSpace.pinned.list', 'agentSpace.bookmarks.list']) {
    const key = verb.includes('pinned') ? 'pinned' : 'bookmarks'
    const r = await ctl.send(verb, { windowId })
    for (const row of r[key]) {
      await ctl.send(verb.includes('pinned') ? 'agentSpace.pinned.remove'
                                             : 'agentSpace.bookmarks.remove',
                     { guid: row.guid })
    }
  }
}

test('pinned: the three Mac scopes are offered', async () => {
  const ctl = await openControl()
  try {
    const before = (await ctl.send('agentSpace.pinned.scope.get')).scope
    for (const scope of ['space', 'profile', 'app']) {
      const r = await ctl.send('agentSpace.pinned.scope.set', { scope })
      assertEq(r.ok, true, `${scope} accepted`)
      assertEq(r.scope, scope, `${scope} is current`)
    }
    const bad = await ctl.send('agentSpace.pinned.scope.set', { scope: 'window' })
    assertEq(bad.ok, false, 'an unknown scope is rejected')
    await ctl.send('agentSpace.pinned.scope.set', { scope: before })
  } finally { ctl.close() }
})

test('pinned: a pin is stored and listed back', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await ctl.send('agentSpace.pinned.scope.set', { scope: 'profile' })
    await clearAll(ctl, windowId)
    const r = await ctl.send('agentSpace.pinned.add',
                             { windowId, title: 'Docs', url: 'https://example.com/docs' })
    assertEq(r.ok, true, 'pin ok')
    const { pinned } = await ctl.send('agentSpace.pinned.list', { windowId })
    assertEq(pinned.length, 1, 'one pin')
    assertEq(pinned[0].title, 'Docs', 'title stored')
    assertEq(pinned[0].url, 'https://example.com/docs', 'url stored')
  } finally { ctl.close() }
})

test('pinned: with no url given, the slot\'s current page is pinned', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await ctl.send('agentSpace.pinned.scope.set', { scope: 'profile' })
    await clearAll(ctl, windowId)
    const marker = `pin-${Date.now()}`
    await ctl.send('agentSpace.navigate',
                   { windowId, url: `data:text/html,<title>${marker}</title>` })
    for (let i = 0; i < 24; i++) {
      await sleep(250)
      const st = await ctl.send('agentSpace.nav.state', { windowId })
      if ((st.title || '').includes(marker)) break
    }
    assertEq((await ctl.send('agentSpace.pinned.add', { windowId })).ok, true,
             'pinned the current page')
    const { pinned } = await ctl.send('agentSpace.pinned.list', { windowId })
    assertEq(pinned.length, 1, 'one pin')
    assert(pinned[0].url.startsWith('data:'), `took the live url (${pinned[0].url})`)
  } finally { ctl.close() }
})

test('pinned: scope decides who sees a pin — space vs profile', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    // Pin under SPACE scope: it belongs to the Space that is open now.
    await ctl.send('agentSpace.pinned.scope.set', { scope: 'space' })
    await clearAll(ctl, windowId)
    await ctl.send('agentSpace.pinned.add',
                   { windowId, title: 'SpaceScoped', url: 'https://example.com/s' })
    assertEq((await ctl.send('agentSpace.pinned.list', { windowId })).pinned.length,
             1, 'visible in the Space it was pinned in')

    // Switch to another Space: a space-scoped pin must NOT follow.
    const spaces = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    const other = spaces.find((s) => !s.active)
    if (other) {
      await ctl.send('agentSpace.spaces.switch', { windowId, spaceId: other.id })
      await sleep(1200)
      assertEq((await ctl.send('agentSpace.pinned.list', { windowId })).pinned.length,
               0, 'not visible from another Space')
      await ctl.send('agentSpace.spaces.switch', { windowId, spaceId: spaces.find((s) => s.active).id })
      await sleep(1200)
    }
  } finally { ctl.close() }
})

test('pinned: widening the scope merges duplicate pins', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await ctl.send('agentSpace.pinned.scope.set', { scope: 'space' })
    await clearAll(ctl, windowId)
    // The same page pinned from two Spaces gives two space-scoped rows...
    const spaces = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    const active = spaces.find((s) => s.active)
    const other = spaces.find((s) => !s.active)
    await ctl.send('agentSpace.pinned.add',
                   { windowId, title: 'Same', url: 'https://example.com/same' })
    if (other) {
      await ctl.send('agentSpace.spaces.switch', { windowId, spaceId: other.id })
      await sleep(1200)
      await ctl.send('agentSpace.pinned.add',
                     { windowId, title: 'Same', url: 'https://example.com/same' })
      await ctl.send('agentSpace.spaces.switch', { windowId, spaceId: active.id })
      await sleep(1200)
    }
    // ...and widening to profile scope collapses them into one, as the Mac's
    // scope migration does (same title+url signature).
    await ctl.send('agentSpace.pinned.scope.set', { scope: 'profile' })
    const { pinned } = await ctl.send('agentSpace.pinned.list', { windowId })
    const same = pinned.filter((p) => p.url === 'https://example.com/same')
    assertEq(same.length, 1, `merged into one pin (saw ${same.length})`)
  } finally { ctl.close() }
})

test('bookmarks: added, listed, and folders nest', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await clearAll(ctl, windowId)
    const folder = await ctl.send('agentSpace.bookmarks.add',
                                  { windowId, title: 'WORK', folder: true })
    assertEq(folder.ok, true, 'folder added')
    const child = await ctl.send('agentSpace.bookmarks.add',
                                 { windowId, title: 'Repo',
                                   url: 'https://example.com/repo',
                                   parentGuid: folder.guid })
    assertEq(child.ok, true, 'child added')

    const top = await ctl.send('agentSpace.bookmarks.list', { windowId })
    assertEq(top.bookmarks.length, 1, 'only the folder at the root')
    assertEq(top.bookmarks[0].folder, true, 'and it is a folder')

    const inside = await ctl.send('agentSpace.bookmarks.list',
                                  { windowId, parentGuid: folder.guid })
    assertEq(inside.bookmarks.length, 1, 'one bookmark inside')
    assertEq(inside.bookmarks[0].title, 'Repo', 'the child')
  } finally { ctl.close() }
})

test('bookmarks: removing a folder takes its children with it', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await clearAll(ctl, windowId)
    const folder = await ctl.send('agentSpace.bookmarks.add',
                                  { windowId, title: 'Temp', folder: true })
    await ctl.send('agentSpace.bookmarks.add',
                   { windowId, title: 'Child', url: 'https://example.com/c',
                     parentGuid: folder.guid })
    assertEq((await ctl.send('agentSpace.bookmarks.remove',
                             { guid: folder.guid })).ok, true, 'folder removed')
    const inside = await ctl.send('agentSpace.bookmarks.list',
                                  { windowId, parentGuid: folder.guid })
    assertEq(inside.bookmarks.length, 0, 'the child went with it')
  } finally { ctl.close() }
})

test('pinned: pins and bookmarks survive a restart', async () => {
  let ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await ctl.send('agentSpace.pinned.scope.set', { scope: 'profile' })
    await clearAll(ctl, windowId)
    await ctl.send('agentSpace.pinned.add',
                   { windowId, title: 'Kept', url: 'https://example.com/kept' })
    await ctl.send('agentSpace.bookmarks.add',
                   { windowId, title: 'KeptMark', url: 'https://example.com/mark' })
  } finally { ctl.close() }

  const { execFileSync } = await import('node:child_process')
  execFileSync('powershell.exe', ['-NoProfile', '-Command', `
Get-Process Phi -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
$deadline = (Get-Date).AddSeconds(20)
while (((Get-Process Phi -ErrorAction SilentlyContinue) | Measure-Object).Count -gt 0 -and (Get-Date) -lt $deadline) {
  Start-Sleep -Milliseconds 400
}
Start-Sleep 1
Start-ScheduledTask -TaskName PhiFxLaunch
`], { stdio: 'ignore' })
  for (let i = 0; i < 40; i++) {
    await sleep(1000)
    try {
      const probe = await openControl()
      const r = await probe.send('agentSpace.ping')
      probe.close()
      if (r.ok) break
    } catch { /* not up yet */ }
  }

  ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const { pinned, scope } = await ctl.send('agentSpace.pinned.list', { windowId })
    assertEq(scope, 'profile', 'scope restored')
    assert(pinned.some((p) => p.url === 'https://example.com/kept'), 'pin restored')
    const { bookmarks } = await ctl.send('agentSpace.bookmarks.list', { windowId })
    assert(bookmarks.some((b) => b.url === 'https://example.com/mark'),
           'bookmark restored')
    await clearAll(ctl, windowId)
  } finally { ctl.close() }
})

test('pinned: a pin keeps the favicon of the page it was made from', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await ctl.send('agentSpace.pinned.scope.set', { scope: 'profile' })
    await clearAll(ctl, windowId)
    // A real site, so there is a real favicon to capture. A pinned tab has no
    // live tab to ask later, so the bytes have to be stored with the row.
    await ctl.send('agentSpace.navigate', { windowId, url: 'https://github.com' })
    let landed = null
    for (let i = 0; i < 40; i++) {
      await sleep(500)
      const st = await ctl.send('agentSpace.nav.state', { windowId })
      if (!st.loading && (st.url || '').includes('github')) { landed = st; break }
    }
    // Say so plainly rather than blaming the favicon for a network failure.
    assert(landed, 'the page actually loaded (needs network on the test box)')

    // A favicon arrives on its own schedule — after the navigation, and slower
    // on a cold start — so pin until one comes through rather than guessing a
    // sleep. `add` reports what it captured.
    let captured = null
    for (let i = 0; i < 20 && !captured; i++) {
      await clearAll(ctl, windowId)
      const r = await ctl.send('agentSpace.pinned.add', { windowId })
      assertEq(r.ok, true, 'pinned')
      if (r.faviconWidth > 0) {
        captured = r
      } else {
        await sleep(1000)
      }
    }
    assert(captured, 'a favicon was captured for the pinned page')
    assert(captured.faviconWidth > 0 && captured.faviconHeight > 0,
           `with a size (${captured.faviconWidth}x${captured.faviconHeight})`)

    const { pinned } = await ctl.send('agentSpace.pinned.list', { windowId })
    assertEq(pinned.length, 1, 'one pin')
    assert(pinned[0].favicon && pinned[0].favicon.length > 0,
           'and the bytes are stored on the row')
    await clearAll(ctl, windowId)
  } finally { ctl.close() }
})

test('pinned: rows.reorder moves a pin among its siblings', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const guids = []
    for (const name of ['RoA', 'RoB', 'RoC']) {
      const r = await ctl.send('agentSpace.pinned.add',
          { windowId, title: name, url: `https://example.com/${name}` })
      assertEq(r.ok, true, `${name} pinned`)
      guids.push(r.guid)
    }
    assertEq((await ctl.send('agentSpace.rows.reorder',
                             { guid: guids[2], index: 0 })).ok,
             true, 'reorder ok')
    await sleep(300)
    const pins = (await ctl.send('agentSpace.pinned.list', { windowId })).pinned
    const order = pins.filter((p) => p.title.startsWith('Ro')).map((p) => p.title)
    assertEq(JSON.stringify(order), JSON.stringify(['RoC', 'RoA', 'RoB']),
             'RoC moved to the front, others kept order')
    for (const g of guids) {
      await ctl.send('agentSpace.pinned.remove', { guid: g })
    }
  } finally { ctl.close() }
})

run()
