// Initial setup (the core binaries), Home (new, existing and recent
// projects) and Demos (opened as editable copies), and the Tsugumi art the
// home and the loading card share: the bird on its branch on low-poly teal.
import { useEffect, useRef, useState } from 'react'
import { api, Demo } from '../api'
import { app } from '../model/app'
import { useObserve } from '../model/observable'
import { prefs } from '../model/prefs'
import { errorText } from '../model/session'
import { Button, Empty, showMenu, Sheet, Spinner, TextField, Toggle } from './controls'
import { DemoThumbnailPlayer } from './demoPlayer'
import { Icon } from './icons'
import { guide } from '../model/guide'
import { GuideCorner } from './guide'
import { FolderPicker } from './sidebar'
import { birdFeet, drawIdleScene, drawTemariScene, drawProgressScene, drawSpool, drawWorkflowBackdrop, HopWalker, PALETTE, SeededRandom, TsugumiBird } from './tsugumiArt'
import { L } from '../i18n'

// ---- Art ---------------------------------------------------------------------------

export { PALETTE }

const LOW_POLY = (() => {
  // The app's generator and seed, so the facets fall where they do there.
  const random = new SeededRandom(7)
  const rng = (lo: number, hi: number) => random.next(lo, hi)
  const cols = 7, rows = 5, w = 700, h = 500
  const pts: [number, number][][] = []
  for (let r = 0; r <= rows; r++) {
    pts.push(Array.from({ length: cols + 1 }, (_, c) => {
      const jx = c === 0 || c === cols ? 0 : rng(-0.35, 0.35)
      const jy = r === 0 || r === rows ? 0 : rng(-0.35, 0.35)
      return [((c + jx) * w) / cols, ((r + jy) * h) / rows] as [number, number]
    }))
  }
  const tris: { d: string; fill: string }[] = []
  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) {
      const a = pts[r][c], b = pts[r][c + 1], cc = pts[r + 1][c], d = pts[r + 1][c + 1]
      for (const t of [[a, b, d], [a, d, cc]]) {
        const shade = rng(-0.05, 0.07)
        const d = `M${t.map((p) => p.join(',')).join('L')}Z`
        if (shade > 0) tris.push({ d, fill: `rgba(255,255,255,${shade.toFixed(3)})` })
        tris.push({ d, fill: `rgba(0,0,0,${(Math.max(0, -shade) + r * 0.02).toFixed(3)})` })
      }
    }
  }
  return tris
})()

/** The key visual's background: teal with faint facets. */
export function LowPolyBackground({ className }: { className?: string }) {
  return (
    <svg className={`low-poly ${className ?? ''}`} viewBox="0 0 700 500" preserveAspectRatio="none" aria-hidden>
      <defs>
        <linearGradient id="tsugumi-teal" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor={PALETTE.tealTop} />
          <stop offset="1" stopColor={PALETTE.tealBottom} />
        </linearGradient>
      </defs>
      <rect width="700" height="500" fill="url(#tsugumi-teal)" />
      {LOW_POLY.map((t, i) => <path key={i} d={t.d} fill={t.fill} />)}
    </svg>
  )
}

/** The bird of the app icon, split into the bird and the branch it sits on. */
let birdArt: Promise<{ bird: string; perch: string }> | undefined
function loadBird() {
  birdArt ??= fetch('/icon.svg').then((r) => r.text()).then((text) => {
    const doc = new DOMParser().parseFromString(text, 'image/svg+xml')
    const ser = new XMLSerializer()
    const perch: string[] = [], bird: string[] = []
    // The icon's perch group also holds the wing: polygons low enough are the branch.
    for (const p of [...(doc.querySelector('#perch')?.children ?? [])]) {
      const ys = (p.getAttribute('points') ?? '').trim().split(/\s+/).map((xy) => Number(xy.split(',')[1]))
      ;(ys.length && Math.min(...ys) >= 690 ? perch : bird).push(ser.serializeToString(p))
    }
    const b = doc.querySelector('#tsugumi-bird')
    if (b) bird.push(ser.serializeToString(b))
    return { bird: bird.join(''), perch: perch.join('') }
  }).catch(() => ({ bird: '', perch: '' }))
  return birdArt
}

