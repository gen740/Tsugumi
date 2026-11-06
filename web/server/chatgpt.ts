// Sign in with ChatGPT for open-source tools: the Agent uses the user's
// ChatGPT plan for its Responses API requests
// (developers.openai.com/siwc/token-sharing-open-source). The same flow as
// the macOS app's ChatGPT.swift.
//
// This server is an agent host of its own: its host ID and registrations are
// in one file of its data folder (0600, written whole and renamed into place),
// apart from the app's keychain item (`security -i` cannot write a record
// this long). Tokens never reach the page, tsugumid or Card workers, and are
// never written to project files, settings or logs.
import crypto from 'node:crypto'
import fs from 'node:fs'
import http from 'node:http'
import path from 'node:path'

export interface ChatGPTEndpoints {
  issuer: string
  authorize: string
  token: string
  revoke: string
  jwks: string
  /** The resource the tokens are for (the Responses API). */
  resource: string
}

export function endpointsFor(issuer: string, resource: string): ChatGPTEndpoints {
  const base = issuer.endsWith('/') ? issuer : issuer + '/'
  return {
    issuer, resource,
    authorize: new URL('api/accounts/authorize', base).href,
    token: new URL('api/accounts/oauth/token', base).href,
    revoke: new URL('api/accounts/oauth/revoke', base).href,
    jwks: new URL('.well-known/jwks.json', base).href,
  }
}

export const OPENAI_ENDPOINTS = endpointsFor('https://auth.openai.com', 'https://api.openai.com/v1')
export const PLAN_SCOPE = 'chatgpt.tokens.use.direct'
const SCOPE = `openid profile email offline_access resource.invoke ${PLAN_SCOPE}`
/** ChatGPT settings > Usage. */
export const USAGE_URL = 'https://chatgpt.com/settings/usage'
/** The chat shows "Manage usage" with this error (the same text as the app's). */
export const USAGE_LIMIT_TEXT = "ChatGPT usage limit reached: review your plan or this app's limit in ChatGPT settings"
const AGENT_NAME = 'Tsugumi'

/** One registration (the record of the sign-in guide). */
interface Account {
  email?: string
  issuer: string
  subject: string
  client_id: string
  ext_agent_host_id: string
  id_token?: string
  access_token?: string
  refresh_token?: string
  token_type?: string
  expires_in?: number
  scopes: string[]
  saved_at?: string
  /** "You're using your ChatGPT plan" was shown for this registration. */
  welcomed?: boolean
}

interface Store {
  ext_agent_host_id: string
  active_client_id?: string
  accounts: Account[]
}

/** What the settings page may know of a registration (never a token). */
export interface AccountInfo { clientId: string; email: string | null; signedIn: boolean; planEnabled: boolean; active: boolean }

export type ChatGPTErrorKind = 'signInRequired' | 'planNotEnabled' | 'usageLimit' | 'notEligible' | 'unsupported' | 'denied' | 'failed'

export class ChatGPTError extends Error {
  constructor(readonly kind: ChatGPTErrorKind, detail = '') {
    super(ChatGPTError.text(kind, detail))
  }
  static text(kind: ChatGPTErrorKind, d: string) {
    switch (kind) {
      case 'signInRequired': return `ChatGPT: sign in again in Settings > API (${d})`
      case 'planNotEnabled': return 'ChatGPT: the use of your ChatGPT plan is not enabled for Tsugumi; enable it in Settings > API'
      case 'usageLimit': return USAGE_LIMIT_TEXT
      case 'notEligible': return `ChatGPT: your plan cannot be used in Tsugumi (${d})`
      case 'unsupported': return `ChatGPT: the request used something your plan does not support (${d})`
      case 'denied': return 'ChatGPT: the sign-in was declined'
      default: return `ChatGPT: ${d}`
    }
  }
}

/** A failure of the server or the network: the tokens stay, and the request is retried. */
export class TransientError extends Error {
  constructor(readonly status: number, message: string) {
    super(message)
  }
}

