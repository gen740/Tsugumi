// The guide bird in the bottom-right corner (model/guide.ts): the bird, its
// panel with the greeting, the demo offer, the tour and the replies with
// their buttons, the rings it draws around what it talks about, and the
// first start's model key question. The macOS app's GuideViews does the same.
import { useEffect, useRef, useState } from 'react'
import { api } from '../api'
import { app } from '../model/app'
import type { ChatController } from '../model/chat'
import { guide, GuideAction, GuideBubble, GuideItem, GuideScreen, GuideTour } from '../model/guide'
import { useObserve } from '../model/observable'
import { prefs } from '../model/prefs'
import { errorText, key, ProjectSession } from '../model/session'
import type { WorkflowTab } from '../model/workspace'
import { runAll } from './actions'
import { Button, Sheet } from './controls'
import { PALETTE, useBird } from './home'
import { Icon } from './icons'
import { Markdown } from './markdown'

/** What the page lets the guide's buttons do (App sets it). */
export interface GuideWindowActions {
  openProject: (path: string) => void
  home: () => void
  newProject: () => void
}
let windowActions: GuideWindowActions | undefined
export function setGuideWindow(a: GuideWindowActions) {
  windowActions = a
}

/** A ring around what the guide points at: `data-guide` of the control. */
function GuideRings() {
  const h = guide.highlight
  if (!h) return null
  const sel = targetSelector(h)
  return <style>{`${sel} { outline: 2.5px solid ${PALETTE.rufous}; outline-offset: 4px; border-radius: 8px; animation: guide-ring 1.6s ease-in-out infinite; }`}</style>
}

/** Tsugumi on Home, in the bottom-right corner (in a project it is the
 *  chat's, GuideNest). It speaks in a bubble beside it (the demo offer, short
 *  answers, choices to tap), flies out to what it talks about (GuideFlyer),
 *  and a click opens its conversation above it. Shown only with a model connection. */
export function GuideCorner({ screen }: { screen: GuideScreen }) {
  useObserve(guide, prefs)
  useEffect(() => guide.setContext(screen), [screen])
  if (!guide.available) return null
  const mood = guide.busy ? 'thinking' : guide.failed ? 'worried' : 'idle'
  const perform = makePerform()
  return (
    <div className="guide-corner">
      {guide.open && <GuidePanel screen={screen} perform={perform} />}
      {!guide.open && !guide.away && (guide.offersDemo ? <OfferBubble perform={perform} />
        : guide.bubble && !guide.speakingChat ? <BubbleView bubble={guide.bubble} perform={perform} />
          : guide.failed ? <span className="guide-bubble material">うまく答えられませんでした</span> : null)}
      <button type="button" className={`guide-avatar ${mood}${prefs.tsugumiStill ? ' still' : ''}${guide.away ? ' away' : ''}`}
        data-guide-home="" title={`ガイドのツグミに聞く（${prefs.label('toggleGuide')}）`}
        aria-label="ガイドのツグミ" aria-expanded={guide.open}
        onClick={() => guide.set('open', !guide.open)}>
        <GuideBird />
      </button>
    </div>
  )
}

/** Where Tsugumi waits in a project: at the top left of the chat (or on the
 *  chat button while the chat is closed). It flies out from here and back. */
export function GuideNest({ busy, small }: { busy: boolean; small?: boolean }) {
  useObserve(guide, prefs)
  if (!guide.available) return null
  return (
    <span className={`guide-nest${small ? ' small' : ''}${busy ? ' thinking' : ''}${prefs.tsugumiStill ? ' still' : ''}${guide.away ? ' away' : ''}`}
      data-guide-home="" title="ツグミ" aria-label={busy ? 'ツグミ（考えています）' : 'ツグミ'} role="img">
      <GuideBird />
    </span>
  )
}