export function useBird() {
  const [art, setArt] = useState<{ bird: string; perch: string }>()
  useEffect(() => {
    let alive = true
    void loadBird().then((a) => alive && setArt(a))
    return () => {
      alive = false
    }
  }, [])
  return art
}

/** Home's bird with the key visual's temari (TsugumiTemariScene.swift), held
 *  still with the ball close when the settings or the system ask for less motion. */
function TsugumiTemariScene() {
  const scene = useArtCanvas((g, w, h, t) => drawTemariScene(g, w, h, t, 0.19), 0)
  return <canvas ref={scene.ref} className="fill-abs" aria-hidden />
}

export interface LoadingContent {
  title: string
  message?: string
  fraction?: number
  detail?: string
  dirty?: boolean
}

/** A canvas drawn every frame (or now and then, when the settings or the
 *  system ask for less motion: then at the fixed time `stillTime`). */
function useArtCanvas(draw: (g: CanvasRenderingContext2D, w: number, h: number, t: number) => void, stillTime: number) {
  useObserve(prefs)
  const still = prefs.tsugumiStill
  const ref = useRef<HTMLCanvasElement>(null)
  const drawRef = useRef(draw)
  drawRef.current = draw
  useEffect(() => {
    const c = ref.current
    if (!c) return
    const paint = () => {
      const dpr = window.devicePixelRatio || 1, w = c.clientWidth, h = c.clientHeight
      if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
        c.width = Math.round(w * dpr)
        c.height = Math.round(h * dpr)
      }
      const g = c.getContext('2d')
      if (!g) return
      g.setTransform(dpr, 0, 0, dpr, 0, 0)
      g.clearRect(0, 0, w, h)
      drawRef.current(g, w, h, still ? stillTime : performance.now() / 1000)
    }
    if (still) {
      paint()
      const t = setInterval(paint, 250)
      return () => clearInterval(t)
    }
    let frame = 0
    const loop = () => {
      paint()
      frame = requestAnimationFrame(loop)
    }
    loop()
    return () => cancelAnimationFrame(frame)
  }, [still, stillTime])
  return { ref, still }
}

/** The temari's column left of the words, and its centre: its thread runs
 *  straight up and hooks over the panel's top edge there, into the picture. */
const SPOOL_WIDTH = 56
const SPOOL_X = 22 + SPOOL_WIDTH / 2
const CATCH_X = SPOOL_X

/** The loading card (TsugumiLoadingCard.swift): a Card of the canvas with its
 *  ports and wires. The bird scene is its picture: the bird holds a thread
 *  unwound from the temari beside the words below; with a fraction the branch
 *  grows as the bar and the temari unwinds, without it the bird hops and pecks. */
