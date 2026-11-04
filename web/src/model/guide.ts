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
import type { ChatController } from './chat'
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
  /** Replies to tap (guide_show "choices"), under the newest reply only. */
  choices?: string[]
}

/** The demo being shown: its id and title, the copy's folder, and whether the guide began it. */
export interface GuideTour { demo: string; title: string; path: string; started: boolean }

/** What the screen the user asks from shows. */
export type GuideScreen = 'home' | 'demos' | 'project'

/** A guide_show call: where the bird flies, what it says there, the replies
 *  it offers to tap and the buttons that act. */
export interface GuideShow { highlight?: string; say?: string; choices: string[]; actions: GuideAction[]; endTour?: boolean; open?: string }

/** What the bird says in its bubble, where it is (target; undefined: at
 *  home) and what can be tapped there; `thinking` while Tsugumi answers. */
export interface GuideBubble { id: number; say?: string; choices: string[]; actions: GuideAction[]; target?: string; thinking?: boolean; farewell?: boolean }

/** The opening of a reply for the bubble: its first sentences, up to about 120 characters. */
export function firstSentence(text: string): string | undefined {
  const line = text.replace(/\*\*|`/g, '').split('\n').map((l) => l.replace(/^[\s#>*•-]+/, '').trim()).find((l) => l) ?? ''
  if (!line) return undefined
  // Whole sentences while they fit, at least the first.
  const sentences = line.match(/[^。！？!?]*[。！？!?]|[^。！？!?]+$/g) ?? [line]
  let s = ''
  for (const x of sentences) {
    if (s && s.length + x.length > 120) break
    s += x.trimStart()
  }
  return s.length > 140 ? s.slice(0, 138) + '…' : s
}

/** What a guide_show call points at, says and offers. */
export function parseShow(args: string): GuideShow {
  let v: any
  try {
    v = JSON.parse(args)
  } catch {
    return { choices: [], actions: [] }
  }
  const actions = (Array.isArray(v?.actions) ? v.actions : []).flatMap((a: any): GuideAction[] => {
    if (!KINDS.includes(a?.kind)) return []
    const label = typeof a.label === 'string' && a.label.trim() ? a.label.trim() : a.kind
    return [{ kind: a.kind, label, demo: typeof a.demo === 'string' ? a.demo : undefined, card: typeof a.card === 'string' ? a.card : undefined }]
  }).slice(0, 3) // unknown kinds are dropped first; at most three buttons
  const h = typeof v?.highlight === 'string' ? v.highlight.trim() : ''
  const say = typeof v?.say === 'string' ? v.say.trim() : ''
  const choices = (Array.isArray(v?.choices) ? v.choices : []).filter((c: unknown): c is string => typeof c === 'string' && !!c.trim())
    .map((c: string) => c.trim()).slice(0, 4)
  return { highlight: h || undefined, say: say || undefined, choices, actions, endTour: v?.endTour === true,
    open: typeof v?.open === 'string' ? v.open.trim() : undefined }
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
  /** The part of the screen the guide points at and the bird flies to (guide_show). */
  highlight?: string
  /** The bird's speech bubble (undefined: it sits quietly at home). */
  bubble?: GuideBubble
  /** The project chat whose bubble is out (undefined: the guide's own on Home). */
  speakingChat?: ChatController
  /** The project and workflow on the screen (choices and buttons act there). */
  contextScreen: GuideScreen = 'home'
  contextSession?: ProjectSession
  contextTab?: WorkflowTab
  /** The bird has left home (its home shows no bird meanwhile). */
  away = false
  /** A Card settings window to open ("card:<path>"), and a workflow scope to
   *  show (pointing into a Card in Card): taken by the project's workbench. */
  openRequest?: string
  scopeRequest?: string[]
  /** Home's demo list asked for by the guide (show_demos). */
  demosRequested = false
  private n = 0
  private pending: GuideAction[] = []
  private pendingChoices: string[] = []
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
      }
      void wasOffered
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
  }

  answerDemoOffer() {
    this.demoAnswered = true
    this.changed()
    void this.saveState({ demoAnswered: true })
  }

  // ---- Conversation ----------------------------------------------------------------

  /** What the user is interested in (the first question on Home): the guide
   *  picks a demo for it and flies to it in the demo list. */
  answerInterest(q: string) {
    if (!q.trim()) return
    this.answerDemoOffer()
    this.demosRequested = true
    this.ask(q, 'demos', undefined, undefined, "[Interest] The user answered what interests them; pick a demo for it.")
  }

  /** A question of the user, with what their screen shows. */
  ask(q: string, screen: GuideScreen, session?: ProjectSession, tab?: WorkflowTab, note?: string) {
    const t = q.trim()
    if (!this.available || this.busy || !t) return
    this.readSession = session
    this.items.push({ id: ++this.n, role: 'user', text: t })
    void this.send((note ? note + '\n' : '') + t + '\n\n' + this.screenNote(screen, session, tab), t, session)
  }

  /** Something the app tells the guide (a tour step, a run that ended); one
   *  that comes while it answers waits for the answer to end. */
  private tell(prompt: string, display: string, session?: ProjectSession) {
    // In the demo's copy, Tsugumi of its chat leads the tour.
    if (session) {
      app.chat(session).tell(prompt, display)
      return
    }
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
    this.pendingChoices = []
    // The bird stays where it is and thinks.
    this.speakingChat = undefined
    this.bubble = { id: ++this.n, choices: [], actions: [], target: this.highlight, thinking: true }
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
          this.speakingChat = undefined
          this.show(shown)
          this.pending.push(...shown.actions)
          this.pendingChoices.push(...shown.choices)
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
        this.bubble = undefined
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
    // The bird says the answer's first sentence where it is, when guide_show gave no line.
    const last = this.items[this.items.length - 1]
    const reply = last?.role === 'assistant' ? last.text : ''
    if (this.bubble) {
      let b = this.bubble
      if (b.thinking) b = { id: ++this.n, choices: [], actions: [], target: b.target }
      if (!b.say) b = { ...b, say: firstSentence(reply) }
      this.bubble = !b.say && !b.choices.length && !b.actions.length && !b.target ? undefined : b
      this.leaveAfterFarewell()
    }
    if (!this.pending.length && !this.pendingChoices.length) return
    // Only the newest buttons and choices are offered.
    for (const i of this.items) { i.actions = undefined; i.choices = undefined }
    if (last?.role === 'assistant') { last.actions = this.pending; last.choices = this.pendingChoices }
    else this.items.push({ id: ++this.n, role: 'assistant', text: '', actions: this.pending, choices: this.pendingChoices })
    this.pending = []
    this.pendingChoices = []
  }

  // ---- Bubbles and Tsugumi of a project's chat ----------------------------------------

  /** The screen the page shows, for choices tapped in a bubble. */
  setContext(screen: GuideScreen, session?: ProjectSession, tab?: WorkflowTab) {
    this.contextScreen = screen
    this.contextSession = session
    this.contextTab = tab
  }

  /** A choice tapped in a bubble: the user's answer, to whichever Tsugumi offered it. */
  choose(c: string) {
    if (this.speakingChat) this.speakingChat.send(c)
    else this.ask(c, this.contextScreen, this.contextSession, this.contextTab)
  }

  /** The bubble is closed: the bird flies back home. */
  dismissBubble() {
    this.bubble = undefined
    this.highlight = undefined
    this.changed()
  }

  /** The chat's Tsugumi called guide_show: the bird flies and speaks for it. */
  chatShow(chat: ChatController, args: string) {
    if (!this.available) return
    this.speakingChat = chat
    this.show(parseShow(args))
    this.changed()
  }

  /** A guide_show of either Tsugumi: where the bird goes, what opens, what it says. */
  private show(shown: GuideShow) {
    if (shown.endTour) this.endTour()
    if (shown.open?.startsWith('card:')) this.openRequest = shown.open
    this.point(shown.highlight)
    this.bubble = { id: ++this.n, say: shown.say, choices: shown.choices, actions: shown.actions, target: shown.highlight, farewell: shown.endTour }
  }

  /** After its last words the bird flies home by itself. */
  private leaveAfterFarewell() {
    const b = this.bubble
    if (!b?.farewell || b.thinking) return
    setTimeout(() => { if (this.bubble?.id === b.id) this.dismissBubble() }, 7000)
  }

  /** The chat answers: a bubble that is out shows that it thinks. */
  chatTurnBegan(chat: ChatController) {
    if (this.speakingChat !== chat || !this.bubble) return
    this.bubble = { id: ++this.n, choices: [], actions: [], target: this.bubble.target, thinking: true }
    this.changed()
  }

  /** The chat's answer ended: a bubble still thinking says its first sentence. */
  chatTurnEnded(chat: ChatController, reply: string) {
    if (this.speakingChat !== chat || !this.bubble) return
    let b = this.bubble
    if (b.thinking) b = { id: ++this.n, choices: [], actions: [], target: b.target }
    if (!b.say) b = { ...b, say: firstSentence(reply) }
    this.bubble = !b.say && !b.choices.length && !b.actions.length ? undefined : b
    this.leaveAfterFarewell()
    this.changed()
  }

  /** Whether a project is the copy of the demo being shown. */
  touring(s: ProjectSession) {
    return !!this.tour && this.same(this.tour.path, s)
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
    let choices: string[] = []
    const flush = () => {
      if (shown.length || choices.length) out.push({ id: ++this.n, role: 'assistant', text: '', actions: shown, choices })
      shown = []
      choices = []
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
          out.push({ id: ++this.n, role: 'assistant', text: b.text, actions: shown.length ? shown : undefined, choices: choices.length ? choices : undefined })
          shown = []
          choices = []
        }
        if (b.type === 'tool_use' && b.name === 'guide_show') {
          const g = parseShow(b.input ?? '{}')
          shown = g.actions
          choices = g.choices
        }
      }
    }
    flush()
    const newest = out.map((x) => !!x.actions?.length || !!x.choices?.length).lastIndexOf(true)
    out.forEach((x, i) => { if (i !== newest) { x.actions = undefined; x.choices = undefined } })
    this.items = out
  }

  // ---- Pointing --------------------------------------------------------------------

  /** Where the bird flies and the ring is drawn, until the bubble is closed or it moves on. */
  private point(target?: string) {
    clearTimeout(this.highlightTimer)
    this.highlight = target
    // A demo's card is in Home's demo list: it opens to show it.
    if (target?.startsWith('demo:')) this.demosRequested = true
    // A Card inside a Card in Card: its workflow opens on the canvas.
    if (target?.startsWith('card:')) {
      const parts = target.slice(5).split('/')
      if (parts.length > 1) this.scopeRequest = parts.slice(0, -1)
    }
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
    lines.push('demos (id, title, tags: description):')
    for (const d of app.status?.demos ?? []) lines.push(`- ${d.id} "${d.title}", ${d.tags.join('/')}: ${d.description.slice(0, 220)}`)
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
    this.open = false
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
      // The bird watches the run; Tsugumi is told only how it ended.
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
