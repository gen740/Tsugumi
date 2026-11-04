// Initial setup (the core binaries), Home (new, existing and recent
// projects) and Demos (opened as editable copies), and the Tsugumi art the
// home and the loading card share: the bird on its branch on low-poly teal.
import { useEffect, useState } from 'react'
import { api, Demo, DemoThumbnail } from '../api'
import { app } from '../model/app'
import { useObserve } from '../model/observable'
import { prefs } from '../model/prefs'
import { errorText } from '../model/session'
import { Button, Empty, showMenu, Sheet, Spinner, TextField, Toggle } from './controls'
import { Icon } from './icons'
import { guide } from '../model/guide'
import { GuideCorner } from './guide'
import { FolderPicker } from './sidebar'

// ---- Art ---------------------------------------------------------------------------

export const PALETTE = {
  tealTop: '#2d5a5c', tealBottom: '#132b33', cream: '#f6ead2', mist: '#a9c4bf', rufous: '#c97d3d',
  olive: '#a9b77a', oliveDark: '#7d9148', bark: '#7d5834', barkDark: '#5c3e24', barkShade: '#4e341e', thread: '#efe0c3',
}

/** A small seeded generator, so the facets stay the same between renders. */
function seeded(seed: number) {
  let a = seed >>> 0
  return (lo: number, hi: number) => {
    a = (a + 0x6d2b79f5) >>> 0
    let t = a
    t = Math.imul(t ^ (t >>> 15), t | 1)
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
    return lo + (hi - lo) * (((t ^ (t >>> 14)) >>> 0) / 4294967296)
  }
}

