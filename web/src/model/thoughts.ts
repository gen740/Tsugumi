// A window's consent, usage and short-lived presentation. Agentd decides when
// work needs another summary and what conversation content to send.
import { L } from '../i18n'

export interface ThoughtReply { text: string; usage: { inputTokens: number; outputTokens: number } }
export interface ThoughtContext { identity: string; active: boolean }
const ENABLED = 'agent.drawThoughts'
const CONSENT = 'agent.drawThoughtsAsked'
function browserStorage(): Storage | undefined {
  try { return globalThis.localStorage } catch { return undefined }
}

export class Thoughts {
  enabled = false
  private consent = false
  usage = { inputTokens: 0, outputTokens: 0 }
  lastError?: string
  private pending = false
  private generation = 0
  private observing = false
  private timer?: ReturnType<typeof setInterval>

  constructor(private readonly context: () => ThoughtContext,
    private readonly request: () => Promise<ThoughtReply>,
    private readonly show: (text?: string) => void,
    private readonly changed: () => void,
    private readonly storage: Pick<Storage, 'getItem' | 'setItem'> | undefined = browserStorage()) {
    try {
      this.consent = storage?.getItem(CONSENT) === 'true'
      this.enabled = this.consent && storage?.getItem(ENABLED) === 'true'
    } catch {}
  }

  get label() {
    return this.enabled
      ? L('思考の過程を描く: オン（使った token: 入力 {0}、出力 {1}）', this.usage.inputTokens, this.usage.outputTokens)
      : L('思考の過程を描く: Agent の考えを要約して Canvas に出します（追加の token を使います）')
  }

  /** False asks the composer to present the first-time consent. */
  toggle(): boolean {
    if (!this.enabled && !this.consent) return false
    this.setEnabled(!this.enabled)
    return true
  }

  accept() {
    this.consent = true
    try { this.storage?.setItem(CONSENT, 'true') } catch {}
    this.setEnabled(true)
  }

  private setEnabled(on: boolean) {
    this.enabled = on
    this.generation++
    try { this.storage?.setItem(ENABLED, String(on)) } catch {}
    if (!on) this.show()
    this.updateTimer()
    this.changed()
  }

  start() {
    this.observing = true
    this.updateTimer()
  }

  private updateTimer() {
    if (!this.enabled || !this.observing) {
      clearInterval(this.timer)
      this.timer = undefined
      return
    }
    // This is a window refresh cadence, not a model-call interval.
    this.timer ??= setInterval(() => void this.refresh(), 1000)
  }

  close() {
    this.observing = false
    clearInterval(this.timer)
    this.timer = undefined
    this.generation++
    this.show()
  }

  async refresh() {
    const before = this.context(), generation = this.generation
    if (!this.enabled || !before.active || this.pending) return
    this.pending = true
    try {
      const reply = await this.request()
      // Even a reply that arrived after Stop used tokens.
      this.usage.inputTokens += reply.usage.inputTokens
      this.usage.outputTokens += reply.usage.outputTokens
      const after = this.context()
      if (this.enabled && after.active && before.identity === after.identity && generation === this.generation) {
        const text = [...(reply.text.split(/\r?\n/)[0] ?? '').replace(/^[\s「」"'`*#-]+|[\s「」"'`*#-]+$/g, '')].slice(0, 60).join('')
        if (text) this.show(text)
      }
      this.lastError = undefined
    } catch (e) {
      this.lastError = String((e as Error).message ?? e)
    } finally {
      this.pending = false
      this.changed()
    }
  }
}
