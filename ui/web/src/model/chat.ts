import { ConversationEvents } from '../wire/agentEvents'
import { onHostEvent } from '../wire/host'
import * as H from '../../shared/gen/tsugumi/host'
import type { Access } from '../../shared/gen/tsugumi/service/access'
// The chat of one project: several conversations (sessions), each an Agent
// run by the host server with its own persisted transcript.
import { api, onServerEvent } from '../api'
import type { ChatRow, AccessGroup } from '../../shared/chatPresentation'
import { s, uuid } from '../wire/rpc'
import type { AppModel } from './app'
import { guide, GuideAction, type GuideShow, parseShow } from './guide'
import { agentAppearance } from './agentAppearance'
import { HelperTeam } from './helperBirds'
import { LiveCodePlayer, writesSources } from './liveCode'
import { Observable } from './observable'
import { Thoughts, type ThoughtReply } from './thoughts'
import { CardKey, ProjectSession, path } from './session'
import { L, modelErrorMessage } from '../i18n'
import { cardRequest } from '../../shared/agentShared'

export interface ChatItem {
  id: string
  role: string
  text: string
  tool?: { name: string; ok?: boolean }
  archived?: boolean
  /** A helper's row (role "helper", delegate): its name, its current or last
   *  tool call (text: the arguments) and how its work ended. */
  helper?: string
  ended?: string
  presentation?: ChatRow
}

export interface SessionInfo {
  id: string
  title: string
  updatedAt: number
  messages: number
}

export type Reference = { kind: 'card' | 'display'; card: CardKey }
export const refLabel = (r: Reference, display = (k: CardKey) => k) => (r.kind === 'card' ? `Card ${display(r.card)}` : L('可視化 {0}', display(r.card)))
const refEq = (a: Reference, b: Reference) => a.kind === b.kind && a.card === b.card

export interface Skill {
  name: string
  description: string
  path: string
  content: string
  source: string
  scoped?: boolean
  scope?: string[]
}

/** The model connection as the host server reports it (never the key). */
export interface ProviderInfo {
  name: string
  model: string
  api: 'anthropicMessages' | 'openAIResponses'
  /** A real provider (with a model choice), not the local fixture. */
  remote: boolean
  /** Requests use the user's ChatGPT plan (Sign in with ChatGPT). */
  plan: boolean
  /** What goes into the requests, from the model menu: the effort the model
   *  takes (none: its default) and Fast mode. */
  effort?: string
  fast?: boolean
  /** Display names of the models (the ChatGPT account's catalog). */
  modelNames: Record<string, string>
  thoughtModel?: string
}

export const USAGE_URL = 'https://chatgpt.com/settings/usage'

interface ContentBlock {
  type: string
  id?: string
  tool_use_id?: string
  text?: string
  name?: string
  input?: string
  content?: string
  is_error?: boolean
  /** A web search the provider ran. */
  query?: string
  sources?: { url: string; title?: string }[]
  error?: string
}

interface Message {
  role: string
  content: ContentBlock[]
  display?: string
}

interface Conversation {
  id: string
  rows: ChatRow[]
  helpers?: { call_id: string; task: number; messages: Message[]; state: string }[]
  tools?: { id: string; name: string; result?: string; isError: boolean; state: string }[]
  messages: Message[]
  archive: Message[]
  usage: { inputTokens: number; outputTokens: number; cacheReadTokens?: number; cacheWriteTokens?: number }
  summary?: string
}

interface InterruptedOperation { kind: string; id?: string; name?: string; arguments?: string }

const COMPACTING = L('会話を圧縮しています…')

