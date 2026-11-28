// Chat, Agent and the settings it needs (skill folders, model connection,
// API key), assembled in the page from Host and Agent RPC. Routes are merged
// into the page's local route table (routes.ts).
import { modelChoices } from '../../shared/agentShared'
import type { ProviderConfig } from './types'
import { CoreHost, CoreHostError } from './coreHost'
import { Environments, projectKey } from './environment'
import { L, type Language } from '../i18n'

export interface ChatContext {
  envs: Environments
  projectDir: (p: unknown) => string
  /** To this page's listeners (and, for the guide, the other pages). */
  broadcast: (v: unknown) => void
  httpError: (status: number, message: string) => Error
  /** A CoreHost for tests; the page's own otherwise. */
  core?: CoreHost
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
  /** The chat's Agent may search the web through the provider (its queries
   *  leave for the provider's search service). Off until the user turns it on. */
  webSearch: boolean
  /** Domains web search is limited to, and domains it may not use (hostd passes them to the provider). */
  webSearchAllowedDomains: string[]
  webSearchBlockedDomains: string[]
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
// hostd decides whether a key is host-wide or belongs to this surface, and
// guards all of them with one revision.
const SETTINGS_KEYS: Record<string, string> = {
  provider: 'agent.provider', model: 'agent.model', openAIModel: 'agent.model.openai', chatGPTModel: 'agent.model.chatgpt',
  effort: 'agent.effort', fastModel: 'agent.fastModel', fixturePort: 'agent.fixturePort',
  allowUnscopedHelpers: 'agent.allowUnscopedHelpers',
  animateAgentWork: 'agent.animateWork', webSearch: 'agent.webSearch',
  webSearchAllowedDomains: 'agent.webSearch.allowedDomains', webSearchBlockedDomains: 'agent.webSearch.blockedDomains', language: 'app.language',
  trustedFolders: 'agent.trustedFolders', vmCpus: 'vm.cpus', vmMemoryGiB: 'vm.memoryGiB', vmStoreGiB: 'vm.storeGiB',
  skillFolders: 'agent.skillFolders',
}
// One settings profile per host, shared with the desktop app (owner
// decision): hostd routes each key to host-wide or surface storage, and
// the surface is the desktop app's.
const PROFILE = 'desktop'
const localProfile = () => PROFILE
const providerProfile = () => PROFILE
type SettingsReply = { revision: string; values: { key: string; value_json: string }[] }
let revision = ''
let settingsConnection = 0
let snapshot = {} as Settings
const settings = (): Settings => snapshot
function applySettings(reply: SettingsReply) {
  if (reply.revision < revision) return
  const values = Object.fromEntries(reply.values.map((s) => [s.key, JSON.parse(s.value_json)]))
  snapshot = Object.fromEntries(Object.entries(SETTINGS_KEYS).map(([name, key]) => [name, values[key]])) as unknown as Settings
  revision = reply.revision
}
async function loadSettings() {
  let connection: number
  do {
    connection = settingsConnection
    const reply = await core.getSettings(localProfile())
    if (connection === settingsConnection) applySettings(reply)
  } while (connection !== settingsConnection)
  return { ...snapshot, revision }
}
async function patchSettings(change: Partial<Settings>, expected?: string, chatGPTAccount?: string) {
  const changes = Object.entries(SETTINGS_KEYS).filter(([name]) => name in change).map(([name, key]) => ({ key, value_json: JSON.stringify(change[name as keyof Settings]) }))
  if (!changes.length) return loadSettings()
  const connection = settingsConnection
  const chatgpt_account = chatGPTAccount && changes.some(change => change.key === SETTINGS_KEYS.provider)
    ? { profile: localProfile(), client_id: chatGPTAccount } : undefined
  const reply = await core.user.call<SettingsReply>('SettingsUpdateRequest', {
    profile: localProfile(), revision: expected ?? (await core.getSettings(localProfile())).revision, changes, chatgpt_account,
  })
  if (connection !== settingsConnection) return loadSettings()
  applySettings(reply)
  return { ...snapshot, revision }
}
const PROVIDERS = { anthropic: { name: 'Anthropic' }, openai: { name: 'OpenAI' } } as const
type KeyedProvider = keyof typeof PROVIDERS
function keyedProvider(id: unknown): KeyedProvider {
  if (id === 'anthropic' || id === 'openai') return id
  throw ctx.httpError(400, 'Unknown provider')
}
async function keySource(p: KeyedProvider) {
  return core.keys.source(p, providerProfile())
}
async function saveKey(p: KeyedProvider, key: string) {
  await core.keys.save(p, key, providerProfile())
  return L('hostd に保存しました。キーは Card やプロジェクト・設定・ログには残りません。')
}
async function deleteKey(p: KeyedProvider) {
  await core.keys.delete(p, providerProfile())
  return L('{0} のキーを削除しました。', PROVIDERS[p].name)
}

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
  return { ...await providerDisplay(id), profile: providerProfile(), accountProfile: localProfile() }
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

interface ChatGPTAccount {
  clientId: string
  email?: string
  signedIn: boolean
  planEnabled: boolean
  active: boolean
}
interface SignInStatus {
  accounts: ChatGPTAccount[]
  attempt_id?: string
  client_id?: string
  state: string
  planEnabled: boolean
  message?: string
  welcome?: string
  revoked: boolean
}
const testIssuer = () => (globalThis as { TSUGUMI_CHATGPT_ISSUER?: string }).TSUGUMI_CHATGPT_ISSUER
const testResource = () => (globalThis as { TSUGUMI_CHATGPT_API?: string }).TSUGUMI_CHATGPT_API
const signInStatus = (attempt_id?: string) =>
  hostRequest<SignInStatus>('SignInStatusRequest', { profile: localProfile(), attempt_id })
const signInAction = (action: string, fields: { client_id?: string; attempt_id?: string }) =>
  hostRequest<SignInStatus>('SignInActionRequest', { profile: localProfile(), action, ...fields })

function chatGPTState(status: SignInStatus) {
  const account = status.accounts.find(a => a.clientId === status.client_id)
  const outcome = status.state === 'complete' && account
    ? { ok: true, clientId: account.clientId, planEnabled: status.planEnabled, welcome: status.welcome, message: status.planEnabled
      ? L('{0} でサインインしました。', account.email ?? account.clientId)
      : L('サインインしましたが、ChatGPT プランの利用は許可されませんでした。「プランの利用を許可」から許可するか、API キーの接続を使ってください。') }
    : status.state === 'cancelled' ? { ok: false, message: L('サインインを中止しました。') }
      : status.state === 'failed' ? { ok: false, message: status.message } : null
  const active = status.accounts.find(a => a.active)
  return { accounts: status.accounts, signingIn: ['pending', 'committing'].includes(status.state),
    attemptId: status.attempt_id, outcome, usageURL: 'https://chatgpt.com/settings/usage',
    connectionMessage: active && providerConnectionMessage(!active.signedIn ? 'chatgpt_sign_in_required'
      : !active.planEnabled ? 'chatgpt_plan_disabled' : undefined) }
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
  const value = await core.user.call<{ provider: { providerId: ProviderId; model: string } }>('ModelSelectionRequest', { profile: providerProfile(), purpose })
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
    id: selected.provider_id,
  })
  return status
}

