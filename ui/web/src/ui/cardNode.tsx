// A Card as drawn on the main canvas, in Card settings and in the demo
// player, at the screen scale `zoom`:
// fonts and spacing are multiplied by the zoom, so text stays sharp at any
// zoom and the hit areas are the geometry's. Also the wire layer.
import { CSSProperties, useEffect, useRef, useState } from 'react'
import {
  Camera, CardGeometry, CardNodeData, currentStage, failedStage, isRouting, notRunStages, PortData, Pt, Rect, Stage, STAGE_INFO,
  NEW_PORT, StageItem, StageProgress, StageStatus,
} from '../model/canvasKit'
import { BoundaryPort, WireStyle } from '../model/canvas'
import type { FlowDirection } from '../model/prefs'
import { CardKey, cardState, formatDuration, formatShortDuration, InlineDraft, KIND_LABEL, relativeAge } from '../model/session'
import { D } from '../wire/rpc'
import { Icon } from './icons'
import { SECTION_STATUS_LABEL, sectionStatus } from './statusSymbol'
import { permissionInfo } from '../model/permissions'
import { L } from '../i18n'

// ---- Texts ------------------------------------------------------------------------

/** "Invocation #3 · Succeeded" and the like; undefined before the first invocation. */
export function runText(d: CardNodeData): string | undefined {
  if (d.executionList && d.state !== undefined) return cardState(d.state).label + (d.runMark ? ` · ${d.runMark}` : '')
  if (d.state === undefined || d.invocation <= 0n) return d.runMark
  let s = L('実行 #{0}', d.invocation)
  const cur = currentStage(d.stage)
  if (d.activity) {
    s += ' · ' + d.activity
  } else if (cur && (d.state === D.CardState.running || d.state === D.CardState.preparing)) {
    s += ' · ' + STAGE_INFO[cur.stage].active
  } else {
    s += ' · ' + cardState(d.state).label
    if (d.state === D.CardState.succeeded && d.runContinues) s += L('（次の入力を待てます）')
  }
  return s + (d.runMark ? ` · ${d.runMark}` : '')
}

/** Plain status for a Card that did not run in this run. */
export function idleText(d: CardNodeData): string {
  if (d.kind === 'draft') return L('未実装（Draft）')
  if (d.kind === 'invalid') return L('定義エラー')
  if (d.state !== undefined && d.state !== D.CardState.not_started) return cardState(d.state).label
  return L('未実行（0 回）')
}

/** The first line of a diagnostic that says something: blank lines and a bare "error:" prefix are skipped. */
export function firstMeaningfulLine(text: string): string {
  for (const raw of text.split(/\r?\n/)) {
    let line = raw.trim()
    if (line.toLowerCase().startsWith('error:')) line = line.slice(6).trim()
    if (line) return line
  }
  return ''
}

/** A Card's status line: its text and colour (a colour name of the theme),
 *  and what its hover says, when more than the line. */
export interface StatusLine { text: string; color: string; hover?: string }

/** The one line under a Card's name: what it is doing now, how its last
 *  invocation ended, or the gist of its failure. Always one line, so the
 *  Card's size never changes with its state. */
export function statusLine(d: CardNodeData, now = Date.now()): StatusLine {
  const mark = d.runMark ? ` · ${d.runMark}` : ''
  const failedColor = cardState(D.CardState.failed).color
  const st = d.state
  // No worker could be chosen (core failure stage=worker_selection): the Card
  // never started, and the hover says each worker's reason.
  if (st === D.CardState.failed && d.notStarted !== undefined) {
    return { text: L('worker を選べず未開始') + mark, color: failedColor, hover: d.notStarted || undefined }
  }
  if (st === D.CardState.failed || st === D.CardState.blocked) {
    let text = (d.failure !== undefined ? firstMeaningfulLine(d.failure) : '') || cardState(st).label
    const stage = d.stage?.failedPart ?? failedStage(d.stage)?.stage
    if (stage !== undefined && STAGE_INFO[stage].name !== text) text = STAGE_INFO[stage].name + ': ' + text
    return { text: text + mark, color: st === D.CardState.failed ? failedColor : cardState(st).color }
  }
  if (d.errors.length && (st === undefined || st === D.CardState.not_started)) return { text: firstMeaningfulLine(d.errors[0]), color: failedColor }
  if (d.executionList) return { text: runText(d) ?? idleText(d), color: st === undefined ? 'secondary' : cardState(st).color }
  if (st === undefined || st === D.CardState.not_started) return { text: (d.result ?? L('未実行')) + mark, color: 'secondary' }
  switch (st) {
    case D.CardState.running:
    case D.CardState.preparing: {
      const cur = currentStage(d.stage)
      return { text: (d.activity ?? (cur ? STAGE_INFO[cur.stage].active : cardState(st).label)) + mark, color: cardState(st).color }
    }
    case D.CardState.succeeded: {
      let text = cardState(st).label
      if (d.invocation > 1n) text += ` ×${d.invocation}`
      if (d.endedAt !== undefined) text += ' · ' + relativeAge(d.endedAt, now)
      if (d.seconds !== undefined) text += ' · ' + formatShortDuration(d.seconds)
      return { text: text + mark, color: 'secondary' }
    }
    case D.CardState.skipped: return { text: cardState(st).label + mark, color: 'secondary' }
    default: return { text: cardState(st).label + mark, color: cardState(st).color }
  }
}