export class ChatController extends Observable {
  items: ChatItem[] = []
  busy = false
  /** Stop was pressed and that answer has not reported its end yet. */
  private stopping = false
  private transition: Promise<unknown> = Promise.resolve()
  private turn = 0
  private reload = 0
  private contentRevision = 0
  loading = false
  private displayedId = ''
  get ready() { return !this.loading && !this.loadError && !!this.currentId && this.displayedId === this.currentId }
  /** A save of the Agent waiting for the user: what it would give Cards. */
  /** A save waiting for the user's approval: of this chat's Agent, or of
   *  Claude Code (from) working on the project through the plugin. */
  private permissions: { id: string; asked: string[]; groups?: AccessGroup[]; from?: string; call_id?: string }[] = []
  get permission() { return this.permissions?.[0] }
  private enqueuePermission(p: { id: string; asked: string[]; groups?: AccessGroup[]; from?: string; call_id?: string }) {
    this.permissions ??= []
    if (!this.permissions.some(x => x.id === p.id)) {
      this.permissionRevision++
      this.permissions.push(p)
    }
  }
  private removePermission(id: string) {
    this.permissionRevision++
    this.permissions = (this.permissions ?? []).filter(p => p.id !== id)
  }
  loadError?: string
  status?: string
  failed = false
  /** The last turn stopped at the ChatGPT plan's usage limit ("Manage usage"). */
  usageLimited = false
  /** The last turn could not make the conversation fit the model's context. */
  contextFull = false
  /** A stopped turn can be continued explicitly. */
  canContinue = false
  interrupted = false
  interruptedOperation?: InterruptedOperation
  sessions: SessionInfo[] = []
  currentId = ''
  usage: { inputTokens: number; outputTokens: number; cacheReadTokens?: number; cacheWriteTokens?: number } = { inputTokens: 0, outputTokens: 0 }
  skills: Skill[] = []
  references: Reference[] = []
  /** Folders of this host attached to this chat: read by the Agent
   *  (attached_folder_read) only, read only. */
  folders: string[] = []
  available: boolean
  provider: ProviderInfo = { name: 'Anthropic', model: 'claude-sonnet-5-5', api: 'anthropicMessages', remote: true, plan: false, modelNames: {} }

  private readonly offEvents: () => void
  private readonly offHost: () => void
  private readonly events = new ConversationEvents((event) => this.handle(event), (error) => this.fail(error))
  private permissionRevision = 0
  private readonly offSession: () => void
  private providerRefresh = 0
  readonly thoughts: Thoughts

  constructor(readonly session: ProjectSession, app: AppModel) {
    super()
    this.thoughts = new Thoughts(() => ({ identity: `${this.currentId}:${this.turn}`, active: this.busy && !!this.provider.thoughtModel }),
      () => api<ThoughtReply>('/api/chat/thoughts', this.base),
      (text) => guide.chatThought(this, text), () => this.changed())
    this.thoughts.start()
    this.available = app.status?.chatAvailable ?? true
    if (!this.available) this.status = app.status?.chatError ?? L('RPC スキーマを読み込めないため、チャットは使えません。通常の操作はすべて UI から行えます。')
    this.offEvents = onServerEvent((e) => {
      if (e.type === 'chat' && e.project === session.dir && e.session === 'external') this.handle(e.event)
      // Progress sent while the stream was down: the conversation is read again.
      if (e.type === 'reconnected' && this.currentId) void this.reread(this.currentId)
      if (e.type === 'settingsChanged' && this.currentId) {
        // A local write already returned the next turn's connection. During a
        // turn the conversation snapshot remains authoritative.
        if (e.provider && !this.busy) {
          this.providerRefresh++
          this.provider = e.provider
          this.changed()
        } else void this.refreshProvider()
      }
    })
    this.offHost = onHostEvent((event) => {
      if (event.valueType !== H.EventPayload.ConversationChanged) return
      const id = s((event.value as H.ConversationChangedT).answeredPermission)
      if (id && this.permissions.some(p => p.id === id)) this.handle({ kind: 'permissionAnswered', id })
    })
    let run = session.currentRunId
    this.offSession = session.subscribe(() => {
      const latest = session.currentRunId
      if (latest && latest !== run && session.runActive) {
        this.showRunning(Date.now() / 1000)
        if (guide.contextSession === session) guide.chatWork('run', '{}')
        this.changed()
      }
      run = latest
    })
    void this.init()
  }

  /** The project was left: this chat hears nothing more. */
  close() {
    this.providerRefresh++
    this.turn++
    this.reload++
    this.thoughts.close()
    this.offEvents()
    this.offHost()
    this.events.close()
    this.offSession()
    this.permissions = []
    this.team.endAll('stopped', Date.now() / 1000)
    guide.chatClosed(this)
  }

  private get base() {
    return { path: this.session.dir, session: this.currentId }
  }

