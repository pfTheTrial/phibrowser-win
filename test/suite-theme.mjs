// Theme suite: the Mac client's theme model, ported — eight built-in themes
// (Utilities/Theme/ThemedColors.swift) and the three-state appearance choice
// (ThemeManager.UserAppearanceChoice), both persisted and applied client-wide.
import { execFileSync } from 'node:child_process'
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

// The Mac's built-in order: Theme.builtInThemes.
const MAC_THEMES = ['pure', 'mint', 'mist', 'aqua', 'iris', 'petal', 'coral', 'amber']

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

test('theme: the eight Mac built-ins are offered, in order', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.theme.get')
    assertEq(r.ok, true, 'theme.get ok')
    assertEq(r.themes.map((t) => t.id).join(','), MAC_THEMES.join(','),
             'same themes, same order as the Mac')
    assert(r.themes.every((t) => t.name && t.name.length), 'each has a name')
  } finally { ctl.close() }
})

test('theme: an unknown theme or appearance is rejected', async () => {
  const ctl = await openControl()
  try {
    const before = await ctl.send('agentSpace.theme.get')
    const r = await ctl.send('agentSpace.theme.set', { themeId: 'chartreuse' })
    assertEq(r.ok, false, 'unknown theme rejected')
    assertEq(r.themeId, before.themeId, 'theme unchanged')
    const a = await ctl.send('agentSpace.theme.set', { appearance: 'sepia' })
    assertEq(a.ok, false, 'unknown appearance rejected')
  } finally { ctl.close() }
})

test('theme: choosing a theme takes effect and is reported back', async () => {
  const ctl = await openControl()
  try {
    for (const id of ['mint', 'coral', 'pure']) {
      const r = await ctl.send('agentSpace.theme.set', { themeId: id })
      assertEq(r.ok, true, `set ${id} ok`)
      assertEq(r.themeId, id, `${id} is current`)
      assertEq((await ctl.send('agentSpace.theme.get')).themeId, id,
               `${id} survives a re-read`)
    }
  } finally { ctl.close() }
})

test('theme: the appearance choice drives light vs dark', async () => {
  const ctl = await openControl()
  try {
    const light = await ctl.send('agentSpace.theme.set', { appearance: 'light' })
    assertEq(light.dark, false, 'light resolves light')
    const dark = await ctl.send('agentSpace.theme.set', { appearance: 'dark' })
    assertEq(dark.dark, true, 'dark resolves dark')
    // System follows Windows, so assert only that it resolves to a boolean.
    const system = await ctl.send('agentSpace.theme.set', { appearance: 'system' })
    assertEq(system.appearance, 'system', 'system accepted')
    assertEq(typeof system.dark, 'boolean', 'system resolves to one of them')
  } finally { ctl.close() }
})

test('theme: the chrome really repaints in the chosen theme (pixels)', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    await ctl.send('agentSpace.layout.set', { mode: 'performance' })
    await ctl.send('agentSpace.theme.set', { appearance: 'light' })
    const h = await ctl.send('agentSpace.window.handle', { windowId })
    const geo = await ctl.send('agentSpace.window.layout', { windowId })
    const probe = () => samplePixel(h.hwnd, 100 + (geo.frameLeft || 0),
                                    400 + (geo.frameTop || 0))

    await ctl.send('agentSpace.theme.set', { themeId: 'mint' })
    await sleep(500)
    const mint = probe()
    await ctl.send('agentSpace.theme.set', { themeId: 'coral' })
    await sleep(500)
    const coral = probe()
    assert(mint !== coral, `sidebar retints (mint ${mint} vs coral ${coral})`)
    // Mint leans green, Coral leans red — the ported palettes, not just "different".
    const [mr, mg] = mint.split(',').map(Number)
    const [cr, , cb] = coral.split(',').map(Number)
    assert(mg > mr, `mint is green-dominant (${mint})`)
    assert(cr > cb, `coral is red-dominant (${coral})`)

    await ctl.send('agentSpace.theme.set', { themeId: 'pure' })
  } finally { ctl.close() }
})

test('theme: the settings window opens', async () => {
  const ctl = await openControl()
  try {
    // The client reports its own HWND: another session cannot enumerate our
    // windows to look for it.
    const r = await ctl.send('agentSpace.settings.open')
    assertEq(r.ok, true, 'settings window opened')
    assert(r.hwnd > 0, `and has a real window (${r.hwnd})`)
  } finally { ctl.close() }
})