/** What the bird says, with the replies to tap and the buttons that act. */
function BubbleView({ bubble, perform, style }: { bubble: GuideBubble; perform: (a: GuideAction) => void; style?: React.CSSProperties }) {
  return (
    <div className="guide-speech material" style={style} role="dialog" aria-label="ツグミの吹き出し">
      <div className="row gap6">
        {bubble.thinking ? <span className="secondary flex1">{guide.activity ? guide.activity + '…' : '考えています…'}</span>
          : <span className="flex1">{bubble.say ?? 'ここです'}</span>}
        <Button kind="borderless" small iconOnly icon="xmark" help="閉じる（ツグミは元の場所に戻ります）" onClick={() => guide.dismissBubble()} />
      </div>
      {!bubble.thinking && <Choices choices={bubble.choices} actions={bubble.actions} perform={perform} onChoice={(c) => guide.choose(c)} />}
      {!bubble.thinking && <ReplyField onSend={(t) => guide.choose(t)} />}
    </div>
  )
}

/** Replies to tap (sent as the user's message) and buttons that act. */
function Choices({ choices, actions, perform, onChoice, busy }: { choices: string[]; actions: GuideAction[]; perform: (a: GuideAction) => void; onChoice: (c: string) => void; busy?: boolean }) {
  if (!choices.length && !actions.length) return null
  return (
    <div className="col gap6">
      {!!actions.length && (
        <div className="row gap6 wrap">
          {actions.map((a, i) => <Button key={i} small kind={i === 0 ? 'prominent' : 'bordered'} disabled={!actionEnabled(a)} onClick={() => perform(a)}>{a.label}</Button>)}
        </div>
      )}
      {!!choices.length && (
        <div className="row gap6 wrap">
          {choices.map((c) => <button key={c} type="button" className="guide-choice" disabled={busy ?? (guide.busy || !!guide.speakingChat?.busy)} onClick={() => onChoice(c)}>{c}</button>)}
        </div>
      )}
    </div>
  )
}

/** The replies and buttons Tsugumi offered with the chat's last answer. */
export function ChatChoices({ chat, session, tab }: { chat: ChatController; session: ProjectSession; tab?: WorkflowTab }) {
  useObserve(chat, guide)
  if (chat.busy || (!chat.choices.length && !chat.offered.length)) return null
  return (
    <div className="chat-choices">
      <Choices choices={chat.choices} actions={chat.offered} perform={makePerform(session, tab)} onChoice={(c) => chat.send(c)} busy={chat.busy} />
    </div>
  )
}

const SUGGESTIONS = ['データの集計とレポート', 'ライブ表示', 'C++ と Python', '天文・地震・タンパク質のデータ']

/** The first greeting: what are you interested in? The answer (typed, or a
 *  suggestion tapped) has Tsugumi pick a demo and fly to it. */
function OfferBubble({ perform }: { perform: (a: GuideAction) => void }) {
  return (
    <div className="guide-speech material" role="dialog" aria-label="ツグミの吹き出し">
      <span>こんにちは、ツグミです。何に興味がありますか？ ぴったりのデモを探してご案内します。</span>
      <div className="row gap6 wrap">
        {SUGGESTIONS.map((c) => <button key={c} type="button" className="guide-choice" onClick={() => guide.answerInterest(c)}>{c}</button>)}
      </div>
      <ReplyField placeholder="興味のあること（例: 画像処理）" autoFocus onSend={(t) => guide.answerInterest(t)} />
      <div className="row gap8">
        <Button small onClick={() => perform({ kind: 'open_demo', label: '', demo: 'report' })}>定番のデモを見る</Button>
        <Button small onClick={() => guide.answerDemoOffer()}>あとで</Button>
      </div>
    </div>
  )
}

