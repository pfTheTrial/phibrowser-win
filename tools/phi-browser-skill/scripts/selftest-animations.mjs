#!/usr/bin/env node
// Copyright 2026 Phinomenon Inc.
//
// INTERACTIVE self-test for the control-handoff round trip and the input
// animations a watching user sees. Run it yourself — it needs a human:
//   node scripts/selftest-animations.mjs
//
// Flow: opens a demo agent Space against a local HTTP server, hands control
// to you (Phi shows the native handoff prompt), and the moment you click
// "Hand back" it performs every input primitive as a watchable choreography —
// mouse movement across hover targets, clicks, a double-click, a raw
// coordinate click, paced typing (fillInput + typeText + Enter), and
// scrolling — while the demo page counts what actually landed. Each phase is
// verified programmatically, so it doubles as a functional check of
// hover/click/fillInput/typeText/pressKey/scroll running AFTER a handoff.
//
// Needs a running Phi Browser with the CDP endpoint enabled (see
// references/install.md). If you never hand back (15 min) the throwaway
// Space is cleaned up and the test exits non-zero; taking control mid-demo
// leaves the Space with you (honoring the handoff rules). ~2 min.

import { createServer } from 'node:http'
import * as H from './lib/helpers.mjs'

const PORT = 8380
const BASE = `http://127.0.0.1:${PORT}`
const SPACE = 'phi-skill-animation-demo'
const HANDBACK_TIMEOUT = 900 // seconds a human gets to click "Hand back"

const NAME_TEXT = 'Hello from Phi’s agent'
const CHAT_TEXT = 'Watch me type, then press Enter'

