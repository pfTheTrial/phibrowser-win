// Phi Windows client autotest harness.
//
// Speaks BOTH halves of the client's unified named pipe (see
// client/phi_control_server.cc):
//   /phi-agent  -> WebSocket client control ({id,type,payloadJson} envelope,
//                  or the typed {id,type,payload} one with X-Phi-Protocol: 2)
//   otherwise   -> CDP (/json/* discovery, /devtools/<kind>/<id> sessions
//                  pumped through the framework's in-process transport)
// Discovery matches the Mac contract: the CDPAgentSocket pointer file in the
// user data dir holds the pipe name on line 1. Line 2 is the per-launch
// token every request must carry as X-Phi-Token, or the client answers 403
// before it consults anything else.
//
// Usage:  node phi-test.mjs [--user-data-dir DIR] [suite ...]
// Exit code 0 = all pass, 1 = any failure (so a runner can gate on it).

import net from 'node:net'
import crypto from 'node:crypto'
import { readFileSync, existsSync } from 'node:fs'

const USER_DATA_DIR =
  argValue('--user-data-dir') || 'C:\\phi-fx-test'

function argValue(flag) {
  const i = process.argv.indexOf(flag)
  return i > 0 ? process.argv[i + 1] : null
}

function pointerLines() {
  const ptr = `${USER_DATA_DIR}\\CDPAgentSocket`
  if (!existsSync(ptr)) throw new Error(`no pointer file: ${ptr}`)
  return readFileSync(ptr, 'utf8').split('\n').map((l) => l.trim())
}

export function pipePath() {
  return pointerLines()[0]
}

/** The launch token (line 2 of the pointer file). */
export function pipeToken() {
  const token = pointerLines()[1] || ''
  if (!token) throw new Error('pointer file has no token line')
  return token
}

/** The header block every request over the pipe must carry. */
function tokenHeader() {
  return `X-Phi-Token: ${pipeToken()}\r\n`
}

/** Raw connect to the client's pipe. */
function connect() {
  return new Promise((resolve, reject) => {
    const sock = net.connect(pipePath())
    sock.once('connect', () => resolve(sock))
    sock.once('error', reject)
  })
}

/** Minimal RFC-6455 client over an already-connected socket. */
class Ws {
  constructor(sock) {
    this.sock = sock
    this.buf = Buffer.alloc(0)
    this.waiters = []
    this.pendingFrames = []
    sock.on('data', (d) => {
      this.buf = Buffer.concat([this.buf, d])
      this.drain()
    })
  }
  drain() {
    for (;;) {
      if (this.buf.length < 2) return
      const b1 = this.buf[1]
      let len = b1 & 0x7f
      let off = 2
      if (len === 126) {
        if (this.buf.length < 4) return
        len = this.buf.readUInt16BE(2)
        off = 4
      } else if (len === 127) {
        if (this.buf.length < 10) return
        len = Number(this.buf.readBigUInt64BE(2))
        off = 10
      }
      if (this.buf.length < off + len) return
      const payload = this.buf.subarray(off, off + len).toString('utf8')
      this.buf = this.buf.subarray(off + len)
      const w = this.waiters.shift()
      if (w) w(payload)
      else this.pendingFrames.push(payload)
    }
  }
  send(text) {
    const p = Buffer.from(text, 'utf8')
    const mask = crypto.randomBytes(4)
    const head = []
    head.push(0x81)
    if (p.length < 126) head.push(0x80 | p.length)
    else if (p.length < 65536) head.push(0x80 | 126, p.length >> 8, p.length & 0xff)
    else throw new Error('frame too large for harness')
    const masked = Buffer.from(p)
    for (let i = 0; i < masked.length; i++) masked[i] ^= mask[i % 4]
    this.sock.write(Buffer.concat([Buffer.from(head), mask, masked]))
  }
  next(timeoutMs = 10000) {
    if (this.pendingFrames.length) {
      return Promise.resolve(this.pendingFrames.shift())
    }
    return new Promise((resolve, reject) => {
      const waiter = (v) => { clearTimeout(t); resolve(v) }
      const t = setTimeout(() => {
        // Drop OUR waiter — a timed-out reader left in the queue would eat
        // the next frame to arrive (that is how a pushed event goes missing).
        const i = this.waiters.indexOf(waiter)
        if (i >= 0) this.waiters.splice(i, 1)
        reject(new Error('ws read timeout'))
      }, timeoutMs)
      this.waiters.push(waiter)
    })
  }
  close() { this.sock.destroy() }
}