export function TsugumiLoadingCard({ content, onCancel }: { content: LoadingContent; onCancel?: () => void }) {
  const f = content.fraction === undefined ? undefined : Math.min(1, Math.max(0, content.fraction))
  const [walker] = useState(() => new HopWalker())
  // While the guide's bird flies in, the card has no bird of its own.
  const hides = () => document.body.classList.contains('guide-handoff')
  const backdrop = useArtCanvas((g, w, h, t) => drawWorkflowBackdrop(g, w, h, t, f), 0.3)
  const scene = useArtCanvas((g, w, h, t) => f === undefined
    ? drawIdleScene(g, w, h, t, CATCH_X, hides())
    : drawProgressScene(g, w, h, t, f, CATCH_X, walker, !scene.still, hides()), f === undefined ? 2.8 : 0.6)
  const spool = useArtCanvas((g, w, h, t) => drawSpool(g, w, h, t, f, SPOOL_X, CATCH_X), 2.8)
  // Where the bird stands, for the guide's bird to land on.
  const [size, setSize] = useState<{ w: number; h: number }>()
  const picture = useRef<HTMLDivElement>(null)
  useEffect(() => {
    const el = picture.current
    if (!el) return
    const ro = new ResizeObserver(() => setSize({ w: el.clientWidth, h: el.clientHeight }))
    ro.observe(el)
    return () => ro.disconnect()
  }, [])
  const bird = size && birdFeet(size.w, size.h, f)
  return (
    <div className="loading-card-stack" role="status" aria-label={content.title} aria-valuetext={f !== undefined ? `${Math.floor(f * 100)}%` : content.message}>
      <div className="loading-wire" />
      <div className="loading-port input" />
      <div className="loading-card">
        <div className="loading-picture" ref={picture} data-guide-loading="">
          <LowPolyBackground />
          <canvas ref={backdrop.ref} className="loading-backdrop" aria-hidden />
          <div className="loading-shade" />
          <canvas ref={scene.ref} aria-hidden />
          {bird && (
            <div className="tsugumi-bird" aria-hidden style={{
              left: bird.feet.x + (TsugumiBird.tailTip - TsugumiBird.feet.x) * bird.scale,
              top: bird.feet.y + (TsugumiBird.top - TsugumiBird.feet.y) * bird.scale,
              width: (TsugumiBird.beakTip - TsugumiBird.tailTip) * bird.scale,
              height: (TsugumiBird.feet.y + 2 - TsugumiBird.top) * bird.scale,
            }} />
          )}
        </div>
        <div className="loading-text">
          {f !== undefined && <div className="loading-percent mono-digits">{Math.floor(f * 100)}%</div>}
          <div className="title3">{content.title}</div>
          {content.message && <div className="callout secondary clamp3">{content.message}</div>}
          {content.detail && <div className="caption mono tertiary ellipsis">{content.detail}</div>}
          {content.dirty && <span className="label callout" style={{ color: PALETTE.rufous }}><Icon name="pencil.circle" />{L('未保存の変更があります（保持しています）')}</span>}
          {onCancel && <Button onClick={onCancel}>{L('キャンセル')}</Button>}
          {/* the temari beside the words; its thread catches on the picture's edge */}
          <canvas ref={spool.ref} className="loading-spool" aria-hidden />
        </div>
      </div>
      <div className="loading-port output" />
      <div className="loading-wire" />
    </div>
  )
}

// ---- Setup -------------------------------------------------------------------------

/** Shown when the core binaries still need building on this host. */
export function SetupView() {
  useObserve(app)
  const s = app.status!.setup
  const [message, setMessage] = useState<string>()
  const step = (title: string, ok: boolean, detail: string, action?: JSX.Element) => (
    <div className="setup-step">
      <Icon name={ok ? 'checkmark.circle.fill' : 'circle.dotted'} size={20} style={{ color: ok ? 'var(--green)' : 'var(--secondary)' }} title={ok ? L('完了') : L('未完了')} />
      <div className="flex1">
        <b>{title}</b>
        <div className="callout secondary selectable">{detail}</div>
      </div>
      {!ok && action}
    </div>
  )
  return (
    <div className="setup">
      <h1><Icon name="shippingbox" size={28} /> Tsugumi の実行環境を準備します</h1>
      <div className="secondary">{L('Web UI では core（tsugumid）をこのホストで直接動かします（VM は使いません）。次の項目を確認してください。')}</div>
      <div className="groupbox">
        {step(L('Nix がインストールされている'), s.nix.ok, s.nix.ok ? L('/nix/store があります。Card の環境は Nix で作ります。') : L('https://nixos.org/download からインストールしてください（flakes を有効に）。'))}
        {step(L('core（tsugumid と tsugumi-worker）がビルドされている'), s.tsugumid.ok && s.worker.ok, `${s.tsugumid.path}\n${s.worker.path}`,
          <Button disabled={!s.nix.ok || !!s.building?.running} onClick={async () => {
            setMessage(undefined)
            try {
              await api('/api/setup/build', {})
              await app.refresh()
            } catch (e) {
              setMessage(errorText(e))
            }
          }}>{L('ビルド')}</Button>)}
        {step(L('Card SDK が見つかる'), s.sdk.ok, s.sdk.path)}
      </div>
      {s.building && (
        <div className="col gap6">
          <div className="row gap8 center">
            {s.building.running ? <><Spinner small />{L('ビルドしています（初回は数分かかります）…')}</> : s.building.code === 0 ? 'ビルドが完了しました。' : <span style={{ color: 'var(--red)' }}>ビルドに失敗しました（{s.building.code}）。</span>}
          </div>
          <pre className="env-log selectable">{s.building.log}</pre>
        </div>
      )}
      <div className="caption secondary mono selectable">手動でビルドする場合: cd {s.repo} &amp;&amp; nix develop --command bash -c 'cmake -S . -B build -G Ninja &amp;&amp; ninja -C build'</div>
      <div className="row gap8 center">
        {message && <span className="label" style={{ color: 'var(--red)' }}><Icon name="exclamationmark.triangle.fill" />{message}</span>}
        <span className="spacer" />
        <Button onClick={() => void app.refresh()}>{L('再確認')}</Button>
      </div>
    </div>
  )
}