  private async init() {
    const turn = this.turn
    if (!await this.refreshSessions() || turn !== this.turn) return
    try {
      const id = this.sessions[0]?.id ?? await this.createSession()
      if (turn !== this.turn) return
      this.select(id)
      void this.reloadSkills()
    } catch (e) {
      if (turn === this.turn) this.showLoadError(e)
    }
  }

  // ---- Sessions ------------------------------------------------------------------

  async refreshSessions() {
    try {
      this.sessions = await api<SessionInfo[]>('/api/chat/sessions', { path: this.session.dir })
      this.changed()
      return true
    } catch (e) {
      if (this.currentId) {
        this.status = L('エラー: {0}', (e as Error).message ?? e)
        this.changed()
      } else this.showLoadError(e)
      return false
    }
  }

  private showLoadError(error: unknown) {
    this.loading = false
    this.status = L('エラー: {0}', (error as Error).message ?? error)
    this.loadError = this.status
    this.changed()
  }

  /** Reads the open conversation again (its turn keeps running on the server). */
  private async reread(id: string) {
    const request = ++this.reload, turn = this.turn, revision = this.contentRevision, permissions = this.permissionRevision
    try {
      const r = await api<{ conversation: Conversation; busy: boolean; resumable?: boolean; interruptedOperation?: InterruptedOperation; continuable?: boolean; provider: ProviderInfo; eventSeq: string; permissions: { id: string; asked: string[]; groups?: AccessGroup[]; from?: string; call_id?: string }[] }>(
        '/api/chat/conversation', { path: this.session.dir, session: id, folders: this.folders })
      if (this.currentId !== id || request !== this.reload || turn !== this.turn) return
      // An observation received after the read began is newer than this snapshot.
      if (revision === this.contentRevision) {
        if (r.conversation.id !== id) throw new Error('The host returned a different conversation')
        const items = r.conversation.rows.map(presentedItem)
        this.items = items
        this.busy = r.busy
        this.adoptInterruption(r)
        this.usage = r.conversation.usage
        this.provider = r.provider
      }
      if (permissions === this.permissionRevision) this.permissions = r.permissions ?? []
      if (r.eventSeq !== undefined) this.events?.start(id, BigInt(r.eventSeq))
      if (this.status === this.loadError) this.status = undefined
      this.loadError = undefined
      this.displayedId = id
      this.loading = false
      this.changed()
    } catch (e) {
      if (this.currentId !== id || request !== this.reload || turn !== this.turn) return
      this.showLoadError(e)
    }
  }

  retryLoad() { if (this.currentId) this.select(this.currentId); else void this.init() }

  select(id: string) {
    this.events?.close()
    this.turn++
    guide.chatThought(this)
    this.busy = false
    this.failed = false
    this.usageLimited = false
    this.contextFull = false
    this.canContinue = false
    this.interrupted = false
    this.interruptedOperation = undefined
    if (this.available) this.status = undefined
    this.permissions = []
    this.loadError = undefined
    this.currentId = id
    this.loading = true
    this.changed()
    if (!this.available) { this.loading = false; return }
    void this.reread(id)
  }

  private adoptInterruption(r: { resumable?: boolean; interruptedOperation?: InterruptedOperation; continuable?: boolean }) {
    const wasInterrupted = this.interrupted
    this.interrupted = r.resumable === true
    this.interruptedOperation = r.interruptedOperation
    this.canContinue = !this.interrupted && r.continuable === true
    if (this.interrupted) {
      this.failed = false
      this.usageLimited = false
      this.contextFull = false
      const op = r.interruptedOperation
      const doing = op?.kind === 'tool' ? `${op.name} (${op.id}) ${op.arguments ?? ''}`
        : op?.kind === 'compact' ? L('会話の圧縮')
        : op?.kind === 'preparing' ? L('メッセージの準備') : L('モデルの応答')
      this.status = L('会話は中断しています。中断時の処理: {0}。再開するか、新しいメッセージを送れます。', doing)
    } else if (wasInterrupted) this.status = undefined
  }

  resume() {
    if (!this.ready || !this.available || !this.interrupted || this.busy) return
    this.begin()
    void this.request('/api/chat/resume', { ...this.base, folders: this.folders }).catch((e) => this.fail(e))
  }

  private async createSession(): Promise<string> {
    return (await api<{ id: string }>('/api/chat/new', { path: this.session.dir })).id
  }

