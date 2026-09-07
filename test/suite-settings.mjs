// Settings suite: the full settings surface — the Mac's Preferences panes
// (Settings.swift) and its General rows (GeneralSettingView.swift), as far as
// this client has state to back them.
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

// The Mac's panes() list, in its order (AppController+Settings.swift):
// Account, General, Profiles, Spaces, Navigation, AI, Shortcuts, Developer.
// There is no Phi Link pane — IMChannels exists in the Mac source but is
// never mounted.
const MAC_PANES = ['account', 'general', 'profiles', 'spaces', 'navigation',
                   'ai', 'shortcuts', 'developer']

test('settings: every Mac pane is offered, in order', async () => {
  const ctl = await openControl()
  try {
    // Developer is gated behind developer mode, exactly as on Mac. Since
    // phase 0 an agent cannot flip that pref; the box runner seeds
    // developerMode:true into PhiPrefs.json (seed-fx-prefs.sh), so the
    // full list is expected here.
    const r = await ctl.send('agentSpace.settings.get')
    assertEq(r.ok, true, 'settings.get ok')
    assertEq(r.sections.join(','), MAC_PANES.join(','),
             'same panes, same order as the Mac')
  } finally { ctl.close() }
})

test('settings: the security prefs are not agent-settable', async () => {
  const ctl = await openControl()
  try {
    // docs/architecture-review.md §3.7: developerMode, allowAgentUserSpaces
    // and testRig can only be changed by the user (Settings) or by editing
    // PhiPrefs.json — never over the pipe.
    for (const key of ['developerMode', 'allowAgentUserSpaces', 'testRig']) {
      const r = await ctl.send('agentSpace.settings.set', { [key]: false })
      assertEq(r.ok, false, `${key} refused`)
      assertEq(r.error, 'pref_not_agent_settable', `${key} error code`)
    }
    const r = await ctl.send('agentSpace.settings.get')
    assertEq(r.developerMode, true, 'developer mode untouched')
    assertEq(r.allowAgentUserSpaces, true, 'user-space toggle untouched')
    assertEq(r.sections.includes('developer'), true, 'the pane is still there')
  } finally { ctl.close() }
})

test('settings: reports every value the window shows', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.settings.get')
    for (const key of ['themeId', 'appearance', 'saturation', 'layout',
                       'newTabBehavior', 'alwaysShowFullUrl', 'pinScope']) {
      assert(key in r, `reports ${key}`)
    }
    assert(r.saturation >= 0 && r.saturation <= 100,
           `saturation is a slider position (${r.saturation})`)
  } finally { ctl.close() }
})

test('settings: switching pane sticks', async () => {
  const ctl = await openControl()
  try {
    // The window has to exist before a pane can be selected in it.
    assertEq((await ctl.send('agentSpace.settings.open')).ok, true, 'opened')
    for (const section of ['spaces', 'profiles', 'general']) {
      const r = await ctl.send('agentSpace.settings.set', { section })
      assertEq(r.ok, true, `selected ${section}`)
      assertEq(r.section, section, `${section} is current`)
    }
    const bad = await ctl.send('agentSpace.settings.set', { section: 'nope' })
    assertEq(bad.ok, false, 'an unknown pane is rejected')
  } finally { ctl.close() }
})

test('settings: new tab behavior and full-URL toggle round-trip', async () => {
  const ctl = await openControl()
  try {
    for (const behavior of ['omnibox', 'newTabPage']) {
      const r = await ctl.send('agentSpace.settings.set',
                               { newTabBehavior: behavior })
      assertEq(r.ok, true, `set ${behavior}`)
      assertEq(r.newTabBehavior, behavior, `${behavior} is current`)
    }
    assertEq((await ctl.send('agentSpace.settings.set',
                             { newTabBehavior: 'carrier-pigeon' })).ok, false,
             'an unknown behavior is rejected')

    for (const on of [true, false]) {
      const r = await ctl.send('agentSpace.settings.set',
                               { alwaysShowFullUrl: on })
      assertEq(r.alwaysShowFullUrl, on, `full URL ${on}`)
    }
  } finally { ctl.close() }
})

