// Space-icon emoji suite: the Mac client's emoji library, ported. The catalog
// file is the SAME artifact the Mac bundles (Resources/Emoji/emoji-catalog.json,
// Unicode 17.0), and Space icons use the Mac's storage form so a Space means
// the same thing on both clients.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

// EmojiCatalog's nine groups, in the catalog's own order.
const MAC_GROUPS = [
  'Smileys & Emotion', 'People & Body', 'Animals & Nature', 'Food & Drink',
  'Travel & Places', 'Activities', 'Objects', 'Symbols', 'Flags',
]

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

test('emoji: the Mac catalog ships and parses', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.emoji.groups')
    assertEq(r.ok, true, 'groups ok')
    assertEq(r.groups.map((g) => g.name).join('|'), MAC_GROUPS.join('|'),
             'same nine groups, same order as the Mac catalog')
    const total = r.groups.reduce((n, g) => n + g.count, 0)
    assertEq(total, 1914, 'every emoji in the Mac catalog is present')
  } finally { ctl.close() }
})

test('emoji: items carry id, text and name', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.emoji.list',
                             { group: 'Smileys & Emotion', limit: 5 })
    assertEq(r.ok, true, 'list ok')
    assertEq(r.emoji.length, 5, 'honoured the limit')
    const first = r.emoji[0]
    assertEq(first.id, '1F600', 'first item is the grinning face, as on Mac')
    assertEq(first.text, '\u{1F600}', 'and carries the emoji itself')
    assert(first.name.length > 0, 'and its name')
  } finally { ctl.close() }
})

test('emoji: a Space carries an icon in the Mac storage form', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
    assert(spaces.length >= 1, 'a space exists')
    for (const space of spaces) {
      assert(space.icon.startsWith('emoji:') || space.icon.startsWith('phi:'),
             `icon uses the Mac scheme (${space.icon})`)
    }
  } finally { ctl.close() }
})

test('emoji: setting a Space icon takes and resolves to the glyph', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const r = await ctl.send('agentSpace.spaces.setIcon',
                             { spaceId: '1', icon: 'emoji:1F680' })  // rocket
    assertEq(r.ok, true, 'setIcon ok')
    const { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
    const one = spaces.find((s) => s.id === '1')
    assertEq(one.icon, 'emoji:1F680', 'stored in the Mac form')
    assertEq(one.iconText, '\u{1F680}', 'and resolved through the catalog')
  } finally { ctl.close() }
})

test('emoji: an unknown icon is rejected, a Mac phi: icon is kept', async () => {
  const ctl = await openControl()
  try {
    const bad = await ctl.send('agentSpace.spaces.setIcon',
                               { spaceId: '1', icon: 'emoji:NOTANEMOJI' })
    assertEq(bad.ok, false, 'unknown emoji id rejected')
    // A Space configured on the Mac may carry a phi: icon. It round-trips
    // verbatim AND resolves through the ported catalog now.
    const phi = await ctl.send('agentSpace.spaces.setIcon',
                               { spaceId: '1', icon: 'phi:phi-icon-rss' })
    assertEq(phi.ok, true, 'a Mac phi: icon is accepted')
    const windowId = await firstWindow(ctl)
    const { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
    const one = spaces.find((s) => s.id === '1')
    assertEq(one.icon, 'phi:phi-icon-rss', 'kept verbatim')
    assert(one.iconText.length > 0,
           `resolved to a glyph through the phi catalog (${one.iconText})`)
    await ctl.send('agentSpace.spaces.setIcon',
                   { spaceId: '1', icon: 'emoji:1F310' })
  } finally { ctl.close() }
})

test('emoji: a Space icon survives a restart', async () => {
  let ctl = await openControl()
  try {
    assertEq((await ctl.send('agentSpace.spaces.setIcon',
                             { spaceId: '1', icon: 'emoji:1F3A8' })).ok,
             true, 'icon set')
  } finally { ctl.close() }

  const { execFileSync } = await import('node:child_process')
  execFileSync('powershell.exe', ['-NoProfile', '-Command', `
Get-Process phibrowser -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
$deadline = (Get-Date).AddSeconds(20)
while (((Get-Process phibrowser -ErrorAction SilentlyContinue) | Measure-Object).Count -gt 0 -and (Get-Date) -lt $deadline) {
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
    const { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
    assertEq(spaces.find((s) => s.id === '1').icon, 'emoji:1F3A8',
             'icon restored')
    await ctl.send('agentSpace.spaces.setIcon',
                   { spaceId: '1', icon: 'emoji:1F310' })
  } finally { ctl.close() }
})

run()
