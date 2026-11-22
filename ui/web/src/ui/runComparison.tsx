import { useEffect, useState } from 'react'
import { useObserve } from '../model/observable'
import { ComparedCard, ComparedField, ComparedMember, ComparedRow, Difference, batchMembers, comparedConditions, comparedParams, comparedValues, inBatch, loadRunComparison, memberName, pairMembers, runScope } from '../model/runComparison'
import { ProjectSession, cardState, errorText, formatDuration, formatMillis, runState } from '../model/session'
import { R, RPCClient, s, segs } from '../wire/rpc'
import { Button, Empty, Select, Spinner } from './controls'
import { L } from '../i18n'
import { environmentChange } from '../model/environments'
import { EnvironmentComparison, EnvironmentReadScope } from './environments'
import { runMark } from '../model/runMark'

export function RunComparisonView({ session, runs, initial, onClose }: { session: ProjectSession; runs: R.RunSummaryT[]; initial?: string; onClose: () => void }) {
  useObserve(session)
  const [left, setLeft] = useState(initial ?? s(runs[0]?.status?.runId))
  const [right, setRight] = useState(s(runs.find(r => s(r.status?.runId) !== (initial ?? s(runs[0]?.status?.runId)))?.status?.runId))
  const [rows, setRows] = useState<ComparedRow[]>()
  const [error, setError] = useState<string>()
  const [diff, setDiff] = useState<{ text: string; truncated: boolean }>()
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
  }, [session.rpc, left, right, attempt, session.recordingRevision])
  useEffect(() => {
    setDiff(undefined); setDiffError(undefined)
    if (!session.rpc || !from || !to || from === to) return
    let alive = true
    void session.versionDiff(to, from).then(v => alive && setDiff(v)).catch(e => alive && setDiffError(errorText(e)))
    return () => { alive = false }
  }, [session.rpc, from, to, attempt])
  const options = runs.map(r => ({ value: s(r.status?.runId), label: `${formatMillis(r.status?.acceptedAt)} · ${runState(r.status!.state).label} · ${s(r.status?.runId).slice(0, 8)}${runMark(r) ? ` · ${runMark(r)}` : ''}` }))
  return <EnvironmentReadScope key={`${left}:${right}:${attempt}`} rpc={session.rpc}><div className="run-comparison col">
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
          {rows.map(row => <ComparedMembers key={row.card} rpc={session.rpc!} left={left} right={right} row={row}>{(shown, choice) => <ComparedCardView row={shown} choice={choice} environmentSection={<EnvironmentComparison key={`${left}:${right}:${row.card}:${String(shown.left?.summary.invocation ?? '')}:${String(shown.right?.summary.invocation ?? '')}`} session={session} left={{ run: left, id: s((shown.left?.record?.record?.summary ?? shown.left?.summary)?.environmentId) }} right={{ run: right, id: s((shown.right?.record?.record?.summary ?? shown.right?.summary)?.environmentId) }} />} displayPath={(p) => session.displayPath(p)} />}</ComparedMembers>)}
        </>}
    </div>
  </div></EnvironmentReadScope>
}

/** The code and settings between the two runs' versions. A run without a recorded
 *  version, and a diff cut short, are said in words: neither reads as "no changes". */
export function VersionDifference({ from, to, diff, error }: { from: string; to: string; diff?: { text: string; truncated: boolean }; error?: string }) {
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
    {runMark(side.record?.run) && <p className="caption">{runMark(side.record?.run)}</p>}
    {inBatch(v) ? <p className="caption">{L('実行リスト {0}/{1}', v.batchIndex ?? 0, v.batchSize ?? 0)} · {memberName(v)}</p>
      : calls > 1n && <p className="caption">{L('{0} 回の呼び出しのうち最後の 1 回', calls)}</p>}
    {side.record?.aggregate && <p className="caption secondary">{L('呼び出し {0} · 成功 {1} · 失敗 {2}', side.record.aggregate.calls ?? 0n, side.record.aggregate.succeeded ?? 0n, side.record.aggregate.failed ?? 0n)}</p>}
    {(side.error || s(v.failure?.diagnostic)) && <p role="alert">{side.error || s(v.failure?.diagnostic)}</p>}
    {side.record?.record?.recordIo === false && <p className="secondary">{L('入出力は記録されていません')}</p>}
    {side.record?.truncated && <p className="secondary">{L('大きな値は概要のみ表示しています。')}</p>}
    {side.record && !s(side.record.record?.paramsJson) && <p className="secondary">{L('パラメータの記録がありません')}</p>}
  </div>
}

