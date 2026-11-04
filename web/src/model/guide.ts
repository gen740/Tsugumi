// The guide bird in the bottom-right corner: one conversation for the whole
// app, run by the host server with the same model connection as the Agent
// (no key, no guide). It explains what is on the screen and leads demo
// tours; it only reads, and what it offers to do (guide_show) is done by the
// user's own press of a button. The macOS app's GuideController does the same.
import { api, onServerEvent } from '../api'
import { AGENT, fill } from '../shared/agentShared'
import { D } from '../shared/rpc'
import { app } from './app'
import { Observable } from './observable'
import { get, str } from '../shared/json'
import { CardKey, cardState, KIND_LABEL, key, ProjectSession, runState } from './session'
import type { WorkflowTab } from './workspace'

export type GuideActionKind = 'open_demo' | 'show_demos' | 'run' | 'stop' | 'open_card' | 'open_settings' | 'new_project' | 'home'
const KINDS: GuideActionKind[] = ['open_demo', 'show_demos', 'run', 'stop', 'open_card', 'open_settings', 'new_project', 'home']

/** A button the guide offers under its reply. */
export interface GuideAction { kind: GuideActionKind; label: string; demo?: string; card?: string }

export interface GuideItem {
  id: number
  /** user, assistant or note (what the app told the guide: a tour step, a run that ended). */
  role: 'user' | 'assistant' | 'note'
  text: string
  actions?: GuideAction[]
}

/** The demo being shown: its id and title, the copy's folder, and whether the guide began it. */
export interface GuideTour { demo: string; title: string; path: string; started: boolean }

/** What the screen the user asks from shows. */
export type GuideScreen = 'home' | 'demos' | 'project'

/** What a guide_show call points at and offers. */
export function parseShow(args: string): { highlight?: string; actions: GuideAction[] } {
  let v: any
  try {
    v = JSON.parse(args)
  } catch {
    return { actions: [] }
  }
  const actions = (Array.isArray(v?.actions) ? v.actions : []).flatMap((a: any): GuideAction[] => {
    if (!KINDS.includes(a?.kind)) return []
    const label = typeof a.label === 'string' && a.label.trim() ? a.label.trim() : a.kind
    return [{ kind: a.kind, label, demo: typeof a.demo === 'string' ? a.demo : undefined, card: typeof a.card === 'string' ? a.card : undefined }]
  }).slice(0, 3) // unknown kinds are dropped first; at most three buttons
  const h = typeof v?.highlight === 'string' ? v.highlight.trim() : ''
  return { highlight: h || undefined, actions }
}

const text = (k: Exclude<keyof typeof AGENT.guide, 'tools' | 'showTool'>, values: Record<string, string> = {}) => fill(AGENT.guide[k], values)
const stateName = (st: D.RunState | undefined) => (st === undefined ? 'none' : D.RunState[st] ?? String(st))
const real = (p: string) => p.replace(/\/+$/, '')

interface Message { role: string; content: { type: string; text?: string; name?: string; input?: string }[]; display?: string }

export class GuideController extends Observable {
  /** A model connection the guide can use (a key for the chosen provider). */
  available = false
  /** The first start's question (provider and key, or skip) was answered. */
  setupDone = true
  /** "デモを見ますか？" was answered (見る or あとで). */
  demoAnswered = true
  tour: GuideTour | null = null
  loaded = false
  items: GuideItem[] = []
  busy = false
  failed = false
  status?: string
  /** What the guide is doing now, in a few words. */
  activity?: string
  open = false
  /** The part of the screen the guide points at (guide_show), for a while. */
  highlight?: string
  /** Home's demo list asked for by the guide (show_demos). */
  demosRequested = false
  private n = 0
  private pending: GuideAction[] = []
  private queued: { prompt: string; display: string }[] = []
  private highlightTimer?: ReturnType<typeof setTimeout>
  private readSession?: ProjectSession
  private tourTab?: WorkflowTab
  private seen?: string
  private seenActive = false
  private resumeAsked = false

  constructor() {
    super()
    onServerEvent((e) => {
      if (e.type === 'guide') this.handle(e.event)
      if (e.type === 'reconnected') void this.refresh()
    })
  }

  set<K extends 'open' | 'demosRequested'>(k: K, v: GuideController[K]) {
    ;(this as GuideController)[k] = v
    this.changed()
  }

