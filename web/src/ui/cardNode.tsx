// A Card as drawn on the main canvas, in Card settings and in the demo
// player, at the screen scale `zoom`:
// fonts and spacing are multiplied by the zoom, so text stays sharp at any
// zoom and the hit areas are the geometry's. Also the wire layer.
import { CSSProperties, useEffect, useRef, useState } from 'react'
import {
  Camera, CardGeometry, CardNodeData, currentStage, isRouting, ParamData, PortData, Pt, Rect, Stage, STAGE_INFO,
  NEW_PORT, STAGE_STATUS_LABEL, StageItem, StageProgress, StageStatus,
} from '../model/canvasKit'
import { BoundaryPort, WireStyle } from '../model/canvas'
import type { FlowDirection } from '../model/prefs'
import { cardState, formatDuration, InlineDraft, KIND_ICON, KIND_LABEL } from '../model/session'
import { D } from '../shared/rpc'
import { Icon } from './icons'
import { permissionInfo } from '../model/permissions'
import { L } from '../i18n'

// ---- Texts ------------------------------------------------------------------------

/** "Invocation #3 · Succeeded" and the like; undefined before the first invocation. */
export function runText(d: CardNodeData): string | undefined {
  if (d.executionList && d.state !== undefined) return cardState(d.state).label
  if (d.state === undefined || d.invocation <= 0n) return undefined
  let s = L('実行 #{0}', d.invocation)
  const cur = currentStage(d.stage)
  if (cur && (d.state === D.CardState.running || d.state === D.CardState.preparing)) {
    s += ' · ' + STAGE_INFO[cur.stage].active
  } else {
    s += ' · ' + cardState(d.state).label
    if (d.state === D.CardState.succeeded && d.runContinues) s += L('（次の入力を待てます）')
  }
  return s
}

/** Plain status for a Card that did not run in this run. */
export function idleText(d: CardNodeData): string {
  if (d.kind === 'draft') return L('未実装（Draft）')
  if (d.state !== undefined && d.state !== D.CardState.not_started) return cardState(d.state).label
  return L('未実行（0 回）')
}

const colorVar = (c: string) => `var(--${c})`
const fade = (c: string, opacity: number) => `color-mix(in srgb, ${colorVar(c)} ${Math.round(opacity * 100)}%, transparent)`

// ---- Card view -----------------------------------------------------------------------

/** Writing a Draft on its Card: what is typed so far, where it goes, and how it ends. */
export interface DraftEdit {
  draft: InlineDraft
  /** Why the Card cannot take the name typed so far. */
  nameProblem?: string
  onChange: (change: Partial<Pick<InlineDraft, 'name' | 'content'>>) => void
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
  /** Committing an input or Switch Card's param (undefined: read-only). */
  onParamCommit?: (name: string, text: string) => void
  paramsEditable?: boolean
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
  const switched = data.kind === 'switch' && data.params.find(param => param.name === 'value')?.value === 'true'
  const fill = switched ? '#0f5cbf' : isRouting(data) ? 'var(--window-bg)' : 'var(--control-bg)'
  const border = selected ? 'var(--accent)' : data.state !== undefined && data.state !== D.CardState.not_started
    ? fade(cardState(data.state).color, 0.7) : fade('secondary', draft ? 0.7 : 0.4)
  const w = s(g.size.width), h = s(g.size.height)
  const parts = [`Card ${data.title}`, KIND_LABEL[data.kind], runText(data) ?? idleText(data)]
  if (data.summary) parts.push(data.summary)
  if (data.errors.length) parts.push(data.errors.join(' '))

