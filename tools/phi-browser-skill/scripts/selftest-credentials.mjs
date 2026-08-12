#!/usr/bin/env node
// Copyright 2026 Phinomenon Inc.
//
// Self-test for the credential fill surface (fillCredential + the shared
// fill core behind fillInput). Run after changing either:
//
//   node scripts/selftest-credentials.mjs               # tier 1 only
//   node scripts/selftest-credentials.mjs github.com    # + tier 2, end-to-end
//
// Tier 1 needs only a running Phi with the CDP endpoint enabled: argument
// validation and the shared fill machinery against a local page — no vault,
// no approval prompts.
//
// Tier 2 (with a vault-domain argument) exercises the real path: it fetches
// the item once with getCredential to learn the expected values, then checks
// that fillCredential's in-app dispatch lands exactly those values in the
// page and that its return carries no secret. Pass a domain that exists in
// the user's vault. Phi pops its approve prompt for each secret call —
// approve, and grant the 10-minute remember to be asked only once. Secrets
// never appear in this test's output (details print lengths and booleans,
// not values).

import { createServer } from 'node:http'
import * as H from './lib/helpers.mjs'

const PORT = 8381
const BASE = `http://127.0.0.1:${PORT}`
const SPACE = 'phi-skill-selftest-cred'
const DOMAIN = process.argv[2] || ''

const LOGIN = `<!doctype html><title>login</title>
<input id="user"><input id="pass" type="password">
<input id="pass2" type="password">
<select id="sel"><option value="optA">A</option><option value="optB">B</option></select>`