/** Client control channel: ctl.send('agentSpace.list', {}) -> parsed response.
 *  `capability` is the X-Phi-Agent-Capability the client echoed on the 101
 *  (phase 4: the principal this connection joined — every connection of one
 *  image in a session shares it), or null on a build that issues none. The
 *  rig never needs to present it: PhiSettings.json testRig makes its
 *  principal the host, which sees and drives every task.
 *
 *  `{protocol: 2}` asks for the typed control wire (`X-Phi-Protocol: 2`):
 *  requests carry `payload` as an object and replies come back as
 *  {id, ok, result|error{code,message?,details?}}. The default is 1 — the
 *  byte-compatible legacy envelope every suite below is written against.
 *  `send()` flattens either wire to the same `{ok, error?, …}` body, so a
 *  suite reads the same fields whichever it opened; `ctl.protocol` says
 *  which the app confirmed, and `ctl.lastEnvelope` keeps the raw reply for
 *  a test that wants to assert the envelope itself. */
export async function openControl({ protocol = 1 } = {}) {
  const sock = await connect()
  const key = crypto.randomBytes(16).toString('base64')
  sock.write(
    `GET /phi-agent HTTP/1.1\r\nHost: phi\r\n${tokenHeader()}` +
    (protocol === 2 ? `X-Phi-Protocol: 2\r\n` : '') +
    `Upgrade: websocket\r\n` +
    `Connection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\n` +
    `Sec-WebSocket-Version: 13\r\n\r\n`)
  const handshake = await new Promise((resolve, reject) => {
    const onData = (d) => {
      const s = d.toString('latin1')
      if (s.includes('\r\n\r\n')) {
        if (!/101/.test(s)) return reject(new Error(`bad handshake: ${s.split('\r\n')[0]}`))
        sock.removeListener('data', onData)
        const head = s.slice(0, s.indexOf('\r\n\r\n'))
        const echoed = /^x-phi-agent-capability:\s*([A-Za-z0-9_-]{32,128})\s*$/im.exec(head)
        const wire = /^x-phi-protocol:\s*(\d+)\s*$/im.exec(head)
        resolve({ capability: echoed ? echoed[1] : null,
                  protocol: wire ? Number(wire[1]) : 1 })
      }
    }
    sock.on('data', onData)
    sock.once('error', reject)
    setTimeout(() => reject(new Error('handshake timeout')), 8000)
  })
  const { capability } = handshake
  const wire = handshake.protocol
  const ws = new Ws(sock)
  let id = 0
  const listeners = []
  // A v2 reply, folded back into the flat body v1 carries in responseJson:
  // the finer legacy token rides v2 as error.details.reason.
  const flatten = (env) => {
    if (env.ok) return { ok: true, ...(env.result ?? {}) }
    const err = env.error ?? {}
    const { reason, ...details } = err.details ?? {}
    return { ok: false, error: reason ?? err.code ?? 'internal', ...details }
  }
  const eventPayload = (env) => {
    if (env.payloadJson === undefined) return env.payload ?? {}
    try { return JSON.parse(env.payloadJson || '{}') } catch { return {} }
  }
  const handle = {
    capability,
    protocol: wire,
    lastEnvelope: null,
    async send(type, payload = {}) {
      const mine = ++id
      ws.send(JSON.stringify(
        wire === 2 ? { id: mine, type, payload }
                   : { id: mine, type, payloadJson: JSON.stringify(payload) }))
      // Pushed events (agentSpace.ownershipChanged and friends) interleave
      // with replies, so read until OUR id comes back — the same id matching
      // the real skill does.
      for (;;) {
        const env = JSON.parse(await ws.next())
        if (env.event) {
          const p = eventPayload(env)
          for (const cb of listeners) { try { cb(env.event, p) } catch {} }
          continue
        }
        if (env.id != null && env.id !== mine) continue
        handle.lastEnvelope = env
        if (env.ok !== undefined) return flatten(env)
        if (env.error) throw new Error(`${type}: ${env.error}`)
        return env.responseJson ? JSON.parse(env.responseJson) : {}
      }
    },
    /** Registers a pushed-event listener: cb(eventType, payload). */
    onEvent(cb) { listeners.push(cb); return () => {
      const i = listeners.indexOf(cb); if (i >= 0) listeners.splice(i, 1) } },
    /** Drains frames for `ms`, dispatching any events that arrive. */
    async pump(ms) {
      const deadline = Date.now() + ms
      while (Date.now() < deadline) {
        let raw
        try { raw = await ws.next(Math.max(1, deadline - Date.now())) } catch { return }
        let env
        try { env = JSON.parse(raw) } catch { continue }
        if (!env.event) continue
        const p = eventPayload(env)
        for (const cb of listeners) { try { cb(env.event, p) } catch {} }
      }
    },
    close: () => ws.close(),
  }
  return handle
}