// ---- Home --------------------------------------------------------------------------

/** "たった今", "5 分前", "3 時間前", "2 日前": no seconds that tick. */
export function relativeAge(ms: number, now = Date.now()): string {
  const d = Math.max(0, (now - ms) / 1000)
  if (d < 60) return L('たった今')
  if (d < 3600) return L('{0} 分前', Math.floor(d / 60))
  if (d < 86400) return L('{0} 時間前', Math.floor(d / 3600))
  return L('{0} 日前', Math.floor(d / 86400))
}

/** Home (new, existing and recent projects) and the demo list, entered from
 *  "デモを見る" with a way back. Lists scroll; nothing is cut at the top. */
export function ProjectChooser({ onOpen }: { onOpen: (path: string) => void }) {
  useObserve(app)
  const [showDemos, setShowDemos] = useState(false)
  const [showNew, setShowNew] = useState(false)
  const [picking, setPicking] = useState(false)
  useEffect(() => {
    document.title = 'Tsugumi'
  }, [])
  useObserve(guide)
  useEffect(() => {
    if (guide.demosRequested) {
      setShowDemos(true)
      guide.set('demosRequested', false)
    }
  }, [guide.demosRequested])
  useEffect(() => {
    if (app.requestNewProject) {
      setShowNew(true)
      app.request('requestNewProject', false)
    }
  }, [app.requestNewProject])
  return (
    <div className="home-window">
      <HomeSidebar showDemos={showDemos} onToggleDemos={() => setShowDemos(!showDemos)} onNew={() => setShowNew(true)} onOpen={() => setPicking(true)} />
      <div className="home-main">
        {showDemos ? <Demos onOpen={onOpen} /> : <Recents onOpen={onOpen} />}
        {app.error && (
          <div className="error-bar">
            <span className="label selectable" style={{ color: 'var(--red)' }}><Icon name="exclamationmark.triangle.fill" />{app.error}</span>
            <span className="spacer" />
            <Button onClick={() => app.setError(undefined)}>{L('閉じる')}</Button>
          </div>
        )}
      </div>
      <GuideCorner screen={showDemos ? 'demos' : 'home'} />
      {showNew && <NewProjectSheet onClose={() => setShowNew(false)} onCreated={onOpen} />}
      {picking && <FolderPicker message={L('tsugumi.json を含むフォルダを選んでください')} requireProject onClose={() => setPicking(false)} onPick={(p) => { setPicking(false); onOpen(p) }} />}
    </div>
  )
}

/** The left column of home: the bird on its branch in the key visual's
 *  colours, the name, and the ways in. */