const ENDED = new Set(['invalid_grant', 'invalid_refresh_token', 'token_expired', 'refresh_token_expired', 'refresh_token_invalidated', 'refresh_token_reused'])

const b64url = (b: Buffer) => b.toString('base64url')
const random = (n = 32) => b64url(crypto.randomBytes(n))
const planEnabled = (a: Account) => a.scopes.includes(PLAN_SCOPE)
const signedIn = (a: Account) => !!a.access_token && !!a.refresh_token
/** The access token ends within five minutes (or its expiry is unknown). */
const expiresSoon = (a: Account) => !a.saved_at || !a.expires_in || Date.parse(a.saved_at) + (a.expires_in - 300) * 1000 < Date.now()
const scopesOf = (s: unknown) => (typeof s === 'string' ? s.split(' ').filter(Boolean).sort() : [])

function errorCode(obj: any): string | undefined {
  if (typeof obj?.error === 'string') return obj.error
  if (obj?.error && typeof obj.error === 'object') return obj.error.code ?? obj.error.type
  return typeof obj?.code === 'string' ? obj.code : undefined
}

/** Every request ends well before a lock counts as stale (60 s), and a hung
 *  token endpoint never holds up the chats queued behind a refresh. */
const REQUEST_TIMEOUT = 15_000

async function postForm(url: string, form: Record<string, string>) {
  const r = await fetch(url, {
    method: 'POST', headers: { 'content-type': 'application/x-www-form-urlencoded' }, body: new URLSearchParams(form).toString(),
    signal: AbortSignal.timeout(REQUEST_TIMEOUT),
  })
  const text = await r.text()
  let obj: any = {}
  try { obj = JSON.parse(text) } catch {}
  return { status: r.status, obj }
}

export interface PlanAccessToken { clientId: string; value: string }

export interface SignInResult { account: AccountInfo; welcome: boolean }

export class ChatGPTAuth {
  private chain: Promise<unknown> = Promise.resolve()
  private pending?: { cancel: () => void }

  constructor(readonly file: string, readonly endpoints: ChatGPTEndpoints = OPENAI_ENDPOINTS) {}

  // ---- Record --------------------------------------------------------------------

  private load(): Store | undefined {
    try {
      return JSON.parse(fs.readFileSync(this.file, 'utf8')) as Store
    } catch {
      return undefined
    }
  }

  private save(s: Store) {
    fs.mkdirSync(path.dirname(this.file), { recursive: true })
    const tmp = `${this.file}.${process.pid}.${random(6)}`
    fs.writeFileSync(tmp, JSON.stringify(s), { mode: 0o600 })
    fs.chmodSync(tmp, 0o600)
    fs.renameSync(tmp, this.file)
  }

  /** Changes of the record one at a time in this server, and with another
   *  server on the same data folder (mkdir is atomic; a lock older than a
   *  minute was left by a process that ended). */
  private locked<T>(f: () => Promise<T> | T): Promise<T> {
    const lock = this.file + '.lock'
    const run = async () => {
      const deadline = Date.now() + 30_000
      for (;;) {
        try {
          fs.mkdirSync(lock, { mode: 0o700 })
          break
        } catch (e) {
          if ((e as NodeJS.ErrnoException).code !== 'EEXIST') throw e
          try {
            if (fs.statSync(lock).mtimeMs < Date.now() - 60_000) { fs.rmdirSync(lock); continue }
          } catch { continue }
          if (Date.now() > deadline) throw new ChatGPTError('failed', `another process holds ${lock}`)
          await new Promise((r) => setTimeout(r, 100))
        }
      }
      try {
        return await f()
      } finally {
        fs.rmSync(lock, { recursive: true, force: true })
      }
    }
    const next = this.chain.then(run, run)
    this.chain = next.catch(() => undefined)
    return next
  }

