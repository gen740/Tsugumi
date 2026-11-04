// Chat, Agent and the settings it needs (skill folders, model connection,
// API key) for the host server. Routes are merged into the server's table.
import { execFile } from 'node:child_process'
import { randomUUID } from 'node:crypto'
import fs from 'node:fs'
import net from 'node:net'
import path from 'node:path'
import { BFBS } from '../src/shared/bfbs'
import { RPCClient, Transport } from '../src/shared/rpc'
import { Agent, AgentEvent, listModels, loadConversation, ProviderConfig, referenceDemos, ToolCatalog } from './agent'
import { ChatGPTAuth, ChatGPTError, endpointsFor, OPENAI_ENDPOINTS, USAGE_URL } from './chatgpt'
import { EnvironmentManager, projectKey } from './env'

export interface ChatContext {
  support: string
  envs: EnvironmentManager
  projectDir: (p: unknown) => string
  broadcast: (v: unknown) => void
  httpError: (status: number, message: string) => Error
  readJSON: <T>(file: string, fallback: T) => T
  writeJSON: (file: string, v: unknown) => void
  /** Folder of the server bundle (tsugumi.bfbs is next to it). */
  here: string
}

type Handler = (b: any, url: URL) => unknown | Promise<unknown>

let ctx: ChatContext
let catalog: ToolCatalog | undefined
let catalogError: string | undefined

export function chatStatus() {
  return { chatAvailable: !catalogError, chatError: catalogError ?? null }
}

type ProviderId = 'anthropic' | 'openai' | 'chatgpt' | 'fixture'

interface Settings {
  skillFolders: { path: string; enabled: boolean }[]
  provider: ProviderId
  /** The Anthropic model, the OpenAI one and the ChatGPT plan's (each provider keeps its own). */
  model: string
  openAIModel: string
  chatGPTModel: string
  fixturePort: number
  /** Models listed earlier with "モデル一覧を取得" (no request from the chat). */
  models: string[]
  /** Their display names (the ChatGPT account's catalog has them). */
  modelNames: Record<string, string>
  /** What the Agent may give Cards without asking (network, sandbox_off,
   *  host_folder_read, host_folder_write); the rest waits for the user. */
  allowedPermissions: string[]
}

export const PERMISSION_KINDS = ['network', 'sandbox_off', 'host_folder_read', 'host_folder_write']
const settingsFile = () => path.join(ctx.support, 'settings.json')
const defaultSettings: Settings = {
  skillFolders: [], provider: 'anthropic', model: 'claude-sonnet-5-5', openAIModel: 'gpt-6.1-sol', chatGPTModel: 'gpt-6.1-sol', fixturePort: 8765,
  models: [], modelNames: {}, allowedPermissions: [],
}
const settings = (): Settings => ({ ...defaultSettings, ...ctx.readJSON<Partial<Settings>>(settingsFile(), {}) })

/** Only these keys are written from the page (never a key or anything else). */
function cleanSettings(b: Record<string, unknown>): Partial<Settings> {
  const out: Partial<Settings> = {}
  if (Array.isArray(b.skillFolders)) {
    out.skillFolders = b.skillFolders.filter((f) => f && typeof f.path === 'string').map((f) => ({ path: String(f.path), enabled: f.enabled !== false }))
  }
  if (b.provider === 'anthropic' || b.provider === 'openai' || b.provider === 'chatgpt' || b.provider === 'fixture') out.provider = b.provider
  if (typeof b.model === 'string') out.model = b.model.trim()
  if (typeof b.openAIModel === 'string') out.openAIModel = b.openAIModel.trim()
  if (typeof b.chatGPTModel === 'string') out.chatGPTModel = b.chatGPTModel.trim()
  if (typeof b.fixturePort === 'number' && Number.isInteger(b.fixturePort) && b.fixturePort > 0 && b.fixturePort < 65536) out.fixturePort = b.fixturePort
  if (Array.isArray(b.models)) out.models = b.models.filter((m): m is string => typeof m === 'string')
  const allowed = b.allowedPermissions
  if (Array.isArray(allowed)) out.allowedPermissions = PERMISSION_KINDS.filter((k) => allowed.includes(k))
  return out
}