// The demo page: big, high-contrast targets (they must read on the scaled
// watch view), a fixed HUD counting every event, and a striped runway tall
// enough to make scrolling obvious. Page script records what landed into
// window.stats for the programmatic checks.
const PAGE = `<!doctype html><meta charset="utf-8"><title>Phi input animation demo</title>
<style>
  body { font: 15px/1.5 -apple-system, sans-serif; margin: 0; color: #1d2430; background: #f6f8fb }
  main { max-width: 860px; margin: 0 auto; padding: 28px 32px 60px }
  h1 { font-size: 22px; margin: 0 0 4px }
  p.sub { color: #5a6474; margin: 0 0 24px }
  section { background: #fff; border: 1px solid #e3e8f0; border-radius: 12px; padding: 18px 20px; margin-bottom: 16px }
  section h2 { font-size: 13px; text-transform: uppercase; letter-spacing: .06em; color: #7a8497; margin: 0 0 12px }
  .dots { display: flex; gap: 28px }
  .dot { width: 40px; height: 40px; border-radius: 50%; background: #e3e8f0; border: 2px solid #c9d2e0; transition: transform .15s, background .15s }
  .dot.hit { background: #4f7cff; border-color: #4f7cff; transform: scale(1.25) }
  .row { display: flex; gap: 16px; align-items: center; flex-wrap: wrap }
  button { font: inherit; padding: 12px 22px; border-radius: 10px; border: 1px solid #c9d2e0; background: #fff; cursor: pointer }
  button:active { transform: scale(.96) }
  #btn-click { background: #4f7cff; border-color: #4f7cff; color: #fff }
  input { font: inherit; padding: 10px 12px; border: 1px solid #c9d2e0; border-radius: 10px; width: 300px }
  #log { margin: 12px 0 0; padding-left: 20px }
  #log li { color: #2c7a3f }
  #hud { position: fixed; top: 14px; right: 14px; background: #10131a; color: #e8ecf4; font: 12px/1.7 ui-monospace, monospace; padding: 10px 14px; border-radius: 10px; pointer-events: none }
  .stripe { height: 300px; display: flex; align-items: center; justify-content: center; color: #98a2b5; font-size: 20px; border-top: 1px dashed #d4dbe6 }
</style>
<main>
  <h1>Phi agent — input animation demo</h1>
  <p class="sub">Hand control back and keep watching: every cursor glide, click ripple, keystroke, and scroll below is the agent.</p>
  <section><h2>1 · Mouse movement</h2>
    <div class="dots"><div class="dot" id="d1"></div><div class="dot" id="d2"></div><div class="dot" id="d3"></div><div class="dot" id="d4"></div><div class="dot" id="d5"></div></div>
  </section>
  <section><h2>2 · Clicks</h2>
    <div class="row">
      <button id="btn-click" type="button">Click me · <span id="nclick">0</span></button>
      <button id="btn-double" type="button">Double-click me · <span id="ndbl">0</span></button>
      <button id="btn-coord" type="button">Coordinate target · <span id="ncoord">0</span></button>
    </div>
  </section>
  <section><h2>3 · Typing</h2>
    <div class="row">
      <input id="name" placeholder="Name field (fillInput)">
      <input id="chat" placeholder="Message (typeText + Enter)">
    </div>
    <ul id="log"></ul>
  </section>
  <section><h2>4 · Scroll runway</h2><div id="runway"></div></section>
</main>
<div id="hud"></div>
<script>
var stats = window.stats = { hovered: [], clicks: 0, dbl: 0, coord: 0, submitted: [], maxScroll: 0 }
function upd () {
  document.getElementById('nclick').textContent = stats.clicks
  document.getElementById('ndbl').textContent = stats.dbl
  document.getElementById('ncoord').textContent = stats.coord
  document.getElementById('hud').textContent =
    'hover ' + stats.hovered.length + '/5 · click ' + stats.clicks +
    ' · dbl ' + stats.dbl + ' · coord ' + stats.coord +
    ' · sent ' + stats.submitted.length + ' · scroll ' + Math.round(window.scrollY) + 'px'
}
document.querySelectorAll('.dot').forEach(function (d) {
  d.addEventListener('mouseenter', function () {
    if (stats.hovered.indexOf(d.id) < 0) stats.hovered.push(d.id)
    d.classList.add('hit'); upd()
  })
})
document.getElementById('btn-click').addEventListener('click', function () { stats.clicks++; upd() })
document.getElementById('btn-double').addEventListener('dblclick', function () { stats.dbl++; upd() })
document.getElementById('btn-coord').addEventListener('click', function () { stats.coord++; upd() })
document.getElementById('chat').addEventListener('keydown', function (e) {
  if (e.key !== 'Enter' || !e.target.value) return
  stats.submitted.push(e.target.value)
  var li = document.createElement('li'); li.textContent = '✓ ' + e.target.value
  document.getElementById('log').appendChild(li)
  e.target.value = ''; upd()
})
addEventListener('scroll', function () {
  if (window.scrollY > stats.maxScroll) stats.maxScroll = window.scrollY
  upd()
}, { passive: true })
var runway = document.getElementById('runway')
for (var i = 1; i <= 8; i++) {
  var s = document.createElement('div'); s.className = 'stripe'
  s.textContent = 'runway · ' + (i * 300) + 'px'
  runway.appendChild(s)
}
upd()
</` + `script>`

function startServer() {
  return new Promise((resolve, reject) => {
    const srv = createServer((req, res) => {
      res.setHeader('content-type', 'text/html')
      res.end(PAGE)
    })
    srv.once('error', (err) => reject(
      new Error(`demo server failed on port ${PORT}: ${err.message}`)))
    srv.listen(PORT, '127.0.0.1', () => resolve(srv))
  })
}

