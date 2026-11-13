// The demo player: a demo's real project drawn with the Web UI's own Card, wire
// and geometry code (cardNode.tsx, model/canvasKit.ts), with a recorded run
// replayed over it. The documentation site bundles it (docs/site/player/build.mjs,
// mountPlayer); Home's demo list shows its compact form (DemoThumbnailPlayer).
// Nothing is computed here and it needs no server: the data comes from
// player-data/<id>.json (build.mjs), the run's order and results from a real run
// (docs/site/player/harvest.py).
import { useEffect, useLayoutEffect, useMemo, useRef, useState } from 'react'
import { createRoot } from 'react-dom/client'
import { pathToScreen, WireStyle } from '../model/canvas'
import {
  applicableStages, Camera, CardGeometry, CardNodeData, displayLink, fitCamera, mergeStageStatus, ParamData, PortData, Pt, Rect, Stage, StageItem, stageParts,
  isDraftWire, StageProgress, StageStatus, union, WireRouter,
} from '../model/canvasKit'
import { cardState, displaySummary, formatDuration, KIND_ICON, KIND_LABEL } from '../model/session'
import { permissionInfo, PermissionKind } from '../model/permissions'
import { D } from '../shared/rpc'
import { CardNodeView, drive, gridStyle, WireDraw, WireLayer } from './cardNode'
import { Button } from './controls'
import { Icon } from './icons'
import { L } from '../i18n'

// ---- The data build.mjs writes --------------------------------------------------------

interface SourceFile { path: string; text: string; cut?: number }
interface InnerCard { id: string; kind: CardNodeData['kind']; summary?: string; count?: number; files: SourceFile[] }
interface CardDoc {
  id: string
  kind: CardNodeData['kind']
  x: number
  y: number
  symbol?: string
  summary?: string
  details: string[]
  inputs: PortData[]
  outputs: PortData[]
  params: ParamData[]
  allParams: ParamData[]
  permissions: PermissionKind[]
  untitled?: boolean
  files: SourceFile[]
  inner?: InnerCard[]
}
interface WireDoc { from: string; fromPort: string; to: string; toPort: string }
interface NoteDoc { id: string; x: number; y: number; width: number; height: number; color: string; fontSize: number; html: string }
interface StageRecord { phase: string; outcome: string; ms?: number | null }
interface FinalState { state: keyof typeof D.CardState; count: number; sent: string[]; stages: StageRecord[]; ms?: number }
interface StepDoc { cards: string[]; say: string; startMs?: number; endMs?: number }
export type Output =
  | { type: 'text'; text: string; markdown?: boolean; html?: string; cut?: number }
  | { type: 'scalar'; value: number; label: string }
  | { type: 'table'; columns: string[]; rows: string[][]; totalRows: number }
  | { type: 'image'; src: string; alt?: string }
  | { type: 'points'; count: number }
  | { type: 'plot'; library: string; title: string; xLabel: string; yLabel: string; series: { label: string; x: number[]; y: number[]; points: boolean }[] }
  | { type: 'module'; name: string }
export interface DemoData {
  id: string
  title: string
  description?: string
  flow: 'topToBottom' | 'leftToRight'
  draftOnly: boolean
  cards: CardDoc[]
  wires: WireDoc[]
  notes: NoteDoc[]
  frames: Record<string, Partial<Rect> & { hidden?: boolean }>
  run?: { id: string; state: string; date: string; host: string; sandbox: string; commit: string; totalMs: number; realOrder: boolean }
  steps: StepDoc[]
  final: Record<string, FinalState>
  outputs: Record<string, Output>
  intro: string
  finale: string
}

// ---- Replay: a list of frames walked by a timer ---------------------------------------

export interface Frame {
  mode: 'idle' | 'playing' | 'done'
  step: number
  sub: number
  tick: number
  ms: number
  recorded?: { from: number; to: number }
}
export const IDLE: Frame = { mode: 'idle', step: -1, sub: 0, tick: 0, ms: 0 }
export const DONE: Frame = { mode: 'done', step: Infinity, sub: 0, tick: 0, ms: 0 }
/** Invocation counts shown while a Card that ran many times (a loop) is replayed. */
const LOOP_TICKS = 8