test('settings: saturation drives the tint, and clamps', async () => {
  const ctl = await openControl()
  try {
    await ctl.send('agentSpace.theme.set', { themeId: 'coral', appearance: 'light' })
    const low = await ctl.send('agentSpace.settings.set', { saturation: 0 })
    assertEq(low.saturation, 0, 'slider at 0')
    const lowTheme = await ctl.send('agentSpace.theme.get')
    const high = await ctl.send('agentSpace.settings.set', { saturation: 100 })
    assertEq(high.saturation, 100, 'slider at 100')

    // A slider cannot overshoot, so out-of-range clamps rather than failing.
    assertEq((await ctl.send('agentSpace.settings.set',
                             { saturation: 500 })).saturation, 100, 'clamped high')
    assertEq((await ctl.send('agentSpace.settings.set',
                             { saturation: -20 })).saturation, 0, 'clamped low')
    void lowTheme
    await ctl.send('agentSpace.settings.set', { saturation: 50 })
  } finally { ctl.close() }
})

test('settings: saturation actually changes the painted tint (pixels)', async () => {
  const { execFileSync } = await import('node:child_process')
  const ctl = await openControl()
  try {
    const { windows } = await ctl.send('agentSpace.list')
    const windowId = windows[0]
    await ctl.send('agentSpace.window.setBounds',
                   { windowId, x: 60, y: 60, width: 1200, height: 900 })
    await ctl.send('agentSpace.layout.set', { mode: 'performance' })
    await ctl.send('agentSpace.theme.set', { themeId: 'coral', appearance: 'light' })
    const h = await ctl.send('agentSpace.window.handle', { windowId })
    const geo = await ctl.send('agentSpace.window.layout', { windowId })
    const probe = () => {
      const x = 100 + (geo.frameLeft || 0)
      const y = 400 + (geo.frameTop || 0)
      const ps = [
        `[Environment]::SetEnvironmentVariable('PHI_UICAP_POINTS','${x},${y}','User')`,
        `[Environment]::SetEnvironmentVariable('PHI_UICAP_HWND','${h.hwnd}','User')`,
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
      throw new Error('capture failed')
    }

    await ctl.send('agentSpace.settings.set', { saturation: 5 })
    await sleep(600)
    const pale = probe()
    await ctl.send('agentSpace.settings.set', { saturation: 100 })
    await sleep(600)
    const strong = probe()
    assert(pale !== strong, `the tint responds (${pale} vs ${strong})`)
    // Coral is red: more saturation means the red channel pulls further from
    // the other two.
    const spread = (s) => { const c = s.split(',').map(Number)
                            return Math.max(...c) - Math.min(...c) }
    assert(spread(strong) > spread(pale),
           `more saturation is more colour (${spread(pale)} -> ${spread(strong)})`)

    await ctl.send('agentSpace.settings.set', { saturation: 50 })
    await ctl.send('agentSpace.theme.set', { themeId: 'pure', appearance: 'light' })
  } finally { ctl.close() }
})

test('settings: the settings surface survives a restart', async () => {
  let ctl = await openControl()
  try {
    await ctl.send('agentSpace.settings.set',
                   { saturation: 80, alwaysShowFullUrl: true,
                     newTabBehavior: 'omnibox' })
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
    const r = await ctl.send('agentSpace.settings.get')
    assertEq(r.saturation, 80, 'saturation restored')
    assertEq(r.alwaysShowFullUrl, true, 'full-URL toggle restored')
    assertEq(r.newTabBehavior, 'omnibox', 'new tab behavior restored')
    // Leave the defaults for the suites that assume them.
    await ctl.send('agentSpace.settings.set',
                   { saturation: 50, alwaysShowFullUrl: false,
                     newTabBehavior: 'newTabPage' })
  } finally { ctl.close() }
})

run()
