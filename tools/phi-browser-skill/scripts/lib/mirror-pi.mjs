// Copyright 2026 Phinomenon Inc.
//
// Pi adapter for the session mirror: locates the driving Pi session's
// transcript (session format v3 JSONL under ~/.pi/agent/sessions/<munged
// cwd>/<timestamp>_<uuid>.jsonl) and parses its records into console lines.
// Everything downstream (control file, cursor, batched forward) is the
// shared core — this file only knows what is Pi-specific.
//
// Discovery: Pi exports no session id to its shells (only the
// PI_CODING_AGENT marker, set in the CLI's own process and inherited), so
// the session is matched by evidence, never guessed: among session files
// written in the last 30 minutes, prefer the one whose header line's cwd
// matches ours and whose tail mentions the task id (Pi records the
// assistant's toolCall — task name included — before the shell runs). The
// header line also carries the session's uuid, which becomes the cursor key.

import { openSync, readSync, closeSync, readdirSync, statSync } from 'node:fs'
import { execFileSync } from 'node:child_process'
import { homedir } from 'node:os'
import { join, basename, isAbsolute, relative } from 'node:path'

// A session file not written for this long belongs to an ended/parked session.
const SESSION_MAX_AGE_MS = 30 * 60 * 1000

// Pi writes the call and its result as separate messages. Keep the small
// amount of display state that Pi's TUI keeps in ToolExecutionComponent so
// Phi can update one card instead of appending a second, disconnected row.
const pendingToolCalls = new Map()
const suppressedToolCalls = new Set()

/**
 * One session-v3 JSONL record → one or more Pi-styled console entries, or
 * null to skip. Assistant content stays in source order (thinking, text,
 * tool calls), just like Pi's interactive transcript. A tool result carries
 * the call id back through `sourceId`, allowing the app to settle the pending
 * card in place.
 */
export function toEntry(obj) {
  if (!obj || obj.type !== 'message' || !obj.message) return null
  const ts = Date.parse(obj.timestamp || '') || undefined
  const message = obj.message
  const role = message.role
  if (role === 'toolResult') return toolResultEntry(message, ts)
  if (role !== 'user' && role !== 'assistant') return null
  const content = Array.isArray(message.content) ? message.content : []
  if (role === 'user') {
    const text = content
      .filter((b) => b && b.type === 'text' && b.text)
      .map((b) => b.text).join('\n\n')
    // Pi prepends the full <skill> document to every user turn's text; the
    // real prompt (if any) follows the closing tag. A skill-only turn is
    // machinery, not a prompt.
    const close = text.lastIndexOf('</skill>')
    const prompt = (close >= 0 ? text.slice(close + '</skill>'.length) : text).trim()
    if (!prompt) return null
    // A "[phi-console]"-marked line is a console command delivered into the
    // session; the app already echoed it at enqueue time.
    if (prompt.startsWith('[phi-console]')) return null
    return { kind: 'user', text: prompt, ts }
  }

  const entries = []
  for (let i = 0; i < content.length; i++) {
    const block = content[i]
    if (!block || typeof block !== 'object') continue
    if (block.type === 'thinking') {
      // Pi joins adjacent thinking blocks into one italic Markdown section.
      const thoughts = []
      for (; i < content.length && content[i]?.type === 'thinking'; i++) {
        const thought = String(content[i]?.thinking || '').trim()
        if (thought) thoughts.push(thought)
      }
      i--
      if (thoughts.length) entries.push({ kind: 'thinking', text: thoughts.join('\n\n'), ts })
      continue
    }
    if (block.type === 'text') {
      const text = String(block.text || '').trim()
      if (text) entries.push({ kind: 'assistant', text, ts })
      continue
    }
    if (block.type === 'toolCall') {
      const entry = toolCallEntry(block, ts)
      if (entry) entries.push(entry)
    }
  }

  const hasToolCalls = content.some((block) => block?.type === 'toolCall')
  if (message.stopReason === 'length') {
    entries.push({ kind: 'error',
      text: 'Error: Model stopped because it reached the maximum output token limit. The response may be incomplete.', ts })
  } else if (!hasToolCalls && message.stopReason === 'aborted') {
    entries.push({ kind: 'error',
      text: message.errorMessage && message.errorMessage !== 'Request was aborted'
        ? message.errorMessage : 'Operation aborted', ts })
  } else if (!hasToolCalls && message.stopReason === 'error') {
    entries.push({ kind: 'error', text: `Error: ${message.errorMessage || 'Unknown error'}`, ts })
  }
  if (!entries.length) return null
  return entries.length === 1 ? entries[0] : entries
}