export function framesOf(data: DemoData, speed: number): Frame[] {
  const kinds = new Map(data.cards.map((c) => [c.id, c.kind]))
  const out: Frame[] = []
  data.steps.forEach((st, step) => {
    const first = out.length
    const code = st.cards.some((c) => kinds.get(c) === 'code' || kinds.get(c) === 'subgraph')
    const loops = st.cards.some((c) => (data.final[c]?.count ?? 0) > 1)
    if (!code) {
      out.push({ mode: 'playing', step, sub: 0, tick: 0, ms: 620 * speed })
    } else {
      out.push({ mode: 'playing', step, sub: 0, tick: 0, ms: 520 * speed })
      if (loops) for (let tick = 0; tick < LOOP_TICKS; tick++) out.push({ mode: 'playing', step, sub: 1, tick, ms: 230 * speed })
      else out.push({ mode: 'playing', step, sub: 1, tick: LOOP_TICKS - 1, ms: 900 * speed })
      out.push({ mode: 'playing', step, sub: 2, tick: LOOP_TICKS - 1, ms: 480 * speed })
    }
    if (st.startMs !== undefined && st.endMs !== undefined) {
      const frames = out.slice(first)
      const duration = frames.reduce((sum, frame) => sum + frame.ms, 0)
      let elapsed = 0
      for (const frame of frames) {
        const from = st.startMs + (st.endMs - st.startMs) * elapsed / duration
        elapsed += frame.ms
        const to = st.startMs + (st.endMs - st.startMs) * elapsed / duration
        frame.recorded = { from, to }
      }
    }
  })
  return out
}

function useReplay(data: DemoData | undefined, speed: number) {
  const [frame, setFrame] = useState<Frame>(IDLE)
  const timer = useRef<ReturnType<typeof setTimeout>>()
  const stop = () => clearTimeout(timer.current)
  useEffect(() => stop, [])
  return {
    frame,
    play() {
      if (!data) return
      stop()
      const frames = framesOf(data, speed)
      let i = 0
      const next = () => {
        if (i >= frames.length) {
          setFrame(DONE)
          return
        }
        const f = frames[i++]
        setFrame(f)
        timer.current = setTimeout(next, f.ms)
      }
      next()
    },
    reset() {
      stop()
      setFrame(IDLE)
    },
    finish() {
      stop()
      setFrame(DONE)
    },
  }
}

const SHOWN_STAGES = applicableStages(true)

/** The stage strip of a finished invocation, as the session derives it from the core's stages. */
function finishedStages(f: FinalState): StageItem[] {
  return SHOWN_STAGES.map((shown) => {
    const parts: StageStatus[] = []
    let seconds: number | undefined
    for (const sp of stageParts(shown)) {
      const r = [...f.stages].reverse().find((x) => (Stage as unknown as Record<string, Stage>)[x.phase] === sp)
      if (!r) {
        parts.push('notRun')
        continue
      }
      parts.push(({ cached: 'cached', reused: 'reused', failed: 'failed', cancelled: 'cancelled' } as Record<string, StageStatus>)[r.outcome] ?? 'done')
      if (r.ms !== undefined && r.ms !== null) seconds = (seconds ?? 0) + r.ms / 1000
    }
    return { stage: shown, status: mergeStageStatus(parts, true), seconds }
  })
}

const resultText = (o: Output | undefined): string | undefined => {
  if (!o) return undefined
  if (o.type === 'plot') return o.title || L('折れ線 {0} 系列', o.series.length)
  if (o.type === 'module') return L('モジュール {0}', o.name)
  const value = o.type === 'text' ? { kind: o.markdown ? 'markdown' : 'text', text: o.text }
    : o.type === 'table' ? { kind: 'table', columns: o.columns, rows: Array(o.totalRows) }
    : o.type === 'points' ? { kind: 'points', x: Array(o.count) }
    : { ...o, kind: o.type }
  return displaySummary({ value } as Parameters<typeof displaySummary>[0])
}

/** Where a Card is in the replay. */
function phaseOf(stepOf: Map<string, number>, id: string, fr: Frame): 'before' | 'running' | 'after' {
  const idx = stepOf.get(id)
  if (fr.mode === 'idle' || idx === undefined) return 'before'
  if (fr.mode === 'done' || idx < fr.step) return 'after'
  return idx === fr.step ? 'running' : 'before'
}

export function nodeData(data: DemoData, stepOf: Map<string, number>, c: CardDoc, fr: Frame): CardNodeData {
  const out = data.outputs[c.id]
  const base: CardNodeData = {
    title: c.id, kind: c.kind, symbol: c.symbol, summary: c.summary, inputs: c.inputs, outputs: c.outputs, params: c.params, invocation: 0n,
    runContinues: fr.mode === 'playing', errors: [], warnings: [], collapsed: false, hasDisplay: !!out, permissions: c.permissions, untitled: c.untitled,
  }
  const f = data.final[c.id]
  const phase = phaseOf(stepOf, c.id, fr)
  if (!f || phase === 'before') return base
  const composite = c.kind === 'subgraph'
  if (phase === 'after') {
    const stage: StageProgress | undefined = c.kind === 'code' ? { items: finishedStages(f), workerReused: false } : undefined
    return { ...base, state: D.CardState[f.state], invocation: composite ? 0n : BigInt(f.count), stage, result: resultText(out) }
  }
  const shown = fr.sub === 0 ? 1 : Math.max(1, Math.ceil((f.count * (fr.tick + 1)) / LOOP_TICKS))
  const done = finishedStages(f)
  const stage: StageProgress | undefined = c.kind === 'code' ? {
    workerReused: false,
    items: done.map((it, i) => (i < fr.sub ? it : i === fr.sub ? { stage: it.stage, status: 'running' as const } : { stage: it.stage, status: 'pending' as const })),
  } : undefined
  return {
    ...base, state: c.kind === 'code' && fr.sub === 0 ? D.CardState.preparing : D.CardState.running, invocation: composite ? 0n : BigInt(shown), stage,
    result: fr.sub >= 1 ? resultText(out) : undefined,
  }
}