  /** The first start asks for a model key (or a skip): nothing usable and never answered. */
  get needsSetup() {
    return this.loaded && !this.setupDone && !this.available
  }

  /** The guide greets and offers the demo once, when it first has a connection. */
  get offersDemo() {
    return this.available && !this.demoAnswered
  }

  /** Reads the guide's state (after Settings or the first start changed the key). */
  async refresh() {
    try {
      const r = await api<{ available: boolean; setupDone: boolean; demoAnswered: boolean; tour: GuideTour | null; busy: boolean; conversation: { archive: Message[]; messages: Message[] } | null }>('/api/guide', {})
      const wasOffered = this.offersDemo
      this.available = r.available
      this.setupDone = r.setupDone
      this.demoAnswered = r.demoAnswered
      this.tour = r.tour
      this.busy = r.busy
      this.loaded = true
      if (r.conversation && !this.busy) this.load([...r.conversation.archive, ...r.conversation.messages])
      if (!this.available) {
        this.open = false
        this.highlight = undefined
      } else if (this.offersDemo && !wasOffered) this.open = true
    } catch {}
    this.changed()
  }

  private async saveState(v: { setupDone?: boolean; demoAnswered?: boolean; tour?: GuideTour | null }) {
    try {
      await api('/api/guide/state', v)
    } catch {}
  }

  /** The first start was answered: with a key that connected, or skipped. */
  async finishSetup() {
    this.setupDone = true
    this.changed()
    await this.saveState({ setupDone: true })
    await this.refresh()
  }

  /** Asks the first start's questions again (Settings). */
  async resetIntroduction() {
    await this.saveState({ setupDone: false, demoAnswered: false })
    this.demoAnswered = false
    await this.refresh()
    if (this.offersDemo) this.set('open', true)
  }

  answerDemoOffer() {
    this.demoAnswered = true
    this.changed()
    void this.saveState({ demoAnswered: true })
  }

  // ---- Conversation ----------------------------------------------------------------

  /** A question of the user, with what their screen shows. */
  ask(q: string, screen: GuideScreen, session?: ProjectSession, tab?: WorkflowTab) {
    const t = q.trim()
    if (!this.available || this.busy || !t) return
    this.readSession = session
    this.clearHighlight()
    this.items.push({ id: ++this.n, role: 'user', text: t })
    void this.send(t + '\n\n' + this.screenNote(screen, session, tab), t, session)
  }

  /** Something the app tells the guide (a tour step, a run that ended); one
   *  that comes while it answers waits for the answer to end. */
  private tell(prompt: string, display: string, session?: ProjectSession) {
    if (!this.available) return
    if (this.busy) {
      this.queued.push({ prompt, display })
      return
    }
    if (session) this.readSession = session
    this.items.push({ id: ++this.n, role: 'note', text: display })
    void this.send(prompt + '\n\n' + this.screenNote('project', this.readSession, this.tourTab), display, this.readSession)
  }

  private async send(prompt: string, display: string, session?: ProjectSession) {
    this.begin()
    try {
      await api('/api/guide/send', { prompt, display, project: session?.dir })
    } catch (e) {
      this.busy = false
      this.activity = undefined
      this.failed = true
      this.status = `うまく答えられませんでした: ${(e as Error).message}`
      this.changed()
    }
  }

  cancel() {
    this.queued = []
    void api('/api/guide/cancel', {})
    this.busy = false
    this.activity = undefined
    this.status = '回答を止めました。'
    this.changed()
  }

  retry() {
    if (!this.available || this.busy) return
    this.begin()
    void api('/api/guide/retry', {})
  }

  /** A new conversation (the old one is gone; the tour stays). */
  async newConversation() {
    if (this.busy) return
    await api('/api/guide/reset', {})
    this.items = []
    this.status = undefined
    this.failed = false
    this.clearHighlight()
    this.changed()
  }

  private begin() {
    this.busy = true
    this.failed = false
    this.status = undefined
    this.pending = []
    this.activity = '考えています'
    this.changed()
  }

