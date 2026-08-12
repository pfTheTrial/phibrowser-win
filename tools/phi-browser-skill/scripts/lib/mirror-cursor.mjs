// Copyright 2026 Phinomenon Inc.
//
// Cursor adapter for the session mirror: locates the driving Cursor IDE
// agent session's transcript and parses its lines into console lines.
// Cursor's in-editor agent (the extension host runs skill heredocs through
// its Shell tool) writes each session as Anthropic-style JSONL under
// ~/.cursor/projects/<project>/agent-transcripts/<id>/<id>.jsonl —
// user/assistant message lines plus turn_ended markers. Tool RESULTS are
// never recorded there (they stream to the project's terminals/*.txt), so
// the task id the other evidence heuristics look for cannot appear. What
// IS recorded — before the shell even runs — is the Shell tool_use whose
// command is this very heredoc. Discovery therefore matches the strongest
// evidence available: the heredoc source this process is executing,
// JSON-escaped, in a freshly written transcript. Fallback when that text
// isn't found (a truncated recording, a script piped from a file): the
// SINGLE fresh transcript that mentions the skill's runner at all — two
// such candidates is a guess, and the mirror never guesses.
//
// Console→session delivery: none exists. Cursor's IDE agent has no ingress
// an outside process may use — its deeplinks only open NEW chats, its
// cursor-app-control MCP has no message tool, the cursor-agent CLI keeps a
// separate session store the IDE only bulk-imports from, and in-IDE chat
// submission sits behind Anysphere-only proposed extension APIs. Console
// commands therefore stay queued until the driver's next round drains them
// via readUserMessages() — the Claude Code/Codex semantics — and, because
// a Cursor turn that already ended may never run another round unprompted,
// CursorConsoleNotice tells the user so in the console instead of letting
// the command disappear into a silent queue.

import { readdirSync, statSync, openSync, readSync, closeSync } from 'node:fs'
import { homedir } from 'node:os'
import { join } from 'node:path'
import { forwardEntries } from './mirror-core.mjs'

// A transcript not written to for this long belongs to an ended session.
const SESSION_MAX_AGE_MS = 30 * 60 * 1000
// Evidence window: the recorded Shell tool_use sits within the newest bytes.
const EVIDENCE_TAIL_BYTES = 256 * 1024
// Enough of the heredoc head to be this round's script and no other, kept
// short so a recorder that truncates long tool arguments still matches.
const SOURCE_NEEDLE_CHARS = 200

/**
 * The driving Cursor session's transcript, or null. `heredocSource` is the
 * script text the current runner round executes (see runner.mjs) — the
 * exact evidence, since Cursor recorded it as the spawning Shell command.
 * Returns {sessionKey, path, format:'cursor'} — sessionKey is the
 * transcript's session id (its basename).
 */
export function discoverCursorTranscript(heredocSource) {
  if (!underCursor()) return null
  const candidates = []
  for (const dir of transcriptDirs()) {
    let files = []
    try { files = readdirSync(dir) } catch { continue }
    for (const file of files) {
      if (!file.endsWith('.jsonl')) continue
      const path = join(dir, file)
      let mtimeMs
      try { mtimeMs = statSync(path).mtimeMs } catch { continue }
      if (Date.now() - mtimeMs > SESSION_MAX_AGE_MS) continue
      candidates.push({
        path, sessionId: file.slice(0, -'.jsonl'.length), mtimeMs,
        tail: tailText(path),
      })
    }
  }
  const hit = (c) => ({ sessionKey: c.sessionId, path: c.path, format: 'cursor' })
  const source = String(heredocSource || '').slice(0, SOURCE_NEEDLE_CHARS)
  if (source.trim()) {
    const needle = jsonEscaped(source)
    const exact = candidates.filter((c) => c.tail.includes(needle))
      .sort((a, b) => b.mtimeMs - a.mtimeMs)
    if (exact.length) return hit(exact[0])
  }
  const marked = candidates.filter(
    (c) => c.tail.includes('phi-browser') && c.tail.includes('runner.mjs'))
  return marked.length === 1 ? hit(marked[0]) : null
}

/**
 * One transcript line → a console line, or null to skip. User and
 * assistant lines carry Anthropic-style content blocks; text blocks are
 * the conversation (tool_use and thinking are machinery), and the user's
 * words sit inside Cursor's prompt envelope. turn_ended markers and lines
 * with no text are skipped. Cursor records no per-line timestamps, so
 * entries carry none (the backfill filter then treats them as current —
 * matching the file-level freshness discovery already enforced).
 */
