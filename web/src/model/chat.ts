// The chat of one project: several conversations (sessions), each an Agent
// run by the host server with its own persisted transcript.
import { api, onServerEvent } from '../api'
import { draftRequest } from '../shared/agentShared'
import { get, str } from '../shared/json'
import { uuid } from '../shared/rpc'
import type { AppModel } from './app'
import { Observable } from './observable'
import { CardKey, cardState, KIND_LABEL, path, ProjectSession } from './session'

export interface ChatItem {
  id: string
  role: string
  text: string
  tool?: { name: string; ok?: boolean }
  archived?: boolean
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
  /** A real provider (with a key and a model choice), not the local fixture. */
  usesKey: boolean
}

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
  permission?: { id: string; asked: string[] }
  status?: string
  failed = false
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
  provider: ProviderInfo = { name: 'Anthropic', model: 'claude-sonnet-5-5', api: 'anthropicMessages', usesKey: true }

  constructor(readonly session: ProjectSession, private readonly app: AppModel) {
    super()
    this.available = app.status?.chatAvailable ?? true
    if (!this.available) this.status = app.status?.chatError ?? 'RPC スキーマを読み込めないため、チャットは使えません。通常の操作はすべて UI から行えます。'
    onServerEvent((e) => {
      if (e.type === 'chat' && e.project === session.dir && e.session === this.currentId) this.handle(e.event)
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

  select(id: string) {
    if (this.currentId && this.busy) void api('/api/chat/cancel', this.base).catch(() => {})
    this.busy = false
    this.failed = false
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
    const refs = [...this.references, ...selection.filter((k) => !this.references.some((r) => r.kind === 'card' && r.card === k)).map((card) => ({ kind: 'card' as const, card }))]
    if (refs.length) prompt += '\n\n[References]\n' + refs.map((r) => this.describe(r)).join('\n')
    if (this.folders.length) {
      prompt += '\n\n[Attached folders]\n' + this.folders.map((f) => `${f} (read with attached_folder_read, folder: "${f}")`).join('\n')
    }
    this.items = [...this.items, { id: uuid(), role: 'user', text: text + (refs.length ? '\n📎 ' + refs.map(refLabel).join(', ') : '') }]
    this.references = []
    this.begin()
    void api('/api/chat/send', { ...this.base, prompt, title: text, folders: this.folders }).catch((e) => this.fail(e))
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

  private begin() {
    this.busy = true
    this.failed = false
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
      case 'permission':
        this.permission = { id: e.id, asked: e.asked }
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
        this.items = [...this.items, { id: uuid(), role: 'tool', text: e.arguments, tool: { name: e.name } }]
        break
      case 'toolFinished': {
        const i = this.items.map((x) => x.tool?.name === e.name && x.tool?.ok === undefined).lastIndexOf(true)
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
        this.busy = false
        this.failed = e.message !== 'cancelled'
        this.status = e.message === 'cancelled' ? '回答を停止しました（Workflow の実行は止めていません）。' : `エラー: ${e.message}`
        this.refreshAfterTurn()
        break
      case 'finished':
        this.busy = false
        // A note left by a retry (e.g. a reply cut at the output limit) is no longer current.
        if (this.status?.startsWith('再試行中')) this.status = undefined
        this.refreshAfterTurn()
        break
      case 'stepLimit':
        this.busy = false
        this.stoppedAtLimit = true
        this.status = `${e.steps} 手の上限で中断しました。「続ける」で続きから再開できます。`
          + (e.unconfirmedRun ? ` 実行 ${String(e.unconfirmedRun).slice(0, 8)} の結果はまだ確かめていません。` : '')
        this.refreshAfterTurn()
        break
    }
    this.changed()
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