/** One HTTP GET over the pipe (CDP's /json endpoints). */
export async function cdpHttp(path) {
  const sock = await connect()
  sock.write(`GET ${path} HTTP/1.1\r\nHost: phi\r\n${tokenHeader()}\r\n`)
  const body = await new Promise((resolve, reject) => {
    // Accumulate BYTES: Content-Length counts bytes, and a decoded string's
    // length counts code units — a single non-ASCII tab title (the box's
    // zh-CN new-tab page, say) made the old string compare wait forever.
    let acc = Buffer.alloc(0)
    sock.on('data', (d) => {
      acc = Buffer.concat([acc, d])
      const i = acc.indexOf('\r\n\r\n')
      if (i < 0) return
      const head = acc.subarray(0, i).toString('latin1')
      const m = /Content-Length:\s*(\d+)/i.exec(head)
      const need = m ? Number(m[1]) : 0
      const rest = acc.subarray(i + 4)
      if (rest.length >= need) resolve(rest.subarray(0, need).toString('utf8'))
    })
    sock.once('error', reject)
    setTimeout(() => reject(new Error('cdp http timeout')), 8000)
  })
  sock.destroy()
  return JSON.parse(body)
}

/** CDP WebSocket session against a target's ws path (from /json list). */
export async function openCdp(wsPath) {
  const sock = await connect()
  const key = crypto.randomBytes(16).toString('base64')
  sock.write(
    `GET ${wsPath} HTTP/1.1\r\nHost: phi\r\n${tokenHeader()}` +
    `Upgrade: websocket\r\n` +
    `Connection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\n` +
    `Sec-WebSocket-Version: 13\r\n\r\n`)
  await new Promise((resolve, reject) => {
    const onData = (d) => {
      const s = d.toString('latin1')
      if (s.includes('\r\n\r\n')) {
        if (!/101/.test(s)) return reject(new Error(`bad cdp handshake: ${s.split('\r\n')[0]}`))
        sock.removeListener('data', onData)
        resolve()
      }
    }
    sock.on('data', onData)
    sock.once('error', reject)
    setTimeout(() => reject(new Error('cdp handshake timeout')), 8000)
  })
  const ws = new Ws(sock)
  let id = 0
  return {
    async send(method, params = {}, sessionId) {
      const mine = ++id
      ws.send(JSON.stringify({ id: mine, method, params, ...(sessionId ? { sessionId } : {}) }))
      for (;;) {
        const msg = JSON.parse(await ws.next())
        if (msg.id === mine) {
          if (msg.error) throw new Error(`${method}: ${msg.error.message}`)
          return msg.result
        }
        // Ignore events/other ids for this simple harness.
      }
    },
    close: () => ws.close(),
  }
}

// ---- tiny test framework ----
const tests = []
export function test(name, fn) { tests.push({ name, fn }) }
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms))

export function assert(cond, msg) {
  if (!cond) throw new Error(`assertion failed: ${msg}`)
}
export function assertEq(actual, expected, msg) {
  if (actual !== expected) {
    throw new Error(`assertion failed: ${msg} (got ${JSON.stringify(actual)}, want ${JSON.stringify(expected)})`)
  }
}

export async function run() {
  let pass = 0, fail = 0
  for (const t of tests) {
    const started = Date.now()
    try {
      await t.fn()
      console.log(`PASS  ${t.name}  (${Date.now() - started}ms)`)
      pass++
    } catch (e) {
      console.log(`FAIL  ${t.name}  (${Date.now() - started}ms)\n      ${e.message}`)
      fail++
    }
  }
  console.log(`\n${pass} passed, ${fail} failed`)
  process.exitCode = fail === 0 ? 0 : 1
}

/**
 * Waits until the client has flushed its coalesced store writes (phase 3:
 * saves land on a 250 ms timer; agentSpace.ping reports pendingSaves). Call
 * before killing the process, or the last change is lost.
 */
export async function waitForSaves(timeoutMs = 3000) {
  const started = Date.now()
  const ctl = await openControl()
  try {
    for (;;) {
      const r = await ctl.send('agentSpace.ping')
      if (!r || r.pendingSaves === undefined || r.pendingSaves === 0) return
      if (Date.now() - started > timeoutMs) return
      await sleep(100)
    }
  } finally { ctl.close() }
}