function HomeSidebar({ showDemos, onToggleDemos, onNew, onOpen }: { showDemos: boolean; onToggleDemos: () => void; onNew: () => void; onOpen: () => void }) {
  return (
    <div className="home-sidebar">
      <LowPolyBackground className="fill-abs" />
      <div className="home-sidebar-content">
        <div className="home-scene"><TsugumiTemariScene /></div>
        <div className="home-title">Tsugumi</div>
        <div className="col gap8 home-buttons">
          <Button kind="prominent" large icon="plus.square" guide="new_project" onClick={onNew}>{L('新規プロジェクト…')}</Button>
          <Button large icon="folder" guide="open_project" onClick={onOpen}>{L('既存のプロジェクトを開く…')}</Button>
          <button type="button" className="home-link" data-guide="demos" onClick={onToggleDemos}>
            <Icon name={showDemos ? 'clock' : 'sparkles'} />{showDemos ? L('最近のプロジェクト') : L('デモを見る')}
          </button>
        </div>
        <span className="spacer" />
        <Button kind="borderless" small icon="gearshape" className="home-settings" guide="settings" help={L('アプリ設定（⌘,）')} onClick={() => app.openWindow(undefined, { kind: 'appSettings' })}>{L('設定')}</Button>
      </div>
    </div>
  )
}

function Recents({ onOpen }: { onOpen: (path: string) => void }) {
  useObserve(app)
  const recents = app.status?.recents ?? []
  /** Choosing recent projects to take off the list (undefined: not choosing). */
  const [chosen, setChosen] = useState<Set<string>>()
  const [now, setNow] = useState(Date.now())
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 60_000)
    return () => clearInterval(t)
  }, [])
  const toggle = (p: string) => {
    if (!chosen) return
    const n = new Set(chosen)
    if (n.has(p)) n.delete(p)
    else n.add(p)
    setChosen(n)
  }
  return (
    <div className="home-pane">
      <div className="row center gap8 home-pane-head">
        <span className="headline">{L('最近のプロジェクト')}</span>
        <span className="spacer" />
        {chosen ? (
          <>
            <Toggle kind="checkbox" checked={recents.length > 0 && chosen.size === recents.length}
              onChange={(v) => setChosen(v ? new Set(recents.map((r) => r.path)) : new Set())} label={L('すべて選択')} />
            <Button disabled={!chosen.size} help={L('一覧から外すだけで、フォルダは削除しません')} onClick={() => {
              void app.forgetRecents(chosen)
              setChosen(undefined)
            }}>{L('一覧から削除')}</Button>
            <Button onClick={() => setChosen(undefined)}>{L('完了')}</Button>
          </>
        ) : recents.length > 0 && <Button help={L('一覧から外すプロジェクトを選ぶ')} onClick={() => setChosen(new Set())}>{L('編集')}</Button>}
      </div>
      {recents.length === 0 ? (
        <Empty title={L('最近開いたプロジェクトはありません')} icon="clock" description={L('新規作成するか、既存のフォルダを開いてください。')} />
      ) : (
        <div className="scroll">
          <div className="recents-list">
            {recents.map((p) => (
              <button key={p.path} type="button" className="recent-row" onClick={() => (chosen ? toggle(p.path) : onOpen(p.path))}
                onContextMenu={(e) => showMenu(e, [
                  { label: L('Finder で表示'), action: () => void api('/api/reveal', { path: p.path }).catch((er) => app.setError(er)) },
                  { label: L('一覧から削除'), action: () => void app.forgetRecents([p.path]) },
                ])}>
                {chosen && (
                  <Icon name={chosen.has(p.path) ? 'checkmark.square.fill' : 'square'} title={chosen.has(p.path) ? L('選択済み') : L('未選択')}
                    style={{ color: chosen.has(p.path) ? 'var(--accent)' : 'var(--secondary)' }} />
                )}
                <Icon name="square.stack.3d.up" className="secondary" />
                <div className="flex1 min0">
                  <div className="headline ellipsis">{p.name}</div>
                  <div className="caption secondary ellipsis-middle" title={p.path}>{p.path}</div>
                </div>
                <span className="caption secondary nowrap">{relativeAge(p.openedAt, now)}</span>
              </button>
            ))}
          </div>
        </div>
      )}
    </div>
  )
}

