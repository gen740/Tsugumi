// Chat, Agent and the settings it needs (skill folders, model connection,
// API key) for the host server. Routes are merged into the server's table.
import { execFile } from 'node:child_process'
import { createHash, randomUUID } from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { AGENT, modelChoices } from '../src/shared/agentShared'
import { type AgentEvent, guideText, type ProviderConfig } from './agent'
import { CoreAgentHost, CoreAgentError, RemoteAgent } from './coreAgent'
import { keychainReader } from './keychain'
import { ChatGPTAuth, ChatGPTError, endpointsFor, OPENAI_ENDPOINTS, USAGE_URL } from './chatgpt'
import { Environments, projectKey } from './env'
import { DesktopSettings } from './desktop'
import { L, language, setLanguage, type Language } from './i18n'

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
let core: CoreAgentHost

export function chatStatus() {
  return { chatAvailable: true, chatError: null }
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
  /** The UI's language (the desktop app's too, on a Mac): the page's strings, the
   *  server's messages, and what tsugumid writes into a project's git. */
  language: Language
}

/** The demo the guide shows: its id and title, the copy's folder, and
 *  whether the guide began it. */
interface GuideTour { demo: string; title: string; path: string; started: boolean }
interface GuideState { setupDone: boolean; demoAnswered: boolean; tour: GuideTour | null }