  return (
    <div className={`cv-card${data.kind === 'switch' ? ' cv-card-switch' : ''} ${p.className ?? ''}`} style={{ width: w, height: h, ...p.style }} data-guide={p.guide} role="group" aria-label={parts.join(', ')}
      onPointerDown={p.onPointerDown} onDoubleClick={p.onDoubleClick} onContextMenu={p.onContextMenu}>
      <svg className="cv-card-fill" width={w} height={h}>
        <path d={outline} fill={fill} style={{ filter: `drop-shadow(0 1px ${selected ? 4 : 1.5}px rgba(0,0,0,${selected ? 0.2 : 0.07}))` }} />
      </svg>
      {g.shape === 'switch' ? <SwitchBody p={p} g={g} /> : g.shape === 'card' ? <CardBody p={p} g={g} /> : g.shape === 'start' ? <StartBody p={p} g={g} /> : <RoutingBody data={data} g={g} z={z} />}
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
        const mark = Math.max(6, s(8)), hit = Math.max(16, s(18))
        const condition = !output && data.kind === 'branch' && port.name === 'condition'
        return (
          <div key={(output ? 'o:' : 'i:') + port.name} className={`cv-port${p.onPortDown ? ' connectable' : ''}`}
            style={{ left: s(pt.x) - hit / 2, top: s(pt.y) - hit / 2, width: hit, height: hit }}
            title={portHelp(data, port, output) + (p.onPortDown ? (output ? L('（ドラッグして入力ポートへ接続）') : L('（ドラッグして出力ポートへ接続）')) : '')}
            onPointerDown={p.onPortDown ? (e) => {
              if (e.button !== 0) return
              e.stopPropagation()
              p.onPortDown!(port.name, output, e)
            } : undefined}>
            {output ? <span className="cv-port-out" style={{ width: mark, height: mark, ...(data.kind === 'switch' ? { boxShadow: `0 0 0 ${s(1)}px #fff` } : {}) }} /> : (
              <span className={`cv-port-in${condition ? ' condition' : ''}`}
                style={{ width: mark, height: mark, borderRadius: Math.max(1, s(1.5)), transform: condition ? 'rotate(45deg)' : undefined }} />
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

/** The whole Card is the switch track. Dragging and selection modifiers still
 *  belong to the canvas; only an ordinary click or keyboard activation toggles. */
function SwitchBody({ p, g }: { p: CardNodeProps; g: CardGeometry }) {
  const z = p.zoom ?? 1
  const on = p.data.params.find(param => param.name === 'value')?.value === 'true'
  const moved = useRef(false)
  const pointerStarted = useRef(false)
  return (
    <button type="button" role="switch" className="cv-switch-card-control" aria-label={p.data.title} aria-checked={on}
      disabled={!p.paramsEditable || !p.onParamCommit} title={`${p.data.title} · value:bool = ${on}`}
      style={{ width: g.size.width * z, height: g.size.height * z, borderRadius: g.size.height * z / 2, color: on ? '#fff' : 'var(--label)' }}
      onPointerDown={event => {
        moved.current = false
        pointerStarted.current = event.button === 0
        if (event.button === 0) drive(event, 3, () => { moved.current = true }, event => {
          if (event.type === 'pointercancel') pointerStarted.current = false
        })
      }}
      onDoubleClick={event => event.stopPropagation()}
      onKeyDown={event => { if (event.key === ' ' || event.key === 'Enter') event.stopPropagation() }}
      onKeyUp={event => { if (event.key === ' ' || event.key === 'Enter') event.stopPropagation() }}
      onClick={event => {
        event.stopPropagation()
        const activate = event.detail === 0 || (pointerStarted.current && !moved.current)
        pointerStarted.current = false
        if (!activate || event.shiftKey || event.metaKey || event.ctrlKey || event.altKey) return
        p.onParamCommit?.('value', on ? 'false' : 'true')
      }}>
      <span className="cv-switch-card-knob" aria-hidden style={{ left: 6 * z, top: (g.size.height - 26) * z / 2, width: 26 * z, height: 26 * z, transform: `translateX(${on ? (g.size.width - 38) * z : 0}px)` }} />
      <span className="cv-switch-card-title cv-title" style={{ left: 6 * z, width: (g.size.width - 42) * z, fontSize: Math.max(6, 12 * z), transform: `translateX(${on ? 0 : 30 * z}px)` }}>{p.data.title}</span>
    </button>
  )
}

/** A Draft's name in its header and its content below, written on the Card. */
function useDraftFields(edit: DraftEdit | undefined) {
  const name = useRef<HTMLInputElement>(null)
  const content = useRef<HTMLTextAreaElement>(null)
  const editing = !!edit
  // The content is what is written first: a name is not needed.
  useEffect(() => {
    if (!editing) return
    const c = content.current
    c?.focus({ preventScroll: true })
    c?.setSelectionRange(c.value.length, c.value.length)
  }, [editing])
  const keys = (e: React.KeyboardEvent, field: 'name' | 'content') => {
    if (!edit || e.nativeEvent.isComposing) return
    if (e.key === 'Escape' || (e.key === 'Enter' && (e.metaKey || e.ctrlKey))) edit.onDone(true)
    else if (field === 'name' && e.key === 'Enter') {
      if (!edit.nameProblem) content.current?.focus({ preventScroll: true })
    } else return
    e.preventDefault()
    e.stopPropagation()
  }
  return { name, content, keys }
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
  return (
    <div className="cv-card-body" style={{ width: s(g.cardWidth) }}
      onBlur={edit ? (e) => { if (!e.currentTarget.contains(e.relatedTarget as Node | null)) edit.onDone() } : undefined}>
      <div style={{ height: s(g.topLabels) }} />
      {g.untitled ? <div style={{ height: s(g.headerRow), flex: 'none' }} /> : <div className="cv-card-header" style={{ height: s(CardGeometry.header), padding: `0 ${s(9)}px`, gap: s(6) }}>
        <span className="secondary cv-center" style={{ width: s(15) }}><Icon name={data.symbol ?? KIND_ICON[data.kind]} size={font(12)} /></span>
        {edit
          ? <input ref={fields.name} className="cv-draft-name" style={{ fontSize: font(12) }} value={edit.draft.name} placeholder={L('名前')} spellCheck={false}
              aria-label={L('Draft の名前')} aria-invalid={!!edit.nameProblem} {...stopping}
              onChange={(e) => edit.onChange({ name: e.target.value })} onKeyDown={(e) => fields.keys(e, 'name')} />
          : <span className="cv-title" style={{ fontSize: font(12) }}>{data.untitled ? data.summary?.split('\n')[0] ?? data.title : data.title}</span>}
        {!edit && <span style={{ flex: 1, minWidth: s(2) }} />}
        {data.permissions?.map((p) => (
          <Icon key={p} name={permissionInfo(p).icon} size={font(10)} title={permissionInfo(p).label}
            style={{ color: p === 'sandbox_off' ? 'var(--orange)' : 'var(--secondary)' }} />
        ))}
        {data.errors.length ? <Icon name="xmark.octagon.fill" size={font(11)} style={{ color: 'var(--red)' }} title={data.errors.join('\n')} />
          : data.warnings.length ? <Icon name="exclamationmark.triangle.fill" size={font(11)} style={{ color: 'var(--yellow)' }} title={data.warnings.join('\n')} /> : null}
        {data.state !== undefined && data.state !== D.CardState.not_started && (
          <span className="cv-state" style={{ fontSize: font(10), gap: s(2) }} title={rt ?? cardState(data.state).label}>
            <Icon name={cardState(data.state).icon} size={font(10)} style={{ color: colorVar(cardState(data.state).color) }} />
            {data.invocation > 0n && <span className="secondary mono-digits">#{String(data.invocation)}</span>}
          </span>
        )}
        {p.onToggleCollapse && data.kind !== 'trigger' && !edit && (
          <button type="button" className="cv-collapse secondary" title={data.collapsed ? L('展開') : L('折りたたむ（実行には影響しません）')}
            onPointerDown={(e) => e.stopPropagation()} onDoubleClick={(e) => e.stopPropagation()}
            onClick={(e) => { e.stopPropagation(); p.onToggleCollapse!() }}>
            <Icon name={data.collapsed ? 'chevron.down' : 'chevron.up'} size={font(9)} />
          </button>
        )}
      </div>}
      {!g.collapsed && (
        <>
          {g.draft && (edit ? (
            <div className="cv-draft-edit" style={{ height: s(g.draftRow), padding: `0 ${s(10)}px ${s(8)}px`, gap: s(2) }} {...stopping}>
              {edit.nameProblem && <span className="cv-draft-problem" role="alert" style={{ fontSize: font(9.5) }}>{edit.nameProblem}</span>}
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
            <div className="cv-summary secondary" style={{ fontSize: font(10), height: s(CardGeometry.summaryRow), padding: `0 ${s(10)}px` }}>{data.summary}</div>
          )}
          {flow === 'leftToRight' && <div style={{ height: s(g.portBand) }} />}
          {data.params.map((param) => (
            <div key={param.name} className="cv-param-row" style={{ height: s(CardGeometry.paramRow), padding: `0 ${s(10)}px` }}>
              <ParamFieldView param={param} editable={!!p.onParamCommit && p.paramsEditable !== false} zoom={z}
                commit={(t) => p.onParamCommit?.(param.name, t)} />
            </div>
          ))}
          {g.result && (
            <div className="cv-result secondary" style={{ height: s(CardGeometry.resultRow), padding: `0 ${s(10)}px`, gap: s(4) }}>
              <Icon name="text.below.photo" size={font(9)} />
              <span className={data.result !== undefined ? 'mono' : ''} style={{ fontSize: font(10) }}>{data.result ?? L('結果はまだありません')}</span>
            </div>
          )}
          {g.stages && (
            <div className="cv-stage-row" style={{ height: s(CardGeometry.stageRow), padding: `0 ${s(8)}px` }}>
              {data.stage && data.invocation > 0n
                ? <StageStrip progress={data.stage} zoom={z} onTap={p.onStageTap} caption={data.executionList ? L('実行 #{0}', data.invocation) : rt ?? ''} />
                : <span className="secondary cv-ellipsis" style={{ fontSize: font(10) }}>{idleText(data)}</span>}
            </div>
          )}
        </>
      )}
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

/** Only the kind's icon, coloured by the state; name and details on hover. */
function RoutingBody({ data, g, z }: { data: CardNodeData; g: CardGeometry; z: number }) {
  const o = g.outlineRect
  const center = g.iconCenter({ x: o.x * z, y: o.y * z, width: o.width * z, height: o.height * z })
  const color = data.state === undefined || data.state === D.CardState.not_started ? 'var(--secondary)' : colorVar(cardState(data.state).color)
  const rt = runText(data)
  const size = Math.max(6, 10 * z)
  return (
    <span className="cv-routing-icon" style={{ left: center.x - size / 2, top: center.y - size / 2, color }}
      title={data.title + ' · ' + KIND_LABEL[data.kind] + (rt ? ' · ' + rt : '')}>
      <Icon name={KIND_ICON[data.kind]} size={size} />
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
        let text = p.name
        if (data.kind === 'branch') {
          // Which output is true / false, and which input is the boolean condition.
          if (output) text = p.name === 'a' ? 'a (T)' : p.name === 'b' ? 'b (F)' : p.name
          else if (p.name === 'condition') text = 'bool'
        }
        let style: CSSProperties
        if (g.shape === 'card') {
          // Inside an ordinary Card: below its input, above its output.
          const half = CardGeometry.portRow / 2 + 0.5
          style = { left: s(pt.x - w / 2), top: s(pt.y + (output ? -half : half)), width: s(w), textAlign: 'center' }
        } else {
          // Beside the port of a routing Card, so the wire (vertical at the port) never runs through the name.
          const left = pt.x < g.outlineRect.x + g.outlineRect.width / 2 - 1 && (data.kind === 'branch' || g.spacing(!output) < 70)
          const lw = data.kind === 'branch' ? 40 : Math.max(CardGeometry.minLabel, Math.min(w, 80))
          const dy = output ? CardGeometry.portRow / 2 : -CardGeometry.portRow / 2
          const x = left ? pt.x - 6 - lw : pt.x + 6
          style = { left: s(x), top: s(pt.y + dy), width: s(lw), textAlign: left ? 'right' : 'left' }
        }
        out.push(<span key={(output ? 'o:' : 'i:') + p.name} className="cv-port-label secondary" style={{ ...style, fontSize: font(9.5) }}>{text}</span>)
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

function portHelp(data: CardNodeData, p: PortData, output: boolean): string {
  let s = (output ? L('出力 ') : L('入力 ')) + p.name
  if (data.kind === 'branch') {
    if (p.name === 'condition') s += L('（bool: true なら a、false なら b へ value を送る）')
    if (p.name === 'a') s += L('（condition が true のとき）')
    if (p.name === 'b') s += L('（condition が false のとき）')
  }
  if (p.typeName) s += ` : ${p.typeName}`
  if (data.kind === 'switch') s += ' { value: bool }'
  s += p.optional ? L('（省略可）') : L('（必須）')
  if (p.note) s += '\n' + p.note
  return s
}

// ---- Stage strip -----------------------------------------------------------------------

const STAGE_COLOR: Record<StageStatus, string> = {
  pending: fade('secondary', 0.45), notRun: fade('secondary', 0.45), running: 'var(--orange)', done: 'var(--green)',
  cached: 'var(--blue)', reused: 'var(--teal, #30b0c7)', failed: 'var(--red)', cancelled: 'var(--brown)',
}
const STAGE_BADGE: Partial<Record<StageStatus, string>> = {
  done: 'checkmark', cached: 'bolt.fill', reused: 'arrow.triangle.2.circlepath', failed: 'xmark', cancelled: 'stop.fill',
}

/** Stages of the latest invocation in their order. Every stage keeps its own
 *  icon; color and a small badge say whether it is pending, running, done,
 *  cached, reused or failed. The running stage is also named in words. */
export function StageStrip({ progress, zoom = 1, onTap, caption = '' }: { progress: StageProgress; zoom?: number; onTap?: (s: Stage) => void; caption?: string }) {
  const s = (v: number) => v * zoom
  const help = (item: StageItem) => {
    let t = STAGE_INFO[item.stage].label + L('：') + STAGE_STATUS_LABEL[item.status]
    if (item.seconds !== undefined) t += ' · ' + formatDuration(item.seconds)
    if (item.status === 'failed' || item.status === 'cancelled') t += L('（クリックでこの段階のログ）')
    else if (onTap && item.status !== 'pending') t += L('（クリックでログ）')
    return t
  }
  return (
    <div className="cv-stages" style={{ gap: s(2) }}>
      {progress.items.flatMap((item, i) => {
        const c = STAGE_COLOR[item.status]
        const badge = STAGE_BADGE[item.status]
        const info = STAGE_INFO[item.stage]
        return [
          ...(i > 0 ? [<span key={'a' + i} className="cv-stage-arrow" aria-hidden><Icon name="arrow.right" size={Math.max(4, s(6))} /></span>] : []),
          <button key={item.stage} type="button" className="cv-stage" disabled={!onTap} title={help(item)}
            aria-label={info.label + L('：') + STAGE_STATUS_LABEL[item.status]}
            style={{ width: s(15), height: s(15), color: c, background: item.status === 'running' ? fade('orange', 0.15) : undefined }}
            onPointerDown={(e) => onTap && e.stopPropagation()} onDoubleClick={(e) => e.stopPropagation()}
            onClick={(e) => { e.stopPropagation(); onTap?.(item.stage) }}>
            <Icon name={item.status === 'running' ? info.activeIcon : info.icon} size={Math.max(6, s(10))} />
            {badge && (
              <span className="cv-stage-badge" style={{ background: c, right: -s(2), bottom: -s(2), padding: Math.max(0.5, s(1)) }}>
                <Icon name={badge} size={Math.max(4, s(5))} />
              </span>
            )}
          </button>,
        ]
      })}
      {progress.workerReused && (
        <span title={L('前回の worker を再利用して実行しました')} style={{ color: 'var(--teal, #30b0c7)', display: 'inline-flex' }}>
          <Icon name="arrow.triangle.2.circlepath" size={Math.max(6, s(8))} />
        </span>
      )}
      <span className="secondary cv-ellipsis" style={{ fontSize: Math.max(6, s(9.5)), flex: 1, minWidth: 0 }}>{caption}</span>
    </div>
  )
}

// ---- Param field --------------------------------------------------------------------------

/** A param shown on an input Card; committed on Return or focus loss. */
export function ParamFieldView({ param, editable, zoom = 1, commit }: { param: ParamData; editable: boolean; zoom?: number; commit: (text: string) => void }) {
  const z = zoom
  const [text, setText] = useState(param.value)
  const [editing, setEditing] = useState(false)
  useEffect(() => {
    if (!editing) setText(param.value)
  }, [param.value, editing])
  const finish = (cancel = false) => {
    if (!editing) return
    setEditing(false)
    if (cancel) setText(param.value)
    else if (text !== param.value) commit(text)
  }
  return (
    <div className="cv-param" style={{ gap: 6 * z }}>
      <span className="secondary cv-ellipsis" style={{ fontSize: Math.max(6, 10 * z), flex: 'none', maxWidth: '45%' }}>{param.name}</span>
      {param.type === 'bool' ? (
        <label className="cv-param-bool" title={editable ? undefined : L('実行中は変更できません')}
          onPointerDown={(e) => e.stopPropagation()} onDoubleClick={(e) => e.stopPropagation()}>
          <input type="checkbox" checked={param.value === 'true'} disabled={!editable} onChange={(e) => commit(e.target.checked ? 'true' : 'false')} />
          <span className="cv-switch" style={{ width: 22 * z, height: 13 * z }} />
        </label>
      ) : (
        // The value is drawn as text at the zoom; the field exists only while editing.
        <div className={`cv-param-field${editing ? ' editing' : ''}`}
          style={{ height: 19 * z, padding: `0 ${4 * z}px`, borderRadius: 4 * z, fontSize: Math.max(6, 11 * z) }}
          title={editable ? L('{0}・クリックして編集、Return で反映', param.type) : L('実行中は変更できません')}
          onClick={(e) => {
            if (!editable || editing) return
            e.stopPropagation()
            setEditing(true)
          }}
          onDoubleClick={(e) => e.stopPropagation()}>
          {editing ? (
            <input className="cv-param-input mono" autoFocus value={text} spellCheck={false}
              onPointerDown={(e) => e.stopPropagation()}
              onChange={(e) => setText(e.target.value)}
              onBlur={() => finish()}
              onKeyDown={(e) => {
                e.stopPropagation()
                if (e.key === 'Enter' && !e.nativeEvent.isComposing) finish()
                if (e.key === 'Escape') finish(true)
              }} />
          ) : <span className="mono cv-ellipsis">{text || ' '}</span>}
        </div>
      )}
    </div>
  )
}

// ---- Wires ------------------------------------------------------------------------------------

export interface WireDraw {
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
    display: { color: fade('purple', 0.75), width: Math.max(1, w - 0.2), dash: '5 4' },
  }
  return (
    <svg className="cv-wires" width={Math.max(1, width)} height={Math.max(1, height)} aria-hidden>
      {wires.map((wire, i) => {
        const st = stroke[wire.style]
        const thick = st.width + (wire.executionList ? 2.8 : 0)
        return (
          <g key={i}>
            <path d={wire.d} fill="none" stroke={st.color} strokeWidth={wire.dotted ? Math.max(thick, WIRE_DOT_MIN) : thick} strokeLinecap="round" strokeLinejoin="round"
              strokeDasharray={wire.dotted ? (wire.style === 'selected' ? WIRE_DOT_SELECTED : WIRE_DOT) : st.dash} className={wire.style === 'active' && anyActive ? 'cv-wire-moving' : undefined} />
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
export function BoundaryBarView({ title, ports, input, zoom, rect, style, onPointerDown }: {
  title: string; ports: BoundaryPort[]; input: boolean; zoom: number; rect: Rect; style: CSSProperties; onPointerDown: (e: React.PointerEvent) => void
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
              title={p.inner ? L('内部の接続先: {0}.{1}', p.inner[0].split('/').pop(), p.inner[1]) : L('内部の接続先が未設定です')}>
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