function Demos({ onOpen }: { onOpen: (path: string) => void }) {
  useObserve(app, guide)
  const [tag, setTag] = useState<string>()
  const [search, setSearch] = useState('')
  // Tsugumi points at a demo: it is shown, whatever the filter.
  useEffect(() => {
    if (!guide.highlight?.startsWith('demo:')) return
    setTag(undefined)
    setSearch('')
  }, [guide.highlight])
  const demos = app.status?.demos ?? []
  const q = search.toLowerCase()
  const shown = demos.filter((d) => (tag === undefined || d.tags.includes(tag)) &&
    (!q || d.title.toLowerCase().includes(q) || d.description.toLowerCase().includes(q) || d.tags.some((t) => t.toLowerCase().includes(q))))
  const tags = [...new Set(demos.flatMap((d) => d.tags))].sort()
  return (
    <div className="home-pane">
      <div className="row center gap8 home-pane-head">
        <span className="title2">{L('デモ')}</span>
        <span className="spacer" />
        <TextField value={search} onChange={setSearch} placeholder={L('探す')} style={{ maxWidth: 220 }} />
      </div>
      <div className="callout secondary">{L('デモは Documents/Tsugumi に編集可能なコピーとして開きます。同梱の元データは変更されません。')}</div>
      {/* Tags to narrow the list as demos grow. */}
      <div className="tag-row">
        <TagChip label={L('すべて')} selected={tag === undefined} onClick={() => setTag(undefined)} />
        {tags.map((t) => <TagChip key={t} label={t} selected={tag === t} onClick={() => setTag(tag === t ? undefined : t)} />)}
      </div>
      <div className="scroll flex1">
        {shown.length === 0 && <Empty title={demos.length === 0 ? L('デモが見つかりません') : L('条件に合うデモはありません')} icon="questionmark.folder" />}
        <div className="demo-grid">
          {shown.map((d) => (
            <DemoCard key={d.id} demo={d} onOpen={async () => {
              try {
                onOpen(await app.copyDemo(d.id))
              } catch (e) {
                app.setError(e)
              }
            }} />
          ))}
        </div>
      </div>
    </div>
  )
}

function TagChip({ label, selected, onClick }: { label: string; selected: boolean; onClick: () => void }) {
  return <button type="button" className={`tag-chip${selected ? ' selected' : ''}`} aria-pressed={selected} onClick={onClick}>{label}</button>
}

/** A demo with a thumbnail of its own Workflow: the demo player in its compact form. */
function DemoCard({ demo, onOpen }: { demo: Demo; onOpen: () => void }) {
  return (
    <div className="demo-card" data-guide={`demo:${demo.id}`}>
      <DemoThumbnailPlayer className="demo-thumb" data={`/player-data/${demo.id}.json`} />
      <div className="headline">{demo.title}</div>
      <div className="callout secondary clamp3">{demo.description}</div>
      <div className="row gap4 wrap">{demo.tags.map((t) => <span key={t} className="demo-tag caption2">{t}</span>)}</div>
      <div className="row end"><Button kind="prominent" onClick={onOpen}>{L('コピーを開く')}</Button></div>
    </div>
  )
}

function NewProjectSheet({ onClose, onCreated }: { onClose: () => void; onCreated: (path: string) => void }) {
  const [name, setName] = useState('')
  const [choosing, setChoosing] = useState(false)
  return (
    <Sheet onClose={onClose} width={380}>
      <form className="sheet-body" onSubmit={(e) => { e.preventDefault(); if (name) setChoosing(true) }}>
        <div className="title2">{L('新規プロジェクト')}</div>
        <TextField value={name} onChange={setName} placeholder={L('名前')} autoFocus />
        <div className="row end gap8">
          <Button onClick={onClose}>{L('キャンセル')}</Button>
          <Button kind="prominent" type="submit" disabled={!name}>{L('作成場所を選ぶ…')}</Button>
        </div>
      </form>
      {choosing && <FolderPicker message={L('プロジェクトのフォルダを作成する場所を選んでください')} allowCreate initial={app.status?.home} onClose={() => setChoosing(false)}
        onPick={async (parent) => {
          setChoosing(false)
          try {
            const p = await app.createProject(parent, name, prefs.flowDirection)
            onClose()
            onCreated(p)
          } catch (e) {
            app.setError(e)
            onClose()
          }
        }} />}
    </Sheet>
  )
}