// Model credentials stay on this host, one Keychain item per provider (the
// same items as the macOS app: service app.tsugumi.agent, account = the
// provider's name). They are never sent to the page, tsugumid or Card
// workers, and never written to project files, settings or logs.
const KEYCHAIN_SERVICE = 'app.tsugumi.agent'
const PROVIDERS = {
  anthropic: { name: 'Anthropic', env: 'ANTHROPIC_API_KEY', file: 'anthropic-key' },
  openai: { name: 'OpenAI', env: 'OPENAI_API_KEY', file: 'openai-key' },
} as const
type KeyedProvider = keyof typeof PROVIDERS
const keyFile = (p: KeyedProvider) => path.join(ctx.support, PROVIDERS[p].file)

function run(cmd: string, args: string[], input?: string): Promise<{ code: number; out: string }> {
  return new Promise((resolve) => {
    const c = execFile(cmd, args, (err, stdout) => resolve({ code: err ? (typeof err.code === 'number' ? err.code : 1) : 0, out: stdout.trim() }))
    if (input !== undefined) c.stdin?.end(input)
  })
}

async function keychainKey(p: KeyedProvider): Promise<string | undefined> {
  if (process.platform !== 'darwin') return undefined
  const r = await run('security', ['find-generic-password', '-s', KEYCHAIN_SERVICE, '-a', PROVIDERS[p].name, '-w'])
  return r.code === 0 && r.out ? r.out : undefined
}

async function apiKey(p: KeyedProvider): Promise<string | undefined> {
  const env = process.env[PROVIDERS[p].env]
  if (env) return env
  const k = await keychainKey(p)
  if (k) return k
  try {
    return fs.readFileSync(keyFile(p), 'utf8').trim() || undefined
  } catch {
    return undefined
  }
}

/** Where a provider's key is (for the settings page; never the key itself). */
async function keySource(p: KeyedProvider): Promise<'env' | 'keychain' | 'file' | null> {
  if (process.env[PROVIDERS[p].env]) return 'env'
  if (await keychainKey(p)) return 'keychain'
  return fs.existsSync(keyFile(p)) ? 'file' : null
}