  /** The record, with this host's ID made and kept before its first sign-in. */
  private loadOrCreate(): Promise<Store> {
    const s = this.load()
    if (s) return Promise.resolve(s)
    return this.locked(() => {
      const again = this.load()
      if (again) return again
      const made: Store = { ext_agent_host_id: `urn:uuid:${crypto.randomUUID()}`, accounts: [] }
      this.save(made)
      return made
    })
  }

  private update(change: (s: Store) => void) {
    return this.locked(() => {
      const s = this.load()
      if (!s) throw new ChatGPTError('signInRequired', 'no ChatGPT account')
      change(s)
      this.save(s)
    })
  }

  // ---- Accounts ------------------------------------------------------------------

  accounts(): AccountInfo[] {
    const s = this.load()
    return (s?.accounts ?? []).map((a) => ({ clientId: a.client_id, email: a.email ?? null, signedIn: signedIn(a), planEnabled: planEnabled(a), active: a.client_id === s?.active_client_id }))
  }

  get signingIn() {
    return !!this.pending
  }

  select(clientId: string) {
    return this.update((s) => {
      if (!s.accounts.some((a) => a.client_id === clientId)) throw new ChatGPTError('failed', 'unknown account')
      s.active_client_id = clientId
    })
  }

  markWelcomed(clientId: string) {
    return this.update((s) => {
      const a = s.accounts.find((x) => x.client_id === clientId)
      if (a) a.welcomed = true
    })
  }

  cancel() {
    this.pending?.cancel()
  }

  // ---- Sign-in -------------------------------------------------------------------

