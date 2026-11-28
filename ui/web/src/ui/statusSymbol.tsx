// The state of a stage as the Card's stage circle (StageDot): the Card's
// face, the log view's pipeline line and sections, the hover's stage table
// and the Card state marks use the same shapes and palette.
import { CSSProperties } from 'react'
import { StageStatus } from '../model/canvasKit'
import { cardState } from '../model/session'
import { D } from '../wire/rpc'
import { Icon } from './icons'
import { L } from '../i18n'

export type SectionStatus = 'pending' | 'waiting' | 'running' | 'done' | 'cached' | 'reused' | 'failed' | 'cancelled' | 'skipped' | 'blocked'

export const SECTION_STATUS_LABEL: Record<SectionStatus, string> = {
  pending: L('未開始'), waiting: L('待機中'), running: L('進行中'), done: L('完了'),
  cached: L('キャッシュを利用'), reused: L('再利用'), failed: L('失敗'), cancelled: L('停止'),
  skipped: L('スキップ'), blocked: L('ブロック'),
}

/** A stage strip status as the log view shows it. */
export function sectionStatus(status: StageStatus, waiting = false): SectionStatus {
  if (status === 'notRun') return 'pending'
  if (status === 'running' && waiting) return 'waiting'
  return status
}

/** The stage circle's status that draws a section state (waiting is a running circle that waits). */
export const sectionStage = (status: SectionStatus): StageStatus => (status === 'waiting' ? 'running' : status)

/** The stage states' colours, one set for the Card's circles, the log's
 *  pipeline line and sections and the hover's stage table (theme colours,
 *  styles.css): Okabe–Ito, safe for colour-blind readers. Done is blue,
 *  cached and reused a lighter sky blue, running and waiting orange, stopped
 *  grey; failed and blocked a deep red the owner chose by eye against the
 *  running orange and the done blue (#C8102E in both themes). Shapes and
 *  marks tell them apart as well. The same set as StagePalette in the Mac app. */
export const STAGE_COLOR: Record<SectionStatus, string> = {
  pending: 'secondary', waiting: 'stage-run', running: 'stage-run', done: 'stage-done',
  cached: 'stage-reuse', reused: 'stage-reuse', failed: 'stage-failed', cancelled: 'stage-cancelled',
  skipped: 'secondary', blocked: 'stage-failed',
}

const colorVar = (c: string) => `var(--${c})`
const fade = (c: string, opacity: number) => `color-mix(in srgb, ${colorVar(c)} ${Math.round(opacity * 100)}%, transparent)`

/** One stage circle: an empty ring before it is reached, a spinning arc while
 *  it runs, a dashed ring while it waits (for the worker). Once it ended a
 *  shape says how, not the colour alone: done is filled, cached a ring with a
 *  dot, reused filled with a hole, failed filled with a cross, stopped filled
 *  with a bar. A stage that never ran because the Card was skipped is the
 *  grey empty ring with a slash; because it was blocked, a red ring with a
 *  thin red cross. The same geometry as StageDot in the Mac app. `size` is
 *  the diameter. */