/** A one-line answer to Tsugumi in its bubble (Return sends it). */
function ReplyField({ onSend, placeholder = 'ツグミに返事', autoFocus }: { onSend: (t: string) => void; placeholder?: string; autoFocus?: boolean }) {
  const [text, setText] = useState('')
  const go = () => {
    const t = text.trim()
    if (!t) return
    setText('')
    onSend(t)
  }
  return (
    <form className="guide-reply-field" onSubmit={(e) => { e.preventDefault(); go() }}>
      <input className="field" value={text} placeholder={placeholder} aria-label="ツグミへの返事" autoFocus={autoFocus}
        onChange={(e) => setText(e.target.value)} onKeyDown={(e) => e.stopPropagation()} />
      <Button kind="borderless" small iconOnly icon="arrow.up.circle.fill" help="送信" type="submit" disabled={!text.trim()} />
    </form>
  )
}

function actionEnabled(a: GuideAction): boolean {
  const session = guide.speakingChat?.session ?? guide.contextSession
  switch (a.kind) {
    case 'run': return !!session && session.connected && !session.runActive
    case 'stop': return !!session?.runActive
    case 'open_card': return !!session
    case 'open_demo': case 'home': case 'new_project': return !session?.runActive
    default: return true
  }
}

/** The elements the guide points at with `h`: the run button is also
 *  "stop"; "card:<id>" is also that Card inside a Card in Card on the screen. */
export function targetSelector(h: string): string {
  const t = h === 'stop' ? 'run' : h
  const sel = [`[data-guide="${CSS.escape(t)}"]`]
  if (t.startsWith('card:') && !t.slice(5).includes('/')) sel.push(`[data-guide^="card:"][data-guide$="/${CSS.escape(t.slice(5))}"]`)
  return sel.join(', ')
}

type Box = { left: number; top: number; width: number; height: number; flip: boolean }
const BIRD = { w: 90, h: 64 }
const visible = (el: Element | null | undefined) => !!el && (el as HTMLElement).getClientRects().length > 0

/** The bird away from home, over the whole page: it flies to what Tsugumi
 *  points at, perches beside it with its bubble, and flies home when the
 *  bubble is closed. When a project loads it flies into the loading card's
 *  bird and grows to its size (the card's bird is it), and comes out of it
 *  again when loading ends. Without motion it appears there instead of flying. */