const LOW_POLY = (() => {
  const rng = seeded(7)
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

/** The bird on a long low-poly branch, hopping now and then (still when the
 *  settings or the system ask for less motion). */
export function TsugumiIdleScene({ birdScale = 0.22, className }: { birdScale?: number; className?: string }) {
  useObserve(prefs)
  const art = useBird()
  const still = prefs.tsugumiStill
  // The scene is drawn in the icon's coordinates: the feet at (505, 706).
  const w = 150 / birdScale * 1.6, h = 150 / birdScale
  const x0 = 505 - w / 2 - 40, y0 = 706 + 22 / birdScale - h
  return (
    <svg className={`tsugumi-scene ${className ?? ''}`} viewBox={`${x0} ${y0} ${w} ${h}`} preserveAspectRatio="xMidYMax meet" aria-hidden>
      <g className="tsugumi-branch">
        <polygon points={`${505 - 520},716 ${505 - 380},700 ${505 - 380},726`} fill={PALETTE.barkDark} />
        <polygon points={`${505 - 380},700 ${505 + 300},694 ${505 - 380},726`} fill={PALETTE.bark} />
        <polygon points={`${505 - 380},726 ${505 + 300},694 ${505 + 300},722`} fill={PALETTE.barkShade} />
        <polygon points={`${505 + 300},694 ${505 + 520},704 ${505 + 300},722`} fill={PALETTE.barkDark} />
        {art && <g dangerouslySetInnerHTML={{ __html: art.perch }} />}
      </g>
      <ellipse cx="505" cy="702" rx="150" ry="14" fill="rgba(0,0,0,0.2)" className={still ? '' : 'tsugumi-shadow'} />
      {art && <g className={`tsugumi-bird${still ? '' : ' hopping'}`} dangerouslySetInnerHTML={{ __html: art.bird }} />}
      {/* A loose thread from the bill. */}
      <path d="M770 404 C 800 470, 760 540, 800 620" fill="none" stroke={PALETTE.thread} strokeWidth={5} strokeLinecap="round" opacity={0.8}
        className={still ? '' : 'tsugumi-thread'} />
    </svg>
  )
}

/** A ball of thread (temari) unwinding into the card. */
function Temari({ fraction }: { fraction?: number }) {
  useObserve(prefs)
  const r = 15 * (1 - 0.35 * (fraction ?? 0))
  return (
    <svg className="temari" width="56" height="92" viewBox="0 0 56 92" aria-hidden>
      <path d={`M28 ${46 + r} C 28 70, 28 80, 28 92`} stroke={PALETTE.thread} strokeWidth={1.5} fill="none" />
      <g className={prefs.tsugumiStill || fraction !== undefined ? '' : 'temari-spin'} style={{ transformOrigin: '28px 46px', transform: fraction !== undefined ? `rotate(${fraction * 900}deg)` : undefined }}>
        <circle cx="28" cy="46" r={r} fill={PALETTE.rufous} />
        <path d={`M${28 - r} 46 Q 28 ${46 - r * 0.8} ${28 + r} 46`} stroke={PALETTE.cream} strokeWidth={1.6} fill="none" />
        <path d={`M${28 - r} 46 Q 28 ${46 + r * 0.8} ${28 + r} 46`} stroke={PALETTE.cream} strokeWidth={1.6} fill="none" />
        <path d={`M28 ${46 - r} Q ${28 + r * 0.8} 46 28 ${46 + r}`} stroke={PALETTE.olive} strokeWidth={1.6} fill="none" />
      </g>
    </svg>
  )
}

export interface LoadingContent {
  title: string
  message?: string
  fraction?: number
  detail?: string
  dirty?: boolean
}

/** The loading card: a Card of the canvas with its ports and wires, the bird
 *  scene as its picture, and what is happening. */
export function TsugumiLoadingCard({ content, onCancel }: { content: LoadingContent; onCancel?: () => void }) {
  const f = content.fraction === undefined ? undefined : Math.min(1, Math.max(0, content.fraction))
  return (
    <div className="loading-card-stack" role="status" aria-label={content.title} aria-valuetext={f !== undefined ? `${Math.floor(f * 100)}%` : content.message}>
      <div className="loading-wire" />
      <div className="loading-port input" />
      <div className="loading-card">
        <div className="loading-picture">
          <LowPolyBackground />
          <div className="loading-shade" />
          <div className="fill-abs" data-guide-loading=""><TsugumiIdleScene birdScale={0.22} /></div>
          {f !== undefined && <div className="loading-progress" style={{ width: `${f * 100}%` }} />}
        </div>
        <div className="loading-text">
          <Temari fraction={f} />
          {f !== undefined && <div className="loading-percent mono-digits">{Math.floor(f * 100)}%</div>}
          <div className="title3">{content.title}</div>
          {content.message && <div className="callout secondary clamp3">{content.message}</div>}
          {content.detail && <div className="caption mono tertiary ellipsis">{content.detail}</div>}
          {content.dirty && <span className="label callout" style={{ color: PALETTE.rufous }}><Icon name="pencil.circle" />未保存の変更があります（保持しています）</span>}
          {onCancel && <Button onClick={onCancel}>キャンセル</Button>}
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
      <Icon name={ok ? 'checkmark.circle.fill' : 'circle.dotted'} size={20} style={{ color: ok ? 'var(--green)' : 'var(--secondary)' }} title={ok ? '完了' : '未完了'} />
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
      <div className="secondary">Web UI では core（tsugumid）をこのホストで直接動かします（VM は使いません）。次の項目を確認してください。</div>
      <div className="groupbox">
        {step('Nix がインストールされている', s.nix.ok, s.nix.ok ? '/nix/store があります。Card の環境は Nix で作ります。' : 'https://nixos.org/download からインストールしてください（flakes を有効に）。')}
        {step('core（tsugumid と tsugumi-worker）がビルドされている', s.tsugumid.ok && s.worker.ok, `${s.tsugumid.path}\n${s.worker.path}`,
          <Button disabled={!s.nix.ok || !!s.building?.running} onClick={async () => {
            setMessage(undefined)
            try {
              await api('/api/setup/build', {})
              await app.refresh()
            } catch (e) {
              setMessage(errorText(e))
            }
          }}>ビルド</Button>)}
        {step('Card SDK が見つかる', s.sdk.ok, s.sdk.path)}
      </div>
      {s.building && (
        <div className="col gap6">
          <div className="row gap8 center">
            {s.building.running ? <><Spinner small />ビルドしています（初回は数分かかります）…</> : s.building.code === 0 ? 'ビルドが完了しました。' : <span style={{ color: 'var(--red)' }}>ビルドに失敗しました（{s.building.code}）。</span>}
          </div>
          <pre className="env-log selectable">{s.building.log}</pre>
        </div>
      )}
      <div className="caption secondary mono selectable">手動でビルドする場合: cd {s.repo} &amp;&amp; nix develop --command bash -c 'cmake -S . -B build -G Ninja &amp;&amp; ninja -C build'</div>
      <div className="row gap8 center">
        {message && <span className="label" style={{ color: 'var(--red)' }}><Icon name="exclamationmark.triangle.fill" />{message}</span>}
        <span className="spacer" />
        <Button onClick={() => void app.refresh()}>再確認</Button>
      </div>
    </div>
  )
}

// ---- Home --------------------------------------------------------------------------

/** "たった今", "5 分前", "3 時間前", "2 日前": no seconds that tick. */
export function relativeAge(ms: number, now = Date.now()): string {
  const d = Math.max(0, (now - ms) / 1000)
  if (d < 60) return 'たった今'
  if (d < 3600) return `${Math.floor(d / 60)} 分前`
  if (d < 86400) return `${Math.floor(d / 3600)} 時間前`
  return `${Math.floor(d / 86400)} 日前`
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
            <Button onClick={() => app.setError(undefined)}>閉じる</Button>
          </div>
        )}
      </div>
      <GuideCorner screen={showDemos ? 'demos' : 'home'} />
      {showNew && <NewProjectSheet onClose={() => setShowNew(false)} onCreated={onOpen} />}
      {picking && <FolderPicker message="tsugumi.json を含むフォルダを選んでください" requireProject onClose={() => setPicking(false)} onPick={(p) => { setPicking(false); onOpen(p) }} />}
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
        <div className="home-scene"><TsugumiIdleScene birdScale={0.19} /></div>
        <div className="home-title">Tsugumi</div>
        <div className="col gap8 home-buttons">
          <Button kind="prominent" large icon="plus.square" guide="new_project" onClick={onNew}>新規プロジェクト…</Button>
          <Button large icon="folder" onClick={onOpen}>既存のプロジェクトを開く…</Button>
          <button type="button" className="home-link" data-guide="demos" onClick={onToggleDemos}>
            <Icon name={showDemos ? 'clock' : 'sparkles'} />{showDemos ? '最近のプロジェクト' : 'デモを見る'}
          </button>
        </div>
        <span className="spacer" />
        <Button kind="borderless" small icon="gearshape" className="home-settings" guide="settings" help="アプリ設定（⌘,）" onClick={() => app.openWindow(undefined, { kind: 'appSettings' })}>設定</Button>
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
        <span className="headline">最近のプロジェクト</span>
        <span className="spacer" />
        {chosen ? (
          <>
            <Toggle kind="checkbox" checked={recents.length > 0 && chosen.size === recents.length}
              onChange={(v) => setChosen(v ? new Set(recents.map((r) => r.path)) : new Set())} label="すべて選択" />
            <Button disabled={!chosen.size} help="一覧から外すだけで、フォルダは削除しません" onClick={() => {
              void app.forgetRecents(chosen)
              setChosen(undefined)
            }}>一覧から削除</Button>
            <Button onClick={() => setChosen(undefined)}>完了</Button>
          </>
        ) : recents.length > 0 && <Button help="一覧から外すプロジェクトを選ぶ" onClick={() => setChosen(new Set())}>編集</Button>}
      </div>
      {recents.length === 0 ? (
        <Empty title="最近開いたプロジェクトはありません" icon="clock" description="新規作成するか、既存のフォルダを開いてください。" />
      ) : (
        <div className="scroll">
          <div className="recents-list">
            {recents.map((p) => (
              <button key={p.path} type="button" className="recent-row" onClick={() => (chosen ? toggle(p.path) : onOpen(p.path))}
                onContextMenu={(e) => showMenu(e, [
                  { label: 'Finder で表示', action: () => void api('/api/reveal', { path: p.path }).catch((er) => app.setError(er)) },
                  { label: '一覧から削除', action: () => void app.forgetRecents([p.path]) },
                ])}>
                {chosen && (
                  <Icon name={chosen.has(p.path) ? 'checkmark.square.fill' : 'square'} title={chosen.has(p.path) ? '選択済み' : '未選択'}
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
        <span className="title2">デモ</span>
        <span className="spacer" />
        <TextField value={search} onChange={setSearch} placeholder="探す" style={{ maxWidth: 220 }} />
      </div>
      <div className="callout secondary">デモは Documents/Tsugumi に編集可能なコピーとして開きます。同梱の元データは変更されません。</div>
      {/* Tags to narrow the list as demos grow. */}
      <div className="tag-row">
        <TagChip label="すべて" selected={tag === undefined} onClick={() => setTag(undefined)} />
        {tags.map((t) => <TagChip key={t} label={t} selected={tag === t} onClick={() => setTag(tag === t ? undefined : t)} />)}
      </div>
      <div className="scroll flex1">
        {shown.length === 0 && <Empty title={demos.length === 0 ? 'デモが見つかりません' : '条件に合うデモはありません'} icon="questionmark.folder" />}
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

/** A demo with a thumbnail drawn from its own Workflow (Cards and wires). */
function DemoCard({ demo, onOpen }: { demo: Demo; onOpen: () => void }) {
  return (
    <div className="demo-card" data-guide={`demo:${demo.id}`}>
      <DemoThumbnailView mini={demo.thumbnail} />
      <div className="headline">{demo.title}</div>
      <div className="callout secondary clamp3">{demo.description}</div>
      <div className="row gap4 wrap">{demo.tags.map((t) => <span key={t} className="demo-tag caption2">{t}</span>)}</div>
      <div className="row end"><Button kind="prominent" onClick={onOpen}>コピーを開く</Button></div>
    </div>
  )
}

const THUMB_COLORS: Record<DemoThumbnail['cards'][number]['kind'], string> = {
  link: PALETTE.olive, routing: PALETTE.mist, source: PALETTE.rufous, other: PALETTE.cream,
}

/** The top-level Workflow of a demo in miniature: Cards as tiles coloured by
 *  kind, routing Cards as small marks, wires as curves. */
function DemoThumbnailView({ mini }: { mini: DemoThumbnail }) {
  const W = 320, H = 130
  const rects = new Map(mini.cards.map((c) => [c.id, { x: c.x, y: c.y, w: c.kind === 'routing' ? 70 : 220, h: c.kind === 'routing' ? 44 : 64, color: THUMB_COLORS[c.kind] }]))
  const all = [...rects.values()]
  let body: JSX.Element | null = null
  if (all.length) {
    const minX = Math.min(...all.map((r) => r.x)) - 30, minY = Math.min(...all.map((r) => r.y)) - 30
    const maxX = Math.max(...all.map((r) => r.x + r.w)) + 30, maxY = Math.max(...all.map((r) => r.y + r.h)) + 30
    const z = Math.min(W / (maxX - minX), H / (maxY - minY))
    const mx = (minX + maxX) / 2, my = (minY + maxY) / 2
    const t = (x: number, y: number) => [(x - mx) * z + W / 2, (y - my) * z + H / 2]
    body = (
      <>
        {mini.wires.map(([a, b], i) => {
          const ra = rects.get(a), rb = rects.get(b)
          if (!ra || !rb) return null
          const [sx, sy] = t(ra.x + ra.w / 2, ra.y + ra.h), [ex, ey] = t(rb.x + rb.w / 2, rb.y)
          const dy = Math.max(8, Math.abs(ey - sy) / 2)
          return <path key={i} d={`M${sx},${sy} C${sx},${sy + dy} ${ex},${ey - dy} ${ex},${ey}`} fill="none" stroke={PALETTE.cream} strokeOpacity={0.55} strokeWidth={1.2} />
        })}
        {all.map((r, i) => {
          const [x, y] = t(r.x, r.y)
          return <rect key={i} x={x} y={y} width={r.w * z} height={r.h * z} rx={Math.max(2, 8 * z)} fill={r.color} fillOpacity={0.9} />
        })}
      </>
    )
  }
  return (
    <svg className="demo-thumb" viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="xMidYMid slice" aria-hidden>
      <defs>
        <linearGradient id="demo-teal" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stopColor={PALETTE.tealTop} />
          <stop offset="1" stopColor={PALETTE.tealBottom} />
        </linearGradient>
      </defs>
      <rect width={W} height={H} fill="url(#demo-teal)" />
      {body}
    </svg>
  )
}

function NewProjectSheet({ onClose, onCreated }: { onClose: () => void; onCreated: (path: string) => void }) {
  const [name, setName] = useState('')
  const [choosing, setChoosing] = useState(false)
  return (
    <Sheet onClose={onClose} width={380}>
      <form className="sheet-body" onSubmit={(e) => { e.preventDefault(); if (name) setChoosing(true) }}>
        <div className="title2">新規プロジェクト</div>
        <TextField value={name} onChange={setName} placeholder="名前" autoFocus />
        <div className="row end gap8">
          <Button onClick={onClose}>キャンセル</Button>
          <Button kind="prominent" type="submit" disabled={!name}>作成場所を選ぶ…</Button>
        </div>
      </form>
      {choosing && <FolderPicker message="プロジェクトのフォルダを作成する場所を選んでください" allowCreate initial={app.status?.home} onClose={() => setChoosing(false)}
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