/** Now, again every 30 seconds while `on` (for "3 min ago"). */
function useNow(on: boolean): number {
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    if (!on) return
    const t = setInterval(() => setNow(Date.now()), 30_000)
    return () => clearInterval(t)
  }, [on])
  return on ? now : Date.now()
}

const colorVar = (c: string) => `var(--${c})`
const fade = (c: string, opacity: number) => `color-mix(in srgb, ${colorVar(c)} ${Math.round(opacity * 100)}%, transparent)`

// ---- Card view -----------------------------------------------------------------------

/** Writing a Draft on its Card: what is typed so far, where it goes, and how it ends. */
export interface DraftEdit {
  draft: InlineDraft
  /** Why the Card cannot take the name typed so far. */
  onChange: (change: Partial<Pick<InlineDraft, 'content'>>) => void
  /** Done, the focus having moved elsewhere (undefined) or by the keyboard (true). */
  onDone: (byKey?: boolean) => void
}

export interface CardNodeProps {
  data: CardNodeData
  flow: FlowDirection
  zoom?: number
  selected?: boolean
  /** What the guide calls this Card when it points at it ("card:<id>"). */
  guide?: string
  /** Pointer down on a port: start drawing a wire (from an output, or back from an input). */
  onPortDown?: (port: string, output: boolean, e: React.PointerEvent) => void
  draftEdit?: DraftEdit
  onStageTap?: (stage: Stage) => void
  onToggleCollapse?: () => void
  /** The badge of open comments: opens them (undefined: only shows the number). */
  onComments?: () => void
  /** The start Card's button: runs the workflow from it (undefined: not offered). */
  onRun?: () => void
  style?: CSSProperties
  className?: string
  onPointerDown?: (e: React.PointerEvent) => void
  onDoubleClick?: (e: React.MouseEvent) => void
  onContextMenu?: (e: React.MouseEvent) => void
}

