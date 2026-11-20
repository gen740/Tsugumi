import type { AccessGroup } from '../shared/chatPresentation'
// Chat, Agent and the settings it needs (skill folders, model connection,
// API key) for the host server. Routes are merged into the server's table.
import { randomUUID } from 'node:crypto'
import path from 'node:path'
import { modelChoices } from '../shared/agentShared'
import type { Access } from '../shared/gen/tsugumi/service/access'
import { type ProviderConfig } from './agent'
import { CoreHost, CoreHostError } from './coreHost'
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
  /** Opens the page in the default browser (the page's key is the server's alone). */
  showOnPage: (project: string) => boolean
  openPage: (project?: string) => void
}

type Handler = (b: any, url: URL, signal?: AbortSignal) => unknown | Promise<unknown>

let ctx: ChatContext
let core: CoreHost

export function chatStatus() {
  return { chatAvailable: true, chatError: null }
}

type ProviderId = 'anthropic' | 'openai' | 'chatgpt' | 'fixture'

interface Settings {
  trustedFolders: string[]
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
  /** Hostd chooses how to produce version notes. */
  versionNoteMode: 'mechanical' | 'agent'
  allowUnscopedHelpers: boolean
  animateAgentWork: boolean
  /** The UI's language (the desktop app's too, on a Mac): the page's strings, the
   *  server's messages, and what tsugumid writes into a project's git. */
  language: Language
  /** The shared VM's CPUs, memory and store size (GiB) from its next start; 0 is this machine's default. */
  vmCpus: number
  vmMemoryGiB: number
  vmStoreGiB: number
}

// The host keeps presentation snapshots only; persistence and validation are RPC.
const SHARED_KEYS: Record<string, string> = {
  provider: 'agent.provider', model: 'agent.model', openAIModel: 'agent.model.openai', chatGPTModel: 'agent.model.chatgpt',
  effort: 'agent.effort', fastModel: 'agent.fastModel', fixturePort: 'agent.fixturePort', allowedPermissions: 'agent.permissions.allowed',
  allowUnscopedHelpers: 'agent.allowUnscopedHelpers',
  animateAgentWork: 'agent.animateWork', webSearch: 'agent.webSearch', versionNoteMode: 'versions.noteMode', language: 'app.language',
}
const HOST_KEYS: Record<string, string> = { trustedFolders: 'agent.trustedFolders', vmCpus: 'vm.cpus', vmMemoryGiB: 'vm.memoryGiB', vmStoreGiB: 'vm.storeGiB' }
const LOCAL_KEYS: Record<string, string> = { skillFolders: 'agent.skillFolders' }
const localProfile = () => `web:${ctx.support}`
const settingsProfile = () => ctx.desktop ? 'desktop' : localProfile()
function settingsProfiles() {
  const profiles = new Map<string, Record<string, string>>()
  for (const [profile, keys] of [[settingsProfile(), SHARED_KEYS], [localProfile(), LOCAL_KEYS], ['desktop', HOST_KEYS]] as const) {
    profiles.set(profile, { ...profiles.get(profile), ...keys })
  }
  return profiles
}
let snapshot = {} as Settings
const settings = (): Settings => snapshot
async function loadSettings() {
  await core.provisionEnvironment()
  const next: Partial<Settings> = {}
  const revision: Record<string, number> = {}
  for (const [profile, keys] of settingsProfiles()) {
    const reply = await core.getSettings(profile)
    revision[profile] = reply.revision
    const values = Object.fromEntries(reply.values.map((s) => [s.key, JSON.parse(s.value_json)]))
    for (const [name, key] of Object.entries(keys)) if (values[key] !== undefined) Object.assign(next, { [name]: values[key] })
  }
  snapshot = next as Settings
  setLanguage(snapshot.language)
  return { ...snapshot, revision }
}
async function patchSettings(change: Partial<Settings>, expected?: Record<string, number>) {
  for (const [profile, keys] of settingsProfiles()) {
    const changes = Object.entries(keys).filter(([name]) => name in change).map(([name, key]) => ({ key, value_json: JSON.stringify(change[name as keyof Settings]) }))
    if (!changes.length) continue
    const revision = expected ? expected[profile] : (await core.getSettings(profile)).revision
    if (revision === undefined) throw ctx.httpError(400, 'Missing settings revision')
    await core.user.call('SettingsUpdateRequest', { profile, revision, changes, client: 'web' })
  }
  return loadSettings()
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
    : L('hostd に保存しました。キーは Card やプロジェクト・設定・ログには残りません。')
}
async function deleteKey(p: KeyedProvider) {
  await core.keys.delete(p, settingsProfile())
  return L('{0} のキーを削除しました。', PROVIDERS[p].name)
}