  newSession() {
    void this.createSession().then(id => {
      this.select(id)
      void this.refreshSessions()
    }).catch(e => this.fail(e))
  }

  async delete(id: string) {
    await api('/api/chat/delete', { path: this.session.dir, session: id }).catch(() => {})
    await this.refreshSessions()
    if (id === this.currentId) this.select(this.sessions[0]?.id ?? await this.createSession())
  }

  /** Re-reads the connection (after Settings > API changed it). */
  async refreshProvider() {
    if (!this.currentId) return
    const refresh = ++this.providerRefresh, turn = this.turn
    try {
      const id = this.currentId
      const r = await api<{ provider: ProviderInfo }>('/api/chat/conversation', { ...this.base })
      if (this.currentId !== id || this.turn !== turn || refresh !== this.providerRefresh) return
      this.provider = r.provider
      this.changed()
    } catch {}
  }

  /** Only visible presentation moments ask the host; leaving the panel removes the listeners. */
  observePresentation(page: Pick<Document, 'visibilityState' | 'addEventListener' | 'removeEventListener'> = document,
    viewport: Pick<Window, 'addEventListener' | 'removeEventListener'> = window) {
    const refresh = () => { if (page.visibilityState !== 'hidden') void this.refreshProvider() }
    page.addEventListener('visibilitychange', refresh)
    viewport.addEventListener('focus', refresh)
    refresh()
    return () => {
      page.removeEventListener('visibilitychange', refresh)
      viewport.removeEventListener('focus', refresh)
    }
  }

  // ---- Skills, references and folders ------------------------------------------

  async reloadSkills() {
    try {
      this.skills = await api<Skill[]>('/api/skills', { path: this.session.dir })
    } catch {}
    this.changed()
  }

  addReference(r: Reference) {
    if (!this.references.some((x) => refEq(x, r))) this.references = [...this.references, r]
    this.changed()
  }

  removeReference(r: Reference) {
    this.references = this.references.filter((x) => !refEq(x, r))
    this.changed()
  }

  /** Attaches folders in one operation (the server validates each one). */
  private folderResources = new Map<string, string>()

  async attach(folders: string[]) {
    if (!folders.length) return
    const r = await this.session.withEnvironmentChange(() => api<{ folders: { folder: string; resource: string }[] }>('/api/chat/folders', { ...this.base, selected: [...new Set(folders)] }))
    for (const item of r.folders) this.folderResources.set(item.folder, item.resource)
    this.folders = [...new Set([...this.folders, ...r.folders.map(item => item.folder)])]
    this.changed()
  }

  /** No longer offered to the Agent. */
  detach(folder: string) {
    this.folders = this.folders.filter((f) => f !== folder)
    const resource = this.folderResources.get(folder)
    this.folderResources.delete(folder)
    if (resource) void api('/api/chat/folder/detach', { ...this.base, resource, folder }).catch((e) => this.fail(e))
    this.changed()
  }

  /** `$name` tokens that match a skill. */
  skillsIn(text: string): Skill[] {
    const names = new Set(text.split(/\s+/).filter((t) => t.startsWith('$')).map((t) => t.slice(1)))
    return this.skills.filter((x) => names.has(x.name))
  }

  // ---- Conversation --------------------------------------------------------------

  send(text: string, selection: CardKey[] = [], scopedSkill?: string) {
    if (!this.ready || !text.trim() || !this.available) return
    // No model connection: Tsugumi answers from its script.
    if (guide.scripted && !this.busy) return guide.answerScripted(text.trim(), this)
    if (this.skillsIn(text).some((s) => s.name === 'demo')) this.demoMode = true
    const refs = [...this.references, ...selection.filter((k) => !this.references.some((r) => r.kind === 'card' && r.card === k)).map((card) => ({ kind: 'card' as const, card }))]
    const shown = text + (refs.length ? '\n📎 ' + refs.map(r => refLabel(r, k => this.session.displayPath(path(k)))).join(', ') : '')
    this.contentRevision++
    this.items = [...this.items, { id: uuid(), role: 'user', text: shown }]
    this.references = []
    this.begin()
    // The chat shows what was typed, not the skills and references attached to it.
    void this.request('/api/chat/send', { ...this.base, text, display: shown, scopedSkill, selectedCards: refs.filter((r) => r.kind === 'card').map((r) => r.card), selectedDisplays: refs.filter((r) => r.kind === 'display').map((r) => r.card), skillNames: this.skillsIn(text).map((s) => s.name), attachedResources: [...this.folderResources.values()] }).catch((e) => this.fail(e))
  }

