#!/usr/bin/env node
// Copyright 2026 Phinomenon Inc.
//
// Self-test for the phi-browser skill. Run after changing the skill:
//   node scripts/selftest.mjs
//
// Needs a running Phi Browser with the CDP endpoint enabled (see
// references/install.md) and at least one browser window open. Drives a
// throwaway hidden agent Space named 'phi-skill-selftest' against a local
// HTTP server — no external sites, safe to run while the user browses.
// Takes ~60s; exits non-zero when any check fails.

import { spawnSync } from 'node:child_process'
import { createServer } from 'node:http'
import { fileURLToPath } from 'node:url'
import * as H from './lib/helpers.mjs'

const PORT = 8379
const BASE = `http://127.0.0.1:${PORT}`
const SPACE = 'phi-skill-selftest'

// The SPA page is deliberately hostile: controls mount late (resolution
// retries), the list keeps streaming while a click glides (moving-target
// re-measure), and readiness signals arrive on a delay (waitForFunction).
const SPA = `<!doctype html><title>spa</title>
<a href="${BASE}/fast">a link</a><ul id="list"></ul><script>
let n = 0
const t = setInterval(() => {
  const li = document.createElement('li'); li.className = 'item'; li.textContent = 'item ' + (++n)
  document.getElementById('list').appendChild(li)
  if (n >= 20) clearInterval(t)
}, 250)
setTimeout(() => {
  const b = document.createElement('button'); b.id = 'late'; b.textContent = 'Late'
  b.onclick = () => { window.clicked = true }
  document.body.appendChild(b)
}, 3000)
setTimeout(() => {
  const i = document.createElement('input'); i.id = 'latefield'
  document.body.appendChild(i)
}, 4000)
setTimeout(() => { window.flag = true }, 5000)
</` + `script>`

function startServer() {
  return new Promise((resolve, reject) => {
    const srv = createServer((req, res) => {
      res.setHeader('content-type', 'text/html')
      const page = (name) => `<!doctype html><title>${name}</title><h1>${name}</h1>`
      if (req.url.startsWith('/slow')) {
        // Delay the HEADERS so Page.navigate itself blocks — the goto-budget
        // tests need a navigation that cannot commit quickly.
        setTimeout(() => res.end(page('slow')), 8000)
      } else if (req.url.startsWith('/spa')) {
        res.end(SPA)
      } else {
        res.end(page('fast'))
      }
    })
    srv.once('error', (err) => reject(
      new Error(`selftest server failed on port ${PORT}: ${err.message}`)))
    srv.listen(PORT, '127.0.0.1', () => resolve(srv))
  })
}

