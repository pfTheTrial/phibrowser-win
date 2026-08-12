// Copyright 2026 Phinomenon Inc.
//
// Codex adapter for the session mirror: locates the driving Codex session's
// rollout transcript and parses its records into console lines. Everything
// downstream (control file, cursor, batched forward) is the shared core —
// this file only knows what is Codex-specific.
//
// Discovery, in order of confidence:
//   1. CODEX_THREAD_ID in the environment names the rollout file exactly
//      (rollout-<timestamp>-<threadId>.jsonl under ~/.codex/sessions).
//   2. Heuristic: among rollouts written in the last 30 minutes, prefer the
//      one whose session_meta cwd matches ours and whose tail mentions the
//      task id — the function_call that spawned this very heredoc (task name
//      included) is recorded in the rollout before the shell runs, so the
//      right file is both fresh and self-referential. No evidence → no
//      mirror (never guess: mirroring the WRONG session into the console is
//      worse than mirroring nothing).
// Both paths are gated on actually running under Codex (env markers or a
// codex ancestor process), so other agents never trigger rollout scans.

import { openSync, readSync, closeSync, readdirSync, statSync } from 'node:fs'
import { execFileSync } from 'node:child_process'
import { homedir } from 'node:os'
import { join, basename } from 'node:path'

// A rollout not written for this long belongs to an ended/parked session.
const ROLLOUT_MAX_AGE_MS = 30 * 60 * 1000

/**
 * One rollout JSONL record → a console line, or null to skip. Rollouts carry
 * each message TWICE — as a `response_item` (raw model item, where role:user
 * also covers system-injected texts) and as an `event_msg` (the display
 * stream, only real prompts and replies). Messages and reasoning come from
 * event_msg — one source, no duplicates, no machinery. Turn lifecycle also
 * comes from event_msg, but becomes transient console activity rather than a
 * stored transcript line. Tool calls have no event_msg counterpart in the
 * rollout, so those come from the function_call / custom_tool_call
 * response_items.
 */
export function toEntry(obj) {
  if (!obj || !obj.payload) return null
  const ts = Date.parse(obj.timestamp || '') || undefined
  if (obj.type === 'response_item') return toolCallEntry(obj.payload, ts)
  if (obj.type !== 'event_msg') return null
  const p = obj.payload
  if (p.type === 'task_started') return { kind: 'activity', text: 'working', ts }
  if (p.type === 'task_complete' || p.type === 'turn_aborted') {
    return { kind: 'activity', text: 'waiting', ts }
  }
  if (p.type === 'agent_reasoning') {
    return typeof p.text === 'string' && p.text.trim()
      ? { kind: 'thinking', text: p.text, ts } : null
  }
  if (p.type === 'collab_waiting_begin') return waitingBeginEntry(p, ts)
  if (p.type === 'collab_waiting_end') return waitingEndEntry(p, ts)
  if (typeof p.message !== 'string' || !p.message.trim()) return null
  if (p.type === 'user_message') {
    // A "[phi-console]"-marked line is a console command delivered into the
    // session; the app already echoed it at enqueue time.
    if (p.message.trimStart().startsWith('[phi-console]')) return null
    return { kind: 'user', text: p.message, ts }
  }
  if (p.type === 'agent_message') {
    return { kind: 'assistant', text: p.message, ts }
  }
  return null
}

// The phi heredocs that drive the browser are already narrated line-by-line
// by the skill's own action log; mirroring the call too would double every
// step.
const PHI_PLUMBING = /runner\.mjs|phi-browser/

/**
 * A function_call / custom_tool_call response_item → the console line the
 * Codex TUI shows for it ("Ran git status", "Updated Plan", "Edited
 * src/app.ts"), or null for machinery: harness pacing (`wait`) and the phi
 * heredocs themselves. Multi-agent waits are also skipped here because their
 * structured event_msg records render the richer Codex-style waiting cells.
 */