  /** Sends a request the app wrote for the model; the chat shows `display`. */
  private sendRequest(display: string, prompt: string) {
    if (!this.ready || !this.available) return
    this.items = [...this.items, { id: uuid(), role: 'user', text: display }]
    this.begin()
    void this.request('/api/chat/send', { ...this.base, text: prompt, display, skillNames: this.demoMode ? ['demo'] : [] }).catch((e) => this.fail(e))
  }

  /** Asks the Agent to implement the Draft Cards of a scope (after they were wired and saved). */
  generateDrafts(scope: string[], cards: CardKey[]) {
    this.sendRequest(L('Draft {0} 枚から Card を生成します', cards.length),
      cardRequest('draftRequest', scope, cards))
  }

  /** The identifiers are explicit user selections; core reads their comments after authorization. */
  generateFromComments(scope: string[], cards: CardKey[]) {
    this.sendRequest(L('選択した Card のコメントに従って生成します'),
      cardRequest('commentRequest', scope, cards))
  }

  cancel() {
    this.turn++
    const turn = this.turn
    guide.chatThought(this)
    void this.request('/api/chat/cancel', this.base).then(() => {
      if (this.turn === turn) void this.refreshProvider()
    }).catch((error) => { if (this.turn === turn) this.fail(error) })
    this.stopping = true
    this.canContinue = true
    this.busy = false
    this.status = L('回答を停止しました。受理済みの実行は止まりません（実行の停止は Canvas の「停止」）。')
    this.changed()
  }

  /** Continues work only when the person asks. */
  continueWork() {
    if (!this.ready || !this.available) return
    this.items = [...this.items, { id: uuid(), role: 'user', text: L('（止まったところから続けます）') }]
    this.begin()
    void this.request('/api/chat/continue', { ...this.base, folders: this.folders }).catch((e) => this.fail(e))
  }

  retry() {
    this.begin()
    void this.request('/api/chat/retry', { ...this.base, folders: this.folders }).catch((e) => this.fail(e))
  }

  compact() {
    this.begin()
    this.status = COMPACTING
    this.changed()
    void this.request('/api/chat/compact', { ...this.base, folders: this.folders }).catch((e) => this.fail(e))
  }

  /** Demo mode ($demo, or a demo tour): the bird leads the user about the
   *  screen for this chat; it ends with the tour's farewell. Otherwise the chat
   *  is a chat, whose bird flies only to show where a feature asked about is. */
  demoMode = false

  /** Replies to tap and buttons Tsugumi offered with its last answer (guide_show). */
  choices: string[] = []
  offered: GuideAction[] = []
  /** Without a model connection: Tsugumi's fixed reply from hostd to what
   *  was asked or what the app noted, with its choices and buttons. */
  showScripted(asked: string | undefined, reply: string, shown: GuideShow) {
    this.items = [...this.items, ...(asked ? [{ id: uuid(), role: 'user', text: asked }] : []), { id: uuid(), role: 'assistant', text: reply }]
    this.choices = shown.choices
    this.offered = shown.actions
    this.changed()
  }

  /** Notes of the app (a demo tour's runs) that came while it answered. */
  private notes: { prompt: string; display: string }[] = []

  /** Something the app tells Tsugumi (a tour step, a run that ended); one that
   *  comes while it answers waits for the answer to end. */
  tell(prompt: string, display: string) {
    // A tour is demo mode: the skill comes with its first note.
    this.demoMode = true
    prompt = display
    if (this.busy) {
      this.notes.push({ prompt, display })
      return
    }
    this.sendRequest(display, prompt)
  }

  private flushNotes() {
    if (!this.notes.length) return
    const n = this.notes
    this.notes = []
    this.sendRequest(n[n.length - 1].display, n.map((x) => x.prompt).join('\n'))
  }

  private get lastReply() {
    return [...this.items].reverse().find((x) => x.role === 'assistant')?.text ?? ''
  }