function samplePixel(hwnd, x, y) {
  const ps = [
    `[Environment]::SetEnvironmentVariable('PHI_UICAP_POINTS','${x},${y}','User')`,
    `[Environment]::SetEnvironmentVariable('PHI_UICAP_HWND','${hwnd}','User')`,
    `Remove-Item C:\\Users\\jdxin\\uicap.txt -ErrorAction SilentlyContinue`,
    `Start-ScheduledTask -TaskName PhiUiCap`,
    `$deadline = (Get-Date).AddSeconds(20)`,
    `do { Start-Sleep -Milliseconds 300; $t = (Get-Content C:\\Users\\jdxin\\uicap.txt -ErrorAction SilentlyContinue) } ` +
      `until (($t -and ($t -contains 'DONE')) -or (Get-Date) -gt $deadline)`,
    `$t`,
  ].join('; ')
  const out = execFileSync('powershell.exe',
    ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', ps],
    { encoding: 'utf8' })
  for (const line of out.split(/\r?\n/)) {
    const m = /^PX \S+ (\d+,\d+,\d+)$/.exec(line.trim())
    if (m) return m[1]
  }
  throw new Error(`capture failed: ${out.trim().split(/\r?\n/)[0] || 'no output'}`)
}

test('theme: a Space can pin its own theme', async () => {
  const ctl = await openControl()
  try {
    const { windows } = await ctl.send('agentSpace.list')
    const windowId = windows[0]
    await ctl.send('agentSpace.theme.set', { themeId: 'pure', appearance: 'light' })
    const { spaces } = await ctl.send('agentSpace.spaces.list', { windowId })
    const active = spaces.find((s) => s.active)
    assert(active, 'a space is active')

    // Pinning a theme to the Space leaves the app-wide theme alone — that is
    // the whole point of the override (the Mac's spaceThemeIds map).
    const r = await ctl.send('agentSpace.spaces.setTheme',
                             { spaceId: active.id, themeId: 'coral' })
    assertEq(r.ok, true, 'space theme set')
    assertEq(r.themeId, 'coral', 'reported back')
    assertEq((await ctl.send('agentSpace.theme.get')).themeId, 'pure',
             'the app-wide theme is untouched')
    const after = (await ctl.send('agentSpace.spaces.list', { windowId })).spaces
    assertEq(after.find((s) => s.id === active.id).themeId, 'coral',
             'listed on the Space')

    // Clearing it puts the Space back on the app-wide theme.
    assertEq((await ctl.send('agentSpace.spaces.setTheme',
                             { spaceId: active.id, themeId: '' })).themeId, '',
             'override cleared')
    assertEq((await ctl.send('agentSpace.spaces.setTheme',
                             { spaceId: 'no-such-space', themeId: 'mint' })).ok,
             false, 'an unknown Space is rejected')
    assertEq((await ctl.send('agentSpace.spaces.setTheme',
                             { spaceId: active.id, themeId: 'chartreuse' })).ok,
             false, 'an unknown theme is rejected')
  } finally { ctl.close() }
})

test('theme: a Space\'s pinned theme paints its window (pixels)', async () => {
  const ctl = await openControl()
  try {
    const { windows } = await ctl.send('agentSpace.list')
    const windowId = windows[0]
    await ctl.send('agentSpace.window.setBounds',
                   { windowId, x: 60, y: 60, width: 1200, height: 900 })
    await ctl.send('agentSpace.layout.set', { mode: 'performance' })
    await ctl.send('agentSpace.theme.set', { themeId: 'mint', appearance: 'light' })
    const h = await ctl.send('agentSpace.window.handle', { windowId })
    const geo = await ctl.send('agentSpace.window.layout', { windowId })
    const probe = () => samplePixel(h.hwnd, 100 + (geo.frameLeft || 0),
                                    400 + (geo.frameTop || 0))
    const active = (await ctl.send('agentSpace.spaces.list', { windowId }))
                     .spaces.find((s) => s.active)

    await sleep(500)
    const following = probe()   // Mint, the app-wide theme.
    await ctl.send('agentSpace.spaces.setTheme',
                   { spaceId: active.id, themeId: 'coral' })
    await sleep(600)
    const pinned = probe()      // Coral, pinned to this Space only.
    assert(following !== pinned,
           `the window retints for the Space (${following} -> ${pinned})`)
    const [fr, fg] = following.split(',').map(Number)
    const [pr, , pb] = pinned.split(',').map(Number)
    assert(fg > fr, `app-wide mint is green-dominant (${following})`)
    assert(pr > pb, `the Space's coral is red-dominant (${pinned})`)

    await ctl.send('agentSpace.spaces.setTheme',
                   { spaceId: active.id, themeId: '' })
    await ctl.send('agentSpace.theme.set', { themeId: 'pure', appearance: 'light' })
  } finally { ctl.close() }
})

run()
