import { api, onServerEvent } from '../api'
import { errorText } from './session'
import { Observable } from './observable'

export interface ChatGPTAccountInfo { clientId: string; email: string | null; signedIn: boolean; planEnabled: boolean; active: boolean }
export interface ChatGPTState {
  accounts: ChatGPTAccountInfo[]
  signingIn: boolean
  attemptId?: string
  outcome: { ok: boolean; message: string; clientId?: string; welcome?: string } | null
  usageURL: string
  connectionMessage?: string
}

/** The loopback callback belongs to the server's machine (docs/security.md).
 *  A non-loopback page cannot offer this local sign-in flow. */
export function chatGPTSignInIsLocal(hostname: string) {
  return ['localhost', '127.0.0.1', '[::1]', '::1'].includes(hostname)
}

/** One presentation state machine for Settings and onboarding. Dependencies
 *  are replaceable by the existing API/browser doubles in behavioral tests. */
export class ChatGPTSignInModel extends Observable {
  state: ChatGPTState = { accounts: [], signingIn: false, outcome: null, usageURL: 'https://chatgpt.com/settings/usage' }
  message?: string
  connectionError?: string
  url?: string
  onConnected: () => void | Promise<void> = () => {}
  private disposed = false
  private beginning = false
  private cancelRequested = false
  private attempt?: { id: string; finished: boolean }
  private read = 0
  private unobserve?: () => void
  constructor(
    readonly local = chatGPTSignInIsLocal(globalThis.location?.hostname ?? ''),
    private readonly request: typeof api = api,
    private readonly open: typeof window.open = (...args) => window.open(...args),
    private readonly observe: typeof onServerEvent = onServerEvent,
  ) { super() }

  mount() {
    this.disposed = false
    const synchronize = () => this.run(async () => { await this.reload(); return this.message })
    this.unobserve = this.observe(e => {
      if (['chatgptChanged', 'eventsConnected', 'reconnected'].includes(e.type)) void synchronize()
      if (e.type === 'hostUnavailable') {
        this.connectionError = e.message
        this.changed()
      }
    })
    return synchronize()
  }
  dispose() {
    this.disposed = true
    this.unobserve?.()
    if (this.beginning || (this.attempt && !this.attempt.finished)) void this.cancel(this.attempt?.id)
  }
  private canComplete(st: ChatGPTState) {
    const account = st.accounts.find(a => a.clientId === st.outcome?.clientId)
    return st.outcome?.ok && account?.signedIn && account.planEnabled
  }
  async reload() {
    const read = ++this.read
    const attempt = this.attempt
    let own: ChatGPTState | undefined
    let shared: ChatGPTState
    try {
      if (attempt) own = await this.request(`/api/chatgpt?attemptId=${encodeURIComponent(attempt.id)}`)
      shared = await this.request('/api/chatgpt')
    } catch (e) {
      if (read === this.read) {
        this.connectionError = errorText(e)
        this.changed()
      }
      return this.state
    }
    if (read !== this.read || attempt !== this.attempt) return this.state
    if (attempt && shared.attemptId === attempt.id) own = shared

    this.connectionError = undefined
    let connected = false
    if (attempt && own && !own.signingIn) {
      if (!attempt.finished) this.message = own.outcome?.message
      attempt.finished = true
      this.url = undefined
      // Keep following this result until its welcome is acknowledged.
      if (!own.outcome?.welcome) {
        this.attempt = undefined
        connected = !!this.canComplete(own)
      }
    }
    this.state = { ...shared,
      signingIn: this.beginning || shared.signingIn || !!own?.signingIn,
      outcome: this.beginning ? null : own?.outcome?.welcome ? own.outcome : shared.outcome,
    }
    this.changed()
    if (connected && !this.disposed) await this.onConnected()
    return this.state
  }
  async run(f: () => Promise<string | undefined>) {
    try { this.message = await f() }
    catch (e) { this.message = errorText(e) }
    this.changed()
  }
  signIn(clientId?: string, consent = false) {
    if (this.disposed || !this.local || this.beginning || this.state.signingIn) return
    const tab = this.open('about:blank', '_blank')
    this.beginning = true
    this.cancelRequested = false
    this.message = undefined
    ++this.read
    this.state = { ...this.state, signingIn: true, outcome: null }
    this.changed()
    void this.run(async () => {
      let result: { url: string; attemptId: string }
      try {
        result = await this.request('/api/chatgpt/signin', { clientId, consent })
      } catch (error) {
        this.beginning = false
        tab?.close()
        this.state = { ...this.state, signingIn: false }
        await this.reload()
        throw error
      }
      this.beginning = false
      this.attempt = { id: result.attemptId, finished: false }
      if (this.cancelRequested) {
        tab?.close()
        await this.cancel(result.attemptId)
      } else {
        this.url = result.url
        if (tab) { tab.opener = null; tab.location.href = result.url }
        this.changed()
        // Read once after Begin, even if its completion event already arrived.
        await this.reload()
      }
      return this.message
    })
  }
  async cancel(attemptId = this.attempt?.id ?? this.state.attemptId) {
    this.cancelRequested = true
    if (this.beginning || !attemptId) return
    await this.run(async () => {
      await this.request('/api/chatgpt/cancel', { attemptId })
      await this.reload()
      return this.message
    })
  }
  updateAccount(action: 'select' | 'signout', clientId: string, onChanged?: () => void) {
    return this.run(async () => {
      const result = await this.request<{ message?: string }>(`/api/chatgpt/${action}`, { clientId })
      await this.reload()
      onChanged?.()
      return result.message ?? this.message
    })
  }
  async welcomed(go: boolean) {
    const st = this.state
    const clientId = st.outcome?.welcome
    if (!clientId) return
    const restored = !this.attempt && this.canComplete(st)
    if (go) this.open(st.usageURL, '_blank', 'noopener,noreferrer')
    await this.run(async () => {
      await this.request('/api/chatgpt/welcomed', { clientId })
      await this.reload()
      if (restored && !this.disposed) await this.onConnected()
      return this.message
    })
  }
}