  private begin() {
    this.turn++
    guide.chatThought(this)
    this.choices = []
    this.offered = []
    guide.chatTurnBegan(this)
    this.busy = true
    this.interrupted = false
    this.interruptedOperation = undefined
    this.failed = false
    this.usageLimited = false
    this.contextFull = false
    this.canContinue = false
    this.status = undefined
    this.changed()
  }

  private request(endpoint: string, body: Record<string, unknown>) {
    const turn = this.turn
    const pending = this.transition.catch(() => {}).then(async () => {
      const result = await api(endpoint, body)
      if (endpoint !== '/api/chat/cancel' && turn === this.turn) void this.refreshProvider()
      return result
    })
    this.transition = pending
    return pending
  }

  private fail(e: unknown) {
    this.busy = false
    this.failed = true
    this.status = L('エラー: {0}', (e as Error).message ?? e)
    this.changed()
    void this.refreshProvider()
  }

  answerPermission(allow: boolean, id = this.permission?.id, selected?: string[]) {
    const p = this.permission
    if (!p || p.id !== id) return
    this.removePermission(p.id)
    this.changed()
    const access = selected?.flatMap(line => {
      const prefix = 'project_access {'
      if (!line.startsWith(prefix)) return []
      const { access_id } = JSON.parse(line.slice('project_access '.length)) as { access_id: Access }
      return [access_id]
    })
    void api('/api/chat/permission', { id: p.id, allow, selected: access }).catch((e) => this.fail(e))
  }

  /** The files being written, by who writes them ('': the Agent, or a helper's name). */
  readonly writers = new Map<string, LiveCodePlayer>()

  /** The helpers of the last delegate call, a bird each. */
  readonly team = new HelperTeam((who) => this.writers.get(who), () => agentAppearance.animateWork)

  private writer(who = '') {
    let w = this.writers.get(who)
    if (!w) this.writers.set(who, (w = new LiveCodePlayer()))
    return w
  }

  private showRunning(now: number) {
    for (const writer of this.writers.values()) writer.stop()
    this.team.running(now)
  }

  /** What an event means for the file being written; true: nothing else to do. */
  private noteWriting(who: string, e: any) {
    const now = Date.now() / 1000
    switch (e.kind) {
      case 'text':
        this.session.presence?.finish()
        this.writers.get(who)?.stop()
        return false
      case 'toolInput':
        this.session.presence?.finish()
        // Many a second, so nothing else changes: the panel looks for itself.
        if (!writesSources(e.name)) return true
        this.writer(who).input(e.index, e.delta, now)
        if (!who && !this.demoMode) guide.writer = this.writer()
        return true
      case 'toolFinished':
        if (writesSources(e.name)) {
          this.writer(who).ended(e.ok, now)
          if (e.ok && e.name === 'project_save') this.writer(who).saved(now)
        }
        return false
      case 'toolStarted':
        this.session.presence?.finish()
        // A new tool supersedes the previous file's presentation hold.
        if (!writesSources(e.name)) this.writers.get(who)?.stop()
        if (e.name === 'run') this.showRunning(now)
        return false
      case 'finished':
        this.writer(who).ended(true, now)
        return false
      case 'discardText': case 'retrying': case 'error': case 'cancelled':
        this.writer(who).ended(false, now)
        return false
    }
    return false
  }

