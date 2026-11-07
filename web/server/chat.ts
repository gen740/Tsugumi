// Chat, Agent and the settings it needs (skill folders, model connection,
// API key) for the host server. Routes are merged into the server's table.
import { execFile } from 'node:child_process'
import { randomUUID } from 'node:crypto'
import fs from 'node:fs'
import net from 'node:net'
import path from 'node:path'
import { AGENT, modelChoices } from '../src/shared/agentShared'
import { BFBS } from '../src/shared/bfbs'
import { RPCClient, Transport, webSocketTransport } from '../src/shared/rpc'
import { Agent, externalTools, summarizeVersion, AgentEvent, DEMO_SKILL, guideText, listModels, loadConversation, ProviderConfig, referenceDemos, ToolCatalog } from './agent'
import { keychainReader } from './keychain'
import { withModelList } from './modelList'
import { ChatGPTAuth, ChatGPTError, endpointsFor, OPENAI_ENDPOINTS, USAGE_URL } from './chatgpt'
import { Environments, projectKey } from './env'
import { DesktopSettings } from './desktop'

export interface ChatContext {
  support: string
  envs: Environments
  /** The desktop app's settings, when this server shares them (the Mac's tsugumi command). */
  desktop?: DesktopSettings
  projectDir: (p: unknown) => string
  broadcast: (v: unknown) => void
  httpError: (status: number, message: string) => Error
  readJSON: <T>(file: string, fallback: T) => T
  writeJSON: (file: string, v: unknown) => void
  /** Folder of the server bundle (tsugumi.bfbs is next to it). */
  here: string
  /** How many pages are connected (their event streams). */
  listeners: () => number
  /** Opens the page in the default browser (the page's key is the server's alone). */
  showOnPage: (project: string) => boolean
  openPage: (project?: string) => void
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
  /** The chat's model menu: the effort ('': the model's default), for every
   *  model that takes it, and the one model Fast mode was turned on for. */
  effort: string
  fastModel: string
  fixturePort: number
  /** Models listed earlier with "モデル一覧を取得" (no request from the chat). */
  /** Models listed with "モデル一覧を取得", per connection (no request from the chat). */
  models: Partial<Record<'anthropic' | 'openai' | 'chatgpt', string[]>>
  /** Their display names (the ChatGPT account's catalog has them). */
  modelNames: Record<string, string>
  /** What the Agent may give Cards without asking (network, sandbox_off,
   *  host_folder_read, host_folder_write); the rest waits for the user. */
  allowedPermissions: string[]
  /** The chat's Agent may search the web through the provider (its queries
   *  leave for the provider's search service). Off until the user turns it on. */
  webSearch: boolean
  /** The guide bird: the first start answered, the demo offer answered, the demo being shown. */
  guide: GuideState
  /** A small model writes what each version changed (off: nothing is sent for it). */
  versionSummaries: boolean
}

/** The demo the guide shows: its id and title, the copy's folder, and
 *  whether the guide began it. */
interface GuideTour { demo: string; title: string; path: string; started: boolean }
interface GuideState { setupDone: boolean; demoAnswered: boolean; tour: GuideTour | null }

export const PERMISSION_KINDS = ['network', 'sandbox_off', 'host_folder_read', 'host_folder_write']
const settingsFile = () => path.join(ctx.support, 'settings.json')
const defaultSettings: Settings = {
  skillFolders: [], provider: 'anthropic', model: 'claude-sonnet-5-5', openAIModel: 'gpt-6.1-sol', chatGPTModel: 'gpt-6.1-sol', effort: '', fastModel: '', fixturePort: 8765,
  models: {}, modelNames: {}, allowedPermissions: [], webSearch: false,
  guide: { setupDone: false, demoAnswered: false, tour: null },
  versionSummaries: false,
}
const settings = (): Settings => ({ ...defaultSettings, ...ctx.readJSON<Partial<Settings>>(settingsFile(), {}), ...ctx.desktop?.values })
const saveSettings = (s: Settings) => {
  ctx.desktop?.save(settings(), s)
  ctx.writeJSON(settingsFile(), s)
}