function toolCallEntry(block, ts) {
  const id = String(block.id || '')
  const name = String(block.name || 'tool')
  const args = objectArgs(block.arguments)
  if (name === 'bash' && isPhiPlumbing(args.command)) {
    if (id) suppressedToolCalls.add(id)
    return null
  }
  const display = toolCallDisplay(name, args)
  if (id) pendingToolCalls.set(id, { name, args, ...display, startedAt: ts })
  return {
    kind: 'tool', text: display.title,
    ...(display.detail ? { detail: display.detail } : {}),
    ...(id ? { sourceId: id } : {}),
    toolState: 'pending', ts,
  }
}

function toolResultEntry(message, ts) {
  const id = String(message.toolCallId || '')
  if (id && suppressedToolCalls.delete(id)) return null
  const pending = id ? pendingToolCalls.get(id) : null
  if (id) pendingToolCalls.delete(id)
  const name = String(message.toolName || pending?.name || 'tool')
  const args = pending?.args || {}
  const output = textOutput(message.content)
  const isError = message.isError === true
  const detail = toolResultDetail(name, args, output, message.details,
                                  isError, pending, ts)
  const title = pending?.title || toolCallDisplay(name, args).title
  return {
    kind: 'tool', text: title,
    ...(detail ? { detail } : {}),
    ...(id ? { sourceId: id } : {}),
    toolState: isError ? 'error' : 'success', ts,
  }
}

function toolCallDisplay(name, args) {
  const path = displayPath(args.file_path ?? args.path)
  switch (name) {
  case 'bash': {
    const command = typeof args.command === 'string' ? args.command : '[invalid command arg]'
    return { title: `$ ${command || '...'}`
      + (args.timeout ? ` (timeout ${args.timeout}s)` : '') }
  }
  case 'read': {
    let range = ''
    if (args.offset !== undefined || args.limit !== undefined) {
      const start = numeric(args.offset, 1)
      const limit = Number(args.limit)
      range = `:${start}${Number.isFinite(limit) ? `-${start + limit - 1}` : ''}`
    }
    return { title: `read ${path}${range}` }
  }
  case 'write':
    return { title: `write ${path}`, detail: previewLines(args.content, 10, true) }
  case 'edit':
    return { title: `edit ${path}` }
  case 'grep':
    return { title: `grep /${stringArg(args.pattern)}/ in ${displayPath(args.path || '.')}`
      + (args.glob ? ` (${args.glob})` : '')
      + (args.limit !== undefined ? ` limit ${args.limit}` : '') }
  case 'find':
    return { title: `find ${stringArg(args.pattern)} in ${displayPath(args.path || '.')}`
      + (args.limit !== undefined ? ` (limit ${args.limit})` : '') }
  case 'ls':
    return { title: `ls ${displayPath(args.path || '.')}`
      + (args.limit !== undefined ? ` (limit ${args.limit})` : '') }
  default: {
    const json = Object.keys(args).length ? JSON.stringify(args, null, 2) : ''
    return { title: name, ...(json ? { detail: json } : {}) }
  }
  }
}

function toolResultDetail(name, args, output, details, isError, pending, ts) {
  if (isError) return previewLines(output, 10)
  switch (name) {
  case 'bash': {
    const result = tailLines(output, 5)
    const elapsed = pending?.startedAt && ts
      ? `Took ${((ts - pending.startedAt) / 1000).toFixed(1)}s` : ''
    return [result, elapsed].filter(Boolean).join('\n')
  }
  case 'read': return '' // Pi hides successful read output while collapsed.
  case 'write': return pending?.detail || previewLines(args.content, 10, true)
  case 'edit': return typeof details?.diff === 'string' ? details.diff : ''
  case 'grep': return previewLines(output, 15)
  case 'find':
  case 'ls': return previewLines(output, 20)
  default:
    return [pending?.detail, output.trim()].filter(Boolean).join('\n\n')
  }
}

function objectArgs(value) {
  return value && typeof value === 'object' && !Array.isArray(value) ? value : {}
}

function textOutput(content) {
  if (typeof content === 'string') return content
  if (!Array.isArray(content)) return ''
  return content.filter((block) => block?.type === 'text')
    .map((block) => String(block.text || '')).join('\n')
}