  private handle(e: any) {
    if (this.noteWriting('', e)) return
    if (e.kind === 'helper' && this.noteWriting(e.name, e.event)) return
    switch (e.kind) {
      case 'presentation':
        this.contentRevision++
        this.items = e.rows.map(presentedItem)
        break
      case 'permission':
        // A turn approval's event names only the request; its description is
        // read with the conversation's pending requests.
        if (!e.asked?.length) {
          if (this.currentId) void this.reread(this.currentId)
          break
        }
        this.enqueuePermission({ id: e.id, asked: e.asked, groups: e.groups, from: e.from, call_id: e.call_id })
        break
      case 'permissionAnswered':
        this.removePermission(e.id)
        break
      case 'toolStarted':
        if (e.name === 'guide_show') {
          // The bird flies and speaks; the chat keeps the choices under the answer.
          const shown = parseShow(e.arguments)
          this.choices = shown.choices
          this.offered = shown.actions
          // The bird flies for it: in demo mode, and outside it to show where a
          // feature the user asked about is (the rules keep it to that).
          guide.chatShow(this, e.arguments)
          if (shown.endTour) this.demoMode = false
          break
        }
        // The bird flies to the Card the Agent works on (in demo mode guide_show moves it instead).
        if (!this.demoMode) guide.chatWork(e.name, e.arguments)
        if (e.name === 'delegate') this.startHelpers(e.arguments)
        break
      case 'helperStarted':
        this.startHelpers(JSON.stringify({ tasks: [JSON.parse(e.text)] }))
        break
      case 'helper':
        this.handleHelper(e.name, e.event)
        break
      case 'compacting':
        // The next reply waits for it (over a minute with a long conversation).
        this.status = COMPACTING
        break
      case 'compacted':
        // Earlier messages were summarized for the model; the whole text stays
        // in the conversation, so nothing is announced.
        if (this.status === COMPACTING) this.status = undefined
        this.reloadCurrent()
        break
      case 'retrying':
        this.status = L('再試行中: {0}', e.message)
        break
      case 'cancelled':
      case 'error':
        // The stopped answer ended after the next one began, which goes on.
        if (e.kind === 'cancelled' && this.stopping && this.busy) {
          this.stopping = false
          break
        }
        this.stopping = false
        guide.chatTurnEnded(this, this.lastReply)
        guide.chatWorkEnded()
        this.team.endAll(e.kind === 'cancelled' ? 'stopped' : 'failed', Date.now() / 1000)
        if (e.kind === 'cancelled') {
          this.canContinue = true
          this.notes = []
        }
        this.busy = false
        this.failed = e.kind === 'error'
        this.usageLimited = e.kind === 'error' && e.code === 'subscription_sharing_usage_limit_exceeded'
        this.contextFull = e.kind === 'error' && e.code === 'context_full'
        this.status = e.kind === 'cancelled' ? L('回答を停止しました（Workflow の実行は止めていません）。')
          : this.usageLimited ? L('Usage limit reached: ChatGPT の設定で、プランかこのアプリの利用上限を確かめてください。')
          : this.contextFull ? L('この会話はモデルの文脈に収まらなくなりました。新しい会話を始めてください（この会話は一覧に残ります）。')
          : L('エラー: {0}', modelErrorMessage(e.message, e.code))
        this.refreshAfterTurn()
        break
      case 'finished':
        if (this.failed || this.canContinue) {
          this.refreshAfterTurn()
          break
        }
        this.stopping = false
        guide.chatTurnEnded(this, this.lastReply)
        guide.chatWorkEnded()
        this.team.endAll('done', Date.now() / 1000)
        this.busy = false
        // A note left by a retry (e.g. a reply cut at the output limit) or the compaction is no longer current.
        if (this.status?.startsWith(L('再試行中')) || this.status === COMPACTING) this.status = undefined
        this.refreshAfterTurn()
        queueMicrotask(() => this.flushNotes())
        break

    }
    this.changed()
  }

  /** A delegate call: a bird and a row per helper. */
  private startHelpers(args: string) {
    let tasks: { name?: string; cards?: unknown }[] = []
    try {
      tasks = JSON.parse(args)?.tasks ?? []
    } catch {}
    const named = tasks.filter((t) => t?.name).map((t) => ({
      name: String(t.name),
      cards: Array.isArray(t.cards) ? t.cards.filter((c): c is string => typeof c === 'string') : [],
    }))
    this.team.start(named, Date.now() / 1000)
    guide.team = this.team
  }

  /** What a helper does: its bird and its row. */
  private handleHelper(name: string, e: any) {
    this.team.note(name, e, Date.now() / 1000)

  }

  private reloadCurrent() { void this.reread(this.currentId) }

  private refreshAfterTurn() {
    void this.reread(this.currentId).then(() => this.refreshSessions())
  }
}

function presentedItem(row: ChatRow): ChatItem {
  return { id: row.id, role: row.kind === 'text' ? row.role : row.kind, text: row.kind === 'tool' ? row.arguments + (row.result ? '\n→ ' + row.result : '') : row.text,
    archived: row.archived, helper: row.kind === 'helper' ? row.name : undefined,
    tool: row.kind === 'tool' ? { name: row.name, ok: row.state === 'running' ? undefined : row.state === 'completed' } : undefined,
    ended: row.kind === 'helper' && row.state !== 'running' ? row.state : undefined, presentation: row }
}
