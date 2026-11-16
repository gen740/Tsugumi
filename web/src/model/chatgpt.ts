import { api } from '../api'
import { errorText } from './session'
import { Observable } from './observable'

export interface ChatGPTAccountInfo { clientId: string; email: string | null; signedIn: boolean; planEnabled: boolean; active: boolean }
export interface ChatGPTState {
  accounts: ChatGPTAccountInfo[]
  signingIn: boolean
  outcome: { ok: boolean; message: string; welcome?: string } | null
  usageURL: string
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
  url?: string
  onConnected: () => void | Promise<void> = () => {}
  private completing = false
  private disposed = false
  private polling = false
  private beginning = false
  private cancelRequested = false
  constructor(
    readonly local = chatGPTSignInIsLocal(globalThis.location?.hostname ?? ''),
    private readonly request: typeof api = api,
    private readonly open: typeof window.open = (...args) => window.open(...args),
  ) { super() }

  mount() { this.disposed = false }
  dispose() {
    this.disposed = true
    if (this.beginning || this.state.signingIn) void this.cancel()
  }
  private canComplete(st: ChatGPTState) {
    const active = st.accounts.find(a => a.active)
    return st.outcome?.ok && active?.signedIn && active.planEnabled
  }
  setState = (value: ChatGPTState) => { this.state = value; this.changed() }
  async reload() {
    const state = await this.request<ChatGPTState>('/api/chatgpt')
    // An initial status read may finish after the user has clicked Sign In.
    const current = this.beginning ? { ...state, signingIn: true, outcome: null } : state
    this.setState(current)
    return current
  }
  async run(f: () => Promise<string | undefined>) {
    try { this.message = await f() }
    catch (e) { this.message = errorText(e) }
    this.changed()
  }
  signIn(clientId?: string, consent = false) {
    if (this.disposed || !this.local || this.beginning || this.state?.signingIn) return
    const tab = this.open('about:blank', '_blank')
    // Also disables the button while begin is pending.
    if (this.state) this.setState({ ...this.state, signingIn: true, outcome: null })
    this.message = undefined
    this.completing = false
    this.beginning = true
    this.cancelRequested = false
    void this.run(async () => {
      try {
        const r = await this.request<{ url: string }>('/api/chatgpt/signin', { clientId, consent })
        this.beginning = false
        this.url = r.url
        if (this.cancelRequested) {
          tab?.close()
          await this.request('/api/chatgpt/cancel', {})
        } else if (tab) { tab.opener = null; tab.location.href = r.url }
        await this.poll()
      } catch (e) {
        this.beginning = false
        tab?.close()
        if (this.state) this.setState({ ...this.state, signingIn: false })
        throw e
      }
      return this.message
    })
  }
  async poll() {
    if (this.polling || this.beginning) return
    this.polling = true
    try {
      const v = await this.reload()
      if (v.signingIn) return
      this.url = undefined
      if (v.outcome) this.message = v.outcome.message
      if (!this.disposed && this.canComplete(v) && !this.completing) {
        this.completing = true
        // Keep the setup sheet mounted until the once-only welcome is answered.
        if (!v.outcome?.welcome) await this.onConnected()
      }
      this.changed()
    } catch (e) { this.message = errorText(e); this.changed() }
    finally { this.polling = false }
  }
  async cancel() {
    this.cancelRequested = true
    if (this.beginning) return
    await this.run(async () => {
      await this.request('/api/chatgpt/cancel', {})
      await this.poll()
      return this.message
    })
  }
  async welcomed(go: boolean) {
    const st = this.state
    if (!st?.outcome?.welcome) return
    if (go) this.open(st.usageURL, '_blank', 'noopener,noreferrer')
    await this.run(async () => {
      this.setState(await this.request<ChatGPTState>('/api/chatgpt/welcomed', { clientId: st.outcome!.welcome }))
      if (!this.disposed && this.canComplete(st)) await this.onConnected()
      return this.message
    })
  }
}