async function saveKey(p: KeyedProvider, raw: string) {
  const key = raw.trim()
  // An API key is printable ASCII without spaces or quotes.
  if (!/^[\x21-\x7e]+$/.test(key) || /["\\]/.test(key)) throw ctx.httpError(400, 'API キーに使えない文字が含まれています。')
  if (process.platform === 'darwin') {
    // The secret goes to `security` on its standard input, never as an argument.
    const r = await run('security', ['-i'], `add-generic-password -U -s "${KEYCHAIN_SERVICE}" -a "${PROVIDERS[p].name}" -w "${key}"\n`)
    if (r.code === 0 && (await keychainKey(p)) === key) {
      fs.rmSync(keyFile(p), { force: true })
      return 'Keychain に保存しました。キーは core や Card には渡されず、プロジェクト・設定・ログにも残りません。'
    }
  }
  fs.mkdirSync(ctx.support, { recursive: true })
  fs.writeFileSync(keyFile(p), key, { mode: 0o600 })
  fs.chmodSync(keyFile(p), 0o600)
  return `${keyFile(p)}（0600）に保存しました。キーは core や Card には渡されず、プロジェクト・設定・ログにも残りません。`
}

async function deleteKey(p: KeyedProvider) {
  if (process.platform === 'darwin') await run('security', ['delete-generic-password', '-s', KEYCHAIN_SERVICE, '-a', PROVIDERS[p].name])
  fs.rmSync(keyFile(p), { force: true })
  return `${PROVIDERS[p].name} のキーを削除しました。` + (process.env[PROVIDERS[p].env] ? `（環境変数 ${PROVIDERS[p].env} は残っています）` : '')
}

// Sign in with ChatGPT: this server's registrations and tokens, in a 0600
// file of its data folder. TSUGUMI_CHATGPT_ISSUER and TSUGUMI_CHATGPT_API
// point the sign-in and the requests at the local fixture (tests only).
let chatGPTAuth: ChatGPTAuth | undefined
const chatgpt = () => (chatGPTAuth ??= new ChatGPTAuth(path.join(ctx.support, 'chatgpt.json'),
  process.env.TSUGUMI_CHATGPT_ISSUER ? endpointsFor(process.env.TSUGUMI_CHATGPT_ISSUER, OPENAI_ENDPOINTS.resource) : OPENAI_ENDPOINTS))
/** The last sign-in started from the page: its outcome, once it came. */
let signInOutcome: { ok: boolean; message: string; welcome?: string } | null = null

async function provider(): Promise<ProviderConfig> {
  const s = settings()
  if (s.provider === 'fixture') {
    return { name: 'Local fixture', baseURL: `http://127.0.0.1:${s.fixturePort}`, model: 'fixture', api: 'anthropicMessages', auth: 'none', maxTokens: 32768, contextLimit: 6000 }
  }
  if (s.provider === 'chatgpt') {
    return { name: 'ChatGPT', baseURL: process.env.TSUGUMI_CHATGPT_API ?? 'https://api.openai.com', model: s.chatGPTModel || defaultSettings.chatGPTModel,
             api: 'openAIResponses', auth: 'chatgpt', token: (stale) => chatgpt().accessToken(stale), maxTokens: 32768, contextLimit: 180_000 }
  }
  if (s.provider === 'openai') {
    return { name: 'OpenAI', baseURL: 'https://api.openai.com', model: s.openAIModel || defaultSettings.openAIModel, api: 'openAIResponses', auth: 'apiKey', apiKey: await apiKey('openai'), maxTokens: 32768, contextLimit: 180_000 }
  }
  return { name: 'Anthropic', baseURL: 'https://api.anthropic.com', model: s.model || defaultSettings.model, api: 'anthropicMessages', auth: 'apiKey', apiKey: await apiKey('anthropic'), maxTokens: 32768, contextLimit: 180_000 }
}

/** What the page may know of the connection (no key, no token). */
async function providerInfo() {
  const p = await provider()
  return { name: p.name, model: p.model, api: p.api, remote: p.auth !== 'none', plan: p.auth === 'chatgpt',
           modelNames: p.auth === 'chatgpt' ? settings().modelNames : {} }
}

/** The ChatGPT accounts and the sign-in for the settings page (never a token). */
function chatGPTState() {
  return { accounts: chatgpt().accounts(), signingIn: chatgpt().signingIn, outcome: signInOutcome, usageURL: USAGE_URL }
}

/** The references the Agent may read (reference_read): the repository's
 *  demos that are not Draft tasks or their answers, the Python SDK and the Card guide (dist/ is two levels below). */
function references(): Record<string, string> {
  const repo = path.resolve(ctx.here, '..', '..')
  const out: Record<string, string> = {}
  const add = (name: string, p: string) => {
    if (fs.existsSync(p)) out[name] = p
  }
  // Not the Draft demos nor their answers (referenceDemos).
  Object.assign(out, referenceDemos(path.join(repo, 'demos')))
  add('sdk/python/tsugumi/__init__.py', path.join(repo, 'sdk', 'python', 'tsugumi', '__init__.py'))
  add('card-guide.md', path.join(repo, 'agent', 'Reference', 'card-guide.md'))
  return out
}

/** Attached folders as sent by the page: existing folders of this host. */
function cleanFolders(v: unknown): string[] {
  if (!Array.isArray(v)) return []
  return [...new Set(v.filter((f): f is string => typeof f === 'string' && path.isAbsolute(f)).map((f) => path.resolve(f)))].filter((f) => {
    try {
      return fs.statSync(f).isDirectory()
    } catch {
      return false
    }
  })
}

// ---- Skills ----------------------------------------------------------------------

interface Skill { name: string; description: string; path: string; content: string; source: string }

/** Reads skills from a folder: `<name>/SKILL.md` or `<name>.md`. */
function loadSkills(folder: string, source: string): Skill[] {
  let items: fs.Dirent[]
  try {
    items = fs.readdirSync(folder, { withFileTypes: true })
  } catch {
    return []
  }
  const out: Skill[] = []
  for (const item of items.sort((a, b) => a.name.localeCompare(b.name))) {
    let file = path.join(folder, item.name)
    let name = item.name.replace(/\.[^.]+$/, '')
    if (item.isDirectory()) {
      file = path.join(file, 'SKILL.md')
      name = item.name
    } else if (!item.name.toLowerCase().endsWith('.md')) continue
    let text: string
    try {
      if (fs.statSync(file).size >= 512 * 1024) continue
      text = fs.readFileSync(file, 'utf8')
    } catch {
      continue
    }
    let description = ''
    let lines = text.split('\n')
    if (lines[0] === '---') {
      const end = lines.indexOf('---', 1)
      if (end > 0) {
        for (const l of lines.slice(1, end)) {
          if (l.startsWith('name:')) name = l.slice(5).trim()
          if (l.startsWith('description:')) description = l.slice(12).trim()
        }
        lines = lines.slice(end + 1)
      }
    }
    if (!description) description = (lines.find((l) => l.trim()) ?? '').replace(/^[#\s]+|[#\s]+$/g, '')
    out.push({ name: name.replace(/[^\p{L}\p{N}_-]/gu, '-'), description, path: file, content: text, source })
  }
  return out
}

function skillsFor(project: string | undefined): Skill[] {
  const out = project ? loadSkills(path.join(project, 'skills'), 'プロジェクト') : []
  for (const f of settings().skillFolders) if (f.enabled) out.push(...loadSkills(f.path, path.basename(f.path)))
  return out
}


// ---- Agent hosting -------------------------------------------------------------

/** Unix socket transport: 4 byte little-endian length + FlatBuffer. */
function unixTransport(sock: string): Promise<Transport> {
  return new Promise((resolve, reject) => {
    const c = net.connect(sock)
    let buf = Buffer.alloc(0)
    const t: Transport = {
      send: (f) => {
        const head = Buffer.alloc(4)
        head.writeUInt32LE(f.length)
        c.write(Buffer.concat([head, Buffer.from(f)]))
      },
      close: () => c.destroy(),
    }
    c.on('connect', () => resolve(t))
    c.on('error', (e) => reject(e))
    c.on('data', (d) => {
      buf = Buffer.concat([buf, d])
      while (buf.length >= 4) {
        const n = buf.readUInt32LE(0)
        if (buf.length < 4 + n) break
        t.onFrame?.(new Uint8Array(buf.subarray(4, 4 + n)))
        buf = buf.subarray(4 + n)
      }
    })
    c.on('close', () => t.onClose?.('closed'))
  })
}

/** A save of an Agent waiting for the user: what it would give Cards. */
interface PermissionWait {
  project: string
  session: string
  asked: string[]
  resolve: (approved: Set<string>) => void
}
const permissionWaits = new Map<string, PermissionWait>()

/** The widenings the user approves: the kinds allowed without asking at
 *  once, the others when the page asks (nothing when declined or cancelled). */
function approve(project: string, session: string, asked: string[]): Promise<Set<string>> {
  const allowed = new Set(settings().allowedPermissions)
  const approved = new Set(asked.filter((p) => allowed.has(p.split(' ')[0])))
  const ask = asked.filter((p) => !approved.has(p))
  if (!ask.length) return Promise.resolve(approved)
  return new Promise((resolve) => {
    const id = randomUUID()
    permissionWaits.set(id, { project, session, asked: ask, resolve: (a) => resolve(new Set([...approved, ...a])) })
    ctx.broadcast({ type: 'chat', project, session, event: { kind: 'permission', id, asked: ask } })
  })
}

function answerPermission(id: string, allow: boolean, always: string[] = []) {
  const w = permissionWaits.get(id)
  if (!w) return
  permissionWaits.delete(id)
  if (allow && always.length) {
    const s = settings()
    ctx.writeJSON(settingsFile(), { ...s, allowedPermissions: PERMISSION_KINDS.filter((k) => s.allowedPermissions.includes(k) || always.includes(k)) })
  }
  w.resolve(allow ? new Set(w.asked) : new Set())
  ctx.broadcast({ type: 'chat', project: w.project, session: w.session, event: { kind: 'permissionAnswered', id } })
}

const waitingFor = (project: string, session: string) =>
  [...permissionWaits.entries()].filter(([, w]) => w.project === project && w.session === session)

class ChatHost {
  private agents = new Map<string, Agent>()
  private client?: RPCClient
  readonly dir: string

  constructor(readonly project: string, readonly key: string) {
    this.dir = path.join(ctx.support, 'agent', key, 'sessions')
  }

  /** One authenticated connection per project, reopened after a disconnect. */
  async rpc(): Promise<RPCClient> {
    if (this.client && !this.client.closed) return this.client
    const st = await ctx.envs.start(this.project)
    if (st.state !== 'ready') throw new Error(st.state === 'failed' ? st.message : 'tsugumid is not running')
    const c = new RPCClient(await unixTransport(st.unix))
    await c.connect(undefined)
    this.client = c
    return c
  }

  file(id: string) {
    return path.join(this.dir, `${id.replace(/[^a-zA-Z0-9-]/g, '')}.json`)
  }

  async agent(id: string): Promise<Agent> {
    let a = this.agents.get(id)
    if (!a) {
      a = new Agent(catalog!, () => this.rpc(), await provider(), this.file(id), id, references())
      a.approver = (asked) => approve(this.project, id, asked)
      this.agents.set(id, a)
    }
    return a
  }

  /** The folders a chat may read; tsugumid on this host reads only those
   *  listed in its control folder (attached.json), written here alone. */
  attach(a: Agent, folders: string[]) {
    a.folders = folders
    const all = [...new Set([...this.agents.values()].flatMap((x) => x.folders))].sort()
    const control = ctx.envs.controlDir(this.key)
    fs.mkdirSync(control, { recursive: true, mode: 0o700 })
    const file = path.join(control, 'attached.json')
    fs.writeFileSync(file + '.tmp', JSON.stringify({ folders: all }), { mode: 0o600 })
    fs.renameSync(file + '.tmp', file)
  }

  events(id: string) {
    return (e: AgentEvent) => ctx.broadcast({ type: 'chat', project: this.project, session: id, event: e })
  }

  sessions() {
    let files: string[] = []
    try {
      files = fs.readdirSync(this.dir).filter((f) => f.endsWith('.json'))
    } catch {}
    return files.map((f) => {
      const id = f.slice(0, -5)
      let c
      try { c = this.agents.get(id)?.conversation ?? loadConversation(path.join(this.dir, f), id) }
      catch { return { id, title: `読み込めない会話 (${id})`, updatedAt: 0, messages: 0 } }
      const m = [...c.archive, ...c.messages].find((m) => m.role === 'user')
      const first = m?.display ?? m?.content.flatMap((b) => (b.type === 'text' ? [b.text] : []))[0]
      return { id, title: c.title ?? (first ? first.slice(0, 60) : '新しい会話'), updatedAt: c.updatedAt, messages: c.archive.length + c.messages.length }
    }).sort((a, b) => b.updatedAt - a.updatedAt)
  }

  delete(id: string) {
    this.agents.get(id)?.cancel()
    this.agents.delete(id)
    fs.rmSync(this.file(id), { force: true })
    // (and its helpers' conversations, from delegate)
    fs.rmSync(this.file(id).replace(/\.json$/, '') + '.helpers', { recursive: true, force: true })
  }

  /** Full-text search over every session, compacted parts included. */
  search(text: string) {
    const q = text.toLowerCase()
    const hits: { session: string; title: string; snippet: string }[] = []
    for (const s of this.sessions()) {
      let c
      try { c = loadConversation(this.file(s.id), s.id) } catch { continue }
      for (const m of [...c.archive, ...c.messages]) {
        for (const b of m.content) {
          if (b.type !== 'text') continue
          const i = b.text.toLowerCase().indexOf(q)
          if (i < 0) continue
          hits.push({ session: s.id, title: s.title, snippet: b.text.slice(Math.max(0, i - 40), i + q.length + 60).replace(/\n/g, ' ') })
        }
      }
    }
    return hits
  }
}

const chats = new Map<string, ChatHost>()
function chatFor(dir: string): ChatHost {
  const { key, real } = projectKey(dir)
  let c = chats.get(key)
  if (!c) {
    c = new ChatHost(real, key)
    chats.set(key, c)
  }
  return c
}


const keyedProvider = (v: unknown): KeyedProvider => (v === 'openai' ? 'openai' : 'anthropic')

/** The Agent of a session, ready for a new turn (current connection and folders). */
async function turnAgent(b: any) {
  if (!catalog) throw ctx.httpError(503, catalogError!)
  const host = chatFor(ctx.projectDir(b.path))
  const a = await host.agent(String(b.session))
  a.setProvider(await provider())
  if (b.folders) host.attach(a, cleanFolders(b.folders))
  return { host, a }
}

export function chatRoutes(c: ChatContext): Record<string, Handler> {
  ctx = c
  try {
    catalog = new ToolCatalog(new BFBS(new Uint8Array(fs.readFileSync(path.join(c.here, 'tsugumi.bfbs')))))
  } catch (e) {
    catalogError = `RPC スキーマを読み込めないため、チャットは使えません（${(e as Error).message}）。通常の操作はすべて UI から行えます。`
  }
  const projectDir = c.projectDir
  return {
  'POST /api/skills': (b) => skillsFor(b.path ? projectDir(b.path) : undefined),
  'GET /api/settings': async () => ({
    ...settings(),
    keys: { Anthropic: await keySource('anthropic'), OpenAI: await keySource('openai') },
    keyEnv: { Anthropic: PROVIDERS.anthropic.env, OpenAI: PROVIDERS.openai.env },
  }),
  'POST /api/settings': (b) => {
    const next = { ...settings(), ...cleanSettings(b ?? {}) }
    ctx.writeJSON(settingsFile(), next)
    return next
  },
  'POST /api/settings/key': async (b) => ({ message: await saveKey(keyedProvider(b.provider), String(b.key ?? '')) }),
  'POST /api/settings/key/delete': async (b) => ({ message: await deleteKey(keyedProvider(b.provider)) }),
  'POST /api/settings/models': async () => {
    const list = await listModels(await provider())
    const models = list.map((m) => m.id)
    const modelNames = Object.fromEntries(list.map((m) => [m.id, m.label]))
    ctx.writeJSON(settingsFile(), { ...settings(), models, modelNames })
    return { models, modelNames }
  },
  'GET /api/chatgpt': () => chatGPTState(),
  // Starts a sign-in: the page opens the URL in a tab; the outcome is read back with GET /api/chatgpt.
  'POST /api/chatgpt/signin': async (b) => {
    const { url, done } = await chatgpt().begin(typeof b.clientId === 'string' ? b.clientId : undefined, b.consent === true)
    signInOutcome = null
    done.then((r) => {
      signInOutcome = { ok: true, welcome: r.welcome ? r.account.clientId : undefined, message: r.account.planEnabled
        ? `${r.account.email ?? r.account.clientId} でサインインしました。`
        : 'サインインしましたが、ChatGPT プランの利用は許可されませんでした。「プランの利用を許可」から許可するか、API キーの接続を使ってください。' }
    }, (e: Error) => {
      signInOutcome = { ok: false, message: e.message === new ChatGPTError('failed', 'cancelled').message ? 'サインインを中止しました。' : e.message }
    })
    return { url }
  },
  'POST /api/chatgpt/cancel': () => {
    chatgpt().cancel()
    return chatGPTState()
  },
  'POST /api/chatgpt/select': async (b) => {
    await chatgpt().select(String(b.clientId))
    return chatGPTState()
  },
  'POST /api/chatgpt/welcomed': async (b) => {
    await chatgpt().markWelcomed(String(b.clientId))
    signInOutcome = signInOutcome && { ...signInOutcome, welcome: undefined }
    return chatGPTState()
  },
  'POST /api/chatgpt/signout': async (b) => {
    const id = String(b.clientId)
    const email = chatgpt().accounts().find((a) => a.clientId === id)?.email ?? id
    const revoked = await chatgpt().signOut(id)
    return { ...chatGPTState(), message: revoked ? `${email} からサインアウトしました。`
      : 'このホストのトークンは消しましたが、OpenAI 側での取り消しを確かめられませんでした。ChatGPT の設定で Tsugumi の接続を解除できます。' }
  },
  'POST /api/settings/skillCount': (b) => ({ count: loadSkills(String(b.path), '').length }),
  'POST /api/settings/skillFolderStatus': (b) => {
    const list = Array.isArray(b.paths) ? b.paths.map(String) : []
    return Object.fromEntries(list.map((p: string) => [p, fs.existsSync(p)]))
  },
  'POST /api/chat/sessions': (b) => chatFor(projectDir(b.path)).sessions(),
  'POST /api/chat/conversation': async (b) => {
    if (!catalog) throw ctx.httpError(503, catalogError!)
    const host = chatFor(projectDir(b.path))
    const a = await host.agent(String(b.session))
    if (b.folders) host.attach(a, cleanFolders(b.folders))
    // Resume work left unfinished by a disconnect or restart.
    if (b.resume && !a.busy) {
      a.setProvider(await provider())
      a.resume(host.events(a.id))
    }
    const [wait] = waitingFor(host.project, a.id)
    return { conversation: a.conversation, busy: a.busy, provider: await providerInfo(),
             permission: wait ? { id: wait[0], asked: wait[1].asked } : null }
  },
  'POST /api/chat/send': async (b) => {
    const { host, a } = await turnAgent(b)
    a.send(String(b.prompt), String(b.title), host.events(a.id), typeof b.display === 'string' ? b.display : undefined)
    return { ok: true }
  },
  'POST /api/chat/continue': async (b) => {
    const { host, a } = await turnAgent(b)
    a.continueAfterLimit(host.events(a.id))
    return { ok: true }
  },
  'POST /api/chat/retry': async (b) => {
    const { host, a } = await turnAgent(b)
    a.retry(host.events(a.id))
    return { ok: true }
  },
  'POST /api/chat/compact': async (b) => {
    const { host, a } = await turnAgent(b)
    a.compact(host.events(a.id))
    return { ok: true }
  },
  'POST /api/chat/permission': (b) => {
    answerPermission(String(b.id), b.allow === true, Array.isArray(b.always) ? b.always.map(String) : [])
    return { ok: true }
  },
  'POST /api/chat/cancel': async (b) => {
    const host = chatFor(projectDir(b.path))
    for (const [id] of waitingFor(host.project, String(b.session))) answerPermission(id, false)
    ;(await host.agent(String(b.session))).cancel()
    return { ok: true }
  },
  'POST /api/chat/delete': (b) => {
    chatFor(projectDir(b.path)).delete(String(b.session))
    return { ok: true }
  },
  'POST /api/chat/folder': (b) => {
    // A folder to attach: it must be an existing folder of this host.
    const [f] = cleanFolders([b.folder])
    if (!f) throw ctx.httpError(400, `フォルダが見つかりません: ${b.folder}`)
    return { folder: f, name: path.basename(f) }
  },
  'POST /api/chat/search': (b) => chatFor(projectDir(b.path)).search(String(b.text)),
  }
}
