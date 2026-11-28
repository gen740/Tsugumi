import { L } from '../i18n'
import { D, s, sOpt } from '../wire/rpc'
import { plainText } from './terminalText'

export const workerName = (id: string): string => (id === 'local' ? L('プロジェクトの実行環境') : id)

/** Each worker's reason when no worker could be chosen (the core names them
 *  in the failure's `worker:<id>` arguments, and a timed-out probe's limit in
 *  `timeout:<id>`); undefined for any other failure. */
function workerSelectionReasons(failure?: D.FailureT | null): string[] | undefined {
  if (!failure) return undefined
  const args = failure.args.map((a) => [s(a.key), s(a.value)] as const)
  if (!args.some(([k, v]) => k === 'stage' && v === 'worker_selection')) return undefined
  return args.filter(([k]) => k.startsWith('worker:')).map(([k, v]) => {
    const id = k.slice('worker:'.length)
    const name = workerName(id)
    const limit = args.find(([key]) => key === `timeout:${id}`)?.[1]
    switch (v) {
      case 'timeout': return limit ? L('{0} の probe が時間切れ（{1} 秒）', name, limit) : L('{0} の probe が時間切れ', name)
      case 'failed': return L('{0} の probe が失敗', name)
      case 'not_positive': return L('{0} の probe のスコアが 0 以下', name)
      // Built from another revision: its contract in `contract:<id>`, this Tsugumi's in `contract`.
      case 'outdated': return L('{0} は古い版です（契約 {1}、この Tsugumi は {2}）。ワーカーを同じ版で作り直してください', name,
        args.find(([key]) => key === `contract:${id}`)?.[1] ?? '?', args.find(([key]) => key === 'contract')?.[1] ?? '?')
      default: return L('{0} は使えません', name)
    }
  })
}

/** What a failure says to a person: a Card that never started because no
 *  worker could be chosen in plain words, any other failure as its diagnostic. */
export function failureText(failure?: D.FailureT | null): string | undefined {
  const reasons = workerSelectionReasons(failure)
  if (!reasons) return diagnosticText(failure)
  const head = L('worker を選べなかったため、この Card は開始していません')
  return reasons.length ? `${head}: ${reasons.join(L('、'))}` : head
}

/** A failure's diagnostic as text (no terminal control). */
export function diagnosticText(failure?: D.FailureT | null): string | undefined {
  const diagnostic = sOpt(failure?.diagnostic)
  return diagnostic === undefined ? undefined : plainText(diagnostic)
}

/** The reasons in a line (for the Card's hover), when the Card never started
 *  because no worker could be chosen; undefined for any other failure. */
export const notStartedText = (failure?: D.FailureT | null): string | undefined =>
  workerSelectionReasons(failure)?.join(L('、'))

/** One worker's measurement, as its row in a run's Info says it. */
export function probeResult(probe: D.WorkerProbeResultT): string {
  if (probe.score !== null) return !probe.probe ? L('計測なし（同順位）') : L('スコア {0}', String(probe.score))
  if (probe.unavailableReason) return s(probe.unavailableReason)
  if (probe.failureKind === D.FailureKind.timeout) {
    const head = probe.timeLimitSeconds !== null ? L('時間切れ（{0} 秒）', String(probe.timeLimitSeconds)) : L('時間切れ')
    return probe.failure ? `${head}: ${s(probe.failure)}` : head
  }
  return s(probe.failure) || L('計測に失敗しました')
}