/** As Scene.style in model/canvas.ts: a value on its way moves, one that arrived stays coloured. */
function wireStyle(data: DemoData, stepOf: Map<string, number>, w: WireDoc, fr: Frame): WireStyle {
  const f = data.final[w.from]
  if (!f || !f.sent.includes(w.fromPort)) return 'normal'
  const from = phaseOf(stepOf, w.from, fr)
  if (from === 'before') return 'normal'
  if (from === 'running' && !(f.count > 1 && fr.sub >= 1) && !(fr.sub >= 2)) return 'normal'
  if (fr.mode === 'playing' && phaseOf(stepOf, w.to, fr) !== 'after') return 'active'
  return 'carried'
}

// ---- Output frames --------------------------------------------------------------------

const PLOT_COLOR = ['var(--blue)', 'var(--orange)', 'var(--green)', 'var(--purple)']
const fmt = (v: number) => {
  const a = Math.abs(v)
  return a !== 0 && (a >= 1e5 || a < 1e-2) ? v.toExponential(1) : String(Number(v.toPrecision(3)))
}

/** A figure's own points without the plot library (the app draws these with Plotly,
 *  Vega-Lite or the WebGPU line view, interactive): the page needs neither those nor WebGPU. */
function PlotView({ plot }: { plot: Extract<Output, { type: 'plot' }> }) {
  const W = 560, H = 250
  const m = { left: 54, right: 12, top: plot.title ? 24 : 10, bottom: plot.xLabel ? 36 : 22 }
  const shown = plot.series.slice(0, 4)
  const xs = shown.flatMap((s) => s.x), ys = shown.flatMap((s) => s.y)
  if (!xs.length) return <div className="display-center secondary" style={{ fontSize: 11 }}>{L('図の点を取り出せませんでした')}</div>
  const [x0, x1, y0, y1] = [Math.min(...xs), Math.max(...xs), Math.min(...ys), Math.max(...ys)]
  const sx = (v: number) => m.left + ((v - x0) / (x1 - x0 || 1)) * (W - m.left - m.right)
  const sy = (v: number) => H - m.bottom - ((v - y0) / (y1 - y0 || 1)) * (H - m.top - m.bottom)
  const ticks = (a: number, b: number) => [a, (a + b) / 2, b]
  return (
    <svg className="tp-plot" viewBox={`0 0 ${W} ${H}`} role="img" aria-label={plot.title || L('折れ線の図')}>
      {plot.title && <text x={W / 2} y={14} textAnchor="middle" className="tp-plot-title">{plot.title}</text>}
      {ticks(y0, y1).map((t) => (
        <g key={t}>
          <line x1={m.left} x2={W - m.right} y1={sy(t)} y2={sy(t)} stroke="var(--separator)" />
          <text x={m.left - 6} y={sy(t) + 3.5} textAnchor="end" className="tp-plot-tick">{fmt(t)}</text>
        </g>
      ))}
      {ticks(x0, x1).map((t) => <text key={t} x={sx(t)} y={H - m.bottom + 14} textAnchor="middle" className="tp-plot-tick">{fmt(t)}</text>)}
      {plot.xLabel && <text x={(m.left + W - m.right) / 2} y={H - 6} textAnchor="middle" className="tp-plot-tick">{plot.xLabel}</text>}
      {plot.yLabel && <text transform={`translate(11 ${(m.top + H - m.bottom) / 2}) rotate(-90)`} textAnchor="middle" className="tp-plot-tick">{plot.yLabel}</text>}
      {shown.map((s, i) => s.points
        ? <g key={i} fill={PLOT_COLOR[i]} fillOpacity={0.7}>{s.x.map((x, j) => <circle key={j} cx={sx(x)} cy={sy(s.y[j])} r={2.4} />)}</g>
        : <polyline key={i} fill="none" stroke={PLOT_COLOR[i]} strokeWidth={2} strokeLinejoin="round" points={s.x.map((x, j) => `${sx(x).toFixed(1)},${sy(s.y[j]).toFixed(1)}`).join(' ')}><title>{s.label}</title></polyline>)}
      {shown.length > 1 && shown.map((s, i) => (
        <g key={'k' + i} transform={`translate(${m.left + 8} ${m.top + 8 + i * 14})`}>
          <rect width={10} height={3} y={-3} fill={PLOT_COLOR[i]} rx={1.5} />
          <text x={14} y={1} className="tp-plot-tick">{s.label}</text>
        </g>
      ))}
    </svg>
  )
}