/** A Card that ran an execution list compares one named member at a time:
 *  members pair by implementation label (or number), not by position. */
export function ComparedMembers({ rpc, left, right, row, children }: { rpc: Pick<RPCClient, 'invocations' | 'record'>; left: string; right: string; row: ComparedRow; children: (row: ComparedRow, choice?: React.ReactNode) => React.ReactNode }) {
  const batch = inBatch(row.left?.summary) || inBatch(row.right?.summary)
  const [members, setMembers] = useState<ComparedMember[]>()
  const [problem, setProblem] = useState<string>()
  const [key, setKey] = useState<string>()
  const [shown, setShown] = useState<ComparedRow>()
  useEffect(() => {
    setMembers(undefined); setProblem(undefined); setKey(undefined); setShown(undefined)
    if (!batch) return
    let alive = true
    const controller = new AbortController()
    const side = (run: string, card?: ComparedCard) => card && inBatch(card.summary) ? batchMembers(rpc, run, card.summary, controller.signal) : Promise.resolve(card ? [card.summary] : [])
    void Promise.all([side(left, row.left), side(right, row.right)]).then(([a, b]) => {
      if (!alive) return
      const pairs = pairMembers(a, b)
      setMembers(pairs)
      // The last member is the one the comparison shows by default.
      const last = row.right?.summary ?? row.left?.summary
      setKey(pairs.find(p => (p.right ?? p.left)?.invocation === last?.invocation)?.key ?? pairs.at(-1)?.key)
    }).catch(e => alive && setProblem(errorText(e)))
    return () => { alive = false; controller.abort() }
  }, [rpc, left, right, row, batch])
  const pair = members?.find(m => m.key === key)
  useEffect(() => {
    setShown(undefined)
    if (!pair) return
    let alive = true
    const load = async (run: string, summary?: R.InvocationSummaryT, known?: ComparedCard): Promise<ComparedCard | undefined> => {
      if (!summary) return
      if (known && known.summary.invocation === summary.invocation) return known
      try { return { summary, record: await rpc.record(run, segs(summary.card), summary.invocation, false) } }
      catch (e) { return { summary, error: errorText(e) } }
    }
    void Promise.all([load(left, pair.left, row.left), load(right, pair.right, row.right)]).then(([a, b]) => alive && setShown({ card: row.card, left: a, right: b }))
    return () => { alive = false }
  }, [rpc, left, right, row, pair])
  if (!batch) return <>{children(row)}</>
  const choice = problem ? <p role="alert">{problem}</p> : !members ? <Spinner small /> : <Select label={L('比較する呼び出し')} value={key ?? ''} onChange={setKey}
    options={members.map(m => ({ value: m.key, label: m.left && m.right ? m.name : L('{0}（片方の実行のみ）', m.name) }))} />
  return <>{children(shown ?? row, choice)}</>
}

export function ComparedCardView({ row, displayPath = p => p.join('/'), environmentSection, choice }: {
  row: ComparedRow; displayPath?: (p: string[]) => string; environmentSection?: React.ReactNode; choice?: React.ReactNode;
}) {
  const a = row.left?.record, b = row.right?.record
  const statusChanged = row.left?.summary.state !== row.right?.summary.state || row.left?.summary.durationMs !== row.right?.summary.durationMs
  return <details className="comparison-card" open>
    <summary>{displayPath(row.card.split('/'))} {environmentChange(s((row.left?.record?.record?.summary ?? row.left?.summary)?.environmentId), s((row.right?.record?.record?.summary ?? row.right?.summary)?.environmentId)) === 'changed' && <span className="caption">· {L('環境が変わった Card')}</span>}</summary>
    {choice && <div className="pad8">{choice}</div>}
    <div className={`comparison-pair pad8${statusChanged ? ' changed' : ''}`}><SideStatus side={row.left} /><SideStatus side={row.right} /></div>
    <ComparedFields title={L('実行条件')} fields={comparedConditions(row.left, row.right, displayPath)} />
    {environmentSection}
    <ComparedFields title={L('パラメータ')} fields={comparedParams(a, b)} />
    <ComparedFields title={L('入力')} fields={comparedValues('inputs', a, b)} />
    <ComparedFields title={L('出力')} fields={comparedValues('outputs', a, b)} />
  </details>
}
