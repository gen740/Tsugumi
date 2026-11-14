// Chat, Agent and the settings it needs (skill folders, model connection,
// API key) for the host server. Routes are merged into the server's table.
import { createHash, randomUUID } from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import { modelChoices } from '../src/shared/agentShared'
import { type AgentEvent, type ProviderConfig } from './agent'
import { CoreAgentHost, CoreAgentError, RemoteAgent } from './coreAgent'
import { ChatGPTAuth, ChatGPTError, endpointsFor, OPENAI_ENDPOINTS, USAGE_URL } from './chatgpt'
import { Environments, projectKey } from './environment'
import { L, setLanguage, type Language } from './i18n'

export interface ChatContext {
  support: string
  envs: Environments
  /** The desktop app's settings, when this server shares them (the Mac's tsugumi command). */
  desktop?: boolean
  projectDir: (p: unknown) => string
  broadcast: (v: unknown) => void
  httpError: (status: number, message: string) => Error
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
  /** Agentd chooses how to produce version notes. */
  versionNoteMode: 'mechanical' | 'agent'
  helperParallelism: number
  allowUnscopedHelpers: boolean
  animateAgentWork: boolean
  /** The UI's language (the desktop app's too, on a Mac): the page's strings, the
   *  server's messages, and what tsugumid writes into a project's git. */
  language: Language
}

// The host keeps presentation snapshots only; persistence and validation are RPC.
const SHARED_KEYS: Record<string, string> = {
  provider: 'agent.provider', model: 'agent.model', openAIModel: 'agent.model.openai', chatGPTModel: 'agent.model.chatgpt',
  effort: 'agent.effort', fastModel: 'agent.fastModel', fixturePort: 'agent.fixturePort', allowedPermissions: 'agent.permissions.allowed',
  helperParallelism: 'agent.helperParallelism', allowUnscopedHelpers: 'agent.allowUnscopedHelpers',
  animateAgentWork: 'agent.animateWork', webSearch: 'agent.webSearch', versionNoteMode: 'versions.noteMode', language: 'app.language',
}
const LOCAL_KEYS: Record<string, string> = { skillFolders: 'agent.skillFolders' }
const localProfile = () => `web:${ctx.support}`
const settingsProfile = () => ctx.desktop ? 'desktop' : localProfile()
const revisions: Record<string, number> = {}
let snapshot = {} as Settings
const settings = (): Settings => snapshot
async function loadSettings() {
  await core.provisionEnvironment()
  const next: any = {}
  for (const [profile, keys] of [[settingsProfile(), SHARED_KEYS], [localProfile(), LOCAL_KEYS]] as const) {
    const reply = await core.getSettings(profile)
    revisions[profile] = reply.revision
    const values = Object.fromEntries(reply.values.map((s) => [s.key, JSON.parse(s.value_json)]))
    for (const [name, key] of Object.entries(keys)) if (values[key] !== undefined) next[name] = values[key]
  }
  snapshot = next
  setLanguage(snapshot.language)
}
async function patchSettings(change: Partial<Settings>, expected: Record<string, number> = { ...revisions }) {
  const patches = new Map<string, { key: string; value_json: string }[]>()
  for (const [profile, keys] of [[settingsProfile(), SHARED_KEYS], [localProfile(), LOCAL_KEYS]] as const) {
    const changes = Object.entries(keys).filter(([name]) => name in change).map(([name, key]) => ({ key, value_json: JSON.stringify((change as any)[name]) }))
    if (changes.length) patches.set(profile, [...(patches.get(profile) ?? []), ...changes])
  }
  for (const [profile, changes] of patches) {
    await core.user.call('SettingsUpdateRequest', { profile, revision: expected[profile], changes, client: 'web' })
  }
  await loadSettings()
}
const PROVIDERS = { anthropic: { name: 'Anthropic', env: 'ANTHROPIC_API_KEY' }, openai: { name: 'OpenAI', env: 'OPENAI_API_KEY' } } as const
type KeyedProvider = keyof typeof PROVIDERS
function keyedProvider(id: unknown): KeyedProvider {
  if (id === 'anthropic' || id === 'openai') return id
  throw ctx.httpError(400, 'Unknown provider')
}
async function keySource(p: KeyedProvider) {
  return core.keys.source(p, settingsProfile())
}
async function saveKey(p: KeyedProvider, key: string) {
  const source = await core.keys.save(p, key, settingsProfile())
  return source === 'keychain'
    ? L('Keychain に保存しました。キーは Agent の接続に使われ、Card やプロジェクト・設定・ログには残りません。')
    : L('agentd に保存しました。キーは Card やプロジェクト・設定・ログには残りません。')
}
async function deleteKey(p: KeyedProvider) {
  await core.keys.delete(p, settingsProfile())
  return L('{0} のキーを削除しました。', PROVIDERS[p].name)
}

