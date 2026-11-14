// The chat of one project: several conversations (sessions), each an Agent
// run by the host server with its own persisted transcript.
import { api, onServerEvent } from '../api'
import { toolSummary } from '../shared/agentShared'
import { uuid } from '../shared/rpc'
import type { AppModel } from './app'
import { guide, GuideAction, type GuideShow, parseShow } from './guide'
import { agentAppearance } from './agentAppearance'
import { HelperTeam } from './helperBirds'
import { LiveCodePlayer, writesSources } from './liveCode'
import { Observable } from './observable'
import { Thoughts, type ThoughtReply } from './thoughts'
import { CardKey, ProjectSession, path } from './session'
import { L } from '../i18n'

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
}

export interface SessionInfo {
  id: string
  title: string
  updatedAt: number
  messages: number
}

export interface SearchHit {
  session: string
  title: string
  snippet: string
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

/** The Agent's error at the ChatGPT plan's usage limit (web/server/chatgpt.ts). */
export const USAGE_LIMIT_TEXT = "ChatGPT usage limit reached: review your plan or this app's limit in ChatGPT settings"
export const USAGE_URL = 'https://chatgpt.com/settings/usage'
/** The Agent's error when the conversation cannot be made to fit the model's context (web/server/agent.ts). */
export const CONTEXT_FULL_TEXT = "the conversation no longer fits the model's context"

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
  /** A save of the Agent waiting for the user: what it would give Cards. */
  /** A save waiting for the user's approval: of this chat's Agent, or of
   *  Claude Code (from) working on the project through the plugin. */
  private permissions: { id: string; asked: string[]; from?: string }[] = []
  get permission() { return this.permissions?.[0] }
  private enqueuePermission(p: { id: string; asked: string[]; from?: string }) {
    this.permissions ??= []
    if (!this.permissions.some(x => x.id === p.id)) this.permissions.push(p)
  }
  private removePermission(id: string) { this.permissions = (this.permissions ?? []).filter(p => p.id !== id) }
  status?: string
  failed = false
  /** The last turn stopped at the ChatGPT plan's usage limit ("Manage usage"). */
  usageLimited = false
  /** The last turn could not make the conversation fit the model's context. */
  contextFull = false
  /** The last turn stopped at the step limit and can go on. */
  stoppedAtLimit = false
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
      if (e.type === 'chat' && e.project === session.dir && e.session === this.currentId) this.handle(e.event)
      // Claude Code's saves (the plugin) are approved here, whichever conversation is open.
      else if (e.type === 'chat' && e.project === session.dir && e.session === 'external' && (e.event.kind === 'permission' || e.event.kind === 'permissionAnswered')) this.handle(e.event)
      // Progress sent while the stream was down: the conversation is read again.
      if (e.type === 'reconnected' && this.currentId) void this.reread(this.currentId)
    })
    void this.init()
  }

  /** The project was left: this chat hears nothing more. */
  close() {
    this.thoughts.close()
    this.offEvents()
    this.permissions = []
    this.team.endAll('stopped', Date.now() / 1000)
    guide.chatClosed(this)
  }

  private get base() {
    return { path: this.session.dir, session: this.currentId }
  }

  private async init() {
    await this.refreshSessions()
    this.select(this.sessions[0]?.id ?? uuid())
    void this.reloadSkills()
  }

  // ---- Sessions ------------------------------------------------------------------

  async refreshSessions() {
    try {
      this.sessions = await api<SessionInfo[]>('/api/chat/sessions', { path: this.session.dir })
    } catch {}
    this.changed()
  }

  /** Reads the open conversation again (its turn keeps running on the server). */
  private async reread(id: string) {
    try {
      const r = await api<{ conversation: Conversation; busy: boolean; resumable?: boolean; interruptedOperation?: InterruptedOperation; continuable?: boolean; provider: ProviderInfo; permission: { id: string; asked: string[] } | null }>(
        '/api/chat/conversation', { ...this.base, folders: this.folders })
      if (this.currentId !== id) return
      this.busy = r.busy
      this.adoptInterruption(r)
      if (r.permission) this.enqueuePermission(r.permission)
      this.usage = r.conversation.usage
      this.provider = r.provider
      this.load(r.conversation)
      this.changed()
    } catch {}
  }

  select(id: string) {
    this.turn++
    guide.chatThought(this)
    this.busy = false
    this.failed = false
    this.usageLimited = false
    this.contextFull = false
    this.stoppedAtLimit = false
    this.interrupted = false
    this.interruptedOperation = undefined
    if (this.available) this.status = undefined
    this.permissions = []
    this.currentId = id
    this.items = []
    this.changed()
    if (!this.available) return
    // Opening only reads the core snapshot.
    void api<{ conversation: Conversation; busy: boolean; resumable?: boolean; interruptedOperation?: InterruptedOperation; continuable?: boolean; provider: ProviderInfo; permission: { id: string; asked: string[] } | null }>('/api/chat/conversation', { ...this.base, folders: this.folders })
      .then((r) => {
        if (this.currentId !== id) return
        if (r.permission) this.enqueuePermission(r.permission)
        this.usage = r.conversation.usage
        this.provider = r.provider
        this.busy = r.busy
        this.adoptInterruption(r)
        this.load(r.conversation)
        this.changed()
      })
      .catch((e) => {
        this.status = String(e.message ?? e)
        this.changed()
      })
  }

  private adoptInterruption(r: { resumable?: boolean; interruptedOperation?: InterruptedOperation; continuable?: boolean }) {
    const wasInterrupted = this.interrupted
    this.interrupted = r.resumable === true
    this.interruptedOperation = r.interruptedOperation
    this.stoppedAtLimit = !this.interrupted && r.continuable === true
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
    if (!this.available || !this.interrupted || this.busy) return
    this.begin()
    void this.request('/api/chat/resume', { ...this.base, folders: this.folders }).catch((e) => this.fail(e))
  }

  private load(c: Conversation) {
    const out: ChatItem[] = []
    const tools = new Map<string, ChatItem>()
    const all = [...c.archive, ...c.messages]
    all.forEach((m, i) => {
      const archived = i < c.archive.length
      // A request the app wrote for the model shows as its short form.
      if (m.display) {
        out.push({ id: uuid(), role: m.role, text: m.display, archived })
        return
      }
      for (const b of m.content) {
        if (b.type === 'text') out.push({ id: uuid(), role: m.role, text: b.text ?? '', archived })
        else if (b.type === 'tool_use') {
          const item: ChatItem = { id: uuid(), role: 'tool', text: b.input ?? '', tool: { name: b.name ?? '' }, archived }
          out.push(item)
          if (b.id) tools.set(b.id, item)
        }
        else if (b.type === 'web_search') {
          const sources = (b.sources ?? []).map((s) => (s.title ? `${s.title} — ${s.url}` : s.url)).join('\n')
          out.push({ id: uuid(), role: 'tool', text: `${JSON.stringify({ query: b.query ?? '' })}\n→ ${b.error ? `failed: ${b.error}` : sources || 'no results'}`, tool: { name: 'web_search', ok: !b.error }, archived })
        }
        else if (b.type === 'tool_result') {
          const j = b.tool_use_id ? tools.get(b.tool_use_id) : undefined
          if (j) {
            j.tool!.ok = !b.is_error
            j.text += '\n→ ' + toolSummary(b.content ?? '')
          }
        }
      }
    })
    for (const record of c.tools ?? []) if (record.name === 'operation_approval') {
      out.push({ id: record.id, role: 'tool', text: record.result ?? '', tool: { name: record.name, ok: record.state === 'pending' ? undefined : !record.isError } })
    }
    this.items = out
  }

  newSession() {
    this.select(uuid())
    void this.refreshSessions()
  }

  async delete(id: string) {
    await api('/api/chat/delete', { path: this.session.dir, session: id }).catch(() => {})
    await this.refreshSessions()
    if (id === this.currentId) this.select(this.sessions[0]?.id ?? uuid())
  }

  /** Full-text search over every session, compacted parts included. */
  search(text: string) {
    return api<SearchHit[]>('/api/chat/search', { path: this.session.dir, text })
  }

  /** Re-reads the connection (after Settings > API changed it). */
  async refreshProvider() {
    try {
      const r = await api<{ provider: ProviderInfo }>('/api/chat/conversation', { ...this.base })
      this.provider = r.provider
      this.changed()
    } catch {}
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
    const r = await api<{ folders: { folder: string; resource: string }[] }>('/api/chat/folders', { ...this.base, selected: [...new Set(folders)], folders: this.folders })
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

  send(text: string, selection: CardKey[] = []) {
    if (!text.trim() || !this.available) return
    // No model connection: Tsugumi answers from its script.
    if (guide.scripted && !this.busy) return guide.answerScripted(text.trim(), this)
    if (this.skillsIn(text).some((s) => s.name === 'demo')) this.demoMode = true
    const refs = [...this.references, ...selection.filter((k) => !this.references.some((r) => r.kind === 'card' && r.card === k)).map((card) => ({ kind: 'card' as const, card }))]
    const shown = text + (refs.length ? '\n📎 ' + refs.map(r => refLabel(r, k => this.session.displayPath(path(k)))).join(', ') : '')
    this.items = [...this.items, { id: uuid(), role: 'user', text: shown }]
    this.references = []
    this.begin()
    // The chat shows what was typed, not the skills and references attached to it.
    void this.request('/api/chat/send', { ...this.base, text, display: shown, selectedCards: refs.filter((r) => r.kind === 'card').map((r) => r.card), selectedDisplays: refs.filter((r) => r.kind === 'display').map((r) => r.card), skillNames: this.skillsIn(text).map((s) => s.name), attachedResources: [...this.folderResources.values()] }).catch((e) => this.fail(e))
  }

  /** Sends a request the app wrote for the model; the chat shows `display`. */
  private sendRequest(display: string, prompt: string) {
    if (!this.available) return
    this.items = [...this.items, { id: uuid(), role: 'user', text: display }]
    this.begin()
    void this.request('/api/chat/send', { ...this.base, text: prompt, display, skillNames: this.demoMode ? ['demo'] : [] }).catch((e) => this.fail(e))
  }

  /** Asks the Agent to implement the Draft Cards of a scope (after they were wired and saved). */
  generateDrafts(scope: string[], cards: CardKey[]) {
    this.sendRequest(L('Draft {0} 枚から Card を生成します', cards.length),
      `Implement the selected Draft Cards in scope ${JSON.stringify(scope)}: ${JSON.stringify(cards)}. Read their definitions after access is granted.`)
  }

  /** The identifiers are explicit user selections; core reads their comments after authorization. */
  generateFromComments(scope: string[], cards: CardKey[]) {
    this.sendRequest(L('選択した Card のコメントに従って生成します'),
      `Resolve the open comments on these Cards in scope ${JSON.stringify(scope)}: ${JSON.stringify(cards)}. Read the comments after access is granted.`)
  }

  cancel() {
    this.turn++
    guide.chatThought(this)
    void this.request('/api/chat/cancel', this.base).catch(() => {})
    this.stopping = true
    this.stoppedAtLimit = true
    this.busy = false
    this.status = L('回答を停止しました。受理済みの実行は止まりません（実行の停止は Canvas の「停止」）。')
    this.changed()
  }

  /** Goes on after the step limit stopped the Agent. */
  continueWork() {
    if (!this.available) return
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
  /** Without a model connection: Tsugumi's fixed reply from agentd to what
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
    this.stoppedAtLimit = false
    this.status = undefined
    this.changed()
  }

  private request(endpoint: string, body: Record<string, unknown>) {
    const pending = this.transition.catch(() => {}).then(() => api(endpoint, body))
    this.transition = pending
    return pending
  }

  private fail(e: unknown) {
    this.busy = false
    this.failed = true
    this.status = L('エラー: {0}', (e as Error).message ?? e)
    this.changed()
  }

  /** The user's answer; `always` are kinds allowed without asking from now on. */
  answerPermission(allow: boolean, always: string[], id = this.permission?.id) {
    const p = this.permission
    if (!p || p.id !== id) return
    this.removePermission(p.id)
    this.changed()
    void api('/api/chat/permission', { id: p.id, allow, always }).catch((e) => this.fail(e))
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

  /** What an event means for the file being written; true: nothing else to do. */
  private noteWriting(who: string, e: any) {
    const now = Date.now() / 1000
    switch (e.kind) {
      case 'toolInput':
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
        // What was written is not on the screen once it runs.
        if (e.name === 'run') this.writer(who).stop()
        return false
      case 'finished':
        this.writer(who).ended(true, now)
        return false
      case 'discardText': case 'retrying': case 'error': case 'stepLimit':
        this.writer(who).ended(false, now)
        return false
    }
    return false
  }

  private handle(e: any) {
    if (this.noteWriting('', e)) return
    if (e.kind === 'helper' && this.noteWriting(e.name, e.event)) return
    switch (e.kind) {
      case 'discardText': {
        // The reply is asked again: what it had streamed goes.
        const last = this.items[this.items.length - 1]
        if (last && last.role === 'assistant' && !last.tool) this.items = this.items.slice(0, -1)
        break
      }
      case 'permission':
        this.enqueuePermission({ id: e.id, asked: e.asked, from: e.from })
        break
      case 'permissionAnswered':
        this.removePermission(e.id)
        break
      case 'text': {
        const last = this.items[this.items.length - 1]
        if (last && last.role === 'assistant' && !last.tool) {
          this.items = [...this.items.slice(0, -1), { ...last, text: last.text + e.text }]
        } else {
          this.items = [...this.items, { id: uuid(), role: 'assistant', text: e.text }]
        }
        break
      }
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
        this.items = [...this.items, { id: uuid(), role: 'tool', text: e.arguments, tool: { name: e.name } }]
        // The bird flies to the Card the Agent works on (in demo mode guide_show moves it instead).
        if (!this.demoMode) guide.chatWork(e.name, e.arguments)
        if (e.name === 'delegate') this.startHelpers(e.arguments)
        break
      case 'helper':
        this.handleHelper(e.name, e.event)
        break
      case 'toolFinished': {
        const i = this.items.map((x) => x.role === 'tool' && x.tool?.name === e.name && x.tool?.ok === undefined).lastIndexOf(true)
        if (i >= 0) {
          const it = this.items[i]
          this.items = [...this.items.slice(0, i), { ...it, tool: { name: e.name, ok: e.ok }, text: it.text + '\n→ ' + e.summary }, ...this.items.slice(i + 1)]
        }
        break
      }
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
      case 'error':
        // The stopped answer ended after the next one began, which goes on.
        if (e.message === 'cancelled' && this.stopping && this.busy) {
          this.stopping = false
          break
        }
        this.stopping = false
        guide.chatTurnEnded(this, this.lastReply)
        guide.chatWorkEnded()
        this.team.endAll(e.message === 'cancelled' ? 'stopped' : 'failed', Date.now() / 1000)
        if (e.message === 'cancelled') this.notes = []
        this.busy = false
        this.failed = e.message !== 'cancelled'
        this.usageLimited = e.message === USAGE_LIMIT_TEXT
        this.contextFull = e.message === CONTEXT_FULL_TEXT
        this.status = e.message === 'cancelled' ? L('回答を停止しました（Workflow の実行は止めていません）。')
          : this.usageLimited ? L('Usage limit reached: ChatGPT の設定で、プランかこのアプリの利用上限を確かめてください。')
          : this.contextFull ? L('この会話はモデルの文脈に収まらなくなりました。新しい会話を始めてください（この会話は一覧に残ります）。')
          : L('エラー: {0}', e.message)
        this.refreshAfterTurn()
        break
      case 'finished':
        if (this.failed || this.stoppedAtLimit) {
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
      case 'stepLimit':
        this.stopping = false
        guide.chatTurnEnded(this, this.lastReply)
        guide.chatWorkEnded()
        this.team.endAll('limit', Date.now() / 1000)
        this.busy = false
        this.stoppedAtLimit = true
        this.status = L('{0} 手の上限で中断しました。「続ける」で続きから再開できます。', e.steps)
          + (e.unconfirmedRun ? L(' 実行 {0} の結果はまだ確かめていません。', String(e.unconfirmedRun).slice(0, 8)) : '')
        this.refreshAfterTurn()
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
    this.items = [...this.items, ...named.map((t) => ({ id: uuid(), role: 'helper', text: '', helper: t.name }))]
  }

  /** What a helper does: its bird and its row. */
  private handleHelper(name: string, e: any) {
    this.team.note(name, e, Date.now() / 1000)
    const i = this.items.map((x) => x.role === 'helper' && x.helper === name).lastIndexOf(true)
    if (i < 0) return
    const it = this.items[i]
    let next: ChatItem | undefined
    if (e.kind === 'toolStarted') next = { ...it, tool: { name: e.name }, text: e.arguments }
    else if (e.kind === 'toolFinished' && it.tool?.name === e.name) next = { ...it, tool: { name: e.name, ok: e.ok } }
    else if (e.kind === 'finished') next = { ...it, ended: L('終わりました') }
    else if (e.kind === 'stepLimit') next = { ...it, ended: L('手数の上限で止まりました') }
    else if (e.kind === 'error') next = { ...it, ended: e.message === 'cancelled' ? L('止めました') : L('エラー: {0}', e.message) }
    if (next) this.items = [...this.items.slice(0, i), next, ...this.items.slice(i + 1)]
  }

  private reloadCurrent() {
    const id = this.currentId
    void api<{ conversation: Conversation; resumable?: boolean; interruptedOperation?: InterruptedOperation; continuable?: boolean }>('/api/chat/conversation', { path: this.session.dir, session: id }).then((r) => {
      if (this.currentId !== id) return
      this.load(r.conversation)
      this.changed()
    }).catch(() => {})
  }

  private refreshAfterTurn() {
    const id = this.currentId, turn = this.turn
    void api<{ conversation: Conversation; resumable?: boolean; interruptedOperation?: InterruptedOperation; continuable?: boolean }>('/api/chat/conversation', { path: this.session.dir, session: id }).then((r) => {
      if (this.currentId !== id || this.turn !== turn) return
      this.load(r.conversation)
      this.adoptInterruption(r)
      this.usage = r.conversation.usage
      this.changed()
      void this.refreshSessions()
    }).catch(() => {})
  }
}
