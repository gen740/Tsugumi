// The chat of one project: several conversations (sessions), each an Agent
// run by the host server with its own persisted transcript.
import { api, onServerEvent } from '../api'
import { draftRequest } from '../shared/agentShared'
import { get, str } from '../shared/json'
import { uuid } from '../shared/rpc'
import type { AppModel } from './app'
import { guide, GuideAction, parseShow } from './guide'
import { Observable } from './observable'
import { CardKey, cardState, KIND_LABEL, path, ProjectSession } from './session'

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
export const refLabel = (r: Reference) => (r.kind === 'card' ? `Card ${r.card}` : `可視化 ${r.card}`)
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
  /** Display names of the models (the ChatGPT account's catalog). */
  modelNames: Record<string, string>
}

/** The Agent's error at the ChatGPT plan's usage limit (web/server/chatgpt.ts). */
export const USAGE_LIMIT_TEXT = "ChatGPT usage limit reached: review your plan or this app's limit in ChatGPT settings"
export const USAGE_URL = 'https://chatgpt.com/settings/usage'

interface ContentBlock {
  type: string
  text?: string
  name?: string
  input?: string
  content?: string
  is_error?: boolean
}

interface Message {
  role: string
  content: ContentBlock[]
  display?: string
}

interface Conversation {
  messages: Message[]
  archive: Message[]
  usage: { inputTokens: number; outputTokens: number; cacheReadTokens?: number; cacheWriteTokens?: number }
  summary?: string
}

export class ChatController extends Observable {
  items: ChatItem[] = []
  busy = false
  /** A save of the Agent waiting for the user: what it would give Cards. */
  /** A save waiting for the user's approval: of this chat's Agent, or of
   *  Claude Code (from) working on the project through the plugin. */
  permission?: { id: string; asked: string[]; from?: string }
  status?: string
  failed = false
  /** The last turn stopped at the ChatGPT plan's usage limit ("Manage usage"). */
  usageLimited = false
  /** The last turn stopped at the step limit and can go on. */
  stoppedAtLimit = false
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