// Browser sign-in and account status are scoped to this Web host in agentd.
// Test overrides point the issuer and resource at the local OAuth fixture.
let chatGPTAuth: ChatGPTAuth | undefined
const chatgpt = () => (chatGPTAuth ??= new ChatGPTAuth(localProfile(),
  process.env.TSUGUMI_CHATGPT_ISSUER ? endpointsFor(process.env.TSUGUMI_CHATGPT_ISSUER, process.env.TSUGUMI_CHATGPT_API ?? OPENAI_ENDPOINTS.resource) : OPENAI_ENDPOINTS, (method, fields) => core.user.call(method, fields)))
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
  return { ...p, usesChoices: true, ...modelChoices(s.provider, p.model, s.effort, s.fastModel) }
}

/** Presentation labels and opaque service references for the chosen model. */
async function providerFor(id: ProviderId): Promise<ProviderConfig> {
  return { ...await providerDisplay(id), profile: settingsProfile(), accountProfile: localProfile() }
}

async function providerDisplay(id: ProviderId): Promise<ProviderConfig> {
  const s = { ...settings(), provider: id }
  if (s.provider === 'fixture') {
    return { name: 'Local fixture', model: 'fixture', api: 'anthropicMessages', auth: 'none' }
  }
  if (s.provider === 'chatgpt') {
    return { name: 'ChatGPT', model: s.chatGPTModel,
             api: 'openAIResponses', auth: 'chatgpt' }
  }
  if (s.provider === 'openai') {
    return { name: 'OpenAI', model: s.openAIModel, api: 'openAIResponses', auth: 'apiKey' }
  }
  return { name: 'Anthropic', model: s.model, api: 'anthropicMessages', auth: 'apiKey' }
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
async function chatGPTState() {
  return { accounts: await chatgpt().accounts(), signingIn: chatgpt().signingIn, outcome: signInOutcome, usageURL: USAGE_URL }
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
// app with the same model connection as the Agent. Scripted guidance uses the Host contract. It
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
  if (s.provider === 'chatgpt') return (await chatgpt().accounts()).some((a) => a.active && a.signedIn && a.planEnabled)
  return !!(await keySource(s.provider))
}

// The relay fixes the guide session to this Web host; tabs never own progress.
const guideSession = () => `web:${ctx.support}`
async function guideState() {
  const available = await guideAvailable()
  const progress = await core.user.call('GuideAdvanceRequest', { session_id: guideSession(), language: settings().language,
    event: { kind: 'connection', connected: available } })
  return { available, ...progress }
}

// ---- Skills ----------------------------------------------------------------------

interface Skill { name: string; description: string; path: string; source: string; source_folder: string; attachment_folder?: string; status: string }
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

async function answerPermission(id: string, allow: boolean, selected?: string[]) {
  const access = await core.user.call<{ requests: { id: string; asked: string[] }[] }>('AccessListRequest')
  const asked = access.requests.find(r => r.id === id)?.asked ?? []
  let neededAllowed = allow
  const additional: string[] = []
  for (const request of asked) {
    if (!request.startsWith('project_access ') || request.includes('operation_approval\n')) continue
    const item = JSON.parse(request.slice('project_access '.length))
    const checked = allow && (selected ? selected.includes(request) : item.needed_now === true)
    if (item.needed_now === true) neededAllowed = checked
    else if (checked) additional.push(item.access)
  }
  await core.user.call('AccessResolveRequest', { request_id: id, allow: neededAllowed, also_allow: additional })
  const waiting = permissionWaits.get(id)
  permissionWaits.delete(id)
  waiting?.resolve(neededAllowed)
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
      const access = await core.user.call<{ requests: { id: string; asked: string[] }[] }>('AccessListRequest')
      const request = access.requests.find(r => r.id === id)
      if (!request) throw error
      const allowed = new Promise<boolean>((resolve) => {
        const asked = request.asked
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
    const folders = new Set(skills.map((s) => s.attachment_folder).filter((s): s is string => !!s))
    for (const folder of folders) await this.attach(folder)
  }

  events(id: string) {
    return (event: AgentEvent) => ctx.broadcast({ type: 'chat', project: this.project, session: id, event })
  }

  detach() { for (const conversation of this.conversations.values()) conversation.close() }

  async sessions() {
    return core.user.call('ConversationListRequest', { project_id: await core.project(this.project) })
  }

  async delete(id: string) {
    await core.user.call('ConversationDeleteRequest', { conversation_id: id })
    this.conversations.get(id)?.close()
    this.conversations.delete(id)
  }

  async search(text: string) { return core.user.call('ConversationSearchRequest', { text, project_id: await core.project(this.project) }) }

  async externalCall(name: string, args: unknown) {
    const project = await core.project(this.project)
    const id = `external-${project}`
    // A moved transcript needs fresh grants. The tool request asks agentd for
    // its own access without reading that transcript.
    await core.external.call('ConversationCreateRequest', { conversation_id: id, project_id: project, role: 'external' }).catch(error => {
      if (!(error instanceof CoreAgentError) || error.code !== 'permission_denied') throw error
    })
    const op = randomUUID()
    try {
      return await core.external.call('ToolCallRequest', { conversation_id: id, project_id: project, name,
        arguments_json: typeof args === 'string' ? args : JSON.stringify(args ?? {}), op_id: op })
    } catch (error) {
      if (error instanceof CoreAgentError && error.code === 'grant_required' && error.requestId) {
        await showExternalAccess(error.requestId, this.project)
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
  const access = await core.user.call<{ requests: { id: string; project_id: string; asked: string[]; state: string }[] }>('AccessListRequest')
  const request = access.requests.find((r) => r.id === requestId && r.state === 'pending')
  if (!request) return
  const project = core.projectFolder(request.project_id) ?? fallback
  if (!project) return
  const asked = request.asked
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


/** The Agent of a session, ready for a new turn (current connection and folders). */
async function turnAgent(b: any) {
  const host = chatFor(ctx.projectDir(b.path))
  const a = await host.agent(String(b.session))
  await core.configure(await chatProvider(), a.id)
  return { host, a }
}

// The same authenticated host connection also serves Home's project requests.
export function observeHost(handler: (event: import('./environment').HostEvent) => void) {
  return core.observeHost(handler)
}

export function hostRequest<T = any>(method: string, fields: unknown = {}): Promise<T> {
  return core.user.call<T>(method, fields)
}

export function chatRoutes(c: ChatContext): Record<string, Handler> {
  ctx = c
  // The language for the server's own messages, from the start (the app's, once read).
  core = new CoreAgentHost(c.support, c.here, c.envs)
  core.observeHost((event) => {
    if (event.type === 'SettingsChanged' && event.value.profile === settingsProfile()) c.broadcast({ type: 'settingsChanged' })
  })
  const projectDir = c.projectDir
  const routes: Record<string, Handler> = {
  'POST /api/skills': (b) => skillsFor(b.path ? projectDir(b.path) : undefined),
  'GET /api/agent/appearance': async () => {
    await loadSettings()
    return { animateWork: settings().animateAgentWork }
  },
  'POST /api/agent/appearance': async (b) => {
    await patchSettings({ animateAgentWork: b.animateWork })
    return { animateWork: settings().animateAgentWork }
  },
  'GET /api/settings': async () => ({
    ...settings(),
    revision: { ...revisions },
    ...await modelCatalogs(),
    keys: { Anthropic: await keySource('anthropic'), OpenAI: await keySource('openai') },
    keyEnv: { Anthropic: PROVIDERS.anthropic.env, OpenAI: PROVIDERS.openai.env },
  }),
  'POST /api/settings': async (b) => {
    await patchSettings(b ?? {}, b?.revision ?? { ...revisions })
    return { ...settings(), revision: { ...revisions }, ...await modelCatalogs(), chatProvider: await providerInfo() }
  },
  'POST /api/versions/note': async (b) => {
    const project = await core.project(projectDir(b.path))
    const [anthropic_key_profile, openai_key_profile] = await Promise.all([core.keys.provision('anthropic'), core.keys.provision('openai')])
    return core.user.call('VersionNoteRequest', { project_id: project, commit: String(b.commit), profile: settingsProfile(), language: settings().language, anthropic_key_profile, openai_key_profile })
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
    chatgpt: (await chatgpt().accounts()).some((a) => a.active && a.signedIn && a.planEnabled),
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
      signInOutcome = { ok: false, message: e.message === new ChatGPTError('cancelled').message ? L('サインインを中止しました。') : e.message }
    })
    return { url }
  },
  'POST /api/chatgpt/cancel': async () => {
    await chatgpt().cancel()
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
    const email = (await chatgpt().accounts()).find((a) => a.clientId === id)?.email ?? id
    const revoked = await chatgpt().signOut(id)
    return { ...await chatGPTState(), message: revoked ? L('{0} からサインアウトしました。', email)
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
    const access = await core.user.call<{ requests: { id: string; state: string; asked: string[] }[] }>('AccessListRequest', { conversation_id: a.id })
    const pending = access.requests.find((r) => r.state === 'pending')
    let info = await providerInfo()
    if (a.busy && a.modelBinding) {
      const p = a.modelBinding
      info = { ...info, name: p.providerId === 'chatgpt' ? 'ChatGPT' : p.providerId === 'openai' ? 'OpenAI' : p.providerId === 'fixture' ? 'Local fixture' : 'Anthropic',
        model: p.model, effort: p.effort, fast: p.fast, plan: p.providerId === 'chatgpt', remote: p.providerId !== 'fixture',
        api: p.providerId === 'chatgpt' || p.providerId === 'openai' ? 'openAIResponses' : 'anthropicMessages' }
    }
    return { conversation: a.conversation, busy: a.busy, resumable: a.resumable, interruptedOperation: a.interruptedOperation, continuable: a.continuable, provider: info,
      permission: pending ? { id: pending.id, asked: pending.asked } : wait ? { id: wait[0], asked: wait[1].asked } : null }
  },
  'POST /api/chat/send': async (b) => {
    const { host, a } = await turnAgent(b)
    const turn = randomUUID()
    const skillNames = Array.isArray(b.skillNames) ? b.skillNames.map(String) : []
    await host.attachSkillFolders(skillNames, a.id)
    await withAccess(host.project, a.id, () => a.send(String(b.text ?? ''), host.events(a.id), turn, {
      display: String(b.display ?? b.text ?? ''),
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
  'POST /api/chat/resume': async (b) => {
    const { host, a } = await turnAgent(b)
    await a.action('Resume', host.events(a.id))
    return { ok: true }
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
    await answerPermission(String(b.id), b.allow === true, Array.isArray(b.selected) ? b.selected : undefined)
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
  'POST /api/agent/access/resolve': async (b) => { await answerPermission(String(b.id), b.allow === true, Array.isArray(b.selected) ? b.selected : undefined); return { ok: true } },
  'POST /api/agent/access/revoke': (b) => core.user.call('AccessRevokeRequest', { grant_id: String(b.id) }),
  'POST /api/guide': async () => {
    const state = await guideState()
    const a = state.available ? await guide() : undefined
    return { ...state, conversation: a?.conversation ?? null, busy: a?.busy ?? false }
  },
  'POST /api/guide/get': () => core.user.call('GuideGetRequest', { session_id: guideSession(), language: settings().language }),
  'POST /api/guide/advance': async (b) => {
    const state = await core.user.call('GuideAdvanceRequest', {
      session_id: guideSession(), language: settings().language, event: b.event,
    })
    ctx.broadcast({ type: 'guideChanged', origin: b.origin })
    return state
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
  return Object.fromEntries(Object.entries(routes).map(([key, handler]) => [key, async (body: any, url: URL) => {
    await loadSettings()
    return handler(body, url)
  }]))
}