  /** Starts a sign-in: the URL to open in the browser, and its result once the
   *  loopback callback came. `clientId` signs in again to a saved
   *  registration; none registers a new one. `consent` asks again for the
   *  permissions (to enable the plan later). */
  async begin(clientId?: string, consent = false): Promise<{ url: string; done: Promise<SignInResult> }> {
    this.cancel()
    const store = await this.loadOrCreate()
    const existing = clientId ? store.accounts.find((a) => a.client_id === clientId) : undefined
    if (clientId && !existing) throw new ChatGPTError('failed', 'unknown account')
    const state = random(), nonce = random(), verifier = random(48)
    const challenge = b64url(crypto.createHash('sha256').update(verifier).digest())

    let settle!: { resolve: (q: URLSearchParams) => void; reject: (e: Error) => void }
    const answer = new Promise<URLSearchParams>((resolve, reject) => (settle = { resolve, reject }))
    const server = http.createServer((req, res) => {
      const u = new URL(req.url ?? '/', 'http://127.0.0.1')
      // Another state is not this sign-in's answer (a page or process that found the port): keep waiting.
      if (req.method !== 'GET' || u.pathname !== '/auth/callback' || u.searchParams.get('state') !== state) {
        res.writeHead(404).end('Not found')
        return
      }
      const ok = !u.searchParams.has('error')
      const body = ok
        ? 'サインインしました。このタブを閉じて Tsugumi に戻ってください。 / Signed in. Close this tab and return to Tsugumi.'
        : 'サインインは完了しませんでした。Tsugumi に戻ってください。 / The sign-in did not finish. Return to Tsugumi.'
      res.writeHead(200, { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store', 'referrer-policy': 'no-referrer', connection: 'close' })
      res.end(`<!doctype html><meta charset=utf-8><title>Tsugumi</title><body style="font: 15px -apple-system, sans-serif; margin: 3em"><p>${body}</p>`)
      settle.resolve(u.searchParams)
    })
    await new Promise<void>((resolve, reject) => {
      server.once('error', reject)
      server.listen(0, '127.0.0.1', () => resolve())
    })
    const port = (server.address() as { port: number }).port
    const redirect = `http://127.0.0.1:${port}/auth/callback`
    const timer = setTimeout(() => settle.reject(new ChatGPTError('failed', 'the sign-in was not finished in the browser in time')), 600_000)
    const close = () => {
      clearTimeout(timer)
      server.close()
      server.closeAllConnections()
    }
    const pending = { cancel: () => settle.reject(new ChatGPTError('failed', 'cancelled')) }
    this.pending = pending

    const q = new URLSearchParams({
      client_id: existing?.client_id ?? 'dynamic_agent_client',
      ext_agent_host_id: existing?.ext_agent_host_id ?? store.ext_agent_host_id,
      response_type: 'code', redirect_uri: redirect, scope: SCOPE, resource: this.endpoints.resource,
      state, nonce, code_challenge_method: 'S256', code_challenge: challenge,
    })
    if (existing) {
      if (existing.id_token) q.set('id_token_hint', existing.id_token)
      if (existing.email) q.set('login_hint', existing.email)
    } else {
      q.set('agent_name_hint', AGENT_NAME)
    }
    if (consent) q.set('prompt', 'consent')

    const done = (async (): Promise<SignInResult> => {
      try {
        const cb = await answer
        if (cb.get('state') !== state) throw new ChatGPTError('failed', 'the sign-in answer did not match its request (state)')
        const err = cb.get('error')
        if (err) throw err === 'access_denied' ? new ChatGPTError('denied') : new ChatGPTError('failed', `${err}: ${cb.get('error_description') ?? ''}`)
        let issued: string
        if (existing) {
          const c = cb.get('client_id')
          if (c && c !== existing.client_id) throw new ChatGPTError('failed', 'the sign-in returned another registration')
          issued = existing.client_id
        } else {
          const c = cb.get('client_id')
          if (!c || c === 'dynamic_agent_client') throw new ChatGPTError('failed', 'the registration did not finish (no client ID)')
          issued = c
        }
        const code = cb.get('code')
        if (!code) throw new ChatGPTError('failed', 'the sign-in returned no code')
        const t = await postForm(this.endpoints.token, {
          grant_type: 'authorization_code', client_id: issued, code, code_verifier: verifier, redirect_uri: redirect, resource: this.endpoints.resource,
        })
        if (t.status !== 200) throw new ChatGPTError('failed', `the code exchange failed: ${errorCode(t.obj) ?? `HTTP ${t.status}`}; sign in again`)
        const { id_token, access_token, refresh_token } = t.obj
        if (typeof id_token !== 'string' || typeof access_token !== 'string' || typeof refresh_token !== 'string') {
          throw new ChatGPTError('failed', 'the token response lacks its tokens')
        }
        const claims = await this.verify(id_token, issued, nonce)
        const sub = claims.sub
        if (typeof sub !== 'string' || !sub) throw new ChatGPTError('failed', 'the ID token has no subject')
        if (existing && existing.subject !== sub) throw new ChatGPTError('failed', `you signed in to another ChatGPT account than ${existing.email ?? 'the selected one'}`)
        const account: Account = {
          email: typeof claims.email === 'string' ? claims.email : existing?.email, issuer: this.endpoints.issuer, subject: sub, client_id: issued,
          ext_agent_host_id: existing?.ext_agent_host_id ?? store.ext_agent_host_id, id_token, access_token, refresh_token,
          token_type: t.obj.token_type ?? 'Bearer', expires_in: typeof t.obj.expires_in === 'number' ? t.obj.expires_in : 3600,
          scopes: scopesOf(t.obj.scope ?? cb.get('scope')), saved_at: new Date().toISOString(), welcomed: existing?.welcomed,
        }
        // A new sign-in becomes the active account only now that it is validated.
        await this.update((s) => {
          const i = s.accounts.findIndex((a) => a.client_id === issued)
          if (i >= 0) s.accounts[i] = { ...account, welcomed: s.accounts[i].welcomed }
          else s.accounts.push(account)
          s.active_client_id = issued
        })
        return {
          account: { clientId: issued, email: account.email ?? null, signedIn: true, planEnabled: planEnabled(account), active: true },
          welcome: planEnabled(account) && !account.welcomed,
        }
      } finally {
        close()
        if (this.pending === pending) this.pending = undefined
      }
    })()
    done.catch(() => undefined)
    return { url: `${this.endpoints.authorize}?${q.toString()}`, done }
  }

  /** Signs out: revokes the refresh token, then forgets the tokens (the
   *  registration and the host ID stay). False when the revocation was not
   *  confirmed (the user can disconnect Tsugumi in ChatGPT settings). */
  signOut(clientId: string): Promise<boolean> {
    // Under the lock, so a refresh cannot rotate the token between its
    // revocation and the forgetting of the tokens.
    return this.locked(async () => {
      const s = this.load()
      const a = s?.accounts.find((x) => x.client_id === clientId)
      if (!s || !a) return true
      let revoked = !a.refresh_token
      for (let attempt = 0; attempt < 3 && !revoked && a.refresh_token; attempt++) {
        if (attempt) await new Promise((r) => setTimeout(r, 1000 * attempt))
        try {
          const r = await postForm(this.endpoints.revoke, { token: a.refresh_token, token_type_hint: 'refresh_token', client_id: clientId })
          if (r.status === 200) revoked = true
          else if (r.status < 500) break
        } catch {}
      }
      delete a.access_token, delete a.refresh_token, delete a.id_token, delete a.expires_in, delete a.saved_at
      this.save(s)
      return revoked
    })
  }

  // ---- Tokens --------------------------------------------------------------------

  /** The active account's access token, refreshed near its expiry. `stale` is
   *  a token the API refused: refreshed unless a refresh already replaced it. */
  async accessToken(stale?: PlanAccessToken): Promise<PlanAccessToken> {
    const s = this.load()
    const a = s?.accounts.find((x) => x.client_id === s.active_client_id)
    if (!s || !a) throw new ChatGPTError('signInRequired', 'no ChatGPT account is signed in')
    if (stale && stale.clientId !== a.client_id) throw new ChatGPTError('failed', 'the active account changed; try again')
    if (!signedIn(a)) throw new ChatGPTError('signInRequired', 'signed out')
    if (!planEnabled(a)) throw new ChatGPTError('planNotEnabled')
    if (stale ? a.access_token !== stale.value : !expiresSoon(a)) return { clientId: a.client_id, value: a.access_token! }
    const id = a.client_id
    return this.locked(async () => {
      // Read again: another refresh may have replaced the tokens meanwhile.
      const record = this.load()
      if (record?.active_client_id !== id) throw new ChatGPTError('failed', 'the active account changed; try again')
      const cur = record.accounts.find((x) => x.client_id === id)
      if (!cur || !signedIn(cur)) throw new ChatGPTError('signInRequired', 'signed out')
      if (!planEnabled(cur)) throw new ChatGPTError('planNotEnabled')
      if (stale ? cur.access_token !== stale.value : !expiresSoon(cur)) return { clientId: id, value: cur.access_token! }
      let r: { status: number; obj: any }
      try {
        r = await postForm(this.endpoints.token, { grant_type: 'refresh_token', client_id: id, refresh_token: cur.refresh_token!, resource: this.endpoints.resource })
      } catch (e) {
        throw new TransientError(0, `token refresh failed: ${(e as Error).message}`)
      }
      if (r.status >= 500 || r.status === 429) throw new TransientError(r.status, `token refresh failed (${r.status})`)
      const s2 = this.load()!
      const x = s2.accounts.find((y) => y.client_id === id)!
      if (r.status !== 200) {
        const code = errorCode(r.obj) ?? `HTTP ${r.status}`
        if (!ENDED.has(code)) throw new ChatGPTError('failed', `token refresh failed: ${code}`)
        // The refresh token can no longer be used: sign in again with the same registration.
        delete x.access_token
        delete x.refresh_token
        this.save(s2)
        throw new ChatGPTError('signInRequired', code)
      }
      if (typeof r.obj.access_token !== 'string') throw new ChatGPTError('failed', 'the refresh returned no access token')
      // The access token, its expiry, the scopes and the rotating refresh token change together.
      x.access_token = r.obj.access_token
      x.refresh_token = typeof r.obj.refresh_token === 'string' ? r.obj.refresh_token : x.refresh_token
      x.expires_in = typeof r.obj.expires_in === 'number' ? r.obj.expires_in : 3600
      if (typeof r.obj.scope === 'string') x.scopes = scopesOf(r.obj.scope)
      if (typeof r.obj.id_token === 'string') x.id_token = r.obj.id_token
      x.saved_at = new Date().toISOString()
      this.save(s2)
      if (!planEnabled(x)) throw new ChatGPTError('planNotEnabled')
      return { clientId: id, value: x.access_token! }
    })
  }

  // ---- ID token ------------------------------------------------------------------

  /** Verifies the RS256 signature with the issuer's JWKS, then the issuer,
   *  audience (the issued client ID), expiry and nonce. */
  async verify(idToken: string, audience: string, nonce: string): Promise<Record<string, unknown>> {
    const parts = idToken.split('.')
    let header: any, claims: any
    try {
      header = JSON.parse(Buffer.from(parts[0], 'base64url').toString())
      claims = JSON.parse(Buffer.from(parts[1], 'base64url').toString())
    } catch {
      throw new ChatGPTError('failed', 'the ID token is malformed')
    }
    if (parts.length !== 3 || header.alg !== 'RS256') throw new ChatGPTError('failed', 'the ID token is not signed with RS256')
    const jwks = (await (await fetch(this.endpoints.jwks, { signal: AbortSignal.timeout(REQUEST_TIMEOUT) })).json()) as { keys?: any[] }
    const jwk = (jwks.keys ?? []).find((k) => k.kty === 'RSA' && (!header.kid || k.kid === header.kid))
    if (!jwk) throw new ChatGPTError('failed', 'no key of the issuer verifies the ID token')
    const key = crypto.createPublicKey({ key: { kty: 'RSA', n: jwk.n, e: jwk.e }, format: 'jwk' })
    if (!crypto.verify('RSA-SHA256', Buffer.from(`${parts[0]}.${parts[1]}`), key, Buffer.from(parts[2], 'base64url'))) {
      throw new ChatGPTError('failed', "the ID token's signature is not valid")
    }
    if (claims.iss !== this.endpoints.issuer) throw new ChatGPTError('failed', 'the ID token has another issuer')
    const aud = Array.isArray(claims.aud) ? claims.aud : [claims.aud]
    if (!aud.includes(audience)) throw new ChatGPTError('failed', 'the ID token is for another client')
    if (typeof claims.exp !== 'number' || (claims.exp + 120) * 1000 < Date.now()) throw new ChatGPTError('failed', 'the ID token has expired')
    if (claims.nonce !== nonce) throw new ChatGPTError('failed', "the ID token's nonce does not match")
    return claims
  }
}

/** A failure of a ChatGPT plan request, by its documented error code: the
 *  usage limit stops the turn; an unavailable usage check or user is retried. */
export function planError(status: number, error: any, detail: string, requestId: string | null): Error {
  const code: string | undefined = error?.code
  const message: string = error?.message ?? detail
  const rid = requestId ? ` (request ${requestId})` : ''
  switch (code) {
    case 'subscription_sharing_usage_limit_exceeded': return new ChatGPTError('usageLimit')
    case 'subscription_sharing_usage_unavailable': case 'subscription_sharing_user_unavailable':
      return new TransientError(503, `${code}: ${message}${rid}`)
    case 'subscription_sharing_user_not_eligible': return new ChatGPTError('notEligible', message)
    case 'subscription_sharing_unsupported_capability': return new ChatGPTError('unsupported', error?.param ? `${error.param}: ${message}` : message)
    case undefined:
      if (status === 401) return new ChatGPTError('failed', `the account or its permission was not accepted (401): ${message}${rid}`)
      if (status === 403) return new ChatGPTError('failed', `blocked by a policy or permission check (403): ${message}${rid}`)
      return status >= 500 ? new TransientError(status, `${message}${rid}`) : new ChatGPTError('failed', `HTTP ${status}: ${message}${rid}`)
    default: return new ChatGPTError('failed', `${code}: ${message}${rid}`)
  }
}
