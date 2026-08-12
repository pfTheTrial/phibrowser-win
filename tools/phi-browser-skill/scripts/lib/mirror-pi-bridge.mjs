// Copyright 2026 Phinomenon Inc.
//
// Pi's in-process half of Agent Transcript command delivery. The detached
// mirror tailer can observe Phi, but only a Pi extension loaded inside the
// active session can call pi.sendUserMessage() and wake an idle agent. This
// bridge binds that extension to the task recorded by ensureAgentSpace(),
// drains Phi's authoritative command queue while the task is idle, and hands
// the commands to the extension callback as one user turn.

import { readDaemonControl, openPhiChannel } from './mirror-core.mjs'

const DEFAULT_POLL_MS = 1000

export function formatPiConsoleMessages(messages) {
  return (messages || [])
    .map((message) => String(message?.text || '').trim())
    .filter(Boolean)
    .map((text) => `[phi-console] ${text}`)
    .join('\n\n')
}

export class PiConsoleBridge {
  constructor({
    sessionKey,
    agentPid = process.pid,
    sendUserMessage,
    readControl = readDaemonControl,
    openChannel = openPhiChannel,
    pollMs = DEFAULT_POLL_MS,
  }) {
    if (!sessionKey) throw new Error('PiConsoleBridge: sessionKey is required')
    if (typeof sendUserMessage !== 'function') {
      throw new Error('PiConsoleBridge: sendUserMessage is required')
    }
    this.sessionKey = sessionKey
    this.agentPid = agentPid
    this.sendUserMessage = sendUserMessage
    this.readControl = readControl
    this.openChannel = openChannel
    this.pollMs = pollMs
    this.channel = null
    this.disposeEvent = null
    this.timer = null
    this.running = false
    this.stopped = true
  }

  start() {
    if (!this.stopped) return
    this.stopped = false
    this.#schedule(0)
  }

  stop() {
    this.stopped = true
    if (this.timer) clearTimeout(this.timer)
    this.timer = null
    this.#dropChannel()
  }

  /** One deterministic pass, exported for fixture tests. */
  async pollOnce() {
    const control = this.readControl(this.sessionKey)
    if (!control || control.format !== 'pi' || !control.taskId) {
      this.#dropChannel()
      return false
    }

    const channel = await this.#ensureChannel()
    const { tasks } = await channel.send('agentSpace.list', {})
    const task = (tasks || []).find((candidate) => candidate.taskId === control.taskId)
    if (!task || task.status !== 'idle' || !(task.pendingUserMessages > 0)) {
      return false
    }

    const result = await channel.send(
      'agentSpace.readUserMessages', { taskId: control.taskId })
    if (result && result.ok === false) {
      throw new Error(result.error || 'agentSpace.readUserMessages rejected')
    }
    const text = formatPiConsoleMessages(result?.messages)
    if (!text) return false

    // Always use steer. Pi starts a fresh turn when idle and safely queues the
    // message if a new run began between the app status read and this call.
    this.sendUserMessage(text, { deliverAs: 'steer' })
    return true
  }

  async #ensureChannel() {
    if (this.channel) return this.channel
    const channel = await this.openChannel({ agentPid: this.agentPid })
    this.disposeEvent = channel.onEvent?.('agentSpace.userMessage', ({ taskId }) => {
      const control = this.readControl(this.sessionKey)
      if (control?.format === 'pi' && control.taskId === taskId) this.#schedule(0)
    }) ?? null
    this.channel = channel
    return channel
  }

  #dropChannel() {
    try { this.disposeEvent?.() } catch {}
    try { this.channel?.close() } catch {}
    this.disposeEvent = null
    this.channel = null
  }

  #schedule(delay) {
    if (this.stopped) return
    if (this.timer) clearTimeout(this.timer)
    this.timer = setTimeout(() => this.#run(), delay)
    this.timer.unref?.()
  }

  async #run() {
    this.timer = null
    if (this.stopped || this.running) return
    this.running = true
    try {
      await this.pollOnce()
    } catch {
      // Phi may restart or the consent channel may disappear. Keep the Pi
      // session healthy, drop the stale socket, and retry on the next poll.
      this.#dropChannel()
    } finally {
      this.running = false
      this.#schedule(this.pollMs)
    }
  }
}