export const PERMISSION_KINDS = ['network', 'sandbox_off', 'host_folder_read', 'host_folder_write']
const settingsFile = () => path.join(ctx.support, 'settings.json')
const defaultSettings: Settings = {
  skillFolders: [], provider: 'anthropic', model: 'claude-sonnet-5-5', openAIModel: 'gpt-6.1-sol', chatGPTModel: 'gpt-6.1-sol', effort: '', fastModel: '', fixturePort: 8765,
  allowedPermissions: [], webSearch: false,
  guide: { setupDone: false, demoAnswered: false, tour: null },
  versionSummaries: false,
  language: 'ja',
}
const settings = (): Settings => {
  const saved = ctx.readJSON<Partial<Settings>>(settingsFile(), {})
  const s = Object.fromEntries(Object.keys(defaultSettings).map((key) =>
    [key, (ctx.desktop?.values as any)?.[key] ?? (saved as any)[key] ?? (defaultSettings as any)[key]])) as unknown as Settings
  if (s.language !== 'en') s.language = 'ja'
  setLanguage(s.language)
  return s
}
const saveSettings = (s: Settings) => {
  ctx.desktop?.save(settings(), s)
  ctx.writeJSON(settingsFile(), s)
  setLanguage(s.language)
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
  const allowed = b.allowedPermissions
  if (Array.isArray(allowed)) out.allowedPermissions = PERMISSION_KINDS.filter((k) => allowed.includes(k))
  if (typeof b.webSearch === 'boolean') out.webSearch = b.webSearch
  if (b.language === 'ja' || b.language === 'en') out.language = b.language
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
  if (!/^[\x21-\x7e]+$/.test(key) || /["\\]/.test(key)) throw ctx.httpError(400, L('API キーに使えない文字が含まれています。'))
  if (process.platform === 'darwin') {
    // The secret goes to `security` on its standard input, never as an argument.
    const r = await run('security', ['-i'], `add-generic-password -U -s "${KEYCHAIN_SERVICE}" -a "${PROVIDERS[p].name}" -w "${key}"\n`)
    keychain.forget(PROVIDERS[p].name)
    if (r.code === 0 && (await keychainKey(p)) === key) {
      fs.rmSync(keyFile(p), { force: true })
      return L('Keychain に保存しました。キーは Agent の接続に使われ、Card やプロジェクト・設定・ログには残りません。')
    }
  }
  fs.mkdirSync(ctx.support, { recursive: true })
  fs.writeFileSync(keyFile(p), key, { mode: 0o600 })
  fs.chmodSync(keyFile(p), 0o600)
  return L('{0}（0600）に保存しました。キーは Agent の接続に使われ、Card やプロジェクト・設定・ログには残りません。', keyFile(p))
}

async function deleteKey(p: KeyedProvider) {
  if (process.platform === 'darwin') {
    await run('security', ['delete-generic-password', '-s', KEYCHAIN_SERVICE, '-a', PROVIDERS[p].name])
    keychain.forget(PROVIDERS[p].name)
  }
  fs.rmSync(keyFile(p), { force: true })
  return L('{0} のキーを削除しました。', PROVIDERS[p].name) + (process.env[PROVIDERS[p].env] ? L('（環境変数 {0} は残っています）', PROVIDERS[p].env) : '')
}

// Sign in with ChatGPT: this server's registrations and tokens, in a 0600
// file of its data folder. TSUGUMI_CHATGPT_ISSUER and TSUGUMI_CHATGPT_API
// point the sign-in and the requests at the local fixture (tests only).
let chatGPTAuth: ChatGPTAuth | undefined
const chatgpt = () => (chatGPTAuth ??= new ChatGPTAuth(path.join(ctx.support, 'chatgpt.json'),
  process.env.TSUGUMI_CHATGPT_ISSUER ? endpointsFor(process.env.TSUGUMI_CHATGPT_ISSUER, OPENAI_ENDPOINTS.resource) : OPENAI_ENDPOINTS, (method, fields) => core.user.call(method, fields)))
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
  const thought = await guideProvider()
  return { name: p.name, model: p.model, api: p.api, remote: p.auth !== 'none', plan: p.auth === 'chatgpt', effort: p.effort, fast: !!p.fast,
           thoughtModel: thought.auth === 'none' ? undefined : thought.model,
           modelNames: p.auth === 'chatgpt' ? (await modelCatalogs()).modelNames : {} }
}

/** The ChatGPT accounts and the sign-in for the settings page (never a token). */
function chatGPTState() {
  return { accounts: chatgpt().accounts(), signingIn: chatgpt().signingIn, outcome: signInOutcome, usageURL: USAGE_URL }
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
let guideAgent: RemoteAgent | undefined
/** The project the page asked about (its reads go there), if one is open. */
let guideProject: string | undefined
const guideEvents = (event: AgentEvent) => {
  ctx.broadcast({ type: 'guide', event })
  if (guideProject && (event.kind === 'permission' || event.kind === 'permissionAnswered')) {
    ctx.broadcast({ type: 'chat', project: guideProject, session: EXTERNAL_SESSION, event })
  }
}

async function guide(): Promise<RemoteAgent> {
  if (!guideAgent || guideAgent.project !== guideProject) {
    guideAgent?.close()
    guideAgent = await new RemoteAgent(core, 'guide', guideProject, 'guide').open()
  }
  await core.configure(await guideProvider(), 'guide')
  guideAgent.observe(guideEvents)
  await guideAgent.refresh()
  return guideAgent
}

/** The guide's connection: the Agent's, with a small model: claude-haiku-5-5
 *  on Anthropic, and on the ChatGPT plan the model Tsugumi speaks with:
 *  gpt-6.1-luna when the account offers it, else its newest luna (the plan
 *  refuses models it does not offer). OpenAI keeps the Agent's. Demo tours are
 *  led by the chat, with the chat's model. */
async function guideProvider(): Promise<ProviderConfig> {
  const p = await provider(), id = settings().provider
  if (id === 'chatgpt') return { ...p, model: speechModel((await core.storedModels('chatgpt')).map((m) => m.id), p.model) }
  return id === 'anthropic' ? { ...p, model: 'claude-haiku-5-5' } : p
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

interface Skill { name: string; description: string; path: string; source: string; source_folder: string; status: string }
interface SkillListing { skills: Skill[]; folders: { path: string; status: string }[] }

async function skillMetadata(project?: string, folders = settings().skillFolders.filter((f) => f.enabled).map((f) => f.path), conversationId?: string): Promise<SkillListing> {
  const result = await core.user.call<SkillListing>('SkillsListRequest', { project_path: project, folders, conversation_id: conversationId })
  return { ...result, skills: result.skills.map((s) => ({ ...s,
    description: s.path === 'builtin:demo' ? L('ツグミが画面を飛び回って、デモや画面の使い方を案内します') : s.description,
    source: s.path === 'builtin:demo' ? 'Tsugumi' : s.source_folder === (project && path.join(project, 'skills')) ? L('プロジェクト') : path.basename(s.source_folder),
  })) }
}

async function skillsFor(project?: string, conversationId?: string): Promise<Skill[]> {
  return (await skillMetadata(project, undefined, conversationId)).skills.filter((s) => s.status === 'ready')
}

async function modelCatalogs() {
  const catalogs = await Promise.all(['anthropic', 'openai', 'chatgpt'].map(async (id) => [id, await core.storedModels(id)] as const))
  return { models: Object.fromEntries(catalogs.map(([id, list]) => [id, list.map((m) => m.id)])),
    modelNames: Object.fromEntries((catalogs.find(([id]) => id === 'chatgpt')?.[1] ?? []).map((m) => [m.id, m.name])) }
}

// ---- Agent hosting -------------------------------------------------------------

interface PermissionWait {
  project: string
  session: string
  asked: string[]
  resolve: (allow: boolean) => void
}
const permissionWaits = new Map<string, PermissionWait>()
const EXTERNAL_SESSION = 'external'

async function answerPermission(id: string, allow: boolean) {
  await core.user.call('AccessResolveRequest', { request_id: id, allow })
  const waiting = permissionWaits.get(id)
  permissionWaits.delete(id)
  waiting?.resolve(allow)
  if (waiting) ctx.broadcast({ type: 'chat', project: waiting.project, session: waiting.session,
    event: { kind: 'permissionAnswered', id } })
}

const waitingFor = (project: string, session: string) =>
  [...permissionWaits.entries()].filter(([, w]) => w.project === project && w.session === session)

async function withAccess<T>(project: string, session: string, run: () => Promise<T>, from?: string): Promise<T> {
  for (;;) {
    try { return await run() } catch (error) {
      if (!(error instanceof CoreAgentError) || error.code !== 'grant_required' || !error.requestId) throw error
      const id = error.requestId
      const allowed = new Promise<boolean>((resolve) => {
        const asked = [`project_access ${project}: ${error.message}`]
        permissionWaits.set(id, { project, session, asked, resolve })
        ctx.broadcast({ type: 'chat', project, session, event: { kind: 'permission', id, asked, from } })
      })
      if (!await allowed) throw new CoreAgentError('permission_denied', L('プロジェクトへのアクセスは許可されませんでした。'))
    }
  }
}

class ChatHost {
  private conversations = new Map<string, RemoteAgent>()
  private folders = new Set<string>()
  constructor(readonly project: string, readonly key: string) {}

  async agent(id: string): Promise<RemoteAgent> {
    let a = this.conversations.get(id)
    if (!a) {
      a = await new RemoteAgent(core, id, this.project).open()
      this.conversations.set(id, a)
      const access = await core.user.call<{ requests: { id: string; conversation_id: string; project_id: string; state: string }[] }>('AccessListRequest')
      for (const request of access.requests) {
        if (request.state === 'pending' && request.conversation_id !== id && core.projectFolder(request.project_id) === this.project) {
          await showExternalAccess(request.id, this.project)
        }
      }
    }
    a.observe(this.events(id))
    await a.refresh()
    return a
  }

  async attach(folder: string, existing: string[] = []) {
    return (await this.attachFolders([folder], existing))[0].resource
  }

  async attachFolders(folders: string[], existing: string[] = []) {
    for (const value of [...existing, ...folders]) this.folders.add(value)
    await ctx.envs.attachFolders(this.project, this.key, [...this.folders])
    const project = await core.project(this.project)
    const attached: { folder: string; resource: string }[] = []
    for (const folder of folders) {
      const resource = `folder:${project}:${createHash('sha256').update(folder).digest('hex')}`
      await core.user.call('RegisterFolderRequest', { project_id: project, resource_id: resource, folder, title: path.basename(folder) })
      attached.push({ folder, resource })
    }
    return attached
  }

  async detachFolder(folder: string) {
    this.folders.delete(folder)
    await ctx.envs.attachFolders(this.project, this.key, [...this.folders])
  }

  async attachSkillFolders(names: string[], conversationId: string) {
    const skills = (await skillsFor(this.project, conversationId)).filter((s) => names.includes(s.name))
    const folders = new Set(skills.filter((s) => s.path !== 'builtin:demo' && s.source_folder !== path.join(this.project, 'skills')).map((s) => s.source_folder))
    for (const folder of folders) await this.attach(folder)
  }

  events(id: string) {
    return (event: AgentEvent) => ctx.broadcast({ type: 'chat', project: this.project, session: id, event })
  }

  detach() { for (const conversation of this.conversations.values()) conversation.close() }

  async sessions() {
    return core.user.call('ConversationListRequest', {})
  }

  async delete(id: string) {
    await core.user.call('ConversationDeleteRequest', { conversation_id: id })
    this.conversations.get(id)?.close()
    this.conversations.delete(id)
  }

  async search(text: string) { return core.user.call('ConversationSearchRequest', { text }) }

  async externalCall(name: string, args: unknown) {
    const project = await core.project(this.project)
    const id = `external-${this.key}`
    await core.external.call('ConversationCreateRequest', { conversation_id: id, project_id: project, role: 'external' })
    const op = randomUUID()
    try {
      return await core.external.call('ToolCallRequest', { conversation_id: id, project_id: project, name,
        arguments_json: typeof args === 'string' ? args : JSON.stringify(args ?? {}), op_id: op })
    } catch (error) {
      if (error instanceof CoreAgentError && error.code === 'grant_required' && error.requestId) {
        const asked = [`project_access ${this.project}: ${error.message}`]
        permissionWaits.set(error.requestId, { project: this.project, session: EXTERNAL_SESSION, asked, resolve: () => {} })
        ctx.broadcast({ type: 'chat', project: this.project, session: EXTERNAL_SESSION,
          event: { kind: 'permission', id: error.requestId, asked, from: 'Claude Code' } })
        return { text: JSON.stringify({ code: error.code, message: error.message, request_id: error.requestId }), isError: true }
      }
      throw error
    }
  }
}

const chats = new Map<string, ChatHost>()
function chatFor(dir: string): ChatHost {
  const { key, real } = projectKey(dir)
  let chat = chats.get(key)
  if (!chat) { chat = new ChatHost(real, key); chats.set(key, chat) }
  return chat
}

/** A disconnected page does not own the lifetime of a core conversation. */
export function cancelChats(dir: string) { chats.get(projectKey(dir).key)?.detach() }

async function showExternalAccess(requestId: string, fallback?: string) {
  const access = await core.user.call<{ requests: { id: string; project_id: string; access: string; scope: string; reason: string; state: string }[] }>('AccessListRequest')
  const request = access.requests.find((r) => r.id === requestId && r.state === 'pending')
  if (!request) return
  const project = core.projectFolder(request.project_id) ?? fallback
  if (!project) return
  const asked = [`project_access ${request.access} [${request.scope}] ${request.project_id}: ${request.reason}`]
  permissionWaits.set(requestId, { project, session: EXTERNAL_SESSION, asked, resolve: () => {} })
  ctx.broadcast({ type: 'chat', project, session: EXTERNAL_SESSION,
    event: { kind: 'permission', id: requestId, asked, from: 'Claude Code' } })
}

async function externalAPI(name: string, args: unknown, project?: string) {
  try {
    const result = await core.externalAPI(name, args)
    if (name === 'agent_access') {
      const value = JSON.parse(result.text)
      if (typeof value.id === 'string') await showExternalAccess(value.id, project)
    }
    return result
  } catch (error) {
    if (!(error instanceof CoreAgentError)) throw error
    if (error.code === 'grant_required' && error.requestId) await showExternalAccess(error.requestId, project)
    return { text: JSON.stringify({ code: error.code, message: error.message, request_id: error.requestId }), isError: true }
  }
}

const keyedProvider = (v: unknown): KeyedProvider => (v === 'openai' ? 'openai' : 'anthropic')

/** The Agent of a session, ready for a new turn (current connection and folders). */
async function turnAgent(b: any) {
  const host = chatFor(ctx.projectDir(b.path))
  const a = await host.agent(String(b.session))
  await core.configure(await chatProvider(), a.id)
  return { host, a }
}

// The same authenticated host connection also serves Home's project requests.
export function hostRequest<T = any>(method: string, fields: unknown = {}): Promise<T> {
  return core.user.call<T>(method, fields)
}

export function chatRoutes(c: ChatContext): Record<string, Handler> {
  ctx = c
  // The language for the server's own messages, from the start (the app's, once read).
  settings()
  void ctx.desktop?.refresh().then(() => settings(), () => {})
  core = new CoreAgentHost(c.support, c.here, c.envs)
  const projectDir = c.projectDir
  return {
  'POST /api/skills': (b) => skillsFor(b.path ? projectDir(b.path) : undefined),
  'GET /api/settings': async () => ({
    ...(await ctx.desktop?.refresh(), settings()),
    ...await modelCatalogs(),
    keys: { Anthropic: await keySource('anthropic'), OpenAI: await keySource('openai') },
    keyEnv: { Anthropic: PROVIDERS.anthropic.env, OpenAI: PROVIDERS.openai.env },
  }),
  'POST /api/settings': async (b) => {
    const next = { ...settings(), ...cleanSettings(b ?? {}) }
    saveSettings(next)
    return { ...next, ...await modelCatalogs() }
  },
  // Only while switched on, and not on the ChatGPT plan (its allowance is for the Agent).
  'POST /api/versions/summarize': async (b) => {
    const s = settings()
    if (!s.versionSummaries) throw ctx.httpError(400, L('版の要約はオフです。'))
    if (s.provider === 'chatgpt') throw ctx.httpError(400, L('ChatGPT プランでは版の要約を作りません。'))
    const project = await core.project(projectDir(b.path))
    await core.configure(await provider())
    return core.user.call('VersionSummarizeRequest', { project_id: project, commit: String(b.commit) })
  },
  'POST /api/settings/key': async (b) => ({ message: await saveKey(keyedProvider(b.provider), String(b.key ?? '')) }),
  'POST /api/settings/key/delete': async (b) => ({ message: await deleteKey(keyedProvider(b.provider)) }),
  /** Lists a connection's models (the one in use unless `provider` names another). */
  'POST /api/settings/models': async (b) => {
    const id: ProviderId = b.provider === 'anthropic' || b.provider === 'openai' || b.provider === 'chatgpt' ? b.provider : settings().provider
    if (id === 'fixture') throw ctx.httpError(400, L('fixture にはモデル一覧がありません。'))
    const list = await core.models(await providerFor(id))
    if (!list.length) throw ctx.httpError(502, L('モデル一覧が空でした。保存済みの一覧はそのままです。'))
    return { models: list.map((m) => m.id), modelNames: (await modelCatalogs()).modelNames }
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
        ? L('{0} でサインインしました。', r.account.email ?? r.account.clientId)
        : L('サインインしましたが、ChatGPT プランの利用は許可されませんでした。「プランの利用を許可」から許可するか、API キーの接続を使ってください。') }
    }, (e: Error) => {
      signInOutcome = { ok: false, message: e.message === new ChatGPTError('failed', 'cancelled').message ? L('サインインを中止しました。') : e.message }
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
    return { ...chatGPTState(), message: revoked ? L('{0} からサインアウトしました。', email)
      : L('このホストのトークンは消しましたが、OpenAI 側での取り消しを確かめられませんでした。ChatGPT の設定で Tsugumi の接続を解除できます。') }
  },
  'POST /api/settings/skillCount': async (b) => ({ count: (await skillMetadata(undefined, [String(b.path)])).skills.filter((s) => s.status === 'ready' && s.path !== 'builtin:demo').length }),
  'POST /api/settings/skillFolderStatus': async (b) => {
    const paths = Array.isArray(b.paths) ? b.paths.map(String) : []
    return Object.fromEntries((await skillMetadata(undefined, paths)).folders.map((f) => [f.path, f.status === 'ready']))
  },
  'POST /api/chat/sessions': (b) => chatFor(projectDir(b.path)).sessions(),
  'POST /api/chat/conversation': async (b) => {
    const host = chatFor(projectDir(b.path))
    const a = await host.agent(String(b.session))
    const [wait] = [...waitingFor(host.project, a.id), ...waitingFor(host.project, EXTERNAL_SESSION)]
    const access = await core.user.call<{ requests: { id: string; state: string; access: string; scope: string; reason: string; project_id: string }[] }>('AccessListRequest', { conversation_id: a.id })
    const pending = access.requests.find((r) => r.state === 'pending')
    return { conversation: a.conversation, busy: a.busy, continuable: a.continuable, provider: await providerInfo(),
      permission: pending ? { id: pending.id, asked: [`project_access ${pending.project_id}: ${pending.access} [${pending.scope}] — ${pending.reason}`] } : wait ? { id: wait[0], asked: wait[1].asked } : null }
  },
  'POST /api/chat/send': async (b) => {
    const { host, a } = await turnAgent(b)
    const turn = randomUUID()
    const skillNames = Array.isArray(b.skillNames) ? b.skillNames.map(String) : []
    await host.attachSkillFolders(skillNames, a.id)
    await withAccess(host.project, a.id, () => a.send(String(b.text ?? ''), host.events(a.id), turn, {
      selected_cards: Array.isArray(b.selectedCards) ? b.selectedCards.map(String) : [],
      selected_displays: Array.isArray(b.selectedDisplays) ? b.selectedDisplays.map(String) : [],
      skill_names: [...new Set(skillNames)],
      attached_resources: Array.isArray(b.attachedResources) ? b.attachedResources.map(String) : [],
    }))
      .catch((e) => host.events(a.id)({ kind: 'error', message: e.message }))
    return { ok: true }
  },
  'POST /api/chat/thoughts': async (b) => {
    const a = await chatFor(projectDir(b.path)).agent(String(b.session))
    return core.summarize(a.id, await guideProvider())
  },
  'POST /api/chat/continue': async (b) => {
    const { host, a } = await turnAgent(b)
    await a.action('Continue', host.events(a.id))
    return { ok: true }
  },
  'POST /api/chat/retry': async (b) => {
    const { host, a } = await turnAgent(b)
    await a.action('Retry', host.events(a.id))
    return { ok: true }
  },
  'POST /api/chat/compact': async (b) => {
    const { host, a } = await turnAgent(b)
    await a.action('Compact', host.events(a.id))
    return { ok: true }
  },
  'POST /api/chat/permission': async (b) => {
    await answerPermission(String(b.id), b.allow === true)
    return { ok: true }
  },
  'POST /api/chat/cancel': async (b) => {
    const host = chatFor(projectDir(b.path))
    await (await host.agent(String(b.session))).cancel()
    for (const [id] of waitingFor(host.project, String(b.session))) await answerPermission(id, false)
    return { ok: true }
  },
  'POST /api/chat/delete': async (b) => { await chatFor(projectDir(b.path)).delete(String(b.session)); return { ok: true } },
  'POST /api/chat/folders': async (b) => {
    if (!Array.isArray(b.selected) || !b.selected.length) throw ctx.httpError(400, 'Select at least one folder.')
    // Validate the entire selection before sharing anything or restarting the VM.
    for (const folder of b.selected) {
      if (!cleanFolders([folder]).length) throw ctx.httpError(400, L('フォルダが見つかりません: {0}', folder))
    }
    const folders = cleanFolders(b.selected)
    const host = chatFor(projectDir(b.path))
    return { folders: await host.attachFolders(folders, cleanFolders(b.folders)) }
  },
  'POST /api/chat/folder/detach': async (b) => {
    const project = projectDir(b.path)
    const { key } = projectKey(project)
    const access = await core.user.call<{ grants: { id: string; project_id: string; active: boolean }[] }>('AccessListRequest')
    for (const grant of access.grants.filter((g) => g.active && g.project_id === String(b.resource))) {
      await core.user.call('AccessRevokeRequest', { grant_id: grant.id })
    }
    await chatFor(project).detachFolder(String(b.folder))
    return { ok: true }
  },
  'POST /api/chat/search': (b) => chatFor(projectDir(b.path)).search(String(b.text)),
  // Claude Code (the plugin's MCP server) works on a project through these:
  // the chat's tools, run here with the same checks, and shown on the page.
  'POST /api/agent/tools': () => core.tools(),
  /** Claude Code opened a project: a page that has it or is on Home shows it (others keep
   *  their projects; none free: a browser opens one, unless `browser: false`). */
  'POST /api/agent/open': (b) => {
    const project = chatFor(projectDir(b.path)).project
    const shown = ctx.showOnPage(project)
    const opened = !shown && b.browser !== false
    if (opened) ctx.openPage(project)
    return { project, shown, opened }
  },
  'POST /api/agent/call': (b) => String(b.name).startsWith('agent_')
    ? externalAPI(String(b.name), b.arguments, b.path ? projectDir(b.path) : undefined)
    : chatFor(projectDir(b.path)).externalCall(String(b.name), b.arguments),
  'POST /api/agent/access': () => core.user.call('AccessListRequest'),
  'POST /api/agent/access/resolve': async (b) => { await answerPermission(String(b.id), b.allow === true); return { ok: true } },
  'POST /api/agent/access/revoke': (b) => core.user.call('AccessRevokeRequest', { grant_id: String(b.id) }),
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
      await core.models(await provider())
    } catch (e) {
      throw ctx.httpError(502, String((e as Error).message ?? e))
    }
    return guideState()
  },
  'POST /api/guide/send': async (b) => {
    if (!(await guideAvailable())) throw ctx.httpError(409, L('ガイドに使える API キーがありません。'))
    guideProject = typeof b.project === 'string' && b.project ? projectDir(b.project) : undefined
    const a = await guide()
    if (a.busy) throw ctx.httpError(409, L('ツグミはまだ答えています。'))
    const turn = randomUUID()
    void withAccess(guideProject ?? '', 'guide', () => a.send(String(b.text ?? ''), guideEvents, turn))
      .catch((e) => guideEvents({ kind: 'error', message: e.message }))
    return { ok: true }
  },
  'POST /api/guide/retry': async () => {
    const a = await guide()
    if (!a.busy) await a.action('Retry', guideEvents)
    return { ok: true }
  },
  'POST /api/guide/cancel': async () => {
    await guideAgent?.cancel()
    return { ok: true }
  },
  /** A new conversation (the old one is gone; the tour stays). */
  'POST /api/guide/reset': async () => {
    await guideAgent?.cancel()
    guideAgent = undefined
    await core.user.call('ConversationDeleteRequest', { conversation_id: 'guide' })
    return { ok: true }
  },
  }
}