function startServer() {
  return new Promise((resolve, reject) => {
    const srv = createServer((req, res) => {
      res.setHeader('content-type', 'text/html')
      res.end(LOGIN)
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

const val = (id) => H.js(`document.getElementById(${JSON.stringify(id)}).value`)

async function tier1() {
  // --- shared fill core: fillInput must be unchanged by the refactor ---------
  await H.fillInput('#user', 'paced-value')
  check('fillInput paced path', await val('user') === 'paced-value')
  await H.fillInput('#pass', 'instant-value', { instant: true })
  check('fillInput instant path', await val('pass') === 'instant-value')
  await H.fillInput('#sel', 'optB')
  check('fillInput select match', await val('sel') === 'optB')

  let threw = null
  try { await H.fillInput('#nope', 'x') } catch (e) { threw = e }
  check('fillInput keeps its own error label',
        threw && threw.message.startsWith('fillInput: target not found'),
        String(threw?.message).slice(0, 60))
  threw = null
  try { await H.fillInput('#sel', 'zzz') } catch (e) { threw = e }
  check('fillInput select mismatch keeps its label',
        threw && threw.message.startsWith('fillInput: no option matched zzz'),
        String(threw?.message).slice(0, 60))

  // --- fillCredential argument validation: throws before any vault call, so
  // none of these pops an approval prompt -------------------------------------
  threw = null
  try { await H.fillCredential([5, 5], 'example.com') } catch (e) { threw = e }
  check('fillCredential rejects coordinates',
        threw && /element target/.test(threw.message))
  threw = null
  try { await H.fillCredential('#pass', 'example.com', { field: 'notes' }) } catch (e) { threw = e }
  check('fillCredential rejects an unknown field',
        threw && /field must be/.test(threw.message))
  threw = null
  try { await H.fillCredential('#pass', 42) } catch (e) { threw = e }
  check('fillCredential rejects a bad query',
        threw && /credential query/.test(threw.message))

  // --- origin binding: a domain query is checked against the page host
  // BEFORE the vault is asked, so this throws with no approval prompt ---------
  threw = null
  try { await H.fillCredential('#pass', 'example.com') } catch (e) { threw = e }
  check('fillCredential is origin-bound (page 127.0.0.1 vs example.com)',
        threw && /origin_mismatch/.test(threw.message),
        String(threw?.message).slice(0, 70))

  // --- runWithCredential argument validation (also pre-vault) ----------------
  threw = null
  try { await H.runWithCredential('example.com', [], { envAll: true }) } catch (e) { threw = e }
  check('runWithCredential rejects an empty command',
        threw && /non-empty array/.test(threw.message))
  threw = null
  try {
    await H.runWithCredential('example.com', ['true'], { env: { X: 'bogus' } })
  } catch (e) { threw = e }
  check('runWithCredential rejects an unknown field',
        threw && /unknown field 'bogus'/.test(threw.message))
  threw = null
  try { await H.runWithCredential('example.com', ['true']) } catch (e) { threw = e }
  check('runWithCredential requires an env mapping',
        threw && /env mapping or \{envAll/.test(threw.message))
}

async function tier2() {
  if (!DOMAIN) {
    console.log('\nSKIP  tier 2 (end-to-end vault fill) — pass a vault domain to run it:\n' +
                '      node scripts/selftest-credentials.mjs github.com')
    return
  }
  const status = await H.credentialStatus()
  if (!status.ready) {
    console.log(`\nSKIP  tier 2 — password manager not ready (${status.status}); ` +
                'unlock/sign in via Settings ▸ General ▸ Bitwarden and rerun')
    return
  }

  // Reference values. The test process is allowed to see them — the property
  // under test is that fillCredential's RESULTS and ERRORS never carry them.
  // When several vault items match the domain, Phi refuses with `ambiguous`
  // and candidate usernames instead of picking one; narrow to the first
  // candidate and continue with that query throughout.
  let q = DOMAIN
  let cred
  try {
    cred = await H.getCredential(q)
  } catch (err) {
    const msg = String(err?.message || '')
    if (/not_found/.test(msg)) {
      console.log(`\nSKIP  tier 2 — no ${DOMAIN} item in the vault; rerun with a ` +
                  'domain that exists there')
      return
    }
    const candidates = /vault items match/.test(msg) &&
      (msg.match(/Candidates: (.+?)(?: \(first|$)/) || [])[1]
    if (!candidates) throw err
    check('ambiguous lookup refuses with candidates, no secret', true,
          `${candidates.split(', ').length} candidates`)
    q = { domain: DOMAIN, username: candidates.split(', ')[0] }
    cred = await H.getCredential(q)
  }
  if (!cred.password) {
    console.log(`\nSKIP  tier 2 — the ${DOMAIN} vault item has no password`)
    return
  }
  check('served credential is the unique match', cred.matches === 1,
        `matches: ${cred.matches}`)
  if (cred.username) {
    const narrowed = await H.getCredential({ domain: DOMAIN, username: cred.username },
                                           { fields: ['username'] })
    check('username-narrowed query finds the account',
          narrowed.username === cred.username && narrowed.matches === 1,
          `matches: ${narrowed.matches}`)
  }
  // Clear tier-1 leftovers so a stale value cannot fake a pass.
  await H.js('["user","pass","pass2"].forEach((i) => document.getElementById(i).value = "")')

  // The test page is on 127.0.0.1 while the credential belongs to DOMAIN, so
  // every fill here exercises the explicit cross-origin opt-out — Phi prompts
  // under the destination-qualified scope ("DOMAIN → 127.0.0.1"); grant the
  // 10-minute remember on the first prompt to be asked only once.
  const xo = { allowCrossOrigin: true }
  const ret = await H.fillCredential('#pass', q, xo)
  const landed = await val('pass')
  check('fill password lands the vault value', landed === cred.password,
        `len ${String(landed).length} vs ${cred.password.length}`)
  check('return carries no secret',
        ret.filled === true && ret.field === 'password' &&
        Object.keys(ret).every((k) => ['filled', 'field', 'matches'].includes(k)) &&
        !JSON.stringify(ret).includes(cred.password),
        `keys: ${Object.keys(ret).join(',')}`)

  await H.fillCredential('#pass2', q, xo)
  check('second fill lands the vault value', await val('pass2') === cred.password)

  if (cred.username) {
    await H.fillCredential('#user', q, { field: 'username', ...xo })
    check('fill username lands the vault value', await val('user') === cred.username)
  } else {
    console.log(`SKIP  username fill — the ${DOMAIN} vault item has no username`)
  }

  // A non-INPUT target is refused at tag time, before any vault or approval
  // work — the secret never exists in this process for a fill, so there is
  // nothing an error message could leak (the old runner-side fill needed a
  // scrub here; the in-app fill makes it structural).
  let threw = null
  try { await H.fillCredential('#sel', q, xo) } catch (e) { threw = e }
  const msg = String(threw?.message || '')
  check('non-input target refused before the vault is touched',
        threw && /not an <input> \(select\)/.test(msg) && !msg.includes(cred.password))

  // Readback protection: a routine re-scan after the fill must mask the
  // password field's contents. Checked on the field itself — the password may
  // legitimately collide with visible text elsewhere (e.g. equal the
  // username), and that case is covered by the cliLog scrub below.
  const obs = await H.observe()
  const passEl = (obs.elements || []).find((e) => e.loc === 'css:#pass')
  check('page scan masks password input values',
        passEl != null && passEl.value === '•••',
        `value: ${passEl ? passEl.value === cred.password ? 'LEAKED' : passEl.value : 'field missing'}`)

  // runWithCredential: env injection lands (username is not secret, safe to
  // echo and compare) and secret values are scrubbed from captured output.
  if (cred.username) {
    const run = await H.runWithCredential(q,
      ['node', '-e', 'console.log("U=" + process.env.T_USER)'],
      { env: { T_USER: 'username' } })
    check('runWithCredential injects an env var',
          run.code === 0 && run.stdout.trim() === `U=${cred.username}`,
          `code ${run.code}`)
  }
  const leak = await H.runWithCredential(q,
    ['node', '-e', 'console.log("PW=" + process.env.T_PASS)'],
    { env: { T_PASS: 'password' } })
  check('runWithCredential scrubs secrets from output',
        leak.code === 0 && leak.stdout.includes('PW=•••') &&
        !leak.stdout.includes(cred.password))

  // cliLog — the one channel into the agent's context — must scrub a handled
  // secret even when page JS read it back directly. Runs after
  // runWithCredential, which is what registers the value now: an in-app fill
  // never hands this process the secret, so it has nothing to register.
  let captured = ''
  const origLog = console.log
  console.log = (s) => { captured = String(s) }
  try { H.cliLog('readback: ' + cred.password) } finally { console.log = origLog }
  check('cliLog scrubs handled secrets',
        captured.includes('•••') && !captured.includes(cred.password))
}

async function main() {
  await H.ensureAgentSpace(SPACE)
  await H.openTab(`${BASE}/login`)
  await tier1()
  await tier2()
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
