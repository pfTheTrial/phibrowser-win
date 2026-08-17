// Phi Windows client autotest harness.
//
// Speaks BOTH halves of the client's unified named pipe (see
// client/phi_control_server.cc):
//   /phi-agent  -> WebSocket client control ({id,type,payloadJson} envelope)
//   otherwise   -> CDP (proxied to Chromium's DevTools)
// Discovery matches the Mac contract: the CDPAgentSocket pointer file in the
// user data dir holds the pipe name.
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

export function pipePath() {
  const ptr = `${USER_DATA_DIR}\\CDPAgentSocket`
  if (!existsSync(ptr)) throw new Error(`no pointer file: ${ptr}`)
  return readFileSync(ptr, 'utf8').split('\n')[0].trim()
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

/** Client control channel: ctl.send('agentSpace.list', {}) -> parsed response. */
export async function openControl() {
  const sock = await connect()
  const key = crypto.randomBytes(16).toString('base64')
  sock.write(
    `GET /phi-agent HTTP/1.1\r\nHost: phi\r\nUpgrade: websocket\r\n` +
    `Connection: Upgrade\r\nSec-WebSocket-Key: ${key}\r\n` +
    `Sec-WebSocket-Version: 13\r\n\r\n`)
  await new Promise((resolve, reject) => {
    const onData = (d) => {
      const s = d.toString('latin1')
      if (s.includes('\r\n\r\n')) {
        if (!/101/.test(s)) return reject(new Error(`bad handshake: ${s.split('\r\n')[0]}`))
        sock.removeListener('data', onData)
        resolve()
      }
    }
    sock.on('data', onData)
    sock.once('error', reject)
    setTimeout(() => reject(new Error('handshake timeout')), 8000)
  })
  const ws = new Ws(sock)
  let id = 0
  const listeners = []
  return {
    async send(type, payload = {}) {
      const mine = ++id
      ws.send(JSON.stringify({ id: mine, type, payloadJson: JSON.stringify(payload) }))
      // Pushed events (agentSpace.ownershipChanged and friends) interleave
      // with replies, so read until OUR id comes back — the same id matching
      // the real skill does.
      for (;;) {
        const env = JSON.parse(await ws.next())
        if (env.event) {
          let payload = {}
          try { payload = JSON.parse(env.payloadJson || '{}') } catch {}
          for (const cb of listeners) { try { cb(env.event, payload) } catch {} }
          continue
        }
        if (env.id != null && env.id !== mine) continue
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
        let payload = {}
        try { payload = JSON.parse(env.payloadJson || '{}') } catch {}
        for (const cb of listeners) { try { cb(env.event, payload) } catch {} }
      }
    },
    close: () => ws.close(),
  }
}

/** One HTTP GET over the pipe (CDP's /json endpoints). */
export async function cdpHttp(path) {
  const sock = await connect()
  sock.write(`GET ${path} HTTP/1.1\r\nHost: phi\r\n\r\n`)
  const body = await new Promise((resolve, reject) => {
    let acc = ''
    sock.on('data', (d) => {
      acc += d.toString('utf8')
      const i = acc.indexOf('\r\n\r\n')
      if (i < 0) return
      const head = acc.slice(0, i)
      const m = /Content-Length:\s*(\d+)/i.exec(head)
      const need = m ? Number(m[1]) : 0
      const rest = acc.slice(i + 4)
      if (rest.length >= need) resolve(rest.slice(0, need))
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
    `GET ${wsPath} HTTP/1.1\r\nHost: phi\r\nUpgrade: websocket\r\n` +
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