export function CardNodeView(p: CardNodeProps) {
  const { data, flow } = p
  const z = p.zoom ?? 1
  const s = (v: number) => v * z
  const font = (size: number) => Math.max(6, size * z)
  const g = new CardGeometry(data, flow)
  const o = g.outlineRect
  const outline = g.outline({ x: o.x * z, y: o.y * z, width: o.width * z, height: o.height * z })
  const selected = !!p.selected
  const draft = data.kind === 'draft'
  const fill = isRouting(data) ? 'var(--window-bg)' : 'var(--control-bg)'
  // A Card whose definition cannot be read is drawn as a failure.
  const border = selected ? 'var(--accent)' : data.kind === 'invalid' ? fade(cardState(D.CardState.failed).color, 0.7)
    : data.state !== undefined && data.state !== D.CardState.not_started
    ? fade(cardState(data.state).color, 0.7) : fade('secondary', draft ? 0.7 : 0.4)
  const w = s(g.size.width), h = s(g.size.height)
  const parts = [`Card ${data.title}`, KIND_LABEL[data.kind], runText(data) ?? idleText(data)]
  if (data.summary) parts.push(data.summary)
  if (data.errors.length) parts.push(data.errors.join(' '))

  return (
    <div className={`cv-card ${p.className ?? ''}`} style={{ width: w, height: h, ...p.style }} data-guide={p.guide} role="group" aria-label={parts.join(', ')}
      onPointerDown={p.onPointerDown} onDoubleClick={p.onDoubleClick} onContextMenu={p.onContextMenu}>
      <svg className="cv-card-fill" width={w} height={h}>
        <path d={outline} fill={fill} style={{ filter: `drop-shadow(0 1px ${selected ? 4 : 1.5}px rgba(0,0,0,${selected ? 0.2 : 0.07}))` }} />
      </svg>
      {g.shape === 'card' ? <CardBody p={p} g={g} /> : g.shape === 'start' ? <StartBody p={p} g={g} /> : <RoutingBody data={data} g={g} z={z} />}
      <svg className="cv-card-stroke" width={w} height={h}>
        <path d={outline} fill="none" stroke={border} strokeWidth={selected ? 2.4 : 1} strokeDasharray={draft ? `${5 * z} ${3 * z}` : undefined} />
      </svg>
      <PortLabels data={data} g={g} flow={flow} z={z} />
      {!!data.openComments && (
        <button type="button" className="cv-comments" disabled={!p.onComments} aria-label={L('未解決のコメント {0} 件', data.openComments)}
          title={L('未解決のコメント {0} 件', data.openComments)} {...stopping}
          style={{ left: (o.x + o.width) * z, top: o.y * z, fontSize: font(9.5), height: s(15), minWidth: s(15), padding: `0 ${s(4)}px`, gap: s(2), borderRadius: s(8) }}
          onClick={(e) => { e.stopPropagation(); p.onComments?.() }}>
          <Icon name="bubble.left" size={font(9)} />{data.openComments}
        </button>
      )}
      {[...data.inputs.map((x) => [x, false] as const), ...data.outputs.map((x) => [x, true] as const)].map(([port, output]) => {
        const pt = g.port(port.name, output)
        const mark = g.shape === 'card' ? Math.max(6, s(8)) : s(8), hit = g.portHitSize(z)
        return (
          <div key={(output ? 'o:' : 'i:') + port.name} className={`cv-port${p.onPortDown ? ' connectable' : ''}`}
            style={{ left: s(pt.x) - hit / 2, top: s(pt.y) - hit / 2, width: hit, height: hit }}
            title={portHelp(port, output) + (p.onPortDown ? (output ? L('（ドラッグして入力ポートへ接続）') : L('（ドラッグして出力ポートへ接続）')) : '')}
            onPointerDown={p.onPortDown ? (e) => {
              if (e.button !== 0) return
              e.stopPropagation()
              p.onPortDown!(port.name, output, e)
            } : undefined}>
            {output ? <span className="cv-port-out" style={{ width: mark, height: mark }} /> : (
              <span className="cv-port-in"
                style={{ width: mark, height: mark, borderRadius: Math.max(1, s(1.5)) }} />
            )}
          </div>
        )
      })}
      {p.onPortDown && [false, true].map((output) => {
        const pt = g.newPort(output)
        if (!pt) return null
        const hit = Math.max(16, s(18)), mark = Math.max(6, s(8))
        return (
          <div key={output ? 'new-o' : 'new-i'} className="cv-port connectable"
            style={{ left: s(pt.x) - hit / 2, top: s(pt.y) - hit / 2, width: hit, height: hit }}
            title={output ? L('ドラッグして次の Card へ接続（出力は線に合わせて作られます）') : L('ドラッグして前の Card から接続（入力は線に合わせて作られます）')}
            onPointerDown={(e) => {
              if (e.button !== 0) return
              e.stopPropagation()
              p.onPortDown!(NEW_PORT, output, e)
            }}>
            <span className="cv-port-new" style={{ width: mark, height: mark }} />
          </div>
        )
      })}
    </div>
  )
}

function useDraftFields(edit: DraftEdit | undefined) {
  const content = useRef<HTMLTextAreaElement>(null)
  const editing = !!edit
  // A Draft's name is its id; what is written is its content.
  useEffect(() => {
    if (!editing) return
    const c = content.current
    c?.focus({ preventScroll: true })
    c?.setSelectionRange(c.value.length, c.value.length)
  }, [editing])
  const keys = (e: React.KeyboardEvent, _field: 'content') => {
    if (!edit || e.nativeEvent.isComposing) return
    if (e.key === 'Escape' || (e.key === 'Enter' && (e.metaKey || e.ctrlKey))) edit.onDone(true)
    else return
    e.preventDefault()
    e.stopPropagation()
  }
  return { content, keys }
}