// The relay fixes the guide session to this Web host; tabs never own progress.
const guideSession = () => PROFILE
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

/** The UI's own one-line description of a built-in Skill (hostd's is in English). */
export function builtinSkillDescription(path: string): string | undefined {
  switch (path) {
    case 'builtin:demo': return L('ツグミが画面を飛び回って、デモや画面の使い方を案内します')
    case 'builtin:helpers': return L('依頼されたときの手分けの進め方')
    case 'builtin:review': return L('選択した Card と前提を読む')
    case 'builtin:rerun-compare': return L('選択した Card を再実行して出力の内容を比較する')
    default: return undefined
  }
}

async function skillMetadata(project?: string, folders = settings().skillFolders.filter((f) => f.enabled).map((f) => f.path), conversationId?: string): Promise<SkillListing> {
  const result = await core.user.call<SkillListing>('SkillsListRequest', { project_path: project, folders, conversation_id: conversationId })
  return { ...result, skills: result.skills.map((s) => ({ ...s,
    description: builtinSkillDescription(s.path) ?? s.description,
    source: s.path.startsWith('builtin:') ? 'Tsugumi' : s.source_folder === (project && `${project}/skills`) ? L('プロジェクト') : (s.source_folder.replace(/\/+$/, '').split('/').pop() ?? ''),
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

interface Decision {
  id: string; state: string; conversation_id: string; project_path?: string
  asked: string[]; call_id?: string
}

/** Pending requests, read page by page within the RPC frame budget. */
async function pendingDecisions(filter: { conversation_id?: string; project_id?: string } = {}) {
  const pending: Decision[] = []
  let cursor = '0'
  do {
    const page = await core.user.call<{ decisions: Decision[]; next_cursor: string }>(
      'DecisionListRequest', { ...filter, cursor, pending_only: true })
    pending.push(...page.decisions.filter(r => r.state === 'pending'))
    cursor = page.next_cursor
  } while (cursor && cursor !== '0')
  return pending
}

const answerDecision = (id: string, allow: boolean) =>
  core.user.call('DecisionResolveRequest', { request_id: id, allow })

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


}

const chatFor = (dir: string) => new ChatHost(projectKey(dir))

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

export function hostRequest<T = any>(method: string, fields: unknown = {}): Promise<T> {
  return core.user.call<T>(method, fields)
}

export function chatRoutes(c: ChatContext): Record<string, Handler> {
  ctx = c
  // The language for the server's own messages, from the start (the app's, once read).
  core = c.core ?? new CoreHost(c.envs)
  revision = ''
  core.observeHost((event) => {
    if (event.type === 'ProviderChanged') c.broadcast({ type: 'providerChanged' })
    if (event.type === 'Connected') c.broadcast({ type: 'reconnected' })
    if (['SignInChanged', 'ProviderChanged'].includes(event.type) && event.value.profile === localProfile())
      c.broadcast({ type: 'chatgptChanged' })
    const connected = event.type === 'Connected'
    if (connected) {
      // A new host connection may be a new store; its revisions start over.
      settingsConnection++
      revision = ''
    }
    if (connected || event.type === 'SettingsChanged') {
      const connection = settingsConnection
      void loadSettings().then(() => {
        if (connection === settingsConnection) c.broadcast({ type: 'settingsChanged' })
      }).catch(error => console.error('Host settings refresh failed:', error))
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
  }),
  'POST /api/settings': async (b) => {
    const account = b?.provider === 'chatgpt' && typeof b.chatGPTAccount === 'string' ? b.chatGPTAccount : undefined
    const updated = await patchSettings(b, b.revision, account)
    return { ...updated, ...await modelCatalogs(), chatProvider: await providerInfo() }
  },
  'POST /api/versions/note': async (b) => {
    const project = await core.project(projectDir(b.path))
    return core.user.call('VersionNoteRequest', { project_id: project, commit: String(b.commit), language: settings().language })
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
    chatgpt: (await signInStatus()).accounts.some(a => a.active && a.signedIn && a.planEnabled),
  }),
  'GET /api/chatgpt': async (_b, url) => chatGPTState(await signInStatus(url.searchParams.get('attemptId') ?? undefined)),
  'POST /api/chatgpt/signin': async (b) => {
    // Checks set these on the page (like TSUGUMI_LANGUAGE) to use the loopback OAuth fixture.
    const { url, attempt_id } = await hostRequest<{ url: string; attempt_id: string }>('SignInBeginRequest', {
      profile: localProfile(), client_id: typeof b.clientId === 'string' ? b.clientId : undefined,
      consent: b.consent === true, issuer: testIssuer(), resource: testResource(),
    })
    return { url, attemptId: attempt_id }
  },
  'POST /api/chatgpt/cancel': async (b) => {
    await signInAction('cancel', { attempt_id: String(b.attemptId) })
    return {}
  },
  'POST /api/chatgpt/select': async (b) => {
    await signInAction('select', { client_id: String(b.clientId) })
    return {}
  },
  'POST /api/chatgpt/welcomed': async (b) => {
    await signInAction('welcomed', { client_id: String(b.clientId) })
    return {}
  },
  'POST /api/chatgpt/signout': async (b) => {
    const id = String(b.clientId)
    const status = await signInAction('signout', { client_id: id })
    const email = status.accounts.find(a => a.clientId === id)?.email ?? id
    return { message: status.revoked ? L('{0} からサインアウトしました。', email)
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
    const pending = (await pendingDecisions()).filter(r => r.conversation_id === id || r.project_path === project)
    const permissions = pending.map(r => ({ id: r.id, asked: r.asked, call_id: r.call_id,
      from: r.conversation_id !== id && r.conversation_id !== 'guide' ? 'Claude Code' : undefined }))
    let info = await providerInfo()
    if (a.busy && a.modelBinding) {
      const p = a.modelBinding
      info = { ...info, name: p.providerId === 'chatgpt' ? 'ChatGPT' : p.providerId === 'openai' ? 'OpenAI' : p.providerId === 'fixture' ? 'Local fixture' : 'Anthropic',
        model: p.model, effort: p.effort, fast: p.fast, plan: p.providerId === 'chatgpt', remote: p.providerId !== 'fixture',
        api: p.providerId === 'chatgpt' || p.providerId === 'openai' ? 'openAIResponses' : 'anthropicMessages' }
    }
    return { conversation: a.conversation, busy: a.busy, resumable: a.resumable, interruptedOperation: a.interruptedOperation, continuable: a.continuable,
      runWake: a.runWake !== false, eventSeq: String(a.event_seq), provider: info, permissions }
  },
  'POST /api/chat/send': async (b) => {
    const { host, id } = await turnAgent(b)
    const skillNames = Array.isArray(b.skillNames) ? b.skillNames.map(String) : []
    await host.attachSkillFolders(skillNames, id)
    return core.user.call('ConversationSendRequest', {
      conversation_id: id, turn_id: crypto.randomUUID(), text: String(b.text ?? ''),
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
    await answerDecision(String(b.id), allow)
    return { ok: true }
  },
  'POST /api/chat/runWake': async (b) => {
    await core.conversation(String(b.session), projectKey(projectDir(b.path)))
    await core.user.call('ConversationRunWakeRequest', { conversation_id: String(b.session), run_wake: b.runWake === true })
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
      conversation_id: 'guide', turn_id: crypto.randomUUID(), text: String(b.text ?? ''),
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