function displayPath(value) {
  const raw = typeof value === 'string' ? value : '[invalid path arg]'
  if (!isAbsolute(raw)) return raw || '.'
  const rel = relative(process.cwd(), raw)
  return rel && !rel.startsWith('..') && !isAbsolute(rel) ? rel : raw
}

function stringArg(value) {
  return typeof value === 'string' ? value : '[invalid arg]'
}

function numeric(value, fallback) {
  const n = Number(value)
  return Number.isFinite(n) ? n : fallback
}

function previewLines(value, max, writePreview = false) {
  if (typeof value !== 'string' || !value) return ''
  const lines = value.replace(/\t/g, '    ').split('\n')
  while (lines.at(-1) === '') lines.pop()
  if (lines.length <= max) return lines.join('\n')
  const remaining = lines.length - max
  const suffix = writePreview
    ? `... (${remaining} more lines, ${lines.length} total, to expand)`
    : `... (${remaining} more lines, to expand)`
  return [...lines.slice(0, max), suffix].join('\n')
}

function tailLines(value, max) {
  const trimmed = String(value || '').trim()
  if (!trimmed) return ''
  const lines = trimmed.split('\n')
  if (lines.length <= max) return lines.join('\n')
  return [`... (${lines.length - max} earlier lines, to expand)`,
          ...lines.slice(-max)].join('\n')
}

function isPhiPlumbing(command) {
  return typeof command === 'string'
    && /(?:phi-browser-skill|(?:^|[\\/])runner\.mjs\b)/.test(command)
}

/**
 * The driving Pi session's transcript, or null. `agentPid` (the resolved
 * agent-root process) backs the under-Pi gate when the env marker is
 * stripped. Returns {sessionKey, path, format:'pi'}.
 */
export function discoverPiTranscript(taskId, agentPid) {
  if (!underPi(agentPid)) return null
  const files = recentSessionFiles(sessionsRoot())
  if (!files.length) return null
  const cwd = process.cwd()
  const scored = files.map((f) => {
    let score = 0
    let sessionId = null
    try {
      const header = JSON.parse(firstLine(f.path) || 'null')
      if (header?.type === 'session') {
        sessionId = header.id || null
        if (header.cwd === cwd) score += 2
      }
    } catch {}
    const taskMatch = !!taskId && tailOf(f.path).includes(jsonEscaped(taskId))
    if (taskMatch) score += 3
    return { ...f, score, sessionId, taskMatch }
  }).sort((a, b) => b.score - a.score || b.mtime - a.mtime)
  const best = scored[0]
  // When ensureAgentSpace supplied a task id, its freshly persisted tool call
  // is the binding proof. CWD alone is only a tie-breaker, never permission to
  // mirror or auto-deliver another concurrent Pi session's conversation.
  if (!best || best.score === 0 || (taskId && !best.taskMatch)) return null
  return {
    sessionKey: best.sessionId || `pi-${Math.round(best.mtime)}`,
    path: best.path,
    format: 'pi',
  }
}

// --- gate / filesystem helpers ------------------------------------------------

function underPi(agentPid) {
  if (process.env.PI_CODING_AGENT) return true
  try {
    if (agentPid) {
      // Pi brands itself via argv[0] ("pi" running node), so check the
      // command line, not the executable name.
      const argv0 = execFileSync('/bin/ps', ['-o', 'command=', '-p', String(agentPid)],
                                 { encoding: 'utf8' }).trim().split(/\s+/)[0] || ''
      return basename(argv0).toLowerCase() === 'pi'
    }
  } catch {}
  return false
}

function sessionsRoot() {
  return process.env.PI_CODING_AGENT_SESSION_DIR
    || join(homedir(), '.pi', 'agent', 'sessions')
}

/** Fresh session files across all project subdirs, newest first. */
function recentSessionFiles(root) {
  const out = []
  const now = Date.now()
  let dirs = []
  try { dirs = readdirSync(root) } catch { return out }
  for (const dir of dirs) {
    let files = []
    try { files = readdirSync(join(root, dir)) } catch { continue }
    for (const file of files) {
      if (!file.endsWith('.jsonl')) continue
      try {
        const path = join(root, dir, file)
        const mtime = statSync(path).mtimeMs
        if (now - mtime <= SESSION_MAX_AGE_MS) out.push({ path, mtime })
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

/** taskId as it appears inside the session's JSON-encoded command text. */
function jsonEscaped(value) {
  return JSON.stringify(String(value)).slice(1, -1)
}