export function StageDot({ status, size, waiting = false }: { status: StageStatus; size: number; waiting?: boolean }) {
  const c = colorVar(STAGE_COLOR[sectionStatus(status, waiting)])
  const line = Math.max(0.8, size * 0.15)
  const style: CSSProperties = { width: size, height: size }
  if (status === 'running' && waiting) return <span className="cv-dot cv-dot-waiting" style={{ ...style, borderWidth: line * 1.1, borderColor: c }} />
  if (status === 'pending' || status === 'notRun') return <span className="cv-dot" style={{ ...style, borderWidth: line, borderColor: fade('secondary', 0.6) }} />
  if (status === 'running') {
    return <span className="cv-dot cv-dot-running" style={{ ...style, borderWidth: line * 1.2, borderColor: fade('stage-run', 0.25), borderTopColor: c, borderRightColor: c }} />
  }
  const r = size / 2
  const hole = 'var(--control-bg)'
  if (status === 'skipped' || status === 'blocked') {
    const ink = status === 'skipped' ? fade('secondary', 0.6) : c
    const o = (r - line) * 0.7071
    const x = size * 0.19
    return (
      <svg className={`cv-dot-mark cv-dot-${status}`} width={size} height={size} viewBox={`0 0 ${size} ${size}`} aria-hidden>
        <circle cx={r} cy={r} r={r - line / 2} fill={hole} stroke={ink} strokeWidth={line} />
        {status === 'skipped'
          ? <path d={`M${r - o} ${r + o}L${r + o} ${r - o}`} stroke={ink} strokeWidth={line} strokeLinecap="round" />
          : <path d={`M${r - x} ${r - x}L${r + x} ${r + x}M${r + x} ${r - x}L${r - x} ${r + x}`} stroke={ink} strokeWidth={line * 0.85} strokeLinecap="round" />}
      </svg>
    )
  }
  const k = size * 0.2
  const mark = Math.max(1, size * 0.15)
  return (
    <svg className={`cv-dot-mark cv-dot-${status}`} width={size} height={size} viewBox={`0 0 ${size} ${size}`} aria-hidden>
      {status === 'cached'
        ? <><circle cx={r} cy={r} r={r - line / 2} fill={hole} stroke={c} strokeWidth={line} /><circle cx={r} cy={r} r={k} fill={c} /></>
        : <circle cx={r} cy={r} r={r} fill={c} />}
      {status === 'reused' && <circle cx={r} cy={r} r={k} fill={hole} />}
      {status === 'failed' && <path d={`M${r - k} ${r - k}L${r + k} ${r + k}M${r + k} ${r - k}L${r - k} ${r + k}`}
        stroke={hole} strokeWidth={mark} strokeLinecap="round" />}
      {status === 'cancelled' && <path d={`M${r - size * 0.24} ${r}H${r + size * 0.24}`} stroke={hole} strokeWidth={mark * 1.1} strokeLinecap="round" />}
    </svg>
  )
}

/** A section's state as the stage circle draws it; `size` is the diameter. */
export function StatusSymbol({ status, size = 11 }: { status: SectionStatus; size?: number }) {
  return (
    <span className="status-symbol" role="img" aria-label={SECTION_STATUS_LABEL[status]} style={{ width: size, height: size }}>
      <StageDot status={sectionStage(status)} size={size} waiting={status === 'waiting'} />
    </span>
  )
}

/** A Card's run state as the stage circles draw it, where one stands for it:
 *  waiting for inputs or a slot is a dashed ring, preparing and running the
 *  turning arc, succeeded filled, failed and stopped their marks, skipped the
 *  slashed grey ring, blocked the red ring with a thin cross, not run an
 *  empty ring. The same as stageLook in the Mac app. */
export function cardStateStage(state: D.CardState): { status: StageStatus; waiting: boolean } | undefined {
  switch (state) {
    case D.CardState.not_started: return { status: 'pending', waiting: false }
    case D.CardState.waiting: case D.CardState.ready: return { status: 'running', waiting: true }
    case D.CardState.preparing: case D.CardState.running: return { status: 'running', waiting: false }
    case D.CardState.succeeded: return { status: 'done', waiting: false }
    case D.CardState.failed: return { status: 'failed', waiting: false }
    case D.CardState.cancelled: return { status: 'cancelled', waiting: false }
    case D.CardState.skipped: return { status: 'skipped', waiting: false }
    case D.CardState.blocked: return { status: 'blocked', waiting: false }
    default: return undefined
  }
}

/** A Card's state mark (the sidebar's Card list, the Card's header): the
 *  stage circle for the state, sized for the place. */
export function CardStateMark({ state, size }: { state: D.CardState; size: number }) {
  const look = cardStateStage(state)
  const cs = cardState(state)
  if (!look) return <Icon name={cs.icon} size={size} style={{ color: colorVar(cs.color) }} />
  return (
    <span className="status-symbol" role="img" aria-label={cs.label} style={{ width: size, height: size }}>
      <StageDot status={look.status} size={size} waiting={look.waiting} />
    </span>
  )
}