export function GuideFlyer() {
  useObserve(guide, prefs)
  const [box, setBox] = useState<Box>()
  const [arrived, setArrived] = useState(false)
  const [moving, setMoving] = useState(false)
  const [inCard, setInCard] = useState(false)
  const bubbleRef = useRef<HTMLDivElement>(null)
  const target = guide.available && guide.bubble?.target ? guide.bubble.target : undefined
  const still = prefs.tsugumiStill
  // Where the target and home are now (Cards move with the canvas, loading comes and goes).
  const [tick, setTick] = useState(0)
  useEffect(() => {
    const t = setInterval(() => setTick((n) => n + 1), 250)
    return () => clearInterval(t)
  }, [])
  const homeBox = (): Box | undefined => {
    const el = [...document.querySelectorAll('[data-guide-home]')].find(visible)
    const r = el?.getBoundingClientRect()
    return r ? { left: r.left + r.width / 2 - BIRD.w / 2, top: r.bottom - 4 - BIRD.h, width: BIRD.w, height: BIRD.h, flip: true } : undefined
  }
  const loadingBox = (): Box | undefined => {
    const el = [...document.querySelectorAll('[data-guide-loading] .tsugumi-bird')].find(visible)
    const r = el?.getBoundingClientRect()
    if (!r || !r.width) return undefined
    // The flying bird's picture (viewBox 130 270 680 470) laid over the card's bird.
    const width = r.width / (616 / 680), height = r.height / (410 / 470)
    return { left: r.left - (30 / 680) * width, top: r.top - (28 / 470) * height, width, height, flip: false }
  }
  const scrolled = useRef<string>()
  const perchBox = (): Box | undefined => {
    if (!target) return undefined
    const el = [...document.querySelectorAll(targetSelector(target))].find(visible)
    // A demo card or a control out of view is brought into view once.
    if (el && scrolled.current !== target) {
      scrolled.current = target
      const r0 = el.getBoundingClientRect()
      if (r0.bottom < 0 || r0.top > window.innerHeight) el.scrollIntoView({ block: 'center', behavior: still ? 'auto' : 'smooth' })
    }
    const r = el?.getBoundingClientRect()
    if (!r || r.width === 0) return undefined
    const right = r.right + 320 < window.innerWidth
    const x = Math.min(Math.max(right ? r.right + 18 : r.left - 18, 40), window.innerWidth - 40)
    const y = Math.min(Math.max(r.top + 10, 70), window.innerHeight - 10)
    return { left: x - BIRD.w / 2, top: y - BIRD.h, width: BIRD.w, height: BIRD.h, flip: right }
  }
  const loading = guide.available ? loadingBox() : undefined
  const perch = loading ? undefined : perchBox()
  const goal = loading ? 'loading' : perch ? 'perch' : 'home'
  const key = goal + (loading ? `${Math.round(loading.left)},${Math.round(loading.top)}` : perch ? `${Math.round(perch.left)},${Math.round(perch.top)}` : '')
  void tick
  useEffect(() => {
    const ms = still ? 0 : 900
    const setAway = (v: boolean) => { if (guide.away !== v) { guide.away = v; guide.changed() } }
    const flyTo = (to: Box, then: () => void) => {
      setMoving(true)
      // The next frame starts the flight from where the bird is.
      requestAnimationFrame(() => setBox(to))
      const t = setTimeout(() => { setMoving(false); then() }, ms)
      return () => clearTimeout(t)
    }
    if (goal === 'loading' && loading) {
      if (!guide.away) {
        const h = homeBox()
        if (!h) return
        setBox(h)
        setAway(true)
      }
      setInCard(false)
      setArrived(false)
      document.body.classList.add('guide-handoff')
      return flyTo(loading, () => { setInCard(true); document.body.classList.remove('guide-handoff') })
    }
    document.body.classList.remove('guide-handoff')
    if (goal === 'perch' && perch) {
      if (!guide.away || inCard) {
        if (!inCard) setBox(homeBox() ?? perch)
        setInCard(false)
        setAway(true)
      }
      setArrived(false)
      return flyTo(perch, () => setArrived(true))
    }
    if (guide.away) {
      setArrived(false)
      setInCard(false)
      const h = homeBox()
      if (!h) { setAway(false); return }
      return flyTo(h, () => setAway(false))
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [key])
  if (!guide.away || !box || inCard) return <GuideRings />
  const bw = bubbleRef.current?.offsetWidth ?? 300, bh = bubbleRef.current?.offsetHeight ?? 100
  const feetX = box.left + box.width / 2, feetY = box.top + box.height
  let bx = box.flip ? feetX - 30 : feetX - bw + 30
  let by = feetY + 12
  if (by + bh > window.innerHeight - 8) by = feetY - 70 - bh
  bx = Math.min(Math.max(bx, 8), window.innerWidth - bw - 8)
  by = Math.min(Math.max(by, 8), window.innerHeight - bh - 8)
  return (
    <div className="guide-flyer" aria-live="polite">
      <GuideRings />
      <div className={`guide-flying-bird${moving ? ' moving' : ''}${still ? ' still' : ''}${box.flip ? ' left' : ''}`}
        style={{ left: box.left, top: box.top, width: box.width, height: box.height, transition: still ? 'none' : undefined }} aria-hidden>
        <GuideBird />
      </div>
      {arrived && goal === 'perch' && guide.bubble && (
        <div ref={bubbleRef} className="guide-flyer-bubble" style={{ left: bx, top: by }}>
          <BubbleView bubble={guide.bubble} perform={makePerform(guide.speakingChat?.session ?? guide.contextSession, guide.contextTab)} />
        </div>
      )}
    </div>
  )
}

/** What a button of the guide does, with the app's own controls. */
export function makePerform(session?: ProjectSession, tab?: WorkflowTab) {
  return (a: GuideAction) => {
    switch (a.kind) {
      case 'open_demo':
        void openDemo(a.demo ?? 'report', session)
        return
      case 'show_demos':
        guide.set('demosRequested', true)
        if (session) windowActions?.home()
        return
      case 'run':
        if (session && session.connected && !session.runActive) runAll(session)
        return
      case 'stop':
        if (session?.runActive) void session.cancelRun()
        return
      case 'open_card': {
        if (!session || !a.card) return
        const scoped = key([...(tab?.scope ?? []), a.card])
        const k = session.instance(scoped) ? scoped : key([a.card])
        if (session.instance(k)) app.openWindow(session, { kind: 'card', card: k })
        return
      }
      case 'open_settings':
        app.openWindow(undefined, { kind: 'appSettings' })
        return
      case 'new_project':
        windowActions?.newProject()
        return
      case 'home':
        windowActions?.home()
    }
  }
}

async function openDemo(id: string, session?: ProjectSession) {
  const demo = app.status?.demos.find((d) => d.id === id) ?? app.status?.demos.find((d) => d.id === 'report')
  if (!demo) return
  if (session?.runActive) {
    app.setError('実行中はデモを開けません。停止するか終了を待ってください。')
    return
  }
  try {
    const p = await app.copyDemo(demo.id)
    // The bird leaves the demo's card: it flies into the loading card.
    guide.dismissBubble()
    guide.startTour(demo, p)
    windowActions?.openProject(p)
  } catch (e) {
    app.setError(e)
  }
}

/** The bird of the app icon, facing the page's content. */
function GuideBird() {
  const art = useBird()
  return (
    <svg viewBox="130 270 680 470" aria-hidden className="guide-bird">
      {art && <g transform="translate(940 0) scale(-1 1)" dangerouslySetInnerHTML={{ __html: art.bird }} />}
    </svg>
  )
}

const HINTS: Record<'project' | 'home', string[]> = {
  project: ['この Card は何をしている？', 'いま何が起きている？', 'グラフを表示するには？', 'デモを見せて'],
  home: ['Tsugumi で何ができる？', 'デモを見せて', '新しいプロジェクトの作り方は？'],
}

function GuidePanel({ screen, session, tab, perform }: { screen: GuideScreen; session?: ProjectSession; tab?: WorkflowTab; perform: (a: GuideAction) => void }) {
  const [text, setText] = useState('')
  const end = useRef<HTMLDivElement>(null)
  const field = useRef<HTMLTextAreaElement>(null)
  const last = guide.items[guide.items.length - 1]
  useEffect(() => {
    end.current?.scrollIntoView({ block: 'end' })
  }, [guide.items.length, last?.text, guide.status])
  useEffect(() => {
    if (!guide.offersDemo) field.current?.focus()
  }, [])
  const send = (q: string) => {
    if (guide.busy || !q.trim()) return
    if (guide.offersDemo) guide.answerDemoOffer()
    guide.ask(q, screen, session, tab)
    setText('')
  }
  const enabled = (a: GuideAction) => {
    switch (a.kind) {
      case 'run': return !!session && session.connected && !session.runActive
      case 'stop': return !!session?.runActive
      case 'open_card': return !!session
      case 'open_demo': case 'home': case 'new_project': return !session?.runActive
      default: return true
    }
  }
  return (
    <div className="guide-panel material" role="dialog" aria-label="ガイドのツグミとの会話"
      onKeyDown={(e) => {
        if (e.key === 'Escape') {
          e.stopPropagation()
          guide.set('open', false)
        }
      }}>
      <div className="guide-head">
        <div className="col">
          <span className="headline">ツグミ</span>
          <span className="caption secondary ellipsis" aria-live="polite">{guide.activity ? guide.activity + '…' : '使い方の質問に答えます'}</span>
        </div>
        <span className="spacer" />
        <Button kind="borderless" small disabled={guide.busy} help="新しい会話" onClick={() => void guide.newConversation()}>新しい会話</Button>
        <Button kind="borderless" small iconOnly icon="xmark.circle.fill" help="閉じる（Esc）。ツグミは右下に残ります" onClick={() => guide.set('open', false)} />
      </div>
      {guide.tour && <TourBar tour={guide.tour} session={session} />}
      <div className="guide-scroll" aria-live="polite">
        {guide.offersDemo && <Offer onLater={() => { guide.answerDemoOffer(); field.current?.focus() }} perform={perform} />}
        {!guide.items.length && !guide.offersDemo && (
          <div className="col gap4">
            <span className="callout secondary">いま見ている画面について、気軽に聞いてください。</span>
            {HINTS[screen === 'project' ? 'project' : 'home'].map((q) => (
              <button key={q} type="button" className="link-button" onClick={() => send(q)}>{q}</button>
            ))}
          </div>
        )}
        {guide.items.map((it) => <Row key={it.id} item={it} perform={perform} enabled={enabled} />)}
        {guide.status && (
          <div className="col gap4">
            <span className={`caption selectable${guide.failed ? ' guide-error' : ' secondary'}`}><Icon name={guide.failed ? 'exclamationmark.triangle' : 'info.circle'} /> {guide.status}</span>
            {guide.failed && (
              <div className="row gap8">
                <Button small disabled={guide.busy} onClick={() => guide.retry()}>再試行</Button>
                <Button small onClick={() => perform({ kind: 'open_settings', label: '' })}>API の設定を開く</Button>
              </div>
            )}
          </div>
        )}
        <div ref={end} />
      </div>
      <form className="guide-input" onSubmit={(e) => { e.preventDefault(); send(text) }}>
        <textarea ref={field} rows={1} value={text} placeholder="ツグミに質問" aria-label="ツグミへの質問"
          onChange={(e) => setText(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter' && !e.shiftKey && !e.nativeEvent.isComposing) {
              e.preventDefault()
              send(text)
            }
          }} />
        {guide.busy
          ? <Button kind="borderless" iconOnly icon="stop.circle.fill" help="回答を止める" onClick={() => guide.cancel()} />
          : <Button kind="borderless" iconOnly icon="arrow.up.circle.fill" help="送信" type="submit" disabled={!text.trim()} />}
      </form>
    </div>
  )
}

function TourBar({ tour, session }: { tour: GuideTour; session?: ProjectSession }) {
  const here = !!session && guide.same(tour.path, session)
  return (
    <div className="guide-tour">
      <Icon name="sparkles" />
      <span className="caption ellipsis">{here ? `デモ「${tour.title}」を案内中` : `デモ「${tour.title}」の案内は中断中`}</span>
      <span className="spacer" />
      {!here && (
        <Button small disabled={!!session?.runActive} onClick={() => {
          const p = guide.resumeTour()
          if (p) windowActions?.openProject(p)
        }}>続ける</Button>
      )}
      <Button small onClick={() => guide.endTour()}>終える</Button>
    </div>
  )
}

function Offer({ onLater, perform }: { onLater: () => void; perform: (a: GuideAction) => void }) {
  return (
    <div className="guide-offer callout">
      <div>こんにちは、ツグミです。Tsugumi の使い方を、ここからいつでもご案内します。</div>
      <div className="secondary">まず、動くデモを一緒に見てみますか？ デモは編集できるコピーとして開き、実際に実行しながら Card・接続・結果の見方をお見せします。</div>
      <div className="row gap8">
        <Button kind="prominent" onClick={() => perform({ kind: 'open_demo', label: '', demo: 'report' })}>見る</Button>
        <Button onClick={onLater}>あとで</Button>
      </div>
    </div>
  )
}

function Row({ item, perform, enabled }: { item: GuideItem; perform: (a: GuideAction) => void; enabled: (a: GuideAction) => boolean }) {
  if (item.role === 'user') return <div className="guide-user callout selectable">{item.text}</div>
  if (item.role === 'note') return <div className="guide-note caption secondary"><Icon name="info.circle" /> {item.text}</div>
  return (
    <div className="guide-reply">
      {item.text && <Markdown text={item.text} className="callout" />}
      {!!item.choices?.length && <Choices choices={item.choices} actions={[]} perform={perform} onChoice={(c) => guide.choose(c)} />}
      {!!item.actions?.length && (
        <div className="row gap8 wrap">
          {item.actions.map((a, i) => (
            <Button key={i} small kind={i === 0 ? 'prominent' : 'bordered'} disabled={!enabled(a)} onClick={() => perform(a)}>{a.label}</Button>
          ))}
        </div>
      )}
    </div>
  )
}

// ---- First start ------------------------------------------------------------------------

/** The first start: choose Anthropic or OpenAI and enter that provider's API
 *  key (one is enough), checked by connecting, or start without a guide. The
 *  key goes where Settings > API puts it (the host's Keychain). */
export function GuideSetupSheet() {
  useObserve(guide)
  const [provider, setProvider] = useState<'anthropic' | 'openai'>('anthropic')
  const [key, setKey] = useState('')
  const [working, setWorking] = useState(false)
  const [message, setMessage] = useState<string>()
  const [saved, setSaved] = useState(false)
  if (!guide.needsSetup) return null
  const check = async () => {
    setWorking(true)
    setMessage(undefined)
    try {
      await api('/api/guide/check', {})
      await guide.finishSetup()
    } catch (e) {
      setMessage(`接続を確かめられませんでした（${errorText(e)}）。キーを入れ直すか、ネットワークを確かめて再試行してください。`)
    }
    setWorking(false)
  }
  const connect = async () => {
    const k = key.trim()
    if (!k || working) return
    try {
      await api('/api/settings', { provider })
      await api('/api/settings/key', { provider, key: k })
    } catch (e) {
      setMessage(`キーを保存できませんでした: ${errorText(e)}`)
      return
    }
    setKey('')
    setSaved(true)
    await check()
  }
  return (
    <Sheet onClose={() => void guide.finishSetup()} width={500}>
      <form className="sheet-body guide-setup" onSubmit={(e) => { e.preventDefault(); void connect() }}>
        <div className="row gap12 center">
          <span className="guide-avatar still"><GuideBird /></span>
          <div className="col">
            <div className="title2">ガイドのツグミを呼ぶ準備</div>
            <div className="secondary">右下のツグミが、使い方の質問に答えたりデモを案内したりします。</div>
          </div>
        </div>
        <div className="callout">ツグミとチャットの Agent は、あなたの API キーで OpenAI か Anthropic のモデルを使います。どちらか一方のキーがあれば十分です。キーはこのホストのキーチェーン（macOS 以外では 0600 のファイル）にだけ保存し、プロジェクト・会話の記録・ログには残しません。</div>
        <div className="segmented" role="radiogroup" aria-label="使うモデル">
          {(['anthropic', 'openai'] as const).map((p) => (
            <button key={p} type="button" role="radio" aria-checked={provider === p} className={provider === p ? 'on' : ''}
              onClick={() => { setProvider(p); setMessage(undefined); setSaved(false) }}>{p === 'openai' ? 'OpenAI' : 'Anthropic'}</button>
          ))}
        </div>
        <input type="password" className="field" value={key} autoFocus autoComplete="off" spellCheck={false}
          placeholder={provider === 'openai' ? 'OpenAI の API キー' : 'Anthropic の API キー'} aria-label="API キー"
          onChange={(e) => setKey(e.target.value)} />
        {message && <div className="caption guide-error selectable" role="alert"><Icon name="exclamationmark.triangle" /> {message}</div>}
        <div className="caption secondary">キーがなくても、Tsugumi はすべて UI から使えます（その間ツグミは現れません）。あとから設定の「API」でキーを登録すると現れます。</div>
        <div className="row gap8">
          <Button onClick={() => void guide.finishSetup()}>スキップして始める</Button>
          <span className="spacer" />
          {saved && message && <Button disabled={working} onClick={() => void check()}>再試行</Button>}
          <Button kind="prominent" type="submit" disabled={working || !key.trim()}>{working ? '確かめています…' : '接続して始める'}</Button>
        </div>
      </form>
    </Sheet>
  )
}
