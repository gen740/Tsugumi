import { api, onServerEvent } from '../api'
import { errorText } from './session'
import { Observable } from './observable'

export interface ChatGPTAccountInfo { clientId: string; email: string | null; signedIn: boolean; planEnabled: boolean; active: boolean }
export interface ChatGPTState {
  accounts: ChatGPTAccountInfo[]
  signingIn: boolean
  attemptId?: string
  outcome: { ok: boolean; message: string; clientId?: string; planEnabled?: boolean; welcome?: string } | null
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
  onConnected: (clientId: string) => void | Promise<void> = () => {}
  private disposed = false
  private beginning = false
  private cancelRequested = false
  private attempt?: { id: string; status?: ChatGPTState; notified: boolean }
  private shared = this.state
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
    this.unobserve = this.observe(e => {
      if (['chatgptChanged', 'eventsConnected', 'reconnected'].includes(e.type)) void this.synchronize()
      if (e.type === 'hostUnavailable') {
        this.connectionError = e.message
        this.changed()
      }
    })
    return this.synchronize()
  }
  synchronize() {
    return this.run(async () => { await this.reload(); return this.message })
  }
  dispose() {
    this.disposed = true
    this.unobserve?.()
    if (this.beginning || (this.attempt && this.attempt.status?.signingIn !== false)) void this.cancel(this.attempt?.id)
  }
  private connectedAccount(st: ChatGPTState) {
    const account = st.accounts.find(a => a.clientId === st.outcome?.clientId)
    return st.outcome?.planEnabled && account?.signedIn && account.planEnabled ? account.clientId : undefined
  }
  async reload() {
    const read = ++this.read
    const attempt = this.attempt
    const [shared, own] = await Promise.allSettled([
      this.request<ChatGPTState>('/api/chatgpt'),
      attempt && (attempt.notified ? attempt.status : this.request<ChatGPTState>(`/api/chatgpt?attemptId=${encodeURIComponent(attempt.id)}`)),
    ])
    if (read !== this.read || attempt !== this.attempt) return this.state
    const failure = [shared, own].find(result => result.status === 'rejected')
    this.connectionError = failure && errorText(failure.reason)
    if (shared.status === 'fulfilled') this.shared = shared.value
    if (attempt) {
      let status = own.status === 'fulfilled' ? own.value : undefined
      if ((!status || status.signingIn) && shared.status === 'fulfilled' && shared.value.attemptId === attempt.id)
        status = shared.value
      if (status) {
        if (!status.signingIn && attempt.status?.signingIn !== false) {
          this.message = status.outcome?.message
          this.url = undefined
        }
        attempt.status = status
      }
    }
    const perAttempt = attempt?.status
    const latest = this.shared.attemptId === perAttempt?.attemptId && perAttempt ? perAttempt : this.shared
    // The attempt read also contains current accounts when the shared read fails.
    const current = shared.status === 'rejected' && !attempt?.notified && own.status === 'fulfilled' && own.value
      ? own.value : this.shared
    this.state = { ...latest, accounts: current.accounts, connectionMessage: current.connectionMessage,
      signingIn: this.beginning || latest.signingIn || (!!attempt && perAttempt?.signingIn !== false),
      outcome: this.beginning ? null : attempt ? perAttempt?.outcome ?? null : latest.outcome,
    }
    this.changed()
    if (attempt && !attempt.notified && !this.state.outcome?.welcome && !this.disposed) {
      const clientId = this.connectedAccount(this.state)
      if (clientId) {
        attempt.notified = true
        await this.onConnected(clientId)
      }
    }
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
      this.attempt = { id: result.attemptId, notified: false }
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
  async cancel(attemptId = this.attempt?.status?.signingIn === false ? this.state.attemptId : this.attempt?.id ?? this.state.attemptId) {
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
    const restored = !this.attempt && this.connectedAccount(st)
    if (go) this.open(st.usageURL, '_blank', 'noopener,noreferrer')
    await this.run(async () => {
      await this.request('/api/chatgpt/welcomed', { clientId })
      await this.reload()
      if (restored && !this.disposed) await this.onConnected(restored)
      return this.message
    })
  }
}