function toolCallEntry(p, ts) {
  if (!p || (p.type !== 'function_call' && p.type !== 'custom_tool_call')) return null
  const name = p.name || ''
  const raw = String((p.type === 'custom_tool_call' ? p.input : p.arguments) || '')
  if (name === 'wait' || /(?:^|[.:])wait_agent$/.test(name) || PHI_PLUMBING.test(raw)) {
    return null
  }
  let args = null
  try { args = JSON.parse(raw) } catch {}
  if (name === 'shell' || name === 'container.exec' || name === 'local_shell_call') {
    const cmd = shellCommand(args)
    return cmd ? tool(`Ran ${condense(cmd)}`, ts) : null
  }
  if (name === 'exec') {
    // The JS-repl harness wraps the real command in exec_command({cmd: "…"});
    // surface that command. Everything else the harness does with its cells
    // (write_stdin, wait, kill) is pacing plumbing, not a step to mirror.
    const m = /\bcmd:\s*"((?:[^"\\]|\\.)*)"/.exec(raw)
    if (!m) return null
    let cmd = m[1]
    try { cmd = JSON.parse(`"${m[1]}"`) } catch {}
    return tool(`Ran ${condense(cmd)}`, ts)
  }
  if (name === 'update_plan') {
    const steps = Array.isArray(args?.plan) ? args.plan : []
    const detail = steps
      .map((s) => `${s?.status === 'completed' ? '✔' : '○'} ${condense(s?.step, 60)}`)
      .join(' · ')
    return { ...tool('Updated Plan', ts), ...(detail ? { detail } : {}) }
  }
  if (name === 'apply_patch') {
    const patch = typeof args?.input === 'string' ? args.input : raw
    const files = [...patch.matchAll(/^\*\*\* (Add|Update|Delete) File: (.+)$/gm)]
    if (!files.length) return tool('Edited files', ts)
    const label = { Add: 'Added', Update: 'Edited', Delete: 'Deleted' }[files[0][1]]
    const more = files.length > 1 ? ` (+${files.length - 1} more)` : ''
    return tool(`${label} ${condense(files[0][2])}${more}`, ts)
  }
  return tool(`${name}(${condense(raw)})`, ts)
}

function tool(text, ts) {
  return { kind: 'tool', text, ts }
}

/** Codex's in-progress multi-agent wait cell. */
function waitingBeginEntry(p, ts) {
  const ids = stringArray(p.receiver_thread_ids)
  const agents = mergeAgentMetadata(ids, p.receiver_agents)
  let text = 'Waiting for agents'
  let detail
  if (agents.length === 1) {
    text = `Waiting for ${agentLabel(agents[0])}`
  } else if (agents.length > 1) {
    text = `Waiting for ${agents.length} agents`
    detail = agents.map(agentLabel).join('\n')
  }
  return { ...tool(text, ts), ...(detail ? { detail } : {}) }
}

/** Codex's completed multi-agent wait cell, including each final status. */
function waitingEndEntry(p, ts) {
  const agents = []
  const seen = new Set()
  for (const item of Array.isArray(p.agent_statuses) ? p.agent_statuses : []) {
    if (!item || typeof item !== 'object') continue
    const id = String(item.thread_id || item.agent_thread_id || '')
    if (id) seen.add(id)
    agents.push({
      id,
      nickname: item.agent_nickname,
      role: item.agent_role || item.agent_type,
      status: item.status,
      message: item.message,
    })
  }
  const statuses = p.statuses && typeof p.statuses === 'object' ? p.statuses : {}
  for (const id of Object.keys(statuses).sort()) {
    if (seen.has(id)) continue
    agents.push({ id, status: statuses[id] })
  }
  const detail = agents.length
    ? agents.map((agent) => `${agentLabel(agent)}: ${statusSummary(agent.status, agent.message)}`)
      .join('\n')
    : 'No agents completed yet'
  return { ...tool('Finished waiting', ts), detail }
}

/** Receiver ids in wait order, enriched by the event's optional metadata. */
function mergeAgentMetadata(ids, value) {
  const records = Array.isArray(value) ? value.filter((item) => item && typeof item === 'object') : []
  const byId = new Map(records.map((item) => [
    String(item.thread_id || item.agent_thread_id || ''), item,
  ]))
  const out = []
  const seen = new Set()
  for (const id of ids) {
    const item = byId.get(id) || {}
    out.push({ id, nickname: item.agent_nickname, role: item.agent_role || item.agent_type })
    seen.add(id)
  }
  for (const item of records) {
    const id = String(item.thread_id || item.agent_thread_id || '')
    if (id && seen.has(id)) continue
    out.push({ id, nickname: item.agent_nickname, role: item.agent_role || item.agent_type })
  }
  return out
}

function stringArray(value) {
  return Array.isArray(value) ? value.map(String).filter(Boolean) : []
}

/** Matches Codex's nickname [role] labels, falling back to the thread id. */
function agentLabel(agent) {
  const name = String(agent?.nickname || '').trim() || String(agent?.id || '').trim() || 'agent'
  const role = String(agent?.role || '').trim()
  return role ? `${name} [${role}]` : name
}

/** Legacy rollout AgentStatus or the newer normalized {status,message} shape. */
function statusSummary(status, message) {
  if (status && typeof status === 'object' && !Array.isArray(status)) {
    if ('status' in status) return statusSummary(status.status, status.message ?? message)
    if ('completed' in status) return statusWithMessage('Completed', status.completed)
    if ('errored' in status) return statusWithMessage('Error', status.errored || 'Agent errored')
  }
  switch (String(status || '').toLowerCase()) {
  case 'pending_init': return 'Pending init'
  case 'running': return 'Running'
  case 'interrupted': return 'Interrupted'
  case 'completed': return statusWithMessage('Completed', message)
  case 'errored':
  case 'error': return statusWithMessage('Error', message || 'Agent errored')
  case 'shutdown': return 'Shutdown'
  case 'not_found': return 'Not found'
  default: return status ? condense(status, 160) : 'Unknown'
  }
}