// Browser sign-in and account status are scoped to this Web host in hostd.
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
  const thought = await purposeProvider('summary')
  return { name: p.name, model: p.model, api: p.api, remote: p.auth !== 'none', plan: p.auth === 'chatgpt', effort: p.effort, fast: !!p.fast,
           thoughtModel: thought.auth === 'none' ? undefined : thought.model,
           modelNames: p.auth === 'chatgpt' ? (await modelCatalogs()).modelNames : {} }
}

function providerConnectionMessage(code?: string): string | undefined {
  switch (code) {
    case 'chatgpt_sign_in_required': return L('ChatGPT にもう一度サインインしてください。')
    case 'chatgpt_plan_disabled': return L('このアカウントでは ChatGPT プランの利用が許可されていないため、Agent は使えません。許可するか、API キーの接続を選んでください。')
  }
}

/** The ChatGPT accounts and the sign-in for the settings page (never a token). */
async function chatGPTState() {
  const status = await core.user.call<{ code?: string }>('ProviderStatusRequest', { profile: localProfile(), id: 'chatgpt' })
  return { accounts: await chatgpt().accounts(), signingIn: chatgpt().signingIn, outcome: signInOutcome, usageURL: USAGE_URL,
    connectionMessage: providerConnectionMessage(status.code) }
}

// ---- The guide -----------------------------------------------------------------

// The guide bird in the bottom-right corner: one conversation for the whole
// app with the same model connection as the Agent. Scripted guidance uses the Host contract. It
// reads the project on the page's screen and explains; what it offers to do
// (guide_show) the user does with a button of the page.
let guideProject: string | undefined

async function guideTurn() {
  await core.conversation('guide', guideProject, 'guide')
  await core.configure(await guideProvider(), 'guide')
}

/** The host chooses the guide/summary model from settings and its catalog. */
async function purposeProvider(purpose: 'guide' | 'summary'): Promise<ProviderConfig> {
  const value = await core.user.call<{ provider: { providerId: ProviderId; model: string } }>('ModelSelectionRequest', { profile: settingsProfile(), purpose })
  return { ...await providerFor(value.provider.providerId), model: value.provider.model, purpose }
}

async function guideProvider(): Promise<ProviderConfig> { return purposeProvider('guide') }

/** Whether the chosen provider can be used: a key (Anthropic, OpenAI), a
 *  signed-in ChatGPT plan, or the local fixture. */
async function guideAvailability(): Promise<{ configured: boolean; code?: string }> {
  const p = await guideProvider()
  const selected = await core.provider(p)
  const status = await core.user.call<{ configured: boolean; code?: string }>('ProviderStatusRequest', {
    profile: p.auth === 'chatgpt' ? selected.account_profile ?? selected.profile : selected.profile,
    id: selected.provider_id, key_profile: selected.key_profile,
  })
  return status
}

// The relay fixes the guide session to this Web host; tabs never own progress.
const guideSession = () => `web:${ctx.support}`
async function guideState() {
  const status = await guideAvailability()
  const available = status.configured
  const connectionMessage = providerConnectionMessage(status.code)
  const progress = await core.user.call('GuideAdvanceRequest', { session_id: guideSession(), language: settings().language,
    event: { kind: 'connection', connected: available } })
  return { available, connectionMessage, ...progress }
}

// ---- Skills ----------------------------------------------------------------------

interface Skill { name: string; description: string; path: string; source: string; source_folder: string; attachment_folder?: string; status: string; always?: boolean; scoped?: boolean; scope?: string[] }
interface SkillListing { skills: Skill[]; folders: { path: string; status: string }[] }

