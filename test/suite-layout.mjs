// Layout suite: the three chrome layouts, named and behaving as on Mac
// (PhiPreferences.swift LayoutMode).
//   performance  vertical tabs, address bar in the sidebar   (the default)
//   balanced     vertical tabs, address bar above the content
//   comfortable  horizontal tabs, no sidebar
import { execFileSync } from 'node:child_process'
import { test, run, assert, assertEq, sleep, openControl } from './phi-test.mjs'

const SIDEBAR = 240  // Mirrors kSidebarWidth in phi_window_controller.cc.

async function firstWindow(ctl) {
  const { windows } = await ctl.send('agentSpace.list')
  return windows[0]
}

/** Sets the layout and returns the slot's content rect once it has applied. */
async function useLayout(ctl, windowId, mode) {
  const r = await ctl.send('agentSpace.layout.set', { mode })
  assertEq(r.ok, true, `layout.set ${mode} accepted`)
  assertEq(r.mode, mode, `layout.set echoes ${mode}`)
  await sleep(400)
  const l = await ctl.send('agentSpace.window.layout', { windowId })
  assertEq(l.ok, true, 'window.layout ok')
  assertEq(l.mode, mode, `window reports ${mode}`)
  return l.content
}

test('layout: defaults to performance, the Mac default', async () => {
  const ctl = await openControl()
  try {
    const r = await ctl.send('agentSpace.layout.get')
    assertEq(r.ok, true, 'layout.get ok')
    assert(['performance', 'balanced', 'comfortable'].includes(r.mode),
           `a known mode (${r.mode})`)
  } finally { ctl.close() }
})

test('layout: an unknown mode is rejected and changes nothing', async () => {
  const ctl = await openControl()
  try {
    const before = (await ctl.send('agentSpace.layout.get')).mode
    const r = await ctl.send('agentSpace.layout.set', { mode: 'cosy' })
    assertEq(r.ok, false, 'unknown mode rejected')
    assertEq(r.mode, before, 'mode unchanged')
  } finally { ctl.close() }
})

test('layout: performance keeps the sidebar and gives it the full height', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const content = await useLayout(ctl, windowId, 'performance')
    assertEq(content.x, SIDEBAR, 'content starts right of the sidebar')
    assertEq(content.y, 0, 'no band above the content')
    assert(content.width > 0 && content.height > 0, 'content is non-empty')
  } finally { ctl.close() }
})

test('layout: balanced keeps the sidebar and adds a bar above the content', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const perf = await useLayout(ctl, windowId, 'performance')
    const balanced = await useLayout(ctl, windowId, 'balanced')
    assertEq(balanced.x, SIDEBAR, 'sidebar still there')
    assert(balanced.y > 0, `address bar sits above the content (y=${balanced.y})`)
    assertEq(balanced.width, perf.width, 'same content width as performance')
    assert(balanced.height < perf.height,
           `content is shorter by the bar (${balanced.height} < ${perf.height})`)
  } finally { ctl.close() }
})

test('layout: comfortable drops the sidebar and stacks the chrome on top', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const perf = await useLayout(ctl, windowId, 'performance')
    const comfy = await useLayout(ctl, windowId, 'comfortable')
    assertEq(comfy.x, 0, 'content reaches the left edge — no sidebar')
    assert(comfy.y > 0, `chrome is stacked above (y=${comfy.y})`)
    assertEq(comfy.width, perf.width + SIDEBAR, 'content gains the sidebar width')
    assert(comfy.height < perf.height, 'content is shorter by the stacked rows')
  } finally { ctl.close() }
})

test('layout: every window follows the client-wide setting', async () => {
  const ctl = await openControl()
  try {
    const before = (await ctl.send('agentSpace.list')).windows
    await ctl.send('agentSpace.newWindow')
    let windows = before
    for (let i = 0; i < 24 && windows.length === before.length; i++) {
      await sleep(250)
      windows = (await ctl.send('agentSpace.list')).windows
    }
    const fresh = windows.find((w) => !before.includes(w))
    assert(fresh, 'second window created')
    try {
      await useLayout(ctl, before[0], 'comfortable')
      const other = await ctl.send('agentSpace.window.layout', { windowId: fresh })
      assertEq(other.mode, 'comfortable', 'the other window switched too')
      assertEq(other.content.x, 0, 'and re-laid out its content')
    } finally {
      await ctl.send('agentSpace.close', { windowId: fresh })
      await useLayout(ctl, before[0], 'performance')
    }
  } finally { ctl.close() }
})

test('layout: the sidebar really disappears in comfortable (pixels)', async () => {
  const ctl = await openControl()
  try {
    const windowId = await firstWindow(ctl)
    const h = await ctl.send('agentSpace.window.handle', { windowId })
    // A point deep in the sidebar band: painted blue while a sidebar exists,
    // and part of the (unpainted) content region once it is gone.
    const probe = () => samplePixel(h.ok ? h.hwnd : 0, 100, 400)

    await useLayout(ctl, windowId, 'performance')
    const withSidebar = probe()
    assert(isSidebarBlue(withSidebar), `sidebar paints there (${withSidebar})`)

    await useLayout(ctl, windowId, 'comfortable')
    const without = probe()
    assert(!isSidebarBlue(without), `sidebar is gone (${without})`)

    await useLayout(ctl, windowId, 'performance')
    assert(isSidebarBlue(probe()), 'and comes back')
  } finally { ctl.close() }
})

const isSidebarBlue = (s) => {
  const [r, g, b] = s.split(',').map(Number)
  return b > 200 && r < 160 && g < 190
}

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

run()
