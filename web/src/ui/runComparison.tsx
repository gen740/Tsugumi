import { useEffect, useState } from 'react'
import { ComparedCard, ComparedField, ComparedRow, Difference, comparedConditions, comparedParams, comparedValues, loadRunComparison, runScope, sameDisplay } from '../model/runComparison'
import { ProjectSession, cardState, errorText, formatDuration, formatMillis, recordedDisplay, runState } from '../model/session'
import { R, s, segs } from '../shared/rpc'
import { Button, Empty, Select, Spinner } from './controls'
import { DisplayContent } from './display'
import { L } from '../i18n'

export function RunComparisonView({ session, runs, initial, onClose }: { session: ProjectSession; runs: R.RunSummaryT[]; initial?: string; onClose: () => void }) {
  const [left, setLeft] = useState(initial ?? s(runs[0]?.status?.runId))
  const [right, setRight] = useState(s(runs.find(r => s(r.status?.runId) !== (initial ?? s(runs[0]?.status?.runId)))?.status?.runId))
  const [rows, setRows] = useState<ComparedRow[]>()
  const [error, setError] = useState<string>()
  const [diff, setDiff] = useState<R.VersionDiffT>()
  const [diffError, setDiffError] = useState<string>()
  const [attempt, setAttempt] = useState(0)
  const a = runs.find(r => s(r.status?.runId) === left), b = runs.find(r => s(r.status?.runId) === right)
  const from = s(a?.status?.commit), to = s(b?.status?.commit)
  useEffect(() => {
    setRows(undefined); setError(undefined)
    if (!session.rpc) { setError(L('実行環境に接続していません')); return }
    if (!left || !right || left === right) return
    let alive = true
    const controller = new AbortController()
    void loadRunComparison(session.rpc, left, right, controller.signal).then(v => { if (alive) setRows(v) }).catch(e => alive && setError(errorText(e)))
    return () => { alive = false; controller.abort() }
  }, [session.rpc, left, right, attempt])
  useEffect(() => {
    setDiff(undefined); setDiffError(undefined)
    if (!session.rpc || !from || !to || from === to) return
    let alive = true
    void session.rpc.versionDiff(to, from).then(v => alive && setDiff(v)).catch(e => alive && setDiffError(errorText(e)))
    return () => { alive = false }
  }, [session.rpc, from, to, attempt])
  const options = runs.map(r => ({ value: s(r.status?.runId), label: `${formatMillis(r.status?.acceptedAt)} · ${runState(r.status!.state).label} · ${s(r.status?.runId).slice(0, 8)}` }))
  return <div className="run-comparison col">
    <div className="row center gap8 pad10 border-bottom"><b>{L('実行を比較')}</b><span className="spacer" /><Button onClick={onClose}>{L('閉じる')}</Button></div>
    <div className="comparison-pair pad10 border-bottom">
      <Select label={L('左の実行')} value={left} onChange={setLeft} options={options} />
      <Select label={L('右の実行')} value={right} onChange={setRight} options={options} />
    </div>
    <div className="scroll pad10 col gap8">
      <p className="secondary">{L('各 Card の最後の呼び出しを比較します。記録や現在の Workflow は変更しません。')}</p>
      {left === right || !left || !right ? <Empty title={L('異なる 2 つの実行を選んでください')} icon="arrow.triangle.swap" />
        : error ? <div role="alert">{error}<Button onClick={() => setAttempt(attempt + 1)}>{L('再試行')}</Button></div>
        : !rows ? <Spinner /> : <>
          <RunScopes left={a} right={b} displayPath={(p) => session.displayPath(p)} />
          <VersionDifference from={from} to={to} diff={diff} error={diffError} />
          {!rows.length && <Empty title={L('記録がありません')} icon="tray" />}
          {rows.map(row => <ComparedCardView key={row.card} row={row} displayPath={(p) => session.displayPath(p)} />)}
        </>}
    </div>
  </div>
}

/** The code and settings between the two runs' versions. A run without a recorded
 *  version, and a diff cut short, are said in words: neither reads as "no changes". */
export function VersionDifference({ from, to, diff, error }: { from: string; to: string; diff?: R.VersionDiffT; error?: string }) {
  return <details className="comparison-card" open={false}>
    <summary>{L('コードと設定の差分')} · {from.slice(0, 8) || '—'} → {to.slice(0, 8) || '—'}</summary>
    {!from || !to ? <p>{L('版が記録されていないため差分を表示できません。')}</p>
      : from === to ? <p>{L('同じ版です')}</p> : error ? <p role="alert">{error}</p>
      : !diff ? <Spinner /> : <><pre className="value-box selectable">{s(diff.text) || L('変更はありません')}</pre>{diff.truncated && <p>{L('差分が大きいため一部を表示しています。')}</p>}</>}
  </details>
}

/** `unknown` is said in words: an unmarked field would read as unchanged. */
function DifferenceMark({ difference }: { difference: Difference }) {
  return difference === 'same' ? null : <span className="caption">· {difference === 'changed' ? L('相違あり') : L('比較できません')}</span>
}