function statusWithMessage(label, message) {
  const preview = condense(message, label === 'Completed' ? 240 : 160)
  return preview ? `${label} - ${preview}` : label
}

/** The command of a shell-tool call, unwrapped from its `bash -lc` vector. */
function shellCommand(args) {
  const cmd = args?.command ?? args?.cmd
  if (typeof cmd === 'string') return cmd
  if (!Array.isArray(cmd)) return ''
  const parts = cmd.map(String)
  if (parts.length === 3 && /^(ba|z|da|)sh$/.test(basename(parts[0] || ''))
      && /^-l?c$/.test(parts[1])) {
    return parts[2]
  }
  return parts.join(' ')
}

/** First non-empty line, whitespace collapsed, capped for a title slot. */
function condense(value, max = 120) {
  const line = String(value ?? '').split('\n').map((l) => l.trim())
    .find((l) => l) || ''
  const s = line.replace(/\s+/g, ' ')
  return s.length > max ? `${s.slice(0, max)}…` : s
}

/**
 * The driving Codex session's rollout, or null. `agentPid` (the resolved
 * agent-root process) backs the under-Codex gate when env markers are
 * stripped. Returns {sessionKey, path, format:'codex'}.
 */
export function discoverCodexTranscript(taskId, agentPid) {
  if (!underCodex(agentPid)) return null
  const rollouts = recentRollouts(sessionsRoot())
  if (!rollouts.length) return null

  const threadId = process.env.CODEX_THREAD_ID
  if (threadId) {
    const exact = rollouts.find((r) => basename(r.path).includes(threadId))
    if (exact) return { sessionKey: threadId, path: exact.path, format: 'codex' }
  }

  const cwd = process.cwd()
  const scored = rollouts.map((r) => {
    let score = 0
    try {
      const meta = JSON.parse(firstLine(r.path) || 'null')
      if (meta?.payload?.cwd === cwd) score += 2
    } catch {}
    if (taskId && tailOf(r.path).includes(jsonEscaped(taskId))) score += 3
    return { ...r, score }
  }).sort((a, b) => b.score - a.score || b.mtime - a.mtime)
  const best = scored[0]
  if (!best || best.score === 0) return null
  return {
    sessionKey: threadIdFromName(best.path) || `codex-${Math.round(best.mtime)}`,
    path: best.path,
    format: 'codex',
  }
}

// --- gate / filesystem helpers ------------------------------------------------

function underCodex(agentPid) {
  if (process.env.CODEX_THREAD_ID || process.env.CODEX_SANDBOX) return true
  try {
    if (agentPid) {
      const comm = execFileSync('/bin/ps', ['-o', 'comm=', '-p', String(agentPid)],
                                { encoding: 'utf8' }).trim()
      return basename(comm).toLowerCase().startsWith('codex')
    }
  } catch {}
  return false
}

function sessionsRoot() {
  if (process.env.PHI_CODEX_SESSIONS_DIR) return process.env.PHI_CODEX_SESSIONS_DIR
  return join(process.env.CODEX_HOME || join(homedir(), '.codex'), 'sessions')
}

/** Fresh rollout files from today's and yesterday's date folders, newest first. */
function recentRollouts(root) {
  const out = []
  const now = Date.now()
  for (const daysAgo of [0, 1]) {
    const d = new Date(now - daysAgo * 86400000)
    const dir = join(root, String(d.getFullYear()),
                     String(d.getMonth() + 1).padStart(2, '0'),
                     String(d.getDate()).padStart(2, '0'))
    let files = []
    try { files = readdirSync(dir) } catch { continue }
    for (const file of files) {
      if (!file.startsWith('rollout-') || !file.endsWith('.jsonl')) continue
      try {
        const path = join(dir, file)
        const mtime = statSync(path).mtimeMs
        if (now - mtime <= ROLLOUT_MAX_AGE_MS) out.push({ path, mtime })
      } catch {}
    }
  }
  return out.sort((a, b) => b.mtime - a.mtime)
}

function firstLine(path) {
  const fd = openSync(path, 'r')
  try {
    const buf = Buffer.alloc(8192)
    const n = readSync(fd, buf, 0, buf.length, 0)
    return buf.toString('utf8', 0, n).split('\n')[0]
  } finally { closeSync(fd) }
}

/** The file's last 64KB — enough tail to find the record that spawned us. */
function tailOf(path) {
  try {
    const size = statSync(path).size
    const want = Math.min(size, 64 * 1024)
    const fd = openSync(path, 'r')
    try {
      const buf = Buffer.alloc(want)
      readSync(fd, buf, 0, want, size - want)
      return buf.toString('utf8')
    } finally { closeSync(fd) }
  } catch { return '' }
}

/** taskId as it appears inside the rollout's JSON-encoded command text. */
function jsonEscaped(value) {
  return JSON.stringify(String(value)).slice(1, -1)
}

function threadIdFromName(path) {
  const m = /([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\.jsonl$/
    .exec(basename(path))
  return m ? m[1] : null
}