/** A Card's result, in the classes of web/src/ui/display.tsx. */
function OutputView({ output, base, zoom, large = false }: { output: Output; base: string; zoom: number; large?: boolean }) {
  const size = (v: number) => v * zoom
  switch (output.type) {
    case 'text':
      if (output.html) return <div className="display-body tp-md" style={{ fontSize: size(11) }} dangerouslySetInnerHTML={{ __html: output.html }} />
      return <div className="display-body"><pre className="display-text" style={{ fontSize: size(11) }}>{output.text}</pre></div>
    case 'scalar':
      return (
        <div className="display-center col">
          <span className="secondary" style={{ fontSize: size(10) }}>{output.label}</span>
          <span className="mono-digits" style={{ fontSize: size(22), fontWeight: 600 }}>{Number(output.value.toPrecision(6))}</span>
        </div>
      )
    case 'table':
      return (
        <div className="display-body">
          <table className="display-table" style={{ fontSize: size(10), borderSpacing: `${size(12)}px ${size(3)}px`, padding: size(4) }}>
            <thead><tr>{output.columns.map((c) => <th key={c}>{c}</th>)}</tr></thead>
            <tbody>{output.rows.map((r, i) => <tr key={i}>{r.map((v, j) => <td key={j}>{v}</td>)}</tr>)}</tbody>
          </table>
          {large && output.totalRows > output.rows.length && <p className="secondary caption">{L('全 {0} 行のうち先頭 {1} 行', output.totalRows, output.rows.length)}</p>}
        </div>
      )
    case 'image':
      return <div className="display-center"><img className="display-image" src={new URL(output.src, base).href} alt={output.alt ?? ''} draggable={false} /></div>
    case 'plot':
      return <div className="display-center"><PlotView plot={output} /></div>
    case 'points':
      return <div className="display-center secondary" style={{ fontSize: size(11) }}>{L('3D 点群 {0} 点（アプリでは回して見られます）', output.count)}</div>
    default:
      return <div className="display-center secondary" style={{ fontSize: size(11) }}>{L('モジュール {0}', output.name)}</div>
  }
}

// ---- The canvas ---------------------------------------------------------------------------

interface Placed { doc: CardDoc; node: CardNodeData; geometry: CardGeometry }

