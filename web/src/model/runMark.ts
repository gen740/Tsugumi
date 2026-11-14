import { L } from '../i18n'
import type { R } from '../shared/rpc'

export const RUN_MARKS = { partial: L('部分実行'), derived: L('部分実行の結果を使用') }

/** Map the core's run provenance to a localized mark. Do not infer it from
 *  propagation: downstream runs can use saved inputs too. */
export function runMark(_run?: R.RunSummaryT | R.RunStatusT | null): string | undefined {
  // Wire the explicit provenance fields here when the protocol exposes them.
  return undefined
}