/** Where each run started and how far it went: a partial run reuses saved inputs. */
export function RunScopes({ left, right, displayPath }: { left?: R.RunSummaryT; right?: R.RunSummaryT; displayPath?: (p: string[]) => string }) {
  const a = runScope(left, displayPath), b = runScope(right, displayPath)
  const same = segs(left?.target).join('/') === segs(right?.target).join('/') && (left?.propagate !== false) === (right?.propagate !== false)
  return <ComparedFields title={L('実行の範囲')} fields={[{ name: L('開始した Card'), left: a, right: b, difference: same ? 'same' : 'changed' }]} />
}

function ComparedFields({ title, fields }: { title: string; fields: ComparedField[] }) {
  return <section><h4>{title}</h4>{!fields.length && <p className="secondary">{L('なし')}</p>}{fields.map(f => <div key={f.name} className={`comparison-field${f.difference === 'same' ? '' : ` ${f.difference}`}`}>
    <b>{f.name} <DifferenceMark difference={f.difference} /></b>
    <div className="comparison-pair"><pre className="value-box selectable">{f.left ?? L('この実行にはありません')}</pre><pre className="value-box selectable">{f.right ?? L('この実行にはありません')}</pre></div>
  </div>)}</section>
}

function SideStatus({ side }: { side?: ComparedCard }) {
  if (!side) return <p className="secondary">{L('この実行にはありません')}</p>
  const v = side.record?.record?.summary ?? side.summary, state = cardState(side.record?.status?.state ?? v.state)
  const calls = side.record?.aggregate?.calls ?? v.invocation ?? 0n
  return <div><b style={{ color: `var(--${state.color})` }}>{state.label}</b> · #{String(v.invocation ?? 0n)} · {v.durationMs === null ? '—' : formatDuration(Number(v.durationMs) / 1000)}
    {calls > 1n && <p className="caption">{L('{0} 回の呼び出しのうち最後の 1 回', calls)}</p>}
    {side.record?.aggregate && <p className="caption secondary">{L('呼び出し {0} · 成功 {1} · 失敗 {2}', side.record.aggregate.calls ?? 0n, side.record.aggregate.succeeded ?? 0n, side.record.aggregate.failed ?? 0n)}</p>}
    {(side.error || s(v.failure?.diagnostic)) && <p role="alert">{side.error || s(v.failure?.diagnostic)}</p>}
    {side.record?.record?.recordIo === false && <p className="secondary">{L('入出力は記録されていません')}</p>}
    {side.record?.truncated && <p className="secondary">{L('大きな値は概要のみ表示しています。')}</p>}
    {side.record && !s(side.record.record?.paramsJson) && <p className="secondary">{L('パラメータの記録がありません')}</p>}
  </div>
}

export function ComparedCardView({ row, displayPath = p => p.join('/') }: { row: ComparedRow; displayPath?: (p: string[]) => string }) {
  const a = row.left?.record, b = row.right?.record
  const leftDisplay = a?.record?.display ? recordedDisplay(a.record.display) : undefined, rightDisplay = b?.record?.display ? recordedDisplay(b.record.display) : undefined
  const statusChanged = row.left?.summary.state !== row.right?.summary.state || row.left?.summary.durationMs !== row.right?.summary.durationMs
  const displayChanged = !sameDisplay(leftDisplay, rightDisplay)
  return <details className="comparison-card" open>
    <summary>{displayPath(row.card.split('/'))}</summary>
    <div className={`comparison-pair pad8${statusChanged ? ' changed' : ''}`}><SideStatus side={row.left} /><SideStatus side={row.right} /></div>
    <ComparedFields title={L('実行条件')} fields={comparedConditions(row.left, row.right, displayPath)} />
    <ComparedFields title={L('パラメータ')} fields={comparedParams(a, b)} />
    <ComparedFields title={L('入力')} fields={comparedValues('inputs', a, b)} />
    <ComparedFields title={L('出力')} fields={comparedValues('outputs', a, b)} />
    {(leftDisplay || rightDisplay) && <section><h4>{L('表示')} {displayChanged && <span className="caption">· {L('相違あり')}</span>}</h4><div className={`comparison-pair${displayChanged ? ' changed' : ''}`}>
      <div className="comparison-display" style={{ height: leftDisplay && ['image', 'points', 'module'].includes(leftDisplay.value.kind) ? 400 : undefined }}>{leftDisplay ? <DisplayContent display={leftDisplay} large /> : <Empty small title={L('表示の記録がありません')} icon="chart.xyaxis.line" />}</div>
      <div className="comparison-display" style={{ height: rightDisplay && ['image', 'points', 'module'].includes(rightDisplay.value.kind) ? 400 : undefined }}>{rightDisplay ? <DisplayContent display={rightDisplay} large /> : <Empty small title={L('表示の記録がありません')} icon="chart.xyaxis.line" />}</div>
    </div></section>}
  </details>
}