/** `view` draws a still at that size, fitted (or with its camera) and without the zoom controls (player/svg.mjs renders one per frame). */
export function Canvas({ data, frame, base, compact, selected, onSelect, motion, view, children }: {
  data: DemoData; frame: Frame; base: string; compact: boolean; selected?: string; onSelect: (id: string | undefined) => void; motion: boolean
  view?: { width: number; height: number; camera?: Camera }; children?: React.ReactNode
}) {
  const ref = useRef<HTMLDivElement>(null)
  const [size, setSize] = useState(view ? { width: view.width, height: view.height } : { width: 0, height: 0 })
  const [moved, setCamera] = useState<Camera>({ x: 0, y: 0, zoom: 0.5 })
  const adjusted = useRef(false)
  const stepOf = useMemo(() => {
    const m = new Map<string, number>()
    data.steps.forEach((s, i) => s.cards.forEach((c) => m.set(c, i)))
    return m
  }, [data])
  useLayoutEffect(() => {
    const el = ref.current
    if (!el) return
    const measure = () => setSize({ width: el.clientWidth, height: el.clientHeight })
    const o = new ResizeObserver(measure)
    o.observe(el)
    measure()
    return () => o.disconnect()
  }, [])

  const placed: Placed[] = data.cards.map((doc) => {
    const node = nodeData(data, stepOf, doc, frame)
    return { doc, node, geometry: new CardGeometry(node, data.flow) }
  })
  const byId = new Map(placed.map((p) => [p.doc.id, p]))
  const rectOf = (p: Placed): Rect => ({ x: p.doc.x, y: p.doc.y, ...p.geometry.size })

  // The saved place of a Card's output frame, or beside the Card (Scene.frameRect).
  const frames = placed.flatMap((p) => {
    if (!data.outputs[p.doc.id]) return []
    const f = data.frames[p.doc.id]
    if (f?.hidden) return []
    const width = f?.width ?? 320, height = f?.height ?? 170
    const r: Rect = data.flow === 'topToBottom'
      ? { x: f?.x ?? p.doc.x + p.geometry.size.width + 40, y: f?.y ?? p.doc.y, width, height }
      : { x: f?.x ?? p.doc.x, y: f?.y ?? p.doc.y + p.geometry.height + 30, width, height }
    return [{ p, r }]
  })
  const notes = compact ? [] : data.notes
  const box = [...placed.map(rectOf), ...frames.map((f) => f.r), ...notes].reduce<Rect | null>((u, r) => {
    const rect = { x: r.x, y: r.y, width: r.width, height: r.height }
    return u ? union(u, rect) : rect
  }, null)
  const boxKey = box ? `${box.x},${box.y},${box.width},${box.height}` : ''
  const fitted = (): Camera | undefined => {
    if (!box || size.width <= 0) return undefined
    if (compact) {
      // A thumbnail shows the whole graph, smaller than a canvas ever zooms out.
      const margin = 34, r = { x: box.x - margin, y: box.y - margin, width: box.width + 2 * margin, height: box.height + 2 * margin }
      const zoom = Math.min(0.85, size.width / r.width, size.height / r.height)
      return { zoom, x: size.width / 2 - (r.x + r.width / 2) * zoom, y: size.height / 2 - (r.y + r.height / 2) * zoom }
    }
    return fitCamera(box, size, { top: 44, left: 0, bottom: 0, right: 0 }, 1, 54) ?? undefined
  }
  const fit = () => {
    const c = fitted()
    if (c) setCamera(c)
  }
  const camera = (view && (view.camera ?? fitted())) || moved
  useEffect(() => {
    if (!adjusted.current) fit()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [boxKey, size.width, size.height])

  const z = camera.zoom
  const router = new WireRouter(data.flow, [...placed.map(rectOf), ...frames.map((f) => f.r)])
  const point = (id: string, port: string, output: boolean): Pt => {
    const p = byId.get(id)
    if (!p) return { x: 0, y: 0 }
    const at = p.geometry.port(port, output)
    return { x: p.doc.x + at.x, y: p.doc.y + at.y }
  }
  const draws: WireDraw[] = data.wires.map((w) => ({
    d: pathToScreen(router.route(point(w.from, w.fromPort, true), point(w.to, w.toPort, false)).d, camera),
    style: wireStyle(data, stepOf, w, frame),
    dotted: isDraftWire((id) => byId.get(id)?.doc.kind, w.from, w.to),
  }))
  const shownFrames = frames.filter(({ p }) => {
    const phase = phaseOf(stepOf, p.doc.id, frame)
    return phase === 'after' || (phase === 'running' && frame.sub >= 1)
  })
  for (const { p, r } of shownFrames) {
    const o = p.geometry.outlineRect
    const link = displayLink({ x: p.doc.x + o.x, y: p.doc.y + o.y, width: o.width, height: o.height }, r)
    draws.push({ d: pathToScreen(link, camera), style: 'display' })
  }
  const screen = (r: { x: number; y: number }) => ({ left: Math.round((r.x * z + camera.x) * 2) / 2, top: Math.round((r.y * z + camera.y) * 2) / 2 })
  const zoomTo = (nz: number) => {
    adjusted.current = true
    const c = { x: size.width / 2, y: size.height / 2 }
    const next = Math.max(0.2, Math.min(3, nz))
    setCamera((v) => ({ x: c.x - ((c.x - v.x) * next) / v.zoom, y: c.y - ((c.y - v.y) * next) / v.zoom, zoom: next }))
  }

  return (
    <div ref={ref} className={`cv-surface tp-surface${compact && z < 0.5 ? ' tp-small' : ''}${compact && z < 0.2 ? ' tp-tiny' : ''}`} style={gridStyle(camera, !compact, 20)}
      onPointerDown={compact ? undefined : (e) => {
        if (e.button !== 0) return
        const start = { x: e.clientX, y: e.clientY, cx: camera.x, cy: camera.y }
        drive(e, 3, (ev) => {
          adjusted.current = true
          setCamera((c) => ({ ...c, x: start.cx + ev.clientX - start.x, y: start.cy + ev.clientY - start.y }))
        }, (_ev, moved) => { if (!moved) onSelect(undefined) })
      }}
      onWheel={compact ? undefined : (e) => { if (e.ctrlKey || e.metaKey) zoomTo(z * Math.exp(-e.deltaY * 0.01)) }}>
      {notes.map((n) => (
        <div key={n.id} className="cv-note tp-note" style={{
          position: 'absolute', ...screen(n), width: n.width * z, height: n.height * z, padding: 10 * z, borderRadius: 6 * z,
          background: `color-mix(in srgb, var(--note-${n.color}) 28%, transparent)`, borderColor: `color-mix(in srgb, var(--note-${n.color}) 50%, transparent)`,
        }}>
          <div className="cv-note-text tp-md" style={{ fontSize: n.fontSize * z }} dangerouslySetInnerHTML={{ __html: n.html }} />
        </div>
      ))}
      <WireLayer wires={draws} animate={motion} zoom={z} width={size.width} height={size.height} />
      {shownFrames.map(({ p, r }) => {
        const inv = data.final[p.doc.id]?.count
        return (
          <div key={p.doc.id} className="cv-frame tp-frame" style={{ position: 'absolute', ...screen(r), width: r.width * z, height: r.height * z, padding: 8 * z, borderRadius: 8 * z, gap: 4 * z }}>
            <div className="cv-frame-header secondary" style={{ gap: 6 * z }}>
              <Icon name="chart.xyaxis.line" size={10 * z} style={{ color: 'var(--purple)' }} />
              <span className="cv-ellipsis" style={{ fontSize: 10 * z, fontWeight: 700, color: 'var(--label)' }}>{p.doc.id}</span>
              {data.run && <span className="mono-digits nowrap" style={{ fontSize: 9 * z }}>{L('Run {0} · 実行 #{1}', data.run.id.slice(0, 6), inv ?? 1)}</span>}
            </div>
            <div className="cv-frame-body"><OutputView output={data.outputs[p.doc.id]} base={base} zoom={z} /></div>
          </div>
        )
      })}
      {placed.map(({ doc, node }) => (
        <CardNodeView key={doc.id} data={node} flow={data.flow} zoom={z} selected={selected === doc.id}
          style={{ position: 'absolute', ...screen(doc) }}
          onPointerDown={compact ? undefined : (e) => {
            if (e.button !== 0) return
            e.stopPropagation()
            const start = { x: e.clientX, y: e.clientY, cx: camera.x, cy: camera.y }
            drive(e, 3, (ev) => {
              adjusted.current = true
              setCamera((c) => ({ ...c, x: start.cx + ev.clientX - start.x, y: start.cy + ev.clientY - start.y }))
            }, (_ev, moved) => { if (!moved) onSelect(doc.id) })
          }} />
      ))}
      {!compact && !view && (
        <div className="cv-surface-zoom material" onPointerDown={(e) => e.stopPropagation()}>
          <Button kind="borderless" small iconOnly icon="minus.magnifyingglass" help={L('縮小')} onClick={() => zoomTo(z / 1.2)} />
          <span className="caption mono-digits" style={{ minWidth: 36, textAlign: 'center' }}>{Math.round(z * 100)}%</span>
          <Button kind="borderless" small iconOnly icon="plus.magnifyingglass" help={L('拡大')} onClick={() => zoomTo(z * 1.2)} />
          <Button kind="borderless" small iconOnly icon="arrow.up.left.and.down.right.magnifyingglass" help={L('全体表示')} onClick={() => { adjusted.current = false; fit() }} />
        </div>
      )}
      {children}
    </div>
  )
}

// ---- Side panel -----------------------------------------------------------------------------

function Sources({ files, open = true }: { files: SourceFile[]; open?: boolean }) {
  return (
    <>
      {files.map((f, i) => (
        <details key={f.path} className="tp-file" open={open && i === 0}>
          <summary className="mono">{f.path}</summary>
          <pre className="tp-source mono selectable">{f.text}</pre>
          {f.cut !== undefined && <p className="secondary caption">{L('長いので先頭だけを載せています（全 {0} 文字）。', f.cut)}</p>}
        </details>
      ))}
    </>
  )
}

function Ports({ title, ports }: { title: string; ports: PortData[] }) {
  if (!ports.length) return null
  return (
    <>
      <h4>{title}</h4>
      <ul className="tp-ports">
        {ports.map((p) => (
          <li key={p.name}>
            <span className="mono">{p.name}</span>
            {p.typeName && <span className="mono secondary"> : {p.typeName}</span>}
            {p.optional && <span className="secondary">{L('（省略可）')}</span>}
            {p.note && <div className="secondary caption">{p.note}</div>}
          </li>
        ))}
      </ul>
    </>
  )
}

function SidePanel({ data, card, reached, base, onClose }: { data: DemoData; card: CardDoc; reached: boolean; base: string; onClose: () => void }) {
  const f = data.final[card.id]
  const out = data.outputs[card.id]
  return (
    <aside className="tp-panel material selectable" aria-label={L('Card {0} の中身', card.id)} onPointerDown={(e) => e.stopPropagation()} onWheel={(e) => e.stopPropagation()}>
      <header className="tp-panel-head">
        <Icon name={card.symbol ?? KIND_ICON[card.kind]} size={14} />
        <strong className="cv-ellipsis">{card.id}</strong>
        <span className="secondary caption nowrap">{KIND_LABEL[card.kind]}</span>
        <span style={{ flex: 1 }} />
        <Button kind="borderless" small iconOnly icon="xmark" help={L('閉じる')} onClick={onClose} />
      </header>
      {card.details.map((d, i) => <p key={i} className={i ? 'secondary' : undefined}>{d}</p>)}
      {card.permissions.map((p) => <p key={p} className="caption tp-permission"><Icon name={permissionInfo(p).icon} size={11} /> {permissionInfo(p).label}</p>)}
      {reached && f && data.run?.realOrder && (
        <p className="caption secondary mono-digits">記録：{cardState(D.CardState[f.state]).label} · {f.count > 1 ? L('{0} 回 · ', f.count) : ''}{f.ms !== undefined ? formatDuration(f.ms / 1000) : ''}</p>
      )}
      <Ports title={L('入力')} ports={card.inputs} />
      <Ports title={L('出力')} ports={card.outputs} />
      {card.allParams.length > 0 && (
        <>
          <h4>{L('パラメータ')}</h4>
          <ul className="tp-ports">{card.allParams.map((p) => <li key={p.name}><span className="mono">{p.name}</span><span className="mono secondary"> : {p.type} = {p.value}</span></li>)}</ul>
        </>
      )}
      {reached && out && (
        <>
          <h4>{L('この Card の表示')}</h4>
          <div className="tp-panel-output"><OutputView output={out} base={base} zoom={1} large /></div>
        </>
      )}
      {card.files.length > 0 && <><h4>{L('ファイル')}</h4><Sources files={card.files} /></>}
      {card.inner && card.inner.length > 0 && (
        <>
          <h4>{L('中の Card')}</h4>
          {card.inner.map((c) => (
            <details key={c.id} className="tp-file">
              <summary><span className="mono">{c.id}</span> <span className="secondary caption">{KIND_LABEL[c.kind]}{reached && c.count ? ` · ${c.count} 回` : ''}</span></summary>
              {c.summary && <p className="secondary">{c.summary}</p>}
              <Sources files={c.files} open={false} />
            </details>
          ))}
        </>
      )}
    </aside>
  )
}

// ---- The player -------------------------------------------------------------------------------

/** Interpolate recorded time within a replay frame, independent of the replay speed. */
export function recordedTime(frame: Frame, elapsed: number): number | undefined {
  if (!frame.recorded) return undefined
  const progress = frame.ms > 0 ? Math.max(0, Math.min(1, elapsed / frame.ms)) : 1
  return frame.recorded.from + (frame.recorded.to - frame.recorded.from) * progress
}

// Only the clock re-renders at display cadence; Cards still advance by replay frames.
function ReplayElapsed({ frame, totalMs }: { frame: Frame; totalMs?: number }) {
  const [sample, setSample] = useState<{ frame: Frame; ms: number }>()
  useEffect(() => {
    if (frame.mode !== 'playing' || !frame.recorded) return
    const start = performance.now()
    let request: number
    const tick = (now: number) => {
      const elapsed = now - start
      setSample({ frame, ms: recordedTime(frame, elapsed)! })
      if (elapsed < frame.ms) request = requestAnimationFrame(tick)
    }
    request = requestAnimationFrame(tick)
    return () => cancelAnimationFrame(request)
  }, [frame])
  const ms = frame.mode === 'done' ? totalMs
    : frame.mode === 'playing' ? (sample?.frame === frame ? sample.ms : recordedTime(frame, 0)) : undefined
  return ms === undefined ? null : <span className="secondary mono-digits nowrap" title={L('実際の実行での経過時間（記録）')}>{formatDuration(ms / 1000)}</span>
}

export function RunControl({ data, frame, playing, onRun, onStop }: { data: DemoData; frame: Frame; playing: boolean; onRun: () => void; onStop: () => void }) {
  const label = frame.mode === 'playing' ? L('再生中') : frame.mode === 'done' ? (data.run?.state === 'failed' ? L('失敗') : L('成功')) : L('待機中')
  const icon = frame.mode === 'playing' ? 'play.circle.fill' : frame.mode === 'done' ? 'checkmark.circle.fill' : 'circle'
  const color = frame.mode === 'playing' ? 'orange' : frame.mode === 'done' ? 'green' : 'secondary'
  const real = data.run?.realOrder
  return (
    <div className="capsule material cv-run" onPointerDown={(e) => e.stopPropagation()}>
      <Button kind={playing ? 'danger' : 'prominent'} icon={playing ? 'stop.fill' : 'play.fill'} className="cv-run-button" disabled={data.draftOnly}
        help={data.draftOnly ? L('Draft だけのプロジェクトは、まだ実行できません') : playing ? L('再生を止める') : L('記録した実行を再生する')} onClick={playing ? onStop : onRun}>
        {playing ? L('停止') : L('実行')}
      </Button>
      <span className="cv-run-state">
        <Icon name={icon} style={{ color: `var(--${color})` }} />
        <span className="nowrap">{label}</span>
        {real && <ReplayElapsed frame={frame} totalMs={data.run?.totalMs} />}
      </span>
    </div>
  )
}

const reducedMotion = () => typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches

function Player({ host, url, compact, autoplay }: { host: HTMLElement; url: string; compact: boolean; autoplay: boolean }) {
  const base = useMemo(() => new URL(url, document.baseURI).href, [url])
  const [data, setData] = useState<DemoData>()
  const [failure, setFailure] = useState<string>()
  const [selected, setSelected] = useState<string>()
  const motion = useMemo(() => !reducedMotion(), [])
  const replay = useReplay(data, compact ? 0.6 : 1)
  const [near, setNear] = useState(!compact)

  // A gallery fetches a thumbnail's data only as it comes near the screen.
  useEffect(() => {
    if (!compact) return
    const o = new IntersectionObserver((es) => { if (es.some((e) => e.isIntersecting)) setNear(true) }, { rootMargin: '300px' })
    o.observe(host)
    return () => o.disconnect()
  }, [compact, host])

  useEffect(() => {
    if (!near) return
    let live = true
    fetch(base).then((r) => {
      if (!r.ok) throw new Error(`HTTP ${r.status}`)
      return r.json() as Promise<DemoData>
    }).then((d) => { if (live) setData(d) }).catch((e) => { if (live) setFailure(String(e?.message ?? e)) })
    return () => { live = false }
  }, [base, near])

  // Plays when scrolled into view; a thumbnail also on hover. With reduced motion
  // nothing starts by itself and a thumbnail shows where the run ends.
  const runnable = !!data && !data.draftOnly
  useEffect(() => {
    if (!runnable) return
    if (!motion) {
      if (compact) replay.finish()
      return
    }
    const cleanups: (() => void)[] = []
    if (autoplay) {
      let once = false
      const o = new IntersectionObserver((es) => {
        if (once || !es.some((e) => e.isIntersecting)) return
        once = true
        replay.play()
      }, { threshold: 0.4 })
      o.observe(host)
      cleanups.push(() => o.disconnect())
    }
    if (compact) {
      const enter = () => replay.play()
      host.addEventListener('pointerenter', enter)
      cleanups.push(() => host.removeEventListener('pointerenter', enter))
    }
    return () => cleanups.forEach((c) => c())
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [runnable, motion, compact, autoplay, host])

  useEffect(() => {
    if (compact || !selected) return
    const key = (e: KeyboardEvent) => { if (e.key === 'Escape') setSelected(undefined) }
    window.addEventListener('keydown', key)
    return () => window.removeEventListener('keydown', key)
  }, [compact, selected])

  if (failure) return <div className="tp-root tp-message">{L('デモのデータを読み込めませんでした（{0}）', failure)}</div>
  if (!data) return <div className={`tp-root ${compact ? 'tp-compact' : 'tp-full'} tp-message`}>{compact ? '' : L('読み込み中…')}</div>

  const { frame } = replay
  const playing = frame.mode === 'playing'
  const step = playing ? data.steps[frame.step] : undefined
  const say = step ? step.say : frame.mode === 'done' ? data.finale : data.intro
  const card = data.cards.find((c) => c.id === selected)
  const reached = !!card && frame.mode !== 'idle' && (frame.mode === 'done' || (data.steps.findIndex((s) => s.cards.includes(card.id)) < frame.step))

  if (compact) {
    return (
      <div className="tp-root tp-compact" role="img" aria-label={L('{0} の Canvas{1}', data.title, data.draftOnly ? '（Draft）' : '')}>
        <Canvas data={data} frame={frame} base={base} compact selected={undefined} onSelect={() => undefined} motion={motion} />
      </div>
    )
  }
  return (
    <div className="tp-root tp-full">
      <Canvas data={data} frame={frame} base={base} compact={false} selected={selected} onSelect={setSelected} motion={motion}>
        <div className="tp-top"><RunControl data={data} frame={frame} playing={playing} onRun={replay.play} onStop={replay.reset} /></div>
        {card && <SidePanel data={data} card={card} reached={reached} base={base} onClose={() => setSelected(undefined)} />}
      </Canvas>
      <div className="tp-cards-list">
        {data.cards.map((c) => <button key={c.id} type="button" onClick={() => setSelected(c.id)}>{L('Card {0} の中身を開く', c.id)}</button>)}
      </div>
      <p className="tp-say" aria-live="polite">{say}</p>
    </div>
  )
}

// ---- API --------------------------------------------------------------------------------------

export interface PlayerOptions {
  /** The demo's id (`demos/index.json`, or `card-guide`). */
  demo: string
  /** Relative URL of its player-data/<id>.json. */
  data: string
  /** Play once when scrolled into view (never with prefers-reduced-motion). A compact player does so unless this is false. */
  autoplay?: boolean
  /** The gallery thumbnail: the canvas fitted to the box, no controls and no panel; plays on hover. */
  compact?: boolean
}

/** The compact player inside a page of the Web UI, which has the player's rules itself. */
export function DemoThumbnailPlayer({ data, className }: { data: string; className?: string }) {
  const [host, setHost] = useState<HTMLDivElement | null>(null)
  return <div ref={setHost} className={className}>{host && <Player host={host} url={data} compact autoplay />}</div>
}

function ensureStyles() {
  const href = new URL('../player.css', import.meta.url).href
  const has = [...document.querySelectorAll<HTMLLinkElement>('link[rel=stylesheet]')].some((l) => l.href.split('?')[0] === href)
  if (has) return
  const link = document.createElement('link')
  link.rel = 'stylesheet'
  link.href = href
  document.head.appendChild(link)
}

/** Draws the demo's player into `el`. The stylesheet (site/player.css) is added if the page has not linked it. */
export function mountPlayer(el: HTMLElement, options: PlayerOptions): { destroy(): void } {
  ensureStyles()
  const compact = !!options.compact
  const root = createRoot(el)
  el.dataset.demo = options.demo
  root.render(<Player host={el} url={options.data} compact={compact} autoplay={options.autoplay ?? compact} />)
  return {
    destroy() {
      root.unmount()
      delete el.dataset.demo
    },
  }
}
