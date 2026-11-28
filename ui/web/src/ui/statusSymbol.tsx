// The state of a stage as a plain symbol, never shaped like a control: a
// check, a spinning indicator, an hourglass, a thin empty circle, a cross.
// The log view's pipeline line and sections and the Card's stage strip on the
// canvas use the same symbols.
import { StageStatus } from '../model/canvasKit'
import { Icon } from './icons'
import { L } from '../i18n'

export type SectionStatus = 'pending' | 'waiting' | 'running' | 'done' | 'cached' | 'reused' | 'failed' | 'cancelled'

export const SECTION_STATUS_LABEL: Record<SectionStatus, string> = {
  pending: L('未開始'), waiting: L('待機中'), running: L('進行中'), done: L('完了'),
  cached: L('キャッシュを利用'), reused: L('再利用'), failed: L('失敗'), cancelled: L('停止'),
}

/** A stage strip status as the log view shows it. */
export function sectionStatus(status: StageStatus, waiting = false): SectionStatus {
  if (status === 'notRun') return 'pending'
  if (status === 'running' && waiting) return 'waiting'
  return status
}

/** The stage states' colours, one set for the Card's circles, the log's
 *  pipeline line and sections and the hover's stage table (theme colours,
 *  styles.css): Okabe–Ito, safe for colour-blind readers. Done is blue,
 *  cached and reused a lighter sky blue, running and waiting orange, failed
 *  vermillion, stopped grey; shapes and marks tell them apart as well. The
 *  same set as StagePalette in the Mac app. */
export const STAGE_COLOR: Record<SectionStatus, string> = {
  pending: 'secondary', waiting: 'stage-run', running: 'stage-run', done: 'stage-done',
  cached: 'stage-reuse', reused: 'stage-reuse', failed: 'stage-failed', cancelled: 'stage-cancelled',
}

const SYMBOL: Record<Exclude<SectionStatus, 'running'>, string> = {
  pending: 'circle', waiting: 'hourglass', done: 'checkmark', cached: 'checkmark', reused: 'checkmark', failed: 'xmark', cancelled: 'minus',
}

export function StatusSymbol({ status, size = 13 }: { status: SectionStatus; size?: number }) {
  const label = SECTION_STATUS_LABEL[status]
  if (status === 'running') {
    const border = Math.max(1, size / 8)
    return (
      <span className="status-symbol" role="img" aria-label={label} style={{ width: size, height: size }}>
        <span className="status-spinner" style={{ width: size * 0.8, height: size * 0.8, borderWidth: border }} />
      </span>
    )
  }
  return (
    <span className={`status-symbol status-${status}`} role="img" aria-label={label} style={{ width: size, height: size, color: `var(--${STAGE_COLOR[status]})` }}>
      <Icon name={SYMBOL[status]} size={status === 'pending' ? size * 0.7 : size * 0.85} />
    </span>
  )
}