const results = []
function check(name, ok, detail = '') {
  results.push({ name, ok: !!ok, detail })
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? `  (${detail})` : ''}`)
}

async function main() {
  // --- runner: heredoc scripts get require() ---------------------------------
  const runner = fileURLToPath(new URL('./runner.mjs', import.meta.url))
  const r = spawnSync(process.execPath, [runner], {
    input: "cliLog(typeof require('node:fs').readFileSync)",
    encoding: 'utf8', timeout: 30000,
  })
  check('runner provides require in heredoc', r.stdout.includes('function'),
        (r.stdout + r.stderr).trim().slice(0, 60))

  // --- concurrent openTab: no hang, no duplicate/lost tabs -------------------
  const first = await H.ensureAgentSpace(SPACE)
  let s = Date.now()
  const urls = Array.from({ length: 12 }, (_, i) => `${BASE}/fast?lane=${i + 1}`)
  const opened = await Promise.all(urls.map((u) =>
    H.openTab(u).catch((e) => ({ error: String(e.message || e) }))))
  const errors = opened.filter((o) => o.error)
  const distinct = new Set(opened.filter((o) => o.targetId).map((o) => o.targetId))
  check('concurrent openTab x12: no errors', errors.length === 0,
        errors.map((e) => e.error).join('; ').slice(0, 80))
  check('concurrent openTab x12: distinct tabs', distinct.size === 12,
        `${distinct.size}/12 in ${Date.now() - s}ms`)
  check('concurrent openTab x12: reused the blank seed once',
        opened.filter((o) => o.reused).length <= 1)

  // --- zombie heal: window death must not strand the task record -------------
  for (const t of await H.listTabs()) await H.closeTab(t.targetId)
  await H.wait(1)
  const healed = await H.ensureAgentSpace(SPACE)
  check('zombie Space heals on re-ensure', healed.spaceId !== first.spaceId,
        `spaceId ${first.spaceId} -> ${healed.spaceId}`)
  const afterHeal = await H.openTab(`${BASE}/fast?afterheal`)
  check('openTab works after heal', !!afterHeal.targetId)

  // --- goto: budget honored, degrade instead of hang -------------------------
  s = Date.now()
  const fast = await H.goto(`${BASE}/fast?goto`, { timeout: 10 })
  check('goto fast page', fast.title === 'fast', `${Date.now() - s}ms`)
  s = Date.now()
  let threw = null
  try { await H.goto(`${BASE}/slow`, { timeout: 4 }) } catch (e) { threw = e }
  const tightMs = Date.now() - s
  check('goto over-budget navigation throws promptly',
        threw && /timed out/i.test(threw.message) && tightMs < 7000,
        `${tightMs}ms: ${String(threw?.message).slice(0, 50)}`)
  s = Date.now()
  const slow = await H.goto(`${BASE}/slow?roomy`, { timeout: 15 })
  check('goto slow page within budget', slow.title === 'slow', `${Date.now() - s}ms`)
  s = Date.now()
  const wfl = await H.waitForLoad({ timeout: 5 })
  check('waitForLoad instant on loaded page', !!wfl.ready && Date.now() - s < 1500)

  // --- SPA races: late mounts, moving targets, generic waits -----------------
  await H.goto(`${BASE}/spa`)
  s = Date.now()
  await H.click('#late')
  const clicked = await H.js('window.clicked === true')
  check('click resolves a late-mounted button onto its final position', clicked,
        `${Date.now() - s}ms`)
  s = Date.now()
  await H.fillInput('#latefield', 'hello')
  check('fillInput resolves a late-mounted field',
        (await H.js('document.getElementById("latefield").value')) === 'hello',
        `${Date.now() - s}ms`)
  s = Date.now()
  check('waitForFunction waits for a delayed condition',
        (await H.waitForFunction('window.flag === true', { timeout: 10 })) === true,
        `${Date.now() - s}ms`)
  s = Date.now()
  const many = await H.waitForElement('li.item', { minCount: 15, timeout: 10 })
  check('waitForElement minCount', many.found && many.count >= 15,
        `count ${many.count} in ${Date.now() - s}ms`)
  s = Date.now()
  threw = null
  try { await H.click('#nope') } catch (e) { threw = e }
  const missMs = Date.now() - s
  check('missing target fails after the bounded grace',
        threw && missMs >= 2500 && missMs < 6000, `${missMs}ms`)
  threw = null
  try { await H.waitForFunction('nonsense..syntax', { timeout: 1 }) } catch (e) { threw = e }
  check('waitForFunction surfaces evaluation errors on timeout',
        threw && /SyntaxError/.test(threw.message))

  // --- observe/snapshot share one scan ---------------------------------------
  const obs = await H.observe()
  check('observe sees the SPA controls', (obs.elements || []).length >= 3,
        `${(obs.elements || []).length} elements`)
  check('snapshotText tags refs', /\[ref=\d+/.test(await H.snapshotText()))

  // --- importCookies round-trip (local domain only) --------------------------
  const imp = await H.importCookies([{
    name: '__phi_selftest', value: 'ok', domain: '127.0.0.1',
    expirationDate: Math.floor(Date.now() / 1000) + 120,
  }])
  await H.goto(`${BASE}/fast?cookie`)
  const seen = await H.js('document.cookie')
  await H.cdp('Network.deleteCookies', { name: '__phi_selftest', domain: '127.0.0.1' })
  check('importCookies injects into the profile',
        imp.imported === 1 && seen.includes('__phi_selftest=ok'))
  const leftover = (await H.cdp('Storage.getCookies', {})).cookies
    .filter((c) => c.name === '__phi_selftest').length
  check('selftest cookie cleaned up', leftover === 0)

  await H.complete({ success: true })
}

const server = await startServer()
let fatal = null
try {
  await main()
} catch (err) {
  fatal = err
} finally {
  server.close()
  // Never leave the throwaway Space behind, even on a mid-test crash.
  if (fatal) {
    try { await H.ensureAgentSpace(SPACE); await H.complete({ success: false }) } catch {}
  }
  await H.__dispose().catch(() => {})
}

const failed = results.filter((r) => !r.ok)
console.log(`\n${results.length - failed.length}/${results.length} checks passed` +
            (fatal ? ` — aborted by: ${fatal.message}` : ''))
process.exit(failed.length || fatal ? 1 : 0)
