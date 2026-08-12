// Copyright 2026 Phinomenon Inc.
//
// Hermes adapter for the session mirror: locates the driving Hermes session
// (NousResearch/hermes-agent) and parses its message rows into console
// lines. Hermes stores every session in one SQLite database —
// ~/.hermes/state.db, messages table — not per-session JSONL, so the tailer
// polls rows by autoincrement id through lib/mirror-sqlite. Everything
// downstream (control file, cursor, batched forward) is the shared core.
//
// Discovery is exact: Hermes exports HERMES_SESSION_ID into every tool
// subprocess (the strongest binding of all five agents — gate and session
// key in one).
//
// Console→session delivery goes through the hermes CLI's headless one-shot
// (`hermes --resume <session-id> -z <message>`): sessions live entirely in
// state.db, so resuming one in a scratch process appends the exchange to
// the SAME session — the mirror tails those rows straight back into the
// console. The interactive TUI (if still open on that session) won't show
// the exchange until its next reload; the transcript console, which is
// where the user is typing, always does.

import { existsSync } from 'node:fs'
import { homedir } from 'node:os'
import { join } from 'node:path'
import { queryRows, sqlString } from './mirror-sqlite.mjs'
import { ConsoleCommandBridge, deliverViaAgentCLI } from './mirror-core.mjs'

/**
 * The driving Hermes session's database, or null. Returns
 * {sessionKey, path, format:'hermes'} — sessionKey is the Hermes session id,
 * which is also the messages.session_id the tail polls.
 */
export function discoverHermesTranscript() {
  const sessionId = process.env.HERMES_SESSION_ID
  if (!sessionId) return null
  const db = join(hermesHome(), 'state.db')
  return existsSync(db) ? { sessionKey: sessionId, path: db, format: 'hermes' } : null
}

function hermesHome() {
  return process.env.PHI_HERMES_HOME || process.env.HERMES_HOME
    || join(homedir(), '.hermes')
}

/**
 * SQL for message rows after `since`, ascending. Role filtering happens
 * here (tool/system rows are machinery) and rewinds are honored via
 * active=1 — a rewound-away row simply stops existing for the mirror.
 */
export function hermesQuery(sessionId, since) {
  return `SELECT id, role, content, timestamp FROM messages`
    + ` WHERE session_id = ${sqlString(sessionId)} AND id > ${Number(since)}`
    + ` AND active = 1 AND role IN ('user','assistant')`
    + ` ORDER BY id LIMIT 200`
}

/** One messages row → the tail item (raw carries the row for toEntry). */
export function hermesRowToItem(row) {
  if (!row || typeof row.id !== 'number') return null
  return { index: row.id, raw: JSON.stringify(row) }
}

/** One messages row (as queried above) → a console line, or null to skip. */
export function toEntry(obj) {
  if (!obj || (obj.role !== 'user' && obj.role !== 'assistant')) return null
  const text = contentText(obj.content)
  if (!text) return null
  const ts = obj.timestamp ? Math.round(Number(obj.timestamp) * 1000) : undefined
  if (obj.role === 'user') {
    // A "[phi-console]"-marked line is a console command delivered into the
    // session; the app already echoed it at enqueue time.
    if (text.trimStart().startsWith('[phi-console]')) return null
    return { kind: 'user', text, ts }
  }
  return { kind: 'assistant', text, ts }
}

// How long deliverHermes waits for the CLI to fail before calling the
// command accepted: CLI errors (unknown session, bad config) exit within a
// moment; a healthy one-shot runs the woken turn to completion, far longer
// than the bridge can block.
const ACCEPT_WAIT_MS = 15 * 1000

/**
 * Sends a console command into the session by resuming it headless
 * (`hermes --resume <id> -z <text>`), with the shared accepted-or-rejected
 * semantics of deliverViaAgentCLI. HERMES_SESSION_ID is stripped from the
 * child's environment — this daemon inherited the driving session's export,
 * and the one-shot must bind to `--resume`, not to an env leak.
 */
export function deliverHermes(sessionId, text) {
  // `--resume` with an unknown id does NOT fail — it silently starts a NEW
  // session and burns the turn there. Guard with an existence check so a
  // stale session key surfaces as a delivery error, not a silent fork.
  if (!hermesSessionExists(sessionId)) {
    return Promise.reject(new Error(`hermes session ${sessionId} not found`))
  }
  const env = { ...process.env }
  delete env.HERMES_SESSION_ID
  return deliverViaAgentCLI(
    hermesBin(),
    ['--resume', String(sessionId), '-z', text],
    { acceptWaitMs: ACCEPT_WAIT_MS, env })
}

/** True when the session row exists — or the db is unreadable (a transient
 * lock must not block delivery; the CLI is then the arbiter). */
function hermesSessionExists(sessionId) {
  try {
    return queryRows(join(hermesHome(), 'state.db'),
      `SELECT 1 AS ok FROM sessions WHERE id = ${sqlString(sessionId)} LIMIT 1`
    ).length > 0
  } catch { return true }
}

/** The shared console bridge over the hermes resume-oneshot CLI. */
export class HermesConsoleBridge extends ConsoleCommandBridge {
  // `deliver` is a seam for fixture tests; the daemon uses the real CLI.
  constructor(sessionKey, { deliver = deliverHermes } = {}) {
    super(sessionKey, deliver)
  }
}

function hermesBin() {
  if (process.env.PHI_HERMES_BIN) return process.env.PHI_HERMES_BIN
  for (const p of [join(homedir(), '.local', 'bin', 'hermes'),
                   '/opt/homebrew/bin/hermes', '/usr/local/bin/hermes']) {
    if (existsSync(p)) return p
  }
  return 'hermes'  // PATH
}

// Hermes content is a plain string, or "\x00json:"-prefixed JSON when the
// message is multimodal — then keep the text blocks and drop the rest.
const JSON_PREFIX = '\x00json:'

function contentText(content) {
  if (typeof content !== 'string' || !content.trim()) return null
  if (!content.startsWith(JSON_PREFIX)) return content.trim() ? content : null
  try {
    const blocks = JSON.parse(content.slice(JSON_PREFIX.length))
    if (!Array.isArray(blocks)) return null
    const texts = blocks.filter((b) => b && b.type === 'text' && b.text)
      .map((b) => b.text)
    return texts.length ? texts.join('\n\n') : null
  } catch { return null }
}
