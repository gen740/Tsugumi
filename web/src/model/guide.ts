// The guide bird in the bottom-right corner: one conversation for the whole
// app, run by the host server with the same model connection as the Agent
// (without one it answers from a fixed script: guideScript.ts). It explains what is on the screen and leads demo
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
import { homeStep, noConnection, scriptDemoFor, scriptDemos, scriptStart, scriptStep, type ScriptStep } from './guideScript'
import { L } from '../i18n'

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
export interface GuideBubble {
  id: number; say?: string; choices: string[]; actions: GuideAction[]; target?: string; thinking?: boolean; farewell?: boolean; more?: boolean
  /** Said by the app (a demo picked from the first questions), not by a model: its ほかのデモ asks those questions again. */
  local?: boolean
}

/** The first questions on Home, written by the app (no model): what to try,
 *  then a field or the Drafts, then a demo. */
export type OfferStep = { kind: 'start' } | { kind: 'fields' } | { kind: 'field'; field: string } | { kind: 'drafts' }
export const OTHER_DEMOS = L('ほかのデモ')

/** The panels under the canvas a guide_show "open" names. */
export const OPEN_PANELS: Record<string, 'log' | 'timeline' | 'terminal' | undefined> = { logs: 'log', timeline: 'timeline', terminal: 'terminal' }

/** Whether an answer says more than its bubble (the bubble then points to the chat). */
export const saysMore = (reply: string, say?: string) => reply.trim().length > (say?.length ?? 0) + 120

/** What the Agent's tool call looks at, for the bird (as the Mac app's
 *  PresenceEvents): the Card it reads, runs or writes, and a line about it. */
