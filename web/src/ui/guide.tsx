// The guide bird in the bottom-right corner (model/guide.ts): the bird, its
// panel with the greeting, the demo offer, the tour and the replies with
// their buttons, the rings it draws around what it talks about, and the
// first start's model key question. The macOS app's GuideViews does the same.
import { useEffect, useRef, useState } from 'react'
import { api } from '../api'
import { app } from '../model/app'
import { guide, GuideAction, GuideItem, GuideScreen, GuideTour } from '../model/guide'
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
  const targets = h === 'stop' ? ['run'] : [h]
  const sel = targets.map((t) => `[data-guide="${CSS.escape(t)}"]`).join(', ')
  return <style>{`${sel} { outline: 2.5px solid ${PALETTE.rufous}; outline-offset: 4px; border-radius: 8px; animation: guide-ring 1.6s ease-in-out infinite; }`}</style>
}

/** The bird in the corner and its panel. Shown only with a model connection.
 *  In a project the panel is `docked` in the right column (GuideDock), so it
 *  never covers the canvas; on Home it floats above the bird. */
export function GuideCorner({ screen, session, tab, docked }: { screen: GuideScreen; session?: ProjectSession; tab?: WorkflowTab; docked?: boolean }) {
  useObserve(guide, session, prefs)
  const runKey = session ? guide.runKey(session) : ''
  useEffect(() => {
    if (session) guide.observe(session, tab)
  }, [runKey, session, tab])
  useEffect(() => {
    if (guide.offersDemo && !guide.open) guide.set('open', true)
  }, [guide.offersDemo])
  if (!guide.available) return null
  const tourHere = !!(session && guide.tour && guide.same(guide.tour.path, session))
  const mood = guide.busy ? 'thinking' : guide.failed ? 'worried' : session?.runActive && tourHere ? 'watching' : 'idle'

  const perform = makePerform(session, tab)

  return (
    <div className={`guide-corner${screen === 'project' ? ' in-project' : ''}`}>
      <GuideRings />
      {guide.open && !docked && <GuidePanel screen={screen} session={session} tab={tab} perform={perform} />}
      <div className="guide-corner-row">
        {!guide.open && (guide.activity || guide.failed) && (
          <span className="guide-bubble material" aria-hidden>{guide.activity ? guide.activity + '…' : 'うまく答えられませんでした'}</span>
        )}
        <button type="button" className={`guide-avatar ${mood}${prefs.tsugumiStill ? ' still' : ''}`}
          title={`ガイドのツグミに聞く（${prefs.label('toggleGuide')}）`}
          aria-label="ガイドのツグミ" aria-expanded={guide.open}
          onClick={() => guide.set('open', !guide.open)}>
          <GuideBird />
        </button>
      </div>
    </div>
  )
}

/** What a button of the guide does, with the app's own controls. */
function makePerform(session?: ProjectSession, tab?: WorkflowTab) {
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

/** The guide's panel in a project's right column: above the Agent chat
 *  when it is open (`split`), the whole column otherwise. */
export function GuideDock({ session, tab, split }: { session: ProjectSession; tab?: WorkflowTab; split: boolean }) {
  useObserve(guide, session)
  return <GuidePanel screen="project" session={session} tab={tab} perform={makePerform(session, tab)} docked split={split} />
}

function GuidePanel({ screen, session, tab, perform, docked, split }: { screen: GuideScreen; session?: ProjectSession; tab?: WorkflowTab; perform: (a: GuideAction) => void; docked?: boolean; split?: boolean }) {
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
    <div className={`guide-panel ${docked ? `docked${split ? ' split' : ''}` : 'material'}`} role="dialog" aria-label="ガイドのツグミとの会話"
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