  private handle(e: any) {
    switch (e.kind) {
      case 'text': {
        this.activity = '考えています'
        const last = this.items[this.items.length - 1]
        if (last?.role === 'assistant') last.text += e.text
        else this.items.push({ id: ++this.n, role: 'assistant', text: e.text })
        break
      }
      case 'toolStarted':
        if (e.name === 'guide_show') {
          const shown = parseShow(e.arguments)
          if (shown.highlight) this.point(shown.highlight)
          this.pending.push(...shown.actions)
        } else {
          this.activity = e.name.startsWith('run') || e.name === 'log_range' ? '実行の様子を見ています' : 'プロジェクトを読んでいます'
        }
        break
      case 'discardText':
        if (this.items[this.items.length - 1]?.role === 'assistant') this.items.pop()
        break
      case 'retrying':
        this.status = `再試行中: ${e.message}`
        break
      case 'finished':
        this.ended()
        this.flush()
        break
      case 'stepLimit':
        this.ended()
        this.status = '答えをまとめきれませんでした。聞き方を変えてもう一度どうぞ。'
        break
      case 'error':
        this.ended()
        this.queued = []
        this.failed = e.message !== 'cancelled'
        this.status = e.message === 'cancelled' ? '回答を止めました。' : `うまく答えられませんでした: ${e.message}`
        break
      default:
        return
    }
    this.changed()
  }

  private ended() {
    this.busy = false
    this.activity = undefined
    if (!this.pending.length) return
    // Only the newest buttons are offered.
    for (const i of this.items) i.actions = undefined
    const last = this.items[this.items.length - 1]
    if (last?.role === 'assistant') last.actions = this.pending
    else this.items.push({ id: ++this.n, role: 'assistant', text: '', actions: this.pending })
    this.pending = []
  }

  private flush() {
    if (!this.queued.length) return
    const notes = this.queued
    this.queued = []
    this.tell(notes.map((x) => x.prompt).join('\n'), notes[notes.length - 1].display, this.readSession)
  }

  private load(messages: Message[]) {
    const out: GuideItem[] = []
    // The buttons of a guide_show go under the reply that follows it; only the newest stay.
    let shown: GuideAction[] = []
    const flush = () => {
      if (shown.length) out.push({ id: ++this.n, role: 'assistant', text: '', actions: shown })
      shown = []
    }
    for (const m of messages) {
      if (m.role === 'user') {
        const note = m.content.some((b) => b.type === 'text' && (b.text?.startsWith('[Tour]') || b.text?.startsWith('[Event]')))
        if (m.display) {
          flush()
          out.push({ id: ++this.n, role: note ? 'note' : 'user', text: m.display })
        }
        continue
      }
      for (const b of m.content) {
        if (b.type === 'text' && b.text) {
          out.push({ id: ++this.n, role: 'assistant', text: b.text, actions: shown.length ? shown : undefined })
          shown = []
        }
        if (b.type === 'tool_use' && b.name === 'guide_show') shown = parseShow(b.input ?? '{}').actions
      }
    }
    flush()
    const newest = out.map((x) => !!x.actions?.length).lastIndexOf(true)
    out.forEach((x, i) => { if (i !== newest) x.actions = undefined })
    this.items = out
  }

  // ---- Pointing --------------------------------------------------------------------

  private point(target: string) {
    this.highlight = target
    clearTimeout(this.highlightTimer)
    this.highlightTimer = setTimeout(() => this.clearHighlight(), 12_000)
  }

  clearHighlight() {
    clearTimeout(this.highlightTimer)
    if (this.highlight === undefined) return
    this.highlight = undefined
    this.changed()
  }

  // ---- What the guide sees ---------------------------------------------------------

  /** [Screen]: what the user's screen shows, for the guide to answer about. */
  screenNote(screen: GuideScreen, s?: ProjectSession, tab?: WorkflowTab): string {
    const lines = ['[Screen]']
    if (screen === 'project' && s) {
      lines.push(`screen: the project "${s.scopeName([])}" (folder ${s.dir})`)
      if (tab) {
        lines.push('workflow shown: ' + (tab.scope.length ? `the Card in Card ${tab.scope.join('/')}` : 'the root workflow'))
        const ids = s.cards(tab.scope).map((c) => str(get(c, 'id'))).filter((x): x is string => x !== undefined)
        lines.push('Cards in it: ' + (ids.length ? ids.map((id) => `${id} (${s.kind(key([...tab.scope, id]))})`).join(', ') : 'none'))
        const sel = [...tab.selection].sort()
        lines.push('selected Cards: ' + (sel.length ? sel.map((k) => this.describe(k, s)).join('; ') : 'none'))
      }
      if (s.connected) lines.push(`run: ${s.runStatus ? stateName(s.runStatus.state) : 'none yet'}` + (s.currentRunId ? ` (run ${s.currentRunId})` : ''))
      else lines.push(`connection: not connected to the project's environment (${s.environment.state})`)
      if (this.tour && this.same(this.tour.path, s)) lines.push(`tour: this project is the copy of the demo "${this.tour.demo}" being shown`)
    } else if (screen === 'demos') {
      lines.push('screen: Home, the demo list')
    } else {
      lines.push('screen: Home (recent projects, new project, open, デモを見る)')
    }
    if (this.tour && !(s && this.same(this.tour.path, s))) lines.push(`tour: the demo "${this.tour.demo}" is paused (the user is not in its copy)`)
    lines.push('demos: ' + (app.status?.demos ?? []).map((d) => `${d.id} "${d.title}"`).join(', '))
    return lines.join('\n')
  }

