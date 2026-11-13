// Browser adapter: OAuth, token storage and rotation belong to agentd.
export interface ChatGPTEndpoints {
  issuer: string
  /** The resource the tokens are for (the Responses API). */
  resource: string
}

export function endpointsFor(issuer: string, resource: string): ChatGPTEndpoints {
  return { issuer, resource }
}

export const OPENAI_ENDPOINTS = endpointsFor('https://auth.openai.com', 'https://api.openai.com/v1')
/** ChatGPT settings > Usage. */
export const USAGE_URL = 'https://chatgpt.com/settings/usage'
/** What the settings page may know of a registration (never a token). */
export interface AccountInfo { clientId: string; email: string | null; signedIn: boolean; planEnabled: boolean; active: boolean }

export class ChatGPTError extends Error {
  constructor(detail: string) { super(`ChatGPT: ${detail}`) }
}

export interface SignInResult { account: AccountInfo; welcome: boolean }
export class ChatGPTAuth {
  private attempt?: string
  constructor(readonly profile: string, readonly endpoints: ChatGPTEndpoints,
    private readonly core: (method: string, fields: Record<string, unknown>) => Promise<any>) {}
  private status() { return this.core('SignInStatusRequest', { profile: this.profile, attempt_id: this.attempt }) }
  async accounts(): Promise<AccountInfo[]> { return (await this.status()).accounts ?? [] }
  get signingIn() { return !!this.attempt }
  async select(clientId: string) { await this.core('SignInActionRequest', { profile: this.profile, action: 'select', client_id: clientId }) }
  async markWelcomed(clientId: string) { await this.core('SignInActionRequest', { profile: this.profile, action: 'welcomed', client_id: clientId, attempt_id: this.attempt }) }
  async cancel() { if (this.attempt) await this.core('SignInActionRequest', { profile: this.profile, action: 'cancel', attempt_id: this.attempt }) }
  async signOut(clientId: string): Promise<boolean> { return (await this.core('SignInActionRequest', { profile: this.profile, action: 'signout', client_id: clientId })).revoked === true }
  async begin(clientId?: string, consent = false): Promise<{ url: string; done: Promise<SignInResult> }> {
    await this.cancel()
    const result = await this.core('SignInBeginRequest', { profile: this.profile, client_id: clientId, consent, issuer: this.endpoints.issuer, resource: this.endpoints.resource })
    const attempt = String(result.attempt_id)
    this.attempt = attempt
    const done = (async () => {
      try {
        for (;;) {
          const state = await this.core('SignInStatusRequest', { profile: this.profile, attempt_id: attempt })
          if (state.state === 'complete') {
            const account = (state.accounts as AccountInfo[]).find((a) => a.active)!
            return { account, welcome: state.welcome === account.clientId }
          }
          if (state.state === 'cancelled') throw new ChatGPTError('cancelled')
          if (state.state === 'failed') throw new ChatGPTError(state.message ?? 'Sign-in failed')
          await new Promise((resolve) => setTimeout(resolve, 200))
        }
      } finally { if (this.attempt === attempt) this.attempt = undefined }
    })()
    done.catch(() => {})
    return { url: String(result.url), done }
  }
}