const stopping = {
  onPointerDown: (e: React.PointerEvent) => e.stopPropagation(),
  onDoubleClick: (e: React.MouseEvent) => e.stopPropagation(),
}

function CardBody({ p, g }: { p: CardNodeProps; g: CardGeometry }) {
  const { data, flow } = p
  const z = p.zoom ?? 1
  const s = (v: number) => v * z
  const font = (size: number) => Math.max(6, size * z)
  const rt = runText(data)
  const edit = g.draftEditing ? p.draftEdit : undefined
  const fields = useDraftFields(edit)
  const collapse = p.onToggleCollapse && data.kind !== 'trigger' && !edit && (
    <button type="button" className="cv-collapse secondary" title={data.collapsed ? L('展開') : L('折りたたむ（実行には影響しません）')}
      onPointerDown={(e) => e.stopPropagation()} onDoubleClick={(e) => e.stopPropagation()}
      onClick={(e) => { e.stopPropagation(); p.onToggleCollapse!() }}>
      <Icon name={data.collapsed ? 'chevron.down' : 'chevron.up'} size={font(9)} />
    </button>
  )
  const permissions = data.permissions?.map((p) => (
    <Icon key={p} name={permissionInfo(p).icon} size={font(10)} title={permissionInfo(p).label}
      style={{ color: p === 'sandbox_off' ? 'var(--orange)' : 'var(--secondary)', flex: 'none' }} />
  ))
  return (
    <div className="cv-card-body" style={{ width: s(g.cardWidth) }}
      onBlur={edit ? (e) => { if (!e.currentTarget.contains(e.relatedTarget as Node | null)) edit.onDone() } : undefined}>
      <div style={{ height: s(g.topLabels) }} />
      {g.face ? <CardFace p={p} g={g} marks={<>{permissions}{data.warnings.length ? <Icon name="exclamationmark.triangle.fill" size={font(10)}
        style={{ color: 'var(--yellow)', flex: 'none' }} title={data.warnings.join('\n')} /> : null}{collapse}</>} />
        : g.untitled ? <div style={{ height: s(g.headerRow), flex: 'none' }} /> : <div className="cv-card-header" style={{ height: s(CardGeometry.header), padding: `0 ${s(9)}px`, gap: s(6) }}>
        <span className="secondary cv-center" style={{ width: s(15) }}><Icon name={data.symbol ?? 'xmark.octagon'} size={font(12)} /></span>
        <span className="cv-title" style={{ fontSize: font(12) }}>{data.untitled && !edit ? data.summary?.split('\n')[0] ?? data.title : data.title}</span>
        {!edit && <span style={{ flex: 1, minWidth: s(2) }} />}
        {permissions}
        {data.errors.length ? <Icon name="xmark.octagon.fill" size={font(11)} style={{ color: 'var(--red)' }} title={data.errors.join('\n')} />
          : data.warnings.length ? <Icon name="exclamationmark.triangle.fill" size={font(11)} style={{ color: 'var(--yellow)' }} title={data.warnings.join('\n')} /> : null}
        {data.state !== undefined && data.state !== D.CardState.not_started && (
          <span className="cv-state" style={{ fontSize: font(10), gap: s(2) }} title={rt ?? cardState(data.state).label}>
            <Icon name={cardState(data.state).icon} size={font(10)} style={{ color: colorVar(cardState(data.state).color) }} />
            {data.invocation > 0n && <span className="secondary mono-digits">#{String(data.invocation)}</span>}
          </span>
        )}
        {collapse}
      </div>}
      {!g.collapsed && (
        <>
          {g.draft && (edit ? (
            <div className="cv-draft-edit" style={{ height: s(g.draftRow), padding: `0 ${s(10)}px ${s(8)}px`, gap: s(2) }} {...stopping}>
              <textarea ref={fields.content} style={{ fontSize: font(11), lineHeight: `${s(14)}px` }} value={edit.draft.content}
                placeholder={L('何を受け取り、何を出し、どう処理するかを書きます')} aria-label={L('Draft の内容')}
                onChange={(e) => edit.onChange({ content: e.target.value })} onKeyDown={(e) => fields.keys(e, 'content')} />
            </div>
          ) : (
            <div className={`cv-draft-text secondary${data.summary ? '' : ' placeholder'}`}
              style={{ height: s(g.draftRow), padding: `0 ${s(10)}px`, fontSize: font(11), lineHeight: `${s(14)}px` }}>
              <span>{data.summary ?? L('ダブルクリックで内容を書く')}</span>
            </div>
          ))}
          {g.summary && data.summary && (
            <div className="cv-summary" title={data.summary}
              style={{ fontSize: font(10), height: s(CardGeometry.summaryRow), padding: `0 ${s(10)}px`, color: colorVar(cardState(D.CardState.failed).color) }}>{data.summary}</div>
          )}
          {flow === 'leftToRight' && <div style={{ height: s(g.portBand) }} />}
        </>
      )}
    </div>
  )
}

/** An ordinary Card: its name (two lines at most) and its status line, with
 *  the stage circles on the right. The marks sit beside the name's first line. */
function CardFace({ p, g, marks }: { p: CardNodeProps; g: CardGeometry; marks: JSX.Element }) {
  const { data } = p
  const z = p.zoom ?? 1
  const s = (v: number) => v * z
  const font = (size: number) => Math.max(6, size * z)
  const line = statusLine(data, useNow(data.state === D.CardState.succeeded))
  return (
    <div className="cv-face" style={{ height: s(CardGeometry.face), padding: `0 ${s(9)}px 0 ${s(10)}px`, gap: s(8) }}>
      <div className="cv-face-text" style={{ gap: s(1) }}>
        <div className="cv-face-name" style={{ gap: s(4) }}>
          <span className="cv-name" style={{ fontSize: font(12), lineHeight: `${s(15)}px`, maxHeight: s(30) }}>{data.title}</span>
          <span className="cv-face-marks" style={{ gap: s(4), height: s(15) }}>{marks}</span>
        </div>
        <span className="cv-ellipsis cv-status-line" title={line.hover} style={{ fontSize: font(10), lineHeight: `${s(13)}px`, color: colorVar(line.color) }}>{line.text}</span>
      </div>
      {g.stages ? <StageDots progress={data.stage && data.invocation > 0n ? data.stage : notRunStages(true)} zoom={z} onTap={p.onStageTap} waiting={data.activityWaits} />
        : data.kind === 'subgraph' ? <span className="secondary cv-center"><Icon name={data.symbol ?? 'link'} size={font(10)} /></span> : null}
    </div>
  )
}

/** Its name and a small run button; the run's state beside the name. */
function StartBody({ p, g }: { p: CardNodeProps; g: CardGeometry }) {
  const { data } = p
  const z = p.zoom ?? 1
  const s = (v: number) => v * z
  const font = (size: number) => Math.max(6, size * z)
  const o = g.outlineRect
  const running = data.state !== undefined && data.state !== D.CardState.not_started
  return (
    <div className="cv-card-body cv-start" style={{ width: s(o.width), height: s(o.height), padding: `0 ${s(7)}px`, gap: s(6) }}>
      <button type="button" className="cv-start-run" style={{ width: s(20), height: s(20) }} disabled={!p.onRun} title={L('ここから Workflow を実行')} {...stopping}
        onClick={(e) => { e.stopPropagation(); p.onRun?.() }}>
        <Icon name="play.fill" size={font(9)} />
      </button>
      <span className="cv-title" style={{ fontSize: font(12) }}>{data.title}</span>
      <span style={{ flex: 1 }} />
      {running && <Icon name={cardState(data.state!).icon} size={font(10)} style={{ color: colorVar(cardState(data.state!).color) }} />}
    </div>
  )
}

/** The Card's icon, coloured by the state; name and details on hover. */
function RoutingBody({ data, g, z }: { data: CardNodeData; g: CardGeometry; z: number }) {
  const o = g.outlineRect
  const center = g.iconCenter({ x: o.x * z, y: o.y * z, width: o.width * z, height: o.height * z })
  const color = data.state === undefined || data.state === D.CardState.not_started ? 'var(--secondary)' : colorVar(cardState(data.state).color)
  const rt = runText(data)
  const size = Math.max(6, 10 * z)
  return (
    <span className="cv-routing-icon" style={{ left: center.x - size / 2, top: center.y - size / 2, color }}
      title={data.title + ' · ' + KIND_LABEL[data.kind] + (rt ? ' · ' + rt : '')}>
      <Icon name={data.symbol ?? 'xmark.octagon'} size={size} />
    </span>
  )
}

/** Port names centred on their ports; hidden when there is no room (the whole name and type then show on hover). */
function PortLabels({ data, g, flow, z }: { data: CardNodeData; g: CardGeometry; flow: FlowDirection; z: number }) {
  const s = (v: number) => v * z
  const font = (size: number) => Math.max(6, size * z)
  const out: JSX.Element[] = []
  if (flow === 'topToBottom') {
    for (const [list, output] of [[data.inputs, false], [data.outputs, true]] as const) {
      if (!g.showLabels(!output)) continue
      for (const p of list) {
        const pt = g.port(p.name, output)
        const w = g.spacing(!output)
        let style: CSSProperties
        if (g.shape === 'card') {
          // Inside an ordinary Card: below its input, above its output.
          const half = CardGeometry.portRow / 2 + 0.5
          style = { left: s(pt.x - w / 2), top: s(pt.y + (output ? -half : half)), width: s(w), textAlign: 'center' }
        } else {
          // Beside the port of a routing Card, so the wire (vertical at the port) never runs through the name.
          const left = pt.x < g.outlineRect.x + g.outlineRect.width / 2 - 1 && g.spacing(!output) < 70
          const lw = Math.max(CardGeometry.minLabel, Math.min(w, 80))
          const dy = output ? CardGeometry.portRow / 2 : -CardGeometry.portRow / 2
          const x = left ? pt.x - 6 - lw : pt.x + 6
          style = { left: s(x), top: s(pt.y + dy), width: s(lw), textAlign: left ? 'right' : 'left' }
        }
        out.push(<span key={(output ? 'o:' : 'i:') + p.name} className="cv-port-label secondary" style={{ ...style, fontSize: font(9.5) }}>{p.name}</span>)
      }
    }
  } else if (g.shape === 'card' && !g.collapsed) {
    const w = g.cardWidth / 2 - 14
    if (g.draft) return null
    for (const p of data.inputs) {
      const pt = g.port(p.name, false)
      out.push(<span key={'i:' + p.name} className="cv-port-label secondary" style={{ left: s(pt.x + 10), top: s(pt.y), width: s(w), fontSize: font(10) }}>{p.name}</span>)
    }
    for (const p of data.outputs) {
      const pt = g.port(p.name, true)
      out.push(<span key={'o:' + p.name} className="cv-port-label secondary" style={{ left: s(pt.x - 10 - w), top: s(pt.y), width: s(w), textAlign: 'right', fontSize: font(10) }}>{p.name}</span>)
    }
  }
  return <>{out}</>
}

function portHelp(p: PortData, output: boolean): string {
  let s = (output ? L('出力 ') : L('入力 ')) + p.name
  if (p.typeName) s += ` : ${p.typeName}`
  return s
}

// ---- Stage circles -----------------------------------------------------------------------

export const STAGE_COLOR: Record<StageStatus, string> = {
  pending: 'secondary', notRun: 'secondary', running: 'orange', done: 'green',
  cached: 'blue', reused: 'teal', failed: 'red', cancelled: 'brown',
}

export function stageHelp(item: StageItem, tappable: boolean, waiting = false): string {
  let t = STAGE_INFO[item.stage].label + L('：') + SECTION_STATUS_LABEL[sectionStatus(item.status, waiting)]
  if (item.seconds !== undefined) t += ' · ' + formatDuration(item.seconds)
  if (item.status === 'failed' || item.status === 'cancelled') t += L('（クリックでこの段階のログ）')
  else if (tappable && item.status !== 'pending') t += L('（クリックでログ）')
  return t
}

/** One stage circle: an empty ring before it is reached, a spinning arc while
 *  it runs, a dashed ring while it waits (for the worker), filled once it ended. */
export function StageDot({ status, size, waiting = false }: { status: StageStatus; size: number; waiting?: boolean }) {
  const c = `var(--${STAGE_COLOR[status]}${status === 'reused' ? ', #30b0c7' : ''})`
  const line = Math.max(0.8, size * 0.15)
  const style: CSSProperties = { width: size, height: size }
  if (status === 'running' && waiting) return <span className="cv-dot cv-dot-waiting" style={{ ...style, borderWidth: line * 1.1, borderColor: c }} />
  if (status === 'pending' || status === 'notRun') return <span className="cv-dot" style={{ ...style, borderWidth: line, borderColor: fade('secondary', 0.6) }} />
  if (status === 'running') {
    return <span className="cv-dot cv-dot-running" style={{ ...style, borderWidth: line * 1.2, borderColor: fade('orange', 0.25), borderTopColor: c, borderRightColor: c }} />
  }
  return <span className="cv-dot filled" style={{ ...style, background: c }} />
}

/** Stages of the latest invocation as small circles stacked top to bottom
 *  (build, run, validate) on a thin line: a timeline. Colour and fill say how
 *  each went; a click opens that stage in the log. */
export function StageDots({ progress, zoom = 1, onTap, waiting = false }: {
  progress: StageProgress; zoom?: number; onTap?: (s: Stage) => void
  /** The running execution waits for its worker. */
  waiting?: boolean
}) {
  const s = (v: number) => v * zoom
  const n = progress.items.length
  return (
    <div className="cv-dots" style={{ gap: s(4) }}>
      <span className="cv-dots-line" style={{ width: Math.max(0.5, s(1)), height: s((n - 1) * 13) }} aria-hidden />
      {progress.items.map((item) => {
        const waits = waiting && item.stage === Stage.execute
        return (
          <button key={item.stage} type="button" className="cv-stage" disabled={!onTap} title={stageHelp(item, !!onTap, waits)}
            aria-label={STAGE_INFO[item.stage].label + L('：') + SECTION_STATUS_LABEL[sectionStatus(item.status, waits)]}
            onPointerDown={(e) => onTap && e.stopPropagation()} onDoubleClick={(e) => e.stopPropagation()}
            onClick={(e) => { e.stopPropagation(); onTap?.(item.stage) }}>
            <StageDot status={item.status} size={s(9)} waiting={waits} />
          </button>
        )
      })}
    </div>
  )
}

// ---- Wires ------------------------------------------------------------------------------------

export interface WireDraw {
  drawProgress?: number
  /** Screen units. */
  d: string
  style: WireStyle
  /** A wire to or from a Draft is dotted, whatever its style. */
  dotted?: boolean
  executionList?: boolean
  countLabel?: string
  countAt?: Pt
}

/** A Draft's wires: round dots, spaced so that they flow without a jump at the
 *  phase of the active wires' dashes (a whole number per period, cv-dash), and
 *  never smaller than can be seen on a Canvas zoomed out. */
const WIRE_DOT = `0 ${14 / 3}`
/** A selected wire is thicker: further apart, its dots still read as dots. */
const WIRE_DOT_SELECTED = `0 ${14 / 2}`
const WIRE_DOT_MIN = 2.2

/** Wires drawn at screen scale; active wires move while a run is going on. */
export function WireLayer({ wires, animate = false, zoom = 1, width, height }: { wires: WireDraw[]; animate?: boolean; zoom?: number; width: number; height: number }) {
  const anyActive = animate && wires.some((w) => w.style === 'active')
  const w = Math.max(1, 1.6 * Math.min(1.4, zoom))
  const stroke: Record<WireStyle, { color: string; width: number; dash?: string }> = {
    normal: { color: fade('secondary', 0.75), width: w },
    carried: { color: fade('orange', 0.8), width: w + 0.4 },
    active: { color: 'var(--orange)', width: w + 0.6, dash: anyActive ? '8 6' : undefined },
    selected: { color: 'var(--accent)', width: w + 1.4 },
  }
  return (
    <svg className="cv-wires" width={Math.max(1, width)} height={Math.max(1, height)} aria-hidden>
      {wires.map((wire, i) => {
        const st = stroke[wire.style]
        const thick = st.width + (wire.executionList ? 2.8 : 0)
        return (
          <g key={i}>
            <path d={wire.d} fill="none" stroke={st.color} strokeWidth={wire.dotted ? Math.max(thick, WIRE_DOT_MIN) : thick} strokeLinecap="round" strokeLinejoin="round"
              pathLength={wire.drawProgress === undefined ? undefined : 1}
              strokeDasharray={wire.drawProgress !== undefined ? `${wire.drawProgress} 1` : wire.dotted ? (wire.style === 'selected' ? WIRE_DOT_SELECTED : WIRE_DOT) : st.dash} className={wire.style === 'active' && anyActive ? 'cv-wire-moving' : undefined} />
            {wire.countLabel && wire.countAt && <text x={wire.countAt.x} y={wire.countAt.y - 7} textAnchor="middle"
              fill="var(--label)" stroke="var(--window-bg)" strokeWidth={3} paintOrder="stroke" className="mono"
              fontSize={11}>{wire.countLabel}</text>}
          </g>
        )
      })}
    </svg>
  )
}

/** Dot grid behind a canvas. */
export function gridStyle(camera: Camera, show: boolean, size: number): CSSProperties {
  const step = size * camera.zoom
  if (!show || step < 6) return {}
  const mod = (v: number) => ((v % step) + step) % step
  return {
    backgroundImage: 'radial-gradient(circle at 0.8px 0.8px, var(--grid-dot) 0.8px, transparent 1px)',
    backgroundSize: `${step}px ${step}px`,
    backgroundPosition: `${mod(camera.x)}px ${mod(camera.y)}px`,
  }
}

/** Follows a pointer from `e` until it is released: `move` once it moved past
 *  `threshold` pixels, `end` with whether it did. */
export function drive(e: React.PointerEvent | PointerEvent, threshold: number, move: (ev: PointerEvent) => void, end?: (ev: PointerEvent, moved: boolean) => void) {
  const sx = e.clientX, sy = e.clientY
  let moved = false
  const onMove = (ev: PointerEvent) => {
    if (!moved && Math.hypot(ev.clientX - sx, ev.clientY - sy) < threshold) return
    moved = true
    move(ev)
  }
  const onUp = (ev: PointerEvent) => {
    window.removeEventListener('pointermove', onMove)
    window.removeEventListener('pointerup', onUp)
    window.removeEventListener('pointercancel', onUp)
    end?.(ev, moved)
  }
  window.addEventListener('pointermove', onMove)
  window.addEventListener('pointerup', onUp)
  window.addEventListener('pointercancel', onUp)
}

// ---- Card in Card boundary -------------------------------------------------------------------

/** A bar standing for the outer Card's inputs (above) or outputs (below)
 *  inside a Card in Card: public name, type and the inner port it maps to. */
export function BoundaryBarView({ title, ports, input, zoom, rect, style, cardTitle, onPointerDown }: {
  title: string; ports: BoundaryPort[]; input: boolean; zoom: number; rect: Rect; style: CSSProperties; cardTitle: (card: CardKey) => string; onPointerDown: (e: React.PointerEvent) => void
}) {
  const mark = Math.max(6, 8 * zoom)
  return (
    <div className="cv-boundary" style={{ ...style, borderRadius: 8 * zoom }} title={L('ドラッグで移動できます')} onPointerDown={onPointerDown}
      aria-label={title + ': ' + ports.map((p) => p.name).join(', ')}>
      <span className="cv-boundary-title" style={{ fontSize: Math.max(6, 9.5 * zoom), left: 8 * zoom, top: 3 * zoom, gap: 4 * zoom }}>
        <Icon name={input ? 'arrow.down.to.line' : 'arrow.down.to.line.compact'} size={Math.max(6, 9.5 * zoom)} />{title}
      </span>
      {ports.map((p, i) => {
        const x = (p.point.x - rect.x) * zoom
        return (
          <span key={i}>
            <span className="cv-boundary-port" style={{ left: x - 52 * zoom, width: 104 * zoom, top: (rect.height * zoom) / 2 + 5 * zoom }}
              title={p.inner ? L('内部の接続先: {0}.{1}', cardTitle(p.inner[0]), p.inner[1]) : L('内部の接続先が未設定です')}>
              <span style={{ fontSize: Math.max(6, 10 * zoom), fontWeight: 500 }}>{p.name}</span>
              {p.type && <span className="mono secondary" style={{ fontSize: Math.max(5, 8 * zoom) }}>{p.type}</span>}
            </span>
            <span className={input ? 'cv-boundary-dot in' : 'cv-boundary-dot out'}
              style={{ left: x - mark / 2, top: (input ? rect.height * zoom : 0) - mark / 2, width: mark, height: mark }} />
          </span>
        )
      })}
    </div>
  )
}