  private describe(k: CardKey, s: ProjectSession): string {
    let d = `${k} (${KIND_LABEL[s.kind(k)]}, "${s.displayName(k)}"`
    const st = s.cardStates.get(k)
    if (st) d += `, last state ${cardState(st.state).label}`
    const r = s.resultSummary(k)
    if (r) d += `, result: ${r.slice(0, 300)}`
    return d + ')'
  }

  same(p: string, s: ProjectSession) {
    return real(p) === real(s.dir)
  }

  // ---- Demo tours -----------------------------------------------------------------

  /** The user chose a demo: its copy is about to open; the tour starts there. */
  startTour(demo: { id: string; title: string }, copy: string) {
    this.answerDemoOffer()
    this.tour = { demo: demo.id, title: demo.title, path: copy, started: false }
    this.seen = undefined
    this.open = true
    this.changed()
    void this.saveState({ tour: this.tour })
  }

  endTour() {
    this.tour = null
    this.seen = undefined
    this.changed()
    void this.saveState({ tour: null })
  }

  /** The user asks to go on with a paused tour: its copy opens again. */
  resumeTour(): string | undefined {
    if (!this.tour) return undefined
    this.resumeAsked = true
    return this.tour.path
  }

  /** A project on the screen: in the tour's copy the guide begins the tour
   *  once it is connected, and follows its runs from their real states. */
  observe(s: ProjectSession, tab?: WorkflowTab) {
    const t = this.tour
    if (!this.available || !t || !this.same(t.path, s)) return
    this.tourTab = tab
    const now = this.runKey(s)
    if (!t.started) {
      if (!s.connected) return
      t.started = true
      void this.saveState({ tour: t })
      this.seen = now
      this.seenActive = s.runActive
      this.tell(text('tourStart', { demo: t.demo, title: t.title }), `デモ「${t.title}」の案内を始めます`, s)
      return
    }
    if (this.resumeAsked && s.connected) {
      this.resumeAsked = false
      this.seen = now
      this.seenActive = s.runActive
      this.tell(text('tourResume', { demo: t.demo }), 'デモの案内を続けます', s)
      return
    }
    const before = this.seen
    const wasActive = this.seenActive
    this.seen = now
    if (s.connected) this.seenActive = s.runActive
    if (before === undefined || before === now) return
    if (!s.connected) {
      // Lost while a run was going: its outcome is not known yet.
      if (wasActive) this.tell(text('event', { event: text('disconnected') }), '実行環境との接続が切れました', s)
      return
    }
    const state = s.runStatus ? stateName(s.runStatus.state) : 'none'
    const run = s.currentRunId ?? '-'
    if (before === 'offline') {
      if (wasActive) this.tell(text('event', { event: text('reconnected', { state }) }), '実行環境に再接続しました', s)
      return
    }
    if (s.runActive) {
      if (!before.startsWith(run + ':') || !wasActive) this.tell(text('event', { event: text('runStarted', { run }) }), '実行が始まりました', s)
    } else if (s.runStatus && (wasActive || !before.startsWith(run + ':'))) {
      this.tell(text('event', { event: text('runEnded', { run, state }) }), `実行が終わりました（${runState(s.runStatus.state).label}）`, s)
    }
  }

  /** The run as the guide follows it: "offline", or "<run id>:<state>". */
  runKey(s: ProjectSession) {
    if (!s.connected) return 'offline'
    return `${s.currentRunId ?? '-'}:${s.runStatus ? stateName(s.runStatus.state) : 'none'}`
  }
}

export const guide = new GuideController()