export function workScene(tool: string, args: string): { card?: string; line?: string } {
  let o: any
  try {
    o = JSON.parse(args)
  } catch {
    o = {}
  }
  const segs: string[] | undefined = Array.isArray(o?.card) ? o.card : Array.isArray(o?.card?.segments) ? o.card.segments : undefined
  const card = segs?.length ? segs.join('/') : undefined
  const fileCard = (p: unknown) => {
    const parts = typeof p === 'string' ? p.split('/').filter(Boolean) : []
    return parts.length >= 2 && parts[0] === 'definitions' ? parts[1] : undefined
  }
  switch (tool) {
    case 'run': return card ? { card, line: L('{0} から実行しています', card) } : { line: L('実行しています') }
    case 'run_record': case 'log_range': case 'run_plan': return card ? { card, line: L('{0} の記録を見ています', card) } : { line: L('実行の記録を見ています') }
    case 'project_file_read': {
      const c = fileCard(o?.path)
      return c ? { card: c, line: L('{0} を読んでいます', c) } : { line: L('ファイルを読んでいます') }
    }
    case 'project_save': {
      const changes: any[] = Array.isArray(o?.changes) ? o.changes : []
      const c = changes.map((x) => x?.value_type === 'PutDefinition' ? x?.value?.id : fileCard(x?.value?.path)).find((x) => typeof x === 'string')
      return c ? { card: c, line: L('{0} を書いています', c) } : { line: L('保存しています') }
    }
  }
  const lines: Record<string, string> = {
    project_inspect: L('プロジェクトを見ています'), reference_read: L('書き方を調べています'), project_validate: L('変更を確かめています'),
    run_status: L('実行の終わりを待っています'), run_invocations: L('実行の記録を見ています'), run_history: L('実行の記録を見ています'),
    artifact_list: L('出力を見ています'), artifact_read: L('出力を見ています'), project_file_list: L('ファイルを読んでいます'),
    attached_folder_read: L('添えられたフォルダを読んでいます'), run_cancel: L('実行を止めています'), delegate: L('手分けして進めています'),
  }
  return { line: lines[tool] }
}

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
  /** The bird is there: with a model connection, or answering from its script. */
  available = false
  /** A model connection the guide can use (a key for the chosen provider). */
  connected = false
  /** No connection: Tsugumi answers from guideScript and calls no model. */
  get scripted() {
    return this.available && !this.connected
  }
  /** The first start's question (provider and key, or skip) was answered. */
  setupDone = true
  /** "デモを見ますか？" was answered (見る or あとで). */
  demoAnswered = true
  /** "ガイドを見てみますか？" is asked: the user's own save of a key (or
   *  sign-in) has just made the guide usable. */
  demoInvite = false
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
  /** The chat's Agent at work (outside demo mode): the Card the bird flies to and what it says there. */
  work?: { card?: string; line?: string; from?: string }
  /** An agent outside the page (Claude Code through the plugin) worked on the
   *  open project: the bird is there for it even without a model key here. */
  external = false
  private externalIdle?: ReturnType<typeof setTimeout>

  /** The bird is in the window: Tsugumi can speak (a usable connection), or Claude Code works here. */
  get present() {
    return this.available || this.external
  }

  /** The files the chat's Agent is writing: while one plays, the bird is at its Card. */
  writer?: { at(now: number): { card: string } | undefined }
  /** The chat's helpers: while they are out, this bird stands out among them. */
  team?: { out(now: number): boolean }

  /** The chat's Agent (or Claude Code) called a tool: the bird goes to the Card it looks at. */
  chatWork(tool: string, args: string, from?: string) {
    if (!this.present) return
    const scene = workScene(tool, args)
    // Without a Card of its own the bird stays by the last one, saying what it does now.
    this.work = { card: scene.card ?? this.work?.card, line: scene.line ?? this.work?.line, from }
    this.changed()
  }

  /** The chat's project was left: the bird no longer speaks or works for it. */
  chatClosed(chat: { }) {
    this.work = undefined
    if (this.speakingChat === chat) {
      this.speakingChat = undefined
      this.dismissBubble()
    }
    this.changed()
  }

  /** The chat's Agent ended its turn: the bird goes home. */
  chatWorkEnded() {
    if (!this.work) return
    this.work = undefined
    this.changed()
  }

  /** Claude Code's tool calls on the open project. It has no turns the page
   *  sees: the bird goes home when no call follows for a few seconds. */
  private externalWork(from: string, e: { kind: string; name?: string; arguments?: string }) {
    clearTimeout(this.externalIdle)
    if (!this.external) {
      this.external = true
      this.changed()
    }
    if (e.kind === 'toolStarted' && e.name) this.chatWork(e.name, e.arguments ?? '{}', from)
    if (e.kind === 'toolFinished') this.externalIdle = setTimeout(() => { if (this.work?.from === from) this.chatWorkEnded() }, 10_000)
  }
  /** The chat's answer the bubble points to was asked for: the chat shows it marked. */
  chatFlash = 0

  /** The bubble's "詳しくはチャットに": the chat (or on Home the panel) shows the whole answer. */
  showDetails() {
    if (this.speakingChat) this.chatFlash = Date.now()
    else this.open = true
    this.changed()
  }
  /** What to open for the user to look at ("card:<path>": that Card's settings;
   *  "logs", "timeline", "terminal": a panel under the canvas; "versions"), and a
   *  workflow scope to show (pointing into a Card in Card): taken by the project's workbench. */
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
      // The guide's conversation is Home's: a tab on a project goes on undisturbed (and reads it on returning).
      if (e.type === 'guide' && !this.contextSession) this.handle(e.event)
      if (e.type === 'agentWork' && e.project === this.contextSession?.dir) this.externalWork(String(e.from ?? 'Claude Code'), e.event)
      if (e.type === 'reconnected') void this.refresh()
    })
  }

  set<K extends 'open' | 'demosRequested'>(k: K, v: GuideController[K]) {
    ;(this as GuideController)[k] = v
    this.changed()
  }

  /** The first start asks for a model key (or a skip): nothing usable and never answered. */
  get needsSetup() {
    return this.loaded && !this.setupDone && !this.connected
  }

  /** The guide greets and offers the demo once (without a connection, the scripted tour). */
  get offersDemo() {
    return this.available && !this.demoAnswered
  }

  /** Where the first questions are; ほかのデモ by a demo the app pointed at puts them out again. */
  offerStep: OfferStep = { kind: 'start' }
  asksAgain = false
  /** The corner shows the first questions. */
  get asksDemo() {
    return this.available && (!this.demoAnswered || this.asksAgain)
  }

  setOfferStep(step: OfferStep) {
    this.offerStep = step
    this.changed()
  }

  /** Reads the guide's state (after Settings or the first start changed the key). */
  async refresh() {
    try {
      const r = await api<{ available: boolean; setupDone: boolean; demoAnswered: boolean; tour: GuideTour | null; busy: boolean; conversation: { archive: Message[]; messages: Message[] } | null }>('/api/guide', {})
      const wasOffered = this.offersDemo
      this.available = true
      this.connected = r.available
      this.setupDone = r.setupDone
      this.demoAnswered = r.demoAnswered
      this.tour = r.tour
      this.busy = r.busy
      this.loaded = true
      if (r.conversation && !this.busy) this.load([...r.conversation.archive, ...r.conversation.messages])
      void wasOffered
    } catch {}
    this.changed()
  }

  private async saveState(v: { setupDone?: boolean; demoAnswered?: boolean; tour?: GuideTour | null }) {
    try {
      await api('/api/guide/state', v)
    } catch {}
  }

  /** The user saved a key or signed in (Settings): when that is what makes
   *  the guide usable, the page asks whether to see the demo. */
  async connectionSaved() {
    const before = this.connected
    await this.refresh()
    this.invite(!before)
  }

  /** The first start was answered: with a key that connected (the page then
   *  asks whether to see the demo), or skipped. */
  async finishSetup(connected = false) {
    this.setupDone = true
    this.changed()
    await this.saveState({ setupDone: true })
    await this.refresh()
    this.invite(connected)
  }

  /** A key the user just saved connected the guide: asked even after the
   *  scripted tour's offer was answered, since the guide now uses the model. */
  private invite(newly: boolean) {
    if (!newly || !this.connected) return
    this.demoInvite = true
    this.changed()
  }

  /** The dialog was answered: あとで is the bubble's あとで (ガイドを見る answers it by starting the tour). */
  closeInvite(later: boolean) {
    this.demoInvite = false
    if (later) this.answerDemoOffer()
    else this.changed()
  }

  /** Asks the first start's questions again (Settings). */
  async resetIntroduction() {
    await this.saveState({ setupDone: false, demoAnswered: false })
    this.demoAnswered = false
    this.offerStep = { kind: 'start' }
    await this.refresh()
  }

  answerDemoOffer() {
    this.demoAnswered = true
    this.asksAgain = false
    this.changed()
    void this.saveState({ demoAnswered: true })
  }

  /** A demo picked from the first questions: the bird flies to its card in the
   *  demo list and offers to open it (the app says it; no model). */
  pickDemo(d: { id: string; title: string; field?: string }) {
    this.answerDemoOffer()
    this.speakingChat = undefined
    const say = d.field ? L('「{0}」です。開くと、チャットのツグミが Card の動きと結果を一緒に見ていきます。', d.title)
      : L('「{0}」です。Card の形だけができていて、中身を AI が書きます。開いたら、チャットのツグミと一緒に実装させてみましょう。', d.title)
    this.show({ highlight: `demo:${d.id}`, say, choices: [OTHER_DEMOS], actions: [{ kind: 'open_demo', label: L('このデモを開く'), demo: d.id }] })
    if (this.bubble) this.bubble.local = true
    this.changed()
  }

  /** おすすめを聞く in a field: the guide picks one of its demos. */
  recommend(field: string) {
    this.answerDemoOffer()
    this.demosRequested = true
    this.ask(L('「{0}」のおすすめは？', field), 'demos', undefined, undefined, `[Interest] The user asks which demo of the field "${field}" to see; pick one of its demos.`)
  }

  // ---- Conversation ----------------------------------------------------------------

  /** What the user is interested in (the first question on Home): the guide
   *  picks a demo for it and flies to it in the demo list. */
  answerInterest(q: string) {
    if (!q.trim()) return
    this.answerDemoOffer()
    this.demosRequested = true
    if (this.scripted) return this.play(scriptDemoFor(q) + '/pick', q.trim())
    this.ask(q, 'demos', undefined, undefined, "[Interest] The user answered what interests them; pick a demo for it.")
  }

  /** A question of the user, with what their screen shows. */
  ask(q: string, screen: GuideScreen, session?: ProjectSession, tab?: WorkflowTab, note?: string) {
    const t = q.trim()
    if (!this.available || this.busy || !t) return
    if (this.scripted) return this.answerScripted(t)
    this.readSession = session
    this.items.push({ id: ++this.n, role: 'user', text: t })
    void this.send((note ? note + '\n' : '') + t + '\n\n' + this.screenNote(screen, session, tab), t, session)
  }

  /** Something the app tells the guide (a tour step, a run that ended); one
   *  that comes while it answers waits for the answer to end. Without a
   *  connection the chat shows `script`'s step instead (undefined: nothing). */
  private tell(prompt: string, display: string, session?: ProjectSession, script?: string) {
    if (this.scripted) {
      if (script && session) this.play(script, display, app.chat(session))
      return
    }
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
      this.status = L('うまく答えられませんでした: {0}', (e as Error).message)
      this.changed()
    }
  }

  cancel() {
    this.queued = []
    void api('/api/guide/cancel', {})
    this.busy = false
    this.activity = undefined
    this.status = L('回答を止めました。')
    this.changed()
  }

  retry() {
    if (!this.connected || this.busy) return
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
    this.activity = L('考えています')
    this.changed()
  }

  private handle(e: any) {
    switch (e.kind) {
      case 'text': {
        this.activity = L('考えています')
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
          this.activity = e.name.startsWith('run') || e.name === 'log_range' ? L('実行の様子を見ています') : L('プロジェクトを読んでいます')
        }
        break
      case 'discardText':
        if (this.items[this.items.length - 1]?.role === 'assistant') this.items.pop()
        break
      case 'retrying':
        this.status = L('再試行中: {0}', e.message)
        break
      case 'finished':
        this.ended()
        this.flush()
        break
      case 'stepLimit':
        this.ended()
        this.status = L('答えをまとめきれませんでした。聞き方を変えてもう一度どうぞ。')
        break
      case 'error':
        this.ended()
        this.bubble = undefined
        this.queued = []
        this.failed = e.message !== 'cancelled'
        this.status = e.message === 'cancelled' ? L('回答を止めました。') : L('うまく答えられませんでした: {0}', e.message)
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
      // On Home the bubble is the conversation: it points nowhere else.
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
    if (this.contextSession && !session) void this.refresh()
    this.contextScreen = screen
    this.contextSession = session
    this.contextTab = tab
  }

  /** A choice tapped in a bubble: the user's answer, to whichever Tsugumi offered it. */
  choose(c: string) {
    if (this.bubble?.local && c === OTHER_DEMOS) {
      this.dismissBubble()
      this.asksAgain = true
      this.changed()
      return
    }
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
    if (shown.open && (shown.open.startsWith('card:') || shown.open === 'versions' || OPEN_PANELS[shown.open])) this.openRequest = shown.open
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
    b = { ...b, more: saysMore(reply, b.say) }
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

  // ---- Without a model connection -------------------------------------------------

  /** The step of the script shown last (its choices are the ones out). */
  private scriptAt?: string

  /** A message without a connection: $demo in a project's chat begins the
   *  tour; a tapped choice leads to its step (in the tour's chat, or on Home
   *  after Home's steps); anything else gets the one fixed reply (pointing to
   *  Settings and $demo), with the step's choices still there to go on. */
  answerScripted(t: string, chat?: ChatController) {
    if (chat && t.split(/\s+/).includes('$demo')) return this.startScriptedTour(t, chat)
    const here = chat ? this.touring(chat.session) : !!this.scriptAt && homeStep(this.scriptAt)
    const current = here && this.scriptAt ? scriptStep(this.scriptAt) : undefined
    const next = current?.next[t]
    if (next) return this.play(next, t, chat)
    const reply = noConnection()
    reply.show = { ...reply.show, highlight: current ? this.highlight : undefined, choices: current?.show.choices ?? [],
      // In the tour's own chat the tour is already there.
      actions: chat && current ? reply.show.actions.filter((a) => a.kind !== 'open_demo') : reply.show.actions }
    this.answer(reply, t, chat)
  }

  /** $demo without a connection: in a copy of a demo the script has a tour
   *  of (by its name), the tour begins there (elsewhere the reply says where it can). */
  private startScriptedTour(asked: string, chat: ChatController) {
    const s = chat.session
    const demo = app.status?.demos.find((d) => scriptDemos.includes(d.id) && d.title === s.scopeName([]))
    if (!demo) return this.play('otherProject', asked, chat)
    this.answerDemoOffer()
    this.tour = { demo: demo.id, title: demo.title, path: s.dir, started: true }
    void this.saveState({ tour: this.tour })
    this.seen = this.runKey(s)
    this.seenActive = s.runActive
    this.play(scriptStart(demo.id, 'start'), asked, chat)
  }

  private play(id: string, asked?: string, chat?: ChatController) {
    const step = scriptStep(id)
    if (!step) return
    this.scriptAt = id
    this.answer(step, asked, chat)
  }

  /** A step of the script as an answer: in the chat (its bird speaks only in
   *  the tour's copy) or in the panel, and in the bubble. */
  private answer(step: ScriptStep, asked?: string, chat?: ChatController) {
    if (chat) {
      chat.showScripted(asked, step.text, step.show)
      if (!this.touring(chat.session)) return
      this.speakingChat = chat
    } else {
      this.speakingChat = undefined
      for (const i of this.items) { i.actions = undefined; i.choices = undefined }
      if (asked) this.items.push({ id: ++this.n, role: 'user', text: asked })
      this.items.push({ id: ++this.n, role: 'assistant', text: step.text, actions: step.show.actions, choices: step.show.choices })
    }
    this.show(step.show)
    if (this.bubble) this.bubble = { ...this.bubble, more: saysMore(step.text, step.show.say) }
    this.leaveAfterFarewell()
    this.changed()
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
      lines.push(L('screen: Home (recent projects, new project, open, デモを見る)'))
    }
    if (this.tour && !(s && this.same(this.tour.path, s))) lines.push(`tour: the demo "${this.tour.demo}" is paused (the user is not in its copy)`)
    // The whole list where a demo is chosen; in a project, only the ids (open_demo).
    const demos = app.status?.demos ?? []
    if (screen === 'project' && s) lines.push('demos (ids; their descriptions come on Home): ' + demos.map((d) => d.id).join(', '))
    else {
        lines.push('demos (id, title, field, tags: description):')
      for (const d of demos) lines.push(`- ${d.id} "${d.title}"${d.field ? `, field ${d.field}` : ''}, ${d.tags.join('/')}: ${d.description.slice(0, 220)}`)
    }
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
      this.tell(text('tourStart', { demo: t.demo, title: t.title }), L('デモ「{0}」の案内を始めます', t.title), s, scriptStart(t.demo, 'start'))
      return
    }
    if (this.resumeAsked && s.connected) {
      this.resumeAsked = false
      this.seen = now
      this.seenActive = s.runActive
      this.tell(text('tourResume', { demo: t.demo }), L('デモの案内を続けます'), s,
        this.scriptAt?.startsWith(t.demo + '/') ? this.scriptAt : scriptStart(t.demo, 'resume'))
      return
    }
    const before = this.seen
    const wasActive = this.seenActive
    this.seen = now
    if (s.connected) this.seenActive = s.runActive
    if (before === undefined || before === now) return
    if (!s.connected) {
      // Lost while a run was going: its outcome is not known yet.
      if (wasActive) this.tell(text('event', { event: text('disconnected') }), L('実行環境との接続が切れました'), s)
      return
    }
    const state = s.runStatus ? stateName(s.runStatus.state) : 'none'
    const run = s.currentRunId ?? '-'
    if (before === 'offline') {
      if (wasActive) this.tell(text('event', { event: text('reconnected', { state }) }), L('実行環境に再接続しました'), s)
      return
    }
    if (s.runActive) {
      // The bird watches the run; Tsugumi is told only how it ended.
    } else if (s.runStatus && (wasActive || !before.startsWith(run + ':'))) {
      const ran = this.scriptAt ? scriptStep(this.scriptAt)?.ran : undefined
      this.tell(text('event', { event: text('runEnded', { run, state }) }), L('実行が終わりました（{0}）', runState(s.runStatus.state).label), s,
        s.runStatus.state === D.RunState.succeeded ? ran?.ok : ran?.else)
    }
  }

  /** The run as the guide follows it: "offline", or "<run id>:<state>". */
  runKey(s: ProjectSession) {
    if (!s.connected) return 'offline'
    return `${s.currentRunId ?? '-'}:${s.runStatus ? stateName(s.runStatus.state) : 'none'}`
  }
}

export const guide = new GuideController()