export function toEntry(obj) {
  if (!obj || (obj.role !== 'user' && obj.role !== 'assistant')) return null
  const content = obj.message && obj.message.content
  const text = typeof content === 'string' ? content
    : Array.isArray(content)
      ? content.filter((b) => b && b.type === 'text' && typeof b.text === 'string')
        .map((b) => b.text).join('\n\n')
      : ''
  if (!text.trim()) return null
  if (obj.role === 'user') {
    const query = userQueryText(text)
    if (!query || !query.trim()) return null
    // A "[phi-console]"-marked line is a console command a future transport
    // delivered into the session; the app already echoed it at enqueue time.
    if (query.trimStart().startsWith('[phi-console]')) return null
    return { kind: 'user', text: query }
  }
  return { kind: 'assistant', text }
}

// Envelope tags Cursor wraps around a prompt besides <user_query>: inlined
// skills, attached files, freshed rules. Their presence WITHOUT a query
// marks a machinery line; they never veto a plain-text prompt.
const ENVELOPE_TAGS = /<\/(manually_attached_skills|attached_files|additional_data|custom_instructions|rules)>/

/**
 * The user's words inside Cursor's prompt envelope: the <user_query>
 * block(s) when present, the raw text when the line is plain, null when
 * the line is all envelope and no query.
 */
function userQueryText(text) {
  const queries = [...text.matchAll(/<user_query>\s*([\s\S]*?)\s*<\/user_query>/g)]
    .map((m) => m[1]).filter((q) => q && q.trim())
  if (queries.length) return queries.join('\n\n')
  return ENVELOPE_TAGS.test(text) ? null : text
}

/**
 * The console-command surface for Cursor, in the tailer's bridge slot but
 * deliberately NOT a delivery bridge: it never drains the queue (draining
 * belongs to the agent's next readUserMessages round — see the header). It
 * answers each newly queued command with one console notice saying how the
 * command will reach the agent, so typing at an ended Cursor turn gets an
 * honest reply instead of silence. Same contract as ConsoleCommandBridge:
 * returns true when the task is gone and the daemon should exit.
 */
export class CursorConsoleNotice {
  constructor() { this.noticed = 0 }

  async deliverPending(ctl, channel) {
    if (!ctl.taskId) return false
    const { tasks } = await channel.send('agentSpace.list', {})
    const task = (tasks || []).find((t) => t.taskId === ctl.taskId)
    if (!task) return true
    const queued = task.pendingUserMessages || 0
    if (queued < this.noticed) this.noticed = queued  // a round drained them
    // While a browser round is live it drains the queue itself — only an
    // idle task leaves the user wondering where their command went.
    if (queued > this.noticed && task.status === 'idle') {
      await forwardEntries(ctl.taskId, [{
        kind: 'error',
        text: 'Cursor cannot be woken from outside — your command is queued'
          + ' for its next round',
        detail: 'It is read the next time the agent runs a phi-browser step.'
          + ' If Cursor has finished its turn, send it any message in Cursor'
          + ' to make it check the queue.',
      }], channel)
      this.noticed = queued
    }
    return false
  }
}

// --- gate / filesystem helpers ------------------------------------------------

// Cursor's shells carry its agent/terminal markers; the GUI bundle id
// (inherited by every process the app spawns) is the backstop — it is
// Cursor's fixed ToDesktop app id, not a name that could drift.
function underCursor() {
  if (process.env.CURSOR_AGENT || process.env.CURSOR_TRACE_ID) return true
  return (process.env.__CFBundleIdentifier || '') === 'com.todesktop.230313mzl4w4u92'
}

function stateDir() {
  return process.env.PHI_CURSOR_STATE_DIR || join(homedir(), '.cursor')
}

/** Every project's agent-transcript session directory under the state dir. */
function transcriptDirs() {
  const out = []
  const projects = join(stateDir(), 'projects')
  let dirs = []
  try { dirs = readdirSync(projects) } catch { return out }
  for (const project of dirs) {
    const transcripts = join(projects, project, 'agent-transcripts')
    let sessions = []
    try { sessions = readdirSync(transcripts) } catch { continue }
    for (const session of sessions) out.push(join(transcripts, session))
  }
  return out
}

/** The newest EVIDENCE_TAIL_BYTES of the file as text ('' on any error). */
function tailText(path) {
  try {
    const size = statSync(path).size
    const start = Math.max(0, size - EVIDENCE_TAIL_BYTES)
    const fd = openSync(path, 'r')
    try {
      const buf = Buffer.alloc(size - start)
      readSync(fd, buf, 0, buf.length, start)
      return buf.toString('utf8')
    } finally {
      closeSync(fd)
    }
  } catch { return '' }
}

/** Text as it appears inside the transcript's JSON-encoded command string. */
function jsonEscaped(value) {
  return JSON.stringify(String(value)).slice(1, -1)
}