const results = []
function check(name, ok, detail = '') {
  results.push({ name, ok: !!ok, detail })
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `  (${detail})` : ''}`)
}

async function main() {
  await H.ensureAgentSpace(SPACE)
  await H.openTab(`${BASE}/`)
  check('agent owns the fresh space', (await H.ownership()) === 'agent')

  console.log('\nIn Phi: the handoff prompt is up — jump into the demo Space,')
  console.log('look around, then click "Hand back" and KEEP WATCHING the Space.')
  console.log(`Waiting up to ${HANDBACK_TIMEOUT / 60} minutes for the hand-back…\n`)
  await H.handOff('Animation demo ready. Look around, then click "Hand back" '
    + 'and keep watching — I’ll drive every input so you can see the animations.')
  check('handOff gives the user control', (await H.ownership()) === 'user')

  let back
  try {
    back = await H.waitForAgentControl({ timeout: HANDBACK_TIMEOUT })
  } catch (err) {
    check('user handed control back', false, String(err.message))
    // Nobody came: reclaim the throwaway Space instead of leaving a zombie.
    await H.complete({ success: false }).catch(() => {})
    return
  }
  if (back.gone) {
    check('user handed control back', false,
          `space gone (${back.reason}) — the demo was ended from the browser`)
    return
  }
  check('user handed control back', back.owner === 'agent')

  // The user is presumed watching from here: narrate each phase and pace the
  // actions so the choreography is followable, not a blur.
  await H.narrate('Demo 1/4: moving the mouse')
  for (const d of ['#d1', '#d2', '#d3', '#d4', '#d5']) {
    await H.hover(d)
    await H.wait(0.35)
  }
  const hovered = await H.js('window.stats.hovered.length')
  check('mouse move: hover trail lit all 5 dots', hovered === 5, `${hovered}/5`)

  await H.narrate('Demo 2/4: clicking')
  for (let i = 0; i < 3; i++) {
    await H.click('#btn-click')
    await H.wait(0.4)
  }
  const clicks = await H.js('window.stats.clicks')
  check('click: 3 clicks registered', clicks === 3, `${clicks}/3`)

  await H.click('#btn-double', { clickCount: 2 })
  await H.wait(0.4)
  check('double-click synthesized', (await H.js('window.stats.dbl')) >= 1)

  const pt = await H.js('(() => { const r = document.getElementById("btn-coord")'
    + '.getBoundingClientRect(); return { x: r.x + r.width / 2, y: r.y + r.height / 2 } })()')
  await H.click(Math.round(pt.x), Math.round(pt.y))
  await H.wait(0.4)
  check('coordinate click lands on the button', (await H.js('window.stats.coord')) >= 1)

  await H.narrate('Demo 3/4: typing')
  await H.fillInput('#name', NAME_TEXT)
  check('fillInput typed the name',
        (await H.js('document.getElementById("name").value')) === NAME_TEXT)
  await H.click('#chat')
  await H.typeText(CHAT_TEXT)
  await H.pressKey('Enter')
  const submitted = await H.js('window.stats.submitted')
  check('typeText + Enter submitted the message',
        submitted.length === 1 && submitted[0] === CHAT_TEXT,
        JSON.stringify(submitted).slice(0, 60))

  await H.narrate('Demo 4/4: scrolling')
  for (let i = 0; i < 3; i++) {
    await H.scroll({ dy: 800 })
    await H.wait(0.5)
  }
  const deep = await H.js('window.scrollY')
  check('scroll travels down the runway', deep > 800, `scrollY ${Math.round(deep)}`)
  for (let i = 0; i < 3; i++) {
    await H.scroll({ dy: -900 })
    await H.wait(0.4)
  }
  check('scroll returns to the top', (await H.js('window.scrollY')) < 100)

  await H.narrate('Animation demo complete ✓')
  await H.wait(1.5) // let the last narration land before the Space closes
  await H.complete({ success: results.every((r) => r.ok),
                     message: 'animation demo finished' })
}

const server = await startServer()
let fatal = null
try {
  await main()
} catch (err) {
  fatal = err
} finally {
  server.close()
  if (fatal) {
    if (/user is controlling/i.test(String(fatal.message))) {
      // The user grabbed control mid-demo — that is their right per the
      // handoff rules: leave the Space with them, never yank it away.
      console.log('\nYou took control mid-demo — the Space stays with you; '
        + 'delete it from the Space switcher when done.')
    } else {
      try { await H.ensureAgentSpace(SPACE); await H.complete({ success: false }) } catch {}
    }
  }
  await H.__dispose().catch(() => {})
}

const failed = results.filter((r) => !r.ok)
console.log(`\n${results.length - failed.length}/${results.length} checks passed` +
            (fatal ? ` — aborted by: ${fatal.message}` : ''))
process.exit(failed.length || fatal ? 1 : 0)