/** Only these keys are written from the page (never a key or anything else). */
function cleanSettings(b: Record<string, unknown>): Partial<Settings> {
  const out: Partial<Settings> = {}
  if (Array.isArray(b.skillFolders)) {
    out.skillFolders = b.skillFolders.filter((f) => f && typeof f.path === 'string').map((f) => ({ path: String(f.path), enabled: f.enabled !== false }))
  }
  if (b.provider === 'anthropic' || b.provider === 'openai' || b.provider === 'chatgpt' || b.provider === 'fixture') out.provider = b.provider
  if (typeof b.versionSummaries === 'boolean') out.versionSummaries = b.versionSummaries
  if (typeof b.model === 'string') out.model = b.model.trim()
  if (typeof b.openAIModel === 'string') out.openAIModel = b.openAIModel.trim()
  if (typeof b.chatGPTModel === 'string') out.chatGPTModel = b.chatGPTModel.trim()
  if (b.effort === '' || AGENT.modelOptions.efforts.includes(b.effort as string)) out.effort = b.effort as string
  if (typeof b.fastModel === 'string') out.fastModel = b.fastModel
  if (typeof b.fixturePort === 'number' && Number.isInteger(b.fixturePort) && b.fixturePort > 0 && b.fixturePort < 65536) out.fixturePort = b.fixturePort
  if (b.models && typeof b.models === 'object' && !Array.isArray(b.models)) {
    const m = b.models as Record<string, unknown>
    out.models = Object.fromEntries((['anthropic', 'openai', 'chatgpt'] as const).filter((k) => Array.isArray(m[k]))
      .map((k) => [k, (m[k] as unknown[]).filter((x): x is string => typeof x === 'string')]))
  }
  const allowed = b.allowedPermissions
  if (Array.isArray(allowed)) out.allowedPermissions = PERMISSION_KINDS.filter((k) => allowed.includes(k))
  if (typeof b.webSearch === 'boolean') out.webSearch = b.webSearch
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

const keychain = keychainReader(run, KEYCHAIN_SERVICE)

async function keychainKey(p: KeyedProvider): Promise<string | undefined> {
  if (process.platform !== 'darwin') return undefined
  return keychain.read(PROVIDERS[p].name)
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
  if (process.platform === 'darwin' && (await keychain.exists(PROVIDERS[p].name))) return 'keychain'
  return fs.existsSync(keyFile(p)) ? 'file' : null
}

async function saveKey(p: KeyedProvider, raw: string) {
  const key = raw.trim()
  // An API key is printable ASCII without spaces or quotes.
  if (!/^[\x21-\x7e]+$/.test(key) || /["\\]/.test(key)) throw ctx.httpError(400, 'API キーに使えない文字が含まれています。')
  if (process.platform === 'darwin') {
    // The secret goes to `security` on its standard input, never as an argument.
    const r = await run('security', ['-i'], `add-generic-password -U -s "${KEYCHAIN_SERVICE}" -a "${PROVIDERS[p].name}" -w "${key}"\n`)
    keychain.forget(PROVIDERS[p].name)
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
  if (process.platform === 'darwin') {
    await run('security', ['delete-generic-password', '-s', KEYCHAIN_SERVICE, '-a', PROVIDERS[p].name])
    keychain.forget(PROVIDERS[p].name)
  }
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
  return providerFor(settings().provider)
}

/** The connection of the chat's Agent and its helpers: with the effort and the
 *  Fast mode chosen in the model menu. The guide, the summaries and the checks
 *  use provider(), without them. */
async function chatProvider(): Promise<ProviderConfig> {
  const p = await provider(), s = settings()
  return { ...p, ...modelChoices(s.provider, p.model, s.effort, s.fastModel) }
}

/** A connection with its chosen model (each keeps its own); its key or token when it has one. */
async function providerFor(id: ProviderId): Promise<ProviderConfig> {
  const s = { ...settings(), provider: id }
  if (s.provider === 'fixture') {
    return { name: 'Local fixture', baseURL: `http://127.0.0.1:${s.fixturePort}`, model: 'fixture', api: 'anthropicMessages', auth: 'none', maxTokens: 32768, contextLimit: 6000 }
  }
  if (s.provider === 'chatgpt') {
    return { name: 'ChatGPT', baseURL: process.env.TSUGUMI_CHATGPT_API ?? 'https://api.openai.com', model: s.chatGPTModel || defaultSettings.chatGPTModel,
             api: 'openAIResponses', auth: 'chatgpt', token: (stale) => chatgpt().accessToken(stale), maxTokens: 32768, contextLimit: 180_000, webSearch: s.webSearch }
  }
  if (s.provider === 'openai') {
    return { name: 'OpenAI', baseURL: 'https://api.openai.com', model: s.openAIModel || defaultSettings.openAIModel, api: 'openAIResponses', auth: 'apiKey', apiKey: await apiKey('openai'), maxTokens: 32768, contextLimit: 180_000, webSearch: s.webSearch }
  }
  return { name: 'Anthropic', baseURL: 'https://api.anthropic.com', model: s.model || defaultSettings.model, api: 'anthropicMessages', auth: 'apiKey', apiKey: await apiKey('anthropic'), maxTokens: 32768, contextLimit: 180_000, webSearch: s.webSearch }
}

/** What the page may know of the connection (no key, no token). */
async function providerInfo() {
  const p = await chatProvider()
  return { name: p.name, model: p.model, api: p.api, remote: p.auth !== 'none', plan: p.auth === 'chatgpt', effort: p.effort, fast: !!p.fast,
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

// ---- The guide -----------------------------------------------------------------

// The guide bird in the bottom-right corner: one conversation for the whole
// app with the same model connection as the Agent (no key, no guide). It
// reads the project on the page's screen and explains; what it offers to do
// (guide_show) the user does with a button of the page.
let guideAgent: Agent | undefined
/** The project the page asked about (its reads go there), if one is open. */
let guideProject: string | undefined
const guideFile = () => path.join(ctx.support, 'agent', 'guide', 'conversation.json')
const guideEvents = (e: AgentEvent) => ctx.broadcast({ type: 'guide', event: e })

async function guide(): Promise<Agent> {
  if (!catalog) throw ctx.httpError(503, catalogError!)
  if (!guideAgent) {
    const rpc = () => {
      if (!guideProject) throw new Error(guideText('noProject'))
      return chatFor(guideProject).rpc()
    }
    guideAgent = new Agent(catalog, rpc, await guideProvider(), guideFile(), 'guide', references(), undefined, 'guide')
    // A guide answers in a few steps and never waits on a run.
    guideAgent.maxSteps = 12
    guideAgent.statusWait = 0
  }
  guideAgent.setProvider(await guideProvider())
  return guideAgent
}

/** The guide's connection: the Agent's, with the small model Tsugumi speaks
 *  with on the ChatGPT plan: gpt-6.1-luna when the account offers it, else
 *  its newest luna (the plan refuses models it does not offer). */
async function guideProvider(): Promise<ProviderConfig> {
  const p = await provider()
  return settings().provider === 'chatgpt' ? { ...p, model: speechModel(settings().models.chatgpt ?? [], p.model) } : p
}

export function speechModel(offered: string[], fallback: string): string {
  const preferred = 'gpt-6.1-luna'
  if (offered.includes(preferred)) return preferred
  const version = (m: string) => (m.match(/\d+/g) ?? []).map(Number)
  const newer = (a: number[], b: number[]) => { for (let i = 0; i < Math.max(a.length, b.length); i++) if ((a[i] ?? -1) !== (b[i] ?? -1)) return (a[i] ?? -1) > (b[i] ?? -1); return false }
  return offered.filter((m) => m.includes('luna')).reduce<string | undefined>((best, m) => (!best || newer(version(m), version(best)) ? m : best), undefined) ?? fallback
}

/** Whether the chosen provider can be used: a key (Anthropic, OpenAI), a
 *  signed-in ChatGPT plan, or the local fixture. */
async function guideAvailable(): Promise<boolean> {
  if (!catalog) return false
  const s = settings()
  if (s.provider === 'fixture') return true
  if (s.provider === 'chatgpt') return chatgpt().accounts().some((a) => a.active && a.signedIn && a.planEnabled)
  return !!(await keySource(s.provider))
}

/** The guide's state for the page; a key already there answers the first start. */
async function guideState() {
  const available = await guideAvailable()
  let g = settings().guide
  if (available && !g.setupDone) {
    g = { ...g, setupDone: true }
    saveSettings({ ...settings(), guide: g })
  }
  return { available, ...g }
}

function cleanTour(v: unknown): GuideTour | null {
  const t = v as Partial<GuideTour> | null
  if (!t || typeof t.demo !== 'string' || typeof t.title !== 'string' || typeof t.path !== 'string') return null
  return { demo: t.demo, title: t.title, path: t.path, started: t.started === true }
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

/** The built-in $demo: Tsugumi of the chat shows the screen around (it flies only then). */
const DEMO: Skill = { name: 'demo', description: 'ツグミが画面を飛び回って、デモや画面の使い方を案内します', path: 'builtin:demo', content: DEMO_SKILL, source: 'Tsugumi' }

function skillsFor(project: string | undefined): Skill[] {
  const out = [DEMO, ...(project ? loadSkills(path.join(project, 'skills'), 'プロジェクト') : [])]
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
function approve(project: string, session: string, asked: string[], from?: string): Promise<Set<string>> {
  const allowed = new Set(settings().allowedPermissions)
  const approved = new Set(asked.filter((p) => allowed.has(p.split(' ')[0])))
  const ask = asked.filter((p) => !approved.has(p))
  if (!ask.length) return Promise.resolve(approved)
  // An agent outside the web UI asks the page; with no page open nobody can answer.
  if (from && !ctx.listeners()) return Promise.resolve(approved)
  return new Promise((resolve) => {
    const id = randomUUID()
    permissionWaits.set(id, { project, session, asked: ask, resolve: (a) => resolve(new Set([...approved, ...a])) })
    ctx.broadcast({ type: 'chat', project, session, event: { kind: 'permission', id, asked: ask, from } })
  })
}

/** The session id under which Claude Code's calls (the plugin) ask for approvals. */
const EXTERNAL_SESSION = 'external'

function answerPermission(id: string, allow: boolean, always: string[] = []) {
  const w = permissionWaits.get(id)
  if (!w) return
  permissionWaits.delete(id)
  if (allow && always.length) {
    const s = settings()
    saveSettings({ ...s, allowedPermissions: PERMISSION_KINDS.filter((k) => s.allowedPermissions.includes(k) || always.includes(k)) })
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
    // On this host its Unix socket, which needs no token; in the VM its WebSocket.
    const c = new RPCClient(st.unix ? await unixTransport(st.unix) : await webSocketTransport(st.endpoint))
    await c.connect(st.unix ? undefined : st.token)
    this.client = c
    return c
  }

  file(id: string) {
    return path.join(this.dir, `${id.replace(/[^a-zA-Z0-9-]/g, '')}.json`)
  }

  async agent(id: string): Promise<Agent> {
    let a = this.agents.get(id)
    if (!a) {
      a = new Agent(catalog!, () => this.rpc(), await chatProvider(), this.file(id), id, references())
      a.approver = (asked) => approve(this.project, id, asked)
      this.agents.set(id, a)
    }
    return a
  }

  /** The folders a chat may read. */
  async attach(a: Agent, folders: string[]) {
    a.folders = folders
    const all = [...new Set([...this.agents.values()].flatMap((x) => x.folders))].sort()
    await ctx.envs.attachFolders(this.project, this.key, all)
  }

  events(id: string) {
    return (e: AgentEvent) => ctx.broadcast({ type: 'chat', project: this.project, session: id, event: e })
  }

  /** The Agent that runs Claude Code's tool calls (the plugin): no model of
   *  its own, the same checks and approvals as the chat's. Its records stay
   *  out of the chat's sessions. */
  private outside?: Agent
  async external(): Promise<Agent> {
    if (!this.outside) {
      const file = path.join(ctx.support, 'agent', this.key, 'external.json')
      fs.mkdirSync(path.dirname(file), { recursive: true })
      const a = new Agent(catalog!, () => this.rpc(), await provider(), file, EXTERNAL_SESSION, references())
      a.approver = (asked) => approve(this.project, EXTERNAL_SESSION, asked, 'Claude Code')
      this.outside = a
    }
    return this.outside
  }

  /** Claude Code's tool calls, for the page (Tsugumi flies to the Card it works on). */
  externalEvents() {
    return (e: AgentEvent) => {
      if (e.kind === 'toolStarted' || e.kind === 'toolFinished') ctx.broadcast({ type: 'agentWork', project: this.project, from: 'Claude Code', event: e })
    }
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

  cancelAll() {
    for (const a of this.agents.values()) a.cancel()
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


/** The project was left: no turn of it goes on. */
export function cancelChats(dir: string) {
  chats.get(projectKey(dir).key)?.cancelAll()
}

const keyedProvider = (v: unknown): KeyedProvider => (v === 'openai' ? 'openai' : 'anthropic')

/** The Agent of a session, ready for a new turn (current connection and folders). */
async function turnAgent(b: any) {
  if (!catalog) throw ctx.httpError(503, catalogError!)
  const host = chatFor(ctx.projectDir(b.path))
  const a = await host.agent(String(b.session))
  a.setProvider(await chatProvider())
  if (b.folders) await host.attach(a, cleanFolders(b.folders))
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
    ...(await ctx.desktop?.refresh(), settings()),
    keys: { Anthropic: await keySource('anthropic'), OpenAI: await keySource('openai') },
    keyEnv: { Anthropic: PROVIDERS.anthropic.env, OpenAI: PROVIDERS.openai.env },
  }),
  'POST /api/settings': (b) => {
    const next = { ...settings(), ...cleanSettings(b ?? {}) }
    saveSettings(next)
    return next
  },
  // Only while switched on, and not on the ChatGPT plan (its allowance is for the Agent).
  'POST /api/versions/summarize': async (b) => {
    const s = settings()
    if (!s.versionSummaries) throw ctx.httpError(400, '版の要約はオフです。')
    if (s.provider === 'chatgpt') throw ctx.httpError(400, 'ChatGPT プランでは版の要約を作りません。')
    const c = await provider()
    // Room for the model's own reasoning on a large diff: the answer itself is cut to one line.
    const small = { ...c, maxTokens: 1500, model: s.provider === 'anthropic' ? 'claude-haiku-5-5' : c.model }
    const text = await summarizeVersion(small, String(b.input ?? ''), AbortSignal.timeout(60_000))
    if (!text) throw ctx.httpError(502, 'モデルが文を返しませんでした。')
    return { text }
  },
  'POST /api/settings/key': async (b) => ({ message: await saveKey(keyedProvider(b.provider), String(b.key ?? '')) }),
  'POST /api/settings/key/delete': async (b) => ({ message: await deleteKey(keyedProvider(b.provider)) }),
  /** Lists a connection's models (the one in use unless `provider` names another). */
  'POST /api/settings/models': async (b) => {
    const id: ProviderId = b.provider === 'anthropic' || b.provider === 'openai' || b.provider === 'chatgpt' ? b.provider : settings().provider
    if (id === 'fixture') throw ctx.httpError(400, 'fixture にはモデル一覧がありません。')
    const list = await listModels(await providerFor(id))
    // Read after the wait: the other connections' lists come at the same time.
    const next = withModelList(settings(), id, list)
    if (!next) throw ctx.httpError(502, 'モデル一覧が空でした。保存済みの一覧はそのままです。')
    saveSettings(next)
    return { models: next.models[id], modelNames: next.modelNames }
  },
  /** The connections set up (a key, a signed-in ChatGPT plan), for the chat's model menu. */
  'GET /api/settings/connections': async () => ({
    anthropic: !!(await keySource('anthropic')),
    openai: !!(await keySource('openai')),
    chatgpt: chatgpt().accounts().some((a) => a.active && a.signedIn && a.planEnabled),
  }),
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
    if (b.folders) await host.attach(a, cleanFolders(b.folders))
    // Resume work left unfinished by a disconnect or restart.
    if (b.resume && !a.busy) {
      a.setProvider(await chatProvider())
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
  // Claude Code (the plugin's MCP server) works on a project through these:
  // the chat's tools, run here with the same checks, and shown on the page.
  'POST /api/agent/tools': () => {
    if (!catalog) throw ctx.httpError(503, catalogError!)
    return externalTools(catalog).map((t) => ({ name: t.name, description: t.description, inputSchema: t.inputSchema }))
  },
  /** Claude Code opened a project: a page that has it or is on Home shows it (others keep
   *  their projects; none free: a browser opens one, unless `browser: false`). */
  'POST /api/agent/open': (b) => {
    const project = chatFor(projectDir(b.path)).project
    const shown = ctx.showOnPage(project)
    const opened = !shown && b.browser !== false
    if (opened) ctx.openPage(project)
    return { project, shown, opened }
  },
  'POST /api/agent/call': async (b) => {
    if (!catalog) throw ctx.httpError(503, catalogError!)
    const host = chatFor(projectDir(b.path))
    const input = typeof b.arguments === 'string' ? b.arguments : JSON.stringify(b.arguments ?? {})
    return (await host.external()).call(String(b.name), input, host.externalEvents())
  },
  'POST /api/guide': async () => {
    const state = await guideState()
    const a = state.available ? await guide() : undefined
    return { ...state, conversation: a?.conversation ?? null, busy: a?.busy ?? false }
  },
  'POST /api/guide/state': async (b) => {
    const g = { ...settings().guide }
    if (typeof b.setupDone === 'boolean') g.setupDone = b.setupDone
    if (typeof b.demoAnswered === 'boolean') g.demoAnswered = b.demoAnswered
    if ('tour' in (b ?? {})) g.tour = cleanTour(b.tour)
    saveSettings({ ...settings(), guide: g })
    return guideState()
  },
  /** A connection check for the first start: the provider lists its models with the key. */
  'POST /api/guide/check': async () => {
    try {
      await listModels(await provider())
    } catch (e) {
      throw ctx.httpError(502, String((e as Error).message ?? e))
    }
    return guideState()
  },
  'POST /api/guide/send': async (b) => {
    if (!(await guideAvailable())) throw ctx.httpError(409, 'ガイドに使える API キーがありません。')
    const a = await guide()
    if (a.busy) throw ctx.httpError(409, 'ツグミはまだ答えています。')
    guideProject = typeof b.project === 'string' && b.project ? projectDir(b.project) : undefined
    a.send(String(b.prompt), String(b.display), guideEvents, String(b.display))
    return { ok: true }
  },
  'POST /api/guide/retry': async () => {
    const a = await guide()
    if (!a.busy) a.retry(guideEvents)
    return { ok: true }
  },
  'POST /api/guide/cancel': async () => {
    guideAgent?.cancel()
    return { ok: true }
  },
  /** A new conversation (the old one is gone; the tour stays). */
  'POST /api/guide/reset': async () => {
    guideAgent?.cancel()
    guideAgent = undefined
    fs.rmSync(guideFile(), { force: true })
    return { ok: true }
  },
  }
}