async function skillMetadata(project?: string, folders = settings().skillFolders.filter((f) => f.enabled).map((f) => f.path), conversationId?: string): Promise<SkillListing> {
  const result = await core.user.call<SkillListing>('SkillsListRequest', { project_path: project, folders, conversation_id: conversationId })
  return { ...result, skills: result.skills.map((s) => ({ ...s,
    description: s.path === 'builtin:demo' ? L('ツグミが画面を飛び回って、デモや画面の使い方を案内します') : s.path === 'builtin:helpers' ? L('依頼されたときの手分けの進め方') : s.description,
    source: s.path.startsWith('builtin:') ? 'Tsugumi' : s.source_folder === (project && path.join(project, 'skills')) ? L('プロジェクト') : path.basename(s.source_folder),
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

interface AccessRequest {
  id: string; state: string; conversation_id: string; project_path?: string
  asked: string[]; groups?: AccessGroup[]
}

const pendingAccess = async () =>
  (await core.user.call<{ requests: AccessRequest[] }>('AccessListRequest')).requests.filter(r => r.state === 'pending')

const answerPermission = (id: string, allow: boolean, selected?: Access[]) =>
  core.user.call('AccessResolveRequest', { request_id: id, allow, selected })

class ChatHost {
  constructor(readonly project: string) {}

  async attachFolders(folders: string[]) {
    try { return await ctx.envs.attachFolders(this.project, folders) }
    catch (error) { throw new Error(L('フォルダを添付できませんでした: {0}', (error as Error).message)) }
  }

  async detachFolder(folder: string) {
    await ctx.envs.removeFolders(this.project, [folder])
  }

  async attachSkillFolders(names: string[], conversationId: string) {
    const skills = (await skillsFor(this.project, conversationId)).filter((s) => s.always || names.includes(s.name))
    const folders = new Set(skills.map((s) => s.attachment_folder).filter((s): s is string => !!s))
    if (folders.size) await this.attachFolders([...folders])
  }

  async sessions() {
    return core.user.call('ConversationListRequest', { project_id: await core.project(this.project) })
  }

  async search(text: string) { return core.user.call('ConversationSearchRequest', { text, project_id: await core.project(this.project) }) }

  async externalCall(name: string, args: unknown, signal?: AbortSignal) {
    const project = await core.project(this.project)
    const id = `external-${project}`
    // A moved transcript needs fresh grants. The tool request asks hostd for
    // its own access without reading that transcript.
    await core.external.call('ConversationCreateRequest', { conversation_id: id, project_id: project, role: 'external' }).catch(error => {
      if (!(error instanceof CoreHostError) || error.code !== 'permission_denied') throw error
    })
    const op = (await core.external.call<{ id: string }>('CallIdRequest', { conversation_id: id, name, arguments_json: typeof args === 'string' ? args : JSON.stringify(args ?? {}) })).id
    try {
      return await core.external.call('ToolCallRequest', { conversation_id: id, project_id: project, name,
        arguments_json: typeof args === 'string' ? args : JSON.stringify(args ?? {}), op_id: op }, signal)
    } catch (error) {
      if (error instanceof CoreHostError && error.code === 'grant_required' && error.requestId) {
        await showExternalAccess(error.requestId, this.project)
        return { text: JSON.stringify({ code: error.code, message: error.message, request_id: error.requestId }), isError: true }
      }
      throw error
    }
  }
}

const chatFor = (dir: string) => new ChatHost(projectKey(dir))

async function showExternalAccess(requestId: string, fallback?: string) {
  const request = (await pendingAccess()).find((r) => r.id === requestId)
  const project = request?.project_path ?? fallback
  if (!request || !project) return
  ctx.broadcast({ type: 'chat', project, session: 'external',
    event: { kind: 'permission', id: requestId, asked: request.asked, groups: request.groups, from: 'Claude Code' } })
}

async function externalAPI(name: string, args: unknown, project?: string, signal?: AbortSignal) {
  try {
    const result = await core.externalAPI(name, args, signal)
    if (name === 'agent_access') {
      const value = JSON.parse(result.text)
      if (typeof value.id === 'string') await showExternalAccess(value.id, project)
    }
    return result
  } catch (error) {
    if (!(error instanceof CoreHostError)) throw error
    if (error.code === 'grant_required' && error.requestId) await showExternalAccess(error.requestId, project)
    return { text: JSON.stringify({ code: error.code, message: error.message, request_id: error.requestId }), isError: true }
  }
}


async function turnAgent(b: any) {
  const host = chatFor(ctx.projectDir(b.path))
  const id = String(b.session)
  await core.conversation(id, host.project)
  await core.configure(await chatProvider(), id)
  return { host, id }
}

function conversationAction(action: string) {
  return async (b: any) => {
    const { id } = await turnAgent(b)
    return core.user.call('ConversationActionRequest', { conversation_id: id, action })
  }
}

export function hostReady() { return core.ready() }

export function hostRequest<T = any>(method: string, fields: unknown = {}): Promise<T> {
  return core.user.call<T>(method, fields)
}

export function chatRoutes(c: ChatContext): Record<string, Handler> {
  ctx = c
  // The language for the server's own messages, from the start (the app's, once read).
  core = new CoreHost(c.support, c.here, c.envs)
  core.observeHost((event) => {
    if (event.type === 'ProviderChanged') c.broadcast({ type: 'providerChanged' })
    if (event.type === 'Connected') c.broadcast({ type: 'reconnected' })
    if (event.type === 'SettingsChanged' && [settingsProfile(), localProfile(), 'desktop'].includes(event.value.profile)) {
      void loadSettings().then(() => c.broadcast({ type: 'settingsChanged' }), (error) => console.error(error))
    }
  })
  const initialSettings = loadSettings()
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
    ...await loadSettings(),
    ...await modelCatalogs(),
    keys: { Anthropic: await keySource('anthropic'), OpenAI: await keySource('openai') },
    keyEnv: { Anthropic: PROVIDERS.anthropic.env, OpenAI: PROVIDERS.openai.env },
  }),
  'POST /api/settings': async (b) => {
    const updated = await patchSettings(b, b.revision)
    return { ...updated, ...await modelCatalogs(), chatProvider: await providerInfo() }
  },
  'POST /api/file/format': (b) => core.user.call('FileFormatRequest', { path: String(b.path), text: String(b.text) }),
  'POST /api/versions/graph': async (b) => {
    const project = await core.project(projectDir(b.path))
    return core.user.call('HostVersionGraphRequest', { project, language: settings().language })
  },
  'POST /api/versions/diff': async (b) => {
    const project = await core.project(projectDir(b.path))
    return core.user.call('HostVersionDiffRequest', { project, commit: String(b.commit), from_commit: b.from })
  },
  'POST /api/versions/comments': async (b) => {
    const project = await core.project(projectDir(b.path))
    return core.user.call('VersionCommentsRequest', { project, commit: String(b.commit) })
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
  'POST /api/chat/new': async (b) => {
    const project = await core.project(projectDir(b.path))
    const result = await core.user.call<{ conversation: { id: string } }>('ConversationCreateRequest', { project_id: project })
    return { id: result.conversation.id }
  },
  'POST /api/chat/conversation': async (b) => {
    const project = projectKey(projectDir(b.path))
    const id = String(b.session)
    const a = await core.conversation(id, project)
    const pending = (await pendingAccess()).filter(r => r.conversation_id === id || r.project_path === project)
    const permissions = pending.map(r => ({ id: r.id, asked: r.asked, groups: r.groups,
      from: r.conversation_id !== id && r.conversation_id !== 'guide' ? 'Claude Code' : undefined }))
    let info = await providerInfo()
    if (a.busy && a.modelBinding) {
      const p = a.modelBinding
      info = { ...info, name: p.providerId === 'chatgpt' ? 'ChatGPT' : p.providerId === 'openai' ? 'OpenAI' : p.providerId === 'fixture' ? 'Local fixture' : 'Anthropic',
        model: p.model, effort: p.effort, fast: p.fast, plan: p.providerId === 'chatgpt', remote: p.providerId !== 'fixture',
        api: p.providerId === 'chatgpt' || p.providerId === 'openai' ? 'openAIResponses' : 'anthropicMessages' }
    }
    return { conversation: a.conversation, busy: a.busy, resumable: a.resumable, interruptedOperation: a.interruptedOperation, continuable: a.continuable,
      eventSeq: String(a.event_seq), provider: info, permissions }
  },
  'POST /api/chat/send': async (b) => {
    const { host, id } = await turnAgent(b)
    const skillNames = Array.isArray(b.skillNames) ? b.skillNames.map(String) : []
    await host.attachSkillFolders(skillNames, id)
    return core.user.call('ConversationSendRequest', {
      conversation_id: id, turn_id: randomUUID(), text: String(b.text ?? ''),
      display: String(b.display ?? b.text ?? ''),
      selected_cards: Array.isArray(b.selectedCards) ? b.selectedCards.map(String) : [],
      selected_displays: Array.isArray(b.selectedDisplays) ? b.selectedDisplays.map(String) : [],
      skill_names: [...new Set(skillNames)],
      scoped_skill: b.scopedSkill ? String(b.scopedSkill) : undefined,
      attached_resources: Array.isArray(b.attachedResources) ? b.attachedResources.map(String) : [],
    })
  },
  'POST /api/chat/thoughts': async (b) => {
    const id = String(b.session)
    await core.conversation(id, projectKey(projectDir(b.path)))
    return core.summarize(id, await purposeProvider('summary'))
  },
  'POST /api/chat/resume': conversationAction('Resume'),
  'POST /api/chat/continue': conversationAction('Continue'),
  'POST /api/chat/retry': conversationAction('Retry'),
  'POST /api/chat/compact': conversationAction('Compact'),
  'POST /api/chat/permission': async (b) => {
    const allow = b.allow === true
    if (allow && Array.isArray(b.always) && b.always.length > 0) {
      const { allowedPermissions, revision } = await loadSettings()
      const merged = [...new Set([...allowedPermissions, ...b.always.map(String)])]
      await patchSettings({ allowedPermissions: merged }, revision)
    }
    await answerPermission(String(b.id), allow, Array.isArray(b.selected) ? b.selected : undefined)
    return { ok: true }
  },
  'POST /api/chat/cancel': async (b) => {
    await core.cancelConversation(String(b.session))
    return { ok: true }
  },
  'POST /api/chat/delete': (b) => core.user.call('ConversationDeleteRequest', { conversation_id: String(b.session) }),
  'POST /api/chat/folders': async (b) => {
    if (!Array.isArray(b.selected)) throw ctx.httpError(400, 'Select at least one folder.')
    const folders = b.selected.map(String)
    const host = chatFor(projectDir(b.path))
    return { folders: await host.attachFolders(folders) }
  },
  'POST /api/chat/folder/detach': async (b) => {
    const project = projectDir(b.path)
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
  'POST /api/agent/call': (b, _url, signal) => String(b.name).startsWith('agent_')
    ? externalAPI(String(b.name), b.arguments, b.path ? projectDir(b.path) : undefined, signal)
    : chatFor(projectDir(b.path)).externalCall(String(b.name), b.arguments, signal),
  'POST /api/project-permissions': () => core.user.call('ProjectPermissionsRequest'),
  'POST /api/project-permissions/set': (b) => core.user.call('ProjectPermissionSetRequest', { project_id: String(b.project_id), resource_id: String(b.resource_id), access: b.access, allow: b.allow === true }),
  'POST /api/agent/access': () => core.user.call('AccessListRequest'),
  'POST /api/agent/access/resolve': async (b) => { await answerPermission(String(b.id), b.allow === true, Array.isArray(b.selected) ? b.selected : undefined); return { ok: true } },
  'POST /api/agent/access/revoke': (b) => core.user.call('AccessRevokeRequest', { grant_id: String(b.id) }),
  'POST /api/guide': async () => {
    const state = await guideState()
    const a = state.available ? await core.conversation('guide', guideProject, 'guide') : undefined
    return { ...state, conversation: a?.conversation ?? null, busy: a?.busy ?? false, eventSeq: a && String(a.event_seq) }
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
    const status = await guideAvailability()
    if (!status.configured) throw ctx.httpError(409, providerConnectionMessage(status.code) ?? L('ガイドに使える API キーがありません。'))
    guideProject = typeof b.project === 'string' && b.project ? projectDir(b.project) : undefined
    await guideTurn()
    return core.user.call('ConversationSendRequest', {
      conversation_id: 'guide', turn_id: randomUUID(), text: String(b.text ?? ''),
    })
  },
  'POST /api/guide/retry': async () => {
    await guideTurn()
    return core.user.call('ConversationActionRequest', { conversation_id: 'guide', action: 'Retry' })
  },
  'POST /api/guide/cancel': async () => {
    await core.cancelConversation('guide')
    return { ok: true }
  },
  'POST /api/guide/reset': () => core.user.call('ConversationDeleteRequest', { conversation_id: 'guide' }),
  }
  return Object.fromEntries(Object.entries(routes).map(([key, route]) => [key, async (body, url, signal) => { await initialSettings; return route(body, url, signal) }]))
}