  constructor(readonly session: ProjectSession, private readonly app: AppModel) {
    super()
    this.available = app.status?.chatAvailable ?? true
    if (!this.available) this.status = app.status?.chatError ?? 'RPC スキーマを読み込めないため、チャットは使えません。通常の操作はすべて UI から行えます。'
    onServerEvent((e) => {
      if (e.type === 'chat' && e.project === session.dir && e.session === this.currentId) this.handle(e.event)
      // Claude Code's saves (the plugin) are approved here, whichever conversation is open.
      else if (e.type === 'chat' && e.project === session.dir && e.session === 'external' && (e.event.kind === 'permission' || e.event.kind === 'permissionAnswered')) this.handle(e.event)
      // Progress sent while the stream was down: the conversation is read again.
      if (e.type === 'reconnected' && this.currentId) void this.reread(this.currentId)
    })
    void this.init()
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
      const r = await api<{ conversation: Conversation; busy: boolean; provider: ProviderInfo; permission: { id: string; asked: string[] } | null }>(
        '/api/chat/conversation', { ...this.base, folders: this.folders })
      if (this.currentId !== id) return
      this.busy = r.busy
      this.permission = r.permission ?? undefined
      this.usage = r.conversation.usage
      this.load(r.conversation)
      this.changed()
    } catch {}
  }

  select(id: string) {
    if (this.currentId && this.busy) void api('/api/chat/cancel', this.base).catch(() => {})
    this.busy = false
    this.failed = false
    this.usageLimited = false
    this.stoppedAtLimit = false
    if (this.available) this.status = undefined
    this.currentId = id
    this.items = []
    this.changed()
    if (!this.available) return
    // Resume work left unfinished by a disconnect or restart.
    void api<{ conversation: Conversation; busy: boolean; provider: ProviderInfo; permission: { id: string; asked: string[] } | null }>('/api/chat/conversation', { ...this.base, resume: true, folders: this.folders })
      .then((r) => {
        if (this.currentId !== id) return
        this.permission = r.permission ?? undefined
        this.usage = r.conversation.usage
        this.provider = r.provider
        this.busy = r.busy
        this.load(r.conversation)
        this.changed()
      })
      .catch((e) => {
        this.status = String(e.message ?? e)
        this.changed()
      })
  }

  private load(c: Conversation) {
    const out: ChatItem[] = []
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
        else if (b.type === 'tool_use') out.push({ id: uuid(), role: 'tool', text: b.input ?? '', tool: { name: b.name ?? '' }, archived })
        else if (b.type === 'tool_result') {
          const j = [...out].reverse().find((x) => x.role === 'tool' && x.tool?.ok === undefined)
          if (j) {
            j.tool!.ok = !b.is_error
            j.text += '\n→ ' + (b.content ?? '').slice(0, 400)
          }
        }
      }
    })
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

  /** Attaches a folder of this host (the server checks that it exists). */
  async attach(folder: string) {
    const r = await api<{ folder: string }>('/api/chat/folder', { folder })
    if (this.folders.includes(r.folder)) return
    this.folders = [...this.folders, r.folder]
    this.changed()
  }

  /** No longer offered to the Agent. */
  detach(folder: string) {
    this.folders = this.folders.filter((f) => f !== folder)
    this.changed()
  }

  private describe(r: Reference): string {
    const s = this.session, k = r.card
    if (r.kind === 'card') {
      let out = `Card ${k} (${KIND_LABEL[s.kind(k)]}, definition ${s.definitionDir(k) ?? '-'}, name "${s.displayName(k)}"`
      const st = s.cardStates.get(k)
      if (st) out += `, last state ${cardState(st.state).label}`
      const res = s.resultSummary(k)
      if (res) out += `, result: ${res}`
      return out + ')'
    }
    const v = s.displays.get(k)?.value
    if (!v) return `Display of Card ${k}: no result yet`
    switch (v.kind) {
      case 'text': return `Display of Card ${k} (text):\n${v.text.slice(0, 4000)}`
      case 'markdown': return `Display of Card ${k} (Markdown):\n${v.text.slice(0, 4000)}`
      case 'scalar': return `Display of Card ${k}: ${v.label} = ${v.value}`
      case 'lineChart':
        return `Display of Card ${k} (line chart):\n` + v.series.map((x) =>
          `series ${x.label}: ` + x.x.slice(0, 200).map((xv, i) => `(${xv}, ${x.y[i]})`).join(' ')).join('\n')
      case 'table':
        return `Display of Card ${k} (table):\n` + [v.columns, ...v.rows.slice(0, 200)].map((r) => r.join('\t')).join('\n')
      case 'image': return `Display of Card ${k}: ${v.mediaType} image, ${v.data.length} bytes, ${v.alt}`
      case 'points': return `Display of Card ${k}: 3D point cloud with ${v.x.length} points`
      case 'module': return `Display of Card ${k} (module ${v.name}):\n${v.json.slice(0, 4000)}`
    }
  }

  /** `$name` tokens that match a skill. */
  skillsIn(text: string): Skill[] {
    const names = new Set(text.split(/\s+/).filter((t) => t.startsWith('$')).map((t) => t.slice(1)))
    return this.skills.filter((x) => names.has(x.name))
  }

  // ---- Conversation --------------------------------------------------------------

  send(text: string, selection: CardKey[] = []) {
    if (!text.trim() || !this.available) return
    let prompt = text
    for (const sk of this.skillsIn(text)) prompt += `\n\n[Skill ${sk.name}]\n${sk.content}`
    if (this.skillsIn(text).some((s) => s.name === 'demo')) this.demoMode = true
    const refs = [...this.references, ...selection.filter((k) => !this.references.some((r) => r.kind === 'card' && r.card === k)).map((card) => ({ kind: 'card' as const, card }))]
    if (refs.length) prompt += '\n\n[References]\n' + refs.map((r) => this.describe(r)).join('\n')
    if (this.folders.length) {
      prompt += '\n\n[Attached folders]\n' + this.folders.map((f) => `${f} (read with attached_folder_read, folder: "${f}")`).join('\n')
    }
    const shown = text + (refs.length ? '\n📎 ' + refs.map(refLabel).join(', ') : '')
    this.items = [...this.items, { id: uuid(), role: 'user', text: shown }]
    this.references = []
    this.begin()
    // The chat shows what was typed, not the skills and references attached to it.
    void api('/api/chat/send', { ...this.base, prompt, title: text, display: shown, folders: this.folders }).catch((e) => this.fail(e))
  }

  /** Sends a request the app wrote for the model; the chat shows `display`. */
  private sendRequest(display: string, prompt: string) {
    if (!this.available) return
    this.items = [...this.items, { id: uuid(), role: 'user', text: display }]
    this.begin()
    void api('/api/chat/send', { ...this.base, prompt, title: display, display, folders: this.folders }).catch((e) => this.fail(e))
  }

  /** Asks the Agent to implement the Draft Cards of a scope (after they were wired and saved). */
  generateDrafts(scope: string[], cards: CardKey[]) {
    const s = this.session
    const items = cards.map((k) => {
      // A Card's definition folder is named by its id.
      const meta = s.cardMeta(k)
      return { key: k, definition: path(k).pop() ?? '-', description: str(get(meta, 'description')) ?? '', content: str(get(meta, 'content')) ?? '' }
    })
    const names = cards.map((k) => path(k).pop() ?? k).join('、')
    this.sendRequest(`Draft ${cards.length} 枚（${names}）から Card を生成します`, draftRequest(scope, items))
  }

  cancel() {
    void api('/api/chat/cancel', this.base).catch(() => {})
    this.busy = false
    this.status = '回答を停止しました。受理済みの実行は止まりません（実行の停止は Canvas の「停止」）。'
    this.changed()
  }

  /** Goes on after the step limit stopped the Agent. */
  continueWork() {
    if (!this.available) return
    this.items = [...this.items, { id: uuid(), role: 'user', text: '（止まったところから続けます）' }]
    this.begin()
    void api('/api/chat/continue', { ...this.base, folders: this.folders }).catch((e) => this.fail(e))
  }

  retry() {
    this.begin()
    void api('/api/chat/retry', { ...this.base, folders: this.folders }).catch((e) => this.fail(e))
  }

  compact() {
    this.begin()
    this.status = '会話を圧縮しています…'
    this.changed()
    void api('/api/chat/compact', { ...this.base, folders: this.folders }).catch((e) => this.fail(e))
  }

  /** Demo mode ($demo, or a demo tour): the bird flies about the screen for
   *  this chat; it ends with the tour's farewell. Otherwise the chat is a chat. */
  demoMode = false

  /** Replies to tap and buttons Tsugumi offered with its last answer (guide_show). */
  choices: string[] = []
  offered: GuideAction[] = []
  /** Notes of the app (a demo tour's runs) that came while it answered. */
  private notes: { prompt: string; display: string }[] = []

  /** Something the app tells Tsugumi (a tour step, a run that ended); one that
   *  comes while it answers waits for the answer to end. */
  tell(prompt: string, display: string) {
    // A tour is demo mode: the skill comes with its first note.
    if (!this.demoMode) {
      const demo = this.skills.find((s) => s.name === 'demo')
      if (demo) prompt += `\n\n[Skill demo]\n${demo.content}`
      this.demoMode = true
    }
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
    this.choices = []
    this.offered = []
    guide.chatTurnBegan(this)
    this.busy = true
    this.failed = false
    this.usageLimited = false
    this.stoppedAtLimit = false
    this.status = undefined
    this.changed()
  }

  private fail(e: unknown) {
    this.busy = false
    this.failed = true
    this.status = `エラー: ${(e as Error).message ?? e}`
    this.changed()
  }

  /** The user's answer; `always` are kinds allowed without asking from now on. */
  answerPermission(allow: boolean, always: string[]) {
    const p = this.permission
    if (!p) return
    this.permission = undefined
    this.changed()
    void api('/api/chat/permission', { id: p.id, allow, always }).catch((e) => this.fail(e))
  }

  private handle(e: any) {
    switch (e.kind) {
      case 'discardText': {
        // The reply is asked again: what it had streamed goes.
        const last = this.items[this.items.length - 1]
        if (last && last.role === 'assistant' && !last.tool) this.items = this.items.slice(0, -1)
        break
      }
      case 'permission':
        this.permission = { id: e.id, asked: e.asked, from: e.from }
        break
      case 'permissionAnswered':
        if (this.permission?.id === e.id) this.permission = undefined
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
          // The bird moves only in demo mode; after the farewell the chat is a chat again.
          if (this.demoMode) guide.chatShow(this, e.arguments)
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
      case 'compacted':
        // Earlier messages were summarized for the model; the whole text stays
        // in the conversation, so nothing is announced.
        this.reloadCurrent()
        break
      case 'retrying':
        this.status = `再試行中: ${e.message}`
        break
      case 'error':
        guide.chatTurnEnded(this, this.lastReply)
        guide.chatWorkEnded()
        if (e.message === 'cancelled') this.notes = []
        this.busy = false
        this.failed = e.message !== 'cancelled'
        this.usageLimited = e.message === USAGE_LIMIT_TEXT
        this.status = e.message === 'cancelled' ? '回答を停止しました（Workflow の実行は止めていません）。'
          : this.usageLimited ? 'Usage limit reached: ChatGPT の設定で、プランかこのアプリの利用上限を確かめてください。' : `エラー: ${e.message}`
        this.refreshAfterTurn()
        break
      case 'finished':
        guide.chatTurnEnded(this, this.lastReply)
        guide.chatWorkEnded()
        this.busy = false
        // A note left by a retry (e.g. a reply cut at the output limit) is no longer current.
        if (this.status?.startsWith('再試行中')) this.status = undefined
        this.refreshAfterTurn()
        queueMicrotask(() => this.flushNotes())
        break
      case 'stepLimit':
        guide.chatTurnEnded(this, this.lastReply)
        guide.chatWorkEnded()
        this.busy = false
        this.stoppedAtLimit = true
        this.status = `${e.steps} 手の上限で中断しました。「続ける」で続きから再開できます。`
          + (e.unconfirmedRun ? ` 実行 ${String(e.unconfirmedRun).slice(0, 8)} の結果はまだ確かめていません。` : '')
        this.refreshAfterTurn()
        break
    }
    this.changed()
  }

  /** A delegate call: a row per helper. */
  private startHelpers(args: string) {
    let tasks: { name?: string }[] = []
    try {
      tasks = JSON.parse(args)?.tasks ?? []
    } catch {}
    this.items = [...this.items, ...tasks.filter((t) => t?.name).map((t) => ({ id: uuid(), role: 'helper', text: '', helper: String(t.name) }))]
  }

  /** What a helper does, in its row. */
  private handleHelper(name: string, e: any) {
    const i = this.items.map((x) => x.role === 'helper' && x.helper === name).lastIndexOf(true)
    if (i < 0) return
    const it = this.items[i]
    let next: ChatItem | undefined
    if (e.kind === 'toolStarted') next = { ...it, tool: { name: e.name }, text: e.arguments }
    else if (e.kind === 'toolFinished' && it.tool?.name === e.name) next = { ...it, tool: { name: e.name, ok: e.ok } }
    else if (e.kind === 'finished') next = { ...it, ended: '終わりました' }
    else if (e.kind === 'stepLimit') next = { ...it, ended: '手数の上限で止まりました' }
    else if (e.kind === 'error') next = { ...it, ended: e.message === 'cancelled' ? '止めました' : `エラー: ${e.message}` }
    if (next) this.items = [...this.items.slice(0, i), next, ...this.items.slice(i + 1)]
  }

  private reloadCurrent() {
    const id = this.currentId
    void api<{ conversation: Conversation }>('/api/chat/conversation', { path: this.session.dir, session: id }).then((r) => {
      if (this.currentId !== id) return
      this.load(r.conversation)
      this.changed()
    }).catch(() => {})
  }

  private refreshAfterTurn() {
    const id = this.currentId
    void api<{ conversation: Conversation }>('/api/chat/conversation', { path: this.session.dir, session: id }).then((r) => {
      if (this.currentId === id) this.usage = r.conversation.usage
      void this.refreshSessions()
    }).catch(() => {})
  }
}
